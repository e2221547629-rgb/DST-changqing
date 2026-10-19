// Read real D3D9 state from an isolated hidden test window, never from the running game.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../src/d3d9_capture.hpp"
#include <cstdio>
using namespace dst_probe;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; } } while (false)
struct Window {
    HWND value = CreateWindowExW(0, L"STATIC", L"DSTProbe native test", WS_POPUP, 0, 0, 64, 64,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~Window() { if (value) DestroyWindow(value); }
};
int main() {
    Window window;
    CHECK(window.value != nullptr);
    ComRef<IDirect3D9> api;
    api.value = Direct3DCreate9(D3D_SDK_VERSION);
    if (!api.value) { std::puts("SKIP: D3D9 runtime unavailable"); return 77; }
    D3DPRESENT_PARAMETERS params{};
    params.Windowed = TRUE; params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.BackBufferWidth = 64; params.BackBufferHeight = 64;
    params.hDeviceWindow = window.value;
    ComRef<IDirect3DDevice9> device;
    HRESULT result = api.value->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window.value,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &params, &device.value);
    if (FAILED(result)) result = api.value->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, window.value,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &params, &device.value);
    if (FAILED(result)) {
        std::printf("SKIP: no usable D3D9 adapter, HRESULT=0x%08lx\n", static_cast<unsigned long>(result));
        return 77;
    }
    ComRef<IDirect3DTexture9> texture;
    CHECK(SUCCEEDED(device.value->CreateTexture(32, 16, 1, 0, D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED, &texture.value, nullptr)));
    CHECK(SUCCEEDED(device.value->SetTexture(0, texture.value)));
    D3DVIEWPORT9 viewport{0, 0, 32, 16, 0.0f, 1.0f};
    CHECK(SUCCEEDED(device.value->SetViewport(&viewport)));
    DrawInfo row;
    read_d3d9_state(device.value, row);
    CHECK(row.api == 0x9000 && row.device == identity(device.value));
    CHECK((row.read_flags & 1) != 0 && row.viewport_width == 32 && row.viewport_height == 16);
    CHECK(row.textures[0].id == identity(texture.value));
    CHECK(row.textures[0].width == 32 && row.textures[0].height == 16);
    CHECK(row.textures[0].type == D3DRTYPE_TEXTURE && row.textures[0].format == D3DFMT_A8R8G8B8);
    CHECK(row.textures[0].query_result == D3D_OK && row.textures[0].desc_result == D3D_OK);
    CHECK(row.target != 0 && row.target_width == 64 && row.target_height == 64);
    CHECK(SUCCEEDED(device.value->SetTexture(0, nullptr)));
    DrawInfo unbound;
    read_d3d9_state(device.value, unbound);
    CHECK(unbound.textures[0].id == 0);
    std::puts("PASS: native D3D9 texture identity/dimensions/format, viewport, render target and unbinding.");
    return 0;
}
