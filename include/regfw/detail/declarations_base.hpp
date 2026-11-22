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
    std::function<detail::CheckedIndex(const RegAccessInfo&, RegData)> resolver_;
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

  public:
    IndexMappingBuilder(Source source, Output output) {
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
        rule_.resolver_ = [f = std::move(f)](const RegAccessInfo& info,
                                             RegData logical) mutable -> detail::CheckedIndex {
            auto v = detail::value_from_data<typename Source::Value>(logical);
            if constexpr (std::is_invocable_v<F&, const RegAccessInfo&>)
                return detail::checked_access_result<typename Output::Value>(std::invoke(f, info));
            else if constexpr (std::is_invocable_v<F&, std::uint32_t, decltype(v)>) {
                if (!info.resolved_identity)
                    return {ErrorKind::Device, "no resolved identity"};
                return detail::checked_access_result<typename Output::Value>(
                    std::invoke(f, *info.resolved_identity, v));
            } else
                return detail::checked_access_result<typename Output::Value>(std::invoke(f, v));
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
} // namespace detail

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
