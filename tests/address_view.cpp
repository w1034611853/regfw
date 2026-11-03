#include <regfw/regfw.hpp>
#include <regfw/qemu_bridge.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!(__VA_ARGS__))                                                                        \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " +      \
                                     #__VA_ARGS__);                                                \
    } while (false)
namespace {
constexpr std::uint64_t A = 0xff71f0000000ULL, B = 0xc100000000ULL, STEP = 0x200000;
constexpr unsigned COUNT = 8;
REG_ACCESS_FIELD(VfRoute, std::uint32_t);
REG_ACCESS_FIELD(Logical, std::uint32_t);
REG_ACCESS_FIELD(Physical, std::uint32_t);
struct Module : reg::RegEntityBundle {
    REG_BUNDLE(Module);
    REG(ID, std::uint32_t, reg::at(0).reset(0x1234));
};
struct Shared : reg::RegEntityBundle {
    REG_BUNDLE(Shared);
    REG(VALUE, std::uint32_t, reg::at(0x40).reset(7));
};
struct Private : reg::RegEntityBundle {
    REG_BUNDLE(Private);
    REG(VALUE, std::uint32_t, reg::at(0).reset(3));
    REG(HOLE_NEIGHBOR, std::uint32_t, reg::at(0x80).reset(9));
    REG(PACKET, reg::u512, reg::at(0x100));
    REG(QUEUE, std::uint32_t, reg::at(0x200).array(reg::dims(2, 4), reg::strides(0x20, 4)));
    REG_ACCESS_GROUP(QUEUES,
                     .members = {QUEUE},
                     .coordinates = {{Logical, 1}},
                     .derive = {reg::map_index(Logical, Physical).using_resolver(Physical)});
};
struct Layout : reg::Bundle {
    REG_BUNDLE(Layout);
    REG_ENTITY(module, Module, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_ENTITY(shared, Shared, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_REPEAT_ENTITY(vf, Private, COUNT, reg::entity().reset_per_instance(reg::ResetScope::Vf));
    REG_LAYOUT([&] {
        const auto module_ro =
            reg::view("MODULE_RO", 0x20000).map(module).read_only_ignore_writes();
        const auto pf_shared = reg::view("PF_SHARED", 0xe0000).map(shared);
        auto vf_view = [&](auto i) {
            const auto business = reg::view("BUSINESS", 0xe0000).map(vf[i]).map(pf_shared, 0, 10);
            return reg::view("VF_WINDOW", STEP)
                .identity(1)
                .using_resolver(VfRoute)
                .vf_to_pf()
                .map(module_ro)
                .map(business, 0x20000);
        };
        return reg::address_space("POE_CPU")
            .map_repeat("ENTRY_A", A, reg::repeat(COUNT).stride(STEP), vf_view)
            .map_repeat("ENTRY_B", B, reg::repeat(COUNT).stride(STEP), vf_view);
    });
};
void high_addresses_and_shared_state() {
    reg::RegisterBank<Layout> bank(reg::space().name("POE_CPU"));
    auto& map = bank.map();
    std::array<std::uint32_t, COUNT> route{0, 1, 2, 3, 4, 5, 6, 7};
    unsigned routes = 0, writes = 0, read_calls = 0, completed = 0;
    std::uint64_t last_bus = 0, last_canonical = 0;
    reg::RegAccessInfo last_info;
    map.set_identity_resolver(VfRoute, [&](const reg::IdentityResolveRequest& r) {
        ++routes;
        last_bus = r.bus_offset;
        last_canonical = r.canonical_offset;
        return route.at(r.request_id);
    });
    map.set_access_resolver(Physical, [](const auto& info) {
        return (info.index_as(Logical) + 1) % 4;
    });
    bank->vf[6].VALUE.on_write([&](const reg::RegAccessInfo& info) {
        ++writes;
        last_info = info;
    });
    bank->vf[6].PACKET.on_write([&] {
        ++completed;
    });
    bank->vf[6].PACKET.on_read([&] {
        ++read_calls;
        auto v = reg::u512{};
        for (unsigned i = 0; i < 8; ++i)
            v.set_word(i, 0x100 + i);
        return v;
    });
    bank.finish();
    CHECK(map.layout_warnings().empty());
    CHECK(bank->vf[0].VALUE.address() == A + 0x20000);
    CHECK(map.mmio_write(B + STEP + 0x20000, 42).success());
    CHECK(map.mmio_read(A + STEP + 0x20000) == 42);
    CHECK(bank->vf[1].VALUE.ref() == 42 && bank->vf[0].VALUE.ref() == 3);
    CHECK(last_bus == A + STEP + 0x20000 && last_canonical == A + STEP + 0x20000);
    CHECK(map.mmio_write(B + 3 * STEP + 0x20040, 99).success());
    CHECK(map.mmio_read(A + 7 * STEP + 0x20040) == 99);
    CHECK(map.mmio_read(B + 2 * STEP) == 0x1234);
    CHECK(map.mmio_write(B + 2 * STEP, 0).result == reg::RegMemTxResult::Ignored);
    CHECK(bank->module.ID.ref() == 0x1234);
    CHECK(map.mmio_read(B + STEP + 0x20080) == 9); // PF view hole falls through to VF.
    CHECK(map.try_mmio_read_sized(B + 0x20044, 4).result == reg::RegMemTxResult::DecodeError);
    CHECK(map.try_mmio_read_sized(B & 0xffffffff, 4).result == reg::RegMemTxResult::DecodeError);
    route[2] = 6;
    const auto p = A + 2 * STEP + 0x20000, q = B + 2 * STEP + 0x20000;
    CHECK(map.mmio_write(q, 55).success());
    CHECK(writes == 1 && bank->vf[6].VALUE.ref() == 55 && bank->vf[2].VALUE.ref() == 3);
    CHECK(last_bus == q && last_canonical == p && last_info.address == q &&
          last_info.resolved_address == A + 6 * STEP + 0x20000);
    CHECK(last_info.request_id() == 2 && last_info.resolved_id() == 6);
    CHECK(map.mmio_write(q + 0x200 + 0x20 + 2 * 4, 123).success());
    CHECK(bank->vf[6].QUEUE.at(1, 3).ref() == 123);
    // Pending wide writes and response snapshots cross the two physical aliases.
    for (unsigned i = 0; i < 8; ++i)
        CHECK(map.mmio_write(p + 0x100 + i * 4, i + 1).success());
    CHECK(map.mmio_read(p + 0x100) == 0x100);
    CHECK(read_calls == 1);
    auto state = bank.save();
    for (unsigned i = 8; i < 16; ++i)
        CHECK(map.mmio_write(q + 0x100 + i * 4, i + 1).success());
    CHECK(completed == 1);
    bank.load(state);
    for (unsigned i = 8; i < 16; ++i)
        CHECK(map.mmio_write(q + 0x100 + i * 4, i + 1).success());
    CHECK(completed == 2);
    for (unsigned i = 1; i < 16; ++i)
        CHECK(map.mmio_read(q + 0x100 + i * 4) == (i % 2 ? 0 : 0x100 + i / 2));
    CHECK(read_calls == 1);
    CHECK(bank->vf[6].PACKET.ref().word(0) == (std::uint64_t(2) << 32 | 1));
    const auto before_routes = routes;
    CHECK(map.explain_address(q, 4, reg::RegTransactionKind::Read, 6).find("0xc100420000") !=
          std::string::npos);
    CHECK(routes == before_routes);
    auto bridge = reg::make_qemu_bridge(map);
    std::uint64_t value = 0;
    CHECK(bridge.read(bridge.opaque, q, &value, 4) == 0 && value == 55);
    CHECK(bridge.write(bridge.opaque, q, 66, 4) == 0 && bank->vf[6].VALUE.ref() == 66);
    map.reset_matching({reg::ResetRequest::VfFlr, 6});
    CHECK(bank->vf[6].VALUE.ref() == 3 && bank->shared.VALUE.ref() == 99);
    CHECK(map.describe_layout().find("ff71f") != std::string::npos);
}
struct NestedLayout : reg::Bundle {
    REG_BUNDLE(NestedLayout);
    REG_ENTITY(module, Module, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_ENTITY(shared, Shared, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_ENTITY(pf_business, Private, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_REPEAT_ENTITY(vf, Private, COUNT, reg::entity().reset_per_instance(reg::ResetScope::Vf));
    REG_LAYOUT([&] {
        const auto module_view = reg::view("MODULE", 0x20000).map(module);
        const auto shared_view = reg::view("PF_SHARED", 0xe0000).map(shared);
        auto business = [&](const auto& entity) {
            return reg::view("BUSINESS", 0xe0000).map(entity).map(shared_view);
        };
        const auto pf_view =
            reg::view("PF", 0x1000000).map(module_view).map(business(pf_business), 0x20000);
        auto pf_vf_slot = [&](auto i) {
            return reg::view("PF_VF_SLOT", 0x100000)
                .identity(1)
                .using_resolver(VfRoute)
                .pf_to_vf()
                .reserve(0, 0x20000)
                .map(business(vf[i]), 0x20000);
        };
        auto vf_window = [&](auto i) {
            return reg::view("VF_WINDOW", STEP)
                .identity(1)
                .using_resolver(VfRoute)
                .vf_to_pf()
                .map(module_view.read_only_ignore_writes())
                .map(business(vf[i]), 0x20000);
        };
        const auto poe_cpu =
            reg::view("POE_CPU", 0x2000000)
                .map(pf_view)
                .map_repeat("PF_VF", 0x200000, reg::repeat(COUNT).stride(0x100000), pf_vf_slot)
                .map_repeat("VF", 0x1000000, reg::repeat(COUNT).stride(STEP), vf_window);
        return reg::address_space("ROOT").map(poe_cpu, A).map(poe_cpu, B);
    });
};
void reusable_nested_views() {
    reg::RegisterBank<NestedLayout> b(reg::space().name("ROOT"));
    auto& m = b.map();
    unsigned writes = 0, reads = 0, completed = 0;
    bool ids_ok = true;
    reg::AccessRouteKind route_kind = reg::AccessRouteKind::Direct;
    m.set_identity_resolver(VfRoute, [](const auto& r) {
        return r.request_id == 2 ? 6 : r.request_id;
    });
    m.set_access_resolver(Physical, [](const auto& i) {
        return i.index_as(Logical);
    });
    b->vf[6].VALUE.on_write([&](const reg::RegAccessInfo& i) {
        ++writes;
        route_kind = i.route.kind;
        ids_ok = ids_ok && i.request_id() == 2 && i.resolved_id() == 6;
    });
    b->vf[6].PACKET.on_write([&] {
        ++completed;
    });
    b->vf[6].PACKET.on_read([&] {
        ++reads;
        auto v = reg::u512{};
        for (unsigned k = 0; k < 8; ++k)
            v.set_word(k, 0x900 + k);
        return v;
    });
    b.finish();
    CHECK(m.layout_warnings().empty());
    const auto slot = [](std::uint64_t base, unsigned i) {
        return base + 0x200000 + i * 0x100000 + 0x20000;
    };
    const auto guest = [](std::uint64_t base, unsigned i) {
        return base + 0x1000000 + i * STEP + 0x20000;
    };
    CHECK(m.mmio_write(slot(A, 2), 55).success());
    CHECK(writes == 1 && ids_ok && route_kind == reg::AccessRouteKind::PfToVf);
    CHECK(m.mmio_read(guest(B, 2)) == 55 && b->vf[6].VALUE.ref() == 55 &&
          b->vf[2].VALUE.ref() == 3);
    CHECK(m.mmio_write(guest(B, 2), 66).success());
    CHECK(writes == 2 && ids_ok && route_kind == reg::AccessRouteKind::VfToPf);
    CHECK(m.mmio_read(slot(B, 6)) == 66);
    CHECK(m.mmio_write(A + 0x20000, 77).success() && m.mmio_read(B + 0x20000) == 77 &&
          b->pf_business.VALUE.ref() == 77);
    CHECK(m.mmio_write(slot(B, 3) + 0x40, 88).success());
    CHECK(m.mmio_read(A + 0x20040) == 88 && m.mmio_read(guest(A, 7) + 0x40) == 88 &&
          b->shared.VALUE.ref() == 88);
    CHECK(m.mmio_write(B + 0x1000000 + 3 * STEP, 0).result == reg::RegMemTxResult::Ignored);
    CHECK(m.try_mmio_read_sized(A + 0x200000, 4).result == reg::RegMemTxResult::DecodeError);
    for (unsigned k = 0; k < 8; ++k)
        CHECK(m.mmio_write(slot(A, 2) + 0x100 + k * 4, k + 1).success());
    CHECK(m.mmio_read(guest(A, 2) + 0x100) == 0x900 && reads == 1);
    const auto saved = b.save();
    for (unsigned k = 8; k < 16; ++k)
        CHECK(m.mmio_write(guest(B, 2) + 0x100 + k * 4, k + 1).success());
    CHECK(completed == 1);
    b.load(saved);
    for (unsigned k = 8; k < 16; ++k)
        CHECK(m.mmio_write(guest(B, 2) + 0x100 + k * 4, k + 1).success());
    CHECK(completed == 2);
    for (unsigned k = 1; k < 16; ++k)
        CHECK(m.mmio_read(slot(B, 2) + 0x100 + k * 4) == (k % 2 ? 0 : 0x900 + k / 2));
    CHECK(reads == 1 && b->vf[6].PACKET.ref().word(0) == (std::uint64_t(2) << 32 | 1));
    m.reset_matching({reg::ResetRequest::VfFlr, 6});
    CHECK(b->vf[6].VALUE.ref() == 3 && b->pf_business.VALUE.ref() == 77 &&
          b->shared.VALUE.ref() == 88);
    const auto report = m.describe_layout(reg::LayoutDetail::Full);
    CHECK(report.find("PF_VF_SLOT[i]") != std::string::npos &&
          report.find("VF_WINDOW[i]") != std::string::npos);
    CHECK(report.find("0xff71f") != std::string::npos &&
          report.find("0xc100") != std::string::npos);
    CHECK(report.find("vf[8]") == std::string::npos);
}

struct Word : reg::RegEntityBundle {
    REG_BUNDLE(Word);
    REG(VALUE, std::uint32_t, reg::at(0).reset(7));
};
template <int Mode>
struct Overlay : reg::Bundle {
    REG_BUNDLE(Overlay);
    REG_ENTITY(low, Word);
    REG_ENTITY(high, Word);
    REG_LAYOUT([&] {
        auto low_view = reg::view("LOW", 0x100).map(low);
        auto high_view = reg::view("HIGH", 0x100).map(high);
        if constexpr (Mode == 1)
            high_view = high_view.read_only_error_writes();
        if constexpr (Mode == 5)
            low_view = low_view.read_only_error_writes();
        if constexpr (Mode == 2)
            high_view = reg::view("HIGH", 0x100).reserve(0, 4);
        if constexpr (Mode == 3) {
            low_view = reg::view("LOW_OUTER", 0x100).map(low_view, 0, 100);
            return reg::address_space("OVERLAY").map(low_view, 0, 1).map(high_view, 0, 2);
        }
        return reg::address_space("OVERLAY")
            .map(low_view, 0, 0)
            .map(high_view, 0, Mode == 4 ? 0 : 10);
    });
};
void priorities_and_warnings() {
    auto* logger = hlog_register_logger("overlaytest", "regfw-overlay-test", HLOG_LEVEL_WARN);
    CHECK(logger);
    reg::RegisterBank<Overlay<0>> b(reg::space(0x100).logger(logger));
    const auto warnings = b.map().layout_warnings();
    CHECK(warnings.size() == 1 && warnings[0].code == "REGFW_ENTITY_SHADOWED");
    CHECK(warnings[0].message.find("low.VALUE") != std::string::npos &&
          warnings[0].message.find("high.VALUE") != std::string::npos);
    CHECK(warnings[0].message.find("priority") != std::string::npos);
    b.finish();
    unsigned low_calls = 0, high_calls = 0;
    b->low.VALUE.on_write([&] {
        ++low_calls;
    });
    b->high.VALUE.on_write([&] {
        ++high_calls;
    });
    CHECK(b.map().mmio_write(0, 42).success());
    CHECK(b->high.VALUE.ref() == 42 && b->low.VALUE.ref() == 7 && high_calls == 1 &&
          low_calls == 0);
    b->high.VALUE.on_read([]() -> std::uint32_t {
        throw std::runtime_error("no fallback");
    });
    CHECK(b.map().mmio_read(0) == 0 && b->low.VALUE.ref() == 7);
    reg::RegisterBank<Overlay<1>> ro(reg::space(0x100));
    ro.finish();
    CHECK(ro.map().mmio_write(0, 123).result == reg::RegMemTxResult::AccessError);
    CHECK(ro->low.VALUE.ref() == 7);
    reg::RegisterBank<Overlay<2>> reserved(reg::space(0x100));
    reserved.finish();
    CHECK(reserved.map().try_mmio_read_sized(0, 4).result == reg::RegMemTxResult::DecodeError);
    reg::RegisterBank<Overlay<3>> local(reg::space(0x100));
    local.finish();
    CHECK(local.map().mmio_write(0, 123).success() && local->high.VALUE.ref() == 123 &&
          local->low.VALUE.ref() == 7);
    bool rejected = false;
    try {
        reg::RegisterBank<Overlay<4>> equal(reg::space(0x100));
    } catch (const reg::Error& e) {
        rejected = std::string(e.what()).find("same-priority") != std::string::npos;
    }
    CHECK(rejected);
    reg::RegisterBank<Overlay<5>> permissions(reg::space(0x100));
    permissions.finish();
    CHECK(permissions.map().mmio_write(0, 123).success() && permissions->high.VALUE.ref() == 123);
    hlog_flush();
    char path[2048];
    CHECK(hlog_file_path(path, sizeof(path)) < sizeof(path));
    std::ifstream in(path);
    std::string text{std::istreambuf_iterator<char>(in), {}};
    CHECK(text.find("REGFW_ENTITY_SHADOWED") != std::string::npos &&
          text.find("priority paths") != std::string::npos);
}

struct Slices : reg::RegEntityBundle {
    REG_BUNDLE(Slices);
    REG(VALUES, std::uint32_t, reg::at(0x100).array(reg::dims(4), reg::strides(8)));
    REG(PAIR, std::uint64_t, reg::at(0x200).reset(0x1122334455667788ULL));
};
struct SliceLayout : reg::Bundle {
    REG_BUNDLE(SliceLayout);
    REG_ENTITY(core, Slices);
    REG_ENTITY(upper, Word);
    REG_LAYOUT([&] {
        return reg::address_space("SLICES").map(reg::view("BLOCK", 0x100)
                                                    .map(core.VALUES.subspan(1, 2), 0x20)
                                                    .map(core.PAIR, 0x40)
                                                    .map(upper.VALUE, 0x44, 10),
                                                B);
    });
};
void register_slices_and_split_visibility() {
    reg::RegisterBank<SliceLayout> bank(reg::space());
    bank.finish();
    auto& map = bank.map();
    CHECK(map.mmio_write(B + 0x20, 42).success() && bank->core.VALUES.at(1).ref() == 42);
    CHECK(map.mmio_write(B + 0x28, 43).success() && bank->core.VALUES.at(2).ref() == 43);
    CHECK(map.mmio_read(B + 0x40) == 0x55667788 && map.mmio_read(B + 0x44) == 7);
    CHECK(!map.try_mmio_read_sized(B + 0x40, 8).success()); // Cannot span two selected entities.
    CHECK(map.layout_warnings().size() == 1);
    const auto report = map.describe_layout(reg::LayoutDetail::Full);
    CHECK(report.find("elements=2/4; linear={1..2}") != std::string::npos);
    CHECK(report.find("partial elements=1") != std::string::npos);
}
struct FarLocal : reg::RegEntityBundle {
    REG_BUNDLE(FarLocal);
    REG(VALUE, std::uint32_t, reg::at(0x100000040ULL));
};
struct FarLayout : reg::Bundle {
    REG_BUNDLE(FarLayout);
    REG_REPEAT_ENTITY(vf, FarLocal, 2);
    REG_LAYOUT([&] {
        auto instance = [&](auto i) {
            return reg::view("INSTANCE", 0x200000000ULL)
                .identity(9)
                .using_resolver(VfRoute)
                .map(vf[i]);
        };
        return reg::address_space("FAR").map_repeat(
            "VF", B, reg::repeat(2).stride(0x200000000ULL), instance);
    });
};
void identity_offsets_above_4g() {
    reg::RegisterBank<FarLayout> bank(reg::space());
    std::uint64_t inner = 0, bus = 0;
    bank.map().set_identity_resolver(VfRoute, [&](const auto& r) {
        inner = r.offset_in_instance;
        bus = r.bus_offset;
        return 1 - r.request_id;
    });
    bank.finish();
    CHECK(bank.map().mmio_write(B + 0x100000040ULL, 123).success());
    CHECK(inner == 0x100000040ULL && bus == B + 0x100000040ULL);
    CHECK(bank->vf[1].VALUE.ref() == 123 && bank->vf[0].VALUE.ref() == 0);
    CHECK(bank->vf[1].VALUE.address() == B + 0x300000040ULL);
}


template <bool Reverse>
struct LayerOrder : reg::Bundle {
    REG_BUNDLE(LayerOrder);
    REG_ENTITY(low, Word);
    REG_ENTITY(mid, Word);
    REG_ENTITY(high, Word);
    REG_LAYOUT([&] {
        auto v = reg::view("LAYERS", 0x100);
        if constexpr (Reverse)
            v = v.map(high, 0, 10).map(low).map(mid);
        else
            v = v.map(low).map(mid).map(high, 0, 10);
        return reg::address_space("ORDER").map(v, 0);
    });
};
void declaration_order_independence() {
    reg::RegisterBank<LayerOrder<false>> a(reg::space(0x100));
    reg::RegisterBank<LayerOrder<true>> b(reg::space(0x100));
    a.finish();
    b.finish();
    CHECK(a.map().layout_warnings().size() == 2 && b.map().layout_warnings().size() == 2);
    CHECK(a.map().mmio_write(0, 123).success() && b.map().mmio_write(0, 123).success());
    CHECK(a->high.VALUE.ref() == 123 && b->high.VALUE.ref() == 123 && a->low.VALUE.ref() == 7 &&
          b->mid.VALUE.ref() == 7);
}

void legacy_64bit_and_bounds() {
    constexpr auto top = UINT64_MAX & ~std::uint64_t{3};
    reg::RegisterMap map(top);
    auto low = map.create_reg<std::uint32_t>(0x40, "LOW");
    auto high = map.create_reg<std::uint32_t>(B + 0x40, "HIGH");
    auto end = map.create_reg<std::uint32_t>(top - 4, "END");
    map.map_alias_window(A + 0x40, B + 0x40, 4);
    map.finalize();
    CHECK(high.address() == B + 0x40 && end.address() == top - 4);
    CHECK(map.mmio_write(A + 0x40, 123).success() && high.ref() == 123 && low.ref() == 0);
    CHECK(map.mmio_write(top - 4, 42).success() && end.ref() == 42);
    CHECK(!map.try_mmio_read_sized(top, 4).success());
    auto meta = high.metadata();
    CHECK(meta.address == B + 0x40);
}
} // namespace
int main() try {
    high_addresses_and_shared_state();
    reusable_nested_views();
    priorities_and_warnings();
    register_slices_and_split_visibility();
    identity_offsets_above_4g();
    declaration_order_independence();
    legacy_64bit_and_bounds();
    std::cout << "PASS: composable views, two 64-bit VF entrances, sparse overlay holes, shadow "
                 "warnings, priority locality, no error fallback, shared "
                 "callbacks/transactions/snapshots, QEMU C bridge and reset isolation\n";
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
