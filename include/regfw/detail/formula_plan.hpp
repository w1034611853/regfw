#pragma once
// Formula address decoding plan (0.34.0 DS backend).
//
// A FormulaPlan records the arithmetic of a compiled layout instead of a
// per-word address table:
//
//   address_space / view / repeat  ->  StaticScope table (shared with 0.33.3)
//   REG declaration mount          ->  one FormulaMountSpec per mount
//   REG declaration shape          ->  one shared FormulaArraySpec per shape
//
// The decoder inverts an element lattice. Three exact forms are supported:
//
//   * contiguous / super-increasing (monotone) lattices: O(rank) divisions;
//   * two-dimensional interleaved lattices: one gcd reduction plus a modular
//     inverse, so dims(2,3) strides(0x180,0x100) needs no element table;
//   * higher-rank interleaved lattices: a shared offset-sorted record table
//     (the only shape for which the DSL does not determine a closed form).
//
// Element tables are per declaration shape, never per MMIO word, and are
// shared by every mount of that shape.
#include "static_plan.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
namespace reg::detail {
enum class FormulaDecodeKind : std::uint8_t {
    Scalar,   // one element, no array
    Dense,    // rank-1, stride == occupied
    Monotone, // stride super-increasing: successive division
    Table     // non-monotone: shared sorted records
};
// Reverse-decoding description of one register declaration shape. One
// instance is shared by every mount of every declaration with this geometry.
struct FormulaArraySpec {
    std::uint32_t count{}, occupied{};
    std::uint8_t rank{};
    bool monotone{}; // stride super-increasing
    bool dense{};    // rank-1 contiguous
    FormulaDecodeKind kind{FormulaDecodeKind::Scalar};
    // Stride-ascending axes (kept for diagnostics and differential tests).
    std::array<std::uint64_t, MAX_REG_DIMS> strides_asc{};
    std::array<std::uint32_t, MAX_REG_DIMS> extents_asc{};
    std::array<std::uint64_t, MAX_REG_DIMS> multiplier{};
    // Declaration-order view of the same lattice, used by the arithmetic
    // decoder. Declaration order is what RegElementIndex and linear numbers
    // expose; physical order is an implementation detail.
    std::array<std::uint64_t, MAX_REG_DIMS> strides_decl{};
    std::array<std::uint32_t, MAX_REG_DIMS> extents_decl{};
    // Table fallback: shared offset-sorted element records.
    std::span<const ElementRecord> sparse{};
    // Bounding span from element 0 (exact for arithmetic shapes).
    std::uint64_t span{};
};
// One mounted register declaration: absolute element-0 address plus the
// shared array formula. The mount's scope supplies view metadata.
struct FormulaMountSpec {
    std::uint64_t base{}, end{}; // [base, base+span)
    std::uint32_t binding{}, scope{}, array{};
    bool scalar{}; // width-4 Rw independent (Scalar32 candidate)
};
struct FormulaPlanView {
    std::span<const StaticBinding> bindings;
    std::span<const StaticScope> scopes;
    std::span<const StaticIdentity> identities;
    std::span<const FormulaMountSpec> mounts;
    std::span<const FormulaArraySpec> arrays;
};
// Decode one element offset within a mount. Returns false
// for holes (invalid digits or a byte outside the occupied width). The linear
// index keeps the declaration-order RegElementIndex convention.
constexpr bool formula_decode_offset(const FormulaArraySpec& array,
                                     std::uint64_t offset,
                                     std::uint32_t& linear,
                                     std::uint32_t& byte) {
    if (offset >= array.span || !array.count || !array.occupied)
        return false;
    switch (array.kind) {
        case FormulaDecodeKind::Scalar:
            if (offset >= array.occupied)
                return false;
            byte = static_cast<std::uint32_t>(offset);
            linear = 0;
            return true;
        case FormulaDecodeKind::Dense:
            byte = static_cast<std::uint32_t>(offset % array.occupied);
            linear = static_cast<std::uint32_t>(offset / array.occupied);
            return true;
        case FormulaDecodeKind::Table: {
            const auto& records = array.sparse;
            std::size_t lo = 0, hi = records.size();
            while (lo < hi) {
                const auto mid = lo + (hi - lo) / 2;
                if (records[mid].offset <= offset)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            if (lo == 0)
                return false;
            const auto& record = records[lo - 1];
            const auto inside = offset - record.offset;
            if (inside >= array.occupied)
                return false;
            byte = static_cast<std::uint32_t>(inside);
            linear = record.linear;
            return true;
        }
        case FormulaDecodeKind::Monotone:
            break;
    }
    // Super-increasing lattice: division from the largest stride down is
    // exact. Extent-1 axes never contribute and are skipped so degenerate
    // unit dimensions with large strides remain decodable.
    std::uint64_t digits[MAX_REG_DIMS]{};
    auto rest = offset;
    for (std::size_t d = array.rank; d-- > 0;) {
        if (array.extents_asc[d] < 2)
            continue;
        digits[d] = rest / array.strides_asc[d];
        rest %= array.strides_asc[d];
    }
    const auto inside = static_cast<std::uint32_t>(rest);
    if (inside >= array.occupied)
        return false;
    std::uint64_t value = 0;
    for (std::size_t d = 0; d < array.rank; ++d) {
        if (digits[d] >= array.extents_asc[d])
            return false;
        value += digits[d] * array.multiplier[d];
        if (value > UINT32_MAX)
            return false;
    }
    byte = inside;
    linear = static_cast<std::uint32_t>(value);
    return true;
}
// Forward encode (diagnostics enumeration): declaration-linear slot -> offset.
constexpr std::uint64_t formula_element_offset(const FormulaArraySpec& array, std::uint32_t slot) {
    if (slot >= array.count)
        return UINT64_MAX;
    if (array.kind == FormulaDecodeKind::Table)
        return slot < array.sparse.size() ? array.sparse[slot].offset : UINT64_MAX;
    if (array.kind == FormulaDecodeKind::Scalar)
        return 0;
    if (array.kind == FormulaDecodeKind::Dense)
        return std::uint64_t(slot) * array.strides_decl[0];
    std::uint64_t offset = 0, rest = slot;
    for (std::size_t d = array.rank; d-- > 0;) {
        const auto e = array.extents_decl[d];
        if (!e)
            return UINT64_MAX;
        offset += (rest % e) * array.strides_decl[d];
        rest /= e;
    }
    return rest ? UINT64_MAX : offset;
}
// Largest element offset <= bound. Used by the range walkers below and by
// installation-time overlap validation, never by the hot path.
constexpr std::uint64_t formula_floor_offset(const FormulaArraySpec& array, std::uint64_t bound) {
    if (!array.count)
        return UINT64_MAX;
    switch (array.kind) {
        case FormulaDecodeKind::Scalar:
            return 0;
        case FormulaDecodeKind::Dense: {
            const auto slot = bound / array.occupied;
            return std::min<std::uint64_t>(slot, array.count - 1) * array.occupied;
        }
        case FormulaDecodeKind::Table: {
            const auto& records = array.sparse;
            std::size_t lo = 0, hi = records.size();
            while (lo < hi) {
                const auto mid = lo + (hi - lo) / 2;
                if (records[mid].offset <= bound)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            return lo ? records[lo - 1].offset : UINT64_MAX;
        }
        case FormulaDecodeKind::Monotone:
            break;
    }
    std::uint64_t offset = 0;
    for (std::size_t d = array.rank; d-- > 0;) {
        if (array.extents_asc[d] < 2)
            continue;
        const auto digit =
            std::min<std::uint64_t>(bound / array.strides_asc[d], array.extents_asc[d] - 1);
        offset += digit * array.strides_asc[d];
        bound -= digit * array.strides_asc[d];
    }
    return offset;
}
// First element offset strictly after `offset` (an exact element offset).
constexpr std::uint64_t formula_next_offset(const FormulaArraySpec& array, std::uint64_t offset) {
    switch (array.kind) {
        case FormulaDecodeKind::Scalar:
            return UINT64_MAX;
        case FormulaDecodeKind::Dense:
            return offset + array.occupied < array.span ? offset + array.occupied : UINT64_MAX;
        case FormulaDecodeKind::Table: {
            const auto& records = array.sparse;
            std::size_t lo = 0, hi = records.size();
            while (lo < hi) {
                const auto mid = lo + (hi - lo) / 2;
                if (records[mid].offset <= offset)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            return lo < records.size() ? records[lo].offset : UINT64_MAX;
        }
        case FormulaDecodeKind::Monotone:
            break;
    }
    std::uint64_t digits[MAX_REG_DIMS]{};
    for (std::size_t d = array.rank; d-- > 0;) {
        if (array.extents_asc[d] < 2)
            continue;
        digits[d] = offset / array.strides_asc[d];
        offset %= array.strides_asc[d];
    }
    for (std::size_t d = 0; d < array.rank; ++d) {
        if (array.extents_asc[d] < 2)
            continue;
        if (++digits[d] < array.extents_asc[d]) {
            std::uint64_t out = 0;
            for (std::size_t k = 0; k < array.rank; ++k)
                out += digits[k] * array.strides_asc[k];
            return out;
        }
        digits[d] = 0;
    }
    return UINT64_MAX;
}
// Does any element of the shape intersect the mount-relative [low,high)?
constexpr bool
formula_array_intersects(const FormulaArraySpec& array, std::uint64_t low, std::uint64_t high) {
    if (low >= high || !array.count)
        return false;
    const auto first = formula_floor_offset(array, low);
    if (first == UINT64_MAX)
        return false;
    if (first <= low && first + array.occupied > low)
        return true;
    const auto next = formula_next_offset(array, first);
    return next != UINT64_MAX && next < high;
}
} // namespace reg::detail
