#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <optional>
#include <ostream>
#include <vector>

namespace dst_probe {
struct TextureInfo {
    uint64_t id = 0;
    uint32_t width = 0, height = 0, depth = 0, format = 0, type = 0;
    uint32_t query_result = 0, desc_result = 0;
};
struct DrawInfo {
    uint32_t ordinal = 0, count = 0, instances = 0, first = 0, first_instance = 0;
    int32_t base_vertex = 0;
    bool indexed = false, skip_request = false;
    uint32_t api = 0, read_flags = 0;
    uint64_t device = 0, vertex_shader = 0, pixel_shader = 0, target = 0;
    uint32_t target_width = 0, target_height = 0, target_format = 0;
    uint32_t viewport_x = 0, viewport_y = 0, viewport_width = 0, viewport_height = 0;
    std::array<TextureInfo, 16> textures{};
};
struct CapturedFrame {
    uint64_t sequence = 0, start_frame = 0;
    uint32_t seen = 0;
    std::vector<DrawInfo> rows;
};

// Bounded one-frame CPU metadata capture. No texture readback or retained COM objects.
class CaptureBuffer {
    mutable std::mutex mutex_;
    void *owner_ = nullptr, *device_ = nullptr;
    bool active_ = false;
    std::atomic<bool> enabled_{false};
    uint64_t sequence_ = 0;
    CapturedFrame frame_;
public:
    static constexpr size_t limit = 4096;
    bool enabled() const { return enabled_.load(std::memory_order_acquire); }
    uint64_t arm(void *owner, void *device, uint64_t start_frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ || !owner || !device) return 0;
        frame_ = {};
        frame_.rows.reserve(limit);
        frame_.sequence = ++sequence_;
        frame_.start_frame = start_frame;
        owner_ = owner;
        device_ = device;
        active_ = true;
        enabled_.store(true, std::memory_order_release);
        return sequence_;
    }
    uint64_t ticket(void *device) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_ && device == device_ ? sequence_ : 0;
    }
    void record(uint64_t ticket, const DrawInfo &row) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || ticket != sequence_) return;
        ++frame_.seen;
        if (frame_.rows.size() < limit) frame_.rows.push_back(row);
    }
    std::optional<CapturedFrame> finish(void *owner) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || owner != owner_) return std::nullopt;
        active_ = false;
        enabled_.store(false, std::memory_order_release);
        owner_ = device_ = nullptr;
        return std::move(frame_);
    }
    bool cancel(void *owner) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || owner != owner_) return false;
        active_ = false;
        enabled_.store(false, std::memory_order_release);
        owner_ = device_ = nullptr;
        frame_ = {};
        return true;
    }
};

inline void write_capture_csv(std::ostream &out, const CapturedFrame &capture) {
    out << "capture_id,frame,draw,kind,count,instances,first,base_vertex,first_instance,skip_request,"
           "api,device,vertex_shader,pixel_shader,render_target,rt_width,rt_height,rt_format,"
           "viewport_x,viewport_y,viewport_width,viewport_height,read_flags,texture_slot,"
           "texture_id,texture_width,texture_height,texture_depth,texture_format,texture_type,"
           "texture_query_result,texture_desc_result\n";
    for (const auto &row : capture.rows) {
        bool emitted = false;
        auto emit = [&](int slot, const TextureInfo &texture) {
            out << capture.sequence << ',' << capture.start_frame + 1 << ',' << row.ordinal << ','
                << (row.indexed ? "indexed" : "draw") << ',' << row.count << ',' << row.instances << ','
                << row.first << ',' << row.base_vertex << ',' << row.first_instance << ',' << row.skip_request << ','
                << row.api << ",0x" << std::hex << row.device << ",0x" << row.vertex_shader << ",0x"
                << row.pixel_shader << ",0x" << row.target << std::dec << ',' << row.target_width << ','
                << row.target_height << ',' << row.target_format << ',' << row.viewport_x << ',' << row.viewport_y
                << ',' << row.viewport_width << ',' << row.viewport_height << ',' << row.read_flags << ',' << slot
                << ",0x" << std::hex << texture.id << std::dec << ',' << texture.width << ',' << texture.height
                << ',' << texture.depth << ',' << texture.format << ',' << texture.type << ','
                << texture.query_result << ',' << texture.desc_result << '\n';
            emitted = true;
        };
        for (size_t slot = 0; slot < row.textures.size(); ++slot) {
            const auto &texture = row.textures[slot];
            if (texture.id || texture.query_result) emit(static_cast<int>(slot), texture);
        }
        if (!emitted) emit(-1, {});
    }
}
} // namespace dst_probe
