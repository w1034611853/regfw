#include "internal/runtime_impl.hpp"
#include <sstream>
#include <iomanip>
#include <unordered_set>

namespace reg::detail {
CallbackStorage::CallbackStorage(std::size_t bytes, std::size_t alignment)
    : alignment_(std::max(alignment, alignof(std::max_align_t))) {
    if (bytes <= INLINE_BYTES && alignment <= alignof(std::max_align_t))
        target_ = inline_storage_;
    else
        target_ = ::operator new(bytes, std::align_val_t(alignment_));
}
CallbackStorage::~CallbackStorage() {
    if (destroy_)
        destroy_(target_);
    if (target_ != inline_storage_)
        ::operator delete(target_, std::align_val_t(alignment_));
}
} // namespace reg::detail
