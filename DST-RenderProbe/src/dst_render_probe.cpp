// Offline diagnostics only. v0.3 adds opt-in one-frame D3D9 metadata capture.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <reshade.hpp>
#include "probe_state.hpp"
#include "d3d9_capture.hpp"
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

extern "C" {
__declspec(dllexport) const char *NAME = "DST Render Probe";
__declspec(dllexport) const char *DESCRIPTION = "v0.3: F6 captures one D3D9 frame; F7 tests draws for 500 ms.";
}

namespace {
bool g_registered = false;
dst_probe::State g_state;
uint64_t g_frame = 0;
std::mutex g_present_mutex;
dst_probe::CaptureBuffer g_capture;
wchar_t g_module_path[32768]{};

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
        meta << "probe_version=0.3\nbackend=D3D9\nsequence=" << capture.sequence
             << "\nframe=" << capture.start_frame + 1 << "\nobserved_draws=" << capture.seen
             << "\nrecorded_draws=" << capture.rows.size()
             << "\ntruncated=" << (capture.seen > capture.rows.size() ? "true" : "false")
             << "\nscreenshot_postfix=" << stem
             << "\nhandles=session-local; not object names or stable content hashes"
             << "\ntexture_pixels=not_read\nvertex_data=not_read"
             << "\nframe_scope=one runtime present interval; other swap chains on same device may contribute\n";
        meta.close();
        const std::string message = "DST Render Probe v0.3 [capture_saved] file=" + csv_path.u8string()
            + " observed=" + std::to_string(capture.seen) + " recorded=" + std::to_string(capture.rows.size())
            + " screenshot_requested=" + stem;
        reshade::log::message(reshade::log::level::info, message.c_str());
        runtime->save_screenshot(stem); // Confirmation comes only from reshade_screenshot, not this void call.
    } catch (const std::exception &error) {
        const std::string message = std::string("DST Render Probe v0.3 [capture_save_failed] ") + error.what();
        reshade::log::message(reshade::log::level::error, message.c_str());
    }
}

void on_screenshot(reshade::api::effect_runtime *, const char *path) {
    const std::string message = std::string("DST Render Probe v0.3 [screenshot_saved] path=") + (path ? path : "unknown");
    reshade::log::message(reshade::log::level::info, message.c_str());
}
void on_runtime_destroy(reshade::api::effect_runtime *runtime) {
    g_capture.cancel(runtime);
}

bool on_draw(reshade::api::command_list *commands, uint32_t count, uint32_t instances, uint32_t first, uint32_t first_instance) {
    uint32_t ordinal;
    const bool request = g_state.draw(GetTickCount64(), &ordinal);
    capture_draw(commands, ordinal, false, count, instances, first, 0, first_instance, request);
    return request;
}
bool on_draw_indexed(reshade::api::command_list *commands, uint32_t count, uint32_t instances, uint32_t first, int32_t base, uint32_t first_instance) {
    uint32_t ordinal;
    const bool request = g_state.draw(GetTickCount64(), &ordinal);
    capture_draw(commands, ordinal, true, count, instances, first, base, first_instance, request);
    return request;
}

dst_probe::Input read_input() {
    dst_probe::Input input;
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    input.foreground = process == GetCurrentProcessId();
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
    if (!input.foreground) {
        g_capture.cancel(runtime);
    } else if (const auto captured = g_capture.finish(runtime)) {
        save_capture(runtime, *captured);
    }
    const char *reason = g_state.update(now, input, frame.draws);
    ++g_frame;
    if (reason && std::string(reason) == "capture_request") {
        try {
            if (!runtime || runtime->get_device()->get_api() != reshade::api::device_api::d3d9) {
                reshade::log::message(reshade::log::level::warning,
                    "DST Render Probe v0.3 [capture_unsupported] metadata capture currently requires D3D9");
            } else {
                const auto sequence = g_capture.arm(runtime, runtime->get_device(), g_frame);
                const std::string message = "DST Render Probe v0.3 [capture_armed] sequence=" + std::to_string(sequence);
                reshade::log::message(reshade::log::level::info, message.c_str());
            }
        } catch (const std::exception &error) {
            const std::string message = std::string("DST Render Probe v0.3 [capture_arm_failed] ") + error.what();
            reshade::log::message(reshade::log::level::error, message.c_str());
        }
    }
    if (reason || g_frame <= 3 || g_frame % 180 == 0) {
        char message[512];
        std::snprintf(message, sizeof(message),
            "DST Render Probe v0.3 [%s] frame=%llu draws=%u target=%u skip=%s "
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
