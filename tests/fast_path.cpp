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
void callback_state_and_frames() {
    reg::RegisterBank<Layout> b(reg::space().name("FRAMES"));
    b.finish();
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
    CHECK(m.mmio_read_sized(A, 1) == 1 && m.mmio_read(B) == 2 && m.mmio_read_sized(A, 2) == 3 &&
          info_ok);
    CHECK(saved.address == A && saved.size == 2 && saved.bus_mask == 0xffff);
    CHECK(m.mmio_read(B) == 4 && info_ok && saved.address == B && saved.size == 4 &&
          saved.bus_mask == 0xffffffff && saved.register_width_bits == 32 && saved.raw_data == 0);
    auto copy = saved;
    m.set_pre_access([](const auto&) {});
    CHECK(m.mmio_read(A) == 5);
    m.clear_pre_access();
    CHECK(m.mmio_read(B) == 6 && info_ok && copy.address == B && copy.size == 4);
    unsigned observed = 0;
    b->regs.MASK.on_write([count = 0, &observed](const auto&) mutable {
        observed = ++count;
    });
    m.mmio_write_sized(A + 4, 1, 1);
    CHECK(observed == 1);
    m.mmio_write(B + 4, 2);
    CHECK(observed == 2);
    m.mmio_write_sized(A + 4, 3, 2);
    CHECK(observed == 3);
    reg::RegisterBank<Layout> late(reg::space().name("COMPLETE"));
    late.finish();
    unsigned complete = 0;
    late->regs.VALUE.on_read([&, bound = false]() mutable {
        if (!bound) {
            late->regs.VALUE.on_read_complete([&](std::uint32_t value, const auto& i) {
                if (value == 77 && i.stored_after == 0 && i.size == 4)
                    ++complete;
            });
            bound = true;
        }
        return std::uint32_t(77);
    });
    CHECK(late.map().mmio_read(A) == 77 && complete == 1);
    CHECK(late.map().mmio_read(B) == 77 && complete == 2);
}
void late_bindings() {
    reg::RegisterBank<Layout> b(reg::space().name("LATE"));
    b.finish();
    auto& m = b.map();
    auto& r = b->regs.VALUE;
    CHECK(m.mmio_write(A, 5).success() && m.mmio_read(B) == 5);
    unsigned pre = 0, writes = 0, reads = 0, completed = 0;
    bool info_ok = true;
    m.set_pre_access([&](const reg::RegAccessInfo& i) {
        ++pre;
        info_ok = info_ok && i.stored_before == 5;
        r.ref() = 10;
    });
    CHECK(m.mmio_read(A) == 10 && pre == 1 && info_ok);
    m.clear_pre_access();
    CHECK(m.mmio_write(B, 11).success() && pre == 1);
    r.on_write([&](std::uint32_t old, const auto& i) {
        ++writes;
        info_ok = info_ok && old == 11 && i.stored_before == 11 && i.stored_after == 12;
    });
    CHECK(m.mmio_write(A, 12).success() && writes == 1 && info_ok);
    r.on_read([&](std::uint32_t candidate, const auto&) -> std::optional<std::uint32_t> {
        ++reads;
        info_ok = info_ok && candidate == 12;
        r.ref() = 42;
        return std::nullopt;
    });
    r.on_read_complete([&](std::uint32_t response, const auto&) {
        ++completed;
        info_ok = info_ok && response == 12;
    });
    CHECK(m.mmio_read(B) == 12 && r.ref() == 42 && reads == 1 && completed == 1 && info_ok);
    m.set_pre_access([](const auto&) {
        throw std::runtime_error("reject");
    });
    CHECK(m.mmio_write(A, 99).result == reg::RegMemTxResult::Ignored && r.ref() == 42 &&
          writes == 1);
    m.clear_pre_access();
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
    b.finish();
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
    CHECK(m.mmio_write(A, 7).result == reg::RegMemTxResult::Ok && b->regs.VALUE.ref() == 7);
    auto read = m.try_mmio_read_sized(A, 4);
    CHECK(read.result == reg::RegMemTxResult::Ok &&
          read.value == reg::REG_READ_CALLBACK_ERROR_MAGIC && completed == 0);
    reg::RegMemTxResult recursive = reg::RegMemTxResult::Ok;
    b->regs.MASK.on_write([&] {
        recursive = m.try_mmio_read_sized(A + 4, 4).result;
    });
    CHECK(m.mmio_write(A + 4, 9).success() && recursive == reg::RegMemTxResult::DeviceError);
    reg::RegisterBank<Layout> dynamic(reg::space().name("DYNAMIC"));
    dynamic.finish();
    auto* logger = hlog_register_logger("fastdynamic", "regfw-fast-dynamic", HLOG_LEVEL_TRACE);
    CHECK(logger);
    dynamic->regs.VALUE.on_read([&] {
        dynamic.map().set_logger(logger);
        return std::uint32_t(77);
    });
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
    // Bind consumers from the resolver: they must run on this very access.
    reg::RegAccessInfo saved;
    bool bound = false;
    m.set_identity_resolver(Route, [&](const auto& req) {
        b->vf[1].VALUE.ref() = 35;
        if (!bound) {
            m.set_pre_access([&](const reg::RegAccessInfo& i) {
                ++pre;
                saved = i;
                b->vf[1].VALUE.ref() = 36;
            });
            bound = true;
        }
        return req.request_id ^ 1;
    });
    CHECK(m.mmio_read(A) == 36 && pre == 1 && saved.stored_before == 35 &&
          saved.request_id() == 0 && saved.resolved_id() == 1 && saved.size == 4 &&
          saved.address == A && saved.register_width_bits == 32 && !saved.wide_values);
    auto retained = saved;
    m.clear_pre_access();
    bound = false;
    m.set_identity_resolver(Route, [&](const auto& req) {
        if (!bound) {
            b->vf[1].VALUE.on_read([&](std::uint32_t value, const reg::RegAccessInfo& i) {
                ++reads;
                saved = i;
                return value + 7;
            });
            bound = true;
        }
        return req.request_id ^ 1;
    });
    CHECK(m.mmio_read(B) == 43 && reads == 1 && saved.address == B);
    // A read callback does not require constructing context on a plain write.
    CHECK(m.mmio_write(A, 42).success() && reads == 1 && b->vf[1].VALUE.ref() == 42);
    bound = false;
    m.set_identity_resolver(Route, [&](const auto& req) {
        b->vf[1].VALUE.ref() = 50;
        if (!bound) {
            b->vf[1].VALUE.on_write([&](std::uint32_t old, const reg::RegAccessInfo& i) {
                ++writes;
                CHECK(old == 50);
                saved = i;
            });
            bound = true;
        }
        return req.request_id ^ 1;
    });
    CHECK(m.mmio_write(A, 55).success() && writes == 1 && saved.stored_before == 50 &&
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
    // Named slots remain live and keep the executing callable alive across self replacement.
    bool destroyed = false, alive = false;
    struct Life {
        bool* destroyed;
        ~Life() {
            *destroyed = true;
        }
    };
    auto life = std::make_shared<Life>();
    life->destroyed = &destroyed;
    fast.map().set_access_resolver(P0, [&, life](const auto&) {
        fast.map().set_access_resolver(P0, [](const auto&) {
            return 0u;
        });
        alive = !destroyed;
        return 0u;
    });
    life.reset();
    fast.map().set_identity_resolver(Route, [](const auto& req) {
        return req.request_id;
    });
#define ZERO_BIND(N)                                                                               \
    if constexpr (N != 0)                                                                          \
        fast.map().set_access_resolver(P##N, [](const auto&) {                                     \
            return 0u;                                                                             \
        });
    FIELDS(ZERO_BIND)
#undef ZERO_BIND
    CHECK(fast.map().mmio_write(A + 4, 5).success() && alive && destroyed);
    fast.map().clear_access_resolver(P0);
    CHECK(fast.map().mmio_write(A, 7).result == reg::RegMemTxResult::DeviceError);
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
} // namespace
int main() try {
    equivalent();
    callback_state_and_frames();
    late_bindings();
    callback_errors_and_dynamic_logging();
    field_lifetimes();
    mapped_equivalence();
    identity_without_context(false);
    identity_without_context(true);
    context_reset();
    std::cout << "PASS: fast/generic equivalence, dynamic binding, storage/snapshot lifetime, 16 "
                 "fields, mapped callback equivalence, context reuse and copied access info\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
