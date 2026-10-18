// DST Render Probe - read-only by default; optional per-draw suppression for offline diagnostics.
// ReShade add-on API: https://github.com/crosire/reshade
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <reshade.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>

extern "C" {
__declspec(dllexport) const char *NAME = "DST Render Probe";
__declspec(dllexport) const char *DESCRIPTION = "Offline draw-call diagnostics; monitoring only by default.";
}

namespace {
bool g_registered = false;
std::atomic<uint32_t> g_draw_index{0};
std::atomic<uint32_t> g_last_draw_count{0};
std::atomic<uint64_t> g_frame{0};
std::atomic<uint32_t> g_target_index{1};
std::atomic<bool> g_skip_enabled{false};

void log_status(const char *reason, uint32_t last_count) {
    char message[256];
    std::snprintf(message, sizeof(message),
                  "DST Render Probe [%s] frame=%llu draws=%u target=%u skip=%s",
                  reason,
                  static_cast<unsigned long long>(g_frame.load(std::memory_order_relaxed)),
                  last_count, g_target_index.load(std::memory_order_relaxed),
                  g_skip_enabled.load(std::memory_order_relaxed) ? "ON" : "OFF");
    reshade::log::message(reshade::log::level::info, message);
}

bool intercept_draw() {
    const uint32_t draw_no = g_draw_index.fetch_add(1, std::memory_order_relaxed) + 1;
    // Do not modify ANY GPU operation unless explicitly enabled by the user.
    return g_skip_enabled.load(std::memory_order_relaxed) &&
           draw_no == g_target_index.load(std::memory_order_relaxed);
}

bool on_draw(reshade::api::command_list *, uint32_t, uint32_t, uint32_t, uint32_t) {
    return intercept_draw();
}

bool on_draw_indexed(reshade::api::command_list *, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {
    return intercept_draw();
}

void on_reshade_present(reshade::api::effect_runtime *) {
    // For D3D9 the effective frame is generally single threaded. This is a diagnostic
    // approximation, not a synchronized per-command-buffer capture for D3D12/Vulkan.
    const uint32_t draw_count = g_draw_index.exchange(0, std::memory_order_relaxed);
    g_last_draw_count.store(draw_count, std::memory_order_relaxed);
    const uint64_t current_frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;

    bool changed = false;
    if ((GetAsyncKeyState(VK_F8) & 1) != 0) {
        g_skip_enabled.store(!g_skip_enabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
        changed = true;
    }
    if ((GetAsyncKeyState(VK_F9) & 1) != 0) {
        uint32_t target = g_target_index.load(std::memory_order_relaxed);
        g_target_index.store(target > 1 ? target - 1 : 1, std::memory_order_relaxed);
        changed = true;
    }
    if ((GetAsyncKeyState(VK_F10) & 1) != 0) {
        const uint32_t target = g_target_index.load(std::memory_order_relaxed);
        g_target_index.store(target < draw_count ? target + 1 : target, std::memory_order_relaxed);
        changed = true;
    }
    if ((GetAsyncKeyState(VK_F11) & 1) != 0) {
        g_target_index.store(1, std::memory_order_relaxed);
        g_skip_enabled.store(false, std::memory_order_relaxed);
        changed = true;
    }
    if (changed || current_frame <= 3 || current_frame % 180 == 0) {
        log_status(changed ? "key" : "heartbeat", draw_count);
    }
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(instance)) return FALSE;
        g_registered = true;
        reshade::register_event<reshade::addon_event::draw>(&on_draw);
        reshade::register_event<reshade::addon_event::draw_indexed>(&on_draw_indexed);
        reshade::register_event<reshade::addon_event::reshade_present>(&on_reshade_present);
        break;
    case DLL_PROCESS_DETACH:
        if (!g_registered) break;
        reshade::unregister_event<reshade::addon_event::draw>(&on_draw);
        reshade::unregister_event<reshade::addon_event::draw_indexed>(&on_draw_indexed);
        reshade::unregister_event<reshade::addon_event::reshade_present>(&on_reshade_present);
        reshade::unregister_addon(instance);
        g_registered = false;
        break;
    }
    return TRUE;
}
