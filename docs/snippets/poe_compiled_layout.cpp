#include <regfw/regfw.hpp>
#include <cstdint>

constexpr std::uint64_t MiB = 1024 * 1024;
constexpr std::uint64_t MODULE_SIZE = 0x20000, BUSINESS_SIZE = 0xe0000;
constexpr unsigned VF_COUNT = 8;
constexpr std::uint64_t BASE_A = 0xff71f0000000ULL, BASE_B = 0xc100000000ULL;
REG_ACCESS_FIELD(VfRoute, std::uint32_t);

// Sample registers; replace their declarations with the actual POE definitions.
// There is no REG_SCHEMA list to maintain.
struct ModuleRegs : reg::RegEntityBundle {
    REG_BUNDLE(ModuleRegs);
    REG(HEADER, std::uint32_t, reg::at(0));
};
struct BusinessRegs : reg::RegEntityBundle {
    REG_BUNDLE(BusinessRegs);
    REG(CTRL, std::uint32_t, reg::at(0x100));
};
struct SharedRegs : reg::RegEntityBundle {
    REG_BUNDLE(SharedRegs);
    REG(VALUE, std::uint32_t, reg::at(0));
};
struct CommandRegs : reg::RegEntityBundle {
    REG_BUNDLE(CommandRegs);
    REG(WORD, reg::u512, reg::at(0).array(reg::dims(16), reg::strides(64)));
};
struct PoeCpuLayout : reg::Bundle {
    REG_BUNDLE(PoeCpuLayout);
    REG_ENTITY(pf_module, ModuleRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_ENTITY(pf_business, BusinessRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_REPEAT_ENTITY(vf,
                      BusinessRegs,
                      VF_COUNT,
                      reg::entity().reset_per_instance(reg::ResetScope::Vf));
    REG_ENTITY(vcpu_int, SharedRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_ENTITY(lcpu_cfg, SharedRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_ENTITY(queue_cfg, SharedRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_ENTITY(grp_ovfl, SharedRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_ENTITY(grp_order, SharedRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_ENTITY(grp_stats, SharedRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_ENTITY(queue_pkt, SharedRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_ENTITY(vcpu_stats, SharedRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_ENTITY(cmd, CommandRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));

    REG_LAYOUT([](auto layout) {
        // One local alias retains the original reg::view / address_space spelling.
        // Alternatively write layout.view(...) / layout.address_space(...).
        namespace reg = ::reg::compiled;
        auto shared = reg::view("PF_SHARED", BUSINESS_SIZE)
                          .map(layout.vcpu_int, 0x00000)
                          .map(layout.lcpu_cfg, 0x10000)
                          .map(layout.queue_cfg, 0x30000)
                          .map(layout.grp_ovfl, 0x50000)
                          .map(layout.grp_order, 0x58000)
                          .map(layout.grp_stats, 0x70000)
                          .map(layout.queue_pkt, 0x74000)
                          .map(layout.vcpu_stats, 0x76000);
        auto business = [&](auto entity) {
            return reg::view("BUSINESS", BUSINESS_SIZE).map(entity).map(shared);
        };
        auto pf_view = reg::view("POE_PF", 16 * MiB)
                           .map(reg::view("MODULE", MODULE_SIZE).map(layout.pf_module), 0)
                           .map(business(layout.pf_business), MODULE_SIZE)
                           .map(reg::view("PF_COMMAND", MiB).map(layout.cmd), MiB)
                           .reserve(10 * MiB, 6 * MiB);
        auto pf_vf_slot = [&](auto i) {
            return reg::view("PF_VF_SLOT", MiB)
                .identity(1)
                .using_resolver(VfRoute)
                .reserve(0, MODULE_SIZE)
                .map(business(layout.vf[i]).pf_to_vf(), MODULE_SIZE);
        };
        auto vf_window = [&](auto i) {
            return reg::view("VF_WINDOW", 2 * MiB)
                .identity(1)
                .using_resolver(VfRoute)
                .map(reg::view("MODULE_RO", MODULE_SIZE)
                         .map(layout.pf_module)
                         .read_only_ignore_writes()
                         .vf_to_pf(),
                     0)
                .map(business(layout.vf[i]).vf_to_pf(), MODULE_SIZE)
                .map(reg::view("VF_COMMAND", MiB).map(layout.cmd).vf_to_pf(), MiB);
        };
        auto poe_cpu =
            reg::view("POE_CPU", 32 * MiB)
                .map(pf_view, 0)
                .map_repeat("PF_VF", 2 * MiB, reg::repeat(VF_COUNT).stride(MiB), pf_vf_slot)
                .map_repeat("VF", 16 * MiB, reg::repeat(VF_COUNT).stride(2 * MiB), vf_window);
        return reg::address_space("POE").map(poe_cpu, BASE_A).map(poe_cpu, BASE_B);
    });
};
int main() {
    reg::RegisterBank<PoeCpuLayout> bank(reg::space().name("POE"));
    bank.map().set_identity_resolver(VfRoute, [](const auto& request) {
        return request.request_id;
    });
    bank.finish();
    bank.map().mmio_write(BASE_A + MODULE_SIZE, 42); // PF shared VCPU_INT
    if (bank.map().mmio_read(BASE_B + 16 * MiB + 7 * 2 * MiB + MODULE_SIZE) != 42)
        return 1;
    bank.map().mmio_write(BASE_A + 2 * MiB + 3 * MiB + MODULE_SIZE + 0x100, 99); // PF -> VF3
    if (bank.map().mmio_read(BASE_B + 16 * MiB + 3 * 2 * MiB + MODULE_SIZE + 0x100) != 99)
        return 2;
    if (bank->vf[2].CTRL.ref() != 0 || bank->pf_business.CTRL.ref() != 0)
        return 3;
    bank->pf_module.HEADER.ref() = 123;
    bank.map().mmio_write(BASE_B + 16 * MiB, 999); // VF module is read-only
    if (bank->pf_module.HEADER.ref() != 123)
        return 4;
    auto snapshot = bank.save();
    bank.reset();
    bank.load(snapshot);
    return bank->vcpu_int.VALUE.ref() == 42 && bank->vf[3].CTRL.ref() == 99 ? 0 : 5;
}
