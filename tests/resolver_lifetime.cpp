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
// A resolver may change storage. pre_access sees the request's saved value if
// the index is unchanged, or the newly selected value if the index changes.
void index_observation(bool finished, bool light, unsigned width) {
    reg::RegisterBank<Device<true>> bank(reg::space(128));
    auto& map = bank.map();
    auto& words = bank->vf[1].X;
    map.set_identity_resolver(Route, [](const auto&) {
        return 1u;
    });
    unsigned selected = 1;
    unsigned calls = 0;
    reg::RegAccessInfo resolver, pre, write;
    auto convert = [&](std::uint32_t vf, reg::RegData logical) {
        ++calls;
        CHECK(vf == 1 && logical == 1);
        words.at(1).ref() = 33;
        words.at(0).ref() = 44;
        return selected;
    };
    if (light)
        map.set_access_resolver(P, convert);
    else
        map.set_access_resolver(P, [&](const reg::RegAccessInfo& i) {
            resolver = i;
            return convert(i.resolved_id(), i.index_as(L));
        });
    map.set_pre_access([&](const reg::RegAccessInfo& i) {
        pre = i;
        words.at(selected).ref() = 55;
    });
    words.on_write([&](const reg::RegAccessInfo& i) {
        write = i;
    });
    if (finished)
        bank.finish();
    for (const auto result : {1u, 0u}) {
        selected = result;
        words.at(1).ref() = 22;
        CHECK(map.mmio_write_sized(4, 0xab, width).success());
        if (!light) {
            CHECK(resolver.stored_before == 22 && resolver.index.linear() == 1);
            CHECK(resolver.index_as(L) == 1 && resolver.request_id() == 0 &&
                  resolver.resolved_id() == 1);
        }
        CHECK(pre.stored_before == (selected == 1 ? 22u : 44u));
        CHECK(pre.index.linear() == selected && pre.request_index.linear() == 1 &&
              pre.index_as(L) == 1 && pre.mapped_as(P) == selected);
        CHECK(write.stored_before == 55 && write.stored_after == 0xab &&
              words.at(selected).ref() == 0xab);
    }
    CHECK(calls == 2);
}

// The second coordinate is narrowed to uint8_t before the named mapping sees
// it. An un-routed register supplies its source instance, or zero when absent.
REG_ACCESS_FIELD(Column, std::uint8_t);
void light_without_identity(std::optional<std::uint32_t> instance) {
    reg::RegisterMap map(4096, "NO_IDENTITY");
    auto words = map.create_reg<std::uint32_t>(
        0, "WORDS", reg::at(0).array(reg::dims(2, 260), reg::strides(1040, 4)), instance);
    reg::Groups groups;
    groups.group("TABLE")
        .members(words)
        .coordinate(L, reg::index<0>)
        .coordinate(Column, reg::index<1>)
        .derive(P)
        .from(Column)
        .using_resolver(P);
    groups.install();
    unsigned calls = 0;
    int output = 7;
    map.set_access_resolver(P, [&](std::uint32_t vf, reg::RegData logical) {
        ++calls;
        CHECK(vf == instance.value_or(0) && logical == 2);
        if (output == -2)
            throw std::runtime_error("device error");
        return output;
    });
    map.finalize();
    const auto address = 1040u + 258u * 4u;
    CHECK(map.mmio_write(address, 77).success() && words.at(1, 7).ref() == 77);
    CHECK(map.mmio_read_sized(address, 2) == 77);
    for (auto invalid : {260, -1, -2}) {
        output = invalid;
        const auto result = map.mmio_write(address, 91);
        CHECK(result.result == (invalid == -2 ? reg::RegMemTxResult::DeviceError
                                              : reg::RegMemTxResult::AccessError));
    }
    CHECK(calls == 5 && words.at(1, 7).ref() == 77 && words.at(1, 258).ref() == 0);
}

struct BothResolverForms {
    unsigned& rich;
    unsigned& light;
    std::uint32_t operator()(const reg::RegAccessInfo& i) {
        ++rich;
        return i.index_as(L);
    }
    std::uint32_t operator()(std::uint32_t, reg::RegData) {
        ++light;
        return 0;
    }
};
void prefer_existing_signature() {
    reg::RegisterBank<Device<true>> bank(reg::space(128));
    bank.map().set_identity_resolver(Route, [](const auto& r) {
        return r.request_id;
    });
    unsigned rich = 0, light = 0;
    bank.map().set_access_resolver(P, BothResolverForms{rich, light});
    bank.finish();
    CHECK(bank.map().mmio_write(4, 17).success());
    CHECK(bank->vf[0].X.at(1).ref() == 17 && rich == 1 && light == 0);
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
    for (bool finished : {false, true}) {
        for (bool light : {false, true})
            for (unsigned width : {1u, 2u, 4u})
                index_observation(finished, light, width);
    }
    prefer_existing_signature();
    registration_from_callback_is_rejected();
    light_without_identity({});
    light_without_identity(7u);
    std::cout << "PASS: fixed resolver ownership, pre-finish replacement, frozen bindings, "
                 "mutable results, exceptions, full/frame paths and snapshot restore\n";
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
