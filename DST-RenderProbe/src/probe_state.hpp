#pragma once
#include <atomic>
#include <cstdint>

namespace dst_probe {
enum Key : uint32_t { F7 = 1, F8 = 2, F9 = 4, F10 = 8, F11 = 16 };
struct Input { bool foreground = false; uint32_t keys = 0; bool shift = false; };
struct Frame { uint32_t draws, hits, requests, counted_target; };

// Draw callbacks can run on other threads. Controls run on present.
// Multiple queues and swap chains still have approximate frame boundaries.
class State {
    std::atomic<uint32_t> draws_{0}, hits_{0}, requests_{0};
    std::atomic<uint64_t> burst_requests_{0}, burst_until_{0};
    std::atomic<uint32_t> target_{1};
    std::atomic<bool> single_{false};
    uint32_t previous_keys_ = 0, repeat_key_ = 0;
    uint64_t next_repeat_ = 0;
public:
    uint32_t target() const { return target_.load(std::memory_order_relaxed); }
    bool single() const { return single_.load(std::memory_order_relaxed); }
    uint64_t burst_until() const { return burst_until_.load(std::memory_order_relaxed); }
    uint64_t burst_requests() const { return burst_requests_.load(std::memory_order_relaxed); }
    bool draw(uint64_t now) {
        const uint32_t index = draws_.fetch_add(1, std::memory_order_relaxed) + 1;
        const bool hit = index == target();
        if (hit) hits_.fetch_add(1, std::memory_order_relaxed);
        const bool burst = now < burst_until();
        const bool request = burst || (single() && hit);
        if (request) requests_.fetch_add(1, std::memory_order_relaxed);
        if (burst) burst_requests_.fetch_add(1, std::memory_order_relaxed);
        return request; // A request to ReShade, not proof of visible GPU changes.
    }
    Frame finish_frame() {
        return {draws_.exchange(0, std::memory_order_relaxed),
                hits_.exchange(0, std::memory_order_relaxed),
                requests_.exchange(0, std::memory_order_relaxed), target()};
    }
    const char *update(uint64_t now, Input input, uint32_t last_draws) {
        const uint32_t pressed = input.keys & ~previous_keys_;
        previous_keys_ = input.keys;
        if (!input.foreground) {
            repeat_key_ = 0;
            const bool active = single() || burst_until() != 0;
            single_.store(false, std::memory_order_relaxed);
            burst_until_.store(0, std::memory_order_relaxed);
            return active ? "focus_lost" : nullptr;
        }
        if (pressed & F11) {
            target_.store(1, std::memory_order_relaxed);
            single_.store(false, std::memory_order_relaxed);
            burst_until_.store(0, std::memory_order_relaxed);
            repeat_key_ = 0;
            return "reset";
        }
        const char *reason = nullptr;
        if (burst_until() != 0 && now >= burst_until()) {
            burst_until_.store(0, std::memory_order_relaxed);
            reason = "burst_end";
        }
        if (pressed & F7) {
            single_.store(false, std::memory_order_relaxed);
            burst_requests_.store(0, std::memory_order_relaxed);
            burst_until_.store(now + 500, std::memory_order_relaxed);
            reason = "burst_start";
        } else if (pressed & F8) {
            burst_until_.store(0, std::memory_order_relaxed);
            single_.store(!single(), std::memory_order_relaxed);
            reason = "key";
        }
        const uint32_t direction = (input.keys & F9) ? F9 : ((input.keys & F10) ? F10 : 0);
        if (!direction) {
            repeat_key_ = 0;
        } else if (direction != repeat_key_ || now >= next_repeat_) {
            const bool first = direction != repeat_key_;
            repeat_key_ = direction;
            next_repeat_ = now + (first ? 400 : 60);
            const uint32_t step = input.shift ? 100 : 1;
            uint32_t target = this->target();
            if (direction == F9) {
                target = target > step ? target - step : 1;
            } else {
                const uint32_t limit = last_draws > 0 ? last_draws : 1;
                target = target >= limit || step >= limit - target ? limit : target + step;
            }
            target_.store(target, std::memory_order_relaxed);
            reason = "key";
        }
        return reason;
    }
};
} // namespace dst_probe
