#pragma once
#include "types.hpp"
#include <memory>
#include <vector>

namespace reg::detail {
// A type token is used only to validate a handle; no RTTI or pointer punning.
template <class T>
inline constexpr unsigned char value_type_token = 0;
template <class T>
inline constexpr bool direct_wide_storage_v = [] {
    if constexpr (std::same_as<T, WideRegData>)
        return true;
    else
        return requires(T& v) {
            { v.u2048 } -> std::same_as<WideRegData&>;
        };
}();
class PreparedRestore {
  public:
    virtual ~PreparedRestore() = default;
    virtual void commit() noexcept = 0;
};
class ValueStorage {
  public:
    virtual ~ValueStorage() = default;
    virtual std::size_t size() const noexcept = 0;
    virtual std::size_t object_bytes() const noexcept = 0;
    virtual const void* type_token() const noexcept = 0;
    virtual void* element(std::size_t index) noexcept = 0;
    // Native storage opts in; custom codecs retain their virtual semantics.
    virtual std::byte* scalar_bytes() noexcept {
        return nullptr;
    }
    virtual WideRegData* wide_reference(std::size_t index) noexcept = 0;
    virtual std::unique_ptr<PreparedRestore> prepare_restore(std::span<const RegData> words) = 0;
    virtual RegData read_scalar(std::size_t index) const = 0;
    virtual void write_scalar(std::size_t index, RegData value) = 0;
    virtual WideRegData read_wide(std::size_t index) const = 0;
    virtual void write_wide(std::size_t index, WideRegData value) = 0;
};
template <class T>
class TypedStorage final : public ValueStorage {
    // Construct once and never resize; references survive MMIO/reset/restore.
    struct Element {
        T value;
    };
    static_assert(sizeof(Element) == sizeof(T));
    static_assert(std::is_nothrow_copy_assignable_v<T>,
                  "register values must support non-throwing copy assignment");
    std::vector<Element> values_;
    struct Restore final : PreparedRestore {
        TypedStorage* target;
        std::vector<Element> staged;
        explicit Restore(TypedStorage* storage) : target(storage) {
        }
        void commit() noexcept override {
            std::copy(staged.begin(), staged.end(), target->values_.begin());
        }
    };

  public:
    TypedStorage(std::size_t count, WideRegData initial)
        : values_(count, Element{detail::value_from_data<T>(initial)}) {
    }
    std::size_t size() const noexcept override {
        return values_.size();
    }
    std::size_t object_bytes() const noexcept override {
        return sizeof(T) * values_.size();
    }
    const void* type_token() const noexcept override {
        return &value_type_token<T>;
    }
    void* element(std::size_t index) noexcept override {
        return &values_[index].value;
    }
    WideRegData* wide_reference(std::size_t index) noexcept override {
        if constexpr (std::same_as<T, WideRegData>)
            return &values_[index].value;
        else if constexpr (direct_wide_storage_v<T>)
            return &values_[index].value.u2048;
        else
            return nullptr;
    }
    std::unique_ptr<PreparedRestore> prepare_restore(std::span<const RegData> words) override {
        constexpr auto per = (detail::value_bits<T> + 63) / 64;
        if (words.size() != values_.size() * per)
            throw Error("typed snapshot size mismatch", ErrorKind::Snapshot);
        auto next = std::make_unique<Restore>(this);
        next->staged.reserve(values_.size());
        for (std::size_t i = 0; i < values_.size(); ++i) {
            WideRegData raw;
            for (unsigned w = 0; w < per; ++w)
                raw.set_word(w, words[i * per + w]);
            next->staged.push_back(Element{detail::value_from_data<T>(raw)});
        }
        return next;
    }
    RegData read_scalar(std::size_t index) const override {
        if constexpr (detail::wide_value_v<T>)
            return detail::to_bits(values_[index].value).low_u64();
        else
            return RegData(detail::to_bits(values_[index].value)) &
                   detail::low_mask(detail::value_bits<T>);
    }
    void write_scalar(std::size_t index, RegData value) override {
        values_[index].value = detail::value_from_data<T>(value);
    }
    WideRegData read_wide(std::size_t index) const override {
        if constexpr (detail::wide_value_v<T>)
            return WideRegData(detail::to_bits(values_[index].value));
        else
            return WideRegData(read_scalar(index));
    }
    void write_wide(std::size_t index, WideRegData value) override {
        values_[index].value = detail::value_from_data<T>(value);
    }
};

// A native union supplies only object lifetime start and a type token.
// All its storage/MMIO/snapshot implementation lives in storage.cpp.
struct StorageType {
    std::size_t bytes, alignment;
    unsigned bits, word_bytes;
    const void* token;
    void (*construct)(void*, std::size_t, WideRegData);
    WideRegData* (*wide_pointer)(void*);
    std::unique_ptr<ValueStorage> (*custom_factory)(std::size_t, WideRegData);
    bool direct_wide;
};
std::unique_ptr<ValueStorage>
make_storage(const StorageType& type, std::size_t count, WideRegData initial);
template <class T>
inline constexpr StorageType storage_type = [] {
    static_assert(__is_trivially_copyable(T) && __is_nothrow_assignable(T&, const T&),
                  "register values must be trivially copyable and support non-throwing assignment");
    static_assert(
        value_bits<T> == 8 || value_bits<T> == 16 || value_bits<T> == 32 || value_bits<T> == 64 ||
            (wide_value_v<T> && value_bits<T> <= MAX_REGISTER_BITS && value_bits<T> % 8 == 0),
        "register storage must be 8/16/32/64 or byte-aligned wide <=MAX_REGISTER_BITS bits");
    StorageType type{sizeof(T),
                     alignof(T),
                     value_bits<T>,
                     0,
                     &value_type_token<T>,
                     nullptr,
                     nullptr,
                     nullptr,
                     direct_wide_storage_v<T>};
    if constexpr (native_value_v<T> || (std::integral<T> && !std::same_as<T, bool>) ||
                  std::is_enum_v<T> || is_wide_uint_v<T>) {
        if constexpr (native_value_v<T>)
            type.word_bytes = NativeCodec<T>::Member::word_bytes;
        else if constexpr (is_wide_uint_v<T>)
            type.word_bytes = 8;
        else
            type.word_bytes = sizeof(T);
        // C++20 [intro.object]: operator new implicitly creates objects of
        // implicit-lifetime types. Common trivial raw unions need no per-T
        // initialization function. Non-eligible native classes stay explicit.
        if constexpr (!__is_trivial(T)) {
            if constexpr (!(std::is_trivially_destructible_v<T> &&
                            (std::is_aggregate_v<T> || std::is_trivially_copy_constructible_v<T> ||
                             std::is_trivially_move_constructible_v<T>))) {
                type.construct = +[](void* memory, std::size_t count, WideRegData raw) {
                    const auto value = value_from_data<T>(raw);
                    for (std::size_t i = 0; i < count; ++i)
                        std::construct_at(
                            reinterpret_cast<T*>(static_cast<std::byte*>(memory) + i * sizeof(T)),
                            value);
                };
            }
        }
        if constexpr (std::same_as<T, WideRegData>)
            type.wide_pointer = +[](void* memory) {
                return static_cast<T*>(memory);
            };
        else if constexpr (direct_wide_storage_v<T>)
            type.wide_pointer = +[](void* memory) {
                return &static_cast<T*>(memory)->u2048;
            };
    } else
        type.custom_factory =
            +[](std::size_t count, WideRegData initial) -> std::unique_ptr<ValueStorage> {
            return std::make_unique<TypedStorage<T>>(count, initial);
        };
    return type;
}();

// Direct data members are aliases into the real T object, not cached copies.
// Other user-defined members remain available through T& or ref()/operator->.
#define REGFW_DIRECT_MEMBER_VIEW(N)                                                                \
    template <class T, class = void>                                                               \
    struct Direct_##N {                                                                            \
        Direct_##N() = default;                                                                    \
        explicit Direct_##N(T*) noexcept {                                                         \
        }                                                                                          \
    };                                                                                             \
    template <class T>                                                                             \
    struct Direct_##N<T, std::void_t<decltype(&std::declval<T&>().N)>> {                           \
        using Member = std::remove_reference_t<decltype(std::declval<T&>().N)>;                    \
        Member& N;                                                                                 \
        REGFW_DETAIL_ALWAYS_INLINE explicit Direct_##N(T* value) noexcept : N(value->N) {          \
        }                                                                                          \
    }
REGFW_DIRECT_MEMBER_VIEW(bits);
REGFW_DIRECT_MEMBER_VIEW(u8);
REGFW_DIRECT_MEMBER_VIEW(u16);
REGFW_DIRECT_MEMBER_VIEW(u32);
REGFW_DIRECT_MEMBER_VIEW(u64);
REGFW_DIRECT_MEMBER_VIEW(u128);
REGFW_DIRECT_MEMBER_VIEW(u256);
REGFW_DIRECT_MEMBER_VIEW(u512);
REGFW_DIRECT_MEMBER_VIEW(u1024);
REGFW_DIRECT_MEMBER_VIEW(u2048);
#undef REGFW_DIRECT_MEMBER_VIEW

struct ArrayRoot {
    ArrayRoot() = default;
    template <class T>
    explicit ArrayRoot(T*) noexcept {
    }
};

template <class T>
inline constexpr unsigned direct_member_mask = [] {
    unsigned mask = 0;
    if constexpr (requires { &T::bits; })
        mask |= 1u;
    if constexpr (requires { &T::u8; })
        mask |= 2u;
    if constexpr (requires { &T::u16; })
        mask |= 4u;
    if constexpr (requires { &T::u32; })
        mask |= 8u;
    if constexpr (requires { &T::u64; })
        mask |= 16u;
    if constexpr (requires { &T::u128; })
        mask |= 32u;
    if constexpr (requires { &T::u256; })
        mask |= 64u;
    if constexpr (requires { &T::u512; })
        mask |= 128u;
    if constexpr (requires { &T::u1024; })
        mask |= 256u;
    if constexpr (requires { &T::u2048; })
        mask |= 512u;
    return mask;
}();
template <class T, unsigned Mask = direct_member_mask<T>>
struct DirectValueMembers : Direct_bits<T>,
                            Direct_u8<T>,
                            Direct_u16<T>,
                            Direct_u32<T>,
                            Direct_u64<T>,
                            Direct_u128<T>,
                            Direct_u256<T>,
                            Direct_u512<T>,
                            Direct_u1024<T>,
                            Direct_u2048<T> {
    DirectValueMembers() = default;
    REGFW_DETAIL_ALWAYS_INLINE explicit DirectValueMembers(T* value) noexcept
        : Direct_bits<T>(value), Direct_u8<T>(value), Direct_u16<T>(value), Direct_u32<T>(value),
          Direct_u64<T>(value), Direct_u128<T>(value), Direct_u256<T>(value), Direct_u512<T>(value),
          Direct_u1024<T>(value), Direct_u2048<T>(value) {
    }
};
// The overwhelmingly common native layouts need no ten-base trait lattice.
template <class T>
struct DirectValueMembers<T, 9> {
    using Bits = decltype(std::declval<T&>().bits);
    Bits& bits;
    using Raw = decltype(std::declval<T&>().u32);
    Raw& u32;
    REGFW_DETAIL_ALWAYS_INLINE explicit DirectValueMembers(T* value) noexcept
        : bits(value->bits), u32(value->u32) {
    }
};
template <class T>
struct DirectValueMembers<T, 8> {
    using Raw = decltype(std::declval<T&>().u32);
    Raw& u32;
    REGFW_DETAIL_ALWAYS_INLINE explicit DirectValueMembers(T* value) noexcept : u32(value->u32) {
    }
};
template <class T>
struct DirectValueMembers<T, 17> {
    using Bits = decltype(std::declval<T&>().bits);
    Bits& bits;
    using Raw = decltype(std::declval<T&>().u64);
    Raw& u64;
    REGFW_DETAIL_ALWAYS_INLINE explicit DirectValueMembers(T* value) noexcept
        : bits(value->bits), u64(value->u64) {
    }
};
template <class T>
struct DirectValueMembers<T, 16> {
    using Raw = decltype(std::declval<T&>().u64);
    Raw& u64;
    REGFW_DETAIL_ALWAYS_INLINE explicit DirectValueMembers(T* value) noexcept : u64(value->u64) {
    }
};

} // namespace reg::detail
