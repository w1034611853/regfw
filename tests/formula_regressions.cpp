// Formula-only backend correctness regressions. Each case reproduces a bug
// discovered while reviewing 0.35.1 and must remain in the public test suite.
#include <regfw/regfw.hpp>
#include <iostream>
#include <stdexcept>
#include <string>

#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!(__VA_ARGS__)) {                                                                      \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " +      \
                                     #__VA_ARGS__);                                                \
        }                                                                                          \
    } while (false)

namespace {
struct TwoTables : reg::RegEntityBundle {
    REG_BUNDLE(TwoTables);
    REG(A, std::uint32_t, reg::at(0).array(reg::dims(3, 2, 2), reg::strides(128, 192, 300)));
    REG(B, std::uint32_t, reg::at(0x2000).array(reg::dims(3, 2, 2), reg::strides(128, 192, 304)));
    REG_SCHEMA(&TwoTables::A, &TwoTables::B);
};
struct TablesLayout : reg::Bundle {
    REG_BUNDLE(TablesLayout);
    REG_ENTITY(tables, TwoTables);
    REG_LAYOUT([&] {
        return reg::address_space("TABLES").map(reg::view("V", 0x3000).map(tables), 0);
    });
};

// PreparedAccessInfo must preserve variable-rank indices, not just i/j.
struct RankFourRegs : reg::RegEntityBundle {
    REG_BUNDLE(RankFourRegs);
    REG(DATA,
        std::uint32_t,
        reg::at(0).array(reg::dims(2, 2, 2, 2), reg::strides(0x1000, 0x400, 0x100, 4)));
    REG_SCHEMA(&RankFourRegs::DATA);
};
struct RankFourLayout : reg::Bundle {
    REG_BUNDLE(RankFourLayout);
    REG_ENTITY(regs, RankFourRegs);
    REG_LAYOUT([&] {
        return reg::address_space("RANK4").map(reg::view("V", 0x2000).map(regs), 0);
    });
};
void test_prepared_indices() {
    reg::RegisterBank<RankFourLayout> bank(reg::space(0x2000).name("RANK4"));
    auto& map = bank.map();
    std::array<std::uint32_t, 4> actual{};
    std::uint32_t rank = 0, callbacks = 0;
    bank.finish();
    // Attach the hook after the decoder was prepared; it must remain live.
    bank->regs.DATA.on_write([&](const reg::RegAccessInfo& info) {
        rank = static_cast<std::uint32_t>(info.index.rank());
        for (std::size_t d = 0; d < info.index.coordinates().size() && d < actual.size(); ++d)
            actual[d] = info.index.coordinates()[d];
        ++callbacks;
    });
    for (std::uint32_t a = 0; a < 2; ++a)
        for (std::uint32_t b = 0; b < 2; ++b)
            for (std::uint32_t c = 0; c < 2; ++c)
                for (std::uint32_t d = 0; d < 2; ++d) {
                    const auto address = a * 0x1000 + b * 0x400 + c * 0x100 + d * 4;
                    CHECK(map.mmio_write(address, 0x100 + a * 8 + b * 4 + c * 2 + d).success());
                    CHECK(map.mmio_read(address) == 0x100 + a * 8 + b * 4 + c * 2 + d);
                    CHECK(rank == 4);
                    CHECK((actual == std::array<std::uint32_t, 4>{a, b, c, d}));
                }
    CHECK(callbacks == 16);
}

struct SparseRegs : reg::RegEntityBundle {
    REG_BUNDLE(SparseRegs);
    REG(X, std::uint32_t, reg::at(0).array(reg::dims(4), reg::strides(0x100)));
    REG_SCHEMA(&SparseRegs::X);
};
struct SingleReg : reg::RegEntityBundle {
    REG_BUNDLE(SingleReg);
    REG(Y, std::uint32_t, reg::at(0));
    REG_SCHEMA(&SingleReg::Y);
};
struct CollidingLayout : reg::Bundle {
    REG_BUNDLE(CollidingLayout);
    REG_ENTITY(a, SparseRegs);
    REG_ENTITY(b, SingleReg);
    REG_LAYOUT([&] {
        return reg::address_space("COLLIDE")
            .map(reg::view("VA", 0x400).map(a), 0)
            .map(reg::view("VB", 0x100).map(b), 0x100);
    });
};
// A higher-priority sparse mount has an address *hole* at 0x100. Two
// same-priority lower mounts collide inside that hole and must not escape
// conflict validation merely because the envelope winner is the sparse one.
struct SparseHighest : reg::RegEntityBundle {
    REG_BUNDLE(SparseHighest);
    REG(H, std::uint32_t, reg::at(0).array(reg::dims(2), reg::strides(0x200)));
    REG_SCHEMA(&SparseHighest::H);
};
struct HiddenConflictLayout : reg::Bundle {
    REG_BUNDLE(HiddenConflictLayout);
    REG_ENTITY(high, SparseHighest);
    REG_ENTITY(b, SingleReg);
    REG_ENTITY(c, SingleReg);
    REG_LAYOUT([&] {
        return reg::address_space("HIDDEN_CONFLICT")
            .map(reg::view("HIGH", 0x300).map(high), 0, 10)
            .map(reg::view("LOW1", 0x100).map(b), 0x100)
            .map(reg::view("LOW2", 0x100).map(c), 0x100);
    });
};
struct DisjointLayout : reg::Bundle {
    REG_BUNDLE(DisjointLayout);
    REG_ENTITY(a, SparseRegs);
    REG_ENTITY(b, SingleReg);
    REG_LAYOUT([&] {
        return reg::address_space("DISJOINT")
            .map(reg::view("VA", 0x400).map(a), 0)
            .map(reg::view("VB", 0x100).map(b), 0x0F0);
    });
};
// A wide register overlaps another declaration only at its *last* 4-byte
// lane. Validation must compare actual occupied byte intervals, not starts.
struct WideRegs : reg::RegEntityBundle {
    REG_BUNDLE(WideRegs);
    REG(W, reg::u512, reg::at(0));
    REG_SCHEMA(&WideRegs::W);
};
struct WideTailConflict : reg::Bundle {
    REG_BUNDLE(WideTailConflict);
    REG_ENTITY(a, WideRegs);
    REG_ENTITY(b, SingleReg);
    REG_LAYOUT([&] {
        return reg::address_space("WIDE_TAIL")
            .map(reg::view("WIDE", 0x100).map(a), 0)
            .map(reg::view("TAIL", 0x20).map(b), 0x3C);
    });
};
struct WideTailAdjacent : reg::Bundle {
    REG_BUNDLE(WideTailAdjacent);
    REG_ENTITY(a, WideRegs);
    REG_ENTITY(b, SingleReg);
    REG_LAYOUT([&] {
        return reg::address_space("WIDE_ADJACENT")
            .map(reg::view("WIDE", 0x100).map(a), 0)
            .map(reg::view("TAIL", 0x20).map(b), 0x40);
    });
};

struct ReserveRegs : reg::RegEntityBundle {
    REG_BUNDLE(ReserveRegs);
    REG(X, std::uint32_t, reg::at(0).array(reg::dims(2), reg::strides(0x100)));
    REG_SCHEMA(&ReserveRegs::X);
};
struct StaticReserveGap : reg::Bundle {
    REG_BUNDLE(StaticReserveGap);
    REG_ENTITY(e, ReserveRegs);
    REG_LAYOUT([](auto layout) {
        namespace c = ::reg::compiled;
        return c::address_space("SR")
            .map(c::view("V", 0x300).map(layout.e), 0)
            .map(c::view("RES", 4).reserve(0, 4), 0x80, 10);
    });
};
struct DynamicReserveGap : reg::Bundle {
    REG_BUNDLE(DynamicReserveGap);
    REG_ENTITY(e, ReserveRegs);
    REG_LAYOUT([&] {
        return reg::address_space("DR")
            .map(reg::view("V", 0x300).map(e), 0)
            .map(reg::view("RES", 4).reserve(0, 4), 0x80, 10);
    });
};
struct StaticReserveHit : reg::Bundle {
    REG_BUNDLE(StaticReserveHit);
    REG_ENTITY(e, ReserveRegs);
    REG_LAYOUT([](auto layout) {
        namespace c = ::reg::compiled;
        return c::address_space("SRH")
            .map(c::view("V", 0x300).map(layout.e), 0)
            .map(c::view("RES", 4).reserve(0, 4), 0x100, 10);
    });
};

void test_table_dedup() {
    reg::RegisterBank<TablesLayout> bank(reg::space(0x3000).name("TABLES"));
    auto& map = bank.map();
    // The old dedup key used only (kind, count, width, rank), so B incorrectly
    // shared A's offset table even though its final-axis stride differs.
    CHECK(!map.try_mmio_read_sized(0x2000 + 300, 4).success());
    CHECK(!map.mmio_write(0x2000 + 300, 0xBADu).success());
    for (std::uint32_t i = 0; i < 3; ++i)
        for (std::uint32_t j = 0; j < 2; ++j)
            for (std::uint32_t k = 0; k < 2; ++k) {
                const std::uint64_t addr = 0x2000 + i * 128 + j * 192 + k * 304;
                const auto value = 0x100u + 4 * i + 2 * j + k;
                CHECK(map.mmio_write(addr, value).success());
                CHECK(map.mmio_read(addr) == value);
                CHECK(bank->tables.B.at(i, j, k).ref() == value);
            }
    for (std::uint32_t i = 0; i < 3; ++i)
        for (std::uint32_t j = 0; j < 2; ++j)
            for (std::uint32_t k = 0; k < 2; ++k) {
                const std::uint64_t addr = i * 128 + j * 192 + k * 300;
                const auto value = 0x200u + 4 * i + 2 * j + k;
                CHECK(map.mmio_write(addr, value).success());
                CHECK(map.mmio_read(addr) == value);
                CHECK(bank->tables.A.at(i, j, k).ref() == value);
            }
    const auto checkpoint = bank.save();
    bank.reset();
    bank.load(checkpoint);
    CHECK(map.mmio_read(0x2000 + 304) == 0x101u);
    CHECK(map.mmio_read(300) == 0x201u);
    CHECK(!map.try_mmio_read_sized(0x2000 + 300, 4).success());
}

void test_overlap() {
    bool rejected = false;
    try {
        reg::RegisterBank<CollidingLayout> bank(reg::space(0x400).name("COLLIDE"));
    } catch (const reg::Error& e) {
        rejected = std::string(e.what()).find("conflict") != std::string::npos;
    }
    CHECK(rejected);
    bool hidden_rejected = false;
    try {
        reg::RegisterBank<HiddenConflictLayout> hidden(reg::space(0x400).name("HIDDEN_CONFLICT"));
    } catch (const reg::Error& e) {
        hidden_rejected = std::string(e.what()).find("conflict") != std::string::npos;
    }
    CHECK(hidden_rejected);
    reg::RegisterBank<DisjointLayout> bank(reg::space(0x400).name("DISJOINT"));
    CHECK(bank.map().mmio_write(0x100, 0x11).success());
    CHECK(bank.map().mmio_write(0xF0, 0x22).success());
    CHECK(bank->a.X.at(1).ref() == 0x11u);
    CHECK(bank->b.Y.ref() == 0x22u);

    bool partial_rejected = false;
    try {
        reg::RegisterBank<WideTailConflict> wide(reg::space(0x100).name("WIDE_TAIL"));
    } catch (const reg::Error& e) {
        partial_rejected = e.kind == reg::ErrorKind::Configuration;
    }
    CHECK(partial_rejected);
    reg::RegisterBank<WideTailAdjacent> adjacent(reg::space(0x100).name("WIDE_ADJACENT"));
    CHECK(adjacent.map().mmio_write(0x40, 0x123u).success());
    CHECK(adjacent->b.Y.ref() == 0x123u);
}

void test_reserve_warning() {
    reg::RegisterBank<StaticReserveGap> a(reg::space(0x300).name("SR"));
    reg::RegisterBank<DynamicReserveGap> b(reg::space(0x300).name("DR"));
    CHECK(a.map().layout_warnings().empty());
    CHECK(b.map().layout_warnings().empty());
    CHECK(a.map().mmio_write(0x100, 0x33).success());
    CHECK(b.map().mmio_write(0x100, 0x44).success());
    CHECK(!a.map().try_mmio_read_sized(0x80, 4).success());
    CHECK(!b.map().try_mmio_read_sized(0x80, 4).success());
    reg::RegisterBank<StaticReserveHit> hit(reg::space(0x300).name("SRH"));
    CHECK(!hit.map().layout_warnings().empty());
    CHECK(!hit.map().try_mmio_read_sized(0x100, 4).success());
}
} // namespace

int main() {
    try {
        test_prepared_indices();
        test_table_dedup();
        test_overlap();
        test_reserve_warning();
        std::cout << "formula_regressions OK\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "formula_regressions failed: " << e.what() << '\n';
        return 1;
    }
}
