#pragma once
#include <cstddef>
#include <memory>
#include <utility>

namespace reg::detail {
// Views follow the single-threaded RegisterMap contract, including copies and
// destruction. Keep their lifetime bookkeeping separate from the weak_ptr used
// by cold layout descriptions. No atomic operation belongs in .at().
struct HandleLifetimeState {
    std::size_t references{1}; // The map owns the initial reference.
    bool alive{true};
};

class HandleLifetime {
    HandleLifetimeState* state_{};

    static void release(HandleLifetimeState* state) noexcept {
        if (state && --state->references == 0)
            delete state;
    }
    friend std::shared_ptr<void> make_handle_lifetime();

  public:
    HandleLifetime() = default;
    explicit HandleLifetime(const std::shared_ptr<void>& life) noexcept
        : state_(static_cast<HandleLifetimeState*>(life.get())) {
        if (state_)
            ++state_->references;
    }
    HandleLifetime(const HandleLifetime& other) noexcept : state_(other.state_) {
        if (state_)
            ++state_->references;
    }
    HandleLifetime(HandleLifetime&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {
    }
    ~HandleLifetime() {
        release(state_);
    }
    bool expired() const noexcept {
        return !state_ || !state_->alive;
    }
};

// Retain the existing weak lifetime API for layout descriptions and bundles.
std::shared_ptr<void> make_handle_lifetime();
} // namespace reg::detail
