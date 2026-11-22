#include "internal/runtime_impl.hpp"
#include <sstream>
#include <iomanip>
#include <unordered_set>

namespace reg {
// RegisterMap lifetime and initialization.
RegisterMap::RegisterMap() noexcept = default;

RegisterMap::RegisterMap(std::uint64_t size, std::string name, HLogLogger* logger) : RegisterMap() {
    init(size, std::move(name), logger);
}

void RegisterMap::init(std::uint64_t size, std::string name, HLogLogger* logger) {
    if (size_)
        throw Error("register map has already been initialized", ErrorKind::Configuration);
    const auto checked_size = checked_map_size(size);
    auto next_life = std::make_shared<char>();
    auto next_resolvers = std::make_unique<ResolverRegistry>();
    auto next_scratch = std::make_unique<AccessScratch>();
    // Complete every allocation before publishing any initialized state.
    life_ = std::move(next_life);
    access_resolvers_ = std::move(next_resolvers);
    access_scratch_ = std::move(next_scratch);
    logger_ = logger;
    name_prefix = std::move(name);
    size_ = checked_size;
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
void* checked_value_pointer(
    EntityState* node, std::size_t index, const void* token, std::size_t rank, bool empty) {
    if (!node)
        throw Error("register handle is not bound");

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
    primary->indices.reserve(count);
    primary->mapped_addresses.reserve(count);
    primary->names.reserve(count);
    primary->local_addresses.reserve(count);
    std::vector<std::pair<std::uint64_t, std::size_t>> occupied_ranges;
    if (count > 1)
        occupied_ranges.reserve(count);
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
        if (count > 1)
            occupied_ranges.emplace_back(a, k);
        primary->local_addresses.push_back(a);
        primary->indices.push_back(idx);
        primary->mapped_addresses.push_back(a);
        primary->names.push_back(std::move(full));
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
            throw Error("register array elements overlap:\n  " +
                            detail::declaration(
                                primary->names[bi], before, occupied, n->declaration_location) +
                            "\n  " +
                            detail::declaration(
                                primary->names[ai], after, occupied, n->declaration_location),
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
    return ptr;
}

void RegisterMap::clear_identity_resolver() {
    mutable_bindings();
    identity_resolver_.reset();
    emit_resolver_update("identity", identity_description_, false);
}

bool RegisterMap::has_identity_resolver() const {
    return bool(identity_resolver_);
}

void RegisterMap::clear_pre_access() {
    mutable_bindings();
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
    install_default_layout();
    validate_integrity();
    // Layout and callback bindings are fixed after finish. Logger state
    // and the data consulted by callbacks remain live during each access.
    for (auto& n : nodes_) {
        if (n->scalar_data && n->value_size == 4 && !n->custom_behavior &&
            n->builtin == Access::Rw && n->widths.allows(4))
            n->scalar32_directions =
                (n->transaction_policy.read == SplitReadMode::Independent ? 1 : 0) |
                (n->transaction_policy.write == SplitWriteMode::Independent ? 2 : 0);
    }
    formula_->prepare_fast_paths();
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
        if (count != n->element_count() || count != n->layout().indices.size() ||
            count != n->layout().mapped_addresses.size() || count != n->layout().names.size() ||
            n->transaction_state.size() != (n->transaction_policy.independent() ? 0 : count))
            throw Error("invalid node storage length");
    }
    (void)snapshot_schema();
}

} // namespace reg
