#include "internal/runtime_impl.hpp"
#include <sstream>
#include <iomanip>
#include <unordered_set>

namespace reg {
std::unique_ptr<RegisterMap::Node>
// Entity configuration, Resolver binding and dynamic hook slots.
RegisterMap::configure_node(std::string name,
                            const detail::RegisterConfig& spec,
                            unsigned value_size,
                            std::optional<std::uint32_t> instance,
                            std::optional<ResetScope> default_scope) {
    auto n = std::make_unique<Node>();
    n->owner = this;
    n->name = std::move(name);
    n->declaration_location = spec.location;
    n->dims.assign(spec.dimensions.begin(), spec.dimensions.end());

    n->value_size = value_size;
    n->builtin = spec.builtin;
    n->custom_behavior = spec.scalar_factory || spec.wide_factory;
    n->declared_array = spec.is_array;
    n->writable_mask = spec.writable_mask->low_u64() & n->value_mask();
    n->reset_value = spec.reset_value->low_u64() & n->value_mask();
    if (value_size > 8) {
        n->wide = std::make_unique<detail::WideNodeData>();
        n->wide->wide_writable_mask = *spec.writable_mask & n->wide_mask();
        n->wide->wide_reset_value = *spec.reset_value & n->wide_mask();
        if (spec.wide_factory)
            n->wide->wide_behavior = spec.wide_factory(spec.custom_behavior);
    } else if (spec.scalar_factory)
        n->behavior = spec.scalar_factory(spec.custom_behavior);
    n->reset_domains = spec.domains;
    n->reset_scope = spec.explicit_scope || !default_scope ? spec.scope : *default_scope;
    n->widths = spec.widths;
    n->transaction_policy = spec.transaction_policy.value_or(
        n->is_wide() ? TransactionPolicy::LOW_TO_HIGH : TransactionPolicy::INDEPENDENT);
    n->log_policy = spec.log_policy;
    n->snapshot = {spec.stable_id.empty() ? n->name : std::string(spec.stable_id),
                   spec.order,
                   spec.snapshot_kind};
    n->instance = instance;
    std::size_t count = 1;
    for (auto d : n->dims) {
        if (!d || d > UINT32_MAX || count > UINT32_MAX / d)
            throw Error("invalid dimensions", ErrorKind::Configuration);
        count *= d;
    }
    if (!spec.is_array && count != 1)
        throw Error("declare register arrays with .array(dims(...),strides(...))",
                    ErrorKind::Configuration);
    for (auto stride : spec.stride_bytes)
        if (stride % 4)
            throw Error("register stride must be 4-byte aligned", ErrorKind::Configuration);
    return n;
}
RegisterMap::Node* RegisterMap::create_node(std::uint64_t address,
                                            std::string name,
                                            const detail::RegisterConfig& spec,
                                            const detail::StorageType& type,
                                            std::optional<std::uint32_t> instance,
                                            std::optional<ResetScope> default_scope,
                                            bool storage_only) {
    mutable_layout();
    if (address % 4)
        throw Error("register base must be 4-byte aligned", ErrorKind::Configuration);
    auto n = configure_node(std::move(name), spec, type.bits / 8, instance, default_scope);
    std::size_t count = 1;
    for (auto d : n->dims)
        count *= d;
    auto initial = spec.initial_reset
                       ? (n->is_wide() ? n->wide->wide_reset_value : WideRegData(n->reset_value))
                       : WideRegData{};
    n->values = detail::make_storage(type, count, initial);
    n->scalar_data = n->values->scalar_bytes();
    n->scalar_stride = type.bytes;
    n->direct_wide_storage = type.direct_wide;
    return install_node(address, std::move(n), spec.stride_bytes, storage_only);
}
std::tuple<std::uint64_t, AccessRoute, std::optional<AccessIdentity>>
RegisterMap::resolve_address_with_identity(std::uint64_t address, unsigned size) {
    DispatchGuard guard(*this);
    auto r = resolve_metadata(address, size);
    return std::tuple{r.alias.target, r.alias.route, r.identity};
}
std::pair<std::uint64_t, AccessRoute> RegisterMap::resolve_address_route(std::uint64_t address,
                                                                         unsigned size) const {
    if (address >= size_ || !RegisterAccessWidthPolicy::size_bit(size) || (address & (size - 1)) ||
        size > size_ - address) {
        require_initialized();
        throw Error("invalid resolve address", ErrorKind::Unsupported);
    }
    auto r = resolve_static(address, size);
    return std::pair{r.target, r.route};
}

std::uint64_t RegisterMap::size_bytes() const {
    return size_;
}
std::size_t RegisterMap::mapped_view_count() const noexcept {
    return std::count_if(primary_views_.begin(),
                         primary_views_.end(),
                         [](const auto& v) {
                             return v->mapped;
                         }) +
           std::count_if(windows_.begin(), windows_.end(), [](const auto& w) {
               return !w.terminal;
           });
}
std::weak_ptr<void> RegisterMap::lifetime() const {
    return life_;
}
void RegisterMap::bind_identity_resolver(std::shared_ptr<const IdentityFn> callback,
                                         detail::CallbackDescription description) {
    require_initialized();
    identity_resolver_ = std::move(callback);
    identity_description_ = std::move(description);
    emit_resolver_update("identity", identity_description_, bool(identity_resolver_));
}
void RegisterMap::bind_access_resolver(
    const void* key,
    std::string_view name,
    std::shared_ptr<const detail::AccessResolverSlot::Function> callback,
    detail::CallbackDescription description) {
    auto slot = access_resolver_slot(key, name);
    slot->callback = std::move(callback);
    slot->description = std::move(description);
    emit_resolver_update(slot->name, slot->description, bool(slot->callback));
}
void RegisterMap::release_retired_pre_access() noexcept {
    while (!retired_pre_access_.empty()) {
        decltype(retired_pre_access_) pending;
        pending.swap(retired_pre_access_);
        pending.clear();
    }
}
void RegisterMap::retire_pre_access(const std::shared_ptr<const PreFn>& callback) {
    if (pre_access_active_ && callback)
        retired_pre_access_.push_back(callback);
}
void RegisterMap::bind_pre_access(std::shared_ptr<const PreFn> callback) {
    require_initialized();
    retire_pre_access(pre_access_);
    pre_access_ = std::move(callback);
}
std::shared_ptr<detail::PreAccessSlot> RegisterMap::view_pre_access_slot(const void* key,
                                                                         std::string_view name) {
    require_initialized();
    auto& slot = access_resolvers_->pre_access[key];
    if (!slot) {
        slot = std::make_shared<detail::PreAccessSlot>();
        slot->name = name;
    }
    return slot;
}
void RegisterMap::bind_view_pre_access(const void* key,
                                       std::string_view name,
                                       std::shared_ptr<const PreFn> callback,
                                       detail::CallbackDescription description) {
    auto slot = view_pre_access_slot(key, name);
    retire_pre_access(slot->callback);
    slot->callback = std::move(callback);
    slot->description = std::move(description);
    emit_resolver_update("pre_access:" + slot->name, slot->description, bool(slot->callback));
}
void RegisterMap::clear_view_pre_access(const void* key) {
    require_initialized();
    auto it = access_resolvers_->pre_access.find(key);
    if (it != access_resolvers_->pre_access.end()) {
        retire_pre_access(it->second->callback);
        it->second->callback.reset();
        emit_resolver_update("pre_access:" + it->second->name, it->second->description, false);
    }
}
bool RegisterMap::has_view_pre_access(const void* key) const {
    if (!access_resolvers_)
        return false;
    auto it = access_resolvers_->pre_access.find(key);
    return it != access_resolvers_->pre_access.end() && bool(it->second->callback);
}
void RegisterMap::defer_view_pre_access(const void* key, std::string_view name) {
    mutable_layout();
    view_pre_access_slot(key, name)->deferred = true;
}
void RegisterMap::validate_resolver_bindings() const {
    std::vector<std::string> missing;
    if (std::any_of(identities_.begin(),
                    identities_.end(),
                    [](const auto& w) {
                        return !w.resolver;
                    }) &&
        !identity_resolver_ && !identity_deferred_)
        missing.push_back("identity");
    for (const auto& [key, slot] : access_resolvers_->identities) {
        (void)key;
        if (slot->required && !slot->callback && !slot->deferred)
            missing.push_back("identity:" + slot->name);
    }
    for (const auto& [key, slot] : access_resolvers_->slots) {
        (void)key;
        if (slot->required && !slot->callback && !slot->deferred)
            missing.push_back("index:" + slot->name);
    }
    for (const auto& [key, slot] : access_resolvers_->pre_access) {
        (void)key;
        if (slot->required && !slot->callback && !slot->deferred)
            missing.push_back("pre_access:" + slot->name);
    }
    if (missing.empty())
        return;
    std::sort(missing.begin(), missing.end());
    std::string message = "unbound resolvers before finish: ";
    for (const auto& name : missing) {
        if (message.back() != ' ')
            message += ", ";
        message += name;
    }
    throw Error(std::move(message), ErrorKind::Configuration);
}
std::shared_ptr<detail::AccessResolverSlot>
RegisterMap::access_resolver_slot(const void* id, std::string_view name) {
    require_initialized();
    auto& slot = access_resolvers_->slots[id];
    if (!slot) {
        slot = std::make_shared<detail::AccessResolverSlot>();
        slot->name = name;
    }
    return slot;
}
void RegisterMap::clear_access_resolver(const void* id) {
    require_initialized();
    auto it = access_resolvers_->slots.find(id);
    if (it != access_resolvers_->slots.end()) {
        it->second->callback.reset();
        emit_resolver_update(it->second->name, it->second->description, false);
    }
}
bool RegisterMap::has_access_resolver(const void* id) const {
    if (!access_resolvers_)
        return false;
    auto it = access_resolvers_->slots.find(id);
    return it != access_resolvers_->slots.end() && bool(it->second->callback);
}
namespace detail {
bool overlaps(std::uint64_t a, std::uint64_t n, std::uint64_t b, std::uint64_t m) {
    return a < b + m && b < a + n;
}
std::size_t Access::storage_bytes(const EntityState* node) {
    return node->storage_bytes();
}
WideRegData Access::read_wide(const EntityState* node, std::size_t index) {
    return node->read_wide(index);
}
const RegisterMap* Access::owner(const EntityState* node) {
    return node->owner;
}
std::string join_name(std::string_view prefix, std::string_view name) {
    if (prefix.empty())
        return std::string(name);
    if (name.empty())
        return std::string(prefix);
    return std::string(prefix) + '.' + std::string(name);
}
void* HandleState::value_pointer(const void* token) const {
    check();
    return node_->value_pointer(index_, token);
}
std::size_t HandleState::checked_element(std::size_t i) const {
    check();
    if (i >= count_)
        throw Error("register view element out of range", ErrorKind::Access);
    return index_ + i;
}
std::size_t HandleState::checked_element(RegElementIndex index) const {
    check();
    if (index.linear() < index_ || index.linear() - index_ >= count_ ||
        node_->layout().elements[index.linear()].index != index)
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
void HandleState::ensure_source_available() const {
    auto* n = checked_node();
    if (n->read_callback || (n->wide && n->wide->wide_read_callback))
        throw Error("on_read already registered: " + n->name, ErrorKind::Configuration);
}
void HandleState::ensure_event_available(EventKind kind) const {
    auto* n = checked_node();
    bool exists = kind == EventKind::Write
                      ? bool(n->write_callback) || (n->wide && bool(n->wide->wide_write_callback))
                      : bool(n->read_complete_callback) ||
                            (n->wide && bool(n->wide->wide_read_complete_callback));
    if (exists)
        throw Error(std::string(kind == EventKind::Write ? "on_write" : "on_read_complete") +
                        " already registered: " + n->name,
                    ErrorKind::Configuration);
}
void HandleState::set_source(std::shared_ptr<const ScalarReadSource> callback) const {
    checked_node()->read_callback = std::move(callback);
}
void HandleState::set_source(std::shared_ptr<const WideReadSource> callback) const {
    checked_node()->wide->wide_read_callback = std::move(callback);
}
void HandleState::set_event(EventKind kind, std::shared_ptr<const EventCallback> callback) const {
    auto* n = checked_node();
    if (kind == EventKind::Write) {
        if (n->is_wide())
            n->wide->wide_write_callback = std::move(callback);
        else
            n->write_callback = std::move(callback);
    } else {
        if (n->is_wide())
            n->wide->wide_read_complete_callback = std::move(callback);
        else
            n->read_complete_callback = std::move(callback);
    }
}
} // namespace detail
} // namespace reg
