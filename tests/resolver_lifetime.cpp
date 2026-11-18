#include <regfw/regfw.hpp>
#include <iostream>
#include <stdexcept>
#include <vector>
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!(__VA_ARGS__))                                                                        \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " +      \
                                     #__VA_ARGS__);                                                \
    } while (false)
namespace {
REG_ACCESS_FIELD(Route, std::uint32_t);
REG_ACCESS_FIELD(L, std::uint32_t);
REG_ACCESS_FIELD(P, std::uint32_t);
struct Words : reg::RegEntityBundle {
    REG_BUNDLE(Words);
    REG(X, std::uint32_t, reg::at(0).array(reg::dims(2), reg::strides(4)));
    REG_STATIC_INDEX(Index, X, L, P, 0);
};
template <bool Named>
struct Device : reg::Bundle {
    REG_BUNDLE(Device);
    REG_REPEAT_ENTITY(vf, Words, 2);
    REG_LAYOUT([](auto layout) {
        return layout.address_space("LIFE").map_repeat(
            "VF", 0, reg::repeat(2).stride(64), [&](auto i) {
                auto v = layout.view("V", 64).identity(1).map(layout.vf[i]);
                if constexpr (Named)
                    return v.using_resolver(Route);
                else
                    return v;
            });
    });
};
REG_PRE_ACCESS(UnusedHook);
struct Probe {
    unsigned& destroyed;
    ~Probe() {
        ++destroyed;
    }
};
template <class F>
void frozen(F action) {
    bool rejected = false;
    try {
        action();
    } catch (const reg::Error& e) {
        rejected = e.kind == reg::ErrorKind::Configuration;
    }
    CHECK(rejected);
}
template <bool Named>
void exercise(unsigned width, bool hook, bool throwing) {
    unsigned destroyed = 0, index_calls = 0, writes = 0, reads = 0, hooks = 0;
    bool fail = throwing, redirect = false;
    reg::RegisterBank<Device<Named>> b(reg::space(128));
    auto& m = b.map();
    auto bind_id = [&](auto f) {
        if constexpr (Named)
            m.set_identity_resolver(Route, std::move(f));
        else
            m.set_identity_resolver(std::move(f));
    };
    auto clear_id = [&] {
        if constexpr (Named)
            m.clear_identity_resolver(Route);
        else
            m.clear_identity_resolver();
    };
    // Configuration can replace a function before finish; no invocation retains it.
    auto retired = std::make_shared<Probe>(destroyed);
    bind_id([retired](const auto& r) {
        return r.request_id;
    });
    retired.reset();
    auto identity = std::make_shared<Probe>(destroyed);
    std::weak_ptr<Probe> identity_weak = identity;
    bind_id([&, identity](const auto& r) {
        CHECK(!identity_weak.expired());
        return r.request_id ^ unsigned(redirect);
    });
    identity.reset();
    CHECK(destroyed == 1);
    auto index = std::make_shared<Probe>(destroyed);
    std::weak_ptr<Probe> index_weak = index;
    m.set_access_resolver(P, [&, index](const auto& i) {
        ++index_calls;
        CHECK(!index_weak.expired());
        if (fail)
            throw std::runtime_error("index failure");
        return i.index_as(L);
    });
    index.reset();
    if (hook)
        m.set_pre_access([&](const auto&) {
            ++hooks;
        });
    m.set_pre_access(UnusedHook, [](const auto&) {});
    for (unsigned i = 0; i < 2; ++i) {
        b->vf[i].X.on_write([&] {
            ++writes;
        });
        b->vf[i].X.on_read([&](std::uint32_t value, const auto&) {
            ++reads;
            return value;
        });
    }
    b.finish();
    b.finish(); // Idempotent; it does not reopen registration.
    frozen(clear_id);
    frozen([&] {
        bind_id([](const auto& r) {
            return r.request_id;
        });
    });
    frozen([&] {
        m.clear_access_resolver(P);
    });
    frozen([&] {
        m.set_access_resolver(P, [](const auto&) {
            return 0u;
        });
    });
    frozen([&] {
        m.clear_pre_access();
    });
    frozen([&] {
        m.set_pre_access([](const auto&) {});
    });
    frozen([&] {
        m.clear_pre_access(UnusedHook);
    });
    frozen([&] {
        m.set_pre_access(UnusedHook, [](const auto&) {});
    });
    frozen([&] {
        b->vf[0].X.on_read_complete([] {});
    });
    auto first = m.mmio_write_sized(0, 37, width);
    CHECK(first.result == (throwing ? reg::RegMemTxResult::DeviceError : reg::RegMemTxResult::Ok));
    CHECK(writes == unsigned(!throwing) && destroyed == 1);
    fail = false;
    CHECK(m.mmio_write_sized(0, 49, width).success());
    CHECK(m.mmio_read_sized(0, width) == 49 && reads == 1 && index_calls == 3);
    redirect = true; // Frozen function bindings do not freeze resolver results.
    CHECK(m.mmio_write_sized(0, 61, width).success());
    CHECK(b->vf[1].X.at(0).ref() == 61 && b->vf[0].X.at(0).ref() == 49);
    CHECK(hooks == (hook ? 4u - unsigned(throwing) : 0u));
    auto saved = b.save();
    b.reset();
    b.load(saved);
    CHECK(m.mmio_read_sized(0, width) == 61);
    CHECK(!identity_weak.expired() && !index_weak.expired() && destroyed == 1);
    b.clear();
    CHECK(identity_weak.expired() && index_weak.expired() && destroyed == 3);
}
void registration_from_callback_is_rejected() {
    reg::RegisterBank<Device<true>> b(reg::space(128));
    auto& m = b.map();
    unsigned rejected = 0;
    m.set_identity_resolver(Route, [&](const auto& request) {
        frozen([&] {
            m.clear_identity_resolver(Route);
        });
        ++rejected;
        return request.request_id;
    });
    m.set_access_resolver(P, [&](const auto& info) {
        frozen([&] {
            m.clear_access_resolver(P);
        });
        ++rejected;
        return info.index_as(L);
    });
    b.finish();
    CHECK(m.mmio_write(0, 77).success() && rejected == 2);
    CHECK(m.mmio_read(0) == 77 && rejected == 4);
}
} // namespace
int main() try {
    for (unsigned width : {1u, 2u, 4u})
        for (bool hook : {false, true})
            for (bool throwing : {false, true}) {
                exercise<false>(width, hook, throwing);
                exercise<true>(width, hook, throwing);
            }
    registration_from_callback_is_rejected();
    std::cout << "PASS: fixed resolver ownership, pre-finish replacement, frozen bindings, "
                 "mutable results, exceptions, full/frame paths and snapshot restore\n";
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
