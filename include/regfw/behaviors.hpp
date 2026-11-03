#pragma once
#include "types.hpp"
namespace reg {
enum class Access { Rw, Ro, W1c, W1s, W0c, W0s, Rc, WriteOnly, W1Pulse, Rwsc, RwscImmediate };
inline constexpr Access rw = Access::Rw, ro = Access::Ro, wc = Access::W1c, w1c = Access::W1c,
                        w1s = Access::W1s, w0c = Access::W0c, w0s = Access::W0s, rc = Access::Rc,
                        write_only = Access::WriteOnly, w1_pulse = Access::W1Pulse,
                        rwsc = Access::Rwsc, rwsc_immediate = Access::RwscImmediate,
                        int_wc = Access::W1c, normal_read = Access::Rw, cnt_cyc = Access::Ro;
namespace detail {
constexpr RegData apply_write(Access a, RegData current, RegData value, RegData mask) {
    switch (a) {
        case Access::Rw:
        case Access::WriteOnly:
        case Access::Rwsc:
            return (current & ~mask) | (value & mask);
        case Access::W1c:
            return current & ~(value & mask);
        case Access::W1s:
            return current | (value & mask);
        case Access::W0c:
            return current & (value | ~mask);
        case Access::W0s:
            return current | (~value & mask);
        case Access::RwscImmediate:
            return current & ~mask;
        default:
            return current;
    }
}
} // namespace detail

namespace detail {
constexpr RegData apply_read(Access a, RegData& stored, RegData mask) {
    if (a == Access::WriteOnly)
        return 0;
    auto value = stored & mask;
    if (a == Access::Rc)
        stored &= ~mask;
    return value;
}
} // namespace detail

struct AccessRule {
    RegData mask;
    Access behavior;
};
constexpr AccessRule rule(RegData mask, Access behavior) {
    return {mask, behavior};
}
template <std::size_t N>
struct MixedAccess {
    std::array<AccessRule, N> rules;
};
template <class... R>
constexpr auto mixed(R... r) {
    MixedAccess<sizeof...(R)> out{{r...}};
    RegData covered = 0;
    for (auto x : out.rules) {
        if (covered & x.mask)
            throw Error("overlapping mixed behavior masks", ErrorKind::Configuration);
        covered |= x.mask;
    }
    return out;
}
struct FieldBehavior {
    std::function<RegData(RegData, RegData, RegData)> write;
    std::function<RegData(RegData&, RegData)> read;
};
using WriteBehavior = std::function<RegData(RegData, RegData, RegData)>;
using ReadBehavior = std::function<RegData(RegData&, RegData)>;
namespace detail {
FieldBehavior make_behavior(Access a);
}

namespace detail {
FieldBehavior make_mixed_behavior(std::span<const AccessRule> rules);
template <std::size_t N>
FieldBehavior make_behavior(const MixedAccess<N>& a) {
    return make_mixed_behavior(a.rules);
}

} // namespace detail

namespace detail {
FieldBehavior make_behavior(FieldBehavior a);
}

namespace behaviors {
FieldBehavior rw();
FieldBehavior ro();
FieldBehavior wc();
FieldBehavior w1c();
FieldBehavior w1s();
FieldBehavior w0c();
FieldBehavior w0s();
FieldBehavior rc();
FieldBehavior write_only();
FieldBehavior normal_read();
FieldBehavior cnt_cyc();
FieldBehavior w1_pulse();
FieldBehavior rwsc();
FieldBehavior rwsc_immediate();
FieldBehavior int_wc();
struct Rule {
    RegData mask;
    FieldBehavior behavior;
};
FieldBehavior mixed(std::vector<Rule> rules);
} // namespace behaviors
// namespace behaviors
template <std::size_t R>
struct Dimensions {
    std::array<std::size_t, R> values;
};
template <std::size_t R>
struct Strides {
    std::array<std::uint64_t, R> values;
};
template <class... T>
constexpr auto dims(T... n) {
    static_assert(sizeof...(T) > 0 && sizeof...(T) <= MAX_REG_DIMS);
    return Dimensions<sizeof...(T)>{{static_cast<std::size_t>(n)...}};
}
template <class... T>
constexpr auto strides(T... n) {
    return Strides<sizeof...(T)>{{static_cast<std::uint64_t>(n)...}};
}
struct WideFieldBehavior {
    std::function<WideRegData(WideRegData, WideRegData, WideRegData)> write;
    std::function<WideRegData(WideRegData&, WideRegData)> read;
};
struct WideAccessRule {
    WideRegData mask;
    Access behavior;
};
constexpr WideAccessRule rule(WideRegData mask, Access behavior) {
    return {mask, behavior};
}
template <std::size_t N>
struct WideMixedPlan {
    std::array<WideAccessRule, N> rules;
};
template <class... T>
    requires((std::same_as<T, WideAccessRule>) && ...)
constexpr auto mixed(WideAccessRule first, T... rest) {
    WideMixedPlan<1 + sizeof...(T)> out{{first, rest...}};
    WideRegData covered;
    for (auto r : out.rules) {
        if (bool(covered & r.mask))
            throw std::invalid_argument("overlapping wide behavior masks");
        covered |= r.mask;
    }
    return out;
}
namespace detail {
WideFieldBehavior make_wide_behavior(Access access);
}

namespace detail {
WideFieldBehavior make_wide_behavior(WideFieldBehavior behavior);
}

namespace detail {
WideFieldBehavior make_wide_mixed_behavior(std::span<const WideAccessRule> rules);
template <std::size_t N>
WideFieldBehavior make_wide_behavior(const WideMixedPlan<N>& plan) {
    return make_wide_mixed_behavior(plan.rules);
}

} // namespace detail

namespace detail {
template <std::size_t N>
WideFieldBehavior make_wide_behavior(MixedAccess<N> plan) {
    WideMixedPlan<N> wide{};
    for (std::size_t i = 0; i < N; ++i)
        wide.rules[i] = {WideRegData(plan.rules[i].mask), plan.rules[i].behavior};
    return detail::make_wide_behavior(wide);
}
} // namespace detail

namespace detail {
// A scalar reset or all-bits mask does not need 32 limbs during every
// constexpr DSL evaluation. The wide union member is activated only when
// the declaration actually supplies a wide value.
class RegInitValue {
    enum class Kind { Scalar, Wide, All };
    union Payload {
        RegData scalar;
        WideRegData wide;
        constexpr Payload(RegData value = 0) : scalar(value) {
        }
        constexpr Payload(WideRegData value) : wide(value) {
        }
    } value_;
    Kind kind_{Kind::Scalar};

  public:
    constexpr RegInitValue() = default;
    template <std::integral U>
    constexpr RegInitValue(U value) : value_(static_cast<RegData>(value)) {
        if constexpr (std::is_signed_v<U>)
            if (value < 0)
                throw std::out_of_range("negative wide unsigned value");
    }
    template <unsigned Bits>
    constexpr RegInitValue(const WideUInt<Bits>& value)
        : value_(WideRegData(value)), kind_(Kind::Wide) {
    }
    static constexpr RegInitValue all() {
        RegInitValue value;
        value.kind_ = Kind::All;
        return value;
    }
    constexpr RegData low_u64() const {
        return kind_ == Kind::All    ? ~RegData{}
               : kind_ == Kind::Wide ? value_.wide.low_u64()
                                     : value_.scalar;
    }
    constexpr operator WideRegData() const {
        return kind_ == Kind::All    ? ~WideRegData{}
               : kind_ == Kind::Wide ? value_.wide
                                     : WideRegData(value_.scalar);
    }
};
} // namespace detail

template <std::size_t R = 1, class B = Access, bool IsArray = false>
struct RegisterSpec {
    static_assert(R >= 1 && R <= MAX_REG_DIMS, "register rank must be 1..MAX_REG_DIMS");
    static_assert(IsArray || R == 1, "multidimensional storage requires array declaration");
    template <std::size_t, class, bool>
    friend struct RegisterSpec;
    static constexpr bool is_array = IsArray;
    static constexpr std::size_t rank = R;
    std::source_location location{};
    std::uint64_t offset{};
    detail::RegInitValue reset_value{}, writable_mask = detail::RegInitValue::all();
    B behavior{};
    std::array<std::size_t, R> dimensions{};
    std::array<std::uint64_t, R> stride_bytes{};
    ResetDomains domains{ResetDomains::STANDARD};
    ResetScope scope{};
    bool explicit_scope{};
    RegisterAccessWidthPolicy widths{};
    std::optional<TransactionPolicy> transaction_policy;
    LogPolicy log_policy{LogPolicy::Normal};
    SnapshotKind snapshot_kind{SnapshotKind::Migratable};
    std::string_view stable_id{};
    std::optional<std::uint32_t> order;
    bool initial_reset{true};
    constexpr explicit RegisterSpec(std::uint64_t address = 0,
                                    std::source_location at = std::source_location::current())
        : location(at), offset(address) {
        dimensions.fill(1);
    }
    constexpr auto reset(detail::RegInitValue v) const {
        auto s = *this;
        s.reset_value = v;
        return s;
    }
    constexpr auto mask(detail::RegInitValue v) const {
        auto s = *this;
        s.writable_mask = v;
        return s;
    }
    template <class U>
    constexpr auto access(U b) const {
        auto s = copy_shape<R, U>();
        s.behavior = b;
        return s;
    }
    template <std::size_t N>
    constexpr auto array(Dimensions<N> d, Strides<N> st) const {
        auto s = copy_shape<N, B, true>();
        s.dimensions = d.values;
        s.stride_bytes = st.values;
        return s;
    }
    constexpr auto transaction(TransactionPolicy p) const {
        auto s = *this;
        s.transaction_policy = p;
        return s;
    }
    constexpr auto reset_domains(ResetDomains p) const {
        auto s = *this;
        s.domains = p;
        return s;
    }
    constexpr auto reset_scope(ResetScope p) const {
        auto s = *this;
        s.scope = p;
        s.explicit_scope = true;
        return s;
    }
    constexpr auto access_widths(RegisterAccessWidthPolicy p) const {
        auto s = *this;
        s.widths = p;
        return s;
    }
    constexpr auto log(LogPolicy p) const {
        auto s = *this;
        s.log_policy = p;
        return s;
    }
    constexpr auto snapshot(SnapshotKind p) const {
        auto s = *this;
        s.snapshot_kind = p;
        return s;
    }
    constexpr auto snapshot_stable_id(std::string_view p) const {
        auto s = *this;
        s.stable_id = p;
        return s;
    }
    constexpr auto snapshot_order(std::uint32_t p) const {
        auto s = *this;
        s.order = p;
        return s;
    }
    constexpr auto defer_initial_reset() const {
        auto s = *this;
        s.initial_reset = false;
        return s;
    }

  private:
    template <std::size_t N, class U, bool A = IsArray>
    constexpr RegisterSpec<N, U, A> copy_shape() const {
        RegisterSpec<N, U, A> s(offset, location);
        s.reset_value = reset_value;
        s.writable_mask = writable_mask;
        if constexpr (std::same_as<U, B>)
            s.behavior = behavior;
        if constexpr (N == R) {
            s.dimensions = dimensions;
            s.stride_bytes = stride_bytes;
        }
        s.domains = domains;
        s.scope = scope;
        s.explicit_scope = explicit_scope;
        s.widths = widths;
        s.transaction_policy = transaction_policy;
        s.log_policy = log_policy;
        s.snapshot_kind = snapshot_kind;
        s.stable_id = stable_id;
        s.order = order;
        s.initial_reset = initial_reset;
        return s;
    }
};
constexpr auto at(std::uint64_t address,
                  std::source_location location = std::source_location::current()) {
    return RegisterSpec<>(address, location);
}
namespace detail {
struct RegisterConfig {
    std::uint64_t offset;
    const RegInitValue *reset_value, *writable_mask;
    std::span<const std::size_t> dimensions;
    std::span<const std::uint64_t> stride_bytes;
    ResetDomains domains;
    ResetScope scope;
    bool explicit_scope;
    RegisterAccessWidthPolicy widths;
    std::optional<TransactionPolicy> transaction_policy;
    LogPolicy log_policy;
    SnapshotKind snapshot_kind;
    std::string_view stable_id;
    std::optional<std::uint32_t> order;
    bool initial_reset, is_array;
    Access builtin{};
    const void* custom_behavior{};
    FieldBehavior (*scalar_factory)(const void*){};
    WideFieldBehavior (*wide_factory)(const void*){};
    std::source_location location{};
};
template <bool Wide, std::size_t R, class B, bool A>
RegisterConfig register_config(const RegisterSpec<R, B, A>& s) {
    RegisterConfig out{s.offset,
                       &s.reset_value,
                       &s.writable_mask,
                       s.dimensions,
                       s.stride_bytes,
                       s.domains,
                       s.scope,
                       s.explicit_scope,
                       s.widths,
                       s.transaction_policy,
                       s.log_policy,
                       s.snapshot_kind,
                       s.stable_id,
                       s.order,
                       s.initial_reset,
                       A};
    out.location = s.location;
    if constexpr (std::same_as<B, Access>)
        out.builtin = s.behavior;
    else {
        out.custom_behavior = &s.behavior;
        if constexpr (Wide)
            out.wide_factory = +[](const void* p) {
                return make_wide_behavior(*static_cast<const B*>(p));
            };
        else
            out.scalar_factory = +[](const void* p) {
                return make_behavior(*static_cast<const B*>(p));
            };
    }
    return out;
}
} // namespace detail
struct PhysicalRegisterAddress {
    std::uint64_t base_offset{};
    unsigned rank{1};
    std::array<std::size_t, MAX_REG_DIMS> dimensions{};
    std::array<std::uint64_t, MAX_REG_DIMS> stride_bytes{};
    template <std::size_t R, class B, bool A>
    constexpr explicit PhysicalRegisterAddress(const RegisterSpec<R, B, A>& s)
        : base_offset(s.offset), rank(R) {
        for (std::size_t i = 0; i < R; ++i) {
            dimensions[i] = s.dimensions[i];
            stride_bytes[i] = s.stride_bytes[i];
        }
    }
    constexpr std::uint64_t offset_at(std::span<const std::size_t> indices) const {
        if (indices.size() != rank)
            throw std::out_of_range("physical address rank mismatch");
        auto out = base_offset;
        for (std::size_t i = 0; i < rank; ++i) {
            if (indices[i] >= dimensions[i])
                throw std::out_of_range("physical address index");
            if (indices[i] && stride_bytes[i] > (UINT64_MAX - out) / indices[i])
                throw std::overflow_error("physical address overflow");
            out += indices[i] * stride_bytes[i];
        }
        return out;
    }
    template <class... I>
    constexpr auto offset(I... i) const {
        std::array<std::size_t, sizeof...(I)> a{static_cast<std::size_t>(i)...};
        return offset_at(a);
    }
};
struct PhysicalAddressWindow {
    std::uint64_t base{}, size{};
    template <class... I>
    constexpr auto addr(const PhysicalRegisterAddress& reg, I... i) const {
        auto offset = [&]() {
            if constexpr (sizeof...(I) == 0)
                return reg.offset(0);
            else
                return reg.offset(i...);
        }();
        if (offset >= size || base > UINT64_MAX - offset)
            throw std::out_of_range("physical window address");
        return base + offset;
    }
    constexpr auto child(std::uint64_t offset, std::uint64_t n) const {
        if (offset > size || n > size - offset || base > UINT64_MAX - offset)
            throw std::out_of_range("physical child window");
        return PhysicalAddressWindow{base + offset, n};
    }
    constexpr auto child(PhysicalAddressWindow w) const {
        return child(w.base, w.size);
    }
};
struct PhysicalRepeatAddressWindow {
    std::uint64_t base{}, count{}, stride{}, size{};
    constexpr auto instance(std::size_t n) const {
        if (n >= count || (n && stride > (UINT64_MAX - base) / n))
            throw std::out_of_range("physical instance");
        return PhysicalAddressWindow{base + n * stride, size};
    }
    template <class... I>
    constexpr auto addr(std::size_t n, const PhysicalRegisterAddress& r, I... i) const {
        return instance(n).addr(r, i...);
    }
};
} // namespace reg
// namespace reg
