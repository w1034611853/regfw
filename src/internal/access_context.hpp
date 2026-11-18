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
    // Decoder-owned immutable geometry; no resolver result is cached.
    const void* prepared_view{};
    const void* prepared_access{};
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
    const detail::ScalarAccessSite* scalar_site{};
    const RegElementIndex* scalar_index{};
    std::uint64_t scalar_target{};
    std::uint32_t linear{};
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
    // The fixed device contract permits one non-reentrant access at a time.
    Resolution resolution{};
    // Formula decoding scratch: [0] holds the selected view entry, [1] the
    // identity RedirectBank target. Both live only within one dispatch.
    RegisterMap::AliasEntry decoded[2]{};
    RegisterMap::CompiledAccess decoded_access{};
    detail::ScalarAccessSite direct_site{};
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
} // namespace reg
