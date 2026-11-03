#include <regfw/regfw.hpp>
#include <iostream>
#include <stdexcept>
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!(__VA_ARGS__))                                                                        \
            throw std::runtime_error(#__VA_ARGS__);                                                \
    } while (false)
struct Leaf : reg::RegEntityBundle {
    REG_BUNDLE(Leaf);
    REG(VALUE, std::uint32_t, reg::at(0));
};
struct Storage : reg::Bundle {
    REG_BUNDLE(Storage);
    REG_ENTITY(one, Leaf);
    REG_REPEAT_ENTITY(many, Leaf, 4);
};
template <class F>
void fails(F f) {
    bool error = false;
    try {
        f();
    } catch (const reg::Error& e) {
        error = e.kind == reg::ErrorKind::Configuration;
    }
    CHECK(error);
}
int main() try {
    reg::RegisterBank<Storage> bank(reg::space(0x4000));
    auto& map = bank.map();
    auto invalid = [&](const reg::AddressSpace& space) {
        fails([&] {
            reg::apply_layout(map, space);
        });
    };
    invalid(
        reg::address_space("ROOT").map(reg::view("SMALL", 4).map(reg::view("OUTSIDE", 4), 4), 0));
    invalid(reg::address_space("ROOT").map(reg::view("OFFSET", 4).map(bank->one), 1));
    invalid(reg::address_space("ROOT").map(reg::view("ZERO", 0), 0));
    invalid(reg::address_space("ROOT").map(
        reg::view("BAD_ID", 4)
            .map(bank->one)
            .using_resolver(reg::FieldKey<struct Tag, std::uint32_t>{"R"}),
        0));
    auto repeat = [&](reg::RepeatSpec spec) {
        return reg::address_space("R").map_repeat("V", 0, spec, [&](auto i) {
            return reg::view("I", 8).map(bank->many[i]);
        });
    };
    fails([&] {
        (void)repeat(reg::repeat(0).stride(4));
    });
    fails([&] {
        (void)repeat(reg::repeat(UINT64_MAX).stride(4));
    });
    fails([&] {
        (void)repeat(reg::repeat(2).stride(3));
    });
    invalid(reg::address_space("ROOT").map_repeat(
        "DIFFERENT", 0, reg::repeat(2).stride(0x100), [&](auto i) {
            return reg::view(i ? "B" : "A", 4).map(bank->many[i]);
        }));
    auto local = [&](reg::RepeatSpec spec) {
        return reg::view("PARENT", 0x400).map_repeat("VF", 0x100, spec, [&](auto i) {
            return reg::view("I", 8).map(bank->many[i]);
        });
    };
    fails([&] {
        (void)local(reg::repeat(0).stride(4));
    });
    fails([&] {
        (void)local(reg::repeat(UINT64_MAX).stride(4));
    });
    fails([&] {
        (void)local(reg::repeat(2).stride(3));
    });
    fails([&] {
        (void)local(reg::repeat(4).stride(0x100));
    });
    invalid(reg::address_space("ROOT").map(
        reg::view("PARENT", 0x400)
            .map_repeat("DIFFERENT",
                        0,
                        reg::repeat(2).stride(0x100),
                        [&](auto i) {
                            return reg::view(i ? "B" : "A", 4).map(bank->many[i]);
                        }),
        0));
    reg::RegisterBank<Storage> other(reg::space(0x4000));
    invalid(reg::address_space("ROOT").map(reg::view("FOREIGN", 0x100).map(other->one), 0));
    auto stale = reg::view("STALE", 0x100).map(other->one);
    other.clear();
    invalid(reg::address_space("ROOT").map(stale, 0));
    reg::apply_layout(map,
                      reg::address_space("ROOT").map_repeat(
                          "VF", 0x1000, reg::repeat(4).stride(0x100), [&](auto i) {
                              return reg::view("I", 0x100).map(bank->many[i]);
                          }));
    bank.finish();
    CHECK(map.mmio_write(0x1200, 99).success());
    CHECK(bank->many[2].VALUE.ref() == 99);
    fails([&] {
        reg::apply_layout(map, reg::address_space("LATE").map(reg::view("V", 4).map(bank->one), 0));
    });
    std::cout << "PASS: view boundaries, alignment, repeat shape/count/stride, live same-bank "
                 "ownership, stale handles and frozen layout\n";
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
