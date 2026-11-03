#include "internal/runtime_impl.hpp"
#include <sstream>
#include <iomanip>
#include <unordered_set>

namespace reg::detail {
FieldBehavior make_behavior(reg::Access a) {
    return {[a](auto c, auto v, auto m) {
                return apply_write(a, c, v, m);
            },
            [a](auto& s, auto m) {
                return apply_read(a, s, m);
            }};
}
FieldBehavior make_behavior(FieldBehavior a) {
    if (!a.write || !a.read)
        throw Error("empty custom behavior", ErrorKind::Configuration);
    return a;
}
WideFieldBehavior make_wide_behavior(reg::Access access) {
    // Built-in bitwise semantics compose independently across all limbs.
    return {[access](WideRegData current, WideRegData value, WideRegData mask) {
                WideRegData out;
                for (unsigned w = 0; w < WideRegData::WORDS; ++w)
                    out.set_word(w,
                                 apply_write(access, current.word(w), value.word(w), mask.word(w)));
                return out;
            },
            [access](WideRegData& stored, WideRegData mask) {
                WideRegData out;
                for (unsigned w = 0; w < WideRegData::WORDS; ++w) {
                    auto v = stored.word(w);
                    out.set_word(w, apply_read(access, v, mask.word(w)));
                    stored.set_word(w, v);
                }
                return out;
            }};
}
WideFieldBehavior make_wide_behavior(WideFieldBehavior behavior) {
    if (!behavior.write || !behavior.read)
        throw Error("empty wide behavior", ErrorKind::Configuration);
    return behavior;
}
FieldBehavior make_mixed_behavior(std::span<const AccessRule> input) {
    auto rules = std::make_shared<const std::vector<AccessRule>>(input.begin(), input.end());
    return {[rules](RegData c, RegData v, RegData m) {
                RegData out = c;
                for (auto x : *rules) {
                    auto mask = m & x.mask;
                    if (mask)
                        out = (out & ~mask) | (apply_write(x.behavior, c, v, mask) & mask);
                }
                return out;
            },
            [rules](RegData& s, RegData m) {
                RegData out = 0, covered = 0;
                for (auto x : *rules) {
                    auto mask = m & x.mask;
                    if (mask)
                        out |= apply_read(x.behavior, s, mask) & mask;
                    covered |= x.mask;
                }
                return out | (s & m & ~covered);
            }};
}
WideFieldBehavior make_wide_mixed_behavior(std::span<const WideAccessRule> input) {
    std::vector<std::pair<WideRegData, WideFieldBehavior>> rules;
    WideRegData covered;
    for (auto r : input) {
        if (bool(covered & r.mask))
            throw Error("overlapping wide mixed masks", ErrorKind::Configuration);
        covered |= r.mask;
        rules.emplace_back(r.mask, detail::make_wide_behavior(r.behavior));
    }
    auto shared = std::make_shared<const decltype(rules)>(std::move(rules));
    return {[shared](WideRegData current, WideRegData value, WideRegData mask) {
                auto out = current;
                for (auto& [m, b] : *shared) {
                    auto effective = m & mask;
                    if (bool(effective)) {
                        auto part = b.write(current, value, effective);
                        out = (out & ~effective) | (part & effective);
                    }
                }
                return out;
            },
            [shared](WideRegData& stored, WideRegData mask) {
                WideRegData out, covered_bits;
                for (auto& [m, b] : *shared) {
                    auto effective = m & mask;
                    if (bool(effective))
                        out |= b.read(stored, effective) & effective;
                    covered_bits |= m;
                }
                return out | (stored & (mask & ~covered_bits));
            }};
}
} // namespace reg::detail
namespace reg::behaviors {
FieldBehavior rw() {
    return detail::make_behavior(reg::rw);
}
FieldBehavior ro() {
    return detail::make_behavior(reg::ro);
}
FieldBehavior wc() {
    return detail::make_behavior(reg::wc);
}
FieldBehavior w1c() {
    return detail::make_behavior(reg::w1c);
}
FieldBehavior w1s() {
    return detail::make_behavior(reg::w1s);
}
FieldBehavior w0c() {
    return detail::make_behavior(reg::w0c);
}
FieldBehavior w0s() {
    return detail::make_behavior(reg::w0s);
}
FieldBehavior rc() {
    return detail::make_behavior(reg::rc);
}
FieldBehavior write_only() {
    return detail::make_behavior(reg::write_only);
}
FieldBehavior normal_read() {
    return rw();
}
FieldBehavior cnt_cyc() {
    return ro();
}
FieldBehavior w1_pulse() {
    return detail::make_behavior(reg::w1_pulse);
}
FieldBehavior rwsc() {
    return rw();
}
FieldBehavior rwsc_immediate() {
    return detail::make_behavior(reg::rwsc_immediate);
}
FieldBehavior int_wc() {
    return w1c();
}
FieldBehavior mixed(std::vector<Rule> rules) {
    RegData covered = 0;
    for (const auto& r : rules) {
        if (covered & r.mask)
            throw Error("overlapping mixed masks", ErrorKind::Configuration);
        covered |= r.mask;
    }
    auto p = std::make_shared<const std::vector<Rule>>(std::move(rules));
    return {[p](RegData c, RegData v, RegData m) {
                RegData out = c;
                for (const auto& r : *p) {
                    auto mask = m & r.mask;
                    if (mask)
                        out = (out & ~mask) | (r.behavior.write(c, v, mask) & mask);
                }
                return out;
            },
            [p](RegData& s, RegData m) {
                RegData out = 0, covered_mask = 0;
                for (const auto& r : *p) {
                    auto mask = m & r.mask;
                    if (mask)
                        out |= r.behavior.read(s, mask) & mask;
                    covered_mask |= r.mask;
                }
                return out | (s & m & ~covered_mask);
            }};
}
} // namespace reg::behaviors

namespace reg::detail {
RegisterMetadata node_metadata(const EntityState& n) {
    return {n.name,
            n.layout().elements.front().address,
            n.value_size * 8,
            n.reset_value,
            n.writable_mask,
            n.dims,
            n.layout().strides,
            n.reset_domains,
            n.reset_scope,
            n.widths,
            n.log_policy,
            n.snapshot,
            n.wide ? n.wide->wide_reset_value : WideRegData{},
            n.wide ? n.wide->wide_writable_mask : ~WideRegData{},
            n.transaction_policy};
}
} // namespace reg::detail
