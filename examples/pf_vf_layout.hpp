#pragma once
#include <regfw/regfw.hpp>

namespace example {
inline constexpr std::uint64_t KiB = 1024, MiB = 1024 * KiB;
inline constexpr unsigned VF_COUNT = 8;
inline constexpr std::uint32_t VF_DOMAIN = 1;
inline constexpr std::uint64_t SPACE_SIZE = 32 * MiB;
inline constexpr std::uint64_t HEAD_SIZE = 128 * KiB, BUSINESS_SIZE = 896 * KiB;
constexpr std::uint64_t pf_slot(unsigned i) {
    return 2 * MiB + i * MiB;
}
constexpr std::uint64_t vf_window(unsigned i) {
    return 16 * MiB + i * 2 * MiB;
}
REG_ACCESS_FIELD(VfRoute, std::uint32_t);
REG_ACCESS_FIELD(LogicalPool, std::uint32_t);
REG_ACCESS_FIELD(PhysicalPool, std::uint32_t);

// DOC-BEGIN entities
struct ModuleRegs : reg::RegEntityBundle {
    REG_BUNDLE(ModuleRegs);
    REG(DEVICE_ID, std::uint32_t, reg::at(0x00).reset(0x6188).access(reg::ro));
    REG(STATUS, std::uint32_t, reg::at(0x08));
    REG(TIMESTAMP,
        std::uint64_t,
        reg::at(0x20).access(reg::ro).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
    REG(STATS, std::uint32_t, reg::at(0x80).array(reg::dims(4), reg::strides(4)));
};
struct PfRegs : reg::RegEntityBundle {
    REG_BUNDLE(PfRegs);
    REG(CONTROL, std::uint32_t, reg::at(0));
    REG(CPU_SHARED, std::uint32_t, reg::at(0x40));
    REG(GROUP_SHARED, std::uint32_t, reg::at(0x80));
    REG(DESC_BASE, std::uint64_t, reg::at(0xc0));
};
struct VfRegs : reg::RegEntityBundle {
    REG_BUNDLE(VfRegs);
    REG(CONTROL, std::uint32_t, reg::at(0));
    REG(STATUS, std::uint32_t, reg::at(8).access(reg::ro));
    REG(QUEUE, std::uint32_t, reg::at(0x100).array(reg::dims(2, 4), reg::strides(0x20, 4)));
    REG_ACCESS_GROUP(POOLS,
                     .members = {QUEUE},
                     .coordinates = {{LogicalPool, 1}},
                     .derive = {reg::map_index(LogicalPool, PhysicalPool)
                                    .when(reg::identity_domain(VF_DOMAIN))
                                    .using_resolver(PhysicalPool)
                                    .otherwise(reg::passthrough(LogicalPool))});
};
struct CommandRegs : reg::RegEntityBundle {
    REG_BUNDLE(CommandRegs);
    REG(DOORBELL, std::uint32_t, reg::at(0x10).access(reg::write_only));
};
// DOC-END entities

// DOC-BEGIN windows
struct DeviceRegs : reg::Bundle {
    REG_BUNDLE(DeviceRegs);
    REG_ENTITY(module, ModuleRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_ENTITY(pf, PfRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_REPEAT_ENTITY(vf, VfRegs, VF_COUNT, reg::entity().reset_per_instance(reg::ResetScope::Vf));

    REG_ENTITY(pf_commands, CommandRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_REPEAT_ENTITY(vf_commands,
                      CommandRegs,
                      VF_COUNT,
                      reg::entity().reset_per_instance(reg::ResetScope::Vf));
    REG_LAYOUT([&] {
        const auto module_view = reg::view("MODULE", HEAD_SIZE).map(module);
        auto business = [&](auto i) {
            return reg::view("BUSINESS", BUSINESS_SIZE)
                .map(vf[i])
                .map(pf.CPU_SHARED, 0x40, 10)
                .map(pf.GROUP_SHARED, 0x80, 10);
        };
        const auto pf_view = reg::view("PF", 16 * MiB)
                                 .map(module_view)
                                 .map(reg::view("PF_BUSINESS", BUSINESS_SIZE).map(pf), HEAD_SIZE)
                                 .map(reg::view("PF_COMMAND", MiB).map(pf_commands), MiB)
                                 .reserve(10 * MiB, 6 * MiB);
        auto poe_cpu =
            reg::view("DEVICE", SPACE_SIZE)
                .map(pf_view, 0)
                .map_repeat("PF_TO_VF",
                            2 * MiB,
                            reg::repeat(VF_COUNT).stride(MiB),
                            [&](auto i) {
                                return reg::view("SLOT", MiB)
                                    .identity(VF_DOMAIN)
                                    .using_resolver(VfRoute)
                                    .reserve(0, HEAD_SIZE)
                                    .map(business(i).pf_to_vf(), HEAD_SIZE);
                            })
                .map_repeat("VF", 16 * MiB, reg::repeat(VF_COUNT).stride(2 * MiB), [&](auto i) {
                    return reg::view("VF_WINDOW", 2 * MiB)
                        .identity(VF_DOMAIN)
                        .using_resolver(VfRoute)
                        .map(module_view.read_only_ignore_writes().vf_to_pf())
                        .map(business(i).vf_to_pf(), HEAD_SIZE)
                        .map(reg::view("VF_COMMAND", MiB).map(vf_commands[i]).vf_to_pf(), MiB);
                });
        return reg::address_space("POE").map(poe_cpu, 0);
    });
};
// DOC-END windows
} // namespace example
