#include <regfw/regfw.hpp>
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
union Wide {
    std::uint32_t u32[4];
};
struct BadRegisters : reg::Bundle {
    REG_BUNDLE(BadRegisters);
    REG(FIRST, std::uint64_t, reg::at(0x10));
    REG(SECOND, std::uint32_t, reg::at(0x14));
};
struct Empty : reg::Bundle {
    REG_BUNDLE(Empty);
};
struct BadApertures : reg::Bundle {
    REG_BUNDLE(BadApertures);
    REG_APERTURE(FIRST, Empty, reg::aperture().at(0x10).size_bytes(0x40));
    REG_APERTURE(SECOND, Empty, reg::aperture().at(0x40).size_bytes(0x40));
};
template <class F>
void conflict(F action, std::string_view first, std::string_view second) {
    bool caught = false;
    try {
        action();
    } catch (const reg::Error& e) {
        caught = true;
        CHECK(e.kind == reg::ErrorKind::Configuration);
        const std::string message = e.what();
        CHECK(message.find(first) != std::string::npos &&
              message.find(second) != std::string::npos);
        CHECK(message.find("0x") != std::string::npos &&
              message.find("bytes") != std::string::npos);
        const auto at = message.find("diagnostics.cpp:");
        CHECK(at != std::string::npos);
        CHECK(message.find("diagnostics.cpp:", at + 1) != std::string::npos);
        CHECK(std::string_view(e.location.file_name()).ends_with("diagnostics.cpp"));
    }
    CHECK(caught);
}
void declaration_errors() {
    conflict(
        [] {
            reg::RegisterBank<BadRegisters> bank(reg::space(0x100).name("ROOT"));
        },
        "FIRST",
        "SECOND");
    conflict(
        [] {
            reg::RegisterBank<BadApertures> bank(reg::space(0x100).name("ROOT"));
        },
        "FIRST",
        "SECOND");
    conflict(
        [] {
            reg::RegisterMap arrays(0x100);
            arrays.create_reg<std::uint32_t>(
                0, "ARRAY", reg::at(0).array(reg::dims(2), reg::strides(0)));
        },
        "ARRAY[0]",
        "ARRAY[1]");
}
void read_only_and_truncation(HLogLogger* logger) {
    reg::RegisterMap map(0x400, "RO", logger);
    auto ro = map.create_reg<std::uint32_t>(
        0, "RO32", reg::at(0).reset(7).access(reg::ro).array(reg::dims(2), reg::strides(4)));
    auto ro64 = map.create_reg<std::uint64_t>(
        0x20, "RO64", reg::at(0).access(reg::ro).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
    auto rowide = map.create_reg<Wide>(0x40, "RO128", reg::at(0).access(reg::ro));
    auto rw = map.create_reg<std::uint64_t>(0x80, "RW64");
    auto wide = map.create_reg<Wide>(0x100, "WIDE128");
    auto high = map.create_reg<Wide>(
        0x120, "HIGH128", reg::at(0).transaction(reg::TransactionPolicy::HIGH_TO_LOW));
    unsigned callbacks = 0, index_calls = 0, pre_calls = 0, identity_calls = 0;
    ro.on_write([&] {
        ++callbacks;
    });
    ro64.on_write([&] {
        ++callbacks;
    });
    rowide.on_write([&] {
        ++callbacks;
    });
    reg::Groups groups;
    groups.group("RO_INDEX")
        .members(ro)
        .coordinate(Logical, reg::index<0>)
        .derive(Physical)
        .from(Logical)
        .using_resolver([&](std::uint32_t n) {
            ++index_calls;
            return n;
        });
    groups.install();
    map.set_pre_access([&](const auto&) {
        ++pre_calls;
    });
    reg::RegLayout layout(map);
    layout.identity_repeat_window({0, 1, 8, 8}, 7, reg::IdentityRemapMode::IdentityOnly);
    map.set_identity_resolver([&](const auto& r) {
        ++identity_calls;
        return r.request_id;
    });
    map.map_alias_window_with_policy(
        0x200, 0, 8, reg::AliasAccessPolicy::READ_ONLY_IGNORE_WRITES, "RO_ALIAS");
    map.finalize();
    CHECK(map.mmio_write(0, 99).result == reg::RegMemTxResult::Ignored);
    CHECK(map.mmio_write(0x20, 99).result == reg::RegMemTxResult::Ignored);
    CHECK(map.mmio_write(0x24, 99).result == reg::RegMemTxResult::Ignored);
    CHECK(map.mmio_write(0x40, 99).result == reg::RegMemTxResult::Ignored);
    CHECK(map.mmio_write(0x4c, 99).result == reg::RegMemTxResult::Ignored);
    CHECK(map.mmio_write(0x200, 99).result == reg::RegMemTxResult::Ignored);
    CHECK(callbacks == 0 && index_calls == 0 && pre_calls == 0 && identity_calls == 2);
    CHECK(ro.at(0).ref() == 7 && ro64.ref() == 0 && rowide.u32[0] == 0);
    CHECK(map.mmio_write(0x80, 0x1122334455667788ull).success());
    CHECK(rw.ref() == 0x55667788);
    CHECK(map.mmio_write_sized(0x80, 0x1122334455667788ull, 8).success());
    CHECK(rw.ref() == 0x1122334455667788ull);
    CHECK(map.try_mmio_read_sized(0x104, 4).result == reg::RegMemTxResult::Unsupported);
    CHECK(map.try_mmio_read_sized(0x120, 4).result == reg::RegMemTxResult::Unsupported);
    CHECK(map.try_mmio_read_sized(0x100, 4).success());
    CHECK(map.try_mmio_read_sized(0x10c, 4).success());
    CHECK(map.try_mmio_read_sized(0x12c, 4).success());
    CHECK(map.try_mmio_read_sized(0x120, 4).success());
    (void)wide;
    (void)high;
}
void lint_and_explain() {
    reg::RegisterMap map(0x400, "EXPLAIN");
    auto item =
        map.create_reg<std::uint32_t>(0, "ITEM", reg::at(0).array(reg::dims(2), reg::strides(4)));
    unsigned calls = 0;
    reg::Groups groups;
    groups.group("POOL")
        .members(item)
        .coordinate(Logical, reg::index<0>)
        .derive(Physical)
        .from(Logical)
        .when(reg::identity_domain(7))
        .using_resolver([&](std::uint32_t n) {
            ++calls;
            return n;
        })
        .otherwise(reg::passthrough(Logical));
    groups.install();
    auto warning = map.layout_warnings();
    CHECK(warning.size() == 1 && warning[0].code == "REGFW_MISSING_IDENTITY_DOMAIN");
    CHECK(map.describe_layout().find("LAYOUT WARNINGS") != std::string::npos);
    reg::RegLayout layout(map);
    layout.identity_repeat_window({0, 1, 8, 8}, 7, reg::IdentityRemapMode::IdentityOnly);
    map.set_identity_resolver([&](const auto& r) {
        ++calls;
        return r.request_id;
    });
    map.map_alias_window(0x100, 0, 8, "INNER");
    map.map_alias_window(0x200, 0x100, 8, "OUTER");
    auto command = map.create_reg<std::uint32_t>(0x300, "COMMAND");
    command.on_write([&](const reg::RegAccessInfo&) {
        ++calls;
    });
    map.finalize();
    CHECK(map.layout_warnings().empty());
    const auto before = map.dump_state_versioned().data;
    const auto text = map.explain_address(0x200);
    CHECK(text.find("OUTER") != std::string::npos && text.find("INNER") != std::string::npos);
    CHECK(text.find("IDENTITY") != std::string::npos && text.find("POOL") != std::string::npos &&
          text.find("ITEM") != std::string::npos);
    CHECK(map.explain_address(0x300).find("REGISTER COMMAND") != std::string::npos);
    CHECK(map.explain_address(0x380).find("unmapped") != std::string::npos);
    CHECK(map.explain_address(1).find("unaligned") != std::string::npos);
    CHECK(calls == 0 && before == map.dump_state_versioned().data);
    // A pending wide read snapshot must survive inspection.
    reg::RegisterMap wide_map(0x100);
    auto wide = wide_map.create_reg<Wide>(0, "SNAPSHOT");
    wide.on_read([&] {
        ++calls;
        Wide value{};
        value.u32[3] = 42;
        return value;
    });
    wide_map.finalize();
    CHECK(wide_map.try_mmio_read_sized(0, 4).success() && calls == 1);
    (void)wide_map.explain_address(12);
    CHECK(calls == 1);
    CHECK(wide_map.try_mmio_read_sized(12, 4).value == 42 && calls == 1);
    // Dynamic bank redirection remains symbolic unless the caller supplies an ID.
    reg::RegisterMap dynamic(0x200);
    auto a = dynamic.create_reg<std::uint32_t>(0, "VF0");
    auto b = dynamic.create_reg<std::uint32_t>(0x100, "VF1");
    reg::RegLayout dl(dynamic);
    dl.identity_repeat_window({0, 2, 0x100, 0x100}, 9, reg::IdentityRemapMode::RedirectBank);
    dynamic.set_identity_resolver([&](const auto&) {
        ++calls;
        return 1u;
    });
    dynamic.finalize();
    CHECK(dynamic.explain_address(0).find("resolved_id=?") != std::string::npos);
    CHECK(dynamic.explain_address(0, 4, reg::RegTransactionKind::Read, 1).find("REGISTER VF1") !=
          std::string::npos);
    CHECK(dynamic.explain_address(0, 4, reg::RegTransactionKind::Read, 2).find("out of range") !=
          std::string::npos);
    CHECK(calls == 1 && a.ref() == 0 && b.ref() == 0);
    reg::RegisterMap native(0x100);
    auto full = native.create_reg<std::uint64_t>(
        0x10, "NATIVE64", reg::at(0).access_widths(reg::RegisterAccessWidthPolicy::U64_ONLY));
    native.finalize();
    CHECK(native.mmio_write(0x10, 1).result == reg::RegMemTxResult::Unsupported);
    CHECK(native.mmio_write_sized(0x10, 0x1122334455667788ull, 8).success());
    CHECK(full.ref() == 0x1122334455667788ull);
}
} // namespace
int main() try {
    auto* logger = hlog_register_logger("diagnostics", "verification", HLOG_LEVEL_WARN);
    CHECK(logger);
    declaration_errors();
    read_only_and_truncation(logger);
    lint_and_explain();
    hlog_flush();
    char path[2048];
    CHECK(hlog_file_path(path, sizeof(path)) < sizeof(path));
    std::ifstream file(path);
    CHECK(file.good());
    const std::string log{std::istreambuf_iterator<char>(file), {}};
    CHECK(log.find("write-truncation") != std::string::npos &&
          log.find("1122334455667788") != std::string::npos);
    CHECK(log.find("register=WIDE128") != std::string::npos &&
          log.find("required first beat byte offset=0") != std::string::npos);
    CHECK(log.find("register=HIGH128") != std::string::npos &&
          log.find("required first beat byte offset=12") != std::string::npos);
    std::cout << "PASS: declaration facts, RO suppression, truncation/beat diagnostics, lint and "
                 "side-effect-free explanations\n";
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
