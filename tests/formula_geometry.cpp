// Regression coverage for formula-layout geometry validation:
//  - overlapping view bounding boxes whose absolute register bytes are disjoint
//  - a reserved run at a non-zero offset must not be truncated to its size
#include <regfw/regfw.hpp>
#include <iostream>
#include <stdexcept>
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!(__VA_ARGS__))                                                                        \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " +      \
                                     #__VA_ARGS__);                                                \
    } while (false)
namespace {
struct CoordA : reg::RegEntityBundle {
    REG_BUNDLE(CoordA);
    REG(A, std::uint32_t, reg::at(0).array(reg::dims(4), reg::strides(0x100)));
    REG_SCHEMA(&CoordA::A);
};
struct CoordB : reg::RegEntityBundle {
    REG_BUNDLE(CoordB);
    REG(B, std::uint32_t, reg::at(0).array(reg::dims(2), reg::strides(0x100)));
    REG_SCHEMA(&CoordB::B);
};
struct CoordLayout : reg::Bundle {
    REG_BUNDLE(CoordLayout);
    REG_ENTITY(a, CoordA);
    REG_ENTITY(b, CoordB);
    REG_LAYOUT([](auto layout) {
        namespace reg = ::reg::compiled;
        (void)layout;
        // A covers [0x000,0x304); B covers [0x0f0,0x1f4).  Their bounding boxes
        // overlap, but A{0,0x100,0x200,0x300}+4 and B{0xf0,0x1f0}+4 are disjoint.
        // The old validator compared per-mount-relative offsets and rejected this.
        return reg::address_space("COORD")
            .map(reg::view("A", 0x400).map(reg::target<&CoordLayout::a>()), 0)
            .map(reg::view("B", 0x200).map(reg::target<&CoordLayout::b>()), 0x0f0);
    });
};
struct ReserveRegs : reg::RegEntityBundle {
    REG_BUNDLE(ReserveRegs);
    REG(TABLE, std::uint32_t, reg::at(0));
    REG_SCHEMA(&ReserveRegs::TABLE);
};
struct ReservePruneLayout : reg::Bundle {
    REG_BUNDLE(ReservePruneLayout);
    REG_ENTITY(a, CoordA);
    REG_ENTITY(b, CoordB);
    REG_LAYOUT([&] {
        // The first mount ends after the second. A reserve near its end must
        // not be pruned using only the immediately preceding mount's end.
        return reg::address_space("PRUNE")
            .map(reg::view("A", 0x400).map(a), 0)
            .map(reg::view("B", 0x200).map(b), 0xf0)
            .map(reg::view("RES", 4).reserve(0, 4), 0x300, 10)
            .map(reg::view("GAP", 4).reserve(0, 4), 0x304, 10);
    });
};
struct ReserveLayout : reg::Bundle {
    REG_BUNDLE(ReserveLayout);
    REG_ENTITY(regs, ReserveRegs);
    REG_LAYOUT([](auto layout) {
        namespace reg = ::reg::compiled;
        (void)layout;
        // Reserve [0x1100,0x3100) in absolute space, i.e. offset 0x100 inside a
        // view mounted at 0x1000.  The table at absolute 0x1000 is before it.
        // The old code used {offset,size} as an absolute interval, i.e. [0x100,0x2000),
        // and falsely rejected the table.
        return reg::address_space("RESV").map(
            reg::view("V", 0x4000).map(reg::target<&ReserveLayout::regs>()).reserve(0x100, 0x2000),
            0x1000);
    });
};
struct ShadowRegs : reg::RegEntityBundle {
    REG_BUNDLE(ShadowRegs);
    REG(HIDDEN, std::uint32_t, reg::at(0x100));
    REG(VISIBLE, std::uint32_t, reg::at(0x200));
    REG_SCHEMA(&ShadowRegs::HIDDEN, &ShadowRegs::VISIBLE);
};
struct ShadowLayout : reg::Bundle {
    REG_BUNDLE(ShadowLayout);
    REG_ENTITY(regs, ShadowRegs);
    REG_LAYOUT([](auto layout) {
        namespace reg = ::reg::compiled;
        (void)layout;
        // The reserved child has equal priority but is terminal; it must shadow the
        // register at the same bytes without rejecting the layout.
        return reg::address_space("SHADOW").map(
            reg::view("S", 0x400).map(reg::target<&ShadowLayout::regs>()).reserve(0x100, 0x100), 0);
    });
};

struct ConflictLayout : reg::Bundle {
    REG_BUNDLE(ConflictLayout);
    REG_ENTITY(regs, ShadowRegs);
    REG_LAYOUT([](auto layout) {
        namespace reg = ::reg::compiled;
        (void)layout;
        auto v = reg::view("V", 0x400).map(reg::target<&ConflictLayout::regs>());
        return reg::address_space("CONFLICT").map(v, 0).map(v, 0);
    });
};
template <bool Transposed>
struct GeometryRegs : reg::RegEntityBundle {
    REG_BUNDLE(GeometryRegs);
    REG(DATA,
        std::uint32_t,
        reg::at(0).array(reg::dims(4, 16), reg::strides(Transposed ? 4 : 64, Transposed ? 16 : 4)));
};
struct GeometryViews : reg::Bundle {
    REG_BUNDLE(GeometryViews);
    REG_ENTITY(a, GeometryRegs<false>);
    REG_ENTITY(b, GeometryRegs<true>);
    REG_ENTITY(c, GeometryRegs<false>);
    REG_LAYOUT([&] {
        return reg::address_space("GEOMETRY")
            .map(reg::view("A", 0x100).map(a), 0)
            .map(reg::view("B", 0x100).map(b), 0x1000)
            .map(reg::view("C", 0x100).map(c), 0x2000)
            .map(reg::view("ALIAS", 0x100).map(a), 0x3000);
    });
};
void check_shared_geometry() {
    reg::RegisterBank<GeometryViews> bank(reg::space(0x4000).finalize());
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 16; ++col) {
            const auto linear = row * 16 + col;
            bank->a.DATA.at(row, col).ref() = 100 + linear;
            bank->b.DATA.at(row, col).ref() = 200 + linear;
            bank->c.DATA.at(row, col).ref() = 300 + linear;
        }
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 16; ++col) {
            const auto linear = row * 16 + col;
            CHECK(bank.map().mmio_read(linear * 4) == 100 + linear);
            CHECK(bank.map().mmio_read(0x1000 + row * 4 + col * 16) == 200 + linear);
            CHECK(bank.map().mmio_read(0x2000 + linear * 4) == 300 + linear);
            CHECK(bank.map().mmio_read(0x3000 + linear * 4) == 100 + linear);
        }
}

// Each large leaf keeps its own prepared View. Sparse overlay holes must
// fall through, and reserves belonging to one VF/entrance must not mask another.
struct PartitionRegs : reg::RegEntityBundle {
    REG_BUNDLE(PartitionRegs);
    REG(DATA, std::uint32_t, reg::at(0).array(reg::dims(64), reg::strides(8)));
};
struct PartitionLayout : reg::Bundle {
    REG_BUNDLE(PartitionLayout);
    REG_REPEAT_ENTITY(vf, PartitionRegs, 3);
    REG_ENTITY(overlay, PartitionRegs);
    REG_LAYOUT([&] {
        auto root = reg::view("PARTITIONS", 0x3000)
                        .map_repeat("VF",
                                    0x1000,
                                    reg::repeat(3).stride(0x400),
                                    [&](auto i) {
                                        return reg::view("REGS", 0x400).map(vf[i]);
                                    })
                        .map(reg::view("OVERLAY", 0x200).map(overlay), 0x1004, 10)
                        .map(reg::view("BLOCK_VF0", 4).reserve(0, 4), 0x1008, 20)
                        .map(reg::view("LOWER_VF1", 4).reserve(0, 4), 0x1410, -1)
                        .map(reg::view("BLOCK_VF2", 4).reserve(0, 4), 0x1818, 20);
        for (unsigned i = 0; i < 48; ++i)
            root = root.reserve(i * 8, 4);
        return reg::address_space("PARTITION_ROOT")
            .map(root, 0xc100000000ULL)
            .map(root, 0xff7100000000ULL);
    });
};
void check_partitioned_views() {
    reg::RegisterBank<PartitionLayout> bank(reg::space());
    bank.finish();
    for (const auto base : {0xc100000000ULL, 0xff7100000000ULL}) {
        for (unsigned vf = 0; vf < 3; ++vf)
            for (unsigned i = 0; i < 64; ++i) {
                const auto address = base + 0x1000 + vf * 0x400 + i * 8;
                const bool blocked = (vf == 0 && i == 1) || (vf == 2 && i == 3);
                const auto written = bank.map().mmio_write(address, vf * 1000 + i + 1);
                const auto read = bank.map().try_mmio_read_sized(address, 4);
                CHECK(written.result ==
                      (blocked ? reg::RegMemTxResult::DecodeError : reg::RegMemTxResult::Ok));
                CHECK(read.result == written.result);
                if (!blocked)
                    CHECK(read.value == vf * 1000 + i + 1);
                // The high-priority overlay occupies only VF0's odd words.
                const auto hole = address + 4;
                if (vf == 0) {
                    CHECK(bank.map().mmio_write(hole, 0x8000 + i).success());
                    CHECK(bank->overlay.DATA.at(i).ref() == 0x8000 + i);
                } else
                    CHECK(bank.map().try_mmio_read_sized(hole, 4).result ==
                          reg::RegMemTxResult::DecodeError);
            }
        for (const auto offset : {0u, 0x17fcu, 0x1a00u, 0x2ffcu, 0x3000u})
            CHECK(bank.map().try_mmio_read_sized(base + offset, 4).result ==
                  reg::RegMemTxResult::DecodeError);
    }
    CHECK(bank->vf[0].DATA.at(1).ref() == 0 && bank->vf[2].DATA.at(3).ref() == 0);
    CHECK(bank.map().describe_layout(reg::LayoutDetail::Full).find("PARTITIONS") !=
          std::string::npos);
}

void check_coord() {
    reg::RegisterBank<CoordLayout> bank(reg::space(0x400).name("COORD"));
    bank->a.A.at(0).ref() = 0x11111111u;
    bank->a.A.at(1).ref() = 0x22222222u;
    bank->b.B.at(0).ref() = 0xa0a0a0a0u;
    bank->b.B.at(1).ref() = 0xb0b0b0b0u;
    CHECK(bank.map().mmio_read(0x000) == 0x11111111u);
    CHECK(bank.map().mmio_read(0x100) == 0x22222222u);
    CHECK(bank.map().mmio_read(0x0f0) == 0xa0a0a0a0u);
    CHECK(bank.map().mmio_read(0x1f0) == 0xb0b0b0b0u);
    CHECK(bank.map().mmio_write(0x100, 0x33333333u).success());
    CHECK(bank->a.A.at(1).ref() == 0x33333333u);
    auto d = bank.map().describe_layout(reg::LayoutDetail::Full);
    CHECK(d.find("COORD") != std::string::npos && d.find("A") != std::string::npos);
}
void check_reserve() {
    reg::RegisterBank<ReserveLayout> bank(reg::space(0x5000).name("RESV"));
    CHECK(bank.map().mmio_write(0x1000, 0x12345678u).success());
    CHECK(bank->regs.TABLE.ref() == 0x12345678u);
    CHECK(bank.map().mmio_read(0x1000) == 0x12345678u);
    // Reserved bytes still reject.
    CHECK(!bank.map().mmio_write(0x2800, 1u).success());
    CHECK(!bank.map().try_mmio_read_sized(0x2810, 4).success());
    CHECK(bank.map().describe_layout(reg::LayoutDetail::Full).find("RESV") != std::string::npos);
    reg::RegisterBank<ReservePruneLayout> pruned(reg::space(0x400).finalize());
    CHECK(pruned.map().mmio_write(0x200, 11).success());
    CHECK(pruned.map().mmio_write(0x1f0, 12).success());
    CHECK(pruned->a.A.at(2) == 11 && pruned->b.B.at(1) == 12);
    CHECK(!pruned.map().try_mmio_read_sized(0x300, 4).success());
    CHECK(pruned.map().layout_warnings().size() == 1);
}
void check_conflict() {
    bool rejected = false;
    try {
        reg::RegisterBank<ConflictLayout> bank(reg::space(0x400).name("CONFLICT"));
    } catch (const reg::Error& e) {
        rejected = std::string(e.what()).find("conflict") != std::string::npos;
    }
    CHECK(rejected);
}
void check_shadow() {
    reg::RegisterBank<ShadowLayout> bank(reg::space(0x400).name("SHADOW"));
    CHECK(bank.map().mmio_write(0x200, 0x55u).success());
    CHECK(bank->regs.VISIBLE.ref() == 0x55u);
    CHECK(!bank.map().try_mmio_read_sized(0x100, 4).success());
    CHECK(!bank.map().mmio_write(0x100, 1u).success());
    CHECK(bank->regs.HIDDEN.ref() == 0u);
}
} // namespace
int main() {
    try {
        check_coord();
        check_partitioned_views();
        check_shared_geometry();
        check_reserve();
        check_shadow();
        check_conflict();
        std::cout << "formula_geometry OK\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "formula_geometry failed: " << e.what() << "\n";
        return 1;
    }
}
