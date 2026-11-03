#pragma once
#include <regfw/regfw.hpp>
namespace separate {
struct Registers : reg::RegEntityBundle {
    REG_BUNDLE_DECL(Registers);
    REG(TABLE, std::uint32_t, reg::at(0).array(reg::dims(4), reg::strides(4)));
    REG(WIDE, reg::u512, reg::at(64));
};
struct Root : reg::Bundle {
    REG_BUNDLE_DECL(Root);
    REG_ENTITY(regs, Registers);
    REG_LAYOUT_DECL();
};
struct Dynamic : reg::Bundle {
    REG_BUNDLE_DECL(Dynamic);
    REG_ENTITY(regs, Registers);
    REG_LAYOUT_DECL();
};
void other(reg::RegisterBank<Root>&);
} // namespace separate
