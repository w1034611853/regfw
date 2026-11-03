#pragma once
#include "types.hpp"

namespace reg {
class RegisterMap;
namespace detail {
struct EntityState;
}

// Owns scalar/array state, behavior and callbacks. Address layouts are separate
// non-owning views. REG creates both automatically, under the map's ownership.
class RegEntity final {
    std::unique_ptr<detail::EntityState> state_;
    explicit RegEntity(std::unique_ptr<detail::EntityState> state);
    friend class RegisterMap;

  public:
    ~RegEntity();
    RegEntity(const RegEntity&) = delete;
    RegEntity& operator=(const RegEntity&) = delete;
    RegEntity(RegEntity&&) = delete;
    RegEntity& operator=(RegEntity&&) = delete;
    std::string_view name() const noexcept;
    std::size_t element_count() const noexcept;
    std::size_t storage_bytes() const noexcept;
    unsigned width_bits() const noexcept;
    std::span<const std::size_t> dimensions() const noexcept;
    void reset() const;
};
} // namespace reg
