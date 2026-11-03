#pragma once
// Private implementation: never included by device headers or amalgamation.
#include <regfw/regfw.hpp>
#include <regfw/detail/formula_plan.hpp>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <cstdio>
#include <cstdlib>
namespace reg::detail {
inline void copy_index(RegElementIndex& to, const RegElementIndex& from) noexcept {
    static_assert(std::is_trivially_copyable_v<RegElementIndex>);
    std::memcpy(to.values.data(), from.values.data(), sizeof(to.values));
    to.linear_value = from.linear_value;
    to.rank_value = from.rank_value;
}
// Private diagnostic records use hlog's native severity values.
struct FailedAccess {
    RegTransactionKind kind;
    std::uint64_t address;
    RegData value;
    unsigned size;
};
struct LogRecord {
    HLogLevel level;
    std::string_view phase, name;
    const RegAccessInfo* info{};
    RegData value{};
    std::string_view transaction_action, error;
    const AccessIdentity* identity{};
    std::string_view message{};
    std::source_location location = std::source_location::current();
    const FailedAccess* request{};
};
struct IndexRuleDescription {
    std::string source, output, fallback;
    unsigned dimension{};
    std::optional<std::uint32_t> condition_domain;
    bool conditional{}, metadata_only{};
    CallbackDescription condition_description, resolver_description;
    const void* resolver_key{};
    std::string resolver_name;
};
struct ViewPreAccessChain {
    struct Step {
        std::shared_ptr<PreAccessSlot> slot;
        std::string_view view;
    };
    std::vector<Step> steps;
};
struct LayoutScope {
    std::uint64_t base{}, size{};
    std::string name;
    ResetScope reset;
    std::optional<std::uint32_t> instance;
    std::size_t parent = ~std::size_t{};
    std::uint64_t repeat_count{1}, repeat_stride{}, repeat_index{};
    bool recursive_window{};
    std::string purpose;
};
struct WideNodeData {
    WideRegData wide_writable_mask{~WideRegData{}}, wide_reset_value{};
    WideFieldBehavior wide_behavior;
    using WideReadFn = std::function<WideRegData(WideRegData, const RegAccessInfo&)>;
    using WideWriteFn = EventCallback;
    std::shared_ptr<const WideWriteFn> wide_read_complete_callback;
    std::shared_ptr<const WideReadFn> wide_read_callback;
    std::shared_ptr<const WideWriteFn> wide_write_callback;
};
struct RegViewLayout {
    EntityState* entity{};
    std::size_t layout_scope = ~std::size_t{};
    std::vector<std::uint64_t> strides;
    struct Element {
        std::uint64_t address;
        RegElementIndex index;
        std::string name;
    };
    std::vector<Element> elements;
    std::vector<std::uint64_t> local_addresses;
    std::vector<bool> element_mapped;
    std::string display_name;
    bool mapped{};
};
struct SingleIndexPlan : MappedIndexSite {
    std::uint32_t extent{}, coefficient{};
    AccessResolverSlot* resolver{};
    bool metadata_only{};
};
struct EntityState {
    using ReadFn = ScalarReadSource;
    using WriteFn = EventCallback;
    using DeriveFn = std::function<void(RegAccessInfo&)>;
    // Keep mutable MMIO state together. Diagnostic and snapshot metadata
    // below are not needed by the native scalar execution plan.
    std::shared_ptr<const ReadFn> read_callback;
    std::shared_ptr<const WriteFn> write_callback;
    std::shared_ptr<const WriteFn> read_complete_callback;
    std::byte* scalar_data{};
    std::size_t scalar_stride{};
    RegData writable_mask{~RegData{}};
    bool borrowed{};
    std::uint8_t scalar32_directions{};
    RegisterMap* owner{};
    RegEntity* entity{};
    std::size_t id{};
    std::string name;
    std::source_location declaration_location;
    RegViewLayout& layout();
    const RegViewLayout& layout() const;
    std::unique_ptr<detail::ValueStorage> values;
    bool direct_wide_storage{};
    std::unique_ptr<WideNodeData> wide;
    reg::Access builtin{reg::Access::Rw};
    bool custom_behavior{};
    bool declared_array{};
    bool is_wide() const {
        return value_size > 8;
    }
    WideRegData wide_mask() const {
        return WideRegData::mask(value_size * 8);
    }
    std::size_t element_count() const {
        return values ? values->size() : 0;
    }
    std::size_t storage_bytes() const {
        return values ? values->object_bytes() : 0;
    }
    RegData read_scalar(std::size_t i) const {
        if (!scalar_data)
            return values->read_scalar(i);
        auto* p = scalar_data + i * scalar_stride;
        switch (value_size) {
            case 1: {
                std::uint8_t v;
                std::memcpy(&v, p, 1);
                return v;
            }
            case 2: {
                std::uint16_t v;
                std::memcpy(&v, p, 2);
                return v;
            }
            case 4: {
                std::uint32_t v;
                std::memcpy(&v, p, 4);
                return v;
            }
            default: {
                RegData v;
                std::memcpy(&v, p, 8);
                return v;
            }
        }
    }
    void write_scalar(std::size_t i, RegData v) {
        if (!scalar_data) {
            values->write_scalar(i, v);
            return;
        }
        auto* p = scalar_data + i * scalar_stride;
        switch (value_size) {
            case 1: {
                auto x = std::uint8_t(v);
                std::memcpy(p, &x, 1);
                break;
            }
            case 2: {
                auto x = std::uint16_t(v);
                std::memcpy(p, &x, 2);
                break;
            }
            case 4: {
                auto x = std::uint32_t(v);
                std::memcpy(p, &x, 4);
                break;
            }
            default:
                std::memcpy(p, &v, 8);
                break;
        }
    }
    WideRegData read_wide(std::size_t i) const {
        if (direct_wide_storage)
            return *values->wide_reference(i);
        return values->read_wide(i);
    }
    void write_wide(std::size_t i, WideRegData v) {
        if (direct_wide_storage)
            *values->wide_reference(i) = v;
        else
            values->write_wide(i, v);
    }
    void* value_pointer(std::size_t index, const void* token) const;
    template <class T>
    T* value_pointer(std::size_t index) const {
        return static_cast<T*>(value_pointer(index, &detail::value_type_token<T>));
    }
    std::size_t snapshot_words() const {
        return element_count() * (is_wide() ? (value_size + 7) / 8 : 1);
    }
    std::unique_ptr<SingleIndexPlan> single_index;
    std::vector<std::size_t> dims;
    RegData reset_value{};
    unsigned value_size{4};
    FieldBehavior behavior;
    struct Derivation {
        std::string name;
        DeriveFn callback;
        std::shared_ptr<const std::vector<IndexRuleDescription>> descriptions;
    };
    std::vector<Derivation> derivations;
    ResetDomains reset_domains{ResetDomains::STANDARD};
    ResetScope reset_scope;
    RegisterAccessWidthPolicy widths;
    TransactionPolicy transaction_policy;
    std::vector<detail::RegTransactionState> transaction_state;
    SnapshotMeta snapshot;
    LogPolicy log_policy{LogPolicy::Normal};
    std::optional<std::uint32_t> instance;
    RegData value_mask() const {
        return detail::low_mask(std::min(value_size * 8, 64u));
    }
    void check_borrow() const {
        if (borrowed)
            throw Error("reentrant register access from a behavior: " + name);
    }
    RegData apply_write(RegData current, RegData value, RegData mask) const {
        return custom_behavior ? behavior.write(current, value, mask)
                               : detail::apply_write(builtin, current, value, mask);
    }
    RegData apply_read(RegData& stored, RegData mask) const {
        return custom_behavior ? behavior.read(stored, mask)
                               : detail::apply_read(builtin, stored, mask);
    }
    WideRegData apply_wide_write(const WideRegData& current,
                                 const WideRegData& value,
                                 const WideRegData& mask) const {
        if (custom_behavior)
            return wide->wide_behavior.write(current, value, mask);
        // Fixed-size bitwise operations let the compiler vectorize the limbs.
        switch (builtin) {
            case reg::Access::Rw:
            case reg::Access::WriteOnly:
            case reg::Access::Rwsc:
                return (current & ~mask) | (value & mask);
            case reg::Access::W1c:
                return current & ~(value & mask);
            case reg::Access::W1s:
                return current | (value & mask);
            case reg::Access::W0c:
                return current & (value | ~mask);
            case reg::Access::W0s:
                return current | (~value & mask);
            case reg::Access::RwscImmediate:
                return current & ~mask;
            default:
                return current;
        }
    }
    WideRegData apply_wide_read(WideRegData& stored, const WideRegData& mask) const {
        if (custom_behavior)
            return wide->wide_behavior.read(stored, mask);
        if (builtin == reg::Access::WriteOnly)
            return {};
        auto result = stored & mask;
        if (builtin == reg::Access::Rc)
            stored &= ~mask;
        return result;
    }
    void reset() {
        check_borrow();
        for (std::size_t i = 0; i < element_count(); ++i) {
            if (is_wide())
                write_wide(i, wide->wide_reset_value & wide_mask());
            else
                write_scalar(i, reset_value & value_mask());
        }
        std::fill(
            transaction_state.begin(), transaction_state.end(), detail::RegTransactionState{});
    }
};
RegisterMetadata node_metadata(const EntityState& node);
struct NodeBorrow {
    EntityState& node;
    explicit NodeBorrow(EntityState& n) : node(n) {
        n.check_borrow();
        n.borrowed = true;
    }
    ~NodeBorrow() {
        node.borrowed = false;
    }
    NodeBorrow(const NodeBorrow&) = delete;
};
template <class E>
struct SparseLookup<E>::Page {
    std::array<std::uint32_t, 1024> ids;
    Page() {
        ids.fill(empty);
    }
};
template <class E>
struct SparseLookup<E>::SparsePages {
    std::unordered_map<std::uint64_t, std::unique_ptr<Page>> pages;
    struct Slot {
        std::uint64_t key = UINT64_MAX;
        const Page* page{};
    };
    std::vector<Slot> frozen;
    static std::size_t hash(std::uint64_t key, std::size_t mask) {
        return ((key ^ (key >> 17)) * 0x9e3779b97f4a7c15ULL) & mask;
    }
};
template <class E>
SparseLookup<E>::SparseLookup(std::uint64_t bytes) : bytes_(bytes) {
    if (bytes > (std::uint64_t{1} << 32))
        sparse_ = std::make_unique<SparsePages>();
    else
        pages_.resize(bytes / 4096 + (bytes % 4096 != 0));
}
template <class E>
SparseLookup<E>::~SparseLookup() = default;
template <class E>
const E* SparseLookup<E>::get(std::uint64_t address) const {
    if (address >= bytes_)
        return nullptr;
    const auto p = address >> 12;
    const Page* page;
    if (sparse_) {
        if (!sparse_->frozen.empty()) {
            const auto mask = sparse_->frozen.size() - 1;
            auto slot = SparsePages::hash(p, mask);
            for (;;) {
                const auto& hit = sparse_->frozen[slot];
                if (hit.key == p) {
                    page = hit.page;
                    break;
                }
                if (hit.key == UINT64_MAX)
                    return nullptr;
                slot = (slot + 1) & mask;
            }
        } else {
            const auto it = sparse_->pages.find(p);
            if (it == sparse_->pages.end())
                return nullptr;
            page = it->second.get();
        }
    } else {
        page = pages_[p].get();
        if (!page)
            return nullptr;
    }
    const auto id = page->ids[(address & 4095) >> 2];
    return id == empty ? nullptr : &entries_[id];
}
template <class E>
void SparseLookup<E>::insert(std::uint64_t address, E value) {
    if (address >= bytes_ || get(address))
        throw Error("lookup address collision/outside map", ErrorKind::Configuration);
    if (entries_.size() >= empty)
        throw Error("lookup entry overflow", ErrorKind::Configuration);
    if (sparse_)
        sparse_->frozen.clear();
    auto& page = sparse_ ? sparse_->pages[address >> 12] : pages_[address >> 12];
    if (!page)
        page = std::make_unique<Page>();
    const auto id = static_cast<std::uint32_t>(entries_.size());
    entries_.push_back(std::move(value));
    page->ids[(address & 4095) >> 2] = id;
}
template <class E>
void SparseLookup<E>::freeze() {
    if (!sparse_ || sparse_->pages.empty())
        return;
    if (sparse_->pages.size() > std::numeric_limits<std::size_t>::max() / 2)
        throw Error("page directory overflow", ErrorKind::Configuration);
    std::size_t capacity = 2;
    while (capacity / 2 < sparse_->pages.size()) {
        if (capacity > std::numeric_limits<std::size_t>::max() / 2)
            throw Error("page directory overflow", ErrorKind::Configuration);
        capacity *= 2;
    }
    std::vector<typename SparsePages::Slot> next(capacity);
    for (const auto& [key, page] : sparse_->pages) {
        auto slot = SparsePages::hash(key, capacity - 1);
        while (next[slot].key != UINT64_MAX)
            slot = (slot + 1) & (capacity - 1);
        next[slot] = {key, page.get()};
    }
    sparse_->frozen.swap(next);
}
template <class E>
void SparseLookup<E>::clear() {
    if (sparse_) {
        sparse_->frozen.clear();
        sparse_->pages.clear();
    } else
        for (auto& p : pages_)
            p.reset();
    entries_.clear();
}
template <class E>
void SparseLookup<E>::swap(SparseLookup& other) noexcept {
    std::swap(bytes_, other.bytes_);
    sparse_.swap(other.sparse_);
    pages_.swap(other.pages_);
    entries_.swap(other.entries_);
}
template <class E>
template <class F>
void SparseLookup<E>::for_each_page(F f) const {
    if (sparse_) {
        std::vector<std::uint64_t> keys;
        keys.reserve(sparse_->pages.size());
        for (const auto& [key, page] : sparse_->pages) {
            (void)page;
            keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());
        for (auto p : keys)
            f(p, *sparse_->pages.at(p));
    } else
        for (std::size_t p = 0; p < pages_.size(); ++p)
            if (pages_[p])
                f(std::uint64_t(p), *pages_[p]);
}
template <class E>
template <class F>
void SparseLookup<E>::for_each(F f) const {
    for_each_page([&](std::uint64_t p, const Page& page) {
        for (std::size_t slot = 0; slot < 1024; ++slot) {
            const auto id = page.ids[slot];
            if (id != empty)
                f((p << 12) + (slot << 2), entries_[id]);
        }
    });
}
template <class E>
template <class F>
void SparseLookup<E>::for_each_mut(F f) {
    for_each_page([&](std::uint64_t p, const Page& page) {
        for (std::size_t slot = 0; slot < 1024; ++slot) {
            const auto id = page.ids[slot];
            if (id != empty)
                f((p << 12) + (slot << 2), entries_[id]);
        }
    });
}
struct AddressWindow {
    std::uint64_t source_base{}, target_base{}, size{};
    AccessRoute route{AccessRoute::ALIAS};
    std::string name;
    AliasAccessPolicy policy;
    bool terminal{};
    std::shared_ptr<const std::vector<std::pair<std::uint64_t, std::uint64_t>>> mapped_offsets;
    std::source_location location{};
};
struct ReservedRegion {
    std::uint64_t base{}, size{};
    std::string name;
    std::uint64_t repeat_count{1}, repeat_stride{}, repeat_index{};
    std::source_location location{};
};
struct IdentityResolverSlot {
    using Function = std::function<std::uint32_t(const IdentityResolveRequest&)>;
    std::string name;
    std::shared_ptr<const Function> callback;
    CallbackDescription description;
    bool deferred{}, required{};
};
struct IdentityWindow {
    std::uint64_t base{}, count{}, stride{}, aperture_size{}, region_offset{}, region_size{};
    std::uint32_t domain{};
    IdentityRemapMode mode{};
    IdentityTraceLabels trace_labels;
    std::string name;
    std::shared_ptr<IdentityResolverSlot> resolver;
    std::optional<std::pair<std::uint32_t, std::uint64_t>>
    match(std::uint64_t address, unsigned size, std::optional<std::uint32_t> hint = {}) const {
        auto test =
            [&](std::uint64_t id) -> std::optional<std::pair<std::uint32_t, std::uint64_t>> {
            if (id >= count)
                return {};
            auto start = base + id * stride;
            if (address < start)
                return {};
            auto inner = address - start;
            if (inner < region_offset || inner > region_offset + region_size ||
                size > region_offset + region_size - inner)
                return {};
            return {{static_cast<std::uint32_t>(id), inner}};
        };
        if (hint)
            if (auto m = test(*hint))
                return m;
        if (address < base)
            return {};
        return test((address - base) / stride);
    }
    std::source_location location{};
};
} // namespace reg::detail
