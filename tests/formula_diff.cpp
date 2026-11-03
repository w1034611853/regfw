// Formula backend differential: the battery layout below is compiled twice --
// the formula decoder and the dynamic view compiler -- and must be
// indistinguishable at run time:
//   * identical MMIO results, callback parameters, access-group mapping,
//     identity redirects, snapshots (including cross-backend load of
//     partially completed wide transactions), reset and layout reports.
#include <regfw/regfw.hpp>
#include <stdexcept>
#include <string>
#include <cstdio>
REG_ACCESS_FIELD(Route, std::uint32_t);
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!(__VA_ARGS__))                                                                        \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " +      \
                                     #__VA_ARGS__);                                                \
    } while (false)

namespace {
constexpr std::uint64_t A = 0xff71f0000000ULL, B = 0xc100000000ULL, C = 0xc200000000ULL;
REG_ACCESS_FIELD(Logical, std::uint32_t);
REG_ACCESS_FIELD(Physical, std::uint32_t);
REG_PRE_ACCESS(Gate);

// ---------------------------------------------------------------------------
// Compile-time decode differential.
// ---------------------------------------------------------------------------
// Battery layouts. Mode 0 = dynamic views, 2 = formula decoder.
// ---------------------------------------------------------------------------
// 1. Mixed widths, transactions, nonzero offsets, scalar fast paths.
struct BasicRegs : reg::RegEntityBundle {
    REG_BUNDLE(BasicRegs);
    REG(R8, std::uint8_t, reg::at(0));
    REG(R16, std::uint16_t, reg::at(4));
    REG(RW, std::uint32_t, reg::at(0x40));
    REG(MASK, std::uint32_t, reg::at(0x44).mask(0x00ff00ff));
    REG(W64, std::uint64_t, reg::at(0x80));
    REG(BIG, reg::u512, reg::at(0x100).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
    REG(SLOW, std::uint32_t, reg::at(0x200).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
    REG(WIDE2048, reg::u2048, reg::at(0x300).transaction(reg::TransactionPolicy::HIGH_TO_LOW));
};
// 2. Transposed 512-bit array (GET_GROUP shape, scaled down).
struct GetGroupRegs : reg::RegEntityBundle {
    REG_BUNDLE(GetGroupRegs);
    REG(GGCTL, std::uint32_t, reg::at(0));
    REG(GGSLOT, reg::u512, reg::at(0x1000).array(reg::dims(4, 8), reg::strides(0x20000, 0x100)));
};
// 3. Interleaved (non-monotone) 512-bit array.
struct InterleavedRegs : reg::RegEntityBundle {
    REG_BUNDLE(InterleavedRegs);
    REG(M, reg::u512, reg::at(0).array(reg::dims(2, 3), reg::strides(0x180, 0x100)));
};
// 4. Identity family + access-group mapping + pre_access (VF shape).
struct VfRegs : reg::RegEntityBundle {
    REG_BUNDLE(VfRegs);
    REG(TABLE, std::uint32_t, reg::at(0).array(reg::dims(2, 4), reg::strides(16, 4)));
    REG(RC, std::uint32_t, reg::at(0x3c).access(reg::rc));
    REG(STAT, std::uint32_t, reg::at(0x80).array(reg::dims(64), reg::strides(4)));
    REG_ACCESS_GROUP(MAP,
                     .members = {TABLE},
                     .coordinates = {{Logical, 1}},
                     .derive = {reg::map_index(Logical, Physical).using_resolver(Physical)});
};
struct SharedRegs : reg::RegEntityBundle {
    REG_BUNDLE(SharedRegs);
    REG(SHARED, std::uint32_t, reg::at(32));
};
// 5. Sparse strided arrays and holes.
struct SparseRegs : reg::RegEntityBundle {
    REG_BUNDLE(SparseRegs);
    REG(LUT, std::uint32_t, reg::at(0).array(reg::dims(16), reg::strides(0x100)));
    REG(TBL, std::uint32_t, reg::at(0x10).array(reg::dims(8, 2), reg::strides(4, 0x20)));
};

std::uint64_t random(std::uint64_t& x) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

template <int Mode>
struct Battery : reg::Bundle {
    REG_BUNDLE(Battery);
    REG_ENTITY(basic, BasicRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_ENTITY(get_group, GetGroupRegs);
    REG_ENTITY(inter, InterleavedRegs);
    REG_REPEAT_ENTITY(vf, VfRegs, 4, reg::entity().reset_per_instance(reg::ResetScope::Vf));
    REG_ENTITY(shared, SharedRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_ENTITY(sparse, SparseRegs);
    void configure_layout(reg::RegisterMap& map) {
        if constexpr (Mode == 0) {
            auto vfwin = [&](auto i) {
                return reg::view("VF", 0x200)
                    .identity(1)
                    .using_resolver(Route)
                    .using_pre_access(Gate)
                    .vf_to_pf()
                    .map(vf[i])
                    .map(shared);
            };
            reg::apply_layout(
                map,
                reg::address_space("BAT")
                    .map(reg::view("BASIC", 0x400).map(basic), A)
                    .map(reg::view("GG", 0x100000).map(get_group), B)
                    .map(reg::view("INTER", 0x400).map(inter), C)
                    .map(reg::view("SHARED_TOP", 0x40).map(shared),
                         A + 0x1000) // PF-shared multi-address alias
                    .map_repeat("VFS",
                                A + 0x100000,
                                reg::repeat(4).stride(0x400),
                                [&](auto i) {
                                    return vfwin(i);
                                })
                    .map_repeat("VFS2",
                                B + 0x100000,
                                reg::repeat(4).stride(0x1000),
                                [&](auto i) {
                                    return vfwin(i);
                                }) // ACC-style second stride family
                    .map(reg::view("SPARSE", 0x1000).map(sparse).reserve(0x804, 0xF8), C + 0x1000));
        } else {
            auto composed = [] {
                auto vfwin = [&](auto i) {
                    return reg::static_view("VF", 0x200)
                        .identity(1)
                        .using_resolver(Route)
                        .using_pre_access(Gate)
                        .vf_to_pf()
                        .map(reg::target<&Battery::vf>().at(i))
                        .map(reg::target<&Battery::shared>());
                };
                return reg::static_address_space("BAT")
                    .map(reg::static_view("BASIC", 0x400).map(reg::target<&Battery::basic>()), A)
                    .map(reg::static_view("GG", 0x100000).map(reg::target<&Battery::get_group>()),
                         B)
                    .map(reg::static_view("INTER", 0x400).map(reg::target<&Battery::inter>()), C)
                    .map(reg::static_view("SHARED_TOP", 0x40).map(reg::target<&Battery::shared>()),
                         A + 0x1000)
                    .map_repeat("VFS", A + 0x100000, reg::repeat(4).stride(0x400), vfwin)
                    .map_repeat("VFS2", B + 0x100000, reg::repeat(4).stride(0x1000), vfwin)
                    .map(reg::static_view("SPARSE", 0x1000)
                             .map(reg::target<&Battery::sparse>())
                             .reserve(0x804, 0xF8),
                         C + 0x1000);
            };
            static const auto plan = reg::detail::build_formula_layout_runtime<Battery, composed>();
            reg::detail::install_formula_layout(map, this, plan.view());
        }
    }
};


// ---------------------------------------------------------------------------
// Runtime differential.
// ---------------------------------------------------------------------------
void run_battery() {
    reg::RegisterBank<Battery<0>> dynamic(reg::space().name("BAT"));
    reg::RegisterBank<Battery<2>> formula(reg::space().name("BAT"));
    unsigned shift = 1;
    auto setup = [&](auto& bank) {
        bank.map().set_identity_resolver(Route, [&](const auto& r) {
            return (r.request_id + shift) % 4;
        });
        bank.map().set_access_resolver(Physical, [&](const auto& i) {
            return (i.index_as(Logical) + shift + i.resolved_id()) % 4;
        });
        bank.map().set_pre_access(Gate, [&](const auto&) {});
        for (unsigned v = 0; v < 4; ++v) {
            bank->vf[v].RC.on_read([](auto value, const auto&) {
                return value ^ 0x5a5au;
            });
        }
        bank.finish();
    };
    setup(dynamic);
    setup(formula);
    CHECK(dynamic.map().layout_warnings().size() == formula.map().layout_warnings().size());
    // Deterministic probe set: every mapped word of each region plus holes,
    // boundaries, unaligned and crossing accesses.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges{{A, A + 0x400},
                                                                {B, B + 0x200},
                                                                {B + 0x1000, B + 0x1400},
                                                                {C, C + 0x400},
                                                                {A + 0x1000, A + 0x1040},
                                                                {A + 0x100000, A + 0x100200},
                                                                {A + 0x101000, A + 0x101040},
                                                                {B + 0x100000, B + 0x100200},
                                                                {B + 0x103000, B + 0x103040},
                                                                {C + 0x1000, C + 0x1100},
                                                                {C + 0x10800, C + 0x10900},
                                                                {A - 0x100, A},
                                                                {A + 0x400, A + 0x500},
                                                                {B + 0x200, B + 0x1000},
                                                                {C + 0x400, C + 0x800}};
    std::uint64_t seed = 0x9e3779b97f4a7c15ULL;
    auto probe = [&](std::uint64_t address, unsigned size, reg::RegTransactionKind kind) {
        const auto value =
            kind == reg::RegTransactionKind::Write ? std::uint32_t(random(seed)) : 0u;
        auto w_dyn = dynamic.map().try_mmio_write_sized(address, value, size);
        auto w_for = formula.map().try_mmio_write_sized(address, value, size);
        if (w_dyn.result != w_for.result)
            std::fprintf(stderr,
                         "write mismatch addr=%llx size=%u: dyn=%d for=%d\n",
                         (unsigned long long)address,
                         size,
                         (int)w_dyn.result,
                         (int)w_for.result);
        CHECK(w_dyn.result == w_for.result);
        auto r_dyn = dynamic.map().try_mmio_read_sized(address, size);
        auto r_for = formula.map().try_mmio_read_sized(address, size);
        if (r_dyn.result != r_for.result)
            std::fprintf(stderr,
                         "read result mismatch addr=%llx size=%u: dyn=%d for=%d\n",
                         (unsigned long long)address,
                         size,
                         (int)r_dyn.result,
                         (int)r_for.result);
        if (r_dyn.value != r_for.value)
            std::fprintf(stderr,
                         "read value mismatch addr=%llx size=%u: dyn=%llx for=%llx\n",
                         (unsigned long long)address,
                         size,
                         (unsigned long long)r_dyn.value,
                         (unsigned long long)r_for.value);
        CHECK(r_dyn.result == r_for.result);
        CHECK(r_dyn.value == r_for.value);
    };
    for (const auto& [base, end] : ranges) {
        for (auto address = base; address < end; address += 4)
            for (unsigned size : {1, 2, 4, 8})
                if (!(address & (size - 1)))
                    probe(address, size, reg::RegTransactionKind::Read);
    }
    // Random mixed traffic with changing identity shift.
    for (unsigned k = 0; k < 20000; ++k) {
        auto x = random(seed);
        shift = (x >> 60) % 4;
        const auto region = x % 10;
        std::uint64_t base = A;
        std::uint64_t span = 0x400;
        switch (region) {
            case 0:
                base = A;
                span = 0x400;
                break;
            case 1:
                base = B;
                span = 0x200;
                break;
            case 2:
                base = B + 0x1000;
                span = 0x400;
                break;
            case 3:
                base = C;
                span = 0x400;
                break;
            case 4:
                base = A + 0x1000;
                span = 0x40;
                break;
            case 5:
                base = A + 0x100000 + (x % 4) * 0x400;
                span = 0x100;
                break;
            case 6:
                base = A + 0x101000 + (x % 4) * 0x400;
                span = 0x40;
                break;
            case 7:
                base = B + 0x100000 + (x % 4) * 0x1000;
                span = 0x100;
                break;
            case 8:
                base = B + 0x103000 + (x % 4) * 0x1000;
                span = 0x40;
                break;
            default:
                base = C + 0x1000;
                span = 0x100;
                break;
        }
        const unsigned width = 1u << ((x >> 8) % 4);
        const auto address = base + ((x >> 12) % span & ~(std::uint64_t)(width - 1));
        probe(
            address, width, x % 2 ? reg::RegTransactionKind::Write : reg::RegTransactionKind::Read);
    }
    // Wide register transaction beats: identical staged reads/writes.
    auto beats = [&](std::uint64_t address, unsigned size, bool write, std::uint32_t value) {
        auto one = [&](auto& map) {
            return write ? map.try_mmio_write_sized(address, value, size).result
                         : map.try_mmio_read_sized(address, size).result;
        };
        const auto r_dyn = one(dynamic.map()), r_for = one(formula.map());
        CHECK(r_dyn == r_for);
    };
    for (unsigned lane = 0; lane < 8; ++lane)
        beats(A + 0x100 + lane * 8, 8, false, 0);
    for (unsigned lane = 0; lane < 8; ++lane)
        beats(A + 0x100 + lane * 8, 8, true, 0xdeadbeef + lane);
    for (unsigned lane = 0; lane < 8; lane += 2)
        beats(A + 0x100 + lane * 8, 8, false, 0);
    for (unsigned word = 0; word < 64; ++word)
        beats(A + 0x300 + word * 4, 4, true, word * 0x01010101u);
    for (unsigned word = 64; word-- > 0;)
        beats(A + 0x300 + word * 4, 4, false, 0);
    // Snapshot equality and cross-backend restore, including staged traffic.
    beats(A + 0x100, 8, true, 0xcafebabe);        // partially staged BIG write
    beats(A + 0x300 + 0x40, 4, true, 0x12345678); // partially staged HUGE write
    const auto snap0 = dynamic.save(), snap2 = formula.save();
    CHECK(snap0 == snap2);
    dynamic.load(snap2); // dynamic restore from formula snapshot
    const auto again = formula.save();
    CHECK(again == snap2);
    CHECK(dynamic.save() == snap2);
    // After restore, behavior still matches.
    probe(A + 0x40, 4, reg::RegTransactionKind::Read);
    probe(B + 0x1000, 4, reg::RegTransactionKind::Write);
    // Reset equivalence.
    dynamic.reset();
    formula.reset();
    CHECK(dynamic.save() == formula.save());
}
} // namespace

int main() {
    run_battery();
    return 0;
}
