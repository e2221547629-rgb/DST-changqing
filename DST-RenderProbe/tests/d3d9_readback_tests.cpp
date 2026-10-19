// Read real D3D9 state from an isolated hidden test window, never from the running game.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../src/d3d9_capture.hpp"
#include <cstdio>
#include <sstream>
#include <fstream>
#include "../src/texture_dump.hpp"
#include "../src/texture_readback.hpp"
using namespace dst_probe;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; } } while (false)
struct Window {
    HWND value = CreateWindowExW(0, L"STATIC", L"DSTProbe native test", WS_POPUP, 0, 0, 64, 64,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~Window() { if (value) DestroyWindow(value); }
};
int main(int argc, char **argv) {
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
    if (FAILED(result)) { std::puts("SKIP: no HAL adapter for real pixel rendering"); return 77; }

    ComRef<IDirect3DTexture9> upload, gpu;
    CHECK(SUCCEEDED(device.value->CreateTexture(8,8,1,0,D3DFMT_DXT5,D3DPOOL_SYSTEMMEM,&upload.value,nullptr)));
    CHECK(SUCCEEDED(device.value->CreateTexture(8,8,1,0,D3DFMT_DXT5,D3DPOOL_DEFAULT,&gpu.value,nullptr)));
    D3DLOCKED_RECT locked{};
    CHECK(SUCCEEDED(upload.value->LockRect(0,&locked,nullptr,0)));
    const uint16_t colors[] = {0xf800,0x07e0,0x001f,0xffff};
    for (int b=0; b<4; ++b) {
        auto *block = static_cast<uint8_t *>(locked.pBits)+(b/2)*locked.Pitch+(b%2)*16;
        std::memset(block,0,16); block[0]=block[1]=static_cast<uint8_t>(b*85);
        std::memcpy(block+8,&colors[b],2); std::memcpy(block+10,&colors[b],2);
    }
    CHECK(SUCCEEDED(upload.value->UnlockRect(0)));
    CHECK(SUCCEEDED(device.value->UpdateTexture(upload.value,gpu.value)));
    std::vector<uint8_t> bytes;
    CHECK(read_bc3(gpu.value,bytes)==D3DERR_INVALIDCALL && bytes.empty());
    // Hostile game-like state: non-default targets, depth, stream, texture, viewport,
    // scissors, blending, color mask, sampler and texture transform must all survive.
    ComRef<IDirect3DSurface9> original_target, depth;
    CHECK(SUCCEEDED(device.value->CreateRenderTarget(64,64,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&original_target.value,nullptr)));
    CHECK(SUCCEEDED(device.value->CreateDepthStencilSurface(64,64,D3DFMT_D16,D3DMULTISAMPLE_NONE,0,TRUE,&depth.value,nullptr)));
    CHECK(SUCCEEDED(device.value->SetRenderTarget(0,original_target.value)));
    CHECK(SUCCEEDED(device.value->SetDepthStencilSurface(depth.value)));
    D3DCAPS9 caps{}; CHECK(SUCCEEDED(device.value->GetDeviceCaps(&caps)));
    ComRef<IDirect3DSurface9> second_target;
    if (caps.NumSimultaneousRTs>1) {
        CHECK(SUCCEEDED(device.value->CreateRenderTarget(64,64,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&second_target.value,nullptr)));
        CHECK(SUCCEEDED(device.value->SetRenderTarget(1,second_target.value)));
    }
    ComRef<IDirect3DVertexBuffer9> vertex_buffer;
    CHECK(SUCCEEDED(device.value->CreateVertexBuffer(96,0,D3DFVF_XYZ,D3DPOOL_MANAGED,&vertex_buffer.value,nullptr)));
    CHECK(SUCCEEDED(device.value->SetStreamSource(0,vertex_buffer.value,4,12)));
    CHECK(SUCCEEDED(device.value->SetFVF(D3DFVF_XYZ)));
    CHECK(SUCCEEDED(device.value->SetTexture(0,gpu.value)));
    const D3DVIEWPORT9 original_viewport{3,5,23,19,0.2f,0.8f};
    const RECT original_scissor{4,6,14,16};
    CHECK(SUCCEEDED(device.value->SetViewport(&original_viewport)));
    CHECK(SUCCEEDED(device.value->SetScissorRect(&original_scissor)));
    CHECK(SUCCEEDED(device.value->SetRenderState(D3DRS_SCISSORTESTENABLE,TRUE)));
    CHECK(SUCCEEDED(device.value->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE)));
    CHECK(SUCCEEDED(device.value->SetRenderState(D3DRS_COLORWRITEENABLE,1)));
    CHECK(SUCCEEDED(device.value->SetRenderState(D3DRS_SRGBWRITEENABLE,TRUE)));
    CHECK(SUCCEEDED(device.value->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,TRUE)));
    CHECK(SUCCEEDED(device.value->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR)));
    CHECK(SUCCEEDED(device.value->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_COUNT2)));
    auto restored = [&]() {
        ComRef<IDirect3DSurface9> target_now, depth_now;
        if (FAILED(device.value->GetRenderTarget(0,&target_now.value)) || target_now.value!=original_target.value) return false;
        if (FAILED(device.value->GetDepthStencilSurface(&depth_now.value)) || depth_now.value!=depth.value) return false;
        if (second_target.value) {
            ComRef<IDirect3DSurface9> second_now;
            if (FAILED(device.value->GetRenderTarget(1,&second_now.value)) || second_now.value!=second_target.value) return false;
        }
        ComRef<IDirect3DBaseTexture9> texture_now;
        if (FAILED(device.value->GetTexture(0,&texture_now.value)) || texture_now.value!=gpu.value) return false;
        ComRef<IDirect3DVertexBuffer9> stream; UINT offset=0,stride=0;
        if (FAILED(device.value->GetStreamSource(0,&stream.value,&offset,&stride)) || stream.value!=vertex_buffer.value || offset!=4 || stride!=12) return false;
        D3DVIEWPORT9 viewport{}; RECT scissor{};
        if (FAILED(device.value->GetViewport(&viewport)) || std::memcmp(&viewport,&original_viewport,sizeof(viewport))) return false;
        if (FAILED(device.value->GetScissorRect(&scissor)) || std::memcmp(&scissor,&original_scissor,sizeof(scissor))) return false;
        DWORD value=0;
        if (FAILED(device.value->GetFVF(&value)) || value!=D3DFVF_XYZ) return false;
        if (FAILED(device.value->GetRenderState(D3DRS_SCISSORTESTENABLE,&value)) || value!=TRUE) return false;
        if (FAILED(device.value->GetRenderState(D3DRS_ALPHABLENDENABLE,&value)) || value!=TRUE) return false;
        if (FAILED(device.value->GetRenderState(D3DRS_COLORWRITEENABLE,&value)) || value!=1) return false;
        if (FAILED(device.value->GetRenderState(D3DRS_SRGBWRITEENABLE,&value)) || value!=TRUE) return false;
        if (FAILED(device.value->GetSamplerState(0,D3DSAMP_SRGBTEXTURE,&value)) || value!=TRUE) return false;
        if (FAILED(device.value->GetSamplerState(0,D3DSAMP_MAGFILTER,&value)) || value!=D3DTEXF_LINEAR) return false;
        if (FAILED(device.value->GetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,&value)) || value!=D3DTTFF_COUNT2) return false;
        return true;
    };
    HRESULT restore_hr=S_OK; const char *stage="";
    CHECK(SUCCEEDED(device.value->BeginScene()));
    HRESULT hr=read_texture_bgra(device.value,gpu.value,bytes,restore_hr,stage);
    if (FAILED(hr)) std::printf("readback failed stage=%s hr=0x%08lx restore=0x%08lx\n",stage,static_cast<unsigned long>(hr),static_cast<unsigned long>(restore_hr));
    CHECK(SUCCEEDED(hr) && SUCCEEDED(restore_hr) && restored() && bytes.size()==8*8*4);
    for (int y=0;y<8;++y) for (int x=0;x<8;++x) {
        const int block=(y/4)*2+x/4;
        const uint8_t expected[4][4]={{0,0,255,0},{0,255,0,85},{255,0,0,170},{255,255,255,255}};
        CHECK(std::memcmp(bytes.data()+(y*8+x)*4,expected[block],4)==0);
    }
    std::ostringstream tga(std::ios::binary); write_bgra_tga(tga,8,8,bytes);
    CHECK(tga.str().size()==18+256 && static_cast<uint8_t>(tga.str()[17])==0x28);
    CHECK(std::memcmp(tga.str().data()+18,bytes.data(),bytes.size())==0);
    if (argc==2) {
        std::ofstream output(argv[1],std::ios::binary);
        write_bgra_tga(output,8,8,bytes); CHECK(output.good());
    }
    CHECK(SUCCEEDED(device.value->EndScene()));
    // Exercise RAII restoration on an early error after target/state mutation.
    auto abort_copy = [&]() -> HRESULT {
        ReadbackState state(device.value);
        if (FAILED(state.capture())) return E_FAIL;
        ComRef<IDirect3DSurface9> temporary;
        if (FAILED(device.value->CreateRenderTarget(8,8,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&temporary.value,nullptr))) return E_FAIL;
        device.value->SetDepthStencilSurface(nullptr);
        if (second_target.value) device.value->SetRenderTarget(1,nullptr);
        device.value->SetRenderTarget(0,temporary.value);
        device.value->SetTexture(0,nullptr);
        device.value->SetRenderState(D3DRS_COLORWRITEENABLE,15);
        return E_ABORT; // state destructor must restore even on an error return.
    };
    CHECK(abort_copy()==E_ABORT && restored());
    // The helper must not end the caller's scene and must be reusable.
    CHECK(SUCCEEDED(device.value->BeginScene()));
    CHECK(SUCCEEDED(read_texture_bgra(device.value,gpu.value,bytes,restore_hr,stage)) && restored());
    CHECK(SUCCEEDED(device.value->EndScene()));
    CHECK(read_texture_bgra(nullptr,gpu.value,bytes,restore_hr,stage)==E_POINTER && bytes.empty());
    std::puts("PASS: DEFAULT usage=0 rejects direct BC3 lock; offscreen pixels/alpha/orientation exact; TGA; state restored on success and early abort; repeated use and scene ownership.");
    return 0;
}
