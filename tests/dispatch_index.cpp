#include <regfw/regfw.hpp>
#include <array>
#include <iostream>
#include <stdexcept>
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!(__VA_ARGS__))                                                                        \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " +      \
                                     #__VA_ARGS__);                                                \
    } while (false)
namespace {
constexpr std::uint64_t BASE = 0x10000000000ULL, STEP = 1ULL << 38, HIGH = 0xfffffff000000000ULL;
struct Values : reg::RegEntityBundle {
    REG_BUNDLE(Values);
    REG(GRID, std::uint32_t, reg::at(0).array(reg::dims(128), reg::strides(4)));
};
struct Layout : reg::Bundle {
    REG_BUNDLE(Layout);
    REG_ENTITY(values, Values);
    REG_LAYOUT([&] {
        auto v = reg::view("PAGE", 4096).map(values).reserve(512, 4);
        auto root = reg::address_space("SPARSE");
        for (unsigned i = 0; i < 128; ++i)
            root = root.map(i % 7 == 0 ? v.read_only_ignore_writes() : v, BASE + i * STEP);
        return root.map(v, HIGH);
    });
};
} // namespace
int main() try {
    reg::RegisterBank<Layout> bank(reg::space().name("INDEX"));
    bank.finish();
    auto& m = bank.map();
    for (unsigned i = 0; i < 128; ++i) {
        const auto a = BASE + i * STEP + i * 4;
        const auto previous = bank->values.GRID.at(i).ref();
        auto w = m.mmio_write(a, 1000 + i);
        CHECK(w.result == (i % 7 == 0 ? reg::RegMemTxResult::Ignored : reg::RegMemTxResult::Ok));
        CHECK(m.mmio_read(HIGH + i * 4) == (i % 7 == 0 ? previous : 1000 + i));
        CHECK(m.try_mmio_read_sized(BASE + i * STEP + 512, 4).result ==
              reg::RegMemTxResult::DecodeError);
        CHECK(m.try_mmio_read_sized(BASE + i * STEP + 4096, 4).result ==
              reg::RegMemTxResult::DecodeError);
    }
    auto saved = bank.save();
    m.mmio_write(HIGH, 77);
    bank.load(saved);
    CHECK(m.mmio_read(HIGH) == 0);
    unsigned events = 0, pre = 0;
    bool correct = true;
    bank->values.GRID.on_write([&](const reg::RegAccessInfo& i) {
        ++events;
        correct = correct && i.address == BASE + STEP + 4 && i.index.linear() == 1 &&
                  i.stored_after == 55;
    });
    CHECK(m.mmio_write(BASE + STEP + 4, 55).success() && events == 1 && correct);
    m.set_pre_access([&](const auto&) {
        ++pre;
    });
    CHECK(m.mmio_read(BASE + STEP + 4) == 55 && pre == 1);
    m.clear_pre_access();
    bank->values.GRID.on_read([&](std::uint32_t v, const auto& i) {
        correct = correct && i.index.get() == 1;
        return v + 1;
    });
    CHECK(m.mmio_read(BASE + STEP + 4) == 56 && correct && pre == 1);
    bank->values.GRID.reset();
    CHECK(bank->values.GRID.at(1).ref() == 0);
    std::cout << "PASS: frozen page collisions/misses, sparse high aliases, permissions, late "
                 "callbacks/hooks and restore\n";
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
