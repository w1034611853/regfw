#pragma once
#include "runtime.hpp"
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace reg {
enum class ExecutionMode { Synchronous, Asynchronous };
struct AsyncExecutorOptions {
    std::size_t capacity{65536};
    std::function<void()> thread_enter{}, thread_leave{};
};
struct AsyncStatistics {
    std::uint64_t accepted{}, completed{}, rejected{}, failures{}, high_water{};
};
// Bounded MPSC task queue, one consumer. No register access is replayed here.
class AsyncExecutor {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class detail::AsyncBackend;

  public:
    explicit AsyncExecutor(AsyncExecutorOptions options = {});
    ~AsyncExecutor();
    AsyncExecutor(const AsyncExecutor&) = delete;
    AsyncExecutor& operator=(const AsyncExecutor&) = delete;
    bool in_executor() const noexcept;
    bool post(std::function<void()> task);
    void invoke(std::function<void()> task);
    void fence();
    // Stop external producers first. Multiple callers may wait concurrently.
    // Waits for queued/executing tasks and admitted writes, including descendants.
    // Does not wait for external timer/CPU callbacks or keep the executor alive.
    void drain();
    AsyncStatistics statistics() const noexcept;
};
struct AsyncAccessOptions {
    // A write invoking one of these entities' on_write reserves ONE work slot
    // before behavior/state changes. Its callback must call defer() once.
    // Resolved entities include arrays/aliases/routing. Wide writes reserve
    // only on the commit beat. Ordinary accesses do not consume queue space.
    std::vector<const RegEntity*> deferred_writes;
};
class RegisterEndpointBase {
    std::unique_ptr<detail::AsyncBackend> backend_;
    void enter_state();
    void leave_state() noexcept;
    struct StateGuard {
        RegisterEndpointBase& endpoint;
        explicit StateGuard(RegisterEndpointBase& e) : endpoint(e) {
            endpoint.enter_state();
        }
        ~StateGuard() {
            endpoint.leave_state();
        }
    };

  protected:
    RegisterEndpointBase(RegisterMap&, AsyncExecutor*, AsyncAccessOptions);

  public:
    ~RegisterEndpointBase();
    RegisterEndpointBase(const RegisterEndpointBase&) = delete;
    RegisterEndpointBase& operator=(const RegisterEndpointBase&) = delete;
    RegMmioReadResult read(std::uint64_t address, unsigned size = 4) noexcept;
    RegMmioWriteResult write(std::uint64_t address, RegData value, unsigned size = 4) noexcept;
    // MMIO callbacks already own the lock. No nested scopes, DMA/QEMU or waits.
    // Do not let raw state references or pointers escape the scope.
    template <class F>
    decltype(auto) with_state(F&& task) {
        static_assert(!std::is_reference_v<std::invoke_result_t<F>>,
                      "state references cannot escape their lock scope");
        StateGuard guard(*this);
        return std::invoke(std::forward<F>(task));
    }
    static bool state_lock_held() noexcept;
    // Called exactly once by a declared on_write. Capture values, not borrowed
    // bank/info references. The work runs OUTSIDE the state lock.
    void defer(std::function<void()> work);
    // Explicit external task, without a state lock. False = not accepted.
    bool post(std::function<void()> work);
    void fence();
    // Stop external callers first. Waits for accepted work and its descendants.
    // No BQL/external locks while closing. Executor and map outlive the endpoint.
    void close();
};
class AsyncEndpoint : public RegisterEndpointBase {
  public:
    AsyncEndpoint(RegisterMap& map, AsyncExecutor& executor, AsyncAccessOptions options = {})
        : RegisterEndpointBase(map, &executor, std::move(options)) {
    }
};
template <ExecutionMode Mode>
class RegisterEndpoint;
template <>
class RegisterEndpoint<ExecutionMode::Synchronous> : public RegisterEndpointBase {
  public:
    explicit RegisterEndpoint(RegisterMap& map, AsyncAccessOptions options = {})
        : RegisterEndpointBase(map, nullptr, std::move(options)) {
    }
};
template <>
class RegisterEndpoint<ExecutionMode::Asynchronous> : public AsyncEndpoint {
  public:
    using AsyncEndpoint::AsyncEndpoint;
};
} // namespace reg
