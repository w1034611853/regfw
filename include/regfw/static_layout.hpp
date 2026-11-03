#pragma once
#include "declarations.hpp"
#include "detail/static_plan.hpp"
#include "detail/formula_plan.hpp"
// This header provides the compiled-layout machinery: flip the entry marker
// consumed by the REG_LAYOUT/REG_LAYOUT_IMPL assertions.
#undef REGFW_COMPILED_LAYOUT_ENTRY
#define REGFW_COMPILED_LAYOUT_ENTRY 1
#include <algorithm>
#include <tuple>
#include <vector>
namespace reg {
template <class... Children>
class StaticView;
namespace detail {
template <class T>
struct StaticMemberPointer;
template <class T>
struct StaticMemberPointer<const T> : StaticMemberPointer<T> {};
template <class C, class V>
struct StaticMemberPointer<V C::*> {
    using Owner = C;
    using Value = V;
};
template <class T>
struct StaticRepeatType {
    using Value = T;
    static constexpr std::size_t count = 0;
};
template <class B, std::size_t N>
struct StaticRepeatType<RepeatBundle<B, N>> {
    using Value = B;
    static constexpr std::size_t count = N;
};
template <class B, class F>
constexpr void static_members(F&& f) {
    std::apply(
        [&](const auto&... member) {
            (f(member), ...);
        },
        declared_schema<B>());
}
template <auto... M>
struct StaticPath;
template <auto First, auto... Rest>
struct StaticPath<First, Rest...> {
    using Root = typename StaticMemberPointer<decltype(First)>::Owner;
    static constexpr std::uint64_t hash = [] {
        std::uint64_t h = 1469598103934665603ULL;
        for (unsigned char c : callback_type_name<StaticPath<First, Rest...>>())
            h = (h ^ c) * 1099511628211ULL;
        return h;
    }();
    template <auto M>
    using Append = StaticPath<First, Rest..., M>;
    template <class Current, auto Member, auto... More>
    static decltype(auto)
    walk(Current& current, const std::array<std::uint32_t, MAX_REG_DIMS>& s, unsigned& at) {
        auto& value = current.*Member;
        using V = std::remove_cvref_t<decltype(value)>;
        auto select = [&]() -> decltype(auto) {
            if constexpr (StaticRepeatType<V>::count)
                return value[s[at++]];
            else
                return (value);
        };
        if constexpr (sizeof...(More))
            return walk<std::remove_reference_t<decltype(select())>, More...>(select(), s, at);
        else
            return select();
    }
    static const Bundle* bundle(void* root, const std::array<std::uint32_t, MAX_REG_DIMS>& s) {
        unsigned at = 0;
        return &walk<Root, First, Rest...>(*static_cast<Root*>(root), s, at);
    }
    static EntityState* get(void* root, const std::array<std::uint32_t, MAX_REG_DIMS>& s) {
        unsigned at = 0;
        return Access::node(walk<Root, First, Rest...>(*static_cast<Root*>(root), s, at));
    }
};
template <class T, auto... M>
struct StaticPathType;
template <class T>
struct StaticPathType<T> {
    using Value = T;
};
template <class T, auto First, auto... Rest>
struct StaticPathType<T, First, Rest...> {
    using V = typename StaticMemberPointer<decltype(First)>::Value;
    using Value = typename StaticPathType<typename StaticRepeatType<V>::Value, Rest...>::Value;
};
template <auto Last, auto... Rest>
constexpr auto static_last_member() {
    if constexpr (sizeof...(Rest))
        return static_last_member<Rest...>();
    else
        return Last;
}
template <auto Member>
constexpr std::string_view static_member_name() {
    using Owner = typename StaticMemberPointer<decltype(Member)>::Owner;
    return Owner::template _regfw_static_member<Owner>(
               std::integral_constant<decltype(Member), Member>{})
        .name;
}
template <auto... M>
struct StaticTarget {
    using Path = StaticPath<M...>;
    using Root = typename Path::Root;
    using Value = typename StaticPathType<Root, M...>::Value;
    std::array<std::uint32_t, MAX_REG_DIMS> selectors{};
    unsigned selected{};
    constexpr auto at(std::uint32_t index) const {
        auto out = *this;
        if (out.selected == MAX_REG_DIMS)
            throw "static target selector depth exceeded";
        out.selectors[out.selected++] = index;
        return out;
    }
};
template <class T>
struct StaticMount {
    T value;
    std::uint64_t offset;
    std::int32_t priority;
};
template <class T>
struct StaticRepeatMount {
    std::string_view name;
    RepeatSpec repeat;
    std::vector<T> values;
};
struct StaticReserved {
    std::string_view name = "RESERVED";
    std::uint64_t size;
};
struct StaticViewOptions {
    std::string_view name;
    std::uint64_t size{};
    AliasAccessPolicy policy;
    AccessRouteKind route = AccessRouteKind::Direct;
    std::optional<std::uint32_t> domain;
    const void* resolver_key{};
    std::string_view resolver_name;
    const void* pre_key{};
    std::string_view pre_name;
    std::source_location location;
    std::string_view resolver_type{}, pre_type{};
};
} // namespace detail
template <auto... Member>
constexpr auto target() {
    return detail::StaticTarget<Member...>{};
}
// Layout composition retains the public constexpr-friendly DSL, but expands
// it exactly once at normal runtime; no full-layout constant evaluation.
template <class... Children>
class StaticView {
  public:
    detail::StaticViewOptions options;
    std::tuple<Children...> children;
    constexpr explicit StaticView(detail::StaticViewOptions value, std::tuple<Children...> c = {})
        : options(value), children(std::move(c)) {
    }
    template <class T>
    constexpr auto map(T target, std::uint64_t offset = 0, std::int32_t priority = 0) const {
        if constexpr (requires { target._regfw_target(); })
            return map(target._regfw_target(), offset, priority);
        else {
            using M = detail::StaticMount<T>;
            return StaticView<Children..., M>(
                options,
                std::tuple_cat(children, std::tuple{M{std::move(target), offset, priority}}));
        }
    }
    template <class F>
    constexpr auto map_repeat(std::string_view name,
                              std::uint64_t offset,
                              RepeatSpec spec,
                              F factory,
                              std::int32_t priority = 0) const {
        using T = decltype(factory(std::size_t{}));
        detail::StaticRepeatMount<T> repeat{name, spec, {}};
        if (!spec.count || spec.count > UINT32_MAX || !spec.stride_bytes ||
            spec.count > UINT64_MAX / spec.stride_bytes)
            throw "invalid static repeat";
        repeat.values.reserve(spec.count);
        for (std::size_t i = 0; i < spec.count; ++i)
            repeat.values.push_back(factory(i));
        return map(std::move(repeat), offset, priority);
    }
    constexpr auto
    reserve(std::uint64_t offset, std::uint64_t size, std::int32_t priority = 0) const {
        return map(detail::StaticReserved{"RESERVED", size}, offset, priority);
    }
    constexpr auto identity(std::uint32_t domain) const {
        auto out = *this;
        out.options.domain = domain;
        return out;
    }
    template <class Tag, class V>
    constexpr auto using_resolver(FieldKey<Tag, V> key) const {
        static_assert(std::same_as<V, std::uint32_t>);
        auto out = *this;
        out.options.resolver_key = key.id();
        out.options.resolver_name = key.name;
        out.options.resolver_type = detail::callback_type_name<FieldKey<Tag, V>>();
        return out;
    }
    template <class Tag>
    constexpr auto using_pre_access(PreAccessKey<Tag> key) const {
        if (!options.pre_type.empty())
            throw "duplicate static pre_access";
        auto out = *this;
        out.options.pre_key = key.id();
        out.options.pre_name = key.name;
        out.options.pre_type = detail::callback_type_name<PreAccessKey<Tag>>();
        return out;
    }
    constexpr auto read_only_ignore_writes() const {
        auto out = *this;
        out.options.policy = AliasAccessPolicy::READ_ONLY_IGNORE_WRITES;
        return out;
    }
    constexpr auto read_only_error_writes() const {
        auto out = *this;
        out.options.policy = AliasAccessPolicy::READ_ONLY_ERROR_WRITES;
        return out;
    }
    constexpr auto vf_to_pf() const {
        auto out = *this;
        out.options.route = AccessRouteKind::VfToPf;
        return out;
    }
    constexpr auto pf_to_vf() const {
        auto out = *this;
        out.options.route = AccessRouteKind::PfToVf;
        return out;
    }
};
constexpr auto static_view(std::string_view name,
                           std::uint64_t size,
                           std::source_location at = std::source_location::current()) {
    return StaticView<>(
        detail::StaticViewOptions{name, size, {}, AccessRouteKind::Direct, {}, {}, {}, {}, {}, at});
}
constexpr auto static_address_space(std::string_view name,
                                    std::source_location at = std::source_location::current()) {
    return static_view(name, UINT64_MAX & ~std::uint64_t{3}, at);
}
namespace detail {
// Temporary build vectors are ordinary native allocations. No constexpr
// allocator, count pass, merge-sort buffers, or per-word candidate tables.
template <class T>
using StaticBuffer = std::vector<T>;
enum class StaticPathStatus : std::uint8_t {
    Fast,
    ActualOverlap,
    ReservedIntersection,
    SelfOverlap,
    OutOfBounds,
    IdentityBoundary,
    ArithmeticOverflow,
    UnsupportedGeometry
};
constexpr std::string_view static_path_reason(StaticPathStatus s) {
    switch (s) {
        case StaticPathStatus::Fast:
            return "fast path";
        case StaticPathStatus::ActualOverlap:
            return "ActualOverlap: two different register sources occupy the same bytes";
        case StaticPathStatus::ReservedIntersection:
            return "ReservedIntersection: a reserve covers an occupied register interval";
        case StaticPathStatus::SelfOverlap:
            return "SelfOverlap: elements of one declaration occupy the same bytes";
        case StaticPathStatus::OutOfBounds:
            return "OutOfBounds: a register element crosses its view";
        case StaticPathStatus::IdentityBoundary:
            return "IdentityBoundary: a register element crosses an identity family stride";
        case StaticPathStatus::ArithmeticOverflow:
            return "ArithmeticOverflow: address or count arithmetic overflow";
        case StaticPathStatus::UnsupportedGeometry:
            return "UnsupportedGeometry";
    }
    return "unknown";
}
// Evaluated once per register declaration type and shared by every mount of
// that declaration: element count, occupied packing, the stride-ascending
// axes permutation and the monotonicity proof for direct element iteration.
template <class D>
struct StaticRegisterShape {
    static constexpr auto& spec = D::spec;
    using Value = typename D::Value;
    static constexpr unsigned width = value_bits<Value> / 8, occupied = (width + 3u) & ~3u;
    static constexpr std::uint32_t count = [] {
        std::uint32_t n = 1;
        for (auto d : spec.dimensions) {
            if (!d || d > UINT32_MAX || n > UINT32_MAX / d)
                throw "static register dimension overflow";
            n *= d;
        }
        return n;
    }();
    static constexpr auto axes = [] {
        // Sorted by stride, allowing transposed dimensions and extent 1.
        std::array<std::size_t, MAX_REG_DIMS> out{};
        for (std::size_t d = 0; d < spec.rank; ++d)
            out[d] = d;
        for (std::size_t d = 1; d < spec.rank; ++d) {
            auto j = d;
            while (j && spec.stride_bytes[out[j]] < spec.stride_bytes[out[j - 1]]) {
                std::swap(out[j], out[j - 1]);
                --j;
            }
        }
        return out;
    }();
    // Monotone (Type A): iterating the stride-sorted mixed-radix digits emits
    // element addresses in ascending order. Requires every non-unit axis
    // stride to clear the full inner span so digit rollovers never step back.
    static constexpr bool monotone = [] {
        std::uint64_t span = occupied;
        for (std::size_t d = 0; d < spec.rank; ++d) {
            const auto k = axes[d];
            if (spec.dimensions[k] == 1)
                continue;
            if (spec.stride_bytes[k] < span)
                return false;
            if (spec.stride_bytes[k] > (UINT64_MAX - span) / (spec.dimensions[k] - 1))
                return false;
            span = (spec.dimensions[k] - 1) * spec.stride_bytes[k] + span;
        }
        return true;
    }();
    // Bounding span of all element intervals; equals occupied for scalars.
    static constexpr std::uint64_t span = monotone ? [] {
        std::uint64_t s = occupied;
        for (std::size_t d = 0; d < spec.rank; ++d) {
            const auto k = axes[d];
            if (spec.dimensions[k] == 1)
                continue;
            s = (spec.dimensions[k] - 1) * spec.stride_bytes[k] + s;
        }
        return s;
    }()
                                                   : 0;
    // Ascending physical order equals declaration-linear order exactly when
    // the non-unit axes appear in descending declaration order.
    static constexpr bool linear_order = [] {
        std::ptrdiff_t previous = PTRDIFF_MAX;
        for (std::size_t d = 0; d < spec.rank; ++d) {
            const auto k = static_cast<std::ptrdiff_t>(axes[d]);
            if (spec.dimensions[axes[d]] != 1 && k >= previous)
                return false;
            if (spec.dimensions[axes[d]] != 1)
                previous = k;
        }
        return true;
    }();
};
// Non-monotone element tables are built once per declaration type at runtime.
// In particular, the compiler never constexpr-sorts thousands of offsets.
template <class D>
const std::vector<ElementRecord>& static_sparse_order() {
    static const auto records = [] {
        using Shape = StaticRegisterShape<D>;
        std::vector<ElementRecord> out;
        if constexpr (!Shape::monotone) {
            if (Shape::count > 16384)
                throw "formula layout: UnsupportedGeometry (non-monotone register shape needs a "
                      "per-element record table larger than 16384)";
            out.reserve(Shape::count);
            std::array<std::uint64_t, MAX_REG_DIMS> multiplier{};
            std::uint64_t product = 1;
            for (std::size_t d = Shape::spec.rank; d-- > 0;) {
                multiplier[d] = product;
                product *= Shape::spec.dimensions[d];
            }
            for (std::uint32_t k = 0; k < Shape::count; ++k) {
                std::uint64_t rest = k, offset = 0, linear = 0;
                for (std::size_t d = 0; d < Shape::spec.rank; ++d) {
                    const auto i = rest % Shape::spec.dimensions[d];
                    rest /= Shape::spec.dimensions[d];
                    if (i && Shape::spec.stride_bytes[d] > (UINT64_MAX - offset) / i)
                        throw "formula layout: ArithmeticOverflow (register element address)";
                    offset += i * Shape::spec.stride_bytes[d];
                    linear += i * multiplier[d];
                }
                out.push_back({offset, static_cast<std::uint32_t>(linear)});
            }
            std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
                return a.offset < b.offset || (a.offset == b.offset && a.linear < b.linear);
            });
        }
        return out;
    }();
    return records;
}
// Per-declaration iteration plan shared by all mounts: element cursor digits
// (monotone) or the sorted record table (interleaved).
struct StaticElementPlan {
    std::uint32_t count{};
    unsigned occupied{};
    bool monotone{}, linear_order{};
    std::array<std::uint32_t, MAX_REG_DIMS> digits{};
    std::array<std::uint64_t, MAX_REG_DIMS> steps{};
    // Stride-ascending axis -> declaration axis and its declaration-order
    // linearization multiplier. These drive the formula decoder's arithmetic
    // reverse mapping without any per-element table.
    std::array<std::uint32_t, MAX_REG_DIMS> axes_map{};
    std::array<std::uint64_t, MAX_REG_DIMS> linear_mult{};
    std::span<const ElementRecord> sparse_order{};
};
template <class D>
const StaticElementPlan& static_element_plan() {
    static const auto result = [] {
        using Shape = StaticRegisterShape<D>;
        StaticElementPlan p{};
        p.count = Shape::count;
        p.occupied = Shape::occupied;
        p.monotone = Shape::monotone;
        p.linear_order = Shape::linear_order;
        std::array<std::uint64_t, MAX_REG_DIMS> multiplier{};
        std::uint64_t product = 1; // from_dims linearizes with the last dimension fastest.
        for (std::size_t d = Shape::spec.rank; d-- > 0;) {
            multiplier[d] = product;
            product *= Shape::spec.dimensions[d];
        }
        for (std::size_t i = 0; i < Shape::spec.rank; ++i) {
            const auto a = Shape::axes[i];
            p.digits[i] = static_cast<std::uint32_t>(Shape::spec.dimensions[a]);
            p.steps[i] = Shape::spec.stride_bytes[a];
            p.axes_map[i] = static_cast<std::uint32_t>(a);
            p.linear_mult[i] = multiplier[a];
        }
        if constexpr (!Shape::monotone)
            p.sparse_order = static_sparse_order<D>();
        return p;
    }();
    return result;
}
// One mounted register declaration: element 0's absolute address plus the
// shared iteration plan. The fast paths never materialize per-word data.
struct ElementMount {
    std::uint64_t base;
    std::uint32_t binding, scope;
    const StaticElementPlan* plan{};
    bool scalar{};
};
// Metadata-only traversal used by the formula backend. This is the retained
// measuring half of the old builder: it records bindings, scopes, identity
// families and one mount per register declaration, but has no word/candidate/
// page storage and no full-solve methods.
struct FormulaBuild {
    StaticPathStatus status = StaticPathStatus::Fast;
    StaticBuffer<ElementMount> mounts;
    std::string_view root_type;
    struct RepeatContext {
        bool active{};
        std::uint64_t count{}, stride{}, index{};
    } next_repeat;
    StaticBuffer<StaticBinding> bindings;
    StaticBuffer<std::uint32_t> binding_index;
    std::uint32_t emitted_nodes{}, first_binding = STATIC_NONE;
    StaticBuffer<StaticScope> scopes;
    StaticBuffer<StaticIdentity> identities;
    static constexpr void fit(std::uint64_t offset, std::uint64_t size, std::uint64_t parent) {
        if (!size || offset % 4 || size % 4 || offset > parent || size > parent - offset)
            throw "static view outside parent/unaligned";
    }
    constexpr std::uint32_t scope(StaticViewOptions o,
                                  std::uint64_t offset,
                                  std::int32_t priority,
                                  std::uint32_t parent,
                                  bool mapping = false,
                                  bool reserved = false) {
        fit(offset,
            o.size,
            parent == STATIC_NONE ? UINT64_MAX & ~std::uint64_t{3} : scopes[parent].size);
        if (o.name.empty() || (!o.resolver_type.empty() && !o.domain))
            throw "invalid static view name/resolver";
        StaticScope s;
        s.name = o.name;
        s.base = offset + (parent == STATIC_NONE ? 0 : scopes[parent].base);
        s.size = o.size;
        s.parent = parent;
        s.priority = priority;
        s.mapping = mapping;
        s.reserved = reserved;
        s.policy = o.policy;
        s.route = o.route;
        s.domain = o.domain;
        s.resolver_key = o.resolver_key;
        s.resolver_name = o.resolver_name;
        s.resolver_type = o.resolver_type;
        s.pre_key = o.pre_key;
        s.pre_name = o.pre_name;
        s.pre_type = o.pre_type;
        s.location = o.location;
        if (parent != STATIC_NONE) {
            s.instance = scopes[parent].instance;
            s.repeat_count = scopes[parent].repeat_count;
            s.repeat_stride = scopes[parent].repeat_stride;
            s.repeat_index = scopes[parent].repeat_index;
            if (s.route == AccessRouteKind::Direct)
                s.route = scopes[parent].route;
        }
        if (next_repeat.active) {
            s.repeated = true;
            s.instance = next_repeat.index;
            s.repeat_count = next_repeat.count;
            s.repeat_stride = next_repeat.stride;
            s.repeat_index = next_repeat.index;
            next_repeat.active = false;
        }
        const auto result = static_cast<std::uint32_t>(scopes.size());
        scopes.push_back(s);
        return result;
    }
    template <class... C>
    constexpr void flatten(const StaticView<C...>& v,
                           std::uint64_t offset,
                           std::int32_t priority,
                           std::uint32_t parent = STATIC_NONE) {
        const auto id = scope(v.options, offset, priority, parent);
        std::apply(
            [&](const auto&... child) {
                (mount(child, id), ...);
            },
            v.children);
    }
    template <class T>
    constexpr void mount(const StaticMount<T>& m, std::uint32_t parent) {
        flatten(m.value, m.offset, m.priority, parent);
    }
    constexpr void flatten(const StaticReserved& r,
                           std::uint64_t offset,
                           std::int32_t priority,
                           std::uint32_t parent) {
        scope({r.name, r.size, {}, AccessRouteKind::Direct, {}, {}, {}, {}, {}, {}},
              offset,
              priority,
              parent,
              false,
              true);
    }
    template <class T>
    constexpr void flatten(const StaticRepeatMount<T>& r,
                           std::uint64_t offset,
                           std::int32_t priority,
                           std::uint32_t parent) {
        const auto span = r.repeat.count * r.repeat.stride_bytes;
        const auto family =
            scope({r.name, span, {}, AccessRouteKind::Direct, {}, {}, {}, {}, {}, {}},
                  offset,
                  priority,
                  parent);
        std::size_t first = 0, first_size = 0;
        for (std::size_t i = 0; i < r.values.size(); ++i) {
            next_repeat = {true, r.repeat.count, r.repeat.stride_bytes, i};
            const auto before = scopes.size();
            flatten(r.values[i], i * r.repeat.stride_bytes, 0, family);
            if (scopes[before].size > r.repeat.stride_bytes)
                throw "static repeated view exceeds stride";
            if (!i) {
                first = before;
                first_size = scopes.size() - before;
            } else {
                if (scopes.size() - before != first_size)
                    throw "static repeated view geometry differs";
                for (std::size_t k = 0; k < first_size; ++k) {
                    const auto& a = scopes[first + k];
                    const auto& b = scopes[before + k];
                    if (a.name != b.name || a.size != b.size ||
                        a.base - scopes[first].base != b.base - scopes[before].base ||
                        a.priority != b.priority || a.domain != b.domain ||
                        a.resolver_type != b.resolver_type || a.pre_type != b.pre_type ||
                        a.route != b.route || a.policy.allow_read != b.policy.allow_read ||
                        a.policy.allow_write != b.policy.allow_write ||
                        a.policy.denied_read != b.policy.denied_read ||
                        a.policy.denied_write != b.policy.denied_write)
                        throw "static repeated view declarations differ";
                }
            }
        }
    }
    template <auto... M>
    constexpr void flatten(const StaticTarget<M...>& t,
                           std::uint64_t offset,
                           std::int32_t priority,
                           std::uint32_t parent) {
        using P = typename StaticTarget<M...>::Path;
        using B = typename StaticTarget<M...>::Value;
        static_assert(
            std::derived_from<B, Bundle>,
            "static target must name an entity bundle; put register slices in a separate entity");
        if (root_type != callback_type_name<typename P::Root>())
            throw "static target belongs to a different root type";
        unsigned selected = 0;
        (
            [&] {
                using V = typename StaticMemberPointer<decltype(M)>::Value;
                if constexpr (StaticRepeatType<V>::count) {
                    if (selected >= t.selected ||
                        t.selectors[selected] >= StaticRepeatType<V>::count)
                        throw "static target repeat selector out of range";
                    ++selected;
                }
            }(),
            ...);
        if (selected != t.selected)
            throw "unused static target selector";
        const auto id = scope({static_member_name<static_last_member<M...>()>(),
                               scopes[parent].size - offset,
                               {},
                               AccessRouteKind::Direct,
                               {},
                               {},
                               {},
                               {},
                               {},
                               {}},
                              offset,
                              priority,
                              parent,
                              true);
        const auto start_nodes = static_cast<std::size_t>(emitted_nodes);
        first_binding = STATIC_NONE;
        emit_bundle<B, P>(id, 0, t.selectors, t.selected);
        if (emitted_nodes == start_nodes)
            throw "cannot map an empty static entity group";
        scopes[id].bundle = &P::bundle;
        scopes[id].selectors = t.selectors;
        scopes[id].target_binding = first_binding;
        scopes[id].target_nodes = emitted_nodes - start_nodes;
    }
    template <class B, class P>
    constexpr void emit_bundle(std::uint32_t scope_id,
                               std::uint64_t local,
                               std::array<std::uint32_t, MAX_REG_DIMS> selectors,
                               unsigned depth) {
        static_members<B>([&](const auto& member) {
            using D = std::remove_cvref_t<decltype(member)>;
            if constexpr (std::same_as<D, StaticDynamicMember>)
                throw "REG_DYNAMIC requires dynamic REG_LAYOUT";
            else if constexpr (requires { typename D::Value; })
                emit_register<typename P::template Append<D::member>, D>(
                    scope_id, local, selectors, member.name);
            else {
                using V = typename StaticMemberPointer<decltype(D::member)>::Value;
                using Child = typename StaticRepeatType<V>::Value;
                if constexpr (StaticRepeatType<V>::count) {
                    if (depth == MAX_REG_DIMS)
                        throw "static entity nesting exceeded";
                    for (std::size_t k = 0; k < StaticRepeatType<V>::count; ++k) {
                        selectors[depth] = k;
                        emit_bundle<Child, typename P::template Append<D::member>>(
                            scope_id,
                            local + D::spec.offset + k * D::spec.stride_bytes,
                            selectors,
                            depth + 1);
                    }
                } else
                    emit_bundle<Child, typename P::template Append<D::member>>(
                        scope_id, local + D::spec.offset, selectors, depth);
            }
        });
    }
    template <class P, class D>
    constexpr void emit_register(std::uint32_t scope_id,
                                 std::uint64_t local,
                                 const std::array<std::uint32_t, MAX_REG_DIMS>& selectors,
                                 std::string_view) {
        ++emitted_nodes;
        constexpr auto& spec = D::spec;
        using Shape = StaticRegisterShape<D>;
        constexpr unsigned width = Shape::width, occupied = Shape::occupied;
        constexpr auto count = Shape::count;
        auto hash = P::hash;
        for (auto n : selectors)
            hash = (hash ^ n) * 1099511628211ULL;
        if (binding_index.empty() || bindings.size() * 2 >= binding_index.size()) {
            StaticBuffer<std::uint32_t> next(binding_index.empty() ? 8 : binding_index.size() * 2,
                                             STATIC_NONE);
            for (std::uint32_t k = 0; k < bindings.size(); ++k) {
                auto slot = bindings[k].hash & (next.size() - 1);
                while (next[slot] != STATIC_NONE)
                    slot = (slot + 1) & (next.size() - 1);
                next[slot] = k;
            }
            binding_index = std::move(next);
        }
        auto slot = hash & (binding_index.size() - 1);
        std::uint32_t binding;
        for (;;) {
            binding = binding_index[slot];
            if (binding == STATIC_NONE)
                break;
            if (bindings[binding].hash == hash && bindings[binding].selectors == selectors &&
                bindings[binding].path_type == callback_type_name<P>())
                break;
            slot = (slot + 1) & (binding_index.size() - 1);
        }
        if (binding == STATIC_NONE) {
            binding = bindings.size();
            binding_index[slot] = binding;
            bindings.push_back({&P::get,
                                selectors,
                                scopes[scope_id].base + local + spec.offset,
                                local + spec.offset,
                                hash,
                                callback_type_name<P>(),
                                count,
                                scope_id,
                                width});
        }
        if (first_binding == STATIC_NONE)
            first_binding = binding;
        bool scalar = false;
        if constexpr (std::same_as<std::remove_cvref_t<decltype(spec.behavior)>, reg::Access>)
            scalar = width == 4 && spec.behavior == reg::Access::Rw && spec.widths.allows(4) &&
                     (!spec.transaction_policy || spec.transaction_policy->independent());
        const auto at = local + spec.offset;
        if (local > UINT64_MAX - spec.offset || at > scopes[scope_id].size) {
            status = StaticPathStatus::ArithmeticOverflow;
            return;
        }
        if (at % 4) {
            status = StaticPathStatus::UnsupportedGeometry;
            return;
        }
        const std::uint64_t last = Shape::monotone
                                       ? Shape::span
                                       : (static_sparse_order<D>().size()
                                              ? static_sparse_order<D>().back().offset + occupied
                                              : occupied);
        if (last > scopes[scope_id].size - at) {
            status = StaticPathStatus::OutOfBounds;
            return;
        }
        mounts.push_back(
            {scopes[scope_id].base + at, binding, scope_id, &static_element_plan<D>(), scalar});
    }
    constexpr auto scope_identities() {
        StaticBuffer<std::uint32_t> identities_for(scopes.size(), STATIC_NONE);
        for (std::uint32_t id = 0; id < scopes.size(); ++id) {
            const auto& s = scopes[id];
            if (s.parent != STATIC_NONE)
                identities_for[id] = identities_for[s.parent];
            if (!s.domain)
                continue;
            if (identities_for[id] != STATIC_NONE)
                throw "nested static identity domains unsupported";
            const auto stride = s.repeat_stride ? s.repeat_stride : s.size,
                       base = s.base - s.repeat_index * stride;
            std::uint32_t index = 0;
            for (; index < identities.size(); ++index) {
                const auto& v = identities[index];
                if (v.base == base && v.count == s.repeat_count && v.stride == stride &&
                    v.size == s.size && v.domain == *s.domain && v.resolver_type == s.resolver_type)
                    break;
            }
            if (index == identities.size())
                identities.push_back({base,
                                      s.repeat_count,
                                      stride,
                                      s.size,
                                      *s.domain,
                                      id,
                                      s.resolver_key,
                                      s.resolver_name,
                                      s.resolver_type});
            identities_for[id] = index;
        }
        return identities_for;
    }
};

// ---- Formula metadata build (native C++, once per layout) -----------------
// A single pass over the DSL creates compact bindings, scopes, mount ranges,
// and array formulas. No per-word table and no compiler interpreter work.
template <class Root, auto Factory, class Fill>
void formula_run(Fill&& fill) {
    FormulaBuild b;
    b.root_type = callback_type_name<Root>();
    b.flatten(Factory(), 0, 0);
    if (b.status != StaticPathStatus::Fast)
        throw static_path_reason(b.status);
    const auto identities_for = b.scope_identities();
    StaticBuffer<FormulaArraySpec> arrays;
    StaticBuffer<FormulaMountSpec> mounts;
    for (std::size_t i = 0; i < b.mounts.size(); ++i) {
        const auto& source = b.mounts[i];
        const auto& plan = *source.plan;
        FormulaArraySpec spec{};
        spec.count = plan.count;
        spec.occupied = plan.occupied;
        spec.monotone = plan.monotone;
        std::size_t rank = 0;
        while (rank < MAX_REG_DIMS && plan.digits[rank])
            ++rank;
        spec.rank = static_cast<std::uint8_t>(rank);
        spec.dense =
            plan.monotone && plan.linear_order && rank == 1 && plan.steps[0] == plan.occupied;
        for (std::size_t d = 0; d < rank; ++d) {
            spec.strides_asc[d] = plan.steps[d];
            spec.extents_asc[d] = plan.digits[d];
            spec.axis_asc[d] = plan.axes_map[d];
            spec.multiplier[d] = plan.linear_mult[d];
            const auto axis = plan.axes_map[d];
            spec.strides_decl[axis] = plan.steps[d];
            spec.extents_decl[axis] = plan.digits[d];
            spec.multiplier_decl[axis] = plan.linear_mult[d];
        }
        // Choose the cheapest exact inverse for this shape. A rank-0 shape is
        // one element; a contiguous rank-1 shape is a division; a monotone
        // lattice is successive division from the largest stride. An
        // interleaved lattice is a gcd reduction plus a modular inverse on
        // the two smallest-stride axes, with every larger axis decoded as a
        // super-increasing ladder above them — so interleaved declarations of
        // any rank are arithmetic whenever each non-pair axis dominates the
        // full span below it. Only shapes where no such split exists need the
        // shared element records.
        bool classified = false;
        if (plan.count == 1) {
            spec.kind = FormulaDecodeKind::Scalar;
            spec.span = plan.occupied;
            classified = true;
        } else if (spec.dense) {
            spec.kind = FormulaDecodeKind::Dense;
            spec.span = std::uint64_t(plan.count) * plan.occupied;
            classified = true;
        } else if (plan.monotone) {
            std::uint64_t span = plan.occupied;
            for (std::size_t d = 0; d < rank; ++d)
                span += std::uint64_t(plan.digits[d] - 1) * plan.steps[d];
            spec.kind = FormulaDecodeKind::Monotone;
            spec.span = span;
            classified = true;
        } else
            switch (classify_interleaved(spec)) {
                case FormulaClassifyResult::Arithmetic:
                    classified = true;
                    break;
                case FormulaClassifyResult::SelfOverlap:
                    throw "formula layout: SelfOverlap (one declaration maps elements onto the "
                          "same bytes)";
                case FormulaClassifyResult::BadInverse:
                    throw "formula layout: InterleavedDecode (no modular inverse for the declared "
                          "strides)";
                case FormulaClassifyResult::NeedsTable:
                    break;
            }
        if (!classified) {
            // No arithmetic split exists: keep the shared offset-sorted
            // records. They are per declaration shape, never per word. Refuse
            // shapes whose record table would grow unboundedly: silently
            // falling back to the word builder is the failure mode this
            // backend exists to remove.
            if (plan.sparse_order.size() > 16384)
                throw "formula layout: UnsupportedGeometry (non-monotone register shape needs a "
                      "per-element record table larger than 16384)";
            for (std::size_t k = 1; k < plan.sparse_order.size(); ++k)
                if (plan.sparse_order[k].offset == plan.sparse_order[k - 1].offset)
                    throw "formula layout: SelfOverlap (one declaration maps elements onto the "
                          "same bytes)";
            spec.kind = FormulaDecodeKind::Table;
            spec.sparse = plan.sparse_order;
            spec.span = plan.sparse_order.empty() ? std::uint64_t(plan.occupied)
                                                  : plan.sparse_order.back().offset + plan.occupied;
        }
        std::uint32_t index = 0;
        for (; index < arrays.size(); ++index) {
            const auto& other = arrays[index];
            if (other.count == spec.count && other.occupied == spec.occupied &&
                other.monotone == spec.monotone && other.rank == spec.rank &&
                other.kind == spec.kind && other.dense == spec.dense &&
                other.strides_asc == spec.strides_asc && other.extents_asc == spec.extents_asc &&
                other.axis_asc == spec.axis_asc && other.multiplier == spec.multiplier &&
                other.sparse.size() == spec.sparse.size())
                break;
        }
        if (index == arrays.size())
            arrays.push_back(spec);
        // Identity containment: the mount must stay inside one family stride.
        const auto identity = identities_for[source.scope];
        if (identity != STATIC_NONE) {
            const auto& family = b.identities[identity];
            const auto at = (source.base - family.base) % family.stride;
            if (spec.span > family.stride - at)
                throw "formula layout: IdentityBoundary (a register element crosses an identity "
                      "family stride)";
        }
        const std::uint64_t end = source.base + spec.span;
        mounts.push_back({source.base, end, source.binding, source.scope, index, source.scalar});
    }
    std::sort(mounts.begin(), mounts.end(), [](const auto& a, const auto& c) {
        return a.base < c.base || (a.base == c.base && a.scope < c.scope);
    });
    fill(b, arrays, mounts);
}
// Runtime-owned formula plan: geometry is computed by ordinary native code,
// once per layout factory, rather than interpreted by the compiler.
struct RuntimeFormulaPlan {
    std::vector<StaticBinding> bindings;
    std::vector<StaticScope> scopes;
    std::vector<StaticIdentity> identities;
    std::vector<FormulaMountSpec> mounts;
    std::vector<FormulaArraySpec> arrays;

    FormulaPlanView view() const noexcept {
        return {bindings, scopes, identities, mounts, arrays};
    }
};
template <class Root, auto Factory>
RuntimeFormulaPlan build_formula_layout_runtime() {
    RuntimeFormulaPlan out;
    formula_run<Root, Factory>([&out](const auto& b, const auto& arrays, const auto& mounts) {
        out.bindings.assign(b.bindings.begin(), b.bindings.end());
        out.scopes.assign(b.scopes.begin(), b.scopes.end());
        out.identities.assign(b.identities.begin(), b.identities.end());
        out.mounts.assign(mounts.begin(), mounts.end());
        out.arrays.assign(arrays.begin(), arrays.end());
    });
    return out;
}
void install_formula_layout(RegisterMap&, void*, FormulaPlanView);
} // namespace detail
} // namespace reg

#define REGFW_DETAIL_LAYOUT_BACKEND(RegBundleType, ...)                                            \
    static const auto _regfw_plan =                                                                \
        ::reg::detail::build_formula_layout_runtime<RegBundleType, __VA_ARGS__>();                 \
    ::reg::detail::install_formula_layout(_regfw_map, this, _regfw_plan.view())
#define REG_STATIC_LAYOUT(...)                                                                     \
    void configure_layout(::reg::RegisterMap& _regfw_map) {                                        \
        if (this->storage_only_scope() || region().base() != 0 ||                                  \
            region().size() != _regfw_map.size_bytes())                                            \
            throw ::reg::Error("REG_STATIC_LAYOUT belongs in the map's root bundle",               \
                               ::reg::ErrorKind::Configuration);                                   \
        REGFW_DETAIL_LAYOUT_BACKEND(RegBundleType, __VA_ARGS__);                                   \
    }

// Same composition vocabulary, selected explicitly within a compiled factory.
// Dynamic reg::view keeps its original return type and loop-assignment behavior.
namespace reg::compiled {
using namespace ::reg;
constexpr auto view(std::string_view name,
                    std::uint64_t size,
                    std::source_location at = std::source_location::current()) {
    return ::reg::static_view(name, size, at);
}
constexpr auto address_space(std::string_view name,
                             std::source_location at = std::source_location::current()) {
    return ::reg::static_address_space(name, at);
}
} // namespace reg::compiled
namespace reg::detail {
template <class Root, auto... M>
struct SymbolPath {
    using RootType = Root;
    template <auto Next>
    using Append = SymbolPath<Root, M..., Next>;
    static constexpr auto target(SymbolSelectors s)
        requires(sizeof...(M) > 0)
    {
        return StaticTarget<M...>{s.indices, s.count};
    }
};
template <bool Enable>
struct SymbolViewBuilder {};
template <>
struct SymbolViewBuilder<true> {
    constexpr auto view(std::string_view name,
                        std::uint64_t size,
                        std::source_location at = std::source_location::current()) const {
        return static_view(name, size, at);
    }
};
template <bool Enable>
struct SymbolSpaceBuilder {};
template <>
struct SymbolSpaceBuilder<true> {
    constexpr auto address_space(std::string_view name,
                                 std::source_location at = std::source_location::current()) const {
        return static_address_space(name, at);
    }
};
template <class B,
          class Path,
          class Sequence = std::make_index_sequence<automatic_member_count<B>()>>
struct SymbolObject;
template <class B, class Path, std::size_t... I>
    struct SymbolObject<B, Path, std::index_sequence<I...>> : SymbolViewBuilder < !requires {
    &B::view;
} >, SymbolSpaceBuilder<!requires { &B::address_space; }>,
    decltype(B::template _regfw_symbol_mixin<B, Path>(DeclarationIndex<I>{}))::type... {
    SymbolSelectors _regfw_selectors;
    constexpr explicit SymbolObject(SymbolSelectors s = {})
        : decltype(B::template _regfw_symbol_mixin<B, Path>(DeclarationIndex<I>{}))::type(s)...,
          _regfw_selectors(s) {
    }
    constexpr auto _regfw_target() const
        requires requires { Path::target(_regfw_selectors); }
    {
        return Path::target(_regfw_selectors);
    }
};
template <class B, class Path, std::size_t N>
struct SymbolArray {
    SymbolSelectors selectors;
    constexpr explicit SymbolArray(SymbolSelectors s) : selectors(s) {
    }
    constexpr auto operator[](std::size_t i) const {
        if (i >= N)
            throw "symbolic entity index out of range";
        auto s = selectors;
        if (s.count == MAX_REG_DIMS)
            throw "symbolic entity nesting exceeded";
        s.indices[s.count++] = static_cast<std::uint32_t>(i);
        return SymbolObject<B, Path>(s);
    }
    constexpr std::size_t size() const {
        return N;
    }
};
template <class Path>
struct SymbolRegister {
    SymbolSelectors selectors;
    constexpr explicit SymbolRegister(SymbolSelectors s) : selectors(s) {
    }
    constexpr auto _regfw_target() const {
        return Path::target(selectors);
    }
};
template <class Path, auto Member>
struct SymbolMemberValue {
    using V = typename StaticMemberPointer<decltype(Member)>::Value;
    using B = typename StaticRepeatType<V>::Value;
    using P = typename Path::template Append<Member>;
    static auto select() {
        if constexpr (StaticRepeatType<V>::count > 0)
            return std::type_identity<SymbolArray<B, P, StaticRepeatType<V>::count>>{};
        else if constexpr (std::derived_from<B, Bundle>)
            return std::type_identity<SymbolObject<B, P>>{};
        else
            return std::type_identity<SymbolRegister<P>>{};
    }
    using type = typename decltype(select())::type;
};
template <class Root, class F>
struct SymbolFactory {
    constexpr auto operator()() const {
        return F{}(SymbolObject<Root, SymbolPath<Root>>{});
    }
};
template <class Root>
template <class F>
DeclaredLayoutRoot<Root>::DeclaredLayoutRoot(F&& factory) {
    using Factory = std::remove_cvref_t<F>;
    if constexpr (std::invocable<F&>)
        dynamic = LayoutRoot(std::forward<F>(factory));
    else {
        static_assert(std::is_empty_v<Factory> && std::is_default_constructible_v<Factory>,
                      "compiled REG_LAYOUT requires a capture-free [](auto layout) factory");
        // Built once by native C++ on first use; no complete consteval plan.
        static const auto plan =
            build_formula_layout_runtime<Root, SymbolFactory<Root, Factory>{}>();
        install = [](RegisterMap& map, Root* root) {
            install_formula_layout(map, root, plan.view());
        };
    }
}
template <class Root>
void apply_declared_layout(RegisterMap& map, Root* root, const DeclaredLayoutSpec<Root>& spec) {
    if (spec.root.install) {
        if (spec.views.size() || spec.aliases.size() || spec.identities.size() ||
            spec.reserved.size())
            throw Error("compiled root cannot be mixed with legacy layout entries",
                        ErrorKind::Configuration);
        spec.root.install(map, root);
    } else
        apply_layout(
            map,
            LayoutSpec{
                spec.root.dynamic, spec.views, spec.aliases, spec.identities, spec.reserved});
}
} // namespace reg::detail
