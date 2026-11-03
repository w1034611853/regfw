#pragma once
#include "fixed_uint.hpp"
#include <bit>
#include <limits>

namespace reg {
using RegData = std::uint64_t;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
// Optional dump metadata. Native unions do not need generated value methods.
struct FieldDescriptor {
    std::string_view name;
    unsigned lsb, width;
    std::string (*debug)(RegData){};
};
struct WideFieldDescriptor {
    std::string_view name;
    unsigned lsb, width;
    std::string (*debug)(WideRegData){};
};
namespace detail {
constexpr RegData low_mask(unsigned width) {
    if (width > 64)
        throw std::out_of_range("bit width exceeds 64");
    return width == 64 ? ~RegData{} : width == 0 ? 0 : (RegData{1} << width) - 1;
}
// Detect the user's real raw member, keeping C arrays and std::array intact.
template <class M, unsigned Expected>
struct NativeMember {
    static constexpr bool sequence = false;
    static constexpr unsigned word_bytes = Expected <= 64 ? Expected / 8 : 8;
    static constexpr unsigned bits = sizeof(M) * 8;
    static constexpr bool valid = [] {
        if constexpr (is_wide_uint_v<M>)
            return M::BITS == Expected;
        else
            return std::integral<M> && !std::same_as<M, bool> && bits == Expected;
    }();
};
template <class W, std::size_t N, unsigned Expected>
struct NativeMember<W[N], Expected> {
    using Word = W;
    static constexpr bool sequence = true;
    static constexpr unsigned word_bytes = sizeof(W);
    static constexpr std::size_t count = N;
    static constexpr unsigned bits = sizeof(W) * N * 8;
    static constexpr bool valid =
        N > 0 && std::unsigned_integral<W> && !std::same_as<W, bool> && sizeof(W) * 8 == Expected;
};
template <class W, std::size_t N, unsigned Expected>
struct NativeMember<std::array<W, N>, Expected> : NativeMember<W[N ? N : 1], Expected> {
    static constexpr std::size_t count = N;
    static constexpr unsigned bits = sizeof(W) * N * 8;
    static constexpr bool valid = N > 0 && NativeMember<W[N ? N : 1], Expected>::valid;
};
template <class T>
constexpr auto native_member_type() {
    if constexpr (requires { &T::u8; })
        return std::type_identity<
            NativeMember<std::remove_cvref_t<decltype(std::declval<T&>().u8)>, 8>>{};
    else if constexpr (requires { &T::u16; })
        return std::type_identity<
            NativeMember<std::remove_cvref_t<decltype(std::declval<T&>().u16)>, 16>>{};
    else if constexpr (requires { &T::u32; })
        return std::type_identity<
            NativeMember<std::remove_cvref_t<decltype(std::declval<T&>().u32)>, 32>>{};
    else if constexpr (requires { &T::u64; })
        return std::type_identity<
            NativeMember<std::remove_cvref_t<decltype(std::declval<T&>().u64)>, 64>>{};
    else if constexpr (requires { &T::u128; })
        return std::type_identity<
            NativeMember<std::remove_cvref_t<decltype(std::declval<T&>().u128)>, 128>>{};
    else if constexpr (requires { &T::u256; })
        return std::type_identity<
            NativeMember<std::remove_cvref_t<decltype(std::declval<T&>().u256)>, 256>>{};
    else if constexpr (requires { &T::u512; })
        return std::type_identity<
            NativeMember<std::remove_cvref_t<decltype(std::declval<T&>().u512)>, 512>>{};
    else if constexpr (requires { &T::u1024; })
        return std::type_identity<
            NativeMember<std::remove_cvref_t<decltype(std::declval<T&>().u1024)>, 1024>>{};
    else if constexpr (requires { &T::u2048; })
        return std::type_identity<
            NativeMember<std::remove_cvref_t<decltype(std::declval<T&>().u2048)>, 2048>>{};
    else
        return std::type_identity<void>{};
}
template <class T>
using native_member_t = typename decltype(native_member_type<T>())::type;
template <class T>
inline constexpr bool native_value_v = !std::same_as<native_member_t<T>, void>;
template <class T, unsigned Bits>
constexpr bool native_views_valid() {
    if constexpr (requires { &T::u8; }) {
        using M = NativeMember<std::remove_cvref_t<decltype(std::declval<T&>().u8)>, 8>;
        if constexpr (!requires(T& object) { object.*(&T::u8); } || !M::valid || M::bits != Bits)
            return false;
    }
    if constexpr (requires { &T::u16; }) {
        using M = NativeMember<std::remove_cvref_t<decltype(std::declval<T&>().u16)>, 16>;
        if constexpr (!requires(T& object) { object.*(&T::u16); } || !M::valid || M::bits != Bits)
            return false;
    }
    if constexpr (requires { &T::u32; }) {
        using M = NativeMember<std::remove_cvref_t<decltype(std::declval<T&>().u32)>, 32>;
        if constexpr (!requires(T& object) { object.*(&T::u32); } || !M::valid || M::bits != Bits)
            return false;
    }
    if constexpr (requires { &T::u64; }) {
        using M = NativeMember<std::remove_cvref_t<decltype(std::declval<T&>().u64)>, 64>;
        if constexpr (!requires(T& object) { object.*(&T::u64); } || !M::valid || M::bits != Bits)
            return false;
    }
    if constexpr (requires { &T::u128; }) {
        using M = NativeMember<std::remove_cvref_t<decltype(std::declval<T&>().u128)>, 128>;
        if constexpr (!requires(T& object) { object.*(&T::u128); } || !M::valid || M::bits != Bits)
            return false;
    }
    if constexpr (requires { &T::u256; }) {
        using M = NativeMember<std::remove_cvref_t<decltype(std::declval<T&>().u256)>, 256>;
        if constexpr (!requires(T& object) { object.*(&T::u256); } || !M::valid || M::bits != Bits)
            return false;
    }
    if constexpr (requires { &T::u512; }) {
        using M = NativeMember<std::remove_cvref_t<decltype(std::declval<T&>().u512)>, 512>;
        if constexpr (!requires(T& object) { object.*(&T::u512); } || !M::valid || M::bits != Bits)
            return false;
    }
    if constexpr (requires { &T::u1024; }) {
        using M = NativeMember<std::remove_cvref_t<decltype(std::declval<T&>().u1024)>, 1024>;
        if constexpr (!requires(T& object) { object.*(&T::u1024); } || !M::valid || M::bits != Bits)
            return false;
    }
    if constexpr (requires { &T::u2048; }) {
        using M = NativeMember<std::remove_cvref_t<decltype(std::declval<T&>().u2048)>, 2048>;
        if constexpr (!requires(T& object) { object.*(&T::u2048); } || !M::valid || M::bits != Bits)
            return false;
    }
    return true;
}
template <class T>
struct NativeCodec {
    using Member = native_member_t<T>;
    static_assert(Member::valid,
                  "raw member must match its u8/u16/u32/u64 name (scalar, C array or std::array), "
                  "or hold the matching WideUInt");
    static_assert(__is_trivially_copyable(T) && __is_standard_layout(T),
                  "native register must be trivially copyable and standard-layout");
    static_assert(Member::bits > 0 && Member::bits <= MAX_REGISTER_BITS,
                  "native register exceeds MAX_REGISTER_BITS");
    static_assert(
        Member::bits > 64 || Member::bits == 8 || Member::bits == 16 || Member::bits == 32 ||
            Member::bits == 64,
        "native scalar storage must be exactly 8/16/32/64 bits; wider storage must exceed 64 bits");
    static_assert(sizeof(T) * 8 == Member::bits,
                  "native register size must equal its complete raw member; remove padding or "
                  "mismatched bits");
    // Multiple raw views are allowed only when they cover the same full value.
    static_assert(native_views_valid<T, Member::bits>(),
                  "raw member views must be non-static, correctly typed and have identical widths");
    using Data = std::conditional_t<
        (Member::bits > 64),
        WideUInt<(Member::bits > 64 ? Member::bits : 128)>,
        std::conditional_t<
            Member::bits == 8,
            std::uint8_t,
            std::conditional_t<
                Member::bits == 16,
                std::uint16_t,
                std::conditional_t<Member::bits == 32, std::uint32_t, std::uint64_t>>>>;
    static auto encode(const T& value) noexcept {
        if constexpr (!Member::sequence)
            return std::bit_cast<Data>(value);
        else {
            using Word = typename Member::Word;
            auto words = std::bit_cast<std::array<Word, Member::count>>(value);
            Data result{};
            for (unsigned i = 0; i < Member::count; ++i) {
                if constexpr (Member::bits <= 64)
                    result |= Data(words[i]) << (i * sizeof(Word) * 8);
                else {
                    constexpr unsigned per = 64 / (sizeof(Word) * 8);
                    auto w = i / per;
                    result.set_word(w,
                                    result.word(w) | (std::uint64_t(words[i])
                                                      << ((i % per) * sizeof(Word) * 8)));
                }
            }
            return result;
        }
    }
    static T decode(Data value) noexcept {
        if constexpr (!Member::sequence)
            return std::bit_cast<T>(value);
        else {
            using Word = typename Member::Word;
            std::array<Word, Member::count> words{};
            for (unsigned i = 0; i < Member::count; ++i) {
                if constexpr (Member::bits <= 64)
                    words[i] = Word(value >> (i * sizeof(Word) * 8));
                else {
                    constexpr unsigned per = 64 / (sizeof(Word) * 8);
                    words[i] = Word(value.word(i / per) >> ((i % per) * sizeof(Word) * 8));
                }
            }
            return std::bit_cast<T>(words);
        }
    }
};
template <class T>
constexpr auto to_bits(T value) {
    if constexpr (native_value_v<T>)
        return to_bits(NativeCodec<T>::encode(value));
    else if constexpr (is_wide_uint_v<T>)
        return value;
    else if constexpr (requires { value.raw(); } && !std::integral<T> && !std::is_enum_v<T>)
        return to_bits(value.raw());
    else if constexpr (std::same_as<T, bool>)
        return std::uint64_t(value ? 1 : 0);
    else if constexpr (std::integral<T>)
        return static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<T>>(value));
    else if constexpr (std::is_enum_v<T>)
        return to_bits(static_cast<std::underlying_type_t<T>>(value));
    else
        return to_bits(value.raw());
}
template <class T>
constexpr T from_bits(std::uint64_t value) {
    if constexpr (native_value_v<T>)
        return NativeCodec<T>::decode(static_cast<typename NativeCodec<T>::Data>(value));
    else if constexpr (std::integral<T>) {
        using U = std::make_unsigned_t<std::conditional_t<std::same_as<T, bool>, std::uint8_t, T>>;
        if constexpr (std::is_signed_v<T>)
            return std::bit_cast<T>(static_cast<U>(value));
        else
            return static_cast<T>(value);
    } else if constexpr (std::is_enum_v<T>)
        return static_cast<T>(value);
    else
        return T::from_raw(static_cast<typename T::Raw>(value));
}
template <class T>
struct raw_type {
    using type = typename T::Raw;
};
template <class T>
    requires(native_value_v<T>)
struct raw_type<T> {
    using type = typename NativeCodec<T>::Data;
};
template <std::integral T>
struct raw_type<T> {
    using type = T;
};
template <class T>
    requires std::is_enum_v<T>
struct raw_type<T> {
    using type = std::underlying_type_t<T>;
};
template <unsigned N>
struct raw_type<WideUInt<N>> {
    using type = WideUInt<N>;
};
template <class T>
using raw_type_t = typename raw_type<T>::type;
template <class T>
constexpr T value_from_data(RegData value) {
    if constexpr (is_wide_uint_v<T>)
        return T(value);
    else if constexpr (native_value_v<T>)
        return NativeCodec<T>::decode(static_cast<raw_type_t<T>>(value));
    else if constexpr (std::integral<T> || std::is_enum_v<T>)
        return from_bits<T>(value);
    else
        return T::from_raw(static_cast<raw_type_t<T>>(value));
}
template <class T>
constexpr unsigned value_bits = [] {
    if constexpr (is_wide_uint_v<raw_type_t<T>>)
        return raw_type_t<T>::BITS;
    else
        return unsigned(sizeof(raw_type_t<T>) * 8);
}();
template <class T>
inline constexpr bool wide_value_v = is_wide_uint_v<raw_type_t<T>>;
template <class T>
constexpr T value_from_data(WideRegData value) {
    if constexpr (is_wide_uint_v<T>)
        return value.template truncated_to<T::BITS>();
    else if constexpr (native_value_v<T> && wide_value_v<T>)
        return NativeCodec<T>::decode(value.template truncated_to<value_bits<T>>());
    else if constexpr (wide_value_v<T>)
        return T::from_raw(value.template truncated_to<value_bits<T>>());
    else
        return value_from_data<T>(value.low_u64());
}
template <class T>
constexpr T storage_cast(auto value) {
    if constexpr (is_wide_uint_v<T>) {
        if constexpr (is_wide_uint_v<decltype(value)>)
            return value.template truncated_to<T::BITS>();
        else
            return T(to_bits(value));
    } else {
        if constexpr (is_wide_uint_v<decltype(value)>)
            return static_cast<T>(value.low_u64());
        else
            return static_cast<T>(value);
    }
}

} // namespace detail
} // namespace reg
