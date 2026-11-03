#pragma once
#include <array>
#include <compare>
#include <concepts>
#include <cstdint>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

// Initialization boundaries stay out of giant generated bundle constructors.
#if defined(__GNUC__) || defined(__clang__)
#define REGFW_DETAIL_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define REGFW_DETAIL_NOINLINE __declspec(noinline)
#else
#define REGFW_DETAIL_NOINLINE
#endif
#if defined(REGFW_FAST_COMPILE_INIT) && defined(__clang__)
#define REGFW_DETAIL_INIT_CTOR __attribute__((noinline, optnone))
#elif defined(REGFW_FAST_COMPILE_INIT) && defined(__GNUC__)
#define REGFW_DETAIL_INIT_CTOR __attribute__((noinline, optimize("O0")))
#elif defined(_MSC_VER)
#define REGFW_DETAIL_INIT_CTOR __declspec(noinline)
#else
#define REGFW_DETAIL_INIT_CTOR REGFW_DETAIL_NOINLINE
#endif
#if defined(__GNUC__) || defined(__clang__)
#define REGFW_DETAIL_ALWAYS_INLINE __attribute__((always_inline))
#elif defined(_MSC_VER)
#define REGFW_DETAIL_ALWAYS_INLINE __forceinline
#else
#define REGFW_DETAIL_ALWAYS_INLINE
#endif
namespace reg {
namespace detail {
class NativeStorage;
std::ostream& write_wide_text(std::ostream&, const std::string&);
} // namespace detail
inline constexpr unsigned MAX_REGISTER_BITS = 2048;
// Fixed-size, little-endian limbs; no heap allocation for arithmetic or storage.
// Conversion to a smaller value is always explicit and checked.
template <unsigned Bits>
class WideUInt {
    friend class detail::NativeStorage;
    static_assert(Bits > 64 && Bits <= MAX_REGISTER_BITS);
    static constexpr unsigned count_ = (Bits + 63) / 64;
    std::array<std::uint64_t, count_> words_{};
    constexpr void trim() {
        if constexpr (Bits % 64)
            words_.back() &= (std::uint64_t{1} << (Bits % 64)) - 1;
    }

  public:
    static constexpr unsigned BITS = Bits, WORDS = count_;
    constexpr WideUInt() = default;
    template <std::integral T>
    constexpr WideUInt(T v) {
        if constexpr (std::is_signed_v<T>)
            if (v < 0)
                throw std::out_of_range("negative wide unsigned value");
        words_[0] = static_cast<std::uint64_t>(v);
    }
    template <unsigned Other>
    constexpr explicit(Other > Bits) WideUInt(const WideUInt<Other>& v) {
        if constexpr (Other > Bits) {
            for (unsigned i = WORDS; i < WideUInt<Other>::WORDS; ++i)
                if (v.word(i))
                    throw std::out_of_range("wide checked conversion overflow");
            if constexpr (Bits % 64)
                if (v.word(WORDS - 1) >> (Bits % 64))
                    throw std::out_of_range("wide checked conversion overflow");
        }
        constexpr auto common = WORDS < WideUInt<Other>::WORDS ? WORDS : WideUInt<Other>::WORDS;
        for (unsigned i = 0; i < common; ++i)
            words_[i] = v.word(i);
    }
    constexpr std::uint64_t low_u64() const {
        return words_[0];
    }
    constexpr const auto& words() const {
        return words_;
    }
    constexpr std::uint64_t word(unsigned i) const {
        if (i >= WORDS)
            throw std::out_of_range("wide word index");
        return words_[i];
    }
    constexpr void set_word(unsigned i, std::uint64_t v) {
        if (i >= WORDS)
            throw std::out_of_range("wide word index");
        if constexpr (Bits % 64)
            if (i == WORDS - 1 && (v >> (Bits % 64)))
                throw std::out_of_range("wide word overflow");
        words_[i] = v;
    }
    static constexpr WideUInt from_words(std::array<std::uint64_t, WORDS> words) {
        WideUInt out;
        for (unsigned i = 0; i < WORDS; ++i)
            out.set_word(i, words[i]);
        return out;
    }
    static constexpr WideUInt from_hex(std::string_view text) {
        if (text.starts_with("0x") || text.starts_with("0X"))
            text.remove_prefix(2);
        WideUInt out;
        unsigned position = 0;
        for (auto it = text.rbegin(); it != text.rend(); ++it) {
            char c = *it;
            if (c == '_')
                continue;
            unsigned n = c >= '0' && c <= '9'   ? c - '0'
                         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                : 16;
            if (n == 16)
                throw std::invalid_argument("invalid wide hex digit");
            for (unsigned j = 0; j < 4; ++j)
                if (n & (1u << j)) {
                    if (position + j >= Bits)
                        throw std::out_of_range("wide hex overflow");
                    out.set_bit(position + j, true);
                }
            position += 4;
        }
        return out;
    }
    constexpr bool bit(unsigned i) const {
        if (i >= Bits)
            throw std::out_of_range("wide bit index");
        return (words_[i / 64] >> (i % 64)) & 1;
    }
    constexpr void set_bit(unsigned i, bool v) {
        if (i >= Bits)
            throw std::out_of_range("wide bit index");
        auto m = std::uint64_t{1} << (i % 64);
        words_[i / 64] = (words_[i / 64] & ~m) | (v ? m : 0);
    }
    static constexpr WideUInt mask(unsigned n) {
        if (n > Bits)
            throw std::out_of_range("wide mask width");
        WideUInt v;
        auto full = n / 64, remaining = n % 64;
        for (unsigned i = 0; i < full; ++i)
            v.words_[i] = ~std::uint64_t{};
        if (remaining)
            v.words_[full] = (std::uint64_t{1} << remaining) - 1;
        return v;
    }
    constexpr explicit operator bool() const {
        for (auto w : words_)
            if (w)
                return true;
        return false;
    }
    template <std::unsigned_integral T>
    constexpr T checked() const {
        for (unsigned i = sizeof(T) * 8; i < Bits; ++i)
            if (bit(i))
                throw std::out_of_range("wide checked conversion overflow");
        return static_cast<T>(low_u64());
    }
    template <unsigned N>
    constexpr WideUInt<N> truncated_to() const {
        WideUInt<N> v;
        constexpr auto common = WORDS < WideUInt<N>::WORDS ? WORDS : WideUInt<N>::WORDS;
        for (unsigned i = 0; i < common; ++i) {
            auto value = words_[i];
            if constexpr (N % 64)
                if (i == WideUInt<N>::WORDS - 1)
                    value &= (std::uint64_t{1} << (N % 64)) - 1;
            v.set_word(i, value);
        }
        return v;
    }
    constexpr bool operator==(const WideUInt&) const = default;
    constexpr std::strong_ordering operator<=>(const WideUInt& b) const {
        for (unsigned i = WORDS; i-- > 0;)
            if (auto c = words_[i] <=> b.words_[i]; c != 0)
                return c;
        return std::strong_ordering::equal;
    }
    constexpr WideUInt operator~() const {
        auto v = *this;
        for (auto& w : v.words_)
            w = ~w;
        v.trim();
        return v;
    }
    constexpr WideUInt& operator&=(WideUInt b) {
        for (unsigned i = 0; i < WORDS; ++i)
            words_[i] &= b.words_[i];
        return *this;
    }
    constexpr WideUInt& operator|=(WideUInt b) {
        for (unsigned i = 0; i < WORDS; ++i)
            words_[i] |= b.words_[i];
        return *this;
    }
    constexpr WideUInt& operator^=(WideUInt b) {
        for (unsigned i = 0; i < WORDS; ++i)
            words_[i] ^= b.words_[i];
        return *this;
    }
    friend constexpr WideUInt operator&(WideUInt a, WideUInt b) {
        return a &= b;
    }
    friend constexpr WideUInt operator|(WideUInt a, WideUInt b) {
        return a |= b;
    }
    friend constexpr WideUInt operator^(WideUInt a, WideUInt b) {
        return a ^= b;
    }
    constexpr WideUInt operator<<(unsigned shift) const {
        WideUInt out;
        if (shift >= Bits)
            return out;
        for (unsigned i = 0; i < WORDS; ++i) {
            auto dst = i + shift / 64;
            if (dst >= WORDS)
                break;
            out.words_[dst] |= words_[i] << (shift % 64);
            if (shift % 64 && dst + 1 < WORDS)
                out.words_[dst + 1] |= words_[i] >> (64 - shift % 64);
        }
        out.trim();
        return out;
    }
    constexpr WideUInt operator>>(unsigned shift) const {
        WideUInt out;
        if (shift >= Bits)
            return out;
        for (unsigned dst = 0; dst < WORDS; ++dst) {
            auto src = dst + shift / 64;
            if (src >= WORDS)
                break;
            out.words_[dst] |= words_[src] >> (shift % 64);
            if (shift % 64 && src + 1 < WORDS)
                out.words_[dst] |= words_[src + 1] << (64 - shift % 64);
        }
        return out;
    }
    constexpr WideUInt& operator<<=(unsigned s) {
        return *this = *this << s;
    }
    constexpr WideUInt& operator>>=(unsigned s) {
        return *this = *this >> s;
    }
    friend constexpr WideUInt operator+(WideUInt a, WideUInt b) {
        std::uint64_t carry = 0;
        for (unsigned i = 0; i < WORDS; ++i) {
            auto x = a.words_[i], sum = x + b.words_[i];
            auto c1 = sum < x;
            auto total = sum + carry;
            auto c2 = total < sum;
            a.words_[i] = total;
            carry = c1 || c2;
        }
        a.trim();
        return a;
    }
    friend constexpr WideUInt operator-(WideUInt a, WideUInt b) {
        std::uint64_t borrow = 0;
        for (unsigned i = 0; i < WORDS; ++i) {
            auto x = a.words_[i], diff = x - b.words_[i];
            auto b1 = x < b.words_[i];
            auto total = diff - borrow;
            auto b2 = diff < borrow;
            a.words_[i] = total;
            borrow = b1 || b2;
        }
        a.trim();
        return a;
    }
    constexpr WideUInt& operator+=(WideUInt b) {
        return *this = *this + b;
    }
    constexpr WideUInt& operator-=(WideUInt b) {
        return *this = *this - b;
    }
    std::string hex(bool padded = false) const {
        constexpr char digits[] = "0123456789abcdef";
        std::string s;
        for (unsigned n = (Bits + 3) / 4; n-- > 0;) {
            unsigned d = 0;
            for (unsigned j = 0; j < 4 && n * 4 + j < Bits; ++j)
                if (bit(n * 4 + j))
                    d |= 1u << j;
            if (padded || !s.empty() || d || n == 0)
                s += digits[d];
        }
        return s;
    }
};
template <class T>
struct is_wide_uint : std::false_type {};
template <unsigned N>
struct is_wide_uint<WideUInt<N>> : std::true_type {};
template <class T>
inline constexpr bool is_wide_uint_v = is_wide_uint<T>::value;
template <unsigned N>
std::ostream& operator<<(std::ostream& out, WideUInt<N> value) {
    return detail::write_wide_text(out, value.hex());
}
using u128 = WideUInt<128>;
using u256 = WideUInt<256>;
using u512 = WideUInt<512>;
using u1024 = WideUInt<1024>;
using u2048 = WideUInt<2048>;
using WideRegData = u2048;
} // namespace reg
