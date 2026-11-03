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
constexpr std::uint64_t A = 0xff71f0000000ULL, B = 0xc100000000ULL, C = 0xc200000000ULL;
REG_ACCESS_FIELD(Route, std::uint32_t);
REG_ACCESS_FIELD(L, std::uint32_t);
REG_ACCESS_FIELD(P, std::uint32_t);
REG_PRE_ACCESS(Check);
struct Private : reg::RegEntityBundle {
    REG_BUNDLE(Private);
    REG(TABLE, std::uint32_t, reg::at(0).array(reg::dims(2, 4), reg::strides(16, 4)));
    REG(RC, std::uint32_t, reg::at(64).access(reg::rc));
    REG(MASK, std::uint32_t, reg::at(68).mask(0x00ff00ff));
    REG(WIDE, reg::u512, reg::at(128));
    REG_SCHEMA(&Private::TABLE, &Private::RC, &Private::MASK, &Private::WIDE);
    REG_ACCESS_GROUP(MAP,
                     .members = {TABLE},
                     .coordinates = {{L, 1}},
                     .derive = {reg::map_index(L, P).using_resolver(P)});
};
struct Shared : reg::RegEntityBundle {
    REG_BUNDLE(Shared);
    REG(SHARED, std::uint32_t, reg::at(32));
    REG_SCHEMA(&Shared::SHARED);
};
struct Patch : reg::RegEntityBundle {
    REG_BUNDLE(Patch);
    REG(PATCH, std::uint32_t, reg::at(0));
    REG_SCHEMA(&Patch::PATCH);
};
template <bool Static>
struct Layout : reg::Bundle {
    REG_BUNDLE(Layout);
    REG_REPEAT_ENTITY(vf, Private, 4, reg::entity().reset_per_instance(reg::ResetScope::Vf));
    REG_ENTITY(shared, Shared, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_ENTITY(patch, Patch);
    void configure_layout(reg::RegisterMap& map) {
        if constexpr (Static) {
            static const auto plan = reg::detail::build_formula_layout_runtime<Layout, [] {
                auto v = reg::static_view("DEVICE", 0x400)
                             .map_repeat("VF", 0, reg::repeat(4).stride(0x100), [](auto i) {
                                 return reg::static_view("REGS", 0x100)
                                     .identity(1)
                                     .using_resolver(Route)
                                     .using_pre_access(Check)
                                     .map(reg::target<&Layout::vf>().at(i))
                                     .map(reg::target<&Layout::shared>());
                             });
                return reg::static_address_space("ROOT")
                    .map(v, A)
                    .map(v, B)
                    .map(v.read_only_ignore_writes(), C)
                    .map(reg::static_view("PATCH", 0x100).map(reg::target<&Layout::patch>()),
                         A + 0x400,
                         2)
                    .map(reg::static_view("RESERVED", 0x100).reserve(0, 4), B + 0x100, 3);
            }>();
            CHECK(plan.mounts.size() > 0 && plan.identities.size() == 3);
            reg::detail::install_formula_layout(map, this, plan.view());
        } else {
            auto v = reg::view("DEVICE", 0x400)
                         .map_repeat("VF", 0, reg::repeat(4).stride(0x100), [&](auto i) {
                             return reg::view("REGS", 0x100)
                                 .identity(1)
                                 .using_resolver(Route)
                                 .using_pre_access(Check)
                                 .map(vf[i])
                                 .map(shared);
                         });
            reg::apply_layout(map,
                              reg::address_space("ROOT")
                                  .map(v, A)
                                  .map(v, B)
                                  .map(v.read_only_ignore_writes(), C)
                                  .map(reg::view("PATCH", 0x100).map(patch), A + 0x400, 2)
                                  .map(reg::view("RESERVED", 0x100).reserve(0, 4), B + 0x100, 3));
        }
    }
};
std::uint64_t random(std::uint64_t& x) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}
void differential() {
    reg::RegisterBank<Layout<true>> fast(reg::space().name("TEST"));
    reg::RegisterBank<Layout<false>> dynamic(reg::space().name("TEST"));
    unsigned shift = 1;
    std::uint64_t a_effect = 0, b_effect = 0;
    reg::RegAccessInfo a_info, b_info;
    auto setup = [&](auto& bank, std::uint64_t& effect, reg::RegAccessInfo& saved) {
        bank.map().set_identity_resolver(Route, [&](const auto& r) {
            return (r.request_id + shift) % 4;
        });
        bank.map().set_access_resolver(P, [&](const auto& i) {
            return (i.index_as(L) + shift + i.resolved_id()) % 4;
        });
        bank.map().set_pre_access(Check, [&](const auto& i) {
            effect += i.index.linear() + i.resolved_id();
        });
        for (unsigned v = 0; v < 4; ++v) {
            bank->vf[v].TABLE.on_write([&](std::uint32_t old, const reg::RegAccessInfo& i) {
                effect += old + i.stored_after + i.mapped_as(P);
                saved = i;
            });
            bank->vf[v].TABLE.on_read([&](std::uint32_t value, const auto& i) {
                effect += value + i.request_id();
                saved = i;
                return value ^ 0xa5a5u;
            });
            bank->vf[v].TABLE.on_read_complete([&](std::uint32_t value, const auto& i) {
                effect += value + i.index.linear();
            });
        }
        bank.finish();
    };
    setup(fast, a_effect, a_info);
    setup(dynamic, b_effect, b_info);
    dynamic.map().set_pre_access([](const auto&) {});
    CHECK(fast.map().layout_warnings().size() == dynamic.map().layout_warnings().size());
    std::uint64_t seed = 0x123456789abcdefULL;
    const unsigned offsets[] = {0, 4, 8, 12, 16, 28, 32, 48, 64, 68, 128, 132, 156, 188, 252};
    for (unsigned k = 0; k < 6000; ++k) {
        auto x = random(seed);
        shift = (x >> 32) % 4;
        auto address = (x % 3 == 0   ? A
                        : x % 3 == 1 ? B
                                     : C) +
                       ((x >> 4) % 4) * 0x100 + offsets[(x >> 8) % 15];
        unsigned width = 1u << ((x >> 16) % 4);
        address += ((x >> 24) % 4) & ~(width - 1);
        auto w1 = fast.map().mmio_write_sized(address, x, width),
             w2 = dynamic.map().mmio_write_sized(address, x, width);
        CHECK(w1.result == w2.result);
        auto r1 = fast.map().try_mmio_read_sized(address, width),
             r2 = dynamic.map().try_mmio_read_sized(address, width);
        CHECK(r1.result == r2.result && r1.value == r2.value && a_effect == b_effect);
        CHECK(a_info.address == b_info.address &&
              a_info.resolved_address == b_info.resolved_address && a_info.index == b_info.index &&
              a_info.request_index == b_info.request_index && a_info.view_path == b_info.view_path);
        if (k % 113 == 0) {
            CHECK(fast.save() == dynamic.save());
            auto saved = fast.save();
            fast.load(saved);
            dynamic.load(saved);
        }
    }
    CHECK(fast.save() == dynamic.save());
    // Read and write halves of wide transactions survive snapshots, with shared aliases.
    shift = 0;
    for (unsigned k = 0; k < 8; ++k) {
        CHECK(fast.map().mmio_write(A + 0x200 + 128 + k * 4, k + 17).success());
    }
    auto snapshot = fast.save();
    fast.load(snapshot);
    for (unsigned k = 8; k < 16; ++k)
        CHECK(fast.map().mmio_write(B + 0x200 + 128 + k * 4, k + 17).success());
    CHECK(fast->vf[2].WIDE.ref().word(0) == (std::uint64_t(18) << 32 | 17));
    CHECK(fast.map().mmio_write(B + 32, 99).success() && fast->shared.SHARED.ref() == 99 &&
          fast.map().mmio_read(C + 0x300 + 32) == 99);
    CHECK(fast.map().mmio_write(C + 32, 7).result == reg::RegMemTxResult::Ignored &&
          fast->shared.SHARED.ref() == 99);
    fast.map().clear_identity_resolver(Route);
    CHECK(fast.map().mmio_write(B + 0x200 + 4, 1).result == reg::RegMemTxResult::DeviceError);
    fast.map().set_identity_resolver(Route, [](const auto& r) {
        return r.request_id;
    });
    fast.map().clear_access_resolver(P);
    CHECK(fast.map().mmio_write(B + 0x200 + 4, 1).result == reg::RegMemTxResult::DeviceError);
    fast.map().set_access_resolver(P, [](const auto& i) {
        return i.index_as(L);
    });
    fast.map().clear_pre_access(Check);
    CHECK(fast.map().mmio_write(B + 0x200 + 4, 1).result == reg::RegMemTxResult::Ignored);
    fast.map().set_pre_access(Check, [](const auto&) {});
    CHECK(fast.map().mmio_write(B + 0x200 + 4, 1).success());
    CHECK(fast.map().describe_layout(reg::LayoutDetail::Full).find("VF") != std::string::npos);
    CHECK(fast.map().explain_address(B + 32).find("SHARED") != std::string::npos);
}
struct Macro : reg::Bundle {
    REG_BUNDLE(Macro);
    REG_ENTITY(regs, Shared);
    REG_STATIC_LAYOUT([] {
        return reg::static_address_space("MACRO").map(
            reg::static_view("REGS", 64).map(reg::target<&Macro::regs>()), 0x1000);
    });
};
struct Indexed : reg::RegEntityBundle {
    REG_BUNDLE(Indexed);
    REG(TABLE, std::uint32_t, reg::at(0).array(reg::dims(3, 4), reg::strides(16, 4)));
    REG_STATIC_INDEX(MAP, TABLE, L, P, 0);
    REG_SCHEMA(&Indexed::TABLE);
};
struct IndexedRoot : reg::Bundle {
    REG_BUNDLE(IndexedRoot);
    REG_ENTITY(regs, Indexed);
    REG_STATIC_LAYOUT([] {
        return reg::static_address_space("INDEXED").map(
            reg::static_view("REGS", 64).map(reg::target<&IndexedRoot::regs>()), A);
    });
};
void static_index() {
    reg::RegisterBank<IndexedRoot> b(reg::space().name("INDEX")),
        generic(reg::space().name("INDEX"));
    unsigned shift = 1;
    reg::RegAccessInfo saved, other;
    unsigned bcalls = 0, gcalls = 0;
    generic.map().set_pre_access([](const auto&) {});
    auto setup = [&](auto& bank, reg::RegAccessInfo& info, unsigned& calls) {
        bank.map().set_access_resolver(P, [&, count = 0](const auto& i) mutable {
            calls = ++count;
            return (i.index_as(L) + shift) % 3;
        });
        bank->regs.TABLE.on_write([&](std::uint32_t old, const reg::RegAccessInfo& i) {
            CHECK(i.stored_before == old && i.mapped_as(P) == i.index.values[0]);
            info = i;
        });
        bank->regs.TABLE.on_read([&info, count = 0](std::uint32_t value, const auto& i) mutable {
            info = i;
            return value ^ std::uint32_t(++count);
        });
        bank.finish();
    };
    setup(b, saved, bcalls);
    setup(generic, other, gcalls);
    std::uint64_t seed = 0x13579abcdefULL;
    for (unsigned k = 0; k < 2500; ++k) {
        auto x = random(seed);
        shift = (x >> 20) % 3;
        const auto logical = x % 3, col = (x >> 8) % 4;
        const unsigned width = 1u << (k % 3);
        const auto address = A + logical * 16 + col * 4;
        CHECK(b.map().mmio_write_sized(address, x, width).result ==
              generic.map().mmio_write_sized(address, x, width).result);
        auto a = b.map().try_mmio_read_sized(address, width),
             g = generic.map().try_mmio_read_sized(address, width);
        CHECK(a.result == g.result && a.value == g.value && bcalls == gcalls);
        CHECK(saved.index == other.index && saved.request_index == other.request_index &&
              saved.stored_before == other.stored_before &&
              saved.stored_after == other.stored_after &&
              saved.mapped_as(P) == other.mapped_as(P) && saved.access_group == "MAP");
        if (k % 97 == 0)
            CHECK(b.save() == generic.save());
    }
    auto copy = saved;
    auto state = b.save();
    b.reset();
    b.load(state);
    CHECK(saved.index == copy.index);
    b.map().set_access_resolver(P, [](const auto&) {
        return 9u;
    });
    CHECK(b.map().mmio_write(A, 1).result == reg::RegMemTxResult::AccessError);
    b.map().clear_access_resolver(P);
    CHECK(b.map().mmio_write(A, 1).result == reg::RegMemTxResult::DeviceError);
    // A resolver can add a global hook during this very MMIO. It runs once, with
    // complete metadata, without executing the resolver a second time.
    unsigned resolves = 0, hooks = 0;
    b.map().set_access_resolver(P, [&](const auto& i) {
        ++resolves;
        b.map().set_pre_access([&](const auto& p) {
            ++hooks;
            CHECK(p.mapped_as(P) == p.index.values[0]);
        });
        return i.index_as(L);
    });
    CHECK(b.map().mmio_write(A, 7).success() && resolves == 1 && hooks == 1);
    b.map().clear_pre_access();
}

struct Incomplete : reg::RegEntityBundle {
    REG_BUNDLE(Incomplete);
    REG(X, std::uint32_t, reg::at(0));
    REG(Y, std::uint32_t, reg::at(4));
    REG_SCHEMA(&Incomplete::X);
};
struct IncompleteRoot : reg::Bundle {
    REG_BUNDLE(IncompleteRoot);
    REG_ENTITY(regs, Incomplete);
    REG_STATIC_LAYOUT([] {
        return reg::static_address_space("BAD").map(
            reg::static_view("REGS", 64).map(reg::target<&IncompleteRoot::regs>()), 0);
    });
};
void schema_guard() {
    bool caught = false;
    try {
        reg::RegisterBank<IncompleteRoot> b(reg::space(64));
    } catch (const reg::Error& e) {
        caught = std::string(e.what()).find("REG_SCHEMA") != std::string::npos;
    }
    CHECK(caught);
}
struct ManualEntity : reg::RegEntityBundle {
    explicit ManualEntity(reg::RegScope scope) : RegEntityBundle(std::move(scope)) {
    }
    REG(VALUE, std::uint32_t, reg::at(0));
};
struct ManualRoot : reg::Bundle {
    explicit ManualRoot(reg::RegScope scope) : Bundle(std::move(scope)) {
    }
    REG_ENTITY(regs, ManualEntity);
    REG_LAYOUT([&] {
        return reg::address_space("MANUAL").map(reg::view("REGS", 64).map(regs), 0);
    });
};
void manual_constructors() {
    reg::RegisterBank<ManualRoot> b(reg::space(64));
    b.finish();
    CHECK(b.map().mmio_write(0, 123).success() && b->regs.VALUE.ref() == 123);
}
struct Inner : reg::RegEntityBundle {
    REG_BUNDLE(Inner);
    REG(X, std::uint32_t, reg::at(4));
    REG_SCHEMA(&Inner::X);
};
struct Outer : reg::RegEntityBundle {
    REG_BUNDLE(Outer);
    REG_REPEAT_GROUP(inner, Inner, 2, 0x40, 0x20, 0x20);
    REG_SCHEMA(&Outer::inner);
};
struct NestedRoot : reg::Bundle {
    REG_BUNDLE(NestedRoot);
    REG_ENTITY(outer, Outer);
    REG_STATIC_LAYOUT([] {
        return reg::static_address_space("NESTED")
            .map(reg::static_view("REGS", 0x100).map(reg::target<&NestedRoot::outer>()), A)
            .map(reg::static_view("REGS", 0x100).map(reg::target<&NestedRoot::outer>()), B);
    });
};
void nested_groups() {
    reg::RegisterBank<NestedRoot> b(reg::space());
    b.finish();
    CHECK(b.map().mmio_write(A + 0x64, 55).success() && b->outer.inner[1].X.ref() == 55 &&
          b.map().mmio_read(B + 0x64) == 55);
    CHECK(b->outer.inner[1].X.address() == A + 0x64);
}
struct DenseEntity : reg::RegEntityBundle {
    REG_BUNDLE(DenseEntity);
    REG(TABLE, std::uint32_t, reg::at(0).array(reg::dims(512), reg::strides(8)));
    REG_SCHEMA(&DenseEntity::TABLE);
};
struct DenseRoot : reg::Bundle {
    REG_BUNDLE(DenseRoot);
    REG_ENTITY(regs, DenseEntity);
    void configure_layout(reg::RegisterMap& map) {
        static const auto plan = reg::detail::build_formula_layout_runtime<DenseRoot, [] {
            auto v = reg::static_view("DENSE", 4096).map(reg::target<&DenseRoot::regs>());
            return reg::static_address_space("DENSE").map(v, A).map(v, B);
        }>();
        CHECK(plan.mounts.size() == 2 && plan.arrays.size() == 1);
        reg::detail::install_formula_layout(map, this, plan.view());
    }
};
void dense_pages() {
    reg::RegisterBank<DenseRoot> b(reg::space());
    b.finish();
    CHECK(b.map().mmio_write(A + 16, 22).success() && b->regs.TABLE.at(2).ref() == 22 &&
          b.map().mmio_read(B + 16) == 22);
    CHECK(b.map().try_mmio_read_sized(A + 4, 4).result == reg::RegMemTxResult::DecodeError);
    CHECK(b.map().try_mmio_read_sized(B + 4092, 4).result == reg::RegMemTxResult::DecodeError);
    CHECK(b.map().try_mmio_read_sized(A + 4096, 4).result == reg::RegMemTxResult::DecodeError);
}
void macro_and_instances() {
    reg::RegisterBank<Macro> a(reg::space(0x2000).name("ONE")), b(reg::space(0x2000).name("TWO"));
    a.finish();
    b.finish();
    CHECK(a.map().mmio_write(0x1020, 77).success() && a->regs.SHARED.ref() == 77 &&
          b->regs.SHARED.ref() == 0);
    a.clear();
    CHECK(b.map().mmio_write(0x1020, 88).success() && b->regs.SHARED.ref() == 88);
}
} // namespace
int main() {
    try {
        differential();
        static_index();
        schema_guard();
        macro_and_instances();
        manual_constructors();
        nested_groups();
        dense_pages();
        std::cout << "PASS: consteval layout / dynamic differential, live hooks, overlays, "
                     "aliases, wide snapshot, independent instances\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
