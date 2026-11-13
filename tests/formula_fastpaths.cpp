// Focused regression coverage for the 0.35.1 fast-path machinery:
//  - after finalize, callback/borrow changes must be observed immediately
//    by the cacheless Formula Decoder
//  - repeated hole accesses must remain unmapped without stale decode state
//  - scope comparison must index each chain from its own end (the 0.35.0
//    release indexed one chain with the other's length)
//  - dynamic declaration-pattern mounts: dense, strided, interleaved (rank 2
//    and rank 3 with a super-increasing ladder), non-power-of-two strides
//    and 64-bit wide arrays
//  - the same shapes through the compiled-static path
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

// ---- shapes under test -----------------------------------------------------
struct Shapes : reg::RegEntityBundle {
    REG_BUNDLE(Shapes);
    REG(DENSE, std::uint32_t, reg::at(0x10).array(reg::dims(16), reg::strides(4)));
    REG(STRIDED, std::uint32_t, reg::at(0x100).array(reg::dims(8), reg::strides(0x10)));
    REG(INTER2, std::uint32_t, reg::at(0x200).array(reg::dims(2, 3), reg::strides(0x180, 0x100)));
    REG(LADDER,
        std::uint32_t,
        reg::at(0x1000).array(reg::dims(3, 2, 3), reg::strides(0x800, 0x180, 0x100)));
    REG(HOLES, std::uint32_t, reg::at(0x2400).array(reg::dims(4), reg::strides(0x14)));
    REG(WIDE, std::uint64_t, reg::at(0x2500).array(reg::dims(4), reg::strides(8)));
};
struct DynLayout : reg::Bundle {
    REG_BUNDLE(DynLayout);
    REG_ENTITY(shapes, Shapes);
    REG_LAYOUT([&] {
        return reg::address_space("DYN").map(reg::view("V", 0x3000).map(shapes), 0);
    });
};
struct StaticLayout : reg::Bundle {
    REG_BUNDLE(StaticLayout);
    REG_ENTITY(shapes, Shapes);
    REG_LAYOUT([](auto layout) {
        namespace reg = ::reg::compiled;
        (void)layout;
        return reg::address_space("STA").map(
            reg::view("V", 0x3000).map(reg::target<&StaticLayout::shapes>()), 0);
    });
};

template <class L>
void exercise_geometry() {
    reg::RegisterBank<L> bank(reg::space(0x3000).name("FP").finalize());
    auto& map = bank.map();
    // dense
    for (unsigned i = 0; i < 16; ++i)
        map.mmio_write(0x10 + 4 * i, 0x1000 + i);
    for (unsigned i = 0; i < 16; ++i)
        CHECK(map.mmio_read(0x10 + 4 * i) == 0x1000 + i);
    // strided
    for (unsigned i = 0; i < 8; ++i)
        map.mmio_write(0x100 + 0x10 * i, 0x2000 + i);
    for (unsigned i = 0; i < 8; ++i)
        CHECK(map.mmio_read(0x100 + 0x10 * i) == 0x2000 + i);
    // rank-2 interleaved: element (i,j) at 0x200+i*0x180+j*0x100
    for (unsigned i = 0; i < 2; ++i)
        for (unsigned j = 0; j < 3; ++j)
            map.mmio_write(0x200 + i * 0x180 + j * 0x100, 0x3000 + i * 3 + j);
    for (unsigned i = 0; i < 2; ++i)
        for (unsigned j = 0; j < 3; ++j)
            CHECK(map.mmio_read(0x200 + i * 0x180 + j * 0x100) == 0x3000 + i * 3 + j);
    CHECK(bank->shapes.INTER2.at(1, 1) == 0x3000 + 4);
    // rank-3: interleaved pair under a super-increasing ladder axis
    for (unsigned k = 0; k < 3; ++k)
        for (unsigned i = 0; i < 2; ++i)
            for (unsigned j = 0; j < 3; ++j)
                map.mmio_write(0x1000 + k * 0x800 + i * 0x180 + j * 0x100,
                               0x4000 + k * 6 + i * 3 + j);
    for (unsigned k = 0; k < 3; ++k)
        for (unsigned i = 0; i < 2; ++i)
            for (unsigned j = 0; j < 3; ++j)
                CHECK(map.mmio_read(0x1000 + k * 0x800 + i * 0x180 + j * 0x100) ==
                      0x4000 + k * 6 + i * 3 + j);
    CHECK(bank->shapes.LADDER.at(2, 1, 2) == 0x4000 + 2 * 6 + 1 * 3 + 2);
    // holes must decode to nothing: failed reads return the invalid marker
    const auto invalid = reg::INVALID_READ_VALUE & 0xffffffffu;
    for (std::uint64_t hole : {0x104ull, 0x204ull, 0x1004ull, 0x1114ull, 0x2404ull})
        CHECK(map.mmio_read(hole) == invalid);
    // non-power-of-two strided array
    for (unsigned i = 0; i < 4; ++i)
        map.mmio_write(0x2400 + 0x14 * i, 0x5000 + i);
    for (unsigned i = 0; i < 4; ++i)
        CHECK(map.mmio_read(0x2400 + 0x14 * i) == 0x5000 + i);
    CHECK(map.mmio_read(0x2404) == invalid);
    // 64-bit wide array: both words of every element
    for (unsigned i = 0; i < 4; ++i) {
        map.mmio_write(0x2500 + 8 * i, 0x6000 + i);
        map.mmio_write(0x2504 + 8 * i, 0x7000 + i);
    }
    for (unsigned i = 0; i < 4; ++i)
        CHECK(map.mmio_read(0x2500 + 8 * i) == 0x6000 + i &&
              map.mmio_read(0x2504 + 8 * i) == 0x7000 + i);
    // one 8-byte transaction reads the whole element
    CHECK(map.mmio_read_sized(0x2500, 8) == 0x700000006000ull);
}

// ---- dynamic execution eligibility after finalize -------------------------
struct MemoRegs : reg::RegEntityBundle {
    REG_BUNDLE(MemoRegs);
    REG(A, std::uint32_t, reg::at(0));
    REG(B, std::uint32_t, reg::at(4));
    REG_SCHEMA(&MemoRegs::A, &MemoRegs::B);
};
struct MemoLayout : reg::Bundle {
    REG_BUNDLE(MemoLayout);
    REG_ENTITY(r, MemoRegs);
    REG_LAYOUT([&] {
        return reg::address_space("MEMO").map(reg::view("V", 8).map(r), 0);
    });
};

void memo_dynamic_state() {
    reg::RegisterBank<MemoLayout> bank(reg::space(8).name("MEMO").finalize());
    auto& map = bank.map();
    bank->r.A.ref() = 0x11;
    bank->r.B.ref() = 0x22;
    // Exercise repeated direct decodes while the node is plain.
    for (int round = 0; round < 3; ++round) {
        CHECK(map.mmio_read(0) == (round ? 0x33 : 0x11));
        CHECK(map.mmio_read(4) == 0x22);
        map.mmio_write(0, 0x33);
        CHECK(bank->r.A.ref() == 0x33);
    }
    // Attach an on_read AFTER finish: the direct path must not bypass it.
    unsigned reads = 0;
    bank->r.A.on_read([&](std::uint32_t value, const reg::RegAccessInfo&) {
        ++reads;
        return value ^ 0xff;
    });
    CHECK(map.mmio_read(0) == (0x33u ^ 0xffu));
    CHECK(reads == 1);
    // Fresh bank: an on_write attached later must still run, and a
    // reentrant read from inside a callback must hit the borrow guard.
    reg::RegisterBank<MemoLayout> guard(reg::space(8).name("MEMO").finalize());
    unsigned writes = 0;
    guard->r.B.on_write([&](const reg::RegAccessInfo&) {
        ++writes;
    });
    guard.map().mmio_write(4, 0x44);
    CHECK(guard->r.B.ref() == 0x44 && writes == 1);
    bool reentrant = false;
    guard->r.A.on_read([&](std::uint32_t, const reg::RegAccessInfo&) {
        // Reentrant MMIO on the same map is rejected by the dispatch guard:
        // the nested read fails and B keeps its stored value.
        const auto nested = guard.map().try_mmio_read_sized(4, 4);
        reentrant = !nested.success() && guard->r.B.ref() == 0x44;
        return 0;
    });
    (void)guard.map().mmio_read(0);
    CHECK(reentrant);
    // Another fresh bank: plain reads work again end-to-end
    reg::RegisterBank<MemoLayout> bank2(reg::space(8).name("MEMO").finalize());
    bank2->r.A.ref() = 0x55;
    for (int i = 0; i < 5; ++i)
        CHECK(bank2.map().mmio_read(0) == 0x55);
}

// ---- scope comparison with unequal chain depths -----------------------------
struct Inner : reg::RegEntityBundle {
    REG_BUNDLE(Inner);
    REG(X, std::uint32_t, reg::at(0));
    REG_SCHEMA(&Inner::X);
};
struct OverlapLayout : reg::Bundle {
    REG_BUNDLE(OverlapLayout);
    REG_ENTITY(low, Inner);
    REG_ENTITY(high, Inner);
    REG_LAYOUT([&] {
        return reg::address_space("OVR")
            .map(reg::view("LOW", 0x10).map(low), 0)
            .map(reg::view("HIGH", 0x10)
                     .map(reg::view("WRAP", 0x10)
                              .map(reg::view("DEEPER", 0x10)
                                       .map(reg::view("INNER", 0x10).map(high)))),
                 0,
                 10);
    });
};
void scope_depth_priority() {
    reg::RegisterBank<OverlapLayout> bank(reg::space(0x10).name("OVR").finalize());
    auto& map = bank.map();
    // The 0.35.0 compare() indexed one chain with the other's length and
    // could treat these as same-priority; the higher-priority deeper scope
    // must win the overlapping bytes.
    map.mmio_write(0, 0x77);
    CHECK(bank->high.X.ref() == 0x77);
    CHECK(bank->low.X.ref() == 0);
    CHECK(map.mmio_read(0) == 0x77);
}

REG_ACCESS_FIELD(TableRoute, std::uint32_t);
REG_ACCESS_FIELD(TableLogical, std::uint32_t);
REG_ACCESS_FIELD(TablePhysical, std::uint32_t);
REG_PRE_ACCESS(TableCheck);
struct RoutedTable : reg::RegEntityBundle {
    REG_BUNDLE(RoutedTable);
    REG(DATA,
        std::uint32_t,
        reg::at(0x40).array(reg::dims(4, 32), reg::strides(0xa0, 4)).mask(0x00ff00ff));
    REG_ACCESS_GROUP(
        INDEX,
        .members = {DATA},
        .coordinates = {{TableLogical, 1}},
        .derive = {reg::map_index(TableLogical, TablePhysical).using_resolver(TablePhysical)});
};
template <bool Overlay>
struct RoutedLayout : reg::Bundle {
    REG_BUNDLE(RoutedLayout);
    REG_REPEAT_ENTITY(vf, RoutedTable, 3);
    REG_ENTITY(overlay, Inner);
    REG_LAYOUT([&] {
        auto root = reg::view("ROUTES", 0x2000)
                        .map_repeat("VF", 0, reg::repeat(3).stride(0x600), [&](auto i) {
                            auto v = reg::view("VF_REGS", 0x600)
                                         .map(vf[i])
                                         .identity(19)
                                         .using_resolver(TableRoute)
                                         .using_pre_access(TableCheck);
                            return v;
                        });
        if constexpr (Overlay)
            root = root.map(reg::view("OVERLAY", 4).map(overlay), 0x650, 10)
                       .map(reg::view("RESERVED", 4).reserve(0, 4), 0xc54, 10);
        return reg::address_space("TABLE_ROUTE")
            .map(root, 0xc100000000ULL)
            .map(root, 0xff7100000000ULL)
            .map(root.read_only_ignore_writes(), 0xc200000000ULL);
    });
};
template <bool Overlay>
void routed_tables() {
    reg::RegisterBank<RoutedLayout<Overlay>> fast(reg::space().name("TABLE_ROUTE"));
    reg::RegisterBank<RoutedLayout<Overlay>> generic(reg::space().name("TABLE_ROUTE"));
    unsigned shift = 1, fast_hooks = 0, generic_hooks = 0;
    reg::RegAccessInfo fast_info, generic_info;
    auto bind = [&](auto& bank, unsigned& hooks, reg::RegAccessInfo& saved) {
        bank.map().set_identity_resolver(TableRoute, [&](const auto& r) {
            return (r.request_id + shift) % 3;
        });
        bank.map().set_access_resolver(TablePhysical, [&](const reg::RegAccessInfo& i) {
            return (i.index_as(TableLogical) + shift) % 32;
        });
        bank.map().set_pre_access(TableCheck, [&hooks, &saved](const reg::RegAccessInfo& i) {
            ++hooks;
            saved = i;
        });
    };
    bind(fast, fast_hooks, fast_info);
    bind(generic, generic_hooks, generic_info);
    fast.finish();
    // Independent expectations cover dense-table holes and trimmed bounds.
    for (auto* bank : {&fast, &generic}) {
        const auto base = 0xc100000000ULL;
        CHECK(bank->map().mmio_write(base + 0x40, 0x12345678).success());
        CHECK((*bank)->vf[1].DATA.at(0, 1) == 0x00340078);
        CHECK(bank->map().mmio_read(base + 0x40) == 0x00340078);
        for (const auto hole : {0x30u, 0xc0u, 0x2a0u})
            CHECK(bank->map().try_mmio_read_sized(base + hole, 4).result ==
                  reg::RegMemTxResult::DecodeError);
        CHECK(bank->map().mmio_write(0xc200000040ULL, 0).result == reg::RegMemTxResult::Ignored);
        CHECK((*bank)->vf[1].DATA.at(0, 1) == 0x00340078);
    }
    // The generic bank keeps the full resolver/permission/overlay decoder.
    for (shift = 0; shift < 3; ++shift)
        for (const auto base : {0xc100000000ULL, 0xff7100000000ULL, 0xc200000000ULL})
            for (unsigned vf = 0; vf < 3; ++vf)
                for (unsigned offset = 0; offset < 0x300; offset += 4) {
                    const auto address = base + vf * 0x600 + offset;
                    const auto data = 0x12340000u + offset + vf;
                    auto a = fast.map().mmio_write(address, data);
                    auto b = generic.map().mmio_write(address, data);
                    CHECK(a.result == b.result);
                    auto x = fast.map().try_mmio_read_sized(address, 4);
                    auto y = generic.map().try_mmio_read_sized(address, 4);
                    CHECK(x.result == y.result && x.value == y.value);
                    CHECK(fast_hooks == generic_hooks);
                    if (fast_hooks) {
                        CHECK(fast_info.address == generic_info.address &&
                              fast_info.resolved_address == generic_info.resolved_address &&
                              fast_info.request_id() == generic_info.request_id() &&
                              fast_info.resolved_id() == generic_info.resolved_id() &&
                              fast_info.index.linear() == generic_info.index.linear() &&
                              fast_info.index_as(TableLogical) ==
                                  generic_info.index_as(TableLogical) &&
                              fast_info.mapped_as(TablePhysical) ==
                                  generic_info.mapped_as(TablePhysical));
                    }
                }
    CHECK(fast.save() == generic.save());
}

} // namespace
int main() try {
    exercise_geometry<DynLayout>();
    exercise_geometry<StaticLayout>();
    memo_dynamic_state();
    scope_depth_priority();
    routed_tables<false>();
    routed_tables<true>();
    std::cout
        << "PASS: fast-path caches under dynamic callbacks/borrow, unequal-depth scope priority, "
           "dynamic+static patterns (dense/strided/interleaved2/ladder/holes/wide)\n";
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
