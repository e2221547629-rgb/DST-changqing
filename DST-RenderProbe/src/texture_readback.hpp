#pragma once
#include "d3d9_capture.hpp"
#include <algorithm>
#include <cstring>
#include <vector>

namespace dst_probe {
// ReShade's D3D9 reshade_present callback runs inside its native BeginScene.
// The caller must already be inside a scene; this helper never changes scene ownership.
struct ReadbackState {
    IDirect3DDevice9 *device;
    ComRef<IDirect3DStateBlock9> block;
    ComRef<IDirect3DSurface9> targets[4], depth;
    D3DVIEWPORT9 viewport{};
    DWORD count = 0, srgb_write = 0, srgb_texture = 0;
    bool armed = false;
    explicit ReadbackState(IDirect3DDevice9 *value) : device(value) {}
    HRESULT capture() {
        D3DCAPS9 caps{};
        HRESULT hr = device->GetDeviceCaps(&caps);
        if (FAILED(hr)) return hr;
        count = (std::min)(DWORD(4), caps.NumSimultaneousRTs);
        if (!count) return E_FAIL;
        if (FAILED(hr = device->CreateStateBlock(D3DSBT_ALL, &block.value))) return hr;
        if (FAILED(hr = block.value->Capture())) return hr;
        if (FAILED(hr = device->GetViewport(&viewport))) return hr;
        if (FAILED(hr = device->GetRenderState(D3DRS_SRGBWRITEENABLE, &srgb_write))) return hr;
        if (FAILED(hr = device->GetSamplerState(0, D3DSAMP_SRGBTEXTURE, &srgb_texture))) return hr;
        for (DWORD i = 0; i < count; ++i) {
            hr = device->GetRenderTarget(i, &targets[i].value);
            if (FAILED(hr) && !(i > 0 && hr == D3DERR_NOTFOUND)) return hr;
        }
        hr = device->GetDepthStencilSurface(&depth.value);
        if (FAILED(hr) && hr != D3DERR_NOTFOUND) return hr;
        armed = true;
        return S_OK;
    }
    HRESULT restore() {
        if (!armed) return S_OK;
        HRESULT result = S_OK;
        auto check = [&](HRESULT hr) { if (FAILED(hr) && SUCCEEDED(result)) result = hr; };
        check(device->SetDepthStencilSurface(nullptr));
        for (DWORD i = 1; i < count; ++i) check(device->SetRenderTarget(i, nullptr));
        check(device->SetRenderTarget(0, targets[0].value));
        for (DWORD i = 1; i < count; ++i) check(device->SetRenderTarget(i, targets[i].value));
        check(device->SetDepthStencilSurface(depth.value));
        check(block.value->Apply());
        check(device->SetRenderState(D3DRS_SRGBWRITEENABLE, srgb_write));
        check(device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, srgb_texture));
        check(device->SetViewport(&viewport)); // SetRenderTarget resets the viewport.
        armed = false;
        return result;
    }
    ~ReadbackState() { restore(); }
};

inline HRESULT read_texture_bgra(IDirect3DDevice9 *device, IDirect3DTexture9 *texture,
    std::vector<uint8_t> &bytes, HRESULT &restore_result, const char *&stage) {
    bytes.clear(); restore_result = S_OK; stage = "validate";
    if (!device || !texture) return E_POINTER;
    D3DSURFACE_DESC desc{};
    HRESULT hr = texture->GetLevelDesc(0, &desc);
    if (FAILED(hr)) return hr;
    if (!desc.Width || !desc.Height || desc.Width > 4096 || desc.Height > 4096) return E_INVALIDARG;
    // Allocate before any device mutations or surface locks.
    std::vector<uint8_t> result(static_cast<size_t>(desc.Width) * desc.Height * 4);
    ComRef<IDirect3DSurface9> target, staging;
    stage = "create_target";
    hr = device->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_A8R8G8B8,
        D3DMULTISAMPLE_NONE, 0, FALSE, &target.value, nullptr);
    if (FAILED(hr)) return hr;
    stage = "create_staging";
    hr = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_A8R8G8B8,
        D3DPOOL_SYSTEMMEM, &staging.value, nullptr);
    if (FAILED(hr)) return hr;
    ReadbackState state(device);
    stage = "capture_state";
    if (FAILED(hr = state.capture())) return hr;
    auto render = [&]() -> HRESULT {
        HRESULT call;
#define DST_READBACK_CALL(expr) do { if (FAILED(call = (expr))) return call; } while (false)
        stage = "bind_private_target";
        DST_READBACK_CALL(device->SetDepthStencilSurface(nullptr));
        for (DWORD i = 1; i < state.count; ++i) DST_READBACK_CALL(device->SetRenderTarget(i, nullptr));
        DST_READBACK_CALL(device->SetRenderTarget(0, target.value));
        const D3DVIEWPORT9 viewport{0,0,desc.Width,desc.Height,0.0f,1.0f};
        DST_READBACK_CALL(device->SetViewport(&viewport));
        stage = "set_copy_state";
        DST_READBACK_CALL(device->SetVertexShader(nullptr));
        DST_READBACK_CALL(device->SetPixelShader(nullptr));
        DST_READBACK_CALL(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1));
        DST_READBACK_CALL(device->SetStreamSourceFreq(0, 1));
        const struct { D3DRENDERSTATETYPE key; DWORD value; } states[] = {
            {D3DRS_ZENABLE,FALSE},{D3DRS_ZWRITEENABLE,FALSE},{D3DRS_STENCILENABLE,FALSE},
            {D3DRS_ALPHATESTENABLE,FALSE},{D3DRS_ALPHABLENDENABLE,FALSE},
            {D3DRS_SEPARATEALPHABLENDENABLE,FALSE},{D3DRS_SCISSORTESTENABLE,FALSE},
            {D3DRS_CULLMODE,D3DCULL_NONE},{D3DRS_FILLMODE,D3DFILL_SOLID},
            {D3DRS_FOGENABLE,FALSE},{D3DRS_LIGHTING,FALSE},{D3DRS_CLIPPING,FALSE},
            {D3DRS_CLIPPLANEENABLE,0},{D3DRS_SRGBWRITEENABLE,FALSE},
            {D3DRS_COLORWRITEENABLE,15},{D3DRS_DITHERENABLE,FALSE},{D3DRS_MULTISAMPLEMASK,0xffffffff}
        };
        for (const auto &value : states) DST_READBACK_CALL(device->SetRenderState(value.key,value.value));
        const struct { D3DSAMPLERSTATETYPE key; DWORD value; } samplers[] = {
            {D3DSAMP_MINFILTER,D3DTEXF_POINT},{D3DSAMP_MAGFILTER,D3DTEXF_POINT},
            {D3DSAMP_MIPFILTER,D3DTEXF_NONE},{D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP},
            {D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP},{D3DSAMP_SRGBTEXTURE,FALSE},
            {D3DSAMP_MAXMIPLEVEL,0},{D3DSAMP_MIPMAPLODBIAS,0}
        };
        for (const auto &value : samplers) DST_READBACK_CALL(device->SetSamplerState(0,value.key,value.value));
        DST_READBACK_CALL(device->SetTexture(0,texture));
        DST_READBACK_CALL(device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1));
        DST_READBACK_CALL(device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE));
        DST_READBACK_CALL(device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1));
        DST_READBACK_CALL(device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE));
        DST_READBACK_CALL(device->SetTextureStageState(0,D3DTSS_TEXCOORDINDEX,0));
        DST_READBACK_CALL(device->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE));
        DST_READBACK_CALL(device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE));
        DST_READBACK_CALL(device->SetTextureStageState(1,D3DTSS_ALPHAOP,D3DTOP_DISABLE));
        struct Vertex { float x,y,z,rhw,u,v; };
        const float right = static_cast<float>(desc.Width)-0.5f, bottom = static_cast<float>(desc.Height)-0.5f;
        const Vertex vertices[] = {{-0.5f,-0.5f,0,1,0,0},{right,-0.5f,0,1,1,0},
                                   {-0.5f,bottom,0,1,0,1},{right,bottom,0,1,1,1}};
        stage = "draw_copy";
        DST_READBACK_CALL(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(Vertex)));
        stage = "copy_to_systemmem";
        DST_READBACK_CALL(device->GetRenderTargetData(target.value,staging.value));
        stage = "lock_staging";
        D3DLOCKED_RECT locked{};
        DST_READBACK_CALL(staging.value->LockRect(&locked,nullptr,D3DLOCK_READONLY));
        const size_t stride = static_cast<size_t>(desc.Width)*4;
        if (!locked.pBits || locked.Pitch < 0 || static_cast<size_t>(locked.Pitch) < stride) {
            staging.value->UnlockRect(); return E_FAIL;
        }
        for (size_t row=0; row<desc.Height; ++row)
            std::memcpy(result.data()+row*stride,static_cast<const uint8_t *>(locked.pBits)+row*locked.Pitch,stride);
        stage = "unlock_staging";
        DST_READBACK_CALL(staging.value->UnlockRect());
        return S_OK;
#undef DST_READBACK_CALL
    };
    hr = render();
    restore_result = state.restore();
    if (FAILED(restore_result)) { stage = "restore_state"; return restore_result; }
    if (SUCCEEDED(hr)) { bytes.swap(result); stage = "complete"; }
    return hr;
}

inline void write_bgra_tga(std::ostream &out, uint32_t width, uint32_t height, const std::vector<uint8_t> &bytes) {
    uint8_t header[18]{};
    header[2]=2; header[12]=static_cast<uint8_t>(width); header[13]=static_cast<uint8_t>(width>>8);
    header[14]=static_cast<uint8_t>(height); header[15]=static_cast<uint8_t>(height>>8);
    header[16]=32; header[17]=0x28; // Top origin, 8-bit alpha, no unpremultiplication.
    out.write(reinterpret_cast<const char *>(header),sizeof(header));
    out.write(reinterpret_cast<const char *>(bytes.data()),bytes.size());
}
} // namespace dst_probe
