#include <regfw/regfw.hpp>
#include <iostream>
#include <stdexcept>
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!(__VA_ARGS__))                                                                        \
            throw std::runtime_error(#__VA_ARGS__);                                                \
    } while (false)
union Status {
    std::uint32_t u32;
    struct {
        std::uint32_t count : 16, reserved : 16;
    } bits;
};
union WideStatus {
    std::uint32_t u32[16];
};
int main() {
    reg::RegisterMap map;
    map.init(4096, "READS");
    auto status = map.create_reg<Status>(0, "STATUS", reg::at(0).reset(5));
    bool use_model = true;
    unsigned busy_count = 42, calls = 0;
    status.on_read([&]() -> std::optional<Status> {
        ++calls;
        if (!use_model)
            return std::nullopt;
        Status value{};
        value.bits.count = busy_count;
        return value;
    });
    CHECK(map.mmio_read(0) == 42 && calls == 1 && status.u32 == 5);
    use_model = false;
    CHECK(map.mmio_read(0) == 5 && calls == 2);

    auto rc = map.create_reg<Status>(4, "RC", reg::at(0).reset(9).access(reg::rc));
    rc.on_read([](Status candidate, const auto& info) -> std::optional<Status> {
        CHECK(candidate.u32 == 9 && info.stored_after == 0);
        return std::nullopt;
    });
    CHECK(map.mmio_read(4) == 9 && rc.u32 == 0);
    auto direct = map.create_reg<Status>(8, "DIRECT");
    direct.on_read([] {
        Status v{};
        v.u32 = 17;
        return v;
    });
    CHECK(map.mmio_read(8) == 17);
    auto notification = map.create_reg<Status>(12, "NOTIFY", reg::at(0).reset(3));
    unsigned notified = 0;
    notification.on_read([&] {
        ++notified;
    });
    CHECK(map.mmio_read(12) == 3 && notified == 1);

    auto wide = map.create_reg<WideStatus>(0x100, "WIDE");
    WideStatus model{};
    model.u32[0] = 0x11223344;
    model.u32[15] = 0xaabbccdd;
    unsigned snapshots = 0, completed = 0;
    wide.on_read([&]() -> std::optional<WideStatus> {
        ++snapshots;
        return model;
    });
    // Optional completion side effect. Most registers need only on_read.
    wide.on_read_complete([&](WideStatus snapshot, const auto&) {
        CHECK(snapshot.u32[0] == 0x11223344 && snapshot.u32[15] == 0xaabbccdd);
        ++completed;
    });
    CHECK(map.mmio_read_sized(0x100, 8) == 0x11223344 && snapshots == 1 && completed == 0);
    model.u32[15] = 0x99999999;
    CHECK(map.mmio_read_sized(0x138, 8) == 0xaabbccdd00000000ull && snapshots == 1 &&
          completed == 1);
    CHECK(wide.u32[0] == 0); // Returned snapshot does not overwrite backing storage.

    auto split = map.create_reg<std::uint64_t>(
        0x200, "SPLIT", reg::at(0).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
    unsigned split_calls = 0;
    split.on_read([&]() -> std::optional<std::uint64_t> {
        ++split_calls;
        return 0xaabbccdd11223344ull;
    });
    CHECK(map.mmio_read(0x200) == 0x11223344 && map.mmio_read(0x204) == 0xaabbccdd &&
          split_calls == 1);
    auto indexed = map.create_reg<Status>(
        0x300, "ARRAY", reg::at(0).array(reg::dims(2, 3), reg::strides(0x20, 4)));
    indexed.on_read([](const auto& info) -> std::optional<Status> {
        auto [i, j] = info.index.get();
        Status v{};
        v.u32 = i * 10 + j;
        return v;
    });
    CHECK(map.mmio_read(0x328) == 12);
    auto failure = map.create_reg<Status>(0x400, "FAIL");
    failure.on_read([]() -> Status {
        throw reg::Error("model unavailable");
    });
    CHECK(map.try_mmio_read_sized(0x400, 4).success() &&
          map.mmio_read(0x400) == reg::REG_READ_CALLBACK_ERROR_MAGIC);
    bool rejected = false;
    try {
        status.on_read([] {
            return Status{};
        });
    } catch (const reg::Error&) {
        rejected = true;
    }
    CHECK(rejected);
    map.finalize();
    std::cout << "PASS: unified on_read, optional/nullopt, direct/void callbacks, RC behavior, "
                 "wide snapshots, 64-bit transactions and typed indices\n";
}
