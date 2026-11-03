#include "layout_separate.hpp"
#include <stdexcept>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(#x);                                                          \
    } while (false)
int main() {
    reg::RegisterBank<separate::Root> a(reg::space().name("SPLIT"));
    reg::RegisterBank<separate::Dynamic> b(reg::space().name("SPLIT"));
    a.finish();
    b.finish();
    auto test = [](auto& bank) {
        auto& m = bank.map();
        CHECK(m.mmio_write(0xc100000000ULL, 42).success());
        CHECK(m.mmio_read(0xff71f0000000ULL) == 42);
        for (unsigned i = 0; i < 8; ++i)
            CHECK(m.mmio_write(0xc100000040ULL + i * 4, i + 10).success());
        auto s = bank.save();
        bank.reset();
        bank.load(s);
        for (unsigned i = 8; i < 16; ++i)
            CHECK(m.mmio_write(0xff71f0000040ULL + i * 4, i + 10).success());
        CHECK(bank->regs.WIDE.ref().word(0) == (std::uint64_t(11) << 32 | 10));
        return bank.save();
    };
    CHECK(test(a) == test(b));
    separate::other(a);
    CHECK(a.map().mmio_read(0xff71f0000008ULL) == 73);
}
