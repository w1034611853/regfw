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
struct Probe {
    std::vector<int>& events;
    int id;
    std::function<void()> destroy;
    Probe(std::vector<int>& e, int i, std::function<void()> d = {})
        : events(e), id(i), destroy(std::move(d)) {
    }
    ~Probe() {
        events.push_back(id);
        if (destroy)
            destroy();
    }
};
template <bool Named>
void exercise(unsigned width, bool forced_generic, bool throwing) {
    std::vector<int> events;
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
    if (forced_generic)
        m.set_pre_access([](const auto&) {});
    unsigned index_calls = 0, writes = 0, reads = 0;
    auto identity = std::make_shared<Probe>(events, 10);
    std::weak_ptr<Probe> identity_weak = identity;
    bind_id([&, identity](const auto& request) {
        clear_id();
        CHECK(!identity_weak.expired());
        auto temporary = std::make_shared<Probe>(events, 11);
        std::weak_ptr<Probe> weak = temporary;
        bind_id([temporary](const auto& r) {
            return r.request_id;
        });
        temporary.reset();
        bind_id([](const auto& r) {
            return r.request_id;
        });
        CHECK(weak.expired() && !identity_weak.expired());
        events.push_back(12);
        return request.request_id;
    });
    identity.reset();
    // Rebinding from a retiring callable's destructor must be safe too.
    auto index = std::make_shared<Probe>(events, 20, [&] {
        m.set_access_resolver(P, [](const auto& i) {
            return i.index_as(L);
        });
    });
    std::weak_ptr<Probe> index_weak = index;
    m.set_access_resolver(P, [&, index](const auto& i) -> std::uint32_t {
        ++index_calls;
        CHECK(identity_weak.expired());
        m.clear_access_resolver(P);
        CHECK(!index_weak.expired());
        auto temporary = std::make_shared<Probe>(events, 21);
        std::weak_ptr<Probe> weak = temporary;
        m.set_access_resolver(P, [temporary](const auto&) {
            return 0u;
        });
        temporary.reset();
        m.clear_access_resolver(P);
        CHECK(weak.expired() && !index_weak.expired());
        events.push_back(22);
        if (throwing)
            throw std::runtime_error("index self-clear failure");
        return i.index_as(L);
    });
    index.reset();
    b->vf[0].X.on_write([&] {
        CHECK(index_weak.expired());
        ++writes;
    });
    b->vf[0].X.on_read([&](std::uint32_t value, const auto&) {
        CHECK(index_weak.expired());
        ++reads;
        return value;
    });
    // Do not use a global hook as a proxy for the generic engine: hooks can
    // now use the scalar executor too.
    if (!forced_generic)
        b.finish();
    auto result = m.mmio_write_sized(0, 37, width);
    CHECK(result.result == (throwing ? reg::RegMemTxResult::DeviceError : reg::RegMemTxResult::Ok));
    CHECK(identity_weak.expired() && index_weak.expired() && index_calls == 1 &&
          writes == unsigned(!throwing));
    CHECK((events == std::vector<int>{11, 12, 10, 21, 22, 20}));
    CHECK(m.mmio_write_sized(0, 49, width).success() && writes == unsigned(!throwing) + 1);
    CHECK(m.mmio_read_sized(0, width) == 49 && reads == 1 && index_calls == 1);
    auto snapshot = b.save();
    b.reset();
    b.load(snapshot);
    CHECK(m.mmio_read_sized(0, width) == 49);
}
void identity_exception(bool legacy) {
    std::vector<int> events;
    reg::RegisterMap m(128, "EXCEPTION");
    auto a = m.create_reg<std::uint32_t>(0, "A"), b = m.create_reg<std::uint32_t>(64, "B");
    (void)b;
    reg::RegLayout layout(m);
    layout.identity_repeat_window({0, 2, 64, 64}, 1, reg::IdentityRemapMode::RedirectBank);
    auto p = std::make_shared<Probe>(events, 1);
    std::weak_ptr<Probe> weak = p;
    m.set_identity_resolver([&, p](const auto&) -> std::uint32_t {
        m.clear_identity_resolver();
        CHECK(!weak.expired());
        throw std::runtime_error("self-clear identity");
    });
    p.reset();
    m.finalize();
    if (legacy)
        CHECK(m.mmio_write(0, 10).result == reg::RegMemTxResult::DeviceError);
    else {
        bool failed = false;
        try {
            (void)m.resolve_address_with_identity(0, 4);
        } catch (const reg::Error&) {
            failed = true;
        }
        CHECK(failed);
    }
    CHECK(weak.expired() && events == std::vector<int>{1} && a.ref() == 0);
}
} // namespace
int main() {
    try {
        for (unsigned width : {1u, 2u, 4u})
            for (bool generic : {false, true})
                for (bool throwing : {false, true}) {
                    exercise<false>(width, generic, throwing);
                    exercise<true>(width, generic, throwing);
                }
        identity_exception(true);
        identity_exception(false);
        std::cout << "PASS: resolver self-rebind/clear, immediate unrelated retirement, destructor "
                     "rebind, exceptions, full/frame and legacy paths\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
