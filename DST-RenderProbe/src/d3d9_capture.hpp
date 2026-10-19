#pragma once
#include <d3d9.h>
#include "frame_capture.hpp"

namespace dst_probe {
template<class T> struct ComRef {
    T *value = nullptr;
    ComRef() = default;
    ComRef(const ComRef &) = delete;
    ComRef &operator=(const ComRef &) = delete;
    ~ComRef() { if (value) value->Release(); }
};
template<class T> uint64_t identity(T *value) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value)); }

// Query the native device's actual current state, including state restored via state blocks.
// This avoids relying on cached binding events that could be incomplete.
inline void read_d3d9_state(IDirect3DDevice9 *device, DrawInfo &row) {
    row.api = 0x9000;
    row.device = identity(device);
    D3DVIEWPORT9 viewport{};
    if (SUCCEEDED(device->GetViewport(&viewport))) {
        row.read_flags |= 1;
        row.viewport_x = viewport.X; row.viewport_y = viewport.Y;
        row.viewport_width = viewport.Width; row.viewport_height = viewport.Height;
    }
    ComRef<IDirect3DVertexShader9> vs;
    if (SUCCEEDED(device->GetVertexShader(&vs.value))) {
        row.read_flags |= 2;
        row.vertex_shader = identity(vs.value);
    }
    ComRef<IDirect3DPixelShader9> ps;
    if (SUCCEEDED(device->GetPixelShader(&ps.value))) {
        row.read_flags |= 4;
        row.pixel_shader = identity(ps.value);
    }
    ComRef<IDirect3DSurface9> target;
    if (SUCCEEDED(device->GetRenderTarget(0, &target.value)) && target.value) {
        row.target = identity(target.value);
        D3DSURFACE_DESC desc{};
        if (SUCCEEDED(target.value->GetDesc(&desc))) {
            row.read_flags |= 8;
            row.target_width = desc.Width; row.target_height = desc.Height; row.target_format = desc.Format;
        }
    }
    for (DWORD slot = 0; slot < row.textures.size(); ++slot) {
        auto &info = row.textures[slot];
        ComRef<IDirect3DBaseTexture9> texture;
        const HRESULT query = device->GetTexture(slot, &texture.value);
        info.query_result = static_cast<uint32_t>(query);
        if (FAILED(query) || !texture.value) continue;
        info.id = identity(texture.value);
        info.type = texture.value->GetType();
        HRESULT result = E_NOTIMPL;
        if (info.type == D3DRTYPE_TEXTURE || info.type == D3DRTYPE_CUBETEXTURE) {
            D3DSURFACE_DESC desc{};
            result = info.type == D3DRTYPE_TEXTURE
                ? static_cast<IDirect3DTexture9 *>(texture.value)->GetLevelDesc(0, &desc)
                : static_cast<IDirect3DCubeTexture9 *>(texture.value)->GetLevelDesc(0, &desc);
            if (SUCCEEDED(result)) {
                info.width = desc.Width; info.height = desc.Height; info.depth = 1; info.format = desc.Format;
            }
        } else if (info.type == D3DRTYPE_VOLUMETEXTURE) {
            D3DVOLUME_DESC desc{};
            result = static_cast<IDirect3DVolumeTexture9 *>(texture.value)->GetLevelDesc(0, &desc);
            if (SUCCEEDED(result)) {
                info.width = desc.Width; info.height = desc.Height; info.depth = desc.Depth; info.format = desc.Format;
            }
        }
        info.desc_result = static_cast<uint32_t>(result);
    }
}
} // namespace dst_probe
