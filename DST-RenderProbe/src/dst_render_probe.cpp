// Offline diagnostics only. v0.2 adds measured callback requests and timed recovery.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <reshade.hpp>
#include "probe_state.hpp"
#include <cstdint>
#include <cstdio>
#include <mutex>

extern "C" {
__declspec(dllexport) const char *NAME = "DST Render Probe";
__declspec(dllexport) const char *DESCRIPTION = "v0.2: draw diagnostics; F7 tests all draws for 500 ms then restores.";
}

namespace {
bool g_registered = false;
dst_probe::State g_state;
uint64_t g_frame = 0;
std::mutex g_present_mutex;

bool on_draw(reshade::api::command_list *, uint32_t, uint32_t, uint32_t, uint32_t) {
    return g_state.draw(GetTickCount64());
}
bool on_draw_indexed(reshade::api::command_list *, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {
    return g_state.draw(GetTickCount64());
}

dst_probe::Input read_input() {
    dst_probe::Input input;
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    input.foreground = process == GetCurrentProcessId();
    // Use key-down state, not the unreliable global 'pressed since last call' bit.
    if (GetAsyncKeyState(VK_F7) & 0x8000) input.keys |= dst_probe::F7;
    if (GetAsyncKeyState(VK_F8) & 0x8000) input.keys |= dst_probe::F8;
    if (GetAsyncKeyState(VK_F9) & 0x8000) input.keys |= dst_probe::F9;
    if (GetAsyncKeyState(VK_F10) & 0x8000) input.keys |= dst_probe::F10;
    if (GetAsyncKeyState(VK_F11) & 0x8000) input.keys |= dst_probe::F11;
    input.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    return input;
}

void on_reshade_present(reshade::api::effect_runtime *) {
    const std::lock_guard<std::mutex> lock(g_present_mutex);
    const auto frame = g_state.finish_frame();
    const uint64_t now = GetTickCount64();
    const char *reason = g_state.update(now, read_input(), frame.draws);
    ++g_frame;
    if (reason || g_frame <= 3 || g_frame % 180 == 0) {
        char message[512];
        std::snprintf(message, sizeof(message),
            "DST Render Probe v0.2 [%s] frame=%llu draws=%u target=%u skip=%s "
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
