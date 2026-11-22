#include <regfw/regfw.hpp>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
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
struct RORegisters : reg::RegEntityBundle {
    REG_BUNDLE(RORegisters);
    REG(RO32,
        std::uint32_t,
        reg::at(0).reset(7).access(reg::ro).array(reg::dims(2), reg::strides(4)));
    REG(RO64,
        std::uint64_t,
        reg::at(0x20).access(reg::ro).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
    REG(RO128, Wide, reg::at(0x40).access(reg::ro));
    REG(RW64, std::uint64_t, reg::at(0x80));
    REG(WIDE128, Wide, reg::at(0x100));
    REG(HIGH128, Wide, reg::at(0x120).transaction(reg::TransactionPolicy::HIGH_TO_LOW));
};
struct ROLayout : reg::Bundle {
    REG_BUNDLE(ROLayout);
    REG_ENTITY(regs, RORegisters);
    REG_LAYOUT([&] {
        return reg::address_space("RO_IO")
            .map(reg::view("BASE", 0x200).map(regs), 0)
            .map(reg::view("IDENTITY", 8).map(regs.RO32).identity(7), 0, 1)
            .map(reg::view("RO_ALIAS", 8).map(regs.RO32).identity(7).read_only_ignore_writes(),
                 0x200);
    });
};
void read_only_and_truncation(HLogLogger* logger) {
    reg::RegisterBank<ROLayout> bank(reg::space(0x400).name("RO"));
    auto& map = bank.map();
    map.set_logger(logger);
    auto& ro = bank->regs.RO32;
    auto& ro64 = bank->regs.RO64;
    auto& rowide = bank->regs.RO128;
    auto& rw = bank->regs.RW64;
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
    map.set_identity_resolver([&](const auto& r) {
        ++identity_calls;
        return r.request_id;
    });
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
}
void wide_hex_formatting() {
    CHECK(reg::WideUInt<65>::from_words({1, 1}).hex() == "10000000000000001");
    CHECK(reg::WideUInt<127>::mask(127).hex() == "7" + std::string(31, 'f'));
    CHECK(reg::WideUInt<129>::from_words({11, 0, 1}).hex() == "1" + std::string(31, '0') + "b");
    reg::WideRegData wide;
    CHECK(wide.hex() == "0" && wide.hex(true) == std::string(512, '0'));
    wide = 0xbc;
    CHECK(wide.hex() == "bc" && wide.hex(true) == std::string(510, '0') + "bc");
    wide.set_bit(2047, true);
    CHECK(wide.hex() == "8" + std::string(509, '0') + "bc");
}
void truncation_widths(HLogLogger* logger, std::ifstream& log) {
    reg::RegisterMap map(0x20, "TRUNCATION_WIDTHS", logger);
    map.create_reg<std::uint64_t>(0x10, "VALUE");
    map.finalize();
    log.clear();
    log.seekg(0, std::ios::end);
    for (unsigned size : {1u, 2u, 4u}) {
        const auto overflow = reg::RegData{1} << (size * 8);
        CHECK(map.mmio_write_sized(0x10, overflow, size).success());
        CHECK(map.mmio_write_sized(0x10, overflow - 1, size).success());
    }
    CHECK(map.mmio_write_sized(0x10, UINT64_MAX, 8).success());
    for (unsigned size : {0u, 3u, std::numeric_limits<unsigned>::max()})
        CHECK(map.mmio_write_sized(0x10, UINT64_MAX, size).result ==
              reg::RegMemTxResult::Unsupported);

    hlog_flush();
    constexpr std::array expected{
        "size=1B | write=0x100", "size=2B | write=0x10000", "size=4B | write=0x100000000"};
    std::size_t count = 0;
    for (std::string line; std::getline(log, line);) {
        if (line.find("WARN write-truncation TRUNCATION_WIDTHS ") == std::string::npos)
            continue;
        CHECK(count < expected.size() && line.ends_with(expected[count]));
        ++count;
    }
    CHECK(count == expected.size());
}
struct LintRegs : reg::RegEntityBundle {
    REG_BUNDLE(LintRegs);
    REG(ITEM, std::uint32_t, reg::at(0).array(reg::dims(2), reg::strides(4)));
    REG(COMMAND, std::uint32_t, reg::at(0x8));
};
struct RedirectLayout : reg::Bundle {
    REG_BUNDLE(RedirectLayout);
    REG_REPEAT_ENTITY(vf, LintRegs, 2);
    REG_LAYOUT([&] {
        return reg::address_space("IO").map_repeat(
            "VFS", 0, reg::repeat(2).stride(0x100), [&](auto i) {
                return reg::view("VF", 0x100).map(vf[i]).identity(9);
            });
    });
};
void lint_and_explain() {
    reg::RegisterMap map(0x400, "EXPLAIN");
    auto regs = reg::build<LintRegs>(
        reg::RegScope(map, 0, 0x400, "REGS", {}, {}, {}, 1, 0, 0, false, true));
    auto& item = regs.ITEM;
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
    const auto inner = reg::view("INNER", 8).map(item).identity(7);
    reg::apply_layout(map,
                      reg::address_space("IO")
                          .map(reg::view("OUTER", 8).map(inner), 0x200)
                          .map(reg::view("CMD", 4).map(regs.COMMAND), 0x300));
    map.set_identity_resolver([&](const auto& r) {
        ++calls;
        return r.request_id;
    });
    auto& command = regs.COMMAND;
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
    CHECK(map.explain_address(0x300).find("COMMAND") != std::string::npos);
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
    reg::RegisterBank<RedirectLayout> redirected(reg::space(0x200));
    auto& dynamic = redirected.map();
    dynamic.set_identity_resolver([&](const auto&) {
        ++calls;
        return 1u;
    });
    dynamic.finalize();
    CHECK(dynamic.explain_address(0).find("resolved_id=?") != std::string::npos);
    CHECK(dynamic.explain_address(0, 4, reg::RegTransactionKind::Read, 1).find("VF[1]") !=
          std::string::npos);
    CHECK(dynamic.explain_address(0, 4, reg::RegTransactionKind::Read, 2).find("out of range") !=
          std::string::npos);
    CHECK(calls == 1 && redirected->vf[0].ITEM.at(0).ref() == 0 &&
          redirected->vf[1].ITEM.at(0).ref() == 0);
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
    wide_hex_formatting();
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
    CHECK(log.find("WIDE128") != std::string::npos &&
          log.find("required first beat byte offset=0") != std::string::npos);
    CHECK(log.find("HIGH128") != std::string::npos &&
          log.find("required first beat byte offset=12") != std::string::npos);
    truncation_widths(logger, file);
    std::cout << "PASS: declaration facts, RO suppression, truncation/beat diagnostics, lint and "
                 "side-effect-free explanations\n";
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
