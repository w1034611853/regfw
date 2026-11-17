#include <regfw/regfw.hpp>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <string>
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!(__VA_ARGS__))                                                                        \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " +      \
                                     #__VA_ARGS__);                                                \
    } while (false)
namespace {
constexpr std::uint64_t A = 0xff71f0000000ULL, B = 0xc100000000ULL;
REG_PRE_ACCESS(ParentCheck);
REG_PRE_ACCESS(EntryA);
REG_PRE_ACCESS(EntryB);
REG_PRE_ACCESS(ChildCheck);
REG_PRE_ACCESS(Missing);
REG_ACCESS_FIELD(Route, std::uint32_t);
REG_ACCESS_FIELD(Logical, std::uint32_t);
REG_ACCESS_FIELD(Physical, std::uint32_t);
std::vector<std::string> events;
struct R : reg::RegEntityBundle {
    REG_BUNDLE(R);
    REG(VALUE, unsigned, reg::at(0));
    REG(RO, unsigned, reg::at(4).access(reg::ro));
    REG(PACKET, reg::u512, reg::at(0x40));
    REG(ARRAY, unsigned, reg::at(0x100).array(reg::dims(4), reg::strides(4)));
    REG_ACCESS_GROUP(G,
                     .members = {ARRAY},
                     .coordinates = {{Logical, 0}},
                     .derive = {reg::map_index(Logical, Physical).using_resolver(Physical)});
};
struct Shared : reg::RegEntityBundle {
    REG_BUNDLE(Shared);
    REG(VALUE, unsigned, reg::at(0x20));
};
struct Layout : reg::Bundle {
    REG_BUNDLE(Layout);
    REG_REPEAT_ENTITY(vf, R, 2);
    REG_ENTITY(shared, Shared);
    REG_LAYOUT([&] {
        const auto common = reg::view("SHARED", 0x200).map(shared);
        auto instance = [&](auto i) {
            const auto private_regs =
                reg::view("PRIVATE", 0x200).map(vf[i]).pre_access([i](const reg::RegAccessInfo&) {
                    events.push_back("I" + std::to_string(i));
                });
            return reg::view("INSTANCE", 0x200)
                .identity(1)
                .using_resolver(Route)
                .using_pre_access(ChildCheck)
                .map(private_regs)
                .map(common, 0, 10);
        };
        const auto area = reg::view("AREA", 0x1000)
                              .using_pre_access(ParentCheck)
                              .map_repeat("VF", 0x200, reg::repeat(2).stride(0x200), instance);
        return reg::address_space("ROOT")
            .map(reg::view("A", 0x1000).using_pre_access(EntryA).map(area), A)
            .map(reg::view("B", 0x1000).using_pre_access(EntryB).map(area), B);
    });
};
void ordering_aliases_and_transactions() {
    reg::RegisterBank<Layout> b(reg::space().name("ROOT"));
    auto& m = b.map();
    bool fail_route = false, info_ok = true;
    reg::RegAccessInfo saved_index;
    unsigned reads = 0, commits = 0;
    m.set_identity_resolver(Route, [&](const auto& r) -> unsigned {
        if (fail_route)
            throw std::runtime_error("route");
        return 1 - r.request_id;
    });
    m.set_access_resolver(Physical, [](const auto& i) {
        return (i.index_as(Logical) + 1) % 4;
    });
    m.set_pre_access([](const auto&) {
        events.push_back("G");
    });
    m.set_pre_access(EntryA, [](const auto&) {
        events.push_back("A");
    });
    m.set_pre_access(EntryB, [](const auto&) {
        events.push_back("B");
    });
    m.set_pre_access(ParentCheck, [](const auto&) {
        events.push_back("P");
    });
    m.set_pre_access(ChildCheck, [&](const reg::RegAccessInfo& i) {
        events.push_back("C");
        info_ok = info_ok && i.request_id() == 0 && i.resolved_id() == 1 &&
                  i.view_path.find("INSTANCE[1]") != std::string_view::npos;
        if (i.access_group == "G") {
            info_ok = info_ok && i.index.values[0] == 1 && i.mapped_as(Physical) == 1;
            saved_index = i;
        }
    });
    b->vf[1].VALUE.on_write([] {
        events.push_back("W");
    });
    b->vf[1].PACKET.on_write([&] {
        ++commits;
    });
    b->vf[1].PACKET.on_read([&] {
        ++reads;
        reg::u512 v;
        v.set_word(0, 0x12345678);
        return v;
    });
    b.finish();
    events.clear();
    CHECK(m.mmio_write(A + 0x200, 42).success());
    CHECK(events == std::vector<std::string>({"G", "A", "P", "C", "I1", "W"}) && info_ok &&
          b->vf[1].VALUE.ref() == 42);
    events.clear();
    CHECK(m.mmio_read(B + 0x200) == 42);
    CHECK(events == std::vector<std::string>({"G", "B", "P", "C", "I1"}));
    events.clear();
    CHECK(m.mmio_write(B + 0x220, 9).success());
    CHECK(events == std::vector<std::string>({"G", "B", "P", "C"}) && b->shared.VALUE.ref() == 9);
    events.clear();
    CHECK(m.mmio_write(A + 0x300, 99).success());
    CHECK(info_ok && b->vf[1].ARRAY.at(1).ref() == 99);
    events.clear();
    CHECK(m.mmio_write(A + 0x204, 1).result == reg::RegMemTxResult::Ignored && events.empty());
    CHECK(m.try_mmio_read_sized(A + 0x210, 4).result == reg::RegMemTxResult::DecodeError &&
          events.empty());
    fail_route = true;
    CHECK(m.mmio_write(A + 0x200, 1).result == reg::RegMemTxResult::DeviceError && events.empty());
    fail_route = false;
    events.clear();
    for (unsigned k = 0; k < 8; ++k)
        CHECK(m.mmio_write(A + 0x240 + k * 4, k + 1).success());
    CHECK(events.size() == 40 && commits == 0);
    CHECK(m.mmio_read(A + 0x240) == 0x12345678 && reads == 1);
    auto before = events.size();
    auto snapshot = b.save();
    (void)m.describe_layout(reg::LayoutDetail::Full);
    (void)m.explain_address(A + 0x200);
    CHECK(events.size() == before);
    b.load(snapshot);
    CHECK(events.size() == before);
    for (unsigned k = 8; k < 16; ++k)
        CHECK(m.mmio_write(B + 0x240 + k * 4, k + 1).success());
    CHECK(commits == 1);
    for (unsigned k = 1; k < 16; ++k)
        (void)m.mmio_read(B + 0x240 + k * 4);
    CHECK(reads == 1);
    CHECK(saved_index.request_id() == 0 && saved_index.resolved_id() == 1 &&
          saved_index.request_index.linear() == 0 && saved_index.index.linear() == 1 &&
          saved_index.index_as(Logical) == 0 && saved_index.mapped_as(Physical) == 1);
    m.set_pre_access(EntryA, [](const auto&) {
        events.push_back("A2");
    });
    events.clear();
    (void)m.mmio_read(A + 0x200);
    CHECK(events[1] == "A2");
    m.clear_pre_access(EntryA);
    CHECK(!m.has_pre_access(EntryA) && m.has_pre_access(EntryB) && m.has_pre_access());
    events.clear();
    CHECK(m.mmio_write(A + 0x200, 123).result == reg::RegMemTxResult::Ignored &&
          b->vf[1].VALUE.ref() == 42);
    CHECK(events == std::vector<std::string>({"G"}));
    m.set_pre_access(EntryA, [](const auto&) {});
    events.clear();
    CHECK(m.mmio_write_sized(B + 0x202, 0xAABB, 2).success());
    CHECK(b->vf[1].VALUE.ref() == 0xAABB002A && m.mmio_read_sized(B + 0x202, 2) == 0xAABB);
    events.clear();
    b.reset();
    CHECK(events.empty() && b->vf[1].VALUE.ref() == 0);
}

struct Word : reg::RegEntityBundle {
    REG_BUNDLE(Word);
    REG(VALUE, unsigned, reg::at(0));
};
template <int Mode>
struct Overlay : reg::Bundle {
    REG_BUNDLE(Overlay);
    REG_ENTITY(low, Word);
    REG_ENTITY(high, Word);
    REG_LAYOUT([&] {
        auto lo = reg::view("LOW", 0x100).map(low).pre_access([](const auto&) {
            events.push_back("L");
        });
        auto hi = reg::view("HIGH", 0x100).map(high).pre_access([](const auto&) {
            events.push_back("H");
            if constexpr (Mode == 1)
                throw std::runtime_error("blocked");
        });
        if constexpr (Mode == 2)
            hi = hi.read_only_error_writes();
        return reg::address_space("OVERLAY").map(reg::view("PARENT", 0x100)
                                                     .pre_access([](const auto&) {
                                                         events.push_back("P");
                                                     })
                                                     .map(lo)
                                                     .map(hi, 0, 10),
                                                 0);
    });
};
void overlays_and_failures() {
    reg::RegisterBank<Overlay<0>> ok(reg::space(0x100).finalize());
    events.clear();
    CHECK(ok.map().mmio_write(0, 7).success());
    CHECK(events == std::vector<std::string>({"P", "H"}) && ok->high.VALUE.ref() == 7 &&
          ok->low.VALUE.ref() == 0);
    reg::RegisterBank<Overlay<1>> fail(reg::space(0x100).finalize());
    unsigned callbacks = 0;
    fail->high.VALUE.on_write([&] {
        ++callbacks;
    });
    events.clear();
    CHECK(fail.map().mmio_write(0, 7).result == reg::RegMemTxResult::Ignored);
    CHECK(events == std::vector<std::string>({"P", "H"}) && callbacks == 0 &&
          fail->high.VALUE.ref() == 0 && fail->low.VALUE.ref() == 0);
    CHECK(fail.map().mmio_read(0) == 0);
    reg::RegisterBank<Overlay<2>> denied(reg::space(0x100).finalize());
    events.clear();
    CHECK(denied.map().mmio_write(0, 7).result == reg::RegMemTxResult::AccessError &&
          events.empty());
}
struct Unbound : reg::Bundle {
    REG_BUNDLE(Unbound);
    REG_ENTITY(regs, Word);
    REG_LAYOUT([&] {
        return reg::address_space("MISSING").map(
            reg::view("V", 4).map(regs).using_pre_access(Missing), 0);
    });
};
struct LifetimeProbe {
    bool& active;
    bool& bad;
    unsigned& destroyed;
    ~LifetimeProbe() {
        bad = bad || active;
        ++destroyed;
    }
};
void scalar_hook_frames(bool finalized) {
    reg::RegisterBank<Unbound> b(reg::space(0x100));
    auto& m = b.map();
    auto& value = b->regs.VALUE;
    unsigned global_calls = 0, view_calls = 0, reads = 0, writes = 0, completed = 0;
    bool correct = true;
    reg::RegAccessInfo saved;
    m.set_pre_access([&, calls = 0](const reg::RegAccessInfo& i) mutable {
        global_calls = ++calls;
        correct &= i.stored_before == 10 && i.stored_after == 10 && i.address == 0 &&
                   i.resolved_address == 0 && i.index.linear() == 0 && !i.has_identity() &&
                   i.access_group.empty() && !i.is_wide();
        value.ref() = 20;
    });
    // By-value info and mutable state must work identically through full
    // and frame invocation, including alternating partial/full accesses.
    m.set_pre_access(Missing, [&, calls = 0](reg::RegAccessInfo i) mutable {
        view_calls = ++calls;
        correct &= global_calls == view_calls && value.ref() == 20 && i.stored_before == 10 &&
                   i.stored_after == 10 && i.view_path.find("V") != std::string_view::npos;
        saved = i;
        value.ref() = 30;
    });
    value.on_read([&](std::uint32_t candidate, const auto& i) -> std::optional<std::uint32_t> {
        ++reads;
        correct &= candidate == 30 && i.stored_before == 30 && i.stored_after == 30;
        value.ref() = 90;
        return std::nullopt;
    });
    value.on_read_complete([&](std::uint32_t result, const auto& i) {
        ++completed;
        correct &= result == 30 && i.stored_before == 30;
    });
    value.on_write([&](std::uint32_t old, const auto& i) {
        ++writes;
        correct &= old == 30 && i.stored_before == 30 && i.stored_after == 64;
    });
    if (finalized)
        b.finish();
    for (unsigned width : {4u, 2u, 4u, 1u, 4u}) {
        value.ref() = 10;
        CHECK(m.mmio_read_sized(0, width) == 30 && value.ref() == 90);
        CHECK(saved.size == width && saved.raw_data == 0 && saved.is_read());
        value.ref() = 10;
        CHECK(m.mmio_write_sized(0, 64, width).result == reg::RegMemTxResult::Ok);
        CHECK(saved.size == width && saved.raw_data == 64 && saved.is_write());
        CHECK(value.ref() == 64 && correct);
    }
    CHECK(global_calls == 10 && view_calls == 10 && reads == 5 && writes == 5 && completed == 5);
    m.set_pre_access(nullptr);
    CHECK(m.mmio_write(0, 99).result == reg::RegMemTxResult::Ignored && value.ref() == 64);
    m.clear_pre_access();
    void (*empty_hook)(const reg::RegAccessInfo&) = nullptr;
    m.set_pre_access(Missing, empty_hook);
    CHECK(m.mmio_read(0) == 0 && value.ref() == 64);
}
void self_rebinding_lifetimes() {
    reg::RegisterBank<Unbound> b(reg::space(0x100));
    auto& m = b.map();
    bool active = false, bad = false;
    unsigned destroyed = 0, first = 0, second = 0;
    auto probe = std::make_shared<LifetimeProbe>(active, bad, destroyed);
    m.set_pre_access(Missing, [probe, &m, &active, &first, &second](const auto&) {
        active = true;
        ++first;
        m.set_pre_access(Missing, [&second](const auto&) {
            ++second;
        });
        (void)probe->destroyed;
        active = false;
    });
    probe.reset();
    b.finish();
    CHECK(m.mmio_write(0, 1).success() && first == 1 && second == 0 && destroyed == 1 && !bad);
    CHECK(m.mmio_write(0, 2).success() && second == 1);
    probe = std::make_shared<LifetimeProbe>(active, bad, destroyed);
    m.set_pre_access([probe, &m, &active](const auto&) {
        active = true;
        m.clear_pre_access();
        (void)probe->destroyed;
        active = false;
    });
    probe.reset();
    CHECK(m.mmio_write(0, 3).success() && destroyed == 2 && !bad && !m.has_pre_access());
    probe = std::make_shared<LifetimeProbe>(active, bad, destroyed);
    m.set_pre_access(Missing, [probe, &m, &active](const auto&) {
        active = true;
        m.clear_pre_access(Missing);
        (void)probe->destroyed;
        active = false;
        throw std::runtime_error("after clear");
    });
    probe.reset();
    CHECK(m.mmio_write(0, 4).result == reg::RegMemTxResult::Ignored && destroyed == 3 && !bad &&
          b->regs.VALUE.ref() == 3);
}

void binding_validation() {
    reg::RegisterBank<Unbound> b(reg::space(0x100));
    bool rejected = false;
    try {
        b.finish();
    } catch (const reg::Error& e) {
        rejected = std::string_view(e.what()).find("pre_access:Missing") != std::string_view::npos;
    }
    CHECK(rejected);
    b.map().defer_pre_access(Missing);
    b.finish();
    CHECK(b.map().mmio_write(0, 1).result == reg::RegMemTxResult::Ignored &&
          b->regs.VALUE.ref() == 0);
    b.map().set_pre_access(Missing, [](const auto&) {});
    CHECK(b.map().mmio_write(0, 2).success() && b->regs.VALUE.ref() == 2);
    bool duplicate = false;
    try {
        (void)reg::view("V", 4).using_pre_access(Missing).pre_access([](const auto&) {});
    } catch (const reg::Error&) {
        duplicate = true;
    }
    CHECK(duplicate);
}
} // namespace
int main() try {
    ordering_aliases_and_transactions();
    overlays_and_failures();
    binding_validation();
    self_rebinding_lifetimes();
    scalar_hook_frames(false);
    scalar_hook_frames(true);
    std::cout << "PASS: selected View pre_access chains, global-parent-child order, aliases, "
                 "identity/index remap, holes/overlays, every-beat transactions, snapshot "
                 "independence, exceptions and late binding\n";
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
