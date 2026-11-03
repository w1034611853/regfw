#pragma once
#include "../types.hpp"
#include <span>
namespace reg {
class Bundle;
}
namespace reg::detail {
struct EntityState;
struct StaticIndexRule {
    const void* source{};
    const void* output{};
    std::string_view name, source_name, output_name;
    unsigned dimension{}, rank{};
    std::uint32_t extent{}, coefficient{};
};
inline constexpr std::uint32_t STATIC_NONE = UINT32_MAX;
enum class StaticFlow : std::uint8_t { Generic, Scalar32, Identity };
struct StaticBinding {
    EntityState* (*get)(void*, const std::array<std::uint32_t, MAX_REG_DIMS>&);
    std::array<std::uint32_t, MAX_REG_DIMS> selectors{};
    std::uint64_t canonical_base{}, local_offset{}, hash{};
    std::string_view path_type;
    std::uint32_t elements{}, scope{};
    unsigned width{};
};
struct StaticScope {
    std::string_view name;
    std::uint64_t base{}, size{}, repeat_count{1}, repeat_stride{}, repeat_index{};
    std::uint32_t parent = STATIC_NONE;
    std::optional<std::uint32_t> instance;
    AliasAccessPolicy policy;
    AccessRouteKind route = AccessRouteKind::Direct;
    std::optional<std::uint32_t> domain;
    const void* resolver_key{};
    std::string_view resolver_name, resolver_type;
    const void* pre_key{};
    std::string_view pre_name, pre_type;
    std::int32_t priority{};
    bool mapping{}, reserved{}, repeated{};
    std::source_location location;
    const Bundle* (*bundle)(void*, const std::array<std::uint32_t, MAX_REG_DIMS>&){};
    std::array<std::uint32_t, MAX_REG_DIMS> selectors{};
    std::uint32_t target_binding = STATIC_NONE, target_nodes{};
};
struct StaticIdentity {
    std::uint64_t base{}, count{}, stride{}, size{};
    std::uint32_t domain{}, scope{};
    const void* resolver_key{};
    std::string_view resolver_name, resolver_type;
};
// One register element as a compile-time geometry unit: byte offset from the
// mount base and its declaration-linear index. Shared by both backends.
struct ElementRecord {
    std::uint64_t offset;
    std::uint32_t linear;
};
} // namespace reg::detail
