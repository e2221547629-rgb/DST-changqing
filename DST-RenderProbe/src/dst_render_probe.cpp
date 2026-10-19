// Offline diagnostics only. v0.5 adds opt-in one-frame D3D9 metadata capture.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <reshade.hpp>
#include "probe_state.hpp"
#include "d3d9_capture.hpp"
#include "texture_dump.hpp"
#include "texture_readback.hpp"
#include <memory>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

extern "C" {
__declspec(dllexport) const char *NAME = "DST Render Probe";
__declspec(dllexport) const char *DESCRIPTION = "v0.5: F5 exports candidate textures with GPU fallback; F6 captures frame metadata.";
}

namespace {
bool g_registered = false;
dst_probe::State g_state;
uint64_t g_frame = 0;
std::mutex g_present_mutex;
dst_probe::CaptureBuffer g_capture;
wchar_t g_module_path[32768]{};

// At most 16 candidate slot-0 textures, retained only until the next present.
// Readback at present uses private targets and restores application state.
struct Candidate {
    dst_probe::ComRef<IDirect3DTexture9> texture;
    D3DSURFACE_DESC desc{};
    uint32_t ordinal = 0;
};
std::mutex g_texture_mutex;
std::atomic<bool> g_texture_active{false};
reshade::api::effect_runtime *g_texture_owner = nullptr;
reshade::api::device *g_texture_device = nullptr;
std::vector<std::unique_ptr<Candidate>> g_candidates;
uint64_t g_texture_sequence = 0;
bool g_texture_truncated = false;
void cancel_textures(reshade::api::effect_runtime *runtime) {
    const std::lock_guard<std::mutex> lock(g_texture_mutex);
    if (g_texture_owner != runtime) return;
    g_texture_active.store(false);
    g_candidates.clear(); g_texture_owner = nullptr; g_texture_device = nullptr;
}
void collect_texture(reshade::api::command_list *commands, uint32_t ordinal) {
    if (!g_texture_active.load() || !commands) return;
    try {
        const std::lock_guard<std::mutex> lock(g_texture_mutex);
        if (!g_texture_active.load() || commands->get_device() != g_texture_device) return;
        const auto native = reinterpret_cast<IDirect3DDevice9 *>(static_cast<uintptr_t>(g_texture_device->get_native()));
        if (!native) return;
        dst_probe::ComRef<IDirect3DBaseTexture9> base;
        if (FAILED(native->GetTexture(0, &base.value)) || !base.value || base.value->GetType() != D3DRTYPE_TEXTURE) return;
        auto *texture = static_cast<IDirect3DTexture9 *>(base.value);
        D3DSURFACE_DESC desc{};
        if (FAILED(texture->GetLevelDesc(0, &desc)) || desc.Format != D3DFMT_DXT5) return;
        if (!((desc.Width == 2048 && desc.Height == 2048) || (desc.Width == 1024 && desc.Height == 512))) return;
        for (const auto &item : g_candidates) if (item->texture.value == texture) return;
        if (g_candidates.size() >= 16) { g_texture_truncated = true; return; }
        auto item = std::make_unique<Candidate>();
        item->desc = desc; item->ordinal = ordinal;
        texture->AddRef(); item->texture.value = texture;
        g_candidates.push_back(std::move(item));
    } catch (...) {
        g_texture_active.store(false);
        reshade::log::message(reshade::log::level::error, "DST Render Probe v0.5 [texture_collect_failed]");
    }
}
void finish_textures(reshade::api::effect_runtime *runtime) {
    const std::lock_guard<std::mutex> lock(g_texture_mutex);
    if (g_texture_owner != runtime) return;
    g_texture_active.store(false);
    try {
        if (!g_module_path[0]) throw std::runtime_error("Add-on path unavailable");
        SYSTEMTIME time{}; GetLocalTime(&time);
        char stem[128];
        std::snprintf(stem, sizeof(stem), "textures-%04u%02u%02u-%02u%02u%02u-%03u-p%lu-t%llu",
            time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds,
            GetCurrentProcessId(),static_cast<unsigned long long>(g_texture_sequence));
        const auto directory = std::filesystem::path(g_module_path).parent_path() / "DSTRenderProbe-textures" / stem;
        if (!std::filesystem::create_directories(directory)) throw std::runtime_error("Refusing existing dump directory");
        std::ofstream report;
        report.exceptions(std::ios::badbit | std::ios::failbit);
        report.open(directory / "textures.csv", std::ios::binary);
        report << "texture_id,width,height,format,pool,usage,first_draw,read_hresult,bytes,file,method,direct_hresult,restore_hresult,stage\n";
        size_t saved = 0;
        for (const auto &item : g_candidates) {
            std::vector<uint8_t> bytes;
            const HRESULT direct_hr = dst_probe::read_bc3(item->texture.value, bytes);
            HRESULT hr = direct_hr, restore_hr = S_OK;
            const char *method = "bc3_lock", *stage = "complete";
            if (FAILED(direct_hr)) {
                method = "offscreen_bgra";
                const auto native = reinterpret_cast<IDirect3DDevice9 *>(static_cast<uintptr_t>(g_texture_device->get_native()));
                hr = dst_probe::read_texture_bgra(native,item->texture.value,bytes,restore_hr,stage);
            }
            const auto id = dst_probe::identity(item->texture.value);
            std::string filename;
            if (SUCCEEDED(hr)) {
                filename = "texture-" + std::to_string(id) + (SUCCEEDED(direct_hr) ? ".dds" : ".tga");
                std::ofstream out;
                out.exceptions(std::ios::badbit | std::ios::failbit);
                out.open(directory / filename, std::ios::binary);
                if (SUCCEEDED(direct_hr)) dst_probe::write_bc3_dds(out,item->desc.Width,item->desc.Height,bytes);
                else dst_probe::write_bgra_tga(out,item->desc.Width,item->desc.Height,bytes);
                out.close(); ++saved;
            }
            report << id << ',' << item->desc.Width << ',' << item->desc.Height << ',' << item->desc.Format
                   << ',' << item->desc.Pool << ',' << item->desc.Usage << ',' << item->ordinal
                   << ',' << static_cast<uint32_t>(hr) << ',' << bytes.size() << ',' << filename << ',' << method
                   << ',' << static_cast<uint32_t>(direct_hr) << ',' << static_cast<uint32_t>(restore_hr) << ',' << stage << '\n';
            if (FAILED(restore_hr)) throw std::runtime_error("Device state restoration failed; aborted remaining readbacks");
        }
        report.close();
        std::ofstream meta(directory / "scope.txt");
        meta << "version=0.5\nslot=0\nformat=DXT5\ndimensions=2048x2048 or 1024x512\nmax_candidates=16\n"
             << "truncated=" << g_texture_truncated << "\nsaved=" << saved
             << "\nidentity=session-local; size alone is not an asset match"
             << "\nread=top mip BC3 LockRect or point-sampled BGRA offscreen render, application state restored"
             << "\nTGA=top origin BGRA, alpha preserved, no color-space conversion or unpremultiplication\nfailed reads are not asset mismatches\n";
        meta.close();
        const std::string message = "DST Render Probe v0.5 [textures_saved] directory=" + directory.u8string()
            + " candidates=" + std::to_string(g_candidates.size()) + " saved=" + std::to_string(saved);
        reshade::log::message(reshade::log::level::info, message.c_str());
    } catch (const std::exception &error) {
        const std::string message = std::string("DST Render Probe v0.5 [textures_failed] ") + error.what();
        reshade::log::message(reshade::log::level::error, message.c_str());
    }
    g_candidates.clear(); g_texture_owner = nullptr; g_texture_device = nullptr;
}

void capture_draw(reshade::api::command_list *commands, uint32_t ordinal, bool indexed,
                  uint32_t count, uint32_t instances, uint32_t first, int32_t base,
                  uint32_t first_instance, bool skip_request) {
    if (!g_capture.enabled() || !commands) return;
    auto *device = commands->get_device();
    const auto ticket = g_capture.ticket(device);
    if (!ticket || device->get_api() != reshade::api::device_api::d3d9) return;
    dst_probe::DrawInfo row;
    row.ordinal = ordinal; row.indexed = indexed; row.count = count;
    row.instances = instances; row.first = first; row.base_vertex = base;
    row.first_instance = first_instance; row.skip_request = skip_request;
    const auto native = reinterpret_cast<IDirect3DDevice9 *>(static_cast<uintptr_t>(device->get_native()));
    if (!native) return;
    dst_probe::read_d3d9_state(native, row);
    g_capture.record(ticket, row);
}

void save_capture(reshade::api::effect_runtime *runtime, const dst_probe::CapturedFrame &capture) {
    try {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        char stem[160];
        std::snprintf(stem, sizeof(stem), "capture-%04u%02u%02u-%02u%02u%02u-%03u-p%lu-c%llu",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
            time.wMilliseconds, GetCurrentProcessId(), static_cast<unsigned long long>(capture.sequence));
        if (!g_module_path[0]) throw std::runtime_error("Add-on path unavailable");
        const auto directory = std::filesystem::path(g_module_path).parent_path() / "DSTRenderProbe-captures";
        std::filesystem::create_directories(directory);
        const auto csv_path = directory / (std::string(stem) + ".csv");
        const auto meta_path = directory / (std::string(stem) + ".meta.txt");
        if (std::filesystem::exists(csv_path) || std::filesystem::exists(meta_path))
            throw std::runtime_error("Capture filename already exists; refusing overwrite");
        std::ofstream csv;
        csv.exceptions(std::ios::badbit | std::ios::failbit);
        csv.open(csv_path, std::ios::binary);
        dst_probe::write_capture_csv(csv, capture);
        csv.close();
        std::ofstream meta;
        meta.exceptions(std::ios::badbit | std::ios::failbit);
        meta.open(meta_path, std::ios::binary);
        meta << "probe_version=0.5\nbackend=D3D9\nsequence=" << capture.sequence
             << "\nframe=" << capture.start_frame + 1 << "\nobserved_draws=" << capture.seen
             << "\nrecorded_draws=" << capture.rows.size()
             << "\ntruncated=" << (capture.seen > capture.rows.size() ? "true" : "false")
             << "\nscreenshot_postfix=" << stem
             << "\nhandles=session-local; not object names or stable content hashes"
             << "\ntexture_pixels=not_read\nvertex_data=not_read"
             << "\nframe_scope=one runtime present interval; other swap chains on same device may contribute\n";
        meta.close();
        const std::string message = "DST Render Probe v0.5 [capture_saved] file=" + csv_path.u8string()
            + " observed=" + std::to_string(capture.seen) + " recorded=" + std::to_string(capture.rows.size())
            + " screenshot_requested=" + stem;
        reshade::log::message(reshade::log::level::info, message.c_str());
        runtime->save_screenshot(stem); // Confirmation comes only from reshade_screenshot, not this void call.
    } catch (const std::exception &error) {
        const std::string message = std::string("DST Render Probe v0.5 [capture_save_failed] ") + error.what();
        reshade::log::message(reshade::log::level::error, message.c_str());
    }
}

void on_screenshot(reshade::api::effect_runtime *, const char *path) {
    const std::string message = std::string("DST Render Probe v0.5 [screenshot_saved] path=") + (path ? path : "unknown");
    reshade::log::message(reshade::log::level::info, message.c_str());
}
void on_runtime_destroy(reshade::api::effect_runtime *runtime) {
    g_capture.cancel(runtime);
    cancel_textures(runtime);
}

bool on_draw(reshade::api::command_list *commands, uint32_t count, uint32_t instances, uint32_t first, uint32_t first_instance) {
    uint32_t ordinal;
    const bool request = g_state.draw(GetTickCount64(), &ordinal);
    collect_texture(commands, ordinal);
    capture_draw(commands, ordinal, false, count, instances, first, 0, first_instance, request);
    return request;
}
bool on_draw_indexed(reshade::api::command_list *commands, uint32_t count, uint32_t instances, uint32_t first, int32_t base, uint32_t first_instance) {
    uint32_t ordinal;
    const bool request = g_state.draw(GetTickCount64(), &ordinal);
    collect_texture(commands, ordinal);
    capture_draw(commands, ordinal, true, count, instances, first, base, first_instance, request);
    return request;
}

dst_probe::Input read_input() {
    dst_probe::Input input;
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    input.foreground = process == GetCurrentProcessId();
    if (GetAsyncKeyState(VK_F5) & 0x8000) input.keys |= dst_probe::F5;
    if (GetAsyncKeyState(VK_F6) & 0x8000) input.keys |= dst_probe::F6;
    // Use key-down state, not the unreliable global 'pressed since last call' bit.
    if (GetAsyncKeyState(VK_F7) & 0x8000) input.keys |= dst_probe::F7;
    if (GetAsyncKeyState(VK_F8) & 0x8000) input.keys |= dst_probe::F8;
    if (GetAsyncKeyState(VK_F9) & 0x8000) input.keys |= dst_probe::F9;
    if (GetAsyncKeyState(VK_F10) & 0x8000) input.keys |= dst_probe::F10;
    if (GetAsyncKeyState(VK_F11) & 0x8000) input.keys |= dst_probe::F11;
    input.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    return input;
}

void on_reshade_present(reshade::api::effect_runtime *runtime) {
    const std::lock_guard<std::mutex> lock(g_present_mutex);
    const auto frame = g_state.finish_frame();
    const uint64_t now = GetTickCount64();
    const auto input = read_input();
    if (!input.foreground || (input.keys & dst_probe::F11)) {
        cancel_textures(runtime);
    } else {
        finish_textures(runtime);
    }
    if (!input.foreground) {
        g_capture.cancel(runtime);
    } else if (const auto captured = g_capture.finish(runtime)) {
        save_capture(runtime, *captured);
    }
    const char *reason = g_state.update(now, input, frame.draws);
    ++g_frame;
    if (reason && std::string(reason) == "texture_request") {
        g_capture.cancel(runtime);
        const std::lock_guard<std::mutex> texture_lock(g_texture_mutex);
        if (runtime && runtime->get_device()->get_api() == reshade::api::device_api::d3d9 && !g_texture_owner) {
            g_candidates.clear(); g_texture_truncated = false; ++g_texture_sequence;
            g_texture_owner = runtime; g_texture_device = runtime->get_device();
            g_texture_active.store(true);
            reshade::log::message(reshade::log::level::info, "DST Render Probe v0.5 [textures_armed]");
        } else {
            reshade::log::message(reshade::log::level::warning, "DST Render Probe v0.5 [textures_unavailable]");
        }
    }
    if (reason && std::string(reason) == "capture_request") {
        try {
            if (!runtime || runtime->get_device()->get_api() != reshade::api::device_api::d3d9) {
                reshade::log::message(reshade::log::level::warning,
                    "DST Render Probe v0.5 [capture_unsupported] metadata capture currently requires D3D9");
            } else {
                const auto sequence = g_capture.arm(runtime, runtime->get_device(), g_frame);
                const std::string message = "DST Render Probe v0.5 [capture_armed] sequence=" + std::to_string(sequence);
                reshade::log::message(reshade::log::level::info, message.c_str());
            }
        } catch (const std::exception &error) {
            const std::string message = std::string("DST Render Probe v0.5 [capture_arm_failed] ") + error.what();
            reshade::log::message(reshade::log::level::error, message.c_str());
        }
    }
    if (reason || g_frame <= 3 || g_frame % 180 == 0) {
        char message[512];
        std::snprintf(message, sizeof(message),
            "DST Render Probe v0.5 [%s] frame=%llu draws=%u target=%u skip=%s "
            "counted_target=%u hits=%u skip_requests=%u burst_ms=%llu burst_requests=%llu",
            reason ? reason : "heartbeat", static_cast<unsigned long long>(g_frame),
            frame.draws, g_state.target(), g_state.single() ? "ON" : "OFF",
            frame.counted_target, frame.hits, frame.requests,
            static_cast<unsigned long long>(g_state.burst_until() > now ? g_state.burst_until() - now : 0),
            static_cast<unsigned long long>(g_state.burst_requests()));
        reshade::log::message(reshade::log::level::info, message);
    }
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(instance)) return FALSE;
        g_registered = true;
        if (GetModuleFileNameW(instance, g_module_path, static_cast<DWORD>(sizeof(g_module_path) / sizeof(wchar_t))) == 0)
            g_module_path[0] = L'\0';
        reshade::register_event<reshade::addon_event::draw>(&on_draw);
        reshade::register_event<reshade::addon_event::draw_indexed>(&on_draw_indexed);
        reshade::register_event<reshade::addon_event::reshade_present>(&on_reshade_present);
        reshade::register_event<reshade::addon_event::reshade_screenshot>(&on_screenshot);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&on_runtime_destroy);
        break;
    case DLL_PROCESS_DETACH:
        if (!g_registered) break;
        reshade::unregister_event<reshade::addon_event::draw>(&on_draw);
        reshade::unregister_event<reshade::addon_event::draw_indexed>(&on_draw_indexed);
        reshade::unregister_event<reshade::addon_event::reshade_present>(&on_reshade_present);
        reshade::unregister_event<reshade::addon_event::reshade_screenshot>(&on_screenshot);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(&on_runtime_destroy);
        reshade::unregister_addon(instance);
        g_registered = false;
        break;
    }
    return TRUE;
}
