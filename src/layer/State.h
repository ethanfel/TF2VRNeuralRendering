#pragma once
#include <cstdint>
#include <deque>
#include <optional>
#include <tuple>

namespace tf2vrnr {
// Acquire can precede multiple releases. A timeout is NOT a successful wait,
// even though XR_TIMEOUT_EXPIRED is a nonnegative XrResult.
class ImageQueue {
    struct Entry { uint32_t index; bool waited; };
    std::deque<Entry> entries_;
public:
    void Acquired(uint32_t index) { entries_.push_back({index, false}); }
    void Waited() {
        for (auto& e : entries_) if (!e.waited) { e.waited = true; break; }
    }
    std::optional<uint32_t> Writable() const {
        if (entries_.empty() || !entries_.front().waited) return {};
        return entries_.front().index;
    }
    void Released() { if (!entries_.empty() && entries_.front().waited) entries_.pop_front(); }
};

struct ViewLayout {
    uint64_t swapchain = 0;
    uint32_t slice = 0;
    int32_t x = 0, y = 0, width = 0, height = 0;
    auto Fields() const { return std::tie(swapchain, slice, x, y, width, height); }
    bool operator==(const ViewLayout& b) const { return Fields() == b.Fields(); }
};

// A distinct instance belongs to EACH projection-layer/view pair, scoped to
// an OpenXR session. Never key temporal history by swapchain image index:
// images rotate, and both eyes may share a texture array or atlas.
class ViewHistory {
    bool valid_ = false;
    int64_t time_ = 0;
    ViewLayout layout_;
    uint64_t generation_ = 0;
public:
    bool Observe(const ViewLayout& layout, int64_t time) {
        const bool reset = !valid_ || !(layout == layout_) || time <= time_ || time - time_ > 250000000;
        if (reset) ++generation_;
        valid_ = true; time_ = time; layout_ = layout;
        return reset;
    }
    void Invalidate() { valid_ = false; }
    uint64_t Generation() const { return generation_; }
};

inline bool Fits(uint32_t width, uint32_t height, uint32_t arraySize, const ViewLayout& view) {
    return view.x >= 0 && view.y >= 0 && view.width > 0 && view.height > 0 && view.slice < arraySize &&
        static_cast<uint64_t>(view.x) + static_cast<uint32_t>(view.width) <= width &&
        static_cast<uint64_t>(view.y) + static_cast<uint32_t>(view.height) <= height;
}
}
