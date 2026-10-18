// Simulated ReShade host: verifies the real DLL's loader lifecycle and passive callbacks.
// It does not create a GPU device or run Don't Starve Together.
#include <windows.h>
#include <reshade_events.hpp>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
std::array<void *, 256> callbacks{};
std::vector<std::string> messages;
bool reject_registration = true;
unsigned registrations = 0, unregistrations = 0, removals = 0;
uint32_t api_version = 0;
bool invalid_callback = false;
}

extern "C" {
__declspec(dllexport) bool ReShadeRegisterAddon(void *, uint32_t version) {
    api_version = version;
    if (reject_registration) return false;
    ++registrations;
    return true;
}
__declspec(dllexport) void ReShadeUnregisterAddon(void *) { ++unregistrations; }
__declspec(dllexport) void ReShadeRegisterEvent(reshade::addon_event event, void *callback) {
    const auto index = static_cast<size_t>(event);
    if (index >= callbacks.size() || callbacks[index] != nullptr) {
        invalid_callback = true;
        return;
    }
    callbacks[index] = callback;
}
__declspec(dllexport) void ReShadeUnregisterEvent(reshade::addon_event event, void *callback) {
    const auto index = static_cast<size_t>(event);
    if (index >= callbacks.size() || callbacks[index] != callback) {
        invalid_callback = true;
        return;
    }
    callbacks[index] = nullptr;
    ++removals;
}
__declspec(dllexport) void ReShadeLogMessage(void *, int, const char *message) {
    messages.emplace_back(message);
    std::puts(message);
}
}

#define REQUIRE(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s (Windows error %lu)\n", __LINE__, #condition, GetLastError()); \
    return 1; } } while (false)

int main(int argc, char **argv) {
    REQUIRE(argc == 2);
    // A rejected API registration must fail loading without unregistering unregistered events.
    HMODULE module = LoadLibraryA(argv[1]);
    REQUIRE(module == nullptr);
    REQUIRE(unregistrations == 0 && removals == 0 && !invalid_callback);
    for (auto callback : callbacks) REQUIRE(callback == nullptr);

    reject_registration = false;
    module = LoadLibraryA(argv[1]);
    REQUIRE(module != nullptr);
    REQUIRE(api_version == 18 && registrations == 1);
    const auto name = reinterpret_cast<const char **>(GetProcAddress(module, "NAME"));
    REQUIRE(name != nullptr && std::strcmp(*name, "DST Render Probe") == 0);
    REQUIRE(GetProcAddress(module, "DESCRIPTION") != nullptr);

    using reshade::addon_event;
    const auto draw = reinterpret_cast<reshade::addon_event_traits<addon_event::draw>::decl>(
        callbacks[static_cast<size_t>(addon_event::draw)]);
    const auto indexed = reinterpret_cast<reshade::addon_event_traits<addon_event::draw_indexed>::decl>(
        callbacks[static_cast<size_t>(addon_event::draw_indexed)]);
    const auto present = reinterpret_cast<reshade::addon_event_traits<addon_event::reshade_present>::decl>(
        callbacks[static_cast<size_t>(addon_event::reshade_present)]);
    REQUIRE(draw && indexed && present && !invalid_callback);
    REQUIRE(!draw(nullptr, 3, 1, 0, 0));
    REQUIRE(!indexed(nullptr, 3, 1, 0, 0, 0));
    present(nullptr);
    REQUIRE(!messages.empty() && messages.back().find("frame=1 draws=2 target=1 skip=OFF") != std::string::npos);
    REQUIRE(!draw(nullptr, 6, 1, 0, 0));
    present(nullptr);
    REQUIRE(messages.back().find("frame=2 draws=1 target=1 skip=OFF") != std::string::npos);
    for (unsigned i = 3; i <= 180; ++i) present(nullptr);
    REQUIRE(messages.back().find("frame=180 draws=0 target=1 skip=OFF") != std::string::npos);
    REQUIRE(FreeLibrary(module));
    REQUIRE(unregistrations == 1 && removals == 3 && !invalid_callback);
    for (auto callback : callbacks) REQUIRE(callback == nullptr);
    std::puts("PASS: rejected registration, DLL load, API 18, metadata, passive draws, frame reset, heartbeat, unload.");
    return 0;
}
