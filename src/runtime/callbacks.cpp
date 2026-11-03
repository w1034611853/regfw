#include "internal/runtime_impl.hpp"
#include <sstream>
#include <iomanip>
#include <unordered_set>

namespace reg::detail {
EventCallback::EventCallback(
    std::size_t bytes, std::size_t alignment, Invoke invoke, Destroy destroy, FrameInvoke frame)
    : alignment_(std::max(alignment, alignof(std::max_align_t))), invoke_(invoke),
      frame_invoke_(frame), destroy_(destroy) {
    if (bytes <= INLINE_BYTES && alignment <= alignof(std::max_align_t))
        target_ = inline_storage_;
    else
        target_ = ::operator new(bytes, std::align_val_t(alignment_));
}
EventCallback::~EventCallback() {
    if (constructed_ && destroy_)
        destroy_(target_);
    if (target_ != inline_storage_)
        ::operator delete(target_, std::align_val_t(alignment_));
}
std::shared_ptr<EventCallback> allocate_event(std::size_t bytes,
                                              std::size_t alignment,
                                              EventCallback::Invoke invoke,
                                              EventCallback::Destroy destroy,
                                              EventCallback::FrameInvoke frame) {
    return std::make_shared<EventCallback>(bytes, alignment, invoke, destroy, frame);
}
} // namespace reg::detail
