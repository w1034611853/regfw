// POE-shaped compile benchmark (task §7): ~2000 entity/binding scale, 8 VFs,
// PF-shared overlay remounted in every business view, mixed 32/64/128/512/
// 2048-bit registers, sparse strided arrays, distant apertures, identity
// routes and pre_access, no real address conflicts.
//   default          -> sparse regular fast path
//   -DPOE_FORCE_FULL -> forced complete builder (RegularFill=false)
//   -DPOE_FORMULA    -> formula decoder backend (0.34.0 default elsewhere)
//   -DPOE_INCLUDE_ONLY -> parse/instantiate only
#ifndef POE_FORCE_FULL
#define POE_FORCE_FULL 1
#endif
#include <regfw/regfw.hpp>
REG_ACCESS_FIELD(VfRoute, std::uint32_t);
REG_PRE_ACCESS(CheckGetGroupInit);
REG_PRE_ACCESS(CheckAccInit);

constexpr std::uint64_t POE_MODULE_SIZE = 0x10000;
constexpr std::uint64_t POE_COMMAND_SIZE = 0x1000;
constexpr std::uint64_t POE_COMMAND_OFFSET = 0x900000;
constexpr unsigned POE_VF_COUNT = 8;
constexpr std::uint64_t POE_BUSINESS_SIZE = 0x80000;
constexpr std::uint64_t POE_PF_VF_BASE = 0x1000000, POE_PF_VF_STRIDE = 0x100000;
constexpr std::uint64_t POE_VF_WINDOW_BASE = 0x2000000, POE_VF_WINDOW_STRIDE = 0x200000;
constexpr std::uint64_t POE_CPU_SIZE = 0x3000000, POE_CPU_REG_BASE = 0xC100000000ULL;
constexpr std::uint64_t POE_GET_GROUP_VF_SIZE = 0x810000, POE_GET_GROUP_BASE = 0xD000000000ULL,
                        POE_GET_GROUP_STRIDE = 0x810000;
constexpr std::uint64_t POE_ACC_VF_SIZE = 0x10000, POE_ACC_2M_BASE = 0xFF120000ULL,
                        POE_ACC_2M_STRIDE = 0x200000, POE_ACC_64K_BASE = 0xC1080000ULL,
                        POE_ACC_64K_STRIDE = 0x10000;

struct ModuleRegs : reg::RegEntityBundle {
    REG_BUNDLE(ModuleRegs);
    REG(VERSION, std::uint32_t, reg::at(0));
    REG(RESET, std::uint32_t, reg::at(4).access(reg::rc));
    REG(SCRATCH, std::uint32_t, reg::at(8).mask(0x00ff00ff));
    REG(LINK, std::uint64_t, reg::at(0x10));
    REG(TIMER, std::uint32_t, reg::at(0x18).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
#define MODR(K) REG(M##K, std::uint32_t, reg::at(0x100 + K * 4));
    // clang-format off
    MODR(0)
    MODR(1)
    MODR(2)
    MODR(3)
    MODR(4)
    MODR(5)
    MODR(6)
    MODR(7)
    MODR(8)
    MODR(9)
    MODR(10)
    MODR(11)
    MODR(12)
    MODR(13)
    MODR(14)
    MODR(15)
    MODR(16)
    MODR(17)
    MODR(18)
    MODR(19)
    MODR(20)
    MODR(21)
    MODR(22)
    MODR(23)
    MODR(24)
    MODR(25)
    MODR(26)
    MODR(27)
    MODR(28)
    MODR(29)
    MODR(30)
    MODR(31)
    MODR(32)
    MODR(33)
    MODR(34)
    MODR(35)
    MODR(36)
    MODR(37)
    MODR(38)
    MODR(39)
    MODR(40)
    MODR(41)
    MODR(42)
    MODR(43)
    MODR(44)
    MODR(45)
    MODR(46)
    MODR(47)
    MODR(48)
    MODR(49)
    MODR(50)
    MODR(51)
    MODR(52)
    MODR(53)
    MODR(54)
    MODR(55)
    MODR(56)
    MODR(57)
    MODR(58)
    MODR(59)
    MODR(60)
    MODR(61)
    MODR(62)
    MODR(63)
    MODR(64)
    MODR(65)
    MODR(66)
    MODR(67)
    MODR(68)
    MODR(69)
    MODR(70)
    MODR(71)
    MODR(72)
    MODR(73)
    MODR(74)
    MODR(75)
    MODR(76)
    MODR(77)
    MODR(78)
    MODR(79)
    // clang-format on
#undef MODR
    REG(FWMB0, reg::u2048, reg::at(0x1000).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
    REG(FWMB1, reg::u512, reg::at(0x1200).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
    REG(HIST, std::uint32_t, reg::at(0x2000).array(reg::dims(16), reg::strides(4)));
};
struct CommandRegs : reg::RegEntityBundle {
    REG_BUNDLE(CommandRegs);
    REG(CMD_CTRL, std::uint32_t, reg::at(0).access(reg::write_only));
    REG(CMD_STATUS, std::uint32_t, reg::at(4).access(reg::rc));
#define CPKT(K)                                                                                    \
    REG(PKT##K,                                                                                    \
        reg::u512,                                                                                 \
        reg::at(0x100 + K * 0x40).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
    // clang-format off
    CPKT(0)
    CPKT(1)
    CPKT(2)
    CPKT(3)
    CPKT(4)
    CPKT(5)
    CPKT(6)
    CPKT(7)
    // clang-format on
#undef CPKT
    REG(RSPQ, std::uint32_t, reg::at(0x800).array(reg::dims(32), reg::strides(4)));
};
struct BusinessRegs : reg::RegEntityBundle {
    REG_BUNDLE(BusinessRegs);
    REG(VCTRL, std::uint32_t, reg::at(0));
    REG(VSTATUS, std::uint32_t, reg::at(4).access(reg::rc));
    REG(VMASK, std::uint32_t, reg::at(8).mask(0xff00ff00));
#define VSC(K) REG(V##K, std::uint32_t, reg::at(0x10 + K * 4));
    // clang-format off
    VSC(0)
    VSC(1)
    VSC(2)
    VSC(3)
    VSC(4)
    VSC(5)
    VSC(6)
    VSC(7)
    VSC(8)
    VSC(9)
    VSC(10)
    VSC(11)
    VSC(12)
    VSC(13)
    VSC(14)
    VSC(15)
    VSC(16)
    VSC(17)
    VSC(18)
    VSC(19)
    VSC(20)
    VSC(21)
    VSC(22)
    VSC(23)
    VSC(24)
    VSC(25)
    VSC(26)
    VSC(27)
    VSC(28)
    VSC(29)
    VSC(30)
    VSC(31)
    VSC(32)
    VSC(33)
    VSC(34)
    VSC(35)
    VSC(36)
    VSC(37)
    VSC(38)
    VSC(39)
    // clang-format on
#undef VSC
#define VW128(K) REG(Q##K, reg::u128, reg::at(0x100 + K * 0x20));
    // clang-format off
    VW128(0)
    VW128(1)
    VW128(2)
    VW128(3)
    VW128(4)
    VW128(5)
    VW128(6)
    VW128(7)
    // clang-format on
#undef VW128
    REG(RXTBL, std::uint32_t, reg::at(0x1000).array(reg::dims(32), reg::strides(4)));
    REG(TXTBL, std::uint32_t, reg::at(0x1100).array(reg::dims(32), reg::strides(4)));
    REG(FILT, std::uint32_t, reg::at(0x2000).array(reg::dims(16, 4), reg::strides(4, 64)));
    REG(STAT, std::uint32_t, reg::at(0x3000).array(reg::dims(64), reg::strides(4)));
    REG(BIGDESC, reg::u2048, reg::at(0x4000).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
    REG(LUT, std::uint32_t, reg::at(0x5000).array(reg::dims(64), reg::strides(0x100))); // sparse
};
// 8 shared bundles, ~500 registers together.
struct ShVcpuInt : reg::RegEntityBundle {
    REG_BUNDLE(ShVcpuInt);
    REG(CAUSE, std::uint32_t, reg::at(0).access(reg::rc));
    REG(ENABLE, std::uint32_t, reg::at(4).mask(0xffffffffu));
    REG(AUTO, std::uint32_t, reg::at(8));
#define VI(K) REG(I##K, std::uint32_t, reg::at(0x10 + K * 4));
    // clang-format off
    VI(0)
    VI(1)
    VI(2)
    VI(3)
    VI(4)
    VI(5)
    VI(6)
    VI(7)
    VI(8)
    VI(9)
    VI(10)
    VI(11)
    VI(12)
    VI(13)
    VI(14)
    VI(15)
    // clang-format on
#undef VI
    REG(VECT, std::uint32_t, reg::at(0x100).array(reg::dims(64), reg::strides(4)));
};
struct ShLcpuCfg : reg::RegEntityBundle {
    REG_BUNDLE(ShLcpuCfg);
#define LC(K) REG(C##K, std::uint32_t, reg::at(K * 4));
    // clang-format off
    LC(0)
    LC(1)
    LC(2)
    LC(3)
    LC(4)
    LC(5)
    LC(6)
    LC(7)
    LC(8)
    LC(9)
    LC(10)
    LC(11)
    LC(12)
    LC(13)
    LC(14)
    LC(15)
    LC(16)
    LC(17)
    LC(18)
    LC(19)
    LC(20)
    LC(21)
    LC(22)
    LC(23)
    LC(24)
    LC(25)
    LC(26)
    LC(27)
    LC(28)
    LC(29)
    LC(30)
    LC(31)
    // clang-format on
#undef LC
    REG(LTBL, std::uint32_t, reg::at(0x100).array(reg::dims(32), reg::strides(4)));
};
struct ShQueueCfg : reg::RegEntityBundle {
    REG_BUNDLE(ShQueueCfg);
#define QC(K) REG(Q##K, std::uint32_t, reg::at(K * 4));
    // clang-format off
    QC(0)
    QC(1)
    QC(2)
    QC(3)
    QC(4)
    QC(5)
    QC(6)
    QC(7)
    QC(8)
    QC(9)
    QC(10)
    QC(11)
    QC(12)
    QC(13)
    QC(14)
    QC(15)
    QC(16)
    QC(17)
    QC(18)
    QC(19)
    QC(20)
    QC(21)
    QC(22)
    QC(23)
    QC(24)
    QC(25)
    QC(26)
    QC(27)
    QC(28)
    QC(29)
    QC(30)
    QC(31)
    // clang-format on
#undef QC
    REG(QTBL, std::uint32_t, reg::at(0x100).array(reg::dims(32), reg::strides(4)));
};
struct ShGrpOvfl : reg::RegEntityBundle {
    REG_BUNDLE(ShGrpOvfl);
#define GO(K) REG(G##K, std::uint32_t, reg::at(K * 4));
    // clang-format off
    GO(0)
    GO(1)
    GO(2)
    GO(3)
    GO(4)
    GO(5)
    GO(6)
    GO(7)
    GO(8)
    GO(9)
    GO(10)
    GO(11)
    GO(12)
    GO(13)
    GO(14)
    GO(15)
    // clang-format on
#undef GO
    REG(OVTBL, std::uint32_t, reg::at(0x100).array(reg::dims(16), reg::strides(4)));
};
struct ShGrpOrder : reg::RegEntityBundle {
    REG_BUNDLE(ShGrpOrder);
#define GR(K) REG(R##K, std::uint32_t, reg::at(K * 4));
    // clang-format off
    GR(0)
    GR(1)
    GR(2)
    GR(3)
    GR(4)
    GR(5)
    GR(6)
    GR(7)
    GR(8)
    GR(9)
    GR(10)
    GR(11)
    GR(12)
    GR(13)
    GR(14)
    GR(15)
    // clang-format on
#undef GR
    REG(ORTBL, std::uint32_t, reg::at(0x100).array(reg::dims(16), reg::strides(4)));
};
struct ShGrpStats : reg::RegEntityBundle {
    REG_BUNDLE(ShGrpStats);
    REG(GSRX, std::uint64_t, reg::at(0));
    REG(GSTX, std::uint64_t, reg::at(8));
    REG(GSDROP, std::uint64_t, reg::at(0x10));
#define GS(K) REG(S##K, std::uint32_t, reg::at(0x20 + K * 4));
    // clang-format off
    GS(0)
    GS(1)
    GS(2)
    GS(3)
    GS(4)
    GS(5)
    GS(6)
    GS(7)
    GS(8)
    GS(9)
    GS(10)
    GS(11)
    GS(12)
    GS(13)
    GS(14)
    GS(15)
    // clang-format on
#undef GS
    REG(GSTBL, std::uint32_t, reg::at(0x100).array(reg::dims(64), reg::strides(4)));
};
struct ShQueuePkt : reg::RegEntityBundle {
    REG_BUNDLE(ShQueuePkt);
#define QP(K) REG(P##K, std::uint32_t, reg::at(K * 4));
    // clang-format off
    QP(0)
    QP(1)
    QP(2)
    QP(3)
    QP(4)
    QP(5)
    QP(6)
    QP(7)
    QP(8)
    QP(9)
    QP(10)
    QP(11)
    QP(12)
    QP(13)
    QP(14)
    QP(15)
    QP(16)
    QP(17)
    QP(18)
    QP(19)
    QP(20)
    QP(21)
    QP(22)
    QP(23)
    QP(24)
    QP(25)
    QP(26)
    QP(27)
    QP(28)
    QP(29)
    QP(30)
    QP(31)
    // clang-format on
#undef QP
    REG(QPTBL, std::uint32_t, reg::at(0x100).array(reg::dims(32), reg::strides(4)));
};
struct ShVcpuStats : reg::RegEntityBundle {
    REG_BUNDLE(ShVcpuStats);
    REG(VSRX, std::uint64_t, reg::at(0));
    REG(VSTX, std::uint64_t, reg::at(8));
    REG(VSERR, std::uint64_t, reg::at(0x10));
#define VS(K) REG(U##K, std::uint32_t, reg::at(0x20 + K * 4));
    // clang-format off
    VS(0)
    VS(1)
    VS(2)
    VS(3)
    VS(4)
    VS(5)
    VS(6)
    VS(7)
    VS(8)
    VS(9)
    VS(10)
    VS(11)
    VS(12)
    VS(13)
    VS(14)
    VS(15)
    // clang-format on
#undef VS
    REG(VSTBL, std::uint32_t, reg::at(0x100).array(reg::dims(64), reg::strides(4)));
};
struct GetGroupRegs : reg::RegEntityBundle {
    REG_BUNDLE(GetGroupRegs);
    REG(GGCTL, std::uint32_t, reg::at(0));
    REG(GGSTS, std::uint32_t, reg::at(4).access(reg::rc));
    REG(GGSLOT,
        std::uint32_t,
        reg::at(0x1000).array(reg::dims(512), reg::strides(0x4000))); // 8 MiB strided
};
struct AccRegs : reg::RegEntityBundle {
    REG_BUNDLE(AccRegs);
    REG(ACCCTL, std::uint32_t, reg::at(0));
    REG(ACCSTS, std::uint32_t, reg::at(4).access(reg::rc));
    REG(ACCGRP, reg::u512, reg::at(0x100).array(reg::dims(8), reg::strides(0x800))); // sparse wide
    REG(ACCQ, reg::u2048, reg::at(0x8000).transaction(reg::TransactionPolicy::HIGH_TO_LOW));
};
struct PoeLayout : reg::Bundle {
    REG_BUNDLE(PoeLayout);
    REG_ENTITY(pf_module, ModuleRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_ENTITY(cmd, CommandRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    using Business = BusinessRegs;
    REG_REPEAT_ENTITY(vf,
                      Business,
                      POE_VF_COUNT + 1,
                      reg::entity().reset_per_instance(reg::ResetScope::Vf));
    REG_ENTITY(vcpu_int, ShVcpuInt);
    REG_ENTITY(lcpu_cfg, ShLcpuCfg);
    REG_ENTITY(queue_cfg, ShQueueCfg);
    REG_ENTITY(grp_ovfl, ShGrpOvfl);
    REG_ENTITY(grp_order, ShGrpOrder);
    REG_ENTITY(grp_stats, ShGrpStats);
    REG_ENTITY(queue_pkt, ShQueuePkt);
    REG_ENTITY(vcpu_stats, ShVcpuStats);
    REG_ENTITY(get_group, GetGroupRegs);
    REG_ENTITY(acc, AccRegs);
    void configure_layout(reg::RegisterMap& map) {
        static const auto plan = reg::detail::build_formula_layout_runtime<
            PoeLayout,
            reg::detail::SymbolFactory<PoeLayout, decltype(poe_factory)>{}>();
        reg::detail::install_formula_layout(map, this, plan.view());
    }
    static constexpr auto poe_factory = [](auto layout) {
        namespace reg = ::reg::compiled;
        const auto pf_shared = reg::view("PF_SHARED", POE_BUSINESS_SIZE)
                                   .map(layout.vcpu_int, 0x10000)
                                   .map(layout.lcpu_cfg, 0x18000)
                                   .map(layout.queue_cfg, 0x20000)
                                   .map(layout.grp_ovfl, 0x28000)
                                   .map(layout.grp_order, 0x30000)
                                   .map(layout.grp_stats, 0x38000)
                                   .map(layout.queue_pkt, 0x40000)
                                   .map(layout.vcpu_stats, 0x48000);
        auto business = [&](auto i) {
            return reg::view("BUSINESS", POE_BUSINESS_SIZE).map(layout.vf[i]).map(pf_shared);
        };
        const auto pf_module_view = reg::view("MODULE", POE_MODULE_SIZE).map(layout.pf_module);
        const auto pf_command_view = reg::view("PF_COMMAND", POE_COMMAND_SIZE).map(layout.cmd);
        const auto pf_view = reg::view("POE_PF", POE_VF_WINDOW_BASE)
                                 .map(pf_module_view, 0)
                                 .map(business(POE_VF_COUNT), POE_MODULE_SIZE)
                                 .map(pf_command_view, POE_COMMAND_OFFSET)
                                 .reserve(0x00A0'0000, 0x0060'0000);
        auto pf_vf_slot = [&](auto i) {
            return reg::view("PF_VF_SLOT", POE_PF_VF_STRIDE)
                .identity(1)
                .using_resolver(VfRoute)
                .pf_to_vf()
                .reserve(0, POE_MODULE_SIZE)
                .map(business(i), POE_MODULE_SIZE);
        };
        const auto module_ro_view = reg::view("MODULE_RO", POE_MODULE_SIZE)
                                        .map(layout.pf_module)
                                        .read_only_ignore_writes()
                                        .vf_to_pf();
        const auto vf_command_view =
            reg::view("VF_COMMAND", POE_COMMAND_SIZE).map(layout.cmd).vf_to_pf();
        auto vf_window = [&](auto i) {
            return reg::view("VF_WINDOW", POE_VF_WINDOW_STRIDE)
                .identity(1)
                .using_resolver(VfRoute)
                .vf_to_pf()
                .map(module_ro_view, 0)
                .map(business(i), POE_MODULE_SIZE)
                .map(vf_command_view, 0x80000);
        };
        auto poe_cpu = reg::view("POE_CPU", POE_CPU_SIZE)
                           .map(pf_view, 0)
                           .map_repeat("PF_VF",
                                       POE_PF_VF_BASE,
                                       reg::repeat(POE_VF_COUNT).stride(POE_PF_VF_STRIDE),
                                       pf_vf_slot)
                           .map_repeat("VF",
                                       POE_VF_WINDOW_BASE,
                                       reg::repeat(POE_VF_COUNT).stride(POE_VF_WINDOW_STRIDE),
                                       vf_window);
        auto get_group_vf = [&](auto i) {
            return reg::view("GET_GROUP_VF", POE_GET_GROUP_VF_SIZE)
                .identity(1)
                .using_resolver(VfRoute)
                .vf_to_pf()
                .using_pre_access(CheckGetGroupInit)
                .map(layout.get_group, 0);
        };
        auto acc_vf = [&](auto i) {
            return reg::view("ACC_VF", POE_ACC_VF_SIZE)
                .identity(1)
                .using_resolver(VfRoute)
                .vf_to_pf()
                .using_pre_access(CheckAccInit)
                .map(layout.acc, 0);
        };
        return reg::address_space("POE")
            .map(poe_cpu, POE_CPU_REG_BASE)
            .map_repeat("GET_GROUP",
                        POE_GET_GROUP_BASE,
                        reg::repeat(POE_VF_COUNT).stride(POE_GET_GROUP_STRIDE),
                        get_group_vf)
            .map_repeat("ACC_2M",
                        POE_ACC_2M_BASE,
                        reg::repeat(POE_VF_COUNT).stride(POE_ACC_2M_STRIDE),
                        acc_vf)
            .map_repeat("ACC_64K",
                        POE_ACC_64K_BASE,
                        reg::repeat(POE_VF_COUNT).stride(POE_ACC_64K_STRIDE),
                        acc_vf);
    };
};
#ifndef POE_INCLUDE_ONLY
void construct() {
    reg::RegisterBank<PoeLayout> bank(reg::space());
}
#endif