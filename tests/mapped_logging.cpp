#include <regfw/regfw.hpp>
#include <fstream>
#include <stdexcept>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(#x);                                                          \
    } while (false)
namespace {
REG_ACCESS_FIELD(Route, std::uint32_t);
REG_ACCESS_FIELD(L, std::uint32_t);
REG_ACCESS_FIELD(P, std::uint32_t);
REG_PRE_ACCESS(Check);
struct R : reg::RegEntityBundle {
    REG_BUNDLE(R);
    REG(TABLE, std::uint32_t, reg::at(0).array(reg::dims(4), reg::strides(4)));
    REG_STATIC_INDEX(Indices, TABLE, L, P, 0);
    REG(TRACE_ONLY, std::uint32_t, reg::at(32).log(reg::LogPolicy::Trace));
    REG(HIDDEN, std::uint32_t, reg::at(36).log(reg::LogPolicy::Hidden));
};
struct Root : reg::Bundle {
    REG_BUNDLE(Root);
    REG_REPEAT_ENTITY(vf, R, 2);
    REG_LAYOUT([](auto l) {
        return l.address_space("ROOT").map_repeat(
            "VF", 0x1000, reg::repeat(2).stride(0x100), [&](auto i) {
                return l.view("VF", 0x100)
                    .identity(7)
                    .using_resolver(Route)
                    .using_pre_access(Check)
                    .map(l.vf[i]);
            });
    });
};
std::string log_text() {
    hlog_flush();
    char path[2048];
    CHECK(hlog_file_path(path, sizeof(path)) < sizeof(path));
    std::ifstream f(path);
    return {std::istreambuf_iterator<char>(f), {}};
}
} // namespace
int main() {
    auto* logger = hlog_register_logger("mapped032", "regfw-mapped032", HLOG_LEVEL_INFO);
    CHECK(logger);
    reg::RegisterBank<Root> b(reg::space().name("MAP032"));
    auto& m = b.map();
    m.set_logger(logger);
    unsigned mode = 0, reads = 0, writes = 0, complete = 0;
    reg::RegAccessInfo saved;
    m.set_identity_resolver(Route, [](const auto& r) {
        return 1 - r.request_id;
    });
    m.set_access_resolver(P, [&](const auto& i) {
        CHECK(i.resolved_id() == 1 && i.index_as(L) == 0);
        b->vf[1].TABLE.at(2).ref() = 20;
        if (mode == 1)
            hlog_logger_set_level(logger, HLOG_LEVEL_TRACE);
        return 2u;
    });
    m.set_pre_access(Check, [&](const auto& i) {
        if (i.access_group.empty())
            return;
        CHECK(i.index.linear() == 2 && i.stored_before == 20 && i.stored_after == 20);
        b->vf[1].TABLE.at(2).ref() = 30;
    });
    b->vf[1].TABLE.on_write([&](std::uint32_t old, const auto& i) {
        ++writes;
        CHECK(old == 30 && i.stored_before == 30 && i.stored_after == 41 && i.index.linear() == 2);
        saved = i;
    });
    b->vf[1].TABLE.on_read([&](std::uint32_t candidate, const auto& i) {
        ++reads;
        CHECK(candidate == 30 && i.index.linear() == 2);
        saved = i;
        b->vf[1].TABLE.at(2).ref() = 90;
        if (mode == 2)
            hlog_logger_set_level(logger, HLOG_LEVEL_TRACE);
        return std::optional<std::uint32_t>{};
    });
    b->vf[1].TABLE.on_read_complete([&](std::uint32_t value, const auto& i) {
        ++complete;
        CHECK(value == 30 && i.stored_before == 30 && i.stored_after == 30);
    });
    b.finish();
    CHECK(m.mmio_write(0x1000, 41).success());
    CHECK(m.mmio_read(0x1000) == 30);
    CHECK(log_text().find("READ TABLE") == std::string::npos &&
          log_text().find("WRITE TABLE") == std::string::npos);
    mode = 1;
    CHECK(m.mmio_write(0x1000, 41).success());
    CHECK(log_text().find("WRITE TABLE[2]") != std::string::npos);
    hlog_logger_set_level(logger, HLOG_LEVEL_INFO);
    mode = 2;
    CHECK(m.mmio_read(0x1000) == 30);
    auto text = log_text();
    CHECK(text.find("READ TABLE[2]") != std::string::npos &&
          text.find("index=[0]->[2]") != std::string::npos);
    CHECK(saved.request_id() == 0 && saved.resolved_id() == 1 &&
          saved.request_index.linear() == 0 && saved.index.linear() == 2 &&
          saved.index_as(L) == 0 && saved.mapped_as(P) == 2);
    CHECK(reads == 2 && writes == 2 && complete == 2 && b->vf[1].TABLE.at(2).ref() == 90);
    hlog_logger_set_level(logger, HLOG_LEVEL_DEBUG);
    CHECK(m.mmio_write(0x1020, 1).success());
    CHECK(log_text().find("WRITE TRACE_ONLY") == std::string::npos);
    hlog_logger_set_level(logger, HLOG_LEVEL_TRACE);
    CHECK(m.mmio_write(0x1020, 2).success());
    CHECK(log_text().find("WRITE TRACE_ONLY") != std::string::npos);
    CHECK(m.mmio_write(0x1024, 3).success());
    CHECK(log_text().find("WRITE HIDDEN") == std::string::npos);
}
