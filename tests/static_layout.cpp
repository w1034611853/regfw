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
template <class F>
void frozen(F&& f) {
    bool caught = false;
    try {
        f();
    } catch (const reg::Error& e) {
        caught = e.kind == reg::ErrorKind::Configuration;
    }
    CHECK(caught);
}

// Explicit descriptors and schemas remain supported. The full static/dynamic
// differential is exercised through the symbolic frontend in symbolic_layout.
constexpr std::uint64_t A = 0xff71f0000000ULL, B = 0xc100000000ULL;
REG_ACCESS_FIELD(L, std::uint32_t);
REG_ACCESS_FIELD(P, std::uint32_t);
struct Shared : reg::RegEntityBundle {
    REG_BUNDLE(Shared);
    REG(SHARED, std::uint32_t, reg::at(32));
    REG_SCHEMA(&Shared::SHARED);
};
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
std::uint64_t random(std::uint64_t& x) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}
void static_index() {
    reg::RegisterBank<IndexedRoot> b(reg::space().name("INDEX")),
        generic(reg::space().name("INDEX"));
    unsigned shift = 1;
    reg::RegAccessInfo saved, other;
    unsigned bcalls = 0, gcalls = 0;
    bool invalid = false;
    generic.map().set_pre_access([](const auto&) {});
    auto setup = [&](auto& bank, reg::RegAccessInfo& info, unsigned& calls) {
        bank.map().set_access_resolver(P, [&, count = 0](const auto& i) mutable {
            calls = ++count;
            return invalid ? 9u : (i.index_as(L) + shift) % 3;
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
    invalid = true;
    CHECK(b.map().mmio_write(A, 1).result == reg::RegMemTxResult::AccessError);
    invalid = false;
    frozen([&] {
        b.map().clear_access_resolver(P);
    });
    frozen([&] {
        b.map().set_access_resolver(P, [](const auto&) {
            return 0u;
        });
    });
    frozen([&] {
        b.map().set_pre_access([](const auto&) {});
    });
    CHECK(b.map().mmio_write(A, 7).success());
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
        static_index();
        schema_guard();
        macro_and_instances();
        nested_groups();
        std::cout << "PASS: explicit schemas and static targets, index mapping, nested "
                     "groups and independent instances\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
