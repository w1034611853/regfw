#include "bundle_light.hpp"
#include <stdexcept>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(#x);                                                          \
    } while (false)
int main() {
    using namespace reg;
    RegisterBank<light::Device> bank(space().name("DEV"));
    bank.map().set_identity_resolver(light::Route, [](const auto& r) {
        return r.request_id;
    });
    std::uint32_t observed = 0;
    light::arm_callbacks(bank, observed);
    bank.finish();
    constexpr std::uint64_t pf = 0x1000, win = 0x200000 + 0x100;
    std::uint32_t value = 0;
    light::poke(bank, pf, 0x11223344u);
    CHECK(light::read(bank, pf, value) && (value ^ 0x33u) == 0x11223344u);
    light::poke(bank, win + 0x2000, 7u);
    CHECK(light::read(bank, win + 0x2000, value) && value == 7u);
    CHECK(observed == 7u);
    auto snapshot = bank.save();
    bank.load(snapshot);
    CHECK(light::read(bank, win + 0x2000, value) && value == 7u);
    CHECK(light::read(bank, win + 2 * 0x2000 + 8, value) && value == 0u); // untouched instance data
    return 0;
}
