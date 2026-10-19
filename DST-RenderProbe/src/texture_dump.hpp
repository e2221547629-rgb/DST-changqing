#pragma once
#include "d3d9_capture.hpp"
#include <vector>
#include <cstring>
#include <ostream>

namespace dst_probe {
// Read only the top BC3 level. No binding, render state or game data is changed.
// DEFAULT-pool textures may reject this; report that HRESULT instead of guessing.
inline HRESULT read_bc3(IDirect3DTexture9 *texture, std::vector<uint8_t> &bytes) {
    bytes.clear();
    if (!texture) return E_POINTER;
    D3DSURFACE_DESC desc{};
    HRESULT hr = texture->GetLevelDesc(0, &desc);
    if (FAILED(hr)) return hr;
    if (desc.Format != D3DFMT_DXT5 || !desc.Width || !desc.Height || desc.Width > 4096 || desc.Height > 4096)
        return E_INVALIDARG;
    const size_t stride = ((desc.Width + 3) / 4) * 16;
    const size_t rows = (desc.Height + 3) / 4;
    // Allocate before LockRect, so allocation failures cannot leave a texture locked.
    std::vector<uint8_t> result(stride * rows);
    D3DLOCKED_RECT locked{};
    hr = texture->LockRect(0, &locked, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) return hr;
    if (!locked.pBits || locked.Pitch < 0 || static_cast<size_t>(locked.Pitch) < stride) {
        texture->UnlockRect(0);
        return E_FAIL;
    }
    for (size_t row = 0; row < rows; ++row)
        std::memcpy(result.data() + row * stride,
            static_cast<const uint8_t *>(locked.pBits) + row * locked.Pitch, stride);
    hr = texture->UnlockRect(0);
    if (SUCCEEDED(hr)) bytes.swap(result);
    return hr;
}
inline void write_bc3_dds(std::ostream &out, uint32_t width, uint32_t height, const std::vector<uint8_t> &bytes) {
    const uint32_t header[31] = {
        124, 0x81007, height, width, static_cast<uint32_t>(bytes.size()), 0, 1,
        0,0,0,0,0,0,0,0,0,0,0, 32,4, static_cast<uint32_t>(D3DFMT_DXT5),
        0,0,0,0,0, 0x1000,0,0,0,0
    };
    out.write("DDS ", 4);
    out.write(reinterpret_cast<const char *>(header), sizeof(header));
    out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}
} // namespace dst_probe
