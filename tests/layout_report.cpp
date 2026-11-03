#include "pf_vf_layout.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>

#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!(__VA_ARGS__))                                                                        \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " +      \
                                     #__VA_ARGS__);                                                \
    } while (false)
namespace {
std::size_t occurrences(std::string_view text, std::string_view needle) {
    std::size_t count = 0, at = 0;
    while ((at = text.find(needle, at)) != std::string_view::npos) {
        ++count;
        at += needle.size();
    }
    return count;
}
std::string_view section(const std::string& text, std::string_view begin, std::string_view end) {
    const auto start = text.find(begin);
    CHECK(start != std::string::npos);
    const auto stop = text.find(end, start + begin.size());
    CHECK(stop != std::string::npos);
    return std::string_view(text).substr(start, stop - start);
}
void pf_vf_report() {
    reg::RegisterBank<example::DeviceRegs> bank(reg::space(example::SPACE_SIZE).name("POE"));
    auto& map = bank.map();
    unsigned calls = 0;
    map.set_identity_resolver(example::VfRoute, "route_vf", [&](const auto& r) {
        ++calls;
        return r.request_id;
    });
    map.set_access_resolver(example::PhysicalPool, "route_pool", [&](const auto& r) {
        ++calls;
        return r.index_as(example::LogicalPool);
    });
    bank.finish();
    const auto before = map.dump_state_versioned();
    const auto summary = map.describe_layout();
    CHECK(summary.find("BUNDLE / ADDRESS LAYOUT") != std::string::npos);
    CHECK(summary.find("VISIBLE REGISTER MAP") == std::string::npos);
    CHECK(summary.find("QUEUE") == std::string::npos &&
          summary.find("TIMESTAMP") == std::string::npos);
    CHECK(summary.find("ENTITY;") == std::string::npos);
    CHECK(occurrences(summary, "VIEW → entity: POE.module;") == 2);
    CHECK(occurrences(summary, "VIEW → entity: POE.pf.CPU_SHARED;") == 2);
    CHECK(occurrences(summary, "VIEW → entity: POE.pf.GROUP_SHARED;") == 2);
    CHECK(summary.find("VIEW → entity: POE.vf[0]; priority=0; reset=VF") != std::string::npos);
    CHECK(summary.find("Ranges are mapping bounds") != std::string::npos);
    CHECK(summary.find("VF_WINDOW[1]") == std::string::npos);
    const auto report = map.describe_layout(reg::LayoutDetail::Full);
    CHECK(report.find("VISIBLE REGISTER MAP (compact arrays)") != std::string::npos);
    CHECK(report.find("dims=[2,4]") != std::string::npos);
    CHECK(report.find("strides=[0x20,0x4]") != std::string::npos);
    CHECK(report.find("QUEUE[1][2]") == std::string::npos);
    CHECK(report.find("VF_WINDOW[1]") == std::string::npos);
    CHECK(report.find("i=0..7") != std::string::npos);
    CHECK(report.find("W:ignore") != std::string::npos);
    CHECK(calls == 0);
    CHECK(map.dump_state_versioned().schema == before.schema);
    CHECK(map.dump_state_versioned().data == before.data);
    bank.finish();
    CHECK(map.describe_layout(reg::LayoutDetail::Full) == report);
}
struct Leaf : reg::Bundle {
    REG_BUNDLE(Leaf);
    REG(VALUE, std::uint32_t, reg::at(0));
};
struct Empty : reg::Bundle {
    REG_BUNDLE(Empty);
};
struct ReservedRepeat : reg::Bundle {
    REG_BUNDLE(ReservedRepeat);
    REG_LAYOUT(.reserved = {reg::reserved(0, 0x20).repeat(4, 0x100).name("HEAD")});
};
void reserved_repeats() {
    reg::RegisterBank<ReservedRepeat> bank(reg::space(0x400).name("RESERVED").finalize());
    const auto report = bank.map().describe_layout();
    CHECK(occurrences(report, "HEAD[i]") == 1);
    CHECK(report.find("HEAD[1]") == std::string::npos);
    CHECK(report.find("4 copies; i=0..3; stride=0x00000100") != std::string::npos);
    CHECK(bank.map().try_mmio_read_sized(0x300, 4).result == reg::RegMemTxResult::DecodeError);
}
struct Legacy : reg::Bundle {
    REG_BUNDLE(Legacy);
    REG_REPEAT_GROUP(groups, Leaf, 3, 0, 0x100, 0x100);
};
void repeat_entry_points() {
    reg::RegisterBank<Legacy> legacy(reg::space(0x300).name("LEGACY").finalize());
    CHECK(occurrences(section(legacy.map().describe_layout(reg::LayoutDetail::Full),
                              "BUNDLE / ADDRESS LAYOUT",
                              "STATIC ALIAS MAP"),
                      "│ REGISTER ") == 1);
    reg::RegisterMap map(0x1000, "REPEATS");
    reg::RegLayout layout(map);
    auto fixed = layout.repeat_aperture_at<Leaf, 3>(0, 0x100, 0x100, "fixed");
    auto dynamic = layout.repeat_aperture_at<Leaf>(0x400, 3, 0x100, 0x100, "dynamic");
    layout.finish();
    const auto report = map.describe_layout(reg::LayoutDetail::Full);
    CHECK(occurrences(section(report, "BUNDLE / ADDRESS LAYOUT", "STATIC ALIAS MAP"),
                      "│ REGISTER ") == 2);
    CHECK(report.find("fixed[i]") != std::string::npos &&
          report.find("dynamic[i]") != std::string::npos);
    CHECK(fixed[2].VALUE.ref() == 0 && dynamic[2].VALUE.ref() == 0);
}
void partial_aliases() {
    reg::RegisterMap map(0x1000, "PARTIAL");
    auto values = map.create_reg<std::uint32_t>(
        0x100, "VALUES", reg::at(0).array(reg::dims(16), reg::strides(4)));
    map.map_alias_window(0x400, 0x100, 0x40, "whole");
    map.map_alias_window(0x500, 0x110, 0x10, "middle");
    map.map_alias_window_with_policy(
        0x600, 0x400, 0x40, reg::AliasAccessPolicy::READ_ONLY_IGNORE_WRITES, "readonly");
    unsigned calls = 0;
    map.set_identity_resolver([&](const auto&) {
        ++calls;
        return 0u;
    });
    map.set_pre_access([&](const auto&) {
        ++calls;
    });
    values.on_read([&] {
        ++calls;
        return 1u;
    });
    map.finalize();
    const auto report = map.describe_layout(reg::LayoutDetail::Full);
    const auto regions = section(report, "CANONICAL ADDRESS REGIONS", "ACCESS PATHS");
    CHECK(regions.find("0x00000100..0x0000010f") != std::string_view::npos);
    CHECK(regions.find("0x00000110..0x0000011f") != std::string_view::npos);
    CHECK(regions.find("0x00000120..0x0000013f") != std::string_view::npos);
    CHECK(regions.find("4 views") != std::string_view::npos);
    CHECK(report.find("alias → alias; R, W:ignore") != std::string::npos);
    CHECK(calls == 0 && values.at(0).ref() == 0);
    CHECK(map.mmio_write(0x410, 7).success());
    CHECK(values.at(4).ref() == 7);
    CHECK(map.mmio_write(0x610, 9).result == reg::RegMemTxResult::Ignored);
    CHECK(values.at(4).ref() == 7);
}
void permuted_aliases() {
    reg::RegisterMap map(0x3000, "PERMUTED");
    reg::RegLayout layout(map);
    auto regs = layout.repeat_aperture_at<Leaf, 3>(0, 0x100, 0x100, "regs");
    auto views = layout.repeat_aperture_at<Empty, 3>(0x1000, 0x100, 0x100, "views");
    for (unsigned i = 0; i < 3; ++i)
        map.map_alias_window(views[i].region().base(),
                             regs[(i + 1) % 3].region().base(),
                             0x100,
                             "permuted[" + std::to_string(i) + "]");
    layout.finish();
    const auto report = map.describe_layout(reg::LayoutDetail::Full);
    const auto aliases = section(report, "STATIC ALIAS MAP", "CANONICAL ADDRESS REGIONS");
    CHECK(aliases.find("views[i]") == std::string_view::npos);
    CHECK(aliases.find("views[0] → regs[1]") != std::string_view::npos);
    CHECK(aliases.find("views[2] → regs[0]") != std::string_view::npos);
}
struct SortRegs : reg::RegEntityBundle {
    REG_BUNDLE(SortRegs);
    REG(Z_FIRST, std::uint32_t, reg::at(0));
    REG(A_LAST, std::uint32_t, reg::at(0x10));
    REG(M_ARRAY, std::uint32_t, reg::at(0x40).array(reg::dims(2, 2), reg::strides(0x20, 4)));
    REG(B_MIDDLE, std::uint32_t, reg::at(0x50));
};
struct SortCover : reg::RegEntityBundle {
    REG_BUNDLE(SortCover);
    REG(VALUE, std::uint64_t, reg::at(0));
};
struct SortLayout : reg::Bundle {
    REG_BUNDLE(SortLayout);
    REG_ENTITY(regs, SortRegs);
    REG_ENTITY(cover, SortCover);
    REG_LAYOUT([&] {
        const auto v = reg::view("V", 0x100).map(regs);
        return reg::address_space("SORT")
            .map(reg::view("A_HIGH", 0x100).map(v), 0xff71f0000000ULL)
            .map(reg::view("Z_LOW", 0x100).map(v), 0xc100000000ULL)
            .map(reg::view("OVERRIDE", 8).map(cover), 0xc100000040ULL, 10);
    });
};
void visible_rows_use_numeric_addresses() {
    reg::RegisterBank<SortLayout> b(reg::space().name("SORT").finalize());
    const auto full = b.map().describe_layout(reg::LayoutDetail::Full);
    std::istringstream lines(full.substr(full.find("VISIBLE REGISTER MAP")));
    std::vector<std::string> rows;
    std::string line;
    while (std::getline(lines, line))
        if (line.starts_with("│ SORT."))
            rows.push_back(line);
    CHECK(rows.size() == 9);
    for (std::size_t i = 0; i < 5; ++i)
        CHECK(rows[i].find(i == 2 ? "OVERRIDE" : "Z_LOW") != std::string::npos);
    for (std::size_t i = 5; i < 9; ++i)
        CHECK(rows[i].find("A_HIGH") != std::string::npos);
    for (const auto& [index, name] : std::vector<std::pair<unsigned, std::string>>{{0, "Z_FIRST"},
                                                                                   {1, "A_LAST"},
                                                                                   {3, "B_MIDDLE"},
                                                                                   {4, "M_ARRAY"},
                                                                                   {5, "Z_FIRST"},
                                                                                   {6, "A_LAST"},
                                                                                   {7, "M_ARRAY"},
                                                                                   {8, "B_MIDDLE"}})
        CHECK(rows[index].find(name) != std::string::npos);
    CHECK(rows[4].find("linear={2..3}") != std::string::npos);
    CHECK(rows[4].find("dims=[2,2]") != std::string::npos);
}

struct LargeRegs : reg::RegEntityBundle {
    REG_BUNDLE(LargeRegs);
    REG(MATRIX, std::uint32_t, reg::at(0x100).array(reg::dims(32, 64), reg::strides(0x200, 4)));
    REG(PACKETS, reg::u512, reg::at(0x8000).array(reg::dims(8, 16), reg::strides(0x400, 0x40)));
};
struct Large : reg::Bundle {
    REG_BUNDLE(Large);
    REG_REPEAT_ENTITY(vf, LargeRegs, 4);
    REG_LAYOUT([&] {
        auto v = [&](auto i) {
            return reg::view("VF", 0x10000).map(vf[i]);
        };
        return reg::address_space("LARGE")
            .map_repeat("A", 0xff71f0000000ULL, reg::repeat(4).stride(0x10000), v)
            .map_repeat("B", 0xc100000000ULL, reg::repeat(4).stride(0x10000), v);
    });
};
struct OwnedLeaf : reg::RegEntityBundle {
    REG_BUNDLE(OwnedLeaf);
    REG(VALUE, std::uint32_t, reg::at(0));
};
struct UnequalRepeat : reg::Bundle {
    REG_BUNDLE(UnequalRepeat);
    REG_REPEAT_ENTITY(vf, OwnedLeaf, 3);
    REG_ENTITY(shared, OwnedLeaf);
    REG_LAYOUT([&] {
        return reg::address_space("UNEQUAL")
            .map_repeat("VF",
                        0,
                        reg::repeat(3).stride(0x100),
                        [&](auto i) {
                            return reg::view("INSTANCE", 0x100).map(vf[i]);
                        })
            .map(reg::view("OVERRIDE", 4).map(shared), 0x100, 10);
    });
};
void unequal_repeat_visibility() {
    reg::RegisterBank<UnequalRepeat> b(reg::space(0x1000).finalize());
    const auto full = b.map().describe_layout(reg::LayoutDetail::Full);
    const auto regs = std::string_view(full).substr(full.find("VISIBLE REGISTER MAP"));
    CHECK(regs.find("INSTANCE[0]") != std::string_view::npos &&
          regs.find("INSTANCE[2]") != std::string_view::npos);
    CHECK(regs.find("INSTANCE[i]") == std::string_view::npos);
}

void large_arrays_and_log_levels() {
    auto* debug = hlog_register_logger("layout-debug-only", "layout-debug-only", HLOG_LEVEL_DEBUG);
    auto* trace = hlog_register_logger("layout-trace-full", "layout-trace-full", HLOG_LEVEL_TRACE);
    CHECK(debug && trace);
    reg::RegisterBank<Large> d(reg::space().name("LARGE").logger(debug));
    d.finish();
    reg::RegisterBank<Large> t(reg::space().name("LARGE").logger(trace));
    t.finish();
    const auto summary = d.map().describe_layout();
    const auto full = d.map().describe_layout(reg::LayoutDetail::Full);
    CHECK(summary.find("MATRIX") == std::string::npos &&
          summary.find("PACKETS") == std::string::npos);
    CHECK(summary.find("VF[1]") == std::string::npos);
    CHECK(occurrences(full, "dims=[32,64]") == 2);
    CHECK(occurrences(full, "dims=[8,16]") == 2);
    CHECK(full.find("strides=[0x200,0x4]") != std::string::npos);
    CHECK(full.find("elements=2048/2048") != std::string::npos);
    CHECK(full.find("visible words=2048") != std::string::npos);
    CHECK(full.find("x0*0x00000200 + x1*0x00000004 + i*0x00010000") != std::string::npos);
    CHECK(full.find("MATRIX[0][0]") == std::string::npos &&
          full.find("PACKETS[7][15]") == std::string::npos);
    CHECK(full.size() < 24000 && summary.size() < 12000);
    CHECK(summary.find("VIEW → entity: LARGE.vf[0]") != std::string::npos);
    hlog_flush();
    char path[2048];
    CHECK(hlog_file_path(path, sizeof(path)) < sizeof(path));
    std::ifstream file(path);
    std::string line;
    bool saw_debug = false, saw_trace = false;
    while (std::getline(file, line)) {
        if (line.find("layout-debug-only") == std::string::npos &&
            line.find("layout-trace-full") == std::string::npos)
            continue;
        if (line.find("MATRIX") != std::string::npos || line.find("PACKETS") != std::string::npos) {
            CHECK(line.find("layout-debug-only") == std::string::npos);
            CHECK(line.find("[trace]") != std::string::npos);
            saw_trace = true;
        }
        if (line.find("BUNDLE / ADDRESS LAYOUT") != std::string::npos &&
            line.find("[debug]") != std::string::npos)
            saw_debug = true;
    }
    CHECK(saw_debug && saw_trace);
    std::cout << "Large layout: summary=" << summary.size() << " bytes; full=" << full.size()
              << " bytes for 4 VFs, 2 entrances, 2048 scalar + 128 wide elements per VF\n";
}

} // namespace
int main() try {
    pf_vf_report();
    visible_rows_use_numeric_addresses();
    unequal_repeat_visibility();
    large_arrays_and_log_levels();
    reserved_repeats();
    repeat_entry_points();
    partial_aliases();
    permuted_aliases();
    std::cout << "PASS: compact arrays/repeats, nested bundles, shared multi-hop ranges, "
                 "partial/permuted aliases and side-effect-free reporting\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
