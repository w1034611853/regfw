#pragma once
#include "entity_state.hpp"

namespace reg {
struct RegisterMap::Entry {
    Node* node{};
    std::uint32_t linear{};
    unsigned byte_offset{};
    const detail::RegViewLayout* view{};
};
struct RegisterMap::AliasEntry : detail::ScalarAccessSite {
    std::uint64_t available{};
    Entry target_entry;
    std::optional<std::size_t> identity_index;
    std::int32_t priority{};
    const detail::ViewPreAccessChain* pre_access_chain{};
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
    // The fixed device contract permits one non-reentrant access at a time.
    Resolution resolution{};
    // Formula decoding scratch: [0] holds the selected view entry, [1] the
    // identity RedirectBank target. Both live only within one dispatch.
    RegisterMap::AliasEntry decoded[2]{};
    AccessScratch() {
        resolution.info.index_fields.borrow(index);
        resolution.info.mapped_fields.borrow(mapped);
    }
};
struct RegisterMap::ResolverRegistry {
    std::unordered_map<const void*, std::shared_ptr<detail::AccessResolverSlot>> slots;
    std::unordered_map<const void*, std::shared_ptr<detail::IdentityResolverSlot>> identities;
    std::unordered_map<const void*, std::shared_ptr<detail::PreAccessSlot>> pre_access;
};
} // namespace reg
