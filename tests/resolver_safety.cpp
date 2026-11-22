#include <regfw/regfw.hpp>
#include <regfw/qemu_bridge.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!(__VA_ARGS__))                                                                        \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " +      \
                                     #__VA_ARGS__);                                                \
    } while (false)
namespace {
REG_ACCESS_FIELD(Logical, std::uint32_t);
REG_ACCESS_FIELD(Physical, std::uint32_t);
struct Indexed : reg::Bundle {
    REG_BUNDLE(Indexed);
    REG(VALUE, std::uint32_t, reg::at(0).array(reg::dims(2), reg::strides(4)));
    REG_ACCESS_GROUP(POOL,
                     .members = {VALUE},
                     .coordinates = {{Logical, 0}},
                     .derive = {reg::map_index(Logical, Physical).using_resolver(Physical)});
};
template <class F>
void configuration_error(F&& f, std::string_view name) {
    bool caught = false;
    try {
        f();
    } catch (const reg::Error& error) {
        caught = true;
        CHECK(error.kind == reg::ErrorKind::Configuration);
        CHECK(std::string_view(error.what()).find(name) != std::string_view::npos);
    }
    CHECK(caught);
}
void index_binding() {
    reg::RegisterMap unused(0x100, "UNUSED");
    unused.set_access_resolver(Physical, [](const auto&) {
        return 0u;
    });
    unused.clear_access_resolver(Physical);
    unused.finalize(); // Unreferenced callbacks are not required dependencies.
    reg::RegisterBank<Indexed> bank(reg::space(0x100).name("INDEX"));
    configuration_error(
        [&] {
            bank.finish();
        },
        "index:Physical");
    CHECK(!bank.map().is_finalized());
    unsigned calls = 0;
    bank.map().set_access_resolver(Physical, [&](const auto& info) {
        ++calls;
        return 1u - info.index_as(Logical);
    });
    bank.finish();
    CHECK(calls == 0);
    (void)bank.map().describe_layout(reg::LayoutDetail::Full);
    CHECK(calls == 0);
    CHECK(bank.map().mmio_write(0, 17).success());
    CHECK(bank->VALUE.at(1).ref() == 17 && bank->VALUE.at(0).ref() == 0 && calls == 1);
    configuration_error(
        [&] {
            bank.map().clear_access_resolver(Physical);
        },
        "frozen");
    configuration_error(
        [&] {
            bank.map().set_access_resolver(Physical, [](const auto&) {
                return 0u;
            });
        },
        "frozen");
    CHECK(bank.map().mmio_read(0) == 17);

    reg::RegisterBank<Indexed> late(reg::space(0x100).name("LATE"));
    late.map().defer_access_resolver(Physical);
    configuration_error(
        [&] {
            late.finish();
        },
        "index:Physical");
    CHECK(late.map().mmio_write(0, 19).result == reg::RegMemTxResult::DeviceError);
    late.map().set_access_resolver(Physical, [](const auto& info) {
        return info.index_as(Logical);
    });
    late.finish();
    CHECK(late.map().mmio_write(0, 19).success() && late->VALUE.at(0).ref() == 19);
}
REG_ACCESS_FIELD(IdentityRoute, std::uint32_t);
struct IdentityWord : reg::RegEntityBundle {
    REG_BUNDLE(IdentityWord);
    REG(VALUE, std::uint32_t, reg::at(0));
};
struct IdentityIO : reg::Bundle {
    REG_BUNDLE(IdentityIO);
    REG_REPEAT_ENTITY(vf, IdentityWord, 2);
    REG_LAYOUT([&] {
        return reg::address_space("IO").map_repeat(
            "VFS", 0, reg::repeat(2).stride(0x100), [&](auto i) {
                return reg::view("VF", 0x100).map(vf[i]).identity(7).using_resolver(IdentityRoute);
            });
    });
};
void identity_failure() {
    reg::RegisterBank<IdentityIO> bank(reg::space(0x200).name("IDENTITY"));
    auto& map = bank.map();
    auto& first = bank->vf[0].VALUE;
    auto& second = bank->vf[1].VALUE;
    first.ref() = 11;
    second.ref() = 22;
    configuration_error(
        [&] {
            map.finalize();
        },
        "identity");
    unsigned calls = 0, events = 0;
    unsigned mode = 0;
    first.on_write([&] {
        ++events;
    });
    second.on_write([&] {
        ++events;
    });
    map.set_identity_resolver(IdentityRoute, [&](const auto& request) -> int {
        ++calls;
        if (mode == 1)
            throw std::runtime_error("route table broken");
        if (mode == 2)
            return 2;
        if (mode == 3)
            throw reg::Error("identity denied", reg::ErrorKind::Identity);
        if (mode == 4)
            return -1;
        return static_cast<int>(request.request_id);
    });
    map.finalize();
    CHECK(calls == 0);
    (void)map.describe_layout();
    CHECK(calls == 0);
    const auto before = map.dump_state_versioned();
    mode = 1;
    CHECK(map.try_mmio_read_sized(0, 4).result == reg::RegMemTxResult::DeviceError);
    CHECK(map.mmio_write(0, 33).result == reg::RegMemTxResult::DeviceError);
    auto bridge = reg::make_qemu_bridge(map);
    std::uint64_t value{};
    CHECK(bridge.read(bridge.opaque, 0, &value, 4) != 0);
    CHECK(bridge.write(bridge.opaque, 0, 33, 4) != 0);
    mode = 2;
    CHECK(map.try_mmio_read_sized(0, 4).result == reg::RegMemTxResult::AccessError);
    CHECK(map.mmio_write(0, 33).result == reg::RegMemTxResult::AccessError);
    map.set_identity_rejection_policy(reg::IdentityRejectionPolicy::Ignore);
    const auto ignored = map.try_mmio_read_sized(0, 4);
    CHECK(ignored.result == reg::RegMemTxResult::Ignored && ignored.value == 0xdeadbeef);
    CHECK(map.mmio_write(0, 33).result == reg::RegMemTxResult::Ignored);
    mode = 3;
    CHECK(map.mmio_write(0, 33).result == reg::RegMemTxResult::Ignored);
    mode = 4;
    CHECK(map.mmio_write(0, 33).result == reg::RegMemTxResult::Ignored);
    mode = 1;
    CHECK(map.mmio_write(0, 33).result == reg::RegMemTxResult::DeviceError);
    configuration_error(
        [&] {
            map.clear_identity_resolver(IdentityRoute);
        },
        "frozen");
    CHECK(map.try_mmio_read_sized(0, 4).result == reg::RegMemTxResult::DeviceError);
    CHECK(map.dump_state_versioned().data == before.data && events == 0);

    reg::RegisterBank<IdentityIO> unbound(reg::space(0x200).name("LATE_IDENTITY"));
    auto& late = unbound.map();
    auto& value_reg = unbound->vf[0].VALUE;
    late.defer_identity_resolver(IdentityRoute);
    configuration_error(
        [&] {
            late.finalize();
        },
        "identity");
    CHECK(late.mmio_write(0, 9).result == reg::RegMemTxResult::DeviceError);
    late.set_identity_resolver(IdentityRoute, [](const auto& request) {
        return request.request_id;
    });
    late.finalize();
    CHECK(late.mmio_write(0, 9).success() && value_reg.ref() == 9);
}
void failed_request_log() {
    auto* logger = hlog_register_logger("resolver_safety", "errors", HLOG_LEVEL_ERROR);
    CHECK(logger);
    reg::RegisterMap map(0x100, "HOLE", logger);
    map.finalize();
    CHECK(map.try_mmio_read_sized(0x80, 4).result == reg::RegMemTxResult::DecodeError);
    CHECK(map.try_mmio_write_sized(0x88, 0xaabbccdd, 4).result == reg::RegMemTxResult::DecodeError);
    CHECK(map.try_mmio_write_sized(0, 7, 65536).result == reg::RegMemTxResult::Unsupported);
    hlog_flush();
    char path[1024];
    CHECK(hlog_file_path(path, sizeof(path)) < sizeof(path));
    std::ifstream file(path);
    CHECK(file.good());
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    CHECK(text.find("op=read | addr=0x00000080 | size=4B") != std::string::npos);
    CHECK(text.find("op=write | addr=0x00000088 | size=4B | write=0xaabbccdd") !=
          std::string::npos);
    CHECK(text.find("op=write | addr=0x00000000 | size=65536B | write=0x7") != std::string::npos);
}
} // namespace
int main() try {
    index_binding();
    identity_failure();
    failed_request_log();
    std::cout << "PASS: strict/deferred bindings, identity rejection/error policies, unchanged "
                 "storage, QEMU faults and request diagnostics\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
