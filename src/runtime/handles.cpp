#include "internal/runtime_impl.hpp"
#include <sstream>
#include <iomanip>
#include <unordered_set>

namespace reg {

namespace detail {
HandleState::HandleState() = default;
HandleState::HandleState(EntityState* node, std::size_t index, std::size_t count)
    : node_(node), index_(index) {
    check();
    if (index > node->element_count() ||
        (count != std::dynamic_extent && count > node->element_count() - index))
        throw Error("register view extent out of range", ErrorKind::Access);
    count_ = count == std::dynamic_extent ? node->element_count() - index : count;
}
std::weak_ptr<void> HandleState::weak_lifetime() const {
    return is_alive() ? node_->owner->lifetime() : std::weak_ptr<void>{};
}
} // namespace detail
namespace detail {
void HandleState::unbound_handle() {
    throw Error("register handle is not bound");
}
void HandleState::element_out_of_range() {
    throw Error("register view element out of range", ErrorKind::Access);
}
std::size_t HandleState::checked_element(RegElementIndex index) const {
    check();
    if (index.linear() < index_ || index.linear() - index_ >= count_ ||
        node_->layout().indices[index.linear()] != index)
        throw Error("element does not match register shape", ErrorKind::Access);
    return index.linear();
}
std::size_t HandleState::element_index(std::span<const std::size_t> coordinates) const {
    check();
    std::size_t linear = 0;
    if (coordinates.size() != node_->dims.size())
        throw Error("typed handle rank mismatch");
    if (coordinates.size() == 1)
        return checked_element(coordinates[0]);
    for (std::size_t d = 0; d < coordinates.size(); ++d) {
        if (coordinates[d] >= node_->dims[d])
            throw Error("array coordinate out of range", ErrorKind::Access);
        linear = linear * node_->dims[d] + coordinates[d];
    }
    if (linear < index_ || linear - index_ >= count_)
        throw Error("coordinate outside register view", ErrorKind::Access);
    return linear;
}
EntityState* HandleState::checked_node() const {
    check();
    return node_;
}
bool HandleState::is_alive() const noexcept {
    return node_ != nullptr;
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
    return node_->layout().mapped_addresses[index_];
}
std::uint64_t HandleState::address_span_bytes() const {
    check();
    if (!count_)
        return 0;
    const auto& addresses = node_->layout().mapped_addresses;
    for (std::size_t i = index_; i < index_ + count_; ++i)
        if (!node_->layout().element_mapped.at(i))
            throw Error("RegEntity element has no address mapping", ErrorKind::Access);
    if (addresses[index_ + count_ - 1] < addresses[index_])
        throw Error("RegView address mapping is not monotonic", ErrorKind::Access);
    return std::uint64_t(addresses[index_ + count_ - 1]) - addresses[index_] +
           std::max((node_->value_size + 3u) & ~3u, 4u);
}
std::string HandleState::diagnostic_name() const {
    check();
    return count_ ? node_->layout().names[index_] : node_->name + "[empty]";
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
