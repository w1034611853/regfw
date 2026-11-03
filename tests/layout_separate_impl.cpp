#include "layout_separate.hpp"
namespace separate {
REG_BUNDLE_IMPL(Registers);
REG_BUNDLE_IMPL(Root);
REG_BUNDLE_IMPL(Dynamic);
REG_LAYOUT_IMPL(Root, [](auto layout) {
    namespace reg = ::reg::compiled;
    auto v = reg::view("REGS", 128).map(layout.regs);
    return reg::address_space("SPLIT").map(v, 0xc100000000ULL).map(v, 0xff71f0000000ULL);
});
REG_LAYOUT_IMPL(Dynamic, [&] {
    auto v = reg::view("REGS", 128).map(regs);
    return reg::address_space("SPLIT").map(v, 0xc100000000ULL).map(v, 0xff71f0000000ULL);
});
} // namespace separate
