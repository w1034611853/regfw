// DOC-BEGIN address-view-example
#include <regfw/regfw.hpp>
#include <iostream>
#include <stdexcept>

constexpr std::uint64_t BASE_A = 0xff71f0000000ULL;
constexpr std::uint64_t BASE_B = 0xc100000000ULL;
constexpr std::uint64_t VF_STRIDE = 0x200000;
constexpr unsigned VF_COUNT = 8;
REG_ACCESS_FIELD(VfRoute, std::uint32_t);

struct PfRegs : reg::RegEntityBundle {
    REG_BUNDLE(PfRegs);
    REG(STATUS, std::uint32_t, reg::at(0x40).reset(7));
};
struct VfRegs : reg::RegEntityBundle {
    REG_BUNDLE(VfRegs);
    REG(CONTROL, std::uint32_t, reg::at(0).reset(0));
    REG(COUNTER, std::uint32_t, reg::at(0x80).reset(9));
    // +0x40 is a planned hole, so overlaying PF.STATUS produces no warning.
};
struct Layout : reg::Bundle {
    REG_BUNDLE(Layout);
    REG_ENTITY(pf, PfRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_REPEAT_ENTITY(vf, VfRegs, VF_COUNT, reg::entity().reset_per_instance(reg::ResetScope::Vf));

    REG_LAYOUT([&] {
        const auto pf_shared = reg::view("PF_SHARED", VF_STRIDE).map(pf);
        auto vf_view = [&](auto i) {
            return reg::view("VF", VF_STRIDE)
                .identity(1)
                .using_resolver(VfRoute)
                .map(vf[i])
                .map(pf_shared, 0, 10);
        };
        return reg::address_space("DEVICE")
            .map_repeat("ENTRY_A", BASE_A, reg::repeat(VF_COUNT).stride(VF_STRIDE), vf_view)
            .map_repeat("ENTRY_B", BASE_B, reg::repeat(VF_COUNT).stride(VF_STRIDE), vf_view);
    });
};
int main() {
    reg::RegisterBank<Layout> bank(reg::space().name("DEVICE"));
    bank.map().set_identity_resolver(VfRoute, [](const reg::IdentityResolveRequest& r) {
        return r.request_id;
    });
    bank.finish();
    auto& map = bank.map();
    if (!map.mmio_write(BASE_A + 3 * VF_STRIDE, 42).success() ||
        map.mmio_read(BASE_B + 3 * VF_STRIDE) != 42)
        throw std::runtime_error("VF aliases do not share storage");
    if (!map.mmio_write(BASE_B + VF_STRIDE + 0x40, 99).success() ||
        map.mmio_read(BASE_A + 7 * VF_STRIDE + 0x40) != 99)
        throw std::runtime_error("PF shared storage failed");
    if (map.mmio_read(BASE_B + VF_STRIDE + 0x80) != 9 || !map.layout_warnings().empty())
        throw std::runtime_error("transparent overlay holes failed");
    std::cout
        << "PASS: two high-address entrances, VF private/PF shared storage, transparent holes\n";
}
// DOC-END address-view-example
