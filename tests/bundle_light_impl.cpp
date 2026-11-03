#include "bundle_light.hpp"
#include <regfw/regfw.hpp>
namespace light {
REG_BUNDLE_IMPL(VfRegs);
REG_BUNDLE_IMPL(Device);
REG_LAYOUT_IMPL(Device, [](auto layout) {
    namespace reg = ::reg::compiled;
    return reg::address_space("DEV")
        .map(reg::view("PF", 0x100).map(layout.pf), 0x1000)
        .map_repeat("VFS", 0x200000, reg::repeat(4).stride(0x2000), [&](auto i) {
            return reg::view("WIN", 0x2000)
                .identity(1)
                .using_resolver(Route)
                .vf_to_pf()
                .map(reg::view("REGS", 0x100).map(layout.vf[i]), 0x100);
        });
});
void poke(reg::RegisterBank<Device>& bank, std::uint64_t address, std::uint32_t value) {
    bank.map().mmio_write(address, value);
}
bool read(reg::RegisterBank<Device>& bank, std::uint64_t address, std::uint32_t& value) {
    auto r = bank.map().try_mmio_read_sized(address, 4);
    value = static_cast<std::uint32_t>(r.value);
    return r.result == reg::RegMemTxResult::Ok;
}
} // namespace light
