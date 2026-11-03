#include "../src/internal/runtime_impl.hpp"
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
int main() try {
    reg::RegAccessInfo info;
    info.kind = reg::RegTransactionKind::Write;
    info.address = 0x1225020;
    info.alias_resolved_address = info.resolved_address = 0x325020;
    info.size = 4;
    info.register_width_bits = 32;
    info.raw_data = info.bus_data = info.stored_after = 0x30;
    info.bus_mask = 0xffffffff;
    const std::array<std::size_t, 2> dims{2, 2};
    info.index = info.request_index = reg::RegElementIndex::from_dims(dims, 2);
    info.route = {reg::AccessRouteKind::VfToPf, 1};
    info.source_instance = info.request_identity = info.resolved_identity = 1;
    info.identity_domain = 1;
    info.access_group = "VF_VCPU_TO_LCPU";
    reg::detail::LogRecord record{HLOG_LEVEL_TRACE,
                                  "Write",
                                  "poe.PF.SLOT[1].BUSINESS.POE_GRP_VCPU_MASK[1][0]",
                                  &info,
                                  0x30,
                                  {},
                                  {}};
    const auto unchanged = reg::detail::format_log_record(record);
    CHECK(unchanged.find('\n') == std::string::npos);
    CHECK(unchanged.find("WRITE POE_GRP_VCPU_MASK[1][0]: 0x00000000 -> 0x00000030") !=
          std::string::npos);
    CHECK(unchanged.find("addr: 0x01225020 -> 0x00325020") != std::string::npos);
    CHECK(unchanged.find("route=VF_TO_PF | vf=1") != std::string::npos);
    CHECK(unchanged.find("route=256") == std::string::npos &&
          unchanged.find("request_index=") == std::string::npos);
    CHECK(unchanged.find("index=") == std::string::npos &&
          unchanged.find("alias=") == std::string::npos);
    info.resolved_identity = 6;
    info.index = reg::RegElementIndex::from_dims(dims, 3);
    info.resolved_address = 0x825020;
    const auto changed = reg::detail::format_log_record(record);
    CHECK(changed.find('\n') == std::string::npos);
    CHECK(changed.find("alias=0x00325020 -> 0x00825020") != std::string::npos);
    CHECK(changed.find("vf=1->6 | index=[1][0]->[1][1]") != std::string::npos);
    record.phase = "Read";
    info.kind = reg::RegTransactionKind::Read;
    record.transaction_action = "snapshot-lane";
    const auto read = reg::detail::format_log_record(record);
    CHECK(read.find('\n') == std::string::npos && read.find("READ POE_GRP_VCPU_MASK[1][0]") == 0 &&
          read.find("txn=snapshot-lane") != std::string::npos);
    record.level = HLOG_LEVEL_DEBUG;
    CHECK(reg::detail::format_log_record(record).find('\n') == std::string::npos);
    record.level = HLOG_LEVEL_ERROR;
    record.phase = "on_write";
    record.error = "device failed";
    const auto error = reg::detail::format_log_record(record);
    CHECK(error.find("path=poe.PF.SLOT[1]") != std::string::npos &&
          error.find("error: device failed") != std::string::npos);
    auto* logger = hlog_register_logger("formatcheck", "regfw-log-format", HLOG_LEVEL_TRACE);
    CHECK(logger);
    reg::RegisterMap map(0x100, "FORMATCHECK", logger);
    auto r = map.create_reg<std::uint32_t>(0, "VALUE");
    map.finalize();
    CHECK(map.mmio_write(0, 42).success() && r.ref() == 42);
    hlog_flush();
    char path[2048];
    CHECK(hlog_file_path(path, sizeof(path)) < sizeof(path));
    std::ifstream file(path);
    const std::string text{std::istreambuf_iterator<char>(file), {}};
    CHECK(text.find("WRITE VALUE: 0x00000000 -> 0x0000002a") != std::string::npos);
    CHECK(text.find("WRITE VALUE: 0x00000000 -> 0x0000002a | addr: 0x00000000 | size=4B | "
                    "write=0x0000002a | mask=0xffffffff | route=DIRECT") != std::string::npos);
    CHECK(text.find("[void reg::RegisterMap::access_log") == std::string::npos);
    std::cout << "PASS: single-line Trace reads/writes, readable routes, coordinates, compact "
                 "unchanged mappings, address chains, one-line Debug, full error paths, hlog "
                 "without C++ signature prefix\n";
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
