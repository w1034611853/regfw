#include <regfw/regfw.hpp>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!(__VA_ARGS__))                                                                        \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " +      \
                                     #__VA_ARGS__);                                                \
    } while (false)
struct Probe : reg::Bundle {
    using Bundle::Bundle;
    template <class T, class Spec>
    auto add(std::string_view name, const Spec& spec) {
        return declare<T>(name, spec);
    }
};
template <class F>
void conflict(F fn) {
    bool caught = false;
    try {
        fn();
    } catch (const reg::Error& e) {
        caught = true;
        CHECK(e.kind == reg::ErrorKind::Configuration);
        CHECK(std::string(e.what()).find("overlap") != std::string::npos);
        CHECK(std::string(e.what()).find("init_validation.cpp") != std::string::npos);
    }
    CHECK(caught);
}
void randomized(bool storage) {
    reg::RegisterMap map(0x10000, "ROOT");
    Probe p(reg::RegScope(map, 0, 0x10000, "P", {}, {}, {}, 1, 0, 0, false, storage));
    std::vector<std::pair<unsigned, unsigned>> accepted;
    std::mt19937 rng(3000);
    for (unsigned n = 0; n < 1800; ++n) {
        unsigned at = (rng() % 4096) * 4, bytes = (1 + rng() % 32) * 4;
        bool overlap = false;
        for (auto [a, b] : accepted)
            overlap |= at < a + b && a < at + bytes;
        auto before = map.entity_count();
        auto add = [&] {
            p.add<std::uint32_t>("ITEM_" + std::to_string(n),
                                 reg::at(at).array(reg::dims(bytes / 4), reg::strides(4)));
        };
        if (overlap) {
            conflict(add);
            CHECK(map.entity_count() == before);
        } else {
            add();
            accepted.emplace_back(at, bytes);
            CHECK(map.entity_count() == before + 1);
        }
    }
    map.finalize();
    auto snapshot = map.dump_state_versioned();
    CHECK(snapshot.schema.slots.size() == accepted.size());
}
void holes_and_arrays(bool storage) {
    reg::RegisterMap map(0x1000, "ROOT");
    Probe p(reg::RegScope(map, 0, 0x1000, "P", {}, {}, {}, 1, 0, 0, false, storage));
    auto even = p.add<std::uint32_t>("EVEN", reg::at(0).array(reg::dims(8), reg::strides(8)));
    auto odd = p.add<std::uint32_t>("ODD", reg::at(4).array(reg::dims(8), reg::strides(8)));
    auto matrix =
        p.add<reg::u512>("MATRIX", reg::at(0x100).array(reg::dims(2, 2), reg::strides(64, 128)));
    auto before = map.entity_count();
    conflict([&] {
        p.add<reg::u512>("PARTIAL", reg::at(0x13c));
    });
    CHECK(map.entity_count() == before);
    conflict([&] {
        p.add<reg::u512>("SELF", reg::at(0x400).array(reg::dims(2, 2), reg::strides(32, 128)));
    });
    CHECK(map.entity_count() == before);
    conflict([&] {
        p.add<std::uint32_t>("DUPLICATE",
                             reg::at(0x800).array(reg::dims(2, 2), reg::strides(4, 4)));
    });
    CHECK(map.entity_count() == before);
    // Failed installation must not leave claims behind.
    auto reused = p.add<std::uint32_t>("REUSED", reg::at(0x400));
    reused.ref() = 19;
    even.at(7).ref() = 7;
    odd.at(7).ref() = 9;
    matrix.at(1, 1).ref() = reg::u512(42);
    map.finalize();
    auto saved = map.dump_state_versioned();
    map.reset();
    map.restore_state_versioned(saved);
    CHECK(even.at(7).ref() == 7 && odd.at(7).ref() == 9 && reused.ref() == 19 &&
          matrix.at(1, 1).ref() == reg::u512(42));
    if (!storage) {
        CHECK(map.mmio_read(56) == 7 && map.mmio_read(60) == 9 && map.mmio_read(0x400) == 19);
    }
}
int main() try {
    for (bool storage : {false, true}) {
        randomized(storage);
        holes_and_arrays(storage);
    }
    std::cout << "PASS: interval claims, arbitrary order, holes, wide/transposed overlaps, failed "
                 "install, snapshot\n";
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
