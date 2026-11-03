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
    Scalar,       // one element, no array
    Dense,        // rank-1, stride == occupied
    Monotone,     // stride super-increasing: successive division
    Interleaved2, // two axes: gcd reduction + modular inverse
    Table         // higher-rank interleaved: shared sorted records
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
    std::array<std::uint32_t, MAX_REG_DIMS> axis_asc{};
    std::array<std::uint64_t, MAX_REG_DIMS> multiplier{};
    // Declaration-order view of the same lattice, used by the arithmetic
    // decoder. Declaration order is what RegElementIndex and linear numbers
    // expose; physical order is an implementation detail.
    std::array<std::uint64_t, MAX_REG_DIMS> strides_decl{};
    std::array<std::uint32_t, MAX_REG_DIMS> extents_decl{};
    std::array<std::uint64_t, MAX_REG_DIMS> multiplier_decl{};
    // Interleaved2: the two interleaved declaration axes (the two smallest
    // strides). Axes above them (larger strides) form a super-increasing
    // ladder decoded by successive division, so interleaved declarations of
    // any rank are arithmetic whenever every non-pair axis dominates the
    // full span below it.
    std::array<std::uint8_t, MAX_REG_DIMS> inter_axes{};
    // gcd of the two pair strides and the modular inverse of
    // (stride0/gcd) modulo (stride1/gcd). inv_valid is false for a unit
    // factor, where the inverse is not needed.
    std::uint64_t stride_gcd{1}, inverse{};
    bool inverse_valid{};
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
constexpr std::uint64_t formula_gcd(std::uint64_t a, std::uint64_t b) {
    while (b) {
        const auto r = a % b;
        a = b;
        b = r;
    }
    return a;
}
// Modular inverse with a bounded extended-Euclid walk. Computed once per non-monotone
// array shape during layout initialization.
constexpr std::uint64_t formula_mod_inverse(std::uint64_t a, std::uint64_t m) {
    using S = __int128;
    S t = 0, next = 1;
    std::uint64_t r = m, nr = a;
    while (nr) {
        const auto q = r / nr;
        const auto tr = r - q * nr;
        r = nr;
        nr = tr;
        const auto tt = t - static_cast<S>(q) * next;
        t = next;
        next = tt;
    }
    if (r != 1)
        return 0;
    if (t < 0)
        t += static_cast<S>(m);
    return static_cast<std::uint64_t>(t);
}
// The static and dynamic builders use the same exact arithmetic classifier.
// It changes only the layout descriptor; errors are reported by each caller
// using its existing public diagnostic wording.
enum class FormulaClassifyResult : std::uint8_t { Arithmetic, NeedsTable, SelfOverlap, BadInverse };
constexpr FormulaClassifyResult classify_interleaved(FormulaArraySpec& spec) {
    if (spec.rank < 2 || !spec.strides_asc[0] || !spec.strides_asc[1])
        return FormulaClassifyResult::NeedsTable;
    const auto pair_span = std::uint64_t(spec.extents_asc[1] - 1) * spec.strides_asc[1] +
                           std::uint64_t(spec.extents_asc[0] - 1) * spec.strides_asc[0] +
                           spec.occupied;
    std::uint64_t below = pair_span;
    for (std::size_t d = 2; d < spec.rank; ++d) {
        if (spec.extents_asc[d] < 2)
            continue;
        if (spec.strides_asc[d] < below)
            return FormulaClassifyResult::NeedsTable;
        below += std::uint64_t(spec.extents_asc[d] - 1) * spec.strides_asc[d];
    }
    spec.inter_axes[0] = static_cast<std::uint8_t>(spec.axis_asc[0]);
    spec.inter_axes[1] = static_cast<std::uint8_t>(spec.axis_asc[1]);
    const auto s0 = spec.strides_decl[spec.inter_axes[0]];
    const auto s1 = spec.strides_decl[spec.inter_axes[1]];
    const auto g = formula_gcd(s0, s1);
    const auto u = s0 / g, v = s1 / g;
    if (std::uint64_t(spec.extents_decl[spec.inter_axes[0]] - 1) >= v &&
        std::uint64_t(spec.extents_decl[spec.inter_axes[1]] - 1) >= u)
        return FormulaClassifyResult::SelfOverlap;
    spec.stride_gcd = g;
    spec.inverse_valid = u > 1 && v > 1;
    if (spec.inverse_valid) {
        spec.inverse = formula_mod_inverse(u % v, v);
        if (!spec.inverse)
            return FormulaClassifyResult::BadInverse;
    }
    spec.kind = FormulaDecodeKind::Interleaved2;
    spec.span = below;
    return FormulaClassifyResult::Arithmetic;
}
// Exact inverse of the interleaved pair inside one lattice. inter_axes
// selects the two declaration axes; every axis with a larger stride was
// already divided away by the super-increasing ladder. Returns the pair
// digits' linear contribution and the byte inside one element, or false for
// a hole. All products are guarded by __int128; the common unit-factor
// cases avoid the wide division.
constexpr bool formula_decode_interleaved2(const FormulaArraySpec& a,
                                           std::uint64_t offset,
                                           std::uint32_t& linear,
                                           std::uint32_t& byte) {
    const auto d0 = a.inter_axes[0], d1 = a.inter_axes[1];
    const auto s0 = a.strides_decl[d0], s1 = a.strides_decl[d1];
    const auto e0 = a.extents_decl[d0], e1 = a.extents_decl[d1];
    if (!s0 || !s1 || !e0 || !e1 || !a.occupied)
        return false;
    const auto g = a.stride_gcd;
    if (!g)
        return false;
    const auto u = s0 / g, v = s1 / g; // gcd(u,v) == 1
    // The element start must be a multiple of g; the byte inside the element
    // is one of the 4-byte lanes. Loop from lane 0 upward so a word that is
    // itself an element start wins immediately.
    for (std::uint32_t lane = 0; lane < a.occupied; lane += 4) {
        if (offset < lane)
            break;
        const auto start = offset - lane;
        if (start % g)
            continue;
        const auto k = start / g;
        std::uint64_t i = 0, j = 0;
        if (u == 1) {
            j = k / v;
            i = k % v;
            if (j >= e1 || i >= e0)
                continue;
        } else if (v == 1) {
            i = k / u;
            j = k % u;
            if (i >= e0 || j >= e1)
                continue;
        } else {
            if (!a.inverse_valid || !a.inverse)
                continue;
            // i == i0 (mod v); the modular inverse is precomputed.
            const auto i0 =
                static_cast<std::uint64_t>((static_cast<unsigned __int128>(k % v) * a.inverse) % v);
            if (i0 >= e0)
                continue; // a negative t would make i negative
            const auto i0u = static_cast<unsigned __int128>(i0) * u;
            if (static_cast<unsigned __int128>(k) < i0u)
                continue;
            const auto num = static_cast<unsigned __int128>(k) - i0u;
            if (num % v)
                continue;
            const auto j0 = static_cast<std::uint64_t>(num / v);
            // General solution: i = i0 + v*t, j = j0 - u*t, t >= 0.
            std::uint64_t tmin = 0, tmax = (e0 - 1 - i0) / v;
            if (j0 >= e1)
                tmin = (j0 - (e1 - 1) + u - 1) / u;
            if (const auto cap = j0 / u; cap < tmax)
                tmax = cap;
            if (tmin > tmax)
                continue;
            i = i0 + v * tmin;
            j = j0 - u * tmin;
            if (i >= e0 || j >= e1)
                continue;
        }
        byte = lane;
        linear = static_cast<std::uint32_t>(i * a.multiplier_decl[d0] + j * a.multiplier_decl[d1]);
        return true;
    }
    return false;
}
// Pure arithmetic decode of one element offset within a mount. Returns false
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
        case FormulaDecodeKind::Interleaved2: {
            // Super-increasing ladder above the interleaved pair: divide from
            // the largest stride down (extent-1 axes contribute nothing),
            // then invert the pair on the remainder.
            auto rest = offset;
            std::uint32_t ladder_linear = 0;
            for (std::size_t d = array.rank; d-- > 2;) {
                if (array.extents_asc[d] < 2)
                    continue;
                const auto digit = rest / array.strides_asc[d];
                if (digit >= array.extents_asc[d])
                    return false;
                rest -= digit * array.strides_asc[d];
                ladder_linear += static_cast<std::uint32_t>(digit) *
                                 static_cast<std::uint32_t>(array.multiplier[d]);
            }
            std::uint32_t pair_linear = 0, pair_byte = 0;
            if (!formula_decode_interleaved2(array, rest, pair_linear, pair_byte))
                return false;
            linear = ladder_linear + pair_linear;
            byte = pair_byte;
            return true;
        }
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
        case FormulaDecodeKind::Interleaved2: {
            // Super-increasing ladder first (greedy digits are exact), then
            // the pair floor on the remainder.
            std::uint64_t base = 0;
            for (std::size_t d = array.rank; d-- > 2;) {
                if (array.extents_asc[d] < 2)
                    continue;
                const auto digit =
                    std::min<std::uint64_t>(bound / array.strides_asc[d], array.extents_asc[d] - 1);
                base += digit * array.strides_asc[d];
                bound -= digit * array.strides_asc[d];
            }
            const auto d0 = array.inter_axes[0], d1 = array.inter_axes[1];
            std::uint64_t best = UINT64_MAX;
            for (std::uint32_t i = 0; i < array.extents_decl[d0]; ++i) {
                const auto ibase = std::uint64_t(i) * array.strides_decl[d0];
                if (ibase > bound)
                    break;
                const auto room = bound - ibase;
                const auto j = std::min<std::uint64_t>(room / array.strides_decl[d1],
                                                       std::uint64_t(array.extents_decl[d1]) - 1);
                const auto candidate = ibase + j * array.strides_decl[d1];
                if (candidate <= bound && candidate > best)
                    best = candidate;
            }
            if (best != UINT64_MAX)
                return base + best;
            // No pair element in this cell: fall back to the previous ladder
            // cell with the pair's largest offset.
            for (std::size_t d = 2; d < array.rank; ++d) {
                if (array.extents_asc[d] < 2)
                    continue;
                const auto digit = base / array.strides_asc[d];
                if (!digit)
                    continue;
                std::uint64_t prev = base - array.strides_asc[d];
                // zeroes of every lower ladder axis
                for (std::size_t e = 2; e < d; ++e)
                    prev -= (prev / array.strides_asc[e]) * array.strides_asc[e];
                const auto pair_max =
                    std::uint64_t(array.extents_decl[d0] - 1) * array.strides_decl[d0] +
                    std::uint64_t(array.extents_decl[d1] - 1) * array.strides_decl[d1];
                return prev + pair_max;
            }
            return UINT64_MAX;
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
        case FormulaDecodeKind::Interleaved2: {
            // Next inside the current ladder cell, else the next cell's
            // smallest pair offset (the pair always has an element at 0).
            std::uint64_t digits[MAX_REG_DIMS]{};
            std::uint64_t base = 0, rest = offset;
            for (std::size_t d = array.rank; d-- > 2;) {
                if (array.extents_asc[d] < 2)
                    continue;
                const auto digit = rest / array.strides_asc[d];
                digits[d] = digit;
                base += digit * array.strides_asc[d];
                rest -= digit * array.strides_asc[d];
            }
            const auto d0 = array.inter_axes[0], d1 = array.inter_axes[1];
            std::uint64_t best = UINT64_MAX;
            for (std::uint32_t i = 0; i < array.extents_decl[d0]; ++i) {
                for (std::uint32_t j = 0; j < array.extents_decl[d1]; ++j) {
                    const auto candidate = std::uint64_t(i) * array.strides_decl[d0] +
                                           std::uint64_t(j) * array.strides_decl[d1];
                    if (candidate > rest && candidate < best)
                        best = candidate;
                }
            }
            // Next ladder cell: increment the lowest ladder axis that can
            // still advance, zeroing every lower ladder axis. Under a
            // super-increasing ladder the cells are ordered by base.
            std::uint64_t next_base = UINT64_MAX;
            for (std::size_t d = 2; d < array.rank; ++d) {
                if (array.extents_asc[d] < 2)
                    continue;
                if (digits[d] + 1 < array.extents_asc[d]) {
                    next_base = base;
                    for (std::size_t e = 2; e <= d; ++e)
                        if (array.extents_asc[e] >= 2)
                            next_base -= digits[e] * array.strides_asc[e];
                    next_base += (digits[d] + 1) * array.strides_asc[d];
                    break;
                }
            }
            if (best != UINT64_MAX && (next_base == UINT64_MAX || base + best < next_base))
                return base + best;
            return next_base;
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
