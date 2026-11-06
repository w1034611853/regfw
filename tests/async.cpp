#include <regfw/regfw.hpp>
#include <regfw/async.hpp>
#include "../examples/pf_vf_layout.hpp"
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <future>
#include <iostream>
#include <thread>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(std::string("async check: ") + #x);                           \
    } while (0)
namespace {
union Counter {
    std::uint64_t u64;
    struct {
        std::uint64_t cnt : 32, inverse : 32;
    } bits;
};
struct Registers : reg::RegEntityBundle {
    REG_BUNDLE(Registers);
    REG(VALUE, std::uint32_t, reg::at(0));
    REG(STATUS, std::uint32_t, reg::at(4).access(reg::ro));
    REG(EVENTS, std::uint32_t, reg::at(8).access(reg::w1c).reset(0xffff));
    REG(SPLIT, std::uint64_t, reg::at(16).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
    REG(WIDE, reg::u512, reg::at(0x40));
    REG(CMD, std::uint32_t, reg::at(0x90).access(reg::write_only));
    REG(RC, std::uint32_t, reg::at(0x94).access(reg::rc).reset(17));
    REG(FIFO, std::uint32_t, reg::at(0x98).access(reg::ro));
    REG(COUNT, Counter, reg::at(0xa0));
};
struct Layout : reg::Bundle {
    REG_BUNDLE(Layout);
    REG_ENTITY(regs, Registers);
    REG_LAYOUT([&] {
        auto v = reg::view("REGS", 0x100).map(regs);
        return reg::address_space("TEST").map(v, 0).map(v, 0x1000).map(v.read_only_ignore_writes(),
                                                                       0x2000);
    });
};
using Bank = reg::RegisterBank<Layout>;
void immediate_and_admission() {
    reg::AsyncExecutor ex({.capacity = 4});
    Bank b(reg::space(0x3000));
    reg::AsyncEndpoint* endpoint{};
    std::vector<unsigned> received;
    b->regs.CMD.on_write([&] {
        CHECK(reg::RegisterEndpointBase::state_lock_held());
        const unsigned value = b->regs.VALUE.ref();
        ++b->regs.STATUS.ref();
        endpoint->defer([&, value] {
            CHECK(!reg::RegisterEndpointBase::state_lock_held());
            received.push_back(value);
        });
    });
    b.finish();
    reg::AsyncEndpoint a(b.map(), ex, {{&b->regs.CMD.entity()}});
    endpoint = &a;
    bool duplicate = false;
    try {
        reg::AsyncEndpoint other(b.map(), ex);
    } catch (const reg::Error&) {
        duplicate = true;
    }
    CHECK(duplicate);
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    CHECK(ex.post([&] {
        entered.set_value();
        gate.wait();
    }));
    entered.get_future().get();
    bool valid = true;
    for (unsigned i = 1; i <= 4; ++i) {
        valid &= a.write(0, i).success() && a.read(0x1000).value == i;
        valid &= a.write(0x90, i).success();
    }
    // Queue full: rejected before CMD storage/callback/acceptance counter changes.
    valid &= !a.write(0x90, 99).success() && a.read(4).value == 4;
    valid &= a.write(0, 99).success() && a.read(0).value == 99;
    valid &= a.write(8, 5).success() && a.read(8).value == 0xfffa;
    valid &= a.read(0x94).value == 17 && a.read(0x94).value == 0;
    release.set_value();
    ex.drain();
    CHECK(valid);
    CHECK((received == std::vector<unsigned>{1, 2, 3, 4}));
    a.with_state([&] {
        CHECK(b->regs.CMD.ref() == 4);
    });
    CHECK(a.write(0x2000, 12).success() && a.read(0).value == 99);
    CHECK(a.read(0x400).result == reg::RegMemTxResult::DecodeError);
    CHECK(!a.write(1, 0, 2).success());
    for (unsigned i = 0; i < 200; ++i) {
        CHECK(a.write(0x90, i).success());
        ex.fence();
    }
    ex.drain();
    CHECK(received.size() == 204);
    CHECK(ex.statistics().accepted == ex.statistics().completed && ex.statistics().failures == 0);
}
void sync_selection() {
    Bank b(reg::space(0x3000));
    reg::RegisterEndpointBase* endpoint{};
    unsigned done = 0;
    b->regs.CMD.on_write([&] {
        unsigned value = b->regs.VALUE.ref();
        endpoint->defer([&, value] {
            CHECK(!reg::RegisterEndpointBase::state_lock_held());
            endpoint->with_state([&] {
                b->regs.STATUS.ref() = value;
                ++done;
            });
        });
    });
    b.finish();
    reg::RegisterEndpoint<reg::ExecutionMode::Synchronous> a(b.map(), {{&b->regs.CMD.entity()}});
    endpoint = &a;
    CHECK(a.write(0, 9).success());
    CHECK(a.write(0x90, 1).success());
    CHECK(done == 1 && a.read(4).value == 9);
}
void partial_wide_and_fifo() {
    reg::AsyncExecutor ex;
    Bank b(reg::space(0x3000));
    unsigned commits = 0;
    std::uint64_t response = 0;
    std::deque<unsigned> fifo{11, 22};
    b->regs.WIDE.on_write([&] {
        ++commits;
    });
    b->regs.SPLIT.on_read_complete([&](std::uint64_t v, const auto&) {
        response = v;
    });
    b->regs.FIFO.on_read([&] {
        unsigned v = fifo.empty() ? 0 : fifo.front();
        if (!fifo.empty())
            fifo.pop_front();
        return v;
    });
    b.finish();
    reg::AsyncEndpoint a(b.map(), ex);
    CHECK(a.read(0x98).value == 11 && a.read(0x98).value == 22 && a.read(0x98).value == 0);
    std::array<std::thread, 8> producers;
    std::atomic<bool> valid{true};
    for (unsigned t = 0; t < 8; ++t)
        producers[t] = std::thread([&, t] {
            // Each pair shares a word; other pairs use separate Bank instances below
            // in the real guest. Here use two writers and six concurrent readers.
            if (t < 2)
                for (unsigned i = 0; i < 2000; ++i) {
                    unsigned value = ((i + 1) * 17 + t) & 0xffff;
                    if (!a.write(t * 2, value, 2).success() || a.read(t * 2, 2).value != value)
                        valid = false;
                }
            else
                for (unsigned i = 0; i < 2000; ++i)
                    if (!a.read(0).success())
                        valid = false;
        });
    for (auto& t : producers)
        t.join();
    CHECK(valid.load());
    CHECK(a.write(1, 0xef, 1).success() && a.read(1, 1).value == 0xef);
    for (unsigned i = 0; i < 15; ++i)
        CHECK(a.write(0x40 + 4 * i, i + 1).success());
    CHECK(commits == 0);
    CHECK(a.write(0x7c, 16).success());
    CHECK(commits == 1);
    CHECK(a.read(0x40).value == 1);
    a.with_state([&] {
        for (unsigned i = 0; i < 8; ++i)
            b->regs.WIDE.ref().set_word(i, ~0ULL);
    });
    for (unsigned i = 1; i < 16; ++i)
        CHECK(a.read(0x40 + 4 * i).value == i + 1);
    CHECK(a.read(0x40).value == 0xffffffff);
    for (unsigned i = 1; i < 16; ++i)
        CHECK(a.read(0x40 + 4 * i).value == 0xffffffff);
    a.with_state([&] {
        b->regs.SPLIT.ref() = 0x1122334455667788ULL;
    });
    CHECK(a.read(16).value == 0x55667788);
    a.with_state([&] {
        b->regs.SPLIT.ref() = 0xaabbccdd00000000ULL;
    });
    CHECK(a.read(20).value == 0x11223344 && response == 0x1122334455667788ULL);
    CHECK(a.write(16, 0x123456789abcdef0ULL, 8).success());
    CHECK(a.read(16, 8).value == 0x123456789abcdef0ULL && response == 0x123456789abcdef0ULL);
    CHECK(ex.statistics().accepted == 0); // Polls and ordinary accesses never enqueue.
}
void direct_bitfields() {
    reg::AsyncExecutor ex;
    Bank b(reg::space(0x3000));
    b->regs.COUNT.u64 = 0xffffffff00000000ULL;
    auto increment = [&] {
        ++b->regs.COUNT.bits.cnt;
        std::this_thread::yield();
        b->regs.COUNT.bits.inverse = ~std::uint32_t(b->regs.COUNT.bits.cnt);
    };
    b->regs.CMD.on_write(increment);
    b->regs.COUNT.on_read(increment);
    b.finish();
    reg::AsyncEndpoint a(b.map(), ex);
    std::barrier start(9);
    std::atomic<bool> valid{true};
    std::array<std::thread, 8> producers;
    CHECK(a.post([&] {
        start.arrive_and_wait();
        for (unsigned i = 0; i < 1000; ++i)
            a.with_state(increment);
    }));
    for (auto& t : producers)
        t = std::thread([&] {
            start.arrive_and_wait();
            unsigned previous = 0;
            for (unsigned i = 0; i < 1000; ++i) {
                if (!a.write(0x90, 1).success())
                    valid = false;
                auto r = a.read(0xa0, 8);
                auto n = std::uint32_t(r.value);
                if (!r.success() || std::uint32_t(r.value >> 32) != ~n || n < previous)
                    valid = false;
                previous = n;
            }
        });
    for (auto& t : producers)
        t.join();
    ex.drain();
    CHECK(valid.load());
    a.with_state([&] {
        CHECK(b->regs.COUNT.bits.cnt == 17000);
        CHECK(b->regs.COUNT.bits.inverse == ~17000U);
    });
    // Deterministic guest write before a worker increment, with wait OUTSIDE lock.
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    CHECK(a.post([&] {
        entered.set_value();
        gate.wait();
        a.with_state(increment);
    }));
    entered.get_future().get();
    CHECK(a.write(0xa0, (std::uint64_t(~100U) << 32) | 100, 8).success());
    release.set_value();
    ex.drain();
    a.with_state([&] {
        CHECK(b->regs.COUNT.bits.cnt == 101);
    });
    bool nested = false, wait = false;
    a.with_state([&] {
        try {
            a.with_state([] {});
        } catch (const reg::Error&) {
            nested = true;
        }
        try {
            ex.fence();
        } catch (const reg::Error&) {
            wait = true;
        }
    });
    CHECK(nested && wait);
    ex.invoke([&] {
        bool rejected = false;
        try {
            ex.fence();
        } catch (const reg::Error&) {
            rejected = true;
        }
        CHECK(rejected);
    });
}
void chain() {
    reg::AsyncExecutor ex;
    std::array<std::unique_ptr<Bank>, 4> banks;
    std::array<std::unique_ptr<reg::AsyncEndpoint>, 4> fronts;
    constexpr unsigned path[] = {0, 3, 2, 3, 1, 2, 0};
    unsigned completed = 0, depth = 0, max_depth = 0;
    for (unsigned d = 0; d < 4; ++d) {
        banks[d] = std::make_unique<Bank>(reg::space(0x3000));
        auto& b = *banks[d];
        b->regs.CMD.on_write([&, d] {
            auto step = banks[d]->operator->()->regs.CMD.ref();
            CHECK(path[step] == d);
            fronts[d]->defer([&, d, step] {
                CHECK(!reg::RegisterEndpointBase::state_lock_held());
                ++depth;
                max_depth = std::max(depth, max_depth);
                CHECK(fronts[d]->write(0, step).success());
                CHECK(fronts[d]->read(0).value == step); // self access
                if (step == 6)
                    ++completed;
                else
                    CHECK(fronts[path[step + 1]]->write(0x90, step + 1).success());
                --depth;
            });
        });
        b.finish();
        fronts[d] = std::make_unique<reg::AsyncEndpoint>(
            b.map(), ex, reg::AsyncAccessOptions{{&b->regs.CMD.entity()}});
    }
    for (unsigned i = 0; i < 1000; ++i)
        CHECK(fronts[0]->write(0x90, 0).success());
    ex.drain();
    CHECK(completed == 1000 && max_depth == 1 && ex.statistics().failures == 0);
    for (auto& f : fronts)
        f->close();
}
void routing() {
    reg::AsyncExecutor ex;
    reg::RegisterBank<example::DeviceRegs> b(reg::space(example::SPACE_SIZE));
    reg::AsyncEndpoint* endpoint{};
    reg::AsyncAccessOptions options;
    std::vector<std::pair<unsigned, unsigned>> commands;
    for (unsigned i = 0; i < example::VF_COUNT; ++i) {
        b->vf[i].CONTROL.on_write([&, i] {
            const auto value = b->vf[i].CONTROL.ref();
            endpoint->defer([&, i, value] {
                commands.emplace_back(i, value);
            });
        });
        options.deferred_writes.push_back(&b->vf[i].CONTROL.entity());
    }
    std::array<unsigned, example::VF_COUNT> routes{};
    for (unsigned i = 0; i < routes.size(); ++i)
        routes[i] = i;
    unsigned resolves = 0;
    b.map().set_identity_resolver(example::VfRoute, [&](const reg::IdentityResolveRequest& r) {
        ++resolves;
        return routes[r.request_id];
    });
    b.map().set_access_resolver(example::PhysicalPool, [](const reg::RegAccessInfo& i) {
        return (i.index_as(example::LogicalPool) + 1) % 4;
    });
    b.finish();
    reg::AsyncEndpoint a(b.map(), ex, options);
    endpoint = &a;
    a.with_state([&] {
        routes[2] = 6;
    });
    auto address = example::vf_window(2) + example::HEAD_SIZE;
    CHECK(a.write(address, 0x66).success());
    CHECK(resolves == 1);
    CHECK(a.read(address).value == 0x66);
    CHECK(resolves == 2);
    CHECK(a.write(address + 0x128, 0x55).success());
    CHECK(a.read(address + 0x128).value == 0x55);
    CHECK(a.read(example::pf_slot(2) + example::HEAD_SIZE).value == 0x66);
    a.with_state([&] {
        routes[2] = 5;
    });
    CHECK(a.write(address, 0x77).success() && a.read(address).value == 0x77);
    ex.drain();
    CHECK((commands == std::vector<std::pair<unsigned, unsigned>>{{6, 0x66}, {5, 0x77}}));
}
void wide_admission() {
    reg::AsyncExecutor ex({.capacity = 1});
    Bank b(reg::space(0x3000));
    reg::AsyncEndpoint* endpoint{};
    unsigned done = 0;
    b->regs.WIDE.on_write([&] {
        endpoint->defer([&] {
            ++done;
        });
    });
    b.finish();
    reg::AsyncEndpoint a(b.map(), ex, {{&b->regs.WIDE.entity()}});
    endpoint = &a;
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    CHECK(ex.post([&] {
        entered.set_value();
        gate.wait();
    }));
    entered.get_future().get();
    CHECK(ex.post([] {}));
    bool valid = true;
    for (unsigned i = 0; i < 15; ++i)
        valid &= a.write(0x40 + 4 * i, i + 1).success();
    valid &= !a.write(0x7c, 16).success();
    a.with_state([&] {
        valid &= b->regs.WIDE.ref().low_u64() == 0;
    });
    release.set_value();
    ex.drain();
    CHECK(valid);
    CHECK(a.write(0x7c, 16).success());
    ex.drain();
    CHECK(done == 1 && a.read(0x40).value == 1);
}
void close_and_contracts() {
    reg::AsyncExecutor ex;
    Bank b(reg::space(0x3000));
    b.finish();
    reg::AsyncEndpoint a(b.map(), ex);
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    CHECK(a.post([&] {
        entered.set_value();
        gate.wait();
        CHECK(a.post([&] {
            a.with_state([&] {
                b->regs.STATUS.ref() = 42;
            });
        }));
    }));
    entered.get_future().get();
    auto closer = std::async(std::launch::async, [&] {
        a.close();
    });
    while (a.read(0).success())
        std::this_thread::yield();
    release.set_value();
    closer.get();
    CHECK(b->regs.STATUS.ref() == 42 && !a.read(0).success());
    // Detachment leaves the original Map usable and a new endpoint attachable.
    reg::RegisterEndpoint<reg::ExecutionMode::Synchronous> fresh(b.map());
    CHECK(fresh.write(0, 7).success() && fresh.read(0).value == 7);

    Bank bad(reg::space(0x3000));
    reg::AsyncEndpoint* p{};
    bad->regs.CMD.on_write([&] {
        p->defer([] {});
    });
    bad.finish();
    reg::AsyncEndpoint undeclared(bad.map(), ex);
    p = &undeclared;
    CHECK(!undeclared.write(0x90, 1).success());
    CHECK(undeclared.write(0, 9).success() && undeclared.read(0).value == 9);
}
// Bound deadlock regressions even when std::future destructors would wait for
// the blocked tasks. An old implementation must fail, not hang the test runner.
template <class F>
void await_condition(F ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::cerr << "async drain regression timed out\n";
            std::_Exit(EXIT_FAILURE);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void concurrent_drain() {
    for (unsigned callers : {2u, 8u, 80u}) {
        reg::AsyncExecutor ex({.capacity = 1});
        std::promise<void> entered, release;
        auto gate = release.get_future().share();
        CHECK(ex.post([&] {
            entered.set_value();
            gate.wait();
        }));
        entered.get_future().get();
        std::atomic<unsigned> descendants{};
        std::function<void()> next = [&] {
            if (++descendants < 16)
                CHECK(ex.post(next));
        };
        CHECK(ex.post(next)); // Fill the data queue while the worker is blocked.
        std::vector<std::future<void>> waiters;
        for (unsigned i = 0; i < callers; ++i)
            waiters.push_back(std::async(std::launch::async, [&] {
                ex.drain();
                CHECK(descendants == 16);
            }));
        // All callers are inside drain before allowing either task to finish.
        await_condition([&] {
            return ex.statistics().accepted == 2 + callers;
        });
        for (auto& waiter : waiters)
            CHECK(waiter.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
        release.set_value();
        for (auto& waiter : waiters) {
            await_condition([&] {
                return waiter.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            });
            waiter.get();
        }
        auto stats = ex.statistics();
        CHECK(stats.accepted == stats.completed && !stats.rejected && !stats.failures);
        CHECK(stats.high_water == 1); // Waiters must not consume control/data slots.
        ex.drain();                   // An already idle executor returns as well.
    }
}
void drain_executing_and_failure() {
    reg::AsyncExecutor ex;
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    std::atomic<bool> self_rejected{};
    CHECK(ex.post([&] {
        try {
            ex.drain();
        } catch (const reg::Error&) {
            self_rejected = true;
        }
        entered.set_value();
        gate.wait();
        throw std::runtime_error("intentional background failure");
    }));
    entered.get_future().get();
    auto waiter = std::async(std::launch::async, [&] {
        ex.drain();
    });
    await_condition([&] {
        return ex.statistics().accepted == 2;
    });
    CHECK(waiter.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    release.set_value();
    await_condition([&] {
        return waiter.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    });
    waiter.get();
    CHECK(self_rejected && ex.statistics().failures == 1);
    CHECK(ex.statistics().accepted == ex.statistics().completed);
}
void drain_admitted_write() {
    for (bool commit : {false, true}) {
        reg::AsyncExecutor ex;
        Bank b(reg::space(0x3000));
        reg::AsyncEndpoint* endpoint{};
        std::promise<void> entered, release;
        auto gate = release.get_future().share();
        std::atomic<unsigned> completed{};
        b->regs.CMD.on_write([&] {
            entered.set_value();
            gate.wait(); // Hold a reservation before enqueue.
            if (commit)
                endpoint->defer([&] {
                    ++completed;
                });
            // Missing defer intentionally exercises cancellation/error wakeup.
        });
        b.finish();
        reg::AsyncEndpoint a(b.map(), ex, {{&b->regs.CMD.entity()}});
        endpoint = &a;
        auto writer = std::async(std::launch::async, [&] {
            return a.write(0x90, 1);
        });
        entered.get_future().get();
        auto waiter = std::async(std::launch::async, [&] {
            ex.drain();
        });
        await_condition([&] {
            return ex.statistics().accepted == 1;
        });
        CHECK(waiter.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
        release.set_value();
        await_condition([&] {
            return waiter.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        });
        waiter.get();
        CHECK(writer.get().success() == commit);
        CHECK(completed == unsigned(commit));
        CHECK(ex.statistics().accepted == ex.statistics().completed && !ex.statistics().failures);
    }
}
} // namespace
int main() try {
    concurrent_drain();
    drain_executing_and_failure();
    drain_admitted_write();
    immediate_and_admission();
    sync_selection();
    partial_wide_and_fifo();
    direct_bitfields();
    chain();
    routing();
    wide_admission();
    close_and_contracts();
    std::cout << "PASS: concurrent drain/descendants/reservations/failure, immediate RW, admission "
                 "before mutation, captured commands, sync/async, RC/FIFO/W1C, halfwords, wide "
                 "transactions, protected bitfields, ADCDBCA, live routing\n";
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
