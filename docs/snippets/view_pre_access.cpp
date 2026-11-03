#include <regfw/regfw.hpp>
#include <iostream>
#include <stdexcept>
REG_PRE_ACCESS(CheckBusiness);
struct Regs : reg::RegEntityBundle {
    REG_BUNDLE(Regs);
    REG(VALUE, unsigned, reg::at(0));
};
struct Layout : reg::Bundle {
    REG_BUNDLE(Layout);
    REG_ENTITY(regs, Regs);
    REG_LAYOUT([&] {
        auto business = reg::view("BUSINESS", 0x100).map(regs).using_pre_access(CheckBusiness);
        return reg::address_space("DEVICE")
            .map(business, 0xff71f0000000ULL)
            .map(business, 0xc100000000ULL);
    });
};
int main() {
    reg::RegisterBank<Layout> bank(reg::space());
    unsigned calls = 0;
    bank.map().set_pre_access(CheckBusiness, [&](const reg::RegAccessInfo& info) {
        ++calls;
        if (info.view_path.empty())
            throw std::runtime_error("missing selected view path");
    });
    bank.finish();
    if (!bank.map().mmio_write(0xff71f0000000ULL, 42).success() ||
        bank.map().mmio_read(0xc100000000ULL) != 42 || calls != 2)
        throw std::runtime_error("View pre_access / alias check failed");
    std::cout << "PASS: one View hook, two high-address aliases\n";
}
