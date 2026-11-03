// Golden decode test for the formula backend.  The table was produced by the
// frozen 0.34.0 builder before the legacy backend was removed.
// It pins absolute address -> binding/linear/byte/available/scope/identity/
// route/policy/flow for a layout with wide registers, dense/sparse/monotone/
// interleaved arrays, repeated identity windows and a PF-shared alias.
#include <regfw/regfw.hpp>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!(__VA_ARGS__))                                                                        \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " +      \
                                     #__VA_ARGS__);                                                \
    } while (false)
namespace {
constexpr std::uint64_t A = 0xff71f0000000ULL, B = 0xc100000000ULL, C = 0xc200000000ULL;
REG_ACCESS_FIELD(Route, std::uint32_t);
REG_ACCESS_FIELD(Logical, std::uint32_t);
REG_ACCESS_FIELD(Physical, std::uint32_t);
REG_PRE_ACCESS(Gate);
struct GoldenWord {
    std::uint64_t address, target;
    std::uint32_t binding, linear, byte, available, scope, identity, flow, route_kind,
        route_instance, allow_read, allow_write, denied_read, denied_write;
};
#include "golden/battery_words.inc"

struct BasicRegs : reg::RegEntityBundle {
    REG_BUNDLE(BasicRegs);
    REG(R8, std::uint8_t, reg::at(0));
    REG(R16, std::uint16_t, reg::at(4));
    REG(RW, std::uint32_t, reg::at(0x40));
    REG(MASK, std::uint32_t, reg::at(0x44).mask(0x00ff00ff));
    REG(W64, std::uint64_t, reg::at(0x80));
    REG(BIG, reg::u512, reg::at(0x100).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
    REG(SLOW, std::uint32_t, reg::at(0x200).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
    REG(WIDE2048, reg::u2048, reg::at(0x300).transaction(reg::TransactionPolicy::HIGH_TO_LOW));
};
struct GetGroupRegs : reg::RegEntityBundle {
    REG_BUNDLE(GetGroupRegs);
    REG(GGCTL, std::uint32_t, reg::at(0));
    REG(GGSLOT, reg::u512, reg::at(0x1000).array(reg::dims(4, 8), reg::strides(0x20000, 0x100)));
};
struct InterleavedRegs : reg::RegEntityBundle {
    REG_BUNDLE(InterleavedRegs);
    REG(M, reg::u512, reg::at(0).array(reg::dims(2, 3), reg::strides(0x180, 0x100)));
};
struct VfRegs : reg::RegEntityBundle {
    REG_BUNDLE(VfRegs);
    REG(TABLE, std::uint32_t, reg::at(0).array(reg::dims(2, 4), reg::strides(16, 4)));
    REG(RC, std::uint32_t, reg::at(0x3c).access(reg::rc));
    REG(STAT, std::uint32_t, reg::at(0x80).array(reg::dims(64), reg::strides(4)));
    REG_ACCESS_GROUP(MAP,
                     .members = {TABLE},
                     .coordinates = {{Logical, 1}},
                     .derive = {reg::map_index(Logical, Physical).using_resolver(Physical)});
};
struct SharedRegs : reg::RegEntityBundle {
    REG_BUNDLE(SharedRegs);
    REG(SHARED, std::uint32_t, reg::at(32));
};
struct SparseRegs : reg::RegEntityBundle {
    REG_BUNDLE(SparseRegs);
    REG(LUT, std::uint32_t, reg::at(0).array(reg::dims(16), reg::strides(0x100)));
    REG(TBL, std::uint32_t, reg::at(0x10).array(reg::dims(8, 2), reg::strides(4, 0x20)));
};
template <int Mode>
struct Battery : reg::Bundle {
    REG_BUNDLE(Battery);
    REG_ENTITY(basic, BasicRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_ENTITY(get_group, GetGroupRegs);
    REG_ENTITY(inter, InterleavedRegs);
    REG_REPEAT_ENTITY(vf, VfRegs, 4, reg::entity().reset_per_instance(reg::ResetScope::Vf));
    REG_ENTITY(shared, SharedRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_ENTITY(sparse, SparseRegs);
};
constexpr auto battery_factory = [] {
    auto vfwin = [&](auto i) {
        return reg::static_view("VF", 0x200)
            .identity(1)
            .using_resolver(Route)
            .using_pre_access(Gate)
            .vf_to_pf()
            .map(reg::target<&Battery<1>::vf>().at(i))
            .map(reg::target<&Battery<1>::shared>());
    };
    return reg::static_address_space("BAT")
        .map(reg::static_view("BASIC", 0x400).map(reg::target<&Battery<1>::basic>()), A)
        .map(reg::static_view("GG", 0x100000).map(reg::target<&Battery<1>::get_group>()), B)
        .map(reg::static_view("INTER", 0x400).map(reg::target<&Battery<1>::inter>()), C)
        .map(reg::static_view("SHARED_TOP", 0x40).map(reg::target<&Battery<1>::shared>()),
             A + 0x1000)
        .map_repeat("VFS", A + 0x100000, reg::repeat(4).stride(0x400), vfwin)
        .map_repeat("VFS2", B + 0x100000, reg::repeat(4).stride(0x1000), vfwin)
        .map(reg::static_view("SPARSE", 0x1000)
                 .map(reg::target<&Battery<1>::sparse>())
                 .reserve(0x804, 0xF8),
             C + 0x1000);
};

constexpr bool same_policy(const reg::AliasAccessPolicy& x, const reg::AliasAccessPolicy& y) {
    return x.allow_read == y.allow_read && x.allow_write == y.allow_write &&
           x.denied_read == y.denied_read && x.denied_write == y.denied_write;
}
constexpr reg::AliasAccessPolicy merge_policy(const reg::AliasAccessPolicy& a,
                                              const reg::AliasAccessPolicy& b) {
    auto action =
        [](bool a_allow, reg::DeniedAccessAction a, bool b_allow, reg::DeniedAccessAction c) {
            if (!a_allow && !b_allow)
                return a == reg::DeniedAccessAction::Error || c == reg::DeniedAccessAction::Error
                           ? reg::DeniedAccessAction::Error
                           : reg::DeniedAccessAction::Ignore;
            return !a_allow ? a : c;
        };
    return {a.allow_read && b.allow_read,
            a.allow_write && b.allow_write,
            action(a.allow_read, a.denied_read, b.allow_read, b.denied_read),
            action(a.allow_write, a.denied_write, b.allow_write, b.denied_write)};
}
template <class Scopes>
constexpr int scope_compare(const Scopes& scopes, std::uint32_t a, std::uint32_t b) {
    constexpr auto none = reg::detail::STATIC_NONE;
    if (a == b)
        return 0;
    std::array<std::uint32_t, 64> x{}, y{};
    unsigned nx = 0, ny = 0;
    for (auto p = a; p != none; p = scopes[p].parent) {
        if (nx == x.size())
            return 0;
        x[nx++] = p;
    }
    for (auto p = b; p != none; p = scopes[p].parent) {
        if (ny == y.size())
            return 0;
        y[ny++] = p;
    }
    unsigned k = 0;
    while (k < std::min(nx, ny) && x[nx - 1 - k] == y[ny - 1 - k])
        ++k;
    if (k == nx || k == ny)
        return nx < ny ? -1 : nx > ny ? 1 : 0;
    const auto xp = scopes[x[nx - 1 - k]].priority, yp = scopes[y[ny - 1 - k]].priority;
    return xp < yp ? -1 : xp > yp ? 1 : 0;
}
const char* check_golden() {
    const auto formula = reg::detail::build_formula_layout_runtime<Battery<1>, battery_factory>();
    constexpr auto none = reg::detail::STATIC_NONE;
    std::array<std::uint32_t, 2048> identities_for{};
    for (std::size_t id = 0; id < formula.scopes.size(); ++id) {
        identities_for[id] =
            formula.scopes[id].parent != none ? identities_for[formula.scopes[id].parent] : none;
        const auto& s = formula.scopes[id];
        if (!s.domain)
            continue;
        const auto stride = s.repeat_stride ? s.repeat_stride : s.size;
        const auto base = s.base - s.repeat_index * stride;
        std::uint32_t index = 0;
        for (; index < formula.identities.size(); ++index) {
            const auto& v = formula.identities[index];
            if (v.base == base && v.count == s.repeat_count && v.stride == stride &&
                v.size == s.size && v.domain == *s.domain && v.resolver_type == s.resolver_type)
                break;
        }
        if (index == formula.identities.size())
            return "identity family missing";
        identities_for[id] = index;
    }
    std::array<reg::AliasAccessPolicy, 2048> effective{};
    std::array<bool, 2048> effective_pre{};
    for (std::size_t id = 0; id < formula.scopes.size(); ++id) {
        const auto& s = formula.scopes[id];
        effective[id] = s.parent == none ? s.policy : merge_policy(s.policy, effective[s.parent]);
        effective_pre[id] = !s.pre_type.empty() || (s.parent != none && effective_pre[s.parent]);
    }
    for (const auto& w : kBatteryGolden) {
        const reg::detail::FormulaMountSpec* winner = nullptr;
        std::uint32_t linear = 0, byte = 0;
        for (const auto& m : formula.mounts) {
            if (w.address < m.base || w.address - m.base >= m.end - m.base)
                continue;
            std::uint32_t li = 0, by = 0;
            if (!reg::detail::formula_decode_offset(
                    formula.arrays[m.array], w.address - m.base, li, by))
                continue;
            if (!winner || scope_compare(formula.scopes, m.scope, winner->scope) > 0) {
                winner = &m;
                linear = li;
                byte = by;
            }
        }
        if (!winner)
            return "golden address does not decode";
        if (winner->binding != w.binding)
            return "binding";
        if (linear != w.linear)
            return "linear";
        if (byte != w.byte)
            return "byte";
        if (formula.bindings[winner->binding].canonical_base + (w.address - winner->base) !=
            w.target)
            return "canonical target";
        if (formula.arrays[winner->array].occupied - byte != w.available)
            return "available";
        if (winner->scope != w.scope)
            return "winner scope";
        const auto& s = formula.scopes[winner->scope];
        if (w.route_kind != (unsigned)s.route || w.route_instance != s.instance.value_or(0))
            return "route";
        if (!same_policy(effective[winner->scope],
                         reg::AliasAccessPolicy{!!w.allow_read,
                                                !!w.allow_write,
                                                (reg::DeniedAccessAction)w.denied_read,
                                                (reg::DeniedAccessAction)w.denied_write})) {
            std::fprintf(stderr,
                         "policy mismatch addr=%llx scope=%u got=%d/%d want=%d/%d\n",
                         (unsigned long long)w.address,
                         w.scope,
                         effective[winner->scope].allow_read,
                         effective[winner->scope].allow_write,
                         w.allow_read,
                         w.allow_write);
            return "policy";
        }
        if (identities_for[winner->scope] != w.identity)
            return "identity";
        const auto flow = w.identity != none ? reg::detail::StaticFlow::Identity
                          : winner->scalar && !effective_pre[winner->scope]
                              ? reg::detail::StaticFlow::Scalar32
                              : reg::detail::StaticFlow::Generic;
        if ((unsigned)flow != w.flow)
            return "flow";
    }
    return nullptr;
}
} // namespace
int main() {
    if (const char* reason = check_golden()) {
        std::fprintf(stderr, "formula golden mismatch: %s\n", reason);
        return 1;
    }
    std::printf("formula golden OK (%zu words)\n", std::size(kBatteryGolden));
    return 0;
}
