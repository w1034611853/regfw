#include <regfw/regfw.hpp>
#include <iostream>
#include <stdexcept>
#include <type_traits>

#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!(__VA_ARGS__))                                                                        \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " +      \
                                     #__VA_ARGS__);                                                \
    } while (false)
namespace {
struct Shared : reg::RegEntityBundle {
    REG_BUNDLE(Shared);
    REG(VALUE, std::uint32_t, reg::at(0).reset(7));
};
struct Business : reg::RegEntityBundle {
    REG_BUNDLE(Business);
    REG(VALUE, std::uint32_t, reg::at(0));
    REG(ITEMS, std::uint32_t, reg::at(0x10).array(reg::dims(6), reg::strides(8)));
};
union NativeCounter {
    std::uint32_t u32;
    struct {
        std::uint32_t count : 24;
        std::uint32_t flags : 8;
    } bits;
};
struct NativeArrays : reg::RegEntityBundle {
    REG_BUNDLE(NativeArrays);
    REG(ITEMS, NativeCounter, reg::at(0).array(reg::dims(6), reg::strides(8)));
    REG(GRID, NativeCounter, reg::at(0x40).array(reg::dims(2, 3), reg::strides(32, 8)));
};
struct Doorbell : reg::RegEntityBundle {
    REG_BUNDLE(Doorbell);
    REG(VALUE, std::uint32_t, reg::at(0));
};
struct Layout : reg::Bundle {
    REG_BUNDLE(Layout);
    REG_ENTITY(shared, Shared);
    REG_REPEAT_ENTITY(vf, Business, 2, reg::entity().reset_per_instance(reg::ResetScope::Vf));
    REG_REPEAT_ENTITY(doorbells, Doorbell, 2);
    REG_LAYOUT([&] {
        return reg::address_space("IO")
            .map(reg::view("SHARED", 4).map(shared), 0)
            .map_repeat(
                "DIRECT_VF",
                0x1000,
                reg::repeat(2).stride(0x100),
                [&](auto i) {
                    return reg::view("VF", 0x100).map(vf[i]).map(shared.VALUE, 0x40).identity(1);
                })
            .map_repeat("WINDOWS",
                        0x2000,
                        reg::repeat(2).stride(0x200),
                        [&](auto i) {
                            return reg::view("WINDOW", 0x200)
                                .map(vf[i])
                                .map(shared.VALUE, 0x40)
                                .map(doorbells[i].VALUE, 0x100)
                                .identity(1);
                        })
            .map(reg::view("RO", 4).map(shared).read_only_ignore_writes(), 0x3000);
    });
};
template <class F>
void fails(F&& action, reg::ErrorKind kind) {
    bool caught = false;
    try {
        action();
    } catch (const reg::Error& error) {
        caught = true;
        CHECK(error.kind == kind);
    }
    CHECK(caught);
}
void automatic_ownership_and_mapping() {
    static_assert(!std::is_copy_constructible_v<reg::RegEntity>);
    static_assert(std::same_as<reg::TypedReg<std::uint32_t, 1, false>,
                               reg::RegView<std::uint32_t, 1, false>>);
    reg::RegisterBank<Layout> bank(reg::space(0x4000).name("ENTITY_VIEW"));
    auto& map = bank.map();
    CHECK(map.entity_count() == 7 && map.total_node_data_count() == 17);
    CHECK(map.mapped_view_count() == 7);
    auto initial = map.dump_state_versioned();
    CHECK(initial.schema.slots.size() == 7 && initial.data.size() == 17);
    unsigned writes = 0;
    bank->shared.VALUE.on_write([&] {
        ++writes;
    });
    bool redirect = false;
    unsigned commands = 0;
    for (unsigned i = 0; i < 2; ++i)
        bank->doorbells[i].VALUE.on_write([&, i](const reg::RegAccessInfo& info) {
            CHECK(info.request_id() == 0 && info.resolved_id() == i);
            ++commands;
        });
    map.set_identity_resolver([&](const auto& request) {
        return redirect && request.request_id == 0 ? 1u : request.request_id;
    });
    bank.finish();
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(map.mmio_write(0x2000 + i * 0x200, 10 + i).success());
        CHECK(bank->vf[i].VALUE.ref() == 10 + i);
        CHECK(map.mmio_read(0x1000 + i * 0x100) == 10 + i);
        CHECK(map.mmio_write(0x2040 + i * 0x200, 20 + i).success());
        CHECK(map.mmio_read(0x1040) == 20 + i && map.mmio_read(0x1140) == 20 + i);
    }
    CHECK(writes == 2 && bank->shared.VALUE.ref() == 21);
    CHECK(map.mmio_write(0x3000, 99).result == reg::RegMemTxResult::Ignored);
    CHECK(map.mmio_read(0x3000) == 21 && writes == 2);
    CHECK(map.try_mmio_read_sized(0x2014, 4).result == reg::RegMemTxResult::DecodeError);
    redirect = true;
    CHECK(map.mmio_write(0x2000, 55).success() && bank->vf[1].VALUE.ref() == 55 &&
          bank->vf[0].VALUE.ref() == 10);
    CHECK(map.mmio_write(0x2100, 1).success() && commands == 1);
    CHECK(map.mmio_read(0x1000) == 55);
    map.reset_matching({reg::ResetRequest::VfFlr, 1});
    CHECK(bank->vf[1].VALUE.ref() == 0 && bank->vf[0].VALUE.ref() == 10 &&
          bank->shared.VALUE.ref() == 21);
    map.restore_state_versioned(initial);
    CHECK(bank->shared.VALUE.ref() == 7 && map.entity_count() == 7 && map.mapped_view_count() == 7);
}
void span_bounds_and_lifetime() {
    reg::RegisterBank<Layout> bank(reg::space(0x4000).name("SPANS").finalize());
    auto root = bank->vf[0].ITEMS;
    auto middle = root.subspan(2, 3);
    CHECK(middle.size() == 3 && middle.size_bytes() == 12);
    CHECK(&middle.entity() == &root.entity());
    CHECK(root.entity().element_count() == 6 && root.entity().storage_bytes() == 24 &&
          root.entity().width_bits() == 32);
    CHECK(middle.address() == 0x1020 && middle.address_span_bytes() == 20);
    CHECK(middle.data() == &root.at(2).ref());
    middle.at(0).ref() = 31;
    middle.at_element(2).ref() = 49;
    CHECK(root.at(2).ref() == 31 && root.at(4).ref() == 49);
    CHECK(root.at(2).size() == 1);
    CHECK(bank.map().mmio_read(0x2020) == 31);
    fails(
        [&] {
            (void)middle.at(3);
        },
        reg::ErrorKind::Access);
    fails(
        [&] {
            (void)middle.subspan(4);
        },
        reg::ErrorKind::Access);
    fails(
        [&] {
            (void)middle.subspan(1, 3);
        },
        reg::ErrorKind::Access);
    auto empty = root.subspan(root.size());
    CHECK(empty.empty() && empty.data() == nullptr && empty.address_span_bytes() == 0);
    CHECK(&empty.entity() == &root.entity());
    fails(
        [&] {
            (void)empty.address();
        },
        reg::ErrorKind::Access);
    fails(
        [&] {
            (void)empty.at(0);
        },
        reg::ErrorKind::Access);
    reg::RegView<std::uint32_t, 1, false> unbound;
    CHECK(unbound.empty() && unbound.data() == nullptr);
    CHECK(unbound.subspan(0).empty());
    auto copy = middle;
    const auto views = bank.map().mapped_view_count();
    auto moved = std::move(bank);
    copy.at(1).ref() = 42;
    CHECK(moved->vf[0].ITEMS.at(3).ref() == 42 && moved.map().mapped_view_count() == views);
    CHECK(moved.map().entity_count() == 7);
    CHECK(copy.is_alive() && copy.at(1).ref() == 42);
    // Views borrow storage; callers stop using them before clearing the bank.
}
void native_element_views() {
    using Element = reg::RegView<NativeCounter>;
    std::optional<Element> saved;
    {
        reg::RegisterBank<NativeArrays> bank(reg::space(0x100).name("NATIVE"));
        auto& items = bank->ITEMS;
        auto slice = items.subspan(1, 4).subspan(1, 2);
        saved.emplace(slice.at(1));
        saved->bits.count = 31;
        CHECK(items.at(3).ref().u32 == 31);
        auto copied = *saved;
        auto moved = std::move(copied);
        CHECK(copied.is_alive() && copied.bits.count == 31);
        moved.bits.flags = 7;
        CHECK(items.at_element(3).u32 == (7u << 24 | 31));
        moved = items.at(0);
        moved.u32 = 17;
        CHECK(items.data()[0].u32 == 17 && saved->bits.count == 31);
        auto grid_element = bank->GRID.at(1, 2);
        grid_element.bits.count = 42;
        CHECK(bank->GRID.at_element(5).ref().u32 == 42);
        CHECK(bank.map().mmio_read(0x40 + 32 + 16) == 42);
        fails(
            [&] {
                (void)bank->GRID.at(2, 0);
            },
            reg::ErrorKind::Access);
        fails(
            [&] {
                (void)bank->GRID.at(0, 3);
            },
            reg::ErrorKind::Access);
        fails(
            [&] {
                (void)items.at(-1);
            },
            reg::ErrorKind::Access);
        auto empty = slice.subspan(2).subspan(0);
        CHECK(empty.data() == nullptr && empty.size() == 0);
        auto other_bank = std::move(bank);
        CHECK(saved->is_alive());
    }
    // Destruction of a borrowed view does not touch the bank.
    saved.reset();
}
struct BadView : reg::RegViewBundle {
    REG_BUNDLE(BadView);
    REG(ACCIDENTAL_STORAGE, std::uint32_t, reg::at(0));
};
struct NestedBadView : reg::RegViewBundle {
    REG_BUNDLE(NestedBadView);
    REG_GROUP(child, Business, 0, 0x100);
};
void invalid_views() {
    fails(
        [] {
            reg::RegisterBank<BadView> bad(reg::space(0x100).name("BAD"));
        },
        reg::ErrorKind::Configuration);
    fails(
        [] {
            reg::RegisterBank<NestedBadView> bad(reg::space(0x100).name("NESTED_BAD"));
        },
        reg::ErrorKind::Configuration);
    reg::RegisterBank<Shared> first(reg::space(0x100).name("FIRST"));
    reg::RegisterBank<Shared> second(reg::space(0x100).name("SECOND"));
    fails(
        [&] {
            reg::apply_layout(
                first.map(),
                reg::address_space("CROSS_MAP").map(reg::view("BAD", 4).map(second->VALUE), 0x40));
        },
        reg::ErrorKind::Configuration);
    auto stale = reg::address_space("STALE").map(reg::view("BAD", 4).map(second->VALUE), 0x40);
    second.clear();
    fails(
        [&] {
            reg::apply_layout(first.map(), stale);
        },
        reg::ErrorKind::Configuration);
}
} // namespace
int main() try {
    automatic_ownership_and_mapping();
    span_bounds_and_lifetime();
    native_element_views();
    invalid_views();
    std::cout << "PASS: automatic RegEntity/RegView ownership, repeat/shared routing, permissions, "
                 "commands, snapshots, reset, bounded slices and lifetime checks\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
