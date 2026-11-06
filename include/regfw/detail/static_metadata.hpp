#pragma once
// Literal declarations used by the optional consteval layout compiler.
namespace reg::detail {
template <auto Member, const auto& Spec, class T>
struct StaticRegisterMember {
    static constexpr auto member = Member;
    static constexpr const auto& spec = Spec;
    using Value = T;
    std::string_view name;
};
template <auto Member, const auto& Spec>
struct StaticBundleMember {
    static constexpr auto member = Member;
    static constexpr const auto& spec = Spec;
    std::string_view name;
};
template <class...>
struct StaticUnique : std::true_type {};
template <class T, class... Rest>
struct StaticUnique<T, Rest...>
    : std::bool_constant<(!std::same_as<T, Rest> && ...) && StaticUnique<Rest...>::value> {};
template <class B, auto... Members>
constexpr auto static_schema() {
    static_assert(StaticUnique<std::integral_constant<decltype(Members), Members>...>::value,
                  "duplicate REG_SCHEMA member");
    return std::tuple{B::template _regfw_static_member<B>(
        std::integral_constant<decltype(Members), Members>{})...};
}
struct StaticDynamicMember {
    std::string_view name;
};
} // namespace reg::detail

namespace reg::detail {
// Each declaration probes only declarations preceding it. The probe lambda has
// its own type, so no incomplete-class query is cached across declarations.
// No global counter, line number, friend injection or runtime registration.
template <std::size_t I>
struct DeclarationIndex {};
template <std::size_t I, class F>
constexpr bool declaration_exists() {
    return requires(F f) { f(DeclarationIndex<I>{}); };
}
template <std::size_t Lo, std::size_t Hi, class F>
consteval std::size_t declaration_search() {
    if constexpr (Hi - Lo == 1)
        return Lo;
    else if constexpr (declaration_exists<(Lo + Hi) / 2, F>())
        return declaration_search<(Lo + Hi) / 2, Hi, F>();
    else
        return declaration_search<Lo, (Lo + Hi) / 2, F>();
}
template <class F, std::size_t Hi = 1>
consteval std::size_t declaration_count(F) {
    if constexpr (declaration_exists<Hi, F>())
        return declaration_count<F, Hi * 2>(F{});
    else
        return declaration_search<0, Hi, F>();
}
template <class B>
consteval std::size_t automatic_member_count() {
    return declaration_count([](auto i) -> decltype(B::_regfw_auto_probe(i)) {
        return {};
    });
}
template <class B, std::size_t... I>
constexpr auto automatic_schema(std::index_sequence<I...>) {
    return std::tuple{B::template _regfw_auto_member<B>(DeclarationIndex<I>{})...};
}
template <class B>
constexpr auto declared_schema() {
    if constexpr (requires { B::_regfw_static_schema(); })
        return B::_regfw_static_schema();
    else
        return automatic_schema<B>(std::make_index_sequence<automatic_member_count<B>()>{});
}
struct SymbolSelectors {
    std::array<std::uint32_t, MAX_REG_DIMS> indices{};
    unsigned count{};
};
template <class Path, auto Member>
struct SymbolMemberValue;
template <class Path, auto Member>
using SymbolMemberValueT = typename SymbolMemberValue<Path, Member>::type;
} // namespace reg::detail

#define REGFW_DETAIL_AUTO_MEMBER(NAME)                                                             \
    static constexpr auto _regfw_##NAME##_ordinal =                                                \
        ::reg::detail::declaration_count([](auto i) -> decltype(_regfw_auto_probe(i)) {            \
            return {};                                                                             \
        });                                                                                        \
    static auto _regfw_auto_probe(::reg::detail::DeclarationIndex<_regfw_##NAME##_ordinal + 1>)    \
        -> std::true_type;                                                                         \
    template <class RegfwOwner>                                                                    \
    static constexpr auto _regfw_auto_member(                                                      \
        ::reg::detail::DeclarationIndex<_regfw_##NAME##_ordinal>) {                                \
        return _regfw_static_member<RegfwOwner>(                                                   \
            std::integral_constant<decltype(&RegfwOwner::NAME), &RegfwOwner::NAME>{});             \
    }                                                                                              \
    template <class RegfwOwner, class RegfwPath>                                                   \
    struct _regfw_##NAME##_symbol {                                                                \
        ::reg::detail::SymbolMemberValueT<RegfwPath, &RegfwOwner::NAME> NAME;                      \
        constexpr explicit _regfw_##NAME##_symbol(::reg::detail::SymbolSelectors s) : NAME(s) {    \
        }                                                                                          \
    };                                                                                             \
    template <class RegfwOwner, class RegfwPath>                                                   \
    static auto _regfw_symbol_mixin(::reg::detail::DeclarationIndex<_regfw_##NAME##_ordinal>)      \
        -> std::type_identity<_regfw_##NAME##_symbol<RegfwOwner, RegfwPath>>
