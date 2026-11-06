#include <regfw/async.hpp>
#include "internal/runtime_impl.hpp"
#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace reg {
namespace {
thread_local const void* current_executor{};
thread_local const void* current_state{};
thread_local const void* current_task_owner{};
std::mutex attachment_mutex;
void require_unlocked() {
    if (current_state)
        throw Error("operation cannot hold a register state lock", ErrorKind::Access);
}
struct QueuedEvent {
    std::function<void()> task{};
    std::shared_ptr<std::promise<void>> completion{};
};
} // namespace
struct AsyncExecutor::Impl {
    AsyncExecutorOptions options;
    std::mutex mutex;
    std::condition_variable wake, idle;
    std::vector<QueuedEvent> ring;
    std::size_t head{}, tail{}, count{}, reserved{};
    bool stopping{}, executing{};
    std::atomic<std::uint64_t> accepted{}, completed{}, rejected{}, failures{}, high_water{};
    std::thread worker;
    explicit Impl(AsyncExecutorOptions value) : options(std::move(value)) {
        if (!options.capacity || options.capacity > SIZE_MAX / sizeof(QueuedEvent) - 64)
            throw Error("invalid async queue capacity", ErrorKind::Configuration);
        ring.resize(options.capacity + 64);
    }
    bool reserve() {
        std::lock_guard lock(mutex);
        if (stopping || count + reserved >= options.capacity) {
            ++rejected;
            return false;
        }
        ++reserved;
        return true;
    }
    void cancel_reservation() {
        std::lock_guard lock(mutex);
        --reserved;
        if (is_idle())
            idle.notify_all();
    }
    // Called with mutex held. Include a popped task and uncommitted admissions:
    // either can still create work, even while the ring itself is empty.
    bool is_idle() const {
        return count == 0 && reserved == 0 && !executing;
    }
    bool enqueue(QueuedEvent event, bool control = false, bool reservation = false) {
        bool notify;
        {
            std::lock_guard lock(mutex);
            if (reservation)
                --reserved;
            if (stopping ||
                count + reserved >= (control || reservation ? ring.size() : options.capacity)) {
                ++rejected;
                return false;
            }
            notify = count == 0;
            ring[tail] = std::move(event);
            tail = (tail + 1) % ring.size();
            ++count;
            ++accepted;
            if (count > high_water.load(std::memory_order_relaxed))
                high_water.store(count, std::memory_order_relaxed);
        }
        if (notify)
            wake.notify_one();
        return true;
    }
    void run(std::promise<void>& ready) {
        try {
            if (options.thread_enter)
                options.thread_enter();
            current_executor = this;
            ready.set_value();
        } catch (...) {
            ready.set_exception(std::current_exception());
            return;
        }
        for (;;) {
            QueuedEvent event;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this] {
                    return stopping || count;
                });
                if (!count && stopping)
                    break;
                event = std::move(ring[head]);
                ring[head] = {};
                head = (head + 1) % ring.size();
                --count;
                executing = true;
            }
            try {
                if (event.task)
                    event.task();
                ++completed;
                if (event.completion)
                    event.completion->set_value();
            } catch (...) {
                ++failures;
                ++completed;
                if (event.completion)
                    event.completion->set_exception(std::current_exception());
            }
            // Destroy task captures before allowing drain callers to return.
            event = {};
            {
                std::lock_guard lock(mutex);
                executing = false;
                if (is_idle())
                    idle.notify_all();
            }
        }
        current_executor = nullptr;
        if (options.thread_leave)
            options.thread_leave();
    }
};
namespace detail {
class AsyncBackend {
    RegisterMap& map;
    AsyncExecutor* executor;
    std::mutex state_mutex, pending_mutex;
    std::condition_variable pending_changed;
    std::size_t pending{};
    bool accepting{true}, closed{}, in_mmio{}, reserved{}, invalid_defer{};
    std::function<void()> staged;
    std::unordered_set<const RegEntity*> deferred_writes;
    struct Guard {
        AsyncBackend& b;
        explicit Guard(AsyncBackend& value) : b(value) {
            b.enter();
        }
        ~Guard() {
            b.leave();
        }
    };
    RegMemTxResult admit(const EntityState& node, const RegAccessInfo& info) {
        if (current_state != this || !in_mmio)
            return RegMemTxResult::DeviceError;
        if (info.kind != RegTransactionKind::Write || !deferred_writes.contains(node.entity))
            return RegMemTxResult::Ok;
        auto policy = node.transaction_policy.write;
        if (policy == SplitWriteMode::CommitOnHigh &&
            info.internal_byte_offset + info.size != node.value_size)
            return RegMemTxResult::Ok;
        if (policy == SplitWriteMode::CommitOnLow && info.internal_byte_offset != 0)
            return RegMemTxResult::Ok;
        if (reserved)
            throw Error("duplicate work admission");
        if (executor && !executor->impl_->reserve())
            return RegMemTxResult::DeviceError;
        reserved = true;
        return RegMemTxResult::Ok;
    }
    void cancel() {
        if (reserved && executor)
            executor->impl_->cancel_reservation();
        reserved = false;
        staged = {};
    }
    void finished() {
        std::lock_guard lock(pending_mutex);
        --pending;
        pending_changed.notify_all();
    }
    std::function<void()> wrap(std::function<void()> work) {
        return [this, work = std::move(work)] {
            const auto previous = current_task_owner;
            current_task_owner = this;
            try {
                work();
            } catch (...) {
                current_task_owner = previous;
                finished();
                throw;
            }
            current_task_owner = previous;
            finished();
        };
    }
    // Called under state_mutex, after all state/capture work. Async workers can
    // execute immediately, but must acquire state_mutex for their own updates.
    std::function<void()> commit() {
        auto task = wrap(std::move(staged));
        {
            std::lock_guard lock(pending_mutex);
            ++pending;
        }
        if (executor) {
            reserved = false;
            if (!executor->impl_->enqueue({std::move(task)}, false, true)) {
                finished();
                throw Error("executor stopped with an accepted command");
            }
            return {};
        }
        reserved = false;
        return task;
    }

  public:
    AsyncBackend(RegisterMap& value, AsyncExecutor* runner, AsyncAccessOptions options)
        : map(value), executor(runner),
          deferred_writes(options.deferred_writes.begin(), options.deferred_writes.end()) {
        std::lock_guard attach(attachment_mutex);
        if (!map.finalized_ || map.endpoint_owner_)
            throw Error("endpoint requires a finished, unattached map", ErrorKind::Configuration);
        for (auto* entity : deferred_writes) {
            auto it = std::find_if(map.nodes_.begin(), map.nodes_.end(), [entity](auto* n) {
                return n->entity == entity;
            });
            if (it == map.nodes_.end())
                throw Error("deferred entity belongs to another map", ErrorKind::Configuration);
            auto* n = *it;
            if ((!n->is_wide() && !n->write_callback) ||
                (n->is_wide() && !n->wide->wide_write_callback))
                throw Error("deferred entity requires an on_write capture callback",
                            ErrorKind::Configuration);
        }
        if (!deferred_writes.empty())
            map.access_admission_ = [this](const EntityState& n, const RegAccessInfo& i) {
                return admit(n, i);
            };
        map.endpoint_owner_ = this;
    }
    ~AsyncBackend() {
        if (!closed)
            std::terminate();
    }
    void enter() {
        require_unlocked();
        state_mutex.lock();
        if (closed) {
            state_mutex.unlock();
            throw Error("closed register endpoint", ErrorKind::Access);
        }
        current_state = this;
    }
    void leave() noexcept {
        current_state = nullptr;
        state_mutex.unlock();
    }
    RegMmioReadResult read(std::uint64_t address, unsigned size) noexcept {
        try {
            Guard guard(*this);
            if (!accepting)
                return {0, RegMemTxResult::DeviceError};
            in_mmio = true;
            invalid_defer = false;
            auto result = map.try_mmio_read_sized(address, size);
            in_mmio = false;
            if (invalid_defer)
                result.result = RegMemTxResult::DeviceError;
            return result;
        } catch (...) {
            return {0, RegMemTxResult::DeviceError};
        }
    }
    RegMmioWriteResult write(std::uint64_t address, RegData value, unsigned size) noexcept {
        try {
            std::function<void()> synchronous_work;
            RegMmioWriteResult result;
            {
                Guard guard(*this);
                if (!accepting)
                    return {RegMemTxResult::DeviceError};
                in_mmio = true;
                invalid_defer = false;
                result = map.try_mmio_write_sized(address, value, size);
                in_mmio = false;
                if (invalid_defer)
                    result.result = RegMemTxResult::DeviceError;
                try {
                    if (result.result == RegMemTxResult::Ok && reserved) {
                        if (!staged) {
                            result.result = RegMemTxResult::DeviceError;
                            cancel();
                        } else
                            synchronous_work = commit();
                    } else
                        cancel();
                } catch (...) {
                    cancel();
                    throw;
                }
            }
            if (synchronous_work)
                synchronous_work();
            return result;
        } catch (...) {
            return {RegMemTxResult::DeviceError};
        }
    }
    void defer(std::function<void()> work) {
        if (current_state != this || !in_mmio || !reserved || staged || !work) {
            if (current_state == this && in_mmio)
                invalid_defer = true;
            throw Error("defer requires one admitted on_write capture", ErrorKind::Access);
        }
        staged = std::move(work);
    }
    bool post(std::function<void()> work) {
        std::function<void()> synchronous_work;
        {
            Guard guard(*this);
            if ((!accepting && current_task_owner != this) || !work)
                return false;
            if (executor && !executor->impl_->reserve())
                return false;
            reserved = true;
            staged = std::move(work);
            try {
                synchronous_work = commit();
            } catch (...) {
                cancel();
                throw;
            }
        }
        if (synchronous_work)
            synchronous_work();
        return true;
    }
    void fence() {
        require_unlocked();
        if (executor)
            executor->fence();
    }
    void close() {
        require_unlocked();
        if (current_task_owner == this || (executor && executor->in_executor()))
            throw Error("cannot close endpoint on its worker", ErrorKind::Access);
        {
            std::lock_guard lock(state_mutex);
            if (closed)
                return;
            accepting = false;
        }
        {
            std::unique_lock lock(pending_mutex);
            pending_changed.wait(lock, [this] {
                return pending == 0;
            });
        }
        std::lock_guard lock(state_mutex);
        if (closed)
            return;
        closed = true;
        std::lock_guard attach(attachment_mutex);
        map.access_admission_ = {};
        map.endpoint_owner_ = nullptr;
    }
};
} // namespace detail
AsyncExecutor::AsyncExecutor(AsyncExecutorOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {
    std::promise<void> ready;
    auto future = ready.get_future();
    impl_->worker = std::thread([this, &ready] {
        impl_->run(ready);
    });
    try {
        future.get();
    } catch (...) {
        impl_->worker.join();
        throw;
    }
}
AsyncExecutor::~AsyncExecutor() {
    if (in_executor() || current_state)
        std::terminate();
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->wake.notify_one();
    impl_->worker.join();
}
bool AsyncExecutor::in_executor() const noexcept {
    return current_executor == impl_.get();
}
bool AsyncExecutor::post(std::function<void()> task) {
    require_unlocked();
    return impl_->enqueue({std::move(task)});
}
void AsyncExecutor::invoke(std::function<void()> task) {
    require_unlocked();
    if (in_executor())
        throw Error("synchronous wait on the same async executor", ErrorKind::Access);
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    if (!impl_->enqueue({std::move(task), done}, true))
        throw Error("async control queue full or stopping");
    future.get();
}
void AsyncExecutor::fence() {
    invoke([] {});
}
void AsyncExecutor::drain() {
    require_unlocked();
    if (in_executor())
        throw Error("cannot drain own async executor", ErrorKind::Access);
    std::unique_lock lock(impl_->mutex);
    if (impl_->stopping) {
        ++impl_->rejected;
        throw Error("async executor stopping");
    }
    // Preserve accepted/completed accounting for this control operation, but
    // keep waiters out of the ring so they cannot keep one another non-idle.
    ++impl_->accepted;
    impl_->idle.wait(lock, [this] {
        return impl_->is_idle();
    });
    ++impl_->completed;
}
AsyncStatistics AsyncExecutor::statistics() const noexcept {
    return {impl_->accepted.load(),
            impl_->completed.load(),
            impl_->rejected.load(),
            impl_->failures.load(),
            impl_->high_water.load()};
}
RegisterEndpointBase::RegisterEndpointBase(RegisterMap& map,
                                           AsyncExecutor* executor,
                                           AsyncAccessOptions options)
    : backend_(std::make_unique<detail::AsyncBackend>(map, executor, std::move(options))) {
}
RegisterEndpointBase::~RegisterEndpointBase() {
    backend_->close();
}
void RegisterEndpointBase::enter_state() {
    backend_->enter();
}
void RegisterEndpointBase::leave_state() noexcept {
    backend_->leave();
}
bool RegisterEndpointBase::state_lock_held() noexcept {
    return current_state != nullptr;
}
RegMmioReadResult RegisterEndpointBase::read(std::uint64_t a, unsigned s) noexcept {
    return backend_->read(a, s);
}
RegMmioWriteResult RegisterEndpointBase::write(std::uint64_t a, RegData v, unsigned s) noexcept {
    return backend_->write(a, v, s);
}
void RegisterEndpointBase::defer(std::function<void()> f) {
    backend_->defer(std::move(f));
}
bool RegisterEndpointBase::post(std::function<void()> f) {
    return backend_->post(std::move(f));
}
void RegisterEndpointBase::fence() {
    backend_->fence();
}
void RegisterEndpointBase::close() {
    backend_->close();
}
} // namespace reg
