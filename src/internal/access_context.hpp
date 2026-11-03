#pragma once
#include "entity_state.hpp"

namespace reg {
struct RegisterMap::Entry {
    Node* node;
    std::uint32_t linear;
    unsigned byte_offset;
    std::optional<std::uint32_t> source_instance;
    const detail::RegViewLayout* view{};
    // Optional immutable index from the shared EntityLayout hash.
    // It has a rank and coordinate array (up to MAX_REG_DIMS).
    const RegElementIndex* precomputed_index{};
};
struct RegisterMap::AliasHop {
    std::uint64_t from, to;
    const detail::AddressWindow* window;
};
struct RegisterMap::AliasEntry : detail::ScalarAccessSite {
    using Execute = Resolution& (
        RegisterMap::*)(std::uint64_t, unsigned, RegTransactionKind, RegData, const AliasEntry*);
    Execute execute{};
    const AliasEntry* const* redirect_targets{};
    std::uint64_t identity_inner{};
    std::uint32_t identity_request{};
    std::uint64_t available{};
    std::string_view name;
    bool defined{true};
    bool compiled{};
    std::uint8_t scalar32_directions{};
    std::optional<Entry> target_entry;
    std::optional<std::size_t> identity_index;
    std::int32_t priority{};
    const detail::ViewPreAccessChain* pre_access_chain{};
};
struct RegisterMap::InstanceWindow {
    std::uint64_t base, size;
    std::uint32_t instance;
};
struct RegisterMap::CompiledAccess {
    const AliasEntry* metadata{};
    Node* node{};
    std::byte* scalar{};
    std::uint32_t mask{};
    std::uint8_t directions{}, mapped_directions{};
    enum class Path : std::uint8_t { Generic, Scalar, Mapped };
    Path read_path{}, write_path{};
};
struct RegisterMap::Resolution {
    Entry entry;
    struct Route {
        std::uint64_t target{};
        AccessRoute route;
        AliasAccessPolicy policy;
        const detail::ViewPreAccessChain* pre_access_chain{};
    } alias;
    std::optional<AccessIdentity> identity;
    RegAccessInfo info;
};
struct RegisterMap::AccessScratch {
    RegAccessFieldSet::FieldArray::Array index, mapped;
    // One synchronous access at a time: DispatchGuard rejects same-map
    // reentry before either reusable context can be touched.
    Resolution resolution{};
    // Formula decoding scratch: [0] holds the selected view entry, [1] the
    // identity RedirectBank target. Both live only within one dispatch.
    RegisterMap::AliasEntry decoded[2]{};
    RegisterMap::CompiledAccess decoded_access{};
    AccessScratch() {
        resolution.info.index_fields.borrow(index);
        resolution.info.mapped_fields.borrow(mapped);
    }
};
struct RegisterMap::AddressResolution {
    AliasEntry alias;
    std::optional<AccessIdentity> identity;
    std::uint64_t alias_address;
};

namespace detail {}
struct RegisterMap::ResolverRegistry {
    std::unordered_map<const void*, std::shared_ptr<detail::AccessResolverSlot>> slots;
    std::unordered_map<const void*, std::shared_ptr<detail::IdentityResolverSlot>> identities;
    std::unordered_map<const void*, std::shared_ptr<detail::PreAccessSlot>> pre_access;
};
struct RegisterMap::PreAccessGuard {
    RegisterMap& map;
    explicit PreAccessGuard(RegisterMap& m) : map(m) {
        map.pre_access_active_ = true;
    }
    ~PreAccessGuard() {
        if (!map.retired_pre_access_.empty())
            map.release_retired_pre_access();
        map.pre_access_active_ = false;
    }
};
struct RegisterMap::DispatchGuard {
    RegisterMap& map;
    explicit DispatchGuard(RegisterMap& m) : map(m) {
        if (m.dispatching_)
            throw Error("recursive MMIO on same map is unsupported");
        m.dispatching_ = true;
    }
    ~DispatchGuard() {
        map.dispatching_ = false;
    }
};
} // namespace reg
