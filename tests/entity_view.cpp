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
struct Window : reg::RegViewBundle {
    REG_BUNDLE(Window);
};
struct Readonly : reg::RegViewBundle {
    REG_BUNDLE(Readonly);
};
struct Doorbell : reg::RegEntityBundle {
    REG_BUNDLE(Doorbell);
    REG(VALUE, std::uint32_t, reg::at(0));
};
struct Layout : reg::Bundle {
    REG_BUNDLE(Layout);
    REG_ENTITY(shared, Shared, reg::aperture().at(0).size_bytes(0x100));
    REG_REPEAT_ENTITY(vf,
                      Business,
                      2,
                      reg::aperture().at(0x1000).size_bytes(0x100).stride(0x100).reset_per_instance(
                          reg::ResetScope::Vf));
    REG_REPEAT_VIEW(windows,
                    Window,
                    2,
                    reg::aperture().at(0x2000).size_bytes(0x200).stride(0x200).reset_per_instance(
                        reg::ResetScope::Vf));
    REG_VIEW(readonly, Readonly, reg::aperture().at(0x3000).size_bytes(0x100));
    REG_REPEAT_ENTITY(doorbells, Doorbell, 2, reg::aperture().at(0x3100).size_bytes(4).stride(4));
    REG_LAYOUT(.views =
                   {
                       reg::view(windows).at(0).size_bytes(0x100).to(vf).vf_to_pf(),
                       reg::view(vf).at(0x40).size_bytes(4).to(shared.VALUE).pf_to_vf(),
                       reg::view(readonly)
                           .at(0)
                           .size_bytes(0x100)
                           .to(shared)
                           .read_only_ignore_writes(),
                       reg::view(reg::PhysicalAddressWindow{0x2100, 4}).to(doorbells[0].VALUE),
                       reg::view(reg::PhysicalAddressWindow{0x2300, 4}).to(doorbells[1].VALUE),
                   },
               .identities = {reg::identity(vf, 1), reg::identity(windows, 1)});
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
    reg::RegisterBank<Layout> bank(reg::space(0x4000).name("ENTITY_VIEW").finalize());
    auto& map = bank.map();
    CHECK(map.entity_count() == 7 && map.total_node_data_count() == 17);
    CHECK(map.mapped_view_count() == 14);
    auto initial = map.dump_state_versioned();
    CHECK(initial.schema.slots.size() == 7 && initial.data.size() == 17);
    unsigned writes = 0;
    bank->shared.VALUE.on_write([&] {
        ++writes;
    });
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
    unsigned commands = 0;
    for (unsigned i = 0; i < 2; ++i)
        bank->doorbells[i].VALUE.on_write([&, i](const reg::RegAccessInfo& info) {
            CHECK(info.request_id() == 0 && info.resolved_id() == i);
            ++commands;
        });
    map.set_identity_resolver([](const auto& request) {
        return request.request_id == 0 ? 1u : request.request_id;
    });
    CHECK(map.mmio_write(0x2000, 55).success() && bank->vf[1].VALUE.ref() == 55 &&
          bank->vf[0].VALUE.ref() == 10);
    CHECK(map.mmio_write(0x2100, 1).success() && commands == 1);
    CHECK(map.mmio_read(0x1000) == 55);
    map.reset_matching({reg::ResetRequest::VfFlr, 1});
    CHECK(bank->vf[1].VALUE.ref() == 0 && bank->vf[0].VALUE.ref() == 10 &&
          bank->shared.VALUE.ref() == 21);
    map.restore_state_versioned(initial);
    CHECK(bank->shared.VALUE.ref() == 7 && map.entity_count() == 7 &&
          map.mapped_view_count() == 14);
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
    moved.clear();
    CHECK(!copy.is_alive());
    fails(
        [&] {
            (void)copy.entity();
        },
        reg::ErrorKind::Device);
    fails(
        [&] {
            (void)copy.data();
        },
        reg::ErrorKind::Device);
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
                {.views = {reg::view(reg::PhysicalAddressWindow{0x40, 4}).to(second->VALUE)}});
        },
        reg::ErrorKind::Configuration);
    auto stale = reg::view(reg::PhysicalAddressWindow{0x40, 4}).to(second->VALUE);
    second.clear();
    fails(
        [&] {
            reg::apply_layout(first.map(), {.views = {stale}});
        },
        reg::ErrorKind::Configuration);
}
} // namespace
int main() try {
    automatic_ownership_and_mapping();
    span_bounds_and_lifetime();
    invalid_views();
    std::cout << "PASS: automatic RegEntity/RegView ownership, repeat/shared routing, permissions, "
                 "commands, snapshots, reset, bounded slices and lifetime checks\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
