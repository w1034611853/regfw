#include "internal/runtime_impl.hpp"
#include <sstream>
#include <iomanip>
#include <unordered_set>

namespace reg {

// RegisterMap lifetime and initialization.
RegisterMap::RegisterMap() noexcept : lookup_(0), aliases_(0) {
}

RegisterMap::RegisterMap(std::uint64_t size, std::string name, HLogLogger* logger) : RegisterMap() {
    init(size, std::move(name), logger);
}

void RegisterMap::init(std::uint64_t size, std::string name, HLogLogger* logger) {
    if (size_)
        throw Error("register map has already been initialized", ErrorKind::Configuration);
    const auto checked_size = checked_map_size(size);
    detail::SparseLookup<Entry> next_lookup(checked_size);
    detail::SparseLookup<AliasEntry> next_aliases(checked_size);
    auto next_life = std::make_shared<int>(0);
    auto next_resolvers = std::make_unique<ResolverRegistry>();
    auto next_scratch = std::make_unique<AccessScratch>();
    // Complete every allocation before publishing any initialized state.
    lookup_.swap(next_lookup);
    aliases_.swap(next_aliases);
    life_ = std::move(next_life);
    access_resolvers_ = std::move(next_resolvers);
    access_scratch_ = std::move(next_scratch);
    logger_ = logger;
    name_prefix = std::move(name);
    size_ = checked_size;
}

void RegisterMap::register_source_instance(std::uint64_t base,
                                           std::uint64_t size,
                                           std::uint32_t instance) {
    mutable_layout();
    region_check(base, size);
    instances_.push_back({base, size, instance});
    lookup_.for_each_mut([&](auto address, Entry& entry) {
        if (address >= base && address - base < size)
            entry.source_instance = instance;
    });
}

void RegisterMap::register_identity_repeat_window(PhysicalRepeatAddressWindow window,
                                                  std::uint64_t offset,
                                                  std::uint64_t size,
                                                  std::uint32_t domain,
                                                  IdentityRemapMode mode,
                                                  IdentityTraceLabels labels,
                                                  std::string name,
                                                  std::source_location at) {
    detail::IdentityWindow w{window.base,
                             window.count,
                             window.stride,
                             window.size,
                             offset,
                             size,
                             domain,
                             mode,
                             labels,
                             std::move(name),
                             {},
                             at};
    mutable_layout();
    if (!w.count || w.count > UINT32_MAX || !w.stride || w.stride % 4 || !w.aperture_size ||
        w.aperture_size > w.stride || w.region_offset % 4 || !w.region_size || w.region_size % 4 ||
        w.region_offset > w.aperture_size || w.region_size > w.aperture_size - w.region_offset)
        throw Error("invalid identity window", ErrorKind::Configuration);
    if (w.base > size_ || w.aperture_size > size_ - w.base ||
        (w.count - 1) > (size_ - w.base - w.aperture_size) / w.stride)
        throw Error("identity span overflow/outside map", ErrorKind::Configuration);
    region_check(w.base, (w.count - 1) * w.stride + w.aperture_size);
    for (auto& old : identities_)
        for (std::uint64_t a = 0; a < w.count; ++a)
            for (std::uint64_t b = 0; b < old.count; ++b)
                if (detail::overlaps(w.base + a * w.stride + w.region_offset,
                                     w.region_size,
                                     old.base + b * old.stride + old.region_offset,
                                     old.region_size))
                    throw Error(
                        "overlapping identity windows:\n  " +
                            detail::declaration(w.name,
                                                w.base + a * w.stride + w.region_offset,
                                                w.region_size,
                                                at) +
                            "\n  " +
                            detail::declaration(old.name,
                                                old.base + b * old.stride + old.region_offset,
                                                old.region_size,
                                                old.location),
                        ErrorKind::Configuration,
                        at);
    identities_.push_back(std::move(w));
}

RegisterMap::~RegisterMap() = default;
RegEntity::RegEntity(std::unique_ptr<detail::EntityState> state) : state_(std::move(state)) {
    state_->entity = this;
}
RegEntity::~RegEntity() = default;
std::string_view RegEntity::name() const noexcept {
    return state_->name;
}
std::size_t RegEntity::element_count() const noexcept {
    return state_->element_count();
}
std::size_t RegEntity::storage_bytes() const noexcept {
    return state_->storage_bytes();
}
unsigned RegEntity::width_bits() const noexcept {
    return state_->value_size * 8;
}
std::span<const std::size_t> RegEntity::dimensions() const noexcept {
    return state_->dims;
}
void RegEntity::reset() const {
    state_->reset();
}

namespace detail {
RegViewLayout& EntityState::layout() {
    return *owner->primary_views_.at(id);
}
const RegViewLayout& EntityState::layout() const {
    return *owner->primary_views_.at(id);
}
void* EntityState::value_pointer(std::size_t index, const void* token) const {
    if (!values || values->type_token() != token || index >= element_count())
        throw Error("register value type/index mismatch", ErrorKind::Configuration);
    return values->element(index);
}
void* checked_value_pointer(EntityState* node,
                            std::size_t index,
                            const void* token,
                            const std::weak_ptr<void>& life,
                            std::size_t rank,
                            bool empty) {
    if (!node || life.expired())
        throw Error("register handle outlived its map");
    node->check_borrow();
    if (node->dims.size() != rank)
        throw Error("typed handle rank mismatch");
    if (empty) {
        if (index > node->element_count() || node->values->type_token() != token)
            throw Error("invalid empty register view", ErrorKind::Access);
        return nullptr;
    }
    return node->value_pointer(index, token);
}
void* checked_value_pointer(EntityState* node,
                            std::size_t index,
                            const void* token,
                            const std::shared_ptr<void>& life,
                            std::size_t rank,
                            bool empty) {
    if (!node || !life)
        throw Error("register handle outlived its map");
    node->check_borrow();
    if (node->dims.size() != rank)
        throw Error("typed handle rank mismatch");
    if (empty) {
        if (index > node->element_count() || node->values->type_token() != token)
            throw Error("invalid empty register view", ErrorKind::Access);
        return nullptr;
    }
    return node->value_pointer(index, token);
}
} // namespace detail

bool RegisterMap::is_finalized() const {
    return finalized_;
}

// Entity registration: allocate/validate storage and local array geometry.
REGFW_DETAIL_NOINLINE RegisterMap::Node*
RegisterMap::install_node(std::uint64_t address,
                          std::unique_ptr<Node> n,
                          std::span<const std::uint64_t> strides,
                          bool storage_only) {
    auto primary = std::make_unique<detail::RegViewLayout>();
    primary->entity = n.get();
    primary->mapped = !storage_only;
    primary->display_name = n->name;
    primary->element_mapped.assign(n->element_count(), !storage_only);
    primary->strides.assign(strides.begin(), strides.end());
    n->id = nodes_.size();
    const auto count = n->values->size();
    if (!n->transaction_policy.independent())
        n->transaction_state.resize(count);
    primary->elements.reserve(count);
    primary->local_addresses.reserve(count);
    std::vector<std::pair<std::uint64_t, std::size_t>> occupied_ranges;
    if (count > 1)
        occupied_ranges.reserve(count);
    std::vector<std::pair<std::uint64_t, Entry>> entries;
    for (std::size_t k = 0; k < count; ++k) {
        auto idx = RegElementIndex::from_dims(n->dims, k);
        auto a = address;
        auto full = n->name;
        for (std::size_t d = 0; d < n->dims.size(); ++d) {
            auto c = idx.values[d];
            if (c && primary->strides[d] > (UINT64_MAX - a) / c)
                throw Error("register address overflow", ErrorKind::Configuration);
            a += std::uint64_t(c) * primary->strides[d];
            if (count > 1)
                full += '[' + std::to_string(c) + ']';
        }
        const auto occupied = std::max((n->value_size + 3u) & ~3u, 4u);
        region_check(a, occupied);
        if (!storage_only) {
            for (auto& r : reserved_)
                if (detail::overlaps(a, occupied, r.base, r.size))
                    throw Error(
                        "register overlaps reserved region:\n  " +
                            detail::declaration(full, a, occupied, n->declaration_location) +
                            "\n  " + detail::declaration(r.name, r.base, r.size, r.location),
                        ErrorKind::Configuration,
                        n->declaration_location);
            for (auto& w : windows_)
                if (detail::overlaps(a, occupied, w.source_base, w.size))
                    throw Error(
                        "register overlaps alias source:\n  " +
                            detail::declaration(full, a, occupied, n->declaration_location) +
                            "\n  " + detail::declaration(w.name, w.source_base, w.size, w.location),
                        ErrorKind::Configuration,
                        n->declaration_location);
        }
        if (count > 1)
            occupied_ranges.emplace_back(a, k);
        if (!storage_only)
            for (unsigned byte = 0; byte < occupied; byte += 4) {
                if (auto* old = lookup_.get(a + byte))
                    throw Error(
                        "register address collision:\n  " +
                            detail::declaration(full, a, occupied, n->declaration_location) +
                            "\n  " +
                            detail::declaration(old->node->name,
                                                a + byte - old->byte_offset,
                                                old->node->value_size,
                                                old->node->declaration_location),
                        ErrorKind::Configuration,
                        n->declaration_location);
                entries.push_back(
                    {a + byte,
                     {n.get(), static_cast<std::uint32_t>(k), byte, n->instance, primary.get()}});
            }
        primary->local_addresses.push_back(a);
        primary->elements.push_back({a, idx, std::move(full)});
    }
    // Validate whole element intervals, rather than allocating a hash
    // entry (and copying a name) for every occupied 32-bit word.
    // Array strides may enumerate addresses in non-address order.
    if (!std::is_sorted(occupied_ranges.begin(), occupied_ranges.end()))
        std::sort(occupied_ranges.begin(), occupied_ranges.end());
    const auto occupied = std::max((n->value_size + 3u) & ~3u, 4u);
    for (std::size_t k = 1; k < occupied_ranges.size(); ++k) {
        const auto [before, bi] = occupied_ranges[k - 1];
        const auto [after, ai] = occupied_ranges[k];
        if (detail::overlaps(before, occupied, after, occupied))
            throw Error(
                "register array elements overlap:\n  " +
                    detail::declaration(
                        primary->elements[bi].name, before, occupied, n->declaration_location) +
                    "\n  " +
                    detail::declaration(
                        primary->elements[ai].name, after, occupied, n->declaration_location),
                ErrorKind::Configuration,
                n->declaration_location);
    }
    auto* ptr = n.get();
    auto entity = std::unique_ptr<RegEntity>(new RegEntity(std::move(n)));
    auto grow = [](auto& values) {
        if (values.size() == values.capacity())
            values.reserve(values.size() ? values.size() * 2 : 8);
    };
    grow(entities_);
    grow(nodes_);
    grow(primary_views_);
    entities_.push_back(std::move(entity));
    primary_views_.push_back(std::move(primary));
    nodes_.push_back(ptr);
    for (auto& [a, e] : entries)
        lookup_.insert(a, e);
    return ptr;
}

void RegisterMap::reserve_region(std::uint64_t base,
                                 std::uint64_t size,
                                 std::string name,
                                 std::uint64_t repeat_count,
                                 std::uint64_t repeat_stride,
                                 std::uint64_t repeat_index,
                                 std::source_location at) {
    mutable_layout();
    region_check(base, size);
    lookup_.for_each([&](auto a, const auto& e) {
        if (a >= base && a - base < size)
            throw Error("reserved region overlaps register:\n  " +
                            detail::declaration(name, base, size, at) + "\n  " +
                            detail::declaration(e.node->name,
                                                a - e.byte_offset,
                                                e.node->value_size,
                                                e.node->declaration_location),
                        ErrorKind::Configuration,
                        at);
    });
    for (auto& r : reserved_)
        if (detail::overlaps(base, size, r.base, r.size))
            throw Error("reserved region overlap:\n  " + detail::declaration(name, base, size, at) +
                            "\n  " + detail::declaration(r.name, r.base, r.size, r.location),
                        ErrorKind::Configuration,
                        at);
    for (auto& w : windows_)
        if (detail::overlaps(base, size, w.source_base, w.size))
            throw Error("reserved region overlaps alias:\n  " +
                            detail::declaration(name, base, size, at) + "\n  " +
                            detail::declaration(w.name, w.source_base, w.size, w.location),
                        ErrorKind::Configuration,
                        at);
    reserved_.push_back(
        {base, size, std::move(name), repeat_count, repeat_stride, repeat_index, at});
}

void RegisterMap::map_routed_alias_window_with_policy(std::uint64_t source,
                                                      std::uint64_t target,
                                                      std::uint64_t size,
                                                      AccessRoute route,
                                                      AliasAccessPolicy policy,
                                                      std::string name,
                                                      std::source_location at) {
    mutable_layout();
    region_check(source, size);
    region_check(target, size);
    lookup_.for_each([&](auto a, const auto& e) {
        if (a >= source && a - source < size)
            throw Error("alias source overlaps register:\n  " +
                            detail::declaration(name, source, size, at) + "\n  " +
                            detail::declaration(e.node->name,
                                                a - e.byte_offset,
                                                e.node->value_size,
                                                e.node->declaration_location),
                        ErrorKind::Configuration,
                        at);
    });
    for (auto& w : windows_)
        if (detail::overlaps(source, size, w.source_base, w.size))
            throw Error("alias source overlap:\n  " + detail::declaration(name, source, size, at) +
                            "\n  " + detail::declaration(w.name, w.source_base, w.size, w.location),
                        ErrorKind::Configuration,
                        at);
    for (auto& r : reserved_)
        if (detail::overlaps(source, size, r.base, r.size))
            throw Error("alias source overlaps reserved:\n  " +
                            detail::declaration(name, source, size, at) + "\n  " +
                            detail::declaration(r.name, r.base, r.size, r.location),
                        ErrorKind::Configuration,
                        at);
    windows_.push_back({source, target, size, route, std::move(name), policy, false, {}, at});
}

void RegisterMap::map_alias_window(std::uint64_t source,
                                   std::uint64_t target,
                                   std::uint64_t size,
                                   std::string name,
                                   std::source_location at) {
    map_routed_alias_window_with_policy(source,
                                        target,
                                        size,
                                        AccessRoute::ALIAS,
                                        AliasAccessPolicy::READ_WRITE,
                                        std::move(name),
                                        at);
}

void RegisterMap::map_alias_window_with_policy(std::uint64_t source,
                                               std::uint64_t target,
                                               std::uint64_t size,
                                               AliasAccessPolicy policy,
                                               std::string name,
                                               std::source_location at) {
    map_routed_alias_window_with_policy(
        source, target, size, AccessRoute::ALIAS, policy, std::move(name), at);
}

void RegisterMap::map_routed_alias_window(std::uint64_t source,
                                          std::uint64_t target,
                                          std::uint64_t size,
                                          AccessRoute route,
                                          std::string name,
                                          std::source_location at) {
    map_routed_alias_window_with_policy(
        source, target, size, route, AliasAccessPolicy::READ_WRITE, std::move(name), at);
}

void RegisterMap::clear_identity_resolver() {
    require_initialized();
    identity_resolver_.reset();
    emit_resolver_update("identity", identity_description_, false);
}

bool RegisterMap::has_identity_resolver() const {
    return bool(identity_resolver_);
}

void RegisterMap::clear_pre_access() {
    require_initialized();
    retire_pre_access(pre_access_);
    pre_access_.reset();
}

bool RegisterMap::has_pre_access() const {
    return bool(pre_access_);
}

std::uint64_t RegisterMap::resolve_address(std::uint64_t address, unsigned size) const {
    if (address >= size_ || !RegisterAccessWidthPolicy::size_bit(size) || (address & (size - 1)) ||
        size > size_ - address) {
        require_initialized();
        throw Error("invalid resolve address", ErrorKind::Unsupported);
    }
    return resolve_static(address, size).target;
}

// Finalize the registered layout and validate resolver bindings.
void RegisterMap::finalize() {
    require_initialized();
    validate_resolver_bindings();
    if (finalized_)
        return;
    aliases_.clear();
    for (auto& n : nodes_)
        if (!n->instance && !n->layout().elements.empty())
            n->instance = source_instance(n->layout().elements[0].address);
    auto cache = [&](std::uint64_t a) {
        auto alias = walk_alias(a, 4);
        if (alias.defined && lookup_.get(alias.target) && !aliases_.get(a))
            aliases_.insert(a, alias);
    };
    for (const auto& w : windows_) {
        if (w.mapped_offsets) {
            for (const auto& r : *w.mapped_offsets)
                for (std::uint64_t byte = 0; byte < r.second; byte += 4)
                    cache(w.source_base + r.first + byte);
        } else if (!w.terminal)
            for (std::uint64_t a = w.source_base; a < w.source_base + w.size; a += 4)
                cache(a);
    }
    validate_integrity();
    // These properties cannot change after finish. Dynamic callbacks,
    // hooks, logger state and borrow state are still checked per beat.
    for (auto& n : nodes_) {
        if (n->scalar_data && n->value_size == 4 && !n->custom_behavior &&
            n->builtin == Access::Rw && n->widths.allows(4))
            n->scalar32_directions =
                (n->transaction_policy.read == SplitReadMode::Independent ? 1 : 0) |
                (n->transaction_policy.write == SplitWriteMode::Independent ? 2 : 0);
    }
    if (formula_)
        formula_->prepare_fast_paths();
    lookup_.freeze();
    aliases_.freeze();
    finalized_ = true;
    if (log_enabled(HLOG_LEVEL_WARN))
        for (const auto& warning : layout_warnings()) {
            const auto text = warning.code + ": " + warning.subject + ": " + warning.message;
            detail::LogRecord record{
                HLOG_LEVEL_WARN, "layout-warning", warning.subject, nullptr, 0, {}, {}};
            record.message = text;
            record.location = warning.location;
            emit_log(record);
        }
    emit_layout();
}

void RegisterMap::validate_integrity() const {
    require_initialized();
    for (auto& n : nodes_) {
        std::size_t count = 1;
        for (auto d : n->dims) {
            if (!d || count > UINT32_MAX / d)
                throw Error("invalid node dimensions");
            count *= d;
        }
        if (count != n->element_count() || count != n->layout().elements.size() ||
            n->transaction_state.size() != (n->transaction_policy.independent() ? 0 : count))
            throw Error("invalid node storage length");
    }
    lookup_.for_each([&](auto a, const Entry& e) {
        if (e.linear >= e.node->element_count())
            throw Error("invalid lookup index");
        if (e.node->value_size == 8) {
            auto pair = lookup_.get(e.byte_offset ? a - 4 : a + 4);
            if (!pair || pair->node != e.node || pair->linear != e.linear ||
                pair->byte_offset != (e.byte_offset ? 0 : 4))
                throw Error("invalid 64-bit pair");
        }
    });
    (void)snapshot_schema();
    for (const auto& w : windows_) {
        if (w.mapped_offsets) {
            for (const auto& r : *w.mapped_offsets)
                for (std::uint64_t byte = 0; byte < r.second; byte += 4)
                    (void)walk_alias(w.source_base + r.first + byte, 4);
        } else if (!w.terminal)
            for (std::uint64_t a = w.source_base; a < w.source_base + w.size; a += 4)
                (void)walk_alias(a, 4);
    }
}

} // namespace reg
