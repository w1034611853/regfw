#include "internal/runtime_impl.hpp"
#include <sstream>
#include <iomanip>
#include <unordered_set>

namespace reg {

namespace detail {
HandleState::HandleState() = default;
HandleState::HandleState(EntityState* node,
                         std::size_t index,
                         const std::weak_ptr<void>& life,
                         std::size_t count)
    : node_(node), index_(index), lifetime_(life) {
    check();
    if (index > node->element_count() ||
        (count != std::dynamic_extent && count > node->element_count() - index))
        throw Error("register view extent out of range", ErrorKind::Access);
    count_ = count == std::dynamic_extent ? node->element_count() - index : count;
}
HandleState::HandleState(EntityState* node,
                         std::size_t index,
                         const std::shared_ptr<void>& life,
                         std::size_t count)
    : HandleState(node, index, std::weak_ptr<void>(life), count) {
}
HandleState::HandleState(const HandleState&) = default;
HandleState::HandleState(HandleState&&) noexcept = default;
HandleState::~HandleState() = default;
} // namespace detail
namespace detail {
void HandleState::check() const {
    if (!node_ || lifetime_.expired())
        throw Error("register handle outlived its map");
    node_->check_borrow();
}
EntityState* HandleState::checked_node() const {
    check();
    return node_;
}
bool HandleState::is_alive() const noexcept {
    return node_ && !lifetime_.expired();
}
std::size_t HandleState::size() const {
    if (!node_)
        return 0;
    check();
    return count_;
}
void HandleState::reset() const {
    check();
    node_->reset();
}
const RegEntity& HandleState::entity() const {
    check();
    return *node_->entity;
}
std::uint64_t HandleState::address() const {
    check();
    if (!count_)
        throw Error("empty register view has no address", ErrorKind::Access);
    if (!node_->layout().element_mapped.at(index_))
        throw Error("RegEntity element has no address mapping", ErrorKind::Access);
    return node_->layout().elements[index_].address;
}
std::uint64_t HandleState::address_span_bytes() const {
    check();
    if (!count_)
        return 0;
    const auto& elements = node_->layout().elements;
    for (std::size_t i = index_; i < index_ + count_; ++i)
        if (!node_->layout().element_mapped.at(i))
            throw Error("RegEntity element has no address mapping", ErrorKind::Access);
    if (elements[index_ + count_ - 1].address < elements[index_].address)
        throw Error("RegView address mapping is not monotonic", ErrorKind::Access);
    return std::uint64_t(elements[index_ + count_ - 1].address) - elements[index_].address +
           std::max((node_->value_size + 3u) & ~3u, 4u);
}
std::string HandleState::diagnostic_name() const {
    check();
    return count_ ? node_->layout().elements[index_].name : node_->name + "[empty]";
}
RegisterMetadata HandleState::metadata() const {
    auto n = checked_node();
    return node_metadata(*n);
}
bool HandleState::has_on_read() const {
    auto* n = checked_node();
    return bool(n->read_callback) || (n->wide && bool(n->wide->wide_read_callback));
}
bool HandleState::has_on_write() const {
    auto* n = checked_node();
    return bool(n->write_callback) || (n->wide && bool(n->wide->wide_write_callback));
}
bool HandleState::has_on_read_complete() const {
    auto* n = checked_node();
    return bool(n->read_complete_callback) ||
           (n->wide && bool(n->wide->wide_read_complete_callback));
}
} // namespace detail

} // namespace reg
