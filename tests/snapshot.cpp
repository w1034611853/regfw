#include <regfw/vmstate.hpp>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!(__VA_ARGS__))                                                                        \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " +      \
                                     #__VA_ARGS__);                                                \
    } while (false)
struct File {
    std::vector<std::uint8_t> data;
    std::size_t pos{};
};
extern "C" void module_var_save(void* opaque, const std::uint8_t* p, std::uint32_t n) {
    auto& f = *static_cast<File*>(opaque);
    f.data.insert(f.data.end(), p, p + n);
}
extern "C" void module_var_load(void* opaque, std::uint8_t* p, std::uint32_t n) {
    auto& f = *static_cast<File*>(opaque);
    if (n > f.data.size() - f.pos)
        throw std::runtime_error("short QEMU-style snapshot stream");
    std::memcpy(p, f.data.data() + f.pos, n);
    f.pos += n;
}
union W512 {
    std::uint32_t u32[16];
};
struct R : reg::RegEntityBundle {
    REG_BUNDLE(R);
    REG(CMD, W512, reg::at(0).array(reg::dims(2), reg::strides(0x40)));
    REG(HI, std::uint64_t, reg::at(0x80).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
    REG(LO, std::uint64_t, reg::at(0x88).transaction(reg::TransactionPolicy::HIGH_TO_LOW));
    REG(LH,
        std::uint64_t,
        reg::at(0x90).transaction(reg::TransactionPolicy{reg::SplitReadMode::SnapshotOnLow,
                                                         reg::SplitWriteMode::Independent}));
    REG(LL,
        std::uint64_t,
        reg::at(0x98).transaction(reg::TransactionPolicy{reg::SplitReadMode::SnapshotOnHigh,
                                                         reg::SplitWriteMode::Independent}));
    REG(T64, std::uint64_t, reg::at(0xa0).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
    REG(T128, reg::u128, reg::at(0xb0).transaction(reg::TransactionPolicy::HIGH_TO_LOW));
    REG(FAIL, reg::u128, reg::at(0xc0));
    REG(W96, reg::WideUInt<96>, reg::at(0xd0));
    REG(W2048, reg::u2048, reg::at(0x100));
    REG(ARR, std::uint32_t, reg::at(0x200).array(reg::dims(2, 3), reg::strides(0x20, 4)));
    REG(TEMP, W512, reg::at(0x280).snapshot(reg::SnapshotKind::Transient));
    REG(DERIVED, std::uint32_t, reg::at(0x2c0).snapshot(reg::SnapshotKind::Derived));
};
struct L : reg::Bundle {
    REG_BUNDLE(L);
    REG_ENTITY(core, R);
    REG_LAYOUT([&] {
        const auto core_view = reg::view("CORE", 0x400).map(core);
        return reg::address_space("ROOT").map(core_view, 0).map(core_view, 0x1000);
    });
};
using Bank = reg::RegisterBank<L>;
struct Device {
    Bank bank{reg::space(0x2000)};
    unsigned read_calls{}, write_calls{}, completions{};
    std::array<W512, 2> responses{}, submitted{}, consumed{};
    Device() {
        for (unsigned e = 0; e < 2; ++e)
            for (unsigned k = 0; k < 16; ++k)
                responses[e].u32[k] = 1000 + e * 100 + k;
        bank->core.CMD.on_read([&](const auto& info) -> W512 {
            ++read_calls;
            return responses.at(info.index.get());
        });
        bank->core.CMD.on_write([&](const auto& info) {
            ++write_calls;
            submitted.at(info.index.get()) = bank->core.CMD.at_element(info.index).ref();
        });
        bank->core.CMD.on_read_complete([&](W512 v, const auto& info) {
            ++completions;
            consumed.at(info.index.get()) = v;
        });
        bank.finish();
    }
};
template <class F>
void rejects(F&& f) {
    bool caught = false;
    try {
        f();
    } catch (const std::exception&) {
        caught = true;
    }
    CHECK(caught);
}
std::size_t slot(const reg::VersionedSnapshot& s, std::string_view ending) {
    for (std::size_t i = 0; i < s.schema.slots.size(); ++i)
        if (s.schema.slots[i].stable_id.ends_with(ending))
            return i;
    throw std::runtime_error("missing snapshot slot");
}
void duplex_and_stream() {
    Device source, target;
    auto& m = source.bank.map();
    for (unsigned e = 0; e < 2; e++) {
        for (unsigned k = 0; k < 8; k++)
            CHECK(m.mmio_write(e * 0x40 + k * 4, e * 100 + k + 1).success());
        CHECK(m.mmio_read(e * 0x40) == 1000 + e * 100);
    }
    source.bank->core.W96.ref().set_word(1, 0x12345678);
    source.bank->core.W2048.ref().set_word(31, 0xfedcba9876543210ull);
    source.bank->core.ARR.at(1, 2).ref() = 99;
    source.bank->core.TEMP.ref().u32[0] = 1;
    target.bank->core.TEMP.ref().u32[0] = 2;
    source.bank->core.DERIVED.ref() = 17;
    target.bank->core.DERIVED.ref() = 18;
    const auto checkpoint = source.bank.save();
    CHECK(checkpoint.runtime.size() == checkpoint.schema.slots.size());
    CHECK(checkpoint.runtime.at(slot(checkpoint, ".CMD")).transactions.size() == 2);
    CHECK(checkpoint.schema.slots.size() == 11); // aliases and excluded nodes add no slots
    File file;
    source.bank.save(static_cast<void*>(&file));
    target.bank.load(static_cast<void*>(&file));
    CHECK(file.pos == file.data.size());
    CHECK(target.bank.save() == checkpoint);
    CHECK(target.bank->core.TEMP.ref().u32[0] == 2 && target.bank->core.DERIVED.ref() == 18);
    CHECK(target.bank->core.W96.ref().word(1) == 0x12345678);
    CHECK(target.bank->core.W2048.ref().word(31) == 0xfedcba9876543210ull);
    CHECK(target.bank->core.ARR.at(1, 2).ref() == 99);
    CHECK(target.read_calls == 0 && target.write_calls == 0 && target.completions == 0);
    for (unsigned e = 0; e < 2; e++) {
        for (auto& x : target.responses[e].u32)
            x = 0; // resumed read must use original frozen response
        auto& t = target.bank.map();
        for (unsigned k = 8; k < 16; k++)
            CHECK(t.mmio_write(0x1000 + e * 0x40 + k * 4, e * 100 + k + 1).success());
        for (unsigned k = 1; k < 16; k++)
            CHECK(t.mmio_read(0x1000 + e * 0x40 + k * 4) == 1000 + e * 100 + k);
        for (unsigned k = 0; k < 16; k++) {
            CHECK(target.submitted[e].u32[k] == e * 100 + k + 1);
            CHECK(target.consumed[e].u32[k] == 1000 + e * 100 + k);
        }
    }
    CHECK(target.read_calls == 0 && target.write_calls == 2 && target.completions == 2);
    // Same-instance restores retain view/pointer identities and do not replay callbacks.
    auto handle = source.bank->core.CMD.at(0);
    auto* address = &handle.ref();
    source.bank.load(checkpoint);
    CHECK(&handle.ref() == address && source.bank.save() == checkpoint);
    File second;
    source.bank.Save(&second);
    Device other;
    other.bank.Load(&second);
    CHECK(other.bank.save() == checkpoint);
    File explicit_archive;
    hac::snapshot::Writer writer(&explicit_archive);
    source.bank.save(writer);
    hac::snapshot::Reader reader(&explicit_archive);
    other.bank.load(reader);
    CHECK(other.bank.save() == checkpoint);
}
void transaction_policies() {
    Bank source(reg::space(0x2000)), target(reg::space(0x2000));
    unsigned first_reads = 0, resumed_reads = 0, completed = 0, written = 0;
    auto bind = [&](auto& b, bool incoming) {
        auto read = [&, incoming]() -> std::uint64_t {
            if (incoming) {
                ++resumed_reads;
                return 0xaaaaaaaa55555555ull;
            }
            ++first_reads;
            return 0x1122334455667788ull;
        };
        b->core.HI.on_read(read);
        b->core.LO.on_read(read);
        b->core.LH.on_read(read);
        b->core.LL.on_read(read);
        b->core.T64.on_read(read);
        if (incoming) {
            b->core.HI.on_read_complete([&] {
                ++completed;
            });
            b->core.LO.on_read_complete([&] {
                ++completed;
            });
            b->core.T64.on_read_complete([&] {
                ++completed;
            });
            b->core.HI.on_write([&] {
                ++written;
            });
            b->core.LO.on_write([&] {
                ++written;
            });
            b->core.T64.on_write([&] {
                ++written;
            });
        }
    };
    bind(source, false);
    bind(target, true);
    source->core.T128.on_read([] {
        return reg::u128::from_hex("112233445566778899aabbccddeeff00");
    });
    unsigned high_reads = 0, high_done = 0, high_writes = 0;
    target->core.T128.on_read([&] {
        ++high_reads;
        return reg::u128{};
    });
    target->core.T128.on_read_complete([&] {
        ++high_done;
    });
    target->core.T128.on_write([&] {
        ++high_writes;
    });
    unsigned failed_done = 0;
    source->core.FAIL.on_read([]() -> reg::u128 {
        throw reg::Error("source unavailable");
    });
    target->core.FAIL.on_read_complete([&] {
        ++failed_done;
    });
    source.finish();
    target.finish();
    auto& m = source.map();
    CHECK(m.mmio_read(0x80) == 0x55667788);
    CHECK(m.mmio_write(0x80, 0xabcdef01).success());
    CHECK(m.mmio_read(0x8c) == 0x11223344);
    CHECK(m.mmio_write(0x8c, 0xabcdef02).success());
    CHECK(m.mmio_read(0x90) == 0x55667788);
    CHECK(m.mmio_read(0x9c) == 0x11223344);
    CHECK(m.mmio_read(0xa0) == 0x55667788);
    CHECK(m.mmio_write(0xa0, 0xabcdef03).success());
    CHECK(m.mmio_read_sized(0xb8, 8) == 0x1122334455667788ull);
    CHECK(m.mmio_write_sized(0xb8, 0x8877665544332211ull, 8).success());
    CHECK(m.mmio_read(0xc0) == 0); // Preserve read_source_ok=false, don't invoke completion later.
    const auto checkpoint = source.save();
    target.load(checkpoint);
    auto& t = target.map();
    CHECK(t.mmio_read(0x84) == 0x11223344);
    CHECK(t.mmio_read(0x88) == 0x55667788);
    CHECK(t.mmio_read(0x94) == 0x11223344);
    CHECK(t.mmio_read(0x98) == 0x55667788);
    CHECK(t.mmio_read(0xa4) == 0x11223344);
    CHECK(completed == 3 && resumed_reads == 0);
    CHECK(t.mmio_write(0x84, 0x12345678).success());
    CHECK(target->core.HI.ref() == 0x12345678abcdef01ull);
    CHECK(t.mmio_write(0x88, 0x12345679).success());
    CHECK(target->core.LO.ref() == 0xabcdef0212345679ull);
    CHECK(t.mmio_write(0xa4, 0x1234567a).success());
    CHECK(target->core.T64.ref() == 0x1234567aabcdef03ull);
    CHECK(written == 3);
    CHECK(t.mmio_read_sized(0xb0, 8) == 0x99aabbccddeeff00ull && high_reads == 0 && high_done == 1);
    CHECK(t.mmio_write_sized(0xb0, 0x123456789abcdef0ull, 8).success() && high_writes == 1);
    CHECK(target->core.T128.ref().word(1) == 0x8877665544332211ull);
    CHECK(t.mmio_read(0xcc) == 0 && failed_done == 0);
    CHECK(first_reads == 5);
}
void malformed_and_raw() {
    Device d;
    auto& b = d.bank;
    auto& m = b.map();
    CHECK(m.mmio_write(0, 0xaabbccdd).success());
    CHECK(m.mmio_read(0) == 1000);
    const auto good = b.save();
    const auto cmd = slot(good, ".CMD");
    auto bad = [&](auto mutate) {
        auto copy = good;
        mutate(copy);
        rejects([&] {
            b.load(copy);
        });
        CHECK(b.save() == good);
    };
    bad([](auto& x) {
        x.schema.format_version = 1;
    });
    bad([](auto& x) {
        x.schema.layout_hash ^= 1;
    });
    bad([](auto& x) {
        x.data.pop_back();
    });
    bad([](auto& x) {
        x.runtime.pop_back();
    });
    bad([&](auto& x) {
        x.runtime[cmd].transactions.push_back(x.runtime[cmd].transactions[0]);
    });
    bad([&](auto& x) {
        x.runtime[cmd].transactions[0].element = 2;
    });
    bad([&](auto& x) {
        x.runtime[cmd].transactions[0].read_snapshot.pop_back();
    });
    bad([&](auto& x) {
        x.runtime[cmd].transactions[0].write_mask[0] = 1;
    });
    bad([&](auto& x) {
        x.runtime[cmd].transactions[0].write_pending.back() = 1;
    });
    bad([&](auto& x) {
        x.schema.slots[cmd].dimensions = {1, 2};
    });
    bad([&](auto& x) {
        auto i = slot(x, ".W96");
        x.data[x.schema.slots[i].start_word + 1] |= 1ull << 40;
    });
    File file;
    b.save(&file);
    file.data.pop_back();
    rejects([&] {
        b.load(&file);
    });
    CHECK(b.save() == good);
    File wrong;
    b.save(&wrong);
    wrong.data[0] ^= 1;
    rejects([&] {
        b.load(&wrong);
    });
    CHECK(b.save() == good);
    // Legacy spans remain value-only; callers must use bank.save for resumable state.
    std::vector<reg::RegData> words(m.total_snapshot_data_count());
    m.dump_state(words);
    m.restore_state(words);
    CHECK(m.try_mmio_read_sized(60, 4).result == reg::RegMemTxResult::Unsupported);
    CHECK(m.mmio_write(60, 7).success());
    CHECK(b->core.CMD.at(0).ref().u32[0] == 0);
}
struct Parent {
    Bank bank{reg::space(0x2000).finalize()};
    std::queue<unsigned> fifo;
    std::array<unsigned, 2> route{};
    VMSTATE_FIELDS(bank, fifo, route);
};
void field_recursion() {
    Parent source, target;
    source.fifo.push(17);
    source.route = {1, 0};
    CHECK(source.bank.map().mmio_write(0, 0x1234).success());
    File file;
    source.Save(&file);
    target.Load(&file);
    CHECK(target.fifo.front() == 17 && target.route[0] == 1 &&
          target.bank.save() == source.bank.save());
    CHECK(target.bank.map().mmio_write(60, 0x99).success());
    CHECK(target.bank->core.CMD.at(0).ref().u32[0] == 0x1234);
}
void geometry_and_limits() {
    reg::RegisterMap a(0x100), b(0x100), c(0x100), d(0x100);
    auto x = a.create_reg<std::uint32_t>(
        0, "SAME", reg::at(0).array(reg::dims(2, 3), reg::strides(0x20, 4)));
    auto y = b.create_reg<std::uint32_t>(
        0, "SAME", reg::at(0).array(reg::dims(3, 2), reg::strides(0x20, 4)));
    c.create_reg<std::uint32_t>(0, "SAME");
    d.create_reg<std::uint64_t>(0, "SAME");
    a.finalize();
    b.finalize();
    c.finalize();
    d.finalize();
    x.at(0, 0).ref() = 99;
    rejects([&] {
        b.restore_state_versioned(a.dump_state_versioned());
    });
    CHECK(y.at(0, 0).ref() == 0);
    rejects([&] {
        d.restore_state_versioned(c.dump_state_versioned());
    });
    Device device;
    File file;
    device.bank.save(&file);
    auto before = device.bank.save();
    hac::snapshot::Limits limits;
    limits.max_total_bytes = 32;
    hac::snapshot::Reader reader(&file, limits);
    rejects([&] {
        device.bank.load(reader);
    });
    CHECK(device.bank.save() == before);
}
void reject_reentrancy() {
    Device d;
    bool rejected = false;
    d.bank.map().set_pre_access([&](const auto&) {
        try {
            (void)d.bank.save();
        } catch (const reg::Error&) {
            rejected = true;
        }
    });
    CHECK(d.bank.map().mmio_write(0, 1).success() && rejected);
}
int main() try {
    duplex_and_stream();
    transaction_policies();
    malformed_and_raw();
    field_recursion();
    geometry_and_limits();
    reject_reentrancy();
    std::cout
        << "PASS: complete snapshots, two array elements, shared aliases, pending reads/writes, "
           "unified 64-bit transactions, high-to-low, failed-read flags, 96/2048-bit values, "
           "atomic rejection, original vmstate_fields streams and parent recursion\n";
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
