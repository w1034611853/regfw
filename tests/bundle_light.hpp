#pragma once
// Device-header pattern for the light declaration entry: consumers include
// only <regfw/bundle.hpp>. The layout definition lives in bundle_light_impl.cpp,
// which includes the full <regfw/regfw.hpp> entry.
#include <regfw/bundle.hpp>
namespace light {
REG_ACCESS_FIELD(Route, std::uint32_t);
struct VfRegs : reg::RegEntityBundle {
    REG_BUNDLE_DECL(VfRegs);
    REG(CTL, std::uint32_t, reg::at(0));
    REG(DATA, std::uint32_t, reg::at(8));
};
struct Device : reg::Bundle {
    REG_BUNDLE_DECL(Device);
    REG_ENTITY(pf, VfRegs);
    REG_REPEAT_ENTITY(vf, VfRegs, 4);
    REG_LAYOUT_DECL();
};
// Defined in bundle_light_impl.cpp (full entry) and bundle_light_consumer.cpp.
void poke(reg::RegisterBank<Device>& bank, std::uint64_t address, std::uint32_t value);
bool read(reg::RegisterBank<Device>& bank, std::uint64_t address, std::uint32_t& value);
void arm_callbacks(reg::RegisterBank<Device>& bank, std::uint32_t& observed);
} // namespace light
