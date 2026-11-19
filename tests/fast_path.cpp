#include <regfw/regfw.hpp>
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

constexpr std::uint64_t A = 0xff71f0000000ULL, B = 0xc100000000ULL, C = 0xc200000000ULL;
struct Encoded {
    using Raw = std::uint32_t;
    Raw encoded;
    Raw raw() const {
        return encoded ^ 0xa5a5a5a5u;
    }
    static Encoded from_raw(Raw v) {
        return {v ^ 0xa5a5a5a5u};
    }
};
struct R : reg::RegEntityBundle {
    REG_BUNDLE(R);
    REG(VALUE, std::uint32_t, reg::at(0));
    REG(MASK, std::uint32_t, reg::at(4).mask(0x00ff00ff));
    REG(RC, std::uint32_t, reg::at(8).access(reg::rc));
    REG(W1C, std::uint32_t, reg::at(12).access(reg::w1c).reset(0xffffffff));
    REG(TXN, std::uint64_t, reg::at(16).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
    REG(SMALL, std::uint8_t, reg::at(32));
    REG(HALF, std::uint16_t, reg::at(36));
    REG(CODEC, Encoded, reg::at(40));
    REG_DYNAMIC(CUSTOM,
                std::uint32_t,
                reg::at(48).access(reg::FieldBehavior{[](auto old, auto value, auto mask) {
                                                          return (old & ~mask) | (value & mask);
                                                      },
                                                      [](reg::RegData& stored, reg::RegData mask) {
                                                          auto old = stored;
                                                          stored += 1;
                                                          return old & mask;
                                                      }}));
    REG(ARRAY, std::uint32_t, reg::at(64).array(reg::dims(4), reg::strides(8)));
    REG(WIDE, reg::u512, reg::at(128));
};
struct Layout : reg::Bundle {
    REG_BUNDLE(Layout);
    REG_ENTITY(regs, R);
    REG_LAYOUT([&] {
        auto v = reg::view("REGS", 0x100).map(regs);
        return reg::address_space("ROOT").map(v, A).map(v, B).map(v.read_only_ignore_writes(), C);
    });
};
std::uint64_t random(std::uint64_t& x) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}
void equivalent() {
    reg::RegisterBank<Layout> fast(reg::space().name("TEST")), generic(reg::space().name("TEST"));
    generic.map().set_pre_access([](const auto&) {});
    fast.finish();
    // An installed, unfinalized bank uses the generic engine. A global
    // pre_access hook no longer forces that path on a finished bank.
    std::uint64_t seed = 0x9876543210abcdefULL;
    const unsigned offsets[] = {0, 4, 8, 12, 16, 20, 32, 36, 40, 48, 64, 72, 80, 88, 96, 255};
    for (unsigned k = 0; k < 15000; ++k) {
        auto n = random(seed);
        auto base = (n % 3 == 0 ? A : n % 3 == 1 ? B : C);
        auto address = base + offsets[(n >> 8) % 16] + ((n >> 16) % 4);
        unsigned width = 1u << ((n >> 24) % 4);
        auto w1 = fast.map().mmio_write_sized(address, n, width),
             w2 = generic.map().mmio_write_sized(address, n, width);
        CHECK(w1.result == w2.result);
        auto r1 = fast.map().try_mmio_read_sized(address, width),
             r2 = generic.map().try_mmio_read_sized(address, width);
        CHECK(r1.result == r2.result && r1.value == r2.value);
        if (k % 97 == 0) {
            CHECK(fast.save() == generic.save());
            auto saved = fast.save();
            fast.load(saved);
            generic.load(saved);
        }
    }
    CHECK(fast.save() == generic.save());
    CHECK(fast.map().mmio_write(A + 40, 0x12345678).success());
    CHECK(fast->regs.CODEC.ref().encoded == (0x12345678 ^ 0xa5a5a5a5u));
    CHECK(fast.map().mmio_read(B + 40) == 0x12345678);
    fast->regs.VALUE.ref() = 5;
    CHECK(fast.map().mmio_read(A) == 5);
    auto saved = fast.save();
    fast.map().mmio_write(A, 42);
    fast.load(saved);
    CHECK(fast.map().mmio_read(B) == 5);
    fast->regs.VALUE.reset();
    CHECK(fast.map().mmio_read(A) == 0);
}
void scalar_logged_alias() {
    reg::RegisterBank<Layout> bank(reg::space().name("SCALAR_LOG_ALIAS"));
    auto* logger = hlog_register_logger("aliasfallback", "regfw-alias-fallback", HLOG_LEVEL_TRACE);
    CHECK(logger);
    bank.map().set_logger(logger);
    auto& array = bank->regs.ARRAY;
    array.at(1).ref() = 41;
    unsigned calls = 0;
    reg::RegAccessInfo saved;
    array.on_read([&](std::uint32_t value, const reg::RegAccessInfo& info) {
        ++calls;
        saved = info;
        return value + 1;
    });
    bank.finish();
    const auto canonical = bank.map().resolve_address(A + 72, 4);
    for (const auto base : {A, B}) {
        CHECK(bank.map().mmio_read(base + 72) == 42);
        CHECK(saved.is_read() && saved.size == 4 && saved.address == base + 72 &&
              saved.alias_resolved_address == canonical && saved.resolved_address == canonical &&
              saved.index.linear() == 1 && saved.stored_before == 41);
    }
    CHECK(calls == 2);
}

void direct_scalar_instance() {
    reg::RegisterMap map(0x200, "DIRECT");
    auto vf = reg::build<R>(reg::RegScope(map, 0, 0x100, "VF", {}, 7));
    auto pf = reg::build<R>(reg::RegScope(map, 0x100, 0x100, "PF"));
    std::optional<std::uint32_t> instance;
    auto capture = [&](const reg::RegAccessInfo& info) {
        instance = info.source_instance;
    };
    vf.VALUE.on_write(capture);
    pf.VALUE.on_write(capture);
    map.finalize();
    CHECK(map.mmio_write(0, 1).success() && instance == 7);
    CHECK(map.mmio_write(0x100, 2).success() && !instance);
}
void callback_state_and_frames() {
    reg::RegisterBank<Layout> b(reg::space().name("FRAMES"));
    auto& m = b.map();
    auto& value = b->regs.VALUE;
    value.ref() = 100;
    bool info_ok = true;
    reg::RegAccessInfo saved;
    value.on_read([calls = 0, &saved, &info_ok](std::uint32_t candidate,
                                                const auto& typed) mutable {
        const reg::RegAccessInfo& i = typed;
        info_ok = info_ok && candidate == 100 && i.is_read() && i.stored_before == 100 &&
                  i.stored_after == 100 && i.index.linear() == 0 && i.request_index.linear() == 0 &&
                  !i.has_identity() && i.access_group.empty() && !i.is_wide();
        saved = i;
        return std::uint32_t(++calls);
    });
    unsigned observed = 0;
    b->regs.MASK.on_write([count = 0, &observed](const auto&) mutable {
        observed = ++count;
    });
    b.finish();
    CHECK(m.mmio_read_sized(A, 1) == 1 && m.mmio_read(B) == 2 && m.mmio_read_sized(A, 2) == 3 &&
          info_ok);
    CHECK(saved.address == A && saved.size == 2 && saved.bus_mask == 0xffff);
    CHECK(m.mmio_read(B) == 4 && info_ok && saved.address == B && saved.size == 4 &&
          saved.bus_mask == 0xffffffff && saved.register_width_bits == 32 && saved.raw_data == 0);
    auto copy = saved;
    CHECK(m.mmio_read(A) == 5);
    CHECK(m.mmio_read(B) == 6 && info_ok && copy.address == B && copy.size == 4);
    m.mmio_write_sized(A + 4, 1, 1);
    CHECK(observed == 1);
    m.mmio_write(B + 4, 2);
    CHECK(observed == 2);
    m.mmio_write_sized(A + 4, 3, 2);
    CHECK(observed == 3);
    reg::RegisterBank<Layout> late(reg::space().name("COMPLETE"));
    unsigned complete = 0;
    late->regs.VALUE.on_read([] {
        return std::uint32_t(77);
    });
    late->regs.VALUE.on_read_complete([&](std::uint32_t value, const auto& i) {
        if (value == 77 && i.stored_after == 0 && i.size == 4)
            ++complete;
    });
    late.finish();
    CHECK(late.map().mmio_read(A) == 77 && complete == 1);
    CHECK(late.map().mmio_read(B) == 77 && complete == 2);
}
void fixed_bindings() {
    reg::RegisterBank<Layout> b(reg::space().name("FIXED"));
    auto& m = b.map();
    auto& r = b->regs.VALUE;
    unsigned pre = 0, writes = 0, reads = 0, completed = 0;
    bool info_ok = true, pre_enabled = true, observe_write = false, observe_read = false,
         reject = false;
    m.set_pre_access([&](const reg::RegAccessInfo& i) {
        if (reject)
            throw std::runtime_error("reject");
        if (!pre_enabled)
            return;
        ++pre;
        info_ok &= i.stored_before == 5;
        r.ref() = 10;
    });
    r.on_write([&](std::uint32_t old, const auto& i) {
        if (!observe_write)
            return;
        ++writes;
        info_ok &= old == 11 && i.stored_before == 11 && i.stored_after == 12;
    });
    r.on_read([&](std::uint32_t candidate, const auto&) -> std::optional<std::uint32_t> {
        if (!observe_read)
            return std::nullopt;
        ++reads;
        info_ok &= candidate == 12;
        r.ref() = 42;
        return std::nullopt;
    });
    r.on_read_complete([&](std::uint32_t response, const auto&) {
        if (!observe_read)
            return;
        ++completed;
        info_ok &= response == 12;
    });
    b.finish();
    r.ref() = 5;
    CHECK(m.mmio_read(A) == 10 && pre == 1 && info_ok);
    pre_enabled = false;
    CHECK(m.mmio_write(B, 11).success() && pre == 1);
    observe_write = true;
    CHECK(m.mmio_write(A, 12).success() && writes == 1 && info_ok);
    observe_read = true;
    CHECK(m.mmio_read(B) == 12 && r.ref() == 42 && reads == 1 && completed == 1 && info_ok);
    reject = true;
    CHECK(m.mmio_write(A, 99).result == reg::RegMemTxResult::Ignored && r.ref() == 42 &&
          writes == 1);
    reject = false;
    auto* logger = hlog_register_logger("fastcheck", "regfw-fast-path", HLOG_LEVEL_TRACE);
    CHECK(logger);
    m.set_logger(logger);
    CHECK(m.mmio_write(B + 4, 0x55667788).success());
    CHECK(m.mmio_read(B + 4) == 0x00660088);
    hlog_flush();
    char path[2048];
    CHECK(hlog_file_path(path, sizeof(path)) < sizeof(path));
    std::ifstream file(path);
    std::string text{std::istreambuf_iterator<char>(file), {}};
    if (text.find("MASK") == std::string::npos)
        std::cerr << "log path " << path << " bytes " << text.size() << " enabled "
                  << hlog_enabled(logger, HLOG_LEVEL_TRACE) << '\n';
    CHECK(text.find("WRITE MASK") != std::string::npos &&
          text.find("READ MASK") != std::string::npos);
    m.set_logger(nullptr);
}
void callback_errors_and_dynamic_logging() {
    reg::RegisterBank<Layout> b(reg::space().name("ERRORS"));
    auto& m = b.map();
    unsigned completed = 0;
    b->regs.VALUE.on_write([] {
        throw std::runtime_error("write event");
    });
    b->regs.VALUE.on_read([]() -> std::uint32_t {
        throw std::runtime_error("read source");
    });
    b->regs.VALUE.on_read_complete([&] {
        ++completed;
    });
    b.finish();
    CHECK(m.mmio_write(A, 7).result == reg::RegMemTxResult::Ok && b->regs.VALUE.ref() == 7);
    auto read = m.try_mmio_read_sized(A, 4);
    CHECK(read.result == reg::RegMemTxResult::Ok &&
          read.value == reg::REG_READ_CALLBACK_ERROR_MAGIC && completed == 0);
    reg::RegisterBank<Layout> dynamic(reg::space().name("DYNAMIC"));
    auto* logger = hlog_register_logger("fastdynamic", "regfw-fast-dynamic", HLOG_LEVEL_TRACE);
    CHECK(logger);
    dynamic->regs.VALUE.on_read([&] {
        dynamic.map().set_logger(logger);
        return std::uint32_t(77);
    });
    dynamic.finish();
    CHECK(dynamic.map().mmio_read(A) == 77);
    hlog_flush();
    char path[2048];
    CHECK(hlog_file_path(path, sizeof(path)) < sizeof(path));
    std::ifstream file(path);
    std::string text{std::istreambuf_iterator<char>(file), {}};
    CHECK(text.find("READ VALUE") != std::string::npos &&
          text.find("0000004d") != std::string::npos);
}
#define FIELDS(X)                                                                                  \
    X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)
#define DECL(N)                                                                                    \
    REG_ACCESS_FIELD(L##N, std::uint32_t);                                                         \
    REG_ACCESS_FIELD(P##N, std::uint32_t);
FIELDS(DECL)
#undef DECL
struct Fields : reg::RegEntityBundle {
    REG_BUNDLE(Fields);
    REG(TABLE, std::uint32_t, reg::at(0).array(reg::dims(4), reg::strides(4)));
    REG(PLAIN, std::uint32_t, reg::at(32));
#define COORD(N) {L##N, 0},
#define DERIVE(N) reg::map_index(L##N, P##N).using_resolver(P##N),
    REG_ACCESS_GROUP(ALL_FIELDS,
                     .members = {TABLE},
                     .coordinates = {FIELDS(COORD)},
                     .derive = {FIELDS(DERIVE)});
#undef COORD
#undef DERIVE
};
struct FieldLayout : reg::Bundle {
    REG_BUNDLE(FieldLayout);
    REG_ENTITY(regs, Fields);
    REG_LAYOUT([&] {
        return reg::address_space("FIELDS").map(reg::view("REGS", 64).map(regs), A);
    });
};
REG_ACCESS_FIELD(Route, std::uint32_t);
struct IdentityLayout : reg::Bundle {
    REG_BUNDLE(IdentityLayout);
    REG_REPEAT_ENTITY(vf, R, 2);
    REG_LAYOUT([&] {
        auto v = reg::view("DEVICE", 0x200)
                     .map_repeat("VF", 0, reg::repeat(2).stride(0x100), [&](auto i) {
                         return reg::view("INSTANCE", 0x100)
                             .map(vf[i])
                             .identity(7)
                             .using_resolver(Route);
                     });
        return reg::address_space("ROOT").map(v, A).map(v, B).map(v.read_only_ignore_writes(), C);
    });
};
void identity_without_context(bool finalized) {
    reg::RegisterBank<IdentityLayout> b(reg::space().name("IDENTITY"));
    auto& m = b.map();
    unsigned flip = 1, pre = 0, reads = 0, writes = 0;
    m.set_identity_resolver(Route, [&](const auto& req) {
        return req.request_id ^ flip;
    });
    if (finalized)
        b.finish();
    b->vf[1].MASK.ref() = 0xa5a5a5a5;
    CHECK(m.mmio_write(A + 4, 0x12345678).success());
    CHECK(m.mmio_read(B + 4) == 0xa534a578 && b->vf[0].MASK.ref() == 0);
    flip = 0;
    CHECK(m.mmio_write(A + 4, 0xdeadbeef).success() && b->vf[0].MASK.ref() == 0x00ad00ef);
    flip = 1;
    CHECK(m.mmio_read(A + 4) == 0xa534a578);
    CHECK(m.mmio_write(C + 4, 0).result == reg::RegMemTxResult::Ignored &&
          b->vf[1].MASK.ref() == 0xa534a578);
    reg::RegisterBank<IdentityLayout> hooked(reg::space().name("HOOKED_IDENTITY"));
    auto& hm = hooked.map();
    reg::RegAccessInfo saved;
    unsigned phase = 0;
    hm.set_identity_resolver(Route, [&](const auto& req) {
        if (phase == 0)
            hooked->vf[1].VALUE.ref() = 35;
        if (phase == 2)
            hooked->vf[1].VALUE.ref() = 50;
        return req.request_id ^ 1;
    });
    hm.set_pre_access([&](const reg::RegAccessInfo& i) {
        if (phase != 0)
            return;
        ++pre;
        saved = i;
        hooked->vf[1].VALUE.ref() = 36;
    });
    hooked->vf[1].VALUE.on_read([&](std::uint32_t value, const reg::RegAccessInfo& i) {
        if (phase != 1)
            return value;
        ++reads;
        saved = i;
        return value + 7;
    });
    hooked->vf[1].VALUE.on_write([&](std::uint32_t old, const reg::RegAccessInfo& i) {
        if (phase != 2)
            return;
        ++writes;
        CHECK(old == 50);
        saved = i;
    });
    if (finalized)
        hooked.finish();
    CHECK(hm.mmio_read(A) == 36 && pre == 1 && saved.stored_before == 35 &&
          saved.request_id() == 0 && saved.resolved_id() == 1 && saved.size == 4 &&
          saved.address == A && saved.register_width_bits == 32 && !saved.wide_values);
    auto retained = saved;
    phase = 1;
    CHECK(hm.mmio_read(B) == 43 && reads == 1 && saved.address == B);
    CHECK(hm.mmio_write(A, 42).success() && reads == 1 && hooked->vf[1].VALUE.ref() == 42);
    phase = 2;
    CHECK(hm.mmio_write(A, 55).success() && writes == 1 && saved.stored_before == 50 &&
          saved.stored_after == 55 && saved.raw_data == 55 && saved.request_id() == 0 &&
          saved.resolved_id() == 1 && retained.address == A && retained.stored_before == 35);
}

struct MappedLayout : reg::Bundle {
    REG_BUNDLE(MappedLayout);
    REG_REPEAT_ENTITY(vf, Fields, 4);
    REG_ENTITY(plain, R);
    REG_LAYOUT([&] {
        auto v = reg::view("DEVICE", 0x400)
                     .map_repeat("VF", 0, reg::repeat(4).stride(0x100), [&](auto i) {
                         return reg::view("INSTANCE", 0x100)
                             .map(vf[i])
                             .identity(7)
                             .using_resolver(Route);
                     });
        return reg::address_space("ROOT").map(v, A).map(v, B).map(
            reg::view("PLAIN", 0x100).map(plain), A + 0x800);
    });
};
void mapped_equivalence() {
    reg::RegisterBank<MappedLayout> fast(reg::space().name("MAP")),
        generic(reg::space().name("MAP"));
    generic.map().set_pre_access([](const auto&) {});
    unsigned shift = 1;
    std::uint64_t fw_count = 0, slow_count = 0;
    for (auto* b : {&fast, &generic}) {
        b->map().set_identity_resolver(Route, [&](const auto& req) {
            return (req.request_id + shift) % 4;
        });
#define MAP_BIND(N)                                                                                \
    b->map().set_access_resolver(P##N, [&](const auto& i) {                                        \
        return (i.index_as(L##N) + shift) % 4;                                                     \
    });
        FIELDS(MAP_BIND)
#undef MAP_BIND
        for (unsigned v = 0; v < 4; ++v) {
            auto* count = b == &fast ? &fw_count : &slow_count;
            b->operator->()->vf[v].TABLE.on_write(
                [count](std::uint32_t old, const reg::RegAccessInfo& i) {
                    *count += old + i.stored_after + i.resolved_id() + i.mapped_as(P15);
                });
            b->operator->()->vf[v].TABLE.on_read(
                [count](std::uint32_t candidate, const reg::RegAccessInfo& i) {
                    *count += i.request_id() + i.request_index.linear();
                    return candidate ^ 0x12345678u;
                });
            b->operator->()->vf[v].TABLE.on_read_complete(
                [count](std::uint32_t value, const auto& i) {
                    *count += value + i.index.linear();
                });
        }
        if (b == &fast)
            b->finish();
    }
    std::uint64_t seed = 0x123456789abcdefULL;
    for (unsigned k = 0; k < 6000; ++k) {
        auto x = random(seed);
        shift = (x >> 24) % 4;
        auto addr = (x & 1 ? A : B) + ((x >> 4) % 4) * 0x100 + ((x >> 8) % 4) * 4;
        unsigned width = 1u << ((x >> 16) % 3);
        addr += (x >> 32) % (5 - width);
        auto w1 = fast.map().mmio_write_sized(addr, x, width),
             w2 = generic.map().mmio_write_sized(addr, x, width);
        CHECK(w1.result == w2.result);
        auto r1 = fast.map().try_mmio_read_sized(addr, width),
             r2 = generic.map().try_mmio_read_sized(addr, width);
        CHECK(r1.value == r2.value && r1.result == r2.result && fw_count == slow_count);
        if (k % 97 == 0) {
            CHECK(fast.save() == generic.save());
            auto snap = fast.save();
            fast.load(snap);
            generic.load(snap);
        }
    }
    CHECK(fast.save() == generic.save());
    frozen([&] {
        fast.map().clear_access_resolver(P0);
    });
    frozen([&] {
        fast.map().set_identity_resolver(Route, [](const auto& r) {
            return r.request_id;
        });
    });
    CHECK(fast.map().mmio_write(A, 7).success());
}
void context_reset() {
    reg::RegisterMap empty;
    CHECK(empty.try_mmio_read_sized(0, 4).result == reg::RegMemTxResult::DeviceError);
    CHECK(empty.try_mmio_read_sized(0, 3).result == reg::RegMemTxResult::Unsupported);
    reg::RegisterBank<MappedLayout> b(reg::space().name("CONTEXT"));
    b.map().set_identity_resolver(Route, [](const auto& req) {
        return req.request_id;
    });
#define SAME_BIND(N)                                                                               \
    b.map().set_access_resolver(P##N, [](const auto& i) {                                          \
        return i.index_as(L##N);                                                                   \
    });
    FIELDS(SAME_BIND)
#undef SAME_BIND
    reg::RegAccessInfo captured;
    unsigned calls = 0;
    bool clean = true;
    b->vf[0].TABLE.on_write([&](const reg::RegAccessInfo& i) {
        captured = i;
    });
    b->plain.CODEC.on_write([&](const reg::RegAccessInfo& i) {
        ++calls;
        clean = clean && (!i.has_identity() && !i.identity_mode && !i.identity_source &&
                          !i.request_identity && !i.resolved_identity && i.access_group.empty() &&
                          !i.wide_values);
        bool missing = false;
        try {
            (void)i.index_as(L15);
        } catch (const reg::Error&) {
            missing = true;
        }
        clean = clean && missing;
        missing = false;
        try {
            (void)i.mapped_as(P15);
        } catch (const reg::Error&) {
            missing = true;
        }
        clean = clean && missing;
    });
    b->plain.WIDE.on_read([] {
        return reg::u512{77};
    });
    b.finish();
    CHECK(b.map().mmio_write(A + 4, 8).success() && captured.has_identity() &&
          captured.mapped_as(P15) == 1);
    CHECK(b.map().mmio_write(A + 0x800 + 40, 99).success() && calls == 1 && clean);
    CHECK(b.map().mmio_read(A + 0x800 + 128) == 77);
    CHECK(b.map().mmio_write(A + 0x800 + 40, 100).success() && calls == 2 && clean);
    CHECK(captured.mapped_as(P15) == 1 && captured.resolved_id() == 0);
}
void field_lifetimes() {
    reg::RegisterBank<FieldLayout> b(reg::space().name("FIELDS"));
#define BIND(N)                                                                                    \
    b.map().set_access_resolver(P##N, [](const auto& i) {                                          \
        return (i.index_as(L##N) + 1) % 4;                                                         \
    });
    FIELDS(BIND)
#undef BIND
    reg::RegAccessInfo saved, empty;
    bool captured = false;
    b->regs.TABLE.on_write([&](const reg::RegAccessInfo& i) {
        saved = i;
        captured = true;
    });
    b->regs.PLAIN.on_write([&](const reg::RegAccessInfo& i) {
        empty = i;
    });
    b.finish();
    CHECK(b.map().mmio_write(A, 123).success() && captured);
    CHECK(b->regs.TABLE.at(1).ref() == 123);
    auto copy = saved;
    CHECK(b.map().mmio_write(A + 4, 456).success());
#define VERIFY(N)                                                                                  \
    CHECK(copy.index_as(L##N) == 0 && copy.mapped_as(P##N) == 1);                                  \
    CHECK(saved.index_as(L##N) == 1 && saved.mapped_as(P##N) == 2);
    FIELDS(VERIFY)
#undef VERIFY
    auto moved = std::move(copy);
    CHECK(moved.mapped_as(P15) == 1);
    CHECK(b.map().mmio_write(A + 32, 7).success());
    saved = empty;
    bool missing = false;
    try {
        (void)saved.mapped_as(P15);
    } catch (const reg::Error&) {
        missing = true;
    }
    CHECK(missing);
    CHECK(b.map().mmio_write(A + 8, 8).success());
    CHECK(saved.mapped_as(P15) == 3 && moved.mapped_as(P15) == 1);
}
void same_scalar_info(const reg::RegAccessInfo& a, const reg::RegAccessInfo& b) {
    CHECK(a.kind == b.kind && a.address == b.address &&
          a.alias_resolved_address == b.alias_resolved_address &&
          a.resolved_address == b.resolved_address && a.size == b.size &&
          a.internal_byte_offset == b.internal_byte_offset &&
          a.register_width_bits == b.register_width_bits && a.wide_values == b.wide_values);
    CHECK(a.request_index == b.request_index && a.index == b.index && a.raw_data == b.raw_data &&
          a.bus_data == b.bus_data && a.bus_mask == b.bus_mask &&
          a.stored_before == b.stored_before && a.stored_after == b.stored_after);
    CHECK(a.route == b.route && a.source_instance == b.source_instance &&
          a.request_identity == b.request_identity && a.resolved_identity == b.resolved_identity &&
          a.identity_domain == b.identity_domain && a.identity_mode == b.identity_mode &&
          a.identity_source == b.identity_source && a.is_alias == b.is_alias &&
          a.view_path == b.view_path && a.access_group == b.access_group);
    CHECK(a.alias_access_policy.allow_read == b.alias_access_policy.allow_read &&
          a.alias_access_policy.allow_write == b.alias_access_policy.allow_write &&
          a.alias_access_policy.denied_read == b.alias_access_policy.denied_read &&
          a.alias_access_policy.denied_write == b.alias_access_policy.denied_write);
    for (const auto* info : {&a, &b}) {
        bool absent = false;
        try {
            (void)info->index_as(L15);
        } catch (const reg::Error&) {
            absent = true;
        }
        CHECK(absent);
        absent = false;
        try {
            (void)info->mapped_as(P15);
        } catch (const reg::Error&) {
            absent = true;
        }
        CHECK(absent);
    }
}
void legacy_scalar_equivalence() {
    reg::RegisterBank<R> fast(reg::space(0x100).name("LEGACY"));
    reg::RegisterBank<R> generic(reg::space(0x100).name("LEGACY"));
    std::vector<reg::RegAccessInfo> fast_events, generic_events;
    bool fail_read = false;
    auto bind = [&](auto& bank, auto& events) {
        // Callable state and native storage remain mutable after finish.
        bank->ARRAY.on_write([&](const reg::RegAccessInfo& i) {
            events.push_back(i);
            bank->ARRAY.at_element(i.index.linear()).ref() += 1;
        });
        bank->ARRAY.on_read([&](std::uint32_t value, const auto& i) {
            events.push_back(i);
            if (fail_read)
                throw std::runtime_error("read source failure");
            return value ^ 0x55;
        });
        bank->ARRAY.on_read_complete([&](const reg::RegAccessInfo& i) {
            events.push_back(i);
        });
        bank->MASK.on_write([&](const reg::RegAccessInfo& i) {
            events.push_back(i);
        });
    };
    bind(fast, fast_events);
    bind(generic, generic_events);
    fast.finish();
    std::uint64_t seed = 0x123456789abcdefULL;
    const unsigned offsets[] = {
        0, 4, 8, 12, 16, 20, 32, 36, 40, 48, 64, 72, 80, 88, 68, 95, 96, 255, 256};
    for (unsigned k = 0; k != 900; ++k) {
        fail_read = k % 17 == 0;
        auto value = random(seed);
        const auto address = offsets[value % std::size(offsets)];
        const auto width = 1u << ((value >> 9) % 4);
        auto fw = fast.map().mmio_write_sized(address, value, width);
        auto gw = generic.map().mmio_write_sized(address, value, width);
        CHECK(fw.result == gw.result);
        auto fr = fast.map().try_mmio_read_sized(address, width);
        auto gr = generic.map().try_mmio_read_sized(address, width);
        CHECK(fr.result == gr.result && fr.value == gr.value);
        CHECK(fast_events.size() == generic_events.size());
        for (std::size_t i = 0; i != fast_events.size(); ++i)
            same_scalar_info(fast_events[i], generic_events[i]);
        fast_events.clear();
        generic_events.clear();
        if (k % 100 == 0)
            CHECK(fast.save() == generic.save());
    }
    CHECK(fast.save() == generic.save());
    fail_read = false;
    CHECK(fast.map().mmio_write(72, 0x12345678).success());
    CHECK(generic.map().mmio_write(72, 0x12345678).success());
    CHECK(fast->ARRAY.at(1).ref() == 0x12345679);
    CHECK(fast.map().mmio_read(72) == (0x12345679u ^ 0x55));
    CHECK(generic.map().mmio_read(72) == (0x12345679u ^ 0x55));
    for (std::size_t i = 0; i != fast_events.size(); ++i)
        same_scalar_info(fast_events[i], generic_events[i]);
}
} // namespace
int main() try {
    equivalent();
    legacy_scalar_equivalence();
    scalar_logged_alias();
    direct_scalar_instance();
    callback_state_and_frames();
    fixed_bindings();
    callback_errors_and_dynamic_logging();
    field_lifetimes();
    mapped_equivalence();
    identity_without_context(false);
    identity_without_context(true);
    context_reset();
    std::cout << "PASS: fast/generic equivalence, fixed binding, storage/snapshot lifetime, 16 "
                 "fields, mapped callback equivalence, context reuse and copied access info\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
