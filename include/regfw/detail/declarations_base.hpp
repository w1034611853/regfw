#pragma once
// Declaration machinery without the compiled layout compiler. Included by
// declarations.hpp (full compatibility entry) and bundle.hpp (light public
// entry); do not include this directly.
#include "../address_view.hpp"
#include <initializer_list>

// Flipped to 1 by static_layout.hpp. REG_LAYOUT and REG_LAYOUT_IMPL need the
// compiled-layout machinery in the same translation unit; with only the light
// <regfw/bundle.hpp> entry they fail this assertion at compile time instead of
// deferring the failure to link time (which some compilers accept silently
// under -fsyntax-only).
#ifndef REGFW_COMPILED_LAYOUT_ENTRY
#define REGFW_COMPILED_LAYOUT_ENTRY 0
#endif

namespace reg {
// The list elements hide node pointers; declarations expose register handles only.
class AccessMember {
    friend class Groups;
    detail::EntityState* node_;
    std::weak_ptr<void> lifetime_;

  public:
    template <class T, std::size_t R, bool V>
    AccessMember(const RegView<T, R, V>& value)
        : node_(detail::Access::node(value)), lifetime_(detail::Access::lifetime(value)) {
        if (value.size() != value.entity().element_count())
            throw Error("access group requires a whole-entity RegView", ErrorKind::Configuration);
    }
};

class AccessCoordinate {
    friend class Groups;
    const void* id_;
    std::string_view name_;
    unsigned dimension_;
    RegData (*convert_)(std::uint32_t);
    bool identity_{};

  public:
    template <class Tag, class V>
    AccessCoordinate(FieldKey<Tag, V> key, unsigned dimension)
        : id_(key.id()), name_(key.name), dimension_(dimension), convert_([](std::uint32_t v) {
              return detail::to_bits(detail::value_from_data<V>(v));
          }),
          identity_(std::same_as<V, std::uint32_t>) {
    }
    template <class Tag, class V, unsigned I>
    AccessCoordinate(FieldKey<Tag, V> key, IndexDimension<I>) : AccessCoordinate(key, I) {
        static_assert(I < MAX_REG_DIMS);
    }
};

template <class Source, class Output>
class IndexMappingBuilder;
class IndexMapping {
    friend class Groups;
    template <class Source, class Output>
    friend class IndexMappingBuilder;
    const void* source_{};
    const void* output_{};
    std::string_view output_name_;
    std::function<bool(const RegAccessInfo&)> condition_;
    std::function<RegData(const RegAccessInfo&)> resolver_;
    const void* resolver_key_{};
    std::string_view resolver_name_;
    const void* fallback_{};
    bool metadata_only_{};
    std::optional<std::uint32_t> condition_domain_;
    detail::CallbackDescription condition_description_, resolver_description_;
};

// Only the named field pair and actual callback participate in templates.
// Register types, dimensions, and member counts do not specialize this builder.
template <class Source, class Output>
class IndexMappingBuilder {
    IndexMapping rule_;
    Source source_;

  public:
    IndexMappingBuilder(Source source, Output output) : source_(source) {
        rule_.source_ = source.id();
        rule_.output_ = output.id();
        rule_.output_name_ = output.name;
    }
    template <class C>
    IndexMappingBuilder& when(C condition,
                              std::source_location at = std::source_location::current()) {
        rule_.condition_domain_.reset();
        if constexpr (std::same_as<C, IdentityDomainCondition>)
            rule_.condition_domain_ = condition.domain;
        rule_.condition_description_ = detail::callback_description<C>({}, at);
        rule_.condition_ = std::move(condition);
        return *this;
    }
    template <class Tag, class V>
    IndexMappingBuilder& using_resolver(FieldKey<Tag, V> key) {
        static_assert(std::same_as<V, typename Output::Value>,
                      "resolver field value type must match mapping output");
        rule_.resolver_ = {};
        rule_.resolver_description_ = {};
        rule_.resolver_key_ = key.id();
        rule_.resolver_name_ = key.name;
        return *this;
    }
    template <class F>
    IndexMappingBuilder& using_resolver(F f,
                                        std::source_location at = std::source_location::current()) {
        return using_resolver(std::string_view{}, std::move(f), at);
    }
    template <class F>
    IndexMappingBuilder& using_resolver(std::string_view name,
                                        F f,
                                        std::source_location at = std::source_location::current()) {
        rule_.resolver_key_ = nullptr;
        rule_.resolver_name_ = {};
        rule_.resolver_description_ = detail::callback_description<F>(name, at);
        rule_.resolver_ = [f = std::move(f), source = source_](const RegAccessInfo& info) mutable {
            const auto value = info.index_as(source);
            auto call = [&] {
                if constexpr (std::is_invocable_v<F&, const RegAccessInfo&>)
                    return std::invoke(f, info);
                else if constexpr (std::is_invocable_v<F&, std::uint32_t, decltype(value)>)
                    return std::invoke(f, info.resolved_id(), value);
                else
                    return std::invoke(f, value);
            };
            return detail::checked_access_result<typename Output::Value>(call());
        };
        return *this;
    }
    template <class K>
    IndexMappingBuilder& otherwise(Passthrough<K> p) {
        rule_.fallback_ = p.field.id();
        return *this;
    }
    IndexMappingBuilder& metadata_only() {
        rule_.metadata_only_ = true;
        return *this;
    }
    operator IndexMapping() const {
        return rule_;
    }
};
template <class S, class O>
auto map_index(S source, O output) {
    return IndexMappingBuilder<S, O>(source, output);
}

// Initializer lists are consumed synchronously and are never retained.
struct AccessGroupSpec {
    std::initializer_list<AccessMember> members{};
    std::initializer_list<AccessCoordinate> coordinates{};
    std::initializer_list<IndexMapping> derive{};
};
namespace detail {
struct DeclaredAccessGroup {};
DeclaredAccessGroup declare_access_group(std::string_view name, const AccessGroupSpec& spec);
template <class B, std::size_t N>
PhysicalRepeatAddressWindow declaration_window(const RepeatBundle<B, N>& bundles) {
    const auto first = bundles[0].region();
    auto stride = first.size();
    if constexpr (N > 1) {
        const auto next = bundles[1].region();
        if (next.base() <= first.base())
            throw Error("layout repeat is not ascending", ErrorKind::Configuration);
        stride = next.base() - first.base();
    }
    if (!stride || (N - 1) > (UINT64_MAX - first.base()) / stride)
        throw Error("layout repeat span overflow", ErrorKind::Configuration);
    for (std::size_t i = 1; i < N; ++i) {
        const auto region = bundles[i].region();
        if (region.base() != first.base() + i * stride || region.size() != first.size())
            throw Error("layout repeat has inconsistent windows", ErrorKind::Configuration);
    }
    return {first.base(), N, stride, first.size()};
}
} // namespace detail

struct RegViewSpec {
    PhysicalRepeatAddressWindow source{};
    std::uint64_t offset{}, size{};
    PhysicalRepeatAddressWindow target{};
    bool has_target{};
    AliasAccessPolicy policy = AliasAccessPolicy::READ_WRITE;
    AccessRouteKind route_kind = AccessRouteKind::Alias;
    std::string label;
    std::source_location location{};
    const RegisterMap* source_owner{};
    const RegisterMap* target_owner{};
    std::weak_ptr<void> source_lifetime, target_lifetime;
    auto at(std::uint64_t value) const {
        auto out = *this;
        out.offset = value;
        return out;
    }
    auto size_bytes(std::uint64_t value) const {
        auto out = *this;
        out.size = value;
        return out;
    }
    auto range(std::uint64_t start, std::uint64_t length) const {
        auto out = *this;
        out.offset = start;
        out.size = length;
        return out;
    }
    auto to(PhysicalRepeatAddressWindow value) const {
        auto out = *this;
        out.target = value;
        out.has_target = true;
        out.target_owner = nullptr;
        out.target_lifetime.reset();
        return out;
    }
    auto to(PhysicalAddressWindow value) const {
        return to(PhysicalRepeatAddressWindow{value.base, 1, value.size, value.size});
    }
    auto to(const RegRegion& value) const {
        return to(PhysicalAddressWindow{value.base(), value.size()});
    }
    template <class B>
    auto to(const B& value) const
        requires(std::derived_from<B, Bundle>)
    {
        auto out = to(value.region());
        out.target_owner = detail::Access::owner(value);
        out.target_lifetime = detail::Access::lifetime(value);
        return out;
    }
    template <class B, std::size_t N>
    auto to(const RepeatBundle<B, N>& value) const {
        auto out = to(detail::declaration_window(value));
        out.target_owner = detail::Access::owner(value[0]);
        out.target_lifetime = detail::Access::lifetime(value[0]);
        return out;
    }
    template <class T, std::size_t R, bool V>
    auto to(const RegView<T, R, V>& value) const {
        auto out = to(PhysicalAddressWindow{value.address(), value.address_span_bytes()});
        out.target_owner = detail::Access::owner(value);
        out.target_lifetime = detail::Access::lifetime(value);
        return out;
    }
    auto read_only_ignore_writes() const {
        auto out = *this;
        out.policy = AliasAccessPolicy::READ_ONLY_IGNORE_WRITES;
        return out;
    }
    auto read_only_error_writes() const {
        auto out = *this;
        out.policy = AliasAccessPolicy::READ_ONLY_ERROR_WRITES;
        return out;
    }
    auto vf_to_pf() const {
        auto out = *this;
        out.route_kind = AccessRouteKind::VfToPf;
        return out;
    }
    auto pf_to_vf() const {
        auto out = *this;
        out.route_kind = AccessRouteKind::PfToVf;
        return out;
    }
    auto name(std::string value) const {
        auto out = *this;
        out.label = std::move(value);
        return out;
    }
};
using AliasSpec = RegViewSpec;
inline AliasSpec alias(PhysicalRepeatAddressWindow value,
                       std::source_location at = std::source_location::current()) {
    AliasSpec out;
    out.source = value;
    out.size = value.size;
    out.location = at;
    return out;
}
inline AliasSpec alias(PhysicalAddressWindow value,
                       std::source_location at = std::source_location::current()) {
    return alias(PhysicalRepeatAddressWindow{value.base, 1, value.size, value.size}, at);
}
inline AliasSpec alias(const RegRegion& value,
                       std::source_location at = std::source_location::current()) {
    return alias(PhysicalAddressWindow{value.base(), value.size()}, at);
}
template <class B>
AliasSpec alias(const B& value, std::source_location at = std::source_location::current())
    requires(std::derived_from<B, Bundle>)
{
    auto out = alias(value.region(), at);
    out.source_owner = detail::Access::owner(value);
    out.source_lifetime = detail::Access::lifetime(value);
    return out;
}
template <class B, std::size_t N>
AliasSpec alias(const RepeatBundle<B, N>& value,
                std::source_location at = std::source_location::current()) {
    auto out = alias(detail::declaration_window(value), at);
    out.source_owner = detail::Access::owner(value[0]);
    out.source_lifetime = detail::Access::lifetime(value[0]);
    return out;
}
template <class T, std::size_t R, bool V>
RegViewSpec view(const RegView<T, R, V>& value,
                 std::source_location at = std::source_location::current()) {
    auto out = alias(PhysicalAddressWindow{value.address(), value.address_span_bytes()}, at);
    out.source_owner = detail::Access::owner(value);
    out.source_lifetime = detail::Access::lifetime(value);
    return out;
}
template <class Source>
RegViewSpec view(const Source& source, std::source_location at = std::source_location::current()) {
    return alias(source, at);
}

struct IdentitySpec {
    PhysicalRepeatAddressWindow window{};
    std::uint32_t domain{};
    std::uint64_t offset{}, size{};
    IdentityRemapMode mode = IdentityRemapMode::RedirectBank;
    std::string label;
    std::source_location location{};
    auto range(std::uint64_t start, std::uint64_t length) const {
        auto out = *this;
        out.offset = start;
        out.size = length;
        return out;
    }
    auto name(std::string value) const {
        auto out = *this;
        out.label = std::move(value);
        return out;
    }
};
inline IdentitySpec identity(PhysicalRepeatAddressWindow window,
                             std::uint32_t domain,
                             IdentityRemapMode mode = IdentityRemapMode::RedirectBank,
                             std::source_location at = std::source_location::current()) {
    IdentitySpec out;
    out.window = window;
    out.domain = domain;
    out.size = window.size;
    out.mode = mode;
    out.location = at;
    return out;
}
template <class B, std::size_t N>
IdentitySpec identity(const RepeatBundle<B, N>& value,
                      std::uint32_t domain,
                      IdentityRemapMode mode = IdentityRemapMode::RedirectBank,
                      std::source_location at = std::source_location::current()) {
    return identity(detail::declaration_window(value), domain, mode, at);
}
struct ReservedSpec {
    std::uint64_t base{}, size{}, count = 1, stride{};
    std::string label;
    std::source_location location{};
    auto repeat(std::uint64_t n, std::uint64_t step) const {
        auto out = *this;
        out.count = n;
        out.stride = step;
        return out;
    }
    auto name(std::string value) const {
        auto out = *this;
        out.label = std::move(value);
        return out;
    }
};
inline ReservedSpec reserved(std::uint64_t base,
                             std::uint64_t size,
                             std::source_location at = std::source_location::current()) {
    ReservedSpec out;
    out.base = base;
    out.size = size;
    out.stride = size;
    out.location = at;
    return out;
}
struct LayoutRoot {
    std::optional<AddressSpace> value;
    LayoutRoot() = default;
    template <class F>
        requires std::invocable<F&> && std::convertible_to<std::invoke_result_t<F&>, AddressSpace>
    LayoutRoot(F&& factory) : value(std::invoke(factory)) {
    }
};
struct LayoutSpec {
    LayoutRoot root{};
    std::initializer_list<RegViewSpec> views{};
    std::initializer_list<AliasSpec> aliases{};
    std::initializer_list<IdentitySpec> identities{};
    std::initializer_list<ReservedSpec> reserved{};
};
void apply_layout(RegisterMap& map, const LayoutSpec& spec);
namespace detail {
template <class Root>
struct DeclaredLayoutRoot {
    LayoutRoot dynamic;
    void (*install)(RegisterMap&, Root*){};
    DeclaredLayoutRoot() = default;
    template <class F>
    DeclaredLayoutRoot(F&& factory);
};
template <class Root>
struct DeclaredLayoutSpec {
    DeclaredLayoutRoot<Root> root{};
    std::initializer_list<RegViewSpec> views{};
    std::initializer_list<AliasSpec> aliases{};
    std::initializer_list<IdentitySpec> identities{};
    std::initializer_list<ReservedSpec> reserved{};
};
template <class Root>
void apply_declared_layout(RegisterMap&, Root*, const DeclaredLayoutSpec<Root>&);
} // namespace detail
} // namespace reg

// Put each group after its REG declarations. Every bundle instance installs its
// own members during construction, including automatically repeated VF groups.
#define REG_ACCESS_GROUP(NAME, ...)                                                                \
    [[no_unique_address]] ::reg::detail::DeclaredAccessGroup _regfw_access_group_##NAME =          \
        ::reg::detail::declare_access_group(#NAME, ::reg::AccessGroupSpec{__VA_ARGS__})

// Expands to the existing post-construction layout hook; no second setup call.
#define REG_LAYOUT(...)                                                                            \
    void configure_layout(::reg::RegisterMap& _regfw_map) {                                        \
        static_assert(                                                                             \
            REGFW_COMPILED_LAYOUT_ENTRY,                                                           \
            "REG_LAYOUT requires <regfw/regfw.hpp> (the full entry) in this translation unit; "    \
            "<regfw/bundle.hpp> only declares layouts");                                           \
        if (this->storage_only_scope() || region().base() != 0 ||                                  \
            region().size() != _regfw_map.size_bytes())                                            \
            throw ::reg::Error("REG_LAYOUT belongs in the map's root bundle",                      \
                               ::reg::ErrorKind::Configuration);                                   \
        ::reg::detail::apply_declared_layout(                                                      \
            _regfw_map,                                                                            \
            this,                                                                                  \
            ::reg::detail::DeclaredLayoutSpec<std::remove_cvref_t<decltype(*this)>>{__VA_ARGS__}); \
    }

// Declare in the root class; define exactly once in its namespace in a .cpp.
#define REG_LAYOUT_DECL() void configure_layout(::reg::RegisterMap& _regfw_map)
#define REG_LAYOUT_IMPL(TYPE, ...)                                                                 \
    void TYPE::configure_layout(::reg::RegisterMap& _regfw_map) {                                  \
        static_assert(REGFW_COMPILED_LAYOUT_ENTRY,                                                 \
                      "REG_LAYOUT_IMPL requires <regfw/regfw.hpp> (the full entry) in this "       \
                      "translation unit; "                                                         \
                      "<regfw/bundle.hpp> only declares layouts");                                 \
        if (this->storage_only_scope() || region().base() != 0 ||                                  \
            region().size() != _regfw_map.size_bytes())                                            \
            throw ::reg::Error("REG_LAYOUT belongs in the map's root bundle",                      \
                               ::reg::ErrorKind::Configuration);                                   \
        ::reg::detail::apply_declared_layout(                                                      \
            _regfw_map,                                                                            \
            this,                                                                                  \
            ::reg::detail::DeclaredLayoutSpec<std::remove_cvref_t<decltype(*this)>>{__VA_ARGS__}); \
    }

// Fixed one-coordinate named mapping. The normal REG_ACCESS_GROUP remains for
// conditions, multiple coordinates, custom conversions and metadata-only rules.
#define REG_STATIC_INDEX(NAME, REGISTER, SOURCE, OUTPUT, DIMENSION)                                \
    static inline constexpr auto NAME##_index_plan =                                               \
        ::reg::Groups::index_plan<REGISTER##_spec, DIMENSION>(SOURCE, OUTPUT, #NAME);              \
    [[no_unique_address]] ::reg::detail::DeclaredAccessGroup _regfw_access_group_##NAME =          \
        (::reg::Groups::install_index(::reg::detail::Access::node(REGISTER), NAME##_index_plan),   \
         ::reg::detail::DeclaredAccessGroup{})
