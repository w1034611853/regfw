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
