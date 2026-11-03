#include <regfw/storage.hpp>
#include <cstring>
#include <new>
namespace reg::detail {
class NativeStorage final : public ValueStorage {
    struct Allocation {
        void* data;
        std::size_t alignment;
        Allocation(std::size_t bytes, std::size_t align)
            : data(::operator new(bytes, std::align_val_t(align))), alignment(align) {
        }
        ~Allocation() {
            ::operator delete(data, std::align_val_t(alignment));
        }
        Allocation(const Allocation&) = delete;
    };
    Allocation allocation_;
    std::size_t count_, stride_;
    unsigned bits_, word_bytes_;
    const void* token_;
    WideRegData* (*wide_pointer_)(void*);
    const std::byte* pointer(std::size_t i) const {
        return static_cast<const std::byte*>(allocation_.data) + i * stride_;
    }
    std::byte* pointer(std::size_t i) {
        return static_cast<std::byte*>(allocation_.data) + i * stride_;
    }
    static RegData load(const std::byte* p, unsigned bytes) noexcept {
        switch (bytes) {
            case 1: {
                std::uint8_t v;
                std::memcpy(&v, p, 1);
                return v;
            }
            case 2: {
                std::uint16_t v;
                std::memcpy(&v, p, 2);
                return v;
            }
            case 4: {
                std::uint32_t v;
                std::memcpy(&v, p, 4);
                return v;
            }
            default: {
                std::uint64_t v;
                std::memcpy(&v, p, 8);
                return v;
            }
        }
    }
    static void store(std::byte* p, unsigned bytes, RegData value) noexcept {
        switch (bytes) {
            case 1: {
                auto v = static_cast<std::uint8_t>(value);
                std::memcpy(p, &v, 1);
                break;
            }
            case 2: {
                auto v = static_cast<std::uint16_t>(value);
                std::memcpy(p, &v, 2);
                break;
            }
            case 4: {
                auto v = static_cast<std::uint32_t>(value);
                std::memcpy(p, &v, 4);
                break;
            }
            default:
                std::memcpy(p, &value, 8);
                break;
        }
    }
    struct Restore final : PreparedRestore {
        NativeStorage& target;
        std::vector<RegData> words;
        Restore(NativeStorage& storage, std::span<const RegData> input)
            : target(storage), words(input.begin(), input.end()) {
        }
        void commit() noexcept override {
            const auto per = (target.bits_ + 63) / 64;
            for (std::size_t i = 0; i < target.count_; ++i) {
                WideRegData value;
                for (unsigned w = 0; w < per; ++w)
                    value.set_word(w, words[i * per + w]);
                target.write_wide(i, value);
            }
        }
    };

  public:
    using Construct = void (*)(void*, std::size_t, WideRegData);
    REGFW_DETAIL_NOINLINE NativeStorage(std::size_t count,
                                        std::size_t stride,
                                        std::size_t alignment,
                                        unsigned bits,
                                        unsigned word_bytes,
                                        const void* token,
                                        Construct construct,
                                        WideRegData initial,
                                        WideRegData* (*wide_pointer)(void*) = nullptr)
        : allocation_(count * stride, alignment), count_(count), stride_(stride), bits_(bits),
          word_bytes_(word_bytes), token_(token), wide_pointer_(wide_pointer) {
        if (construct)
            construct(allocation_.data, count, initial);
        else {
            // The aligned operator new call starts the eligible objects'
            // lifetimes; initialize their raw representations in common code.
            std::memset(allocation_.data, 0, count * stride);
            for (std::size_t i = 0; i < count; ++i) {
                if (bits_ <= 64)
                    write_scalar(i, initial.low_u64());
                else
                    write_wide(i, initial);
            }
        }
    }
    std::size_t size() const noexcept override {
        return count_;
    }
    std::size_t object_bytes() const noexcept override {
        return count_ * stride_;
    }
    const void* type_token() const noexcept override {
        return token_;
    }
    void* element(std::size_t i) noexcept override {
        return pointer(i);
    }
    std::byte* scalar_bytes() noexcept override {
        return bits_ <= 64 && std::endian::native == std::endian::little ? pointer(0) : nullptr;
    }
    WideRegData* wide_reference(std::size_t i) noexcept override {
        return wide_pointer_ ? wide_pointer_(element(i)) : nullptr;
    }
    std::unique_ptr<PreparedRestore> prepare_restore(std::span<const RegData> words) override {
        if (words.size() != count_ * ((bits_ + 63) / 64))
            throw Error("typed snapshot size mismatch", ErrorKind::Snapshot);
        return std::make_unique<Restore>(*this, words);
    }
    RegData read_scalar(std::size_t i) const override {
        RegData value = 0;
        const auto bytes = std::min(bits_ / 8, 8u);
        if (bits_ <= 64 && (std::endian::native == std::endian::little || word_bytes_ == bytes))
            return load(pointer(i), bytes);
        for (unsigned off = 0; off < bytes; off += word_bytes_)
            value |= load(pointer(i) + off, word_bytes_) << (off * 8);
        return value;
    }
    void write_scalar(std::size_t i, RegData value) override {
        if (bits_ > 64) {
            write_wide(i, WideRegData(value));
            return;
        }
        if (std::endian::native == std::endian::little || word_bytes_ == bits_ / 8) {
            store(pointer(i), bits_ / 8, value);
            return;
        }
        for (unsigned off = 0; off < bits_ / 8; off += word_bytes_)
            store(pointer(i) + off, word_bytes_, value >> (off * 8));
    }
    WideRegData read_wide(std::size_t i) const override {
        WideRegData value;
        if (std::endian::native == std::endian::little || (word_bytes_ == 8 && bits_ % 64 == 0)) {
            std::memcpy(value.words_.data(), pointer(i), bits_ / 8);
            return value;
        }
        for (unsigned off = 0; off < bits_ / 8; off += word_bytes_) {
            const auto word = off / 8, shift = (off % 8) * 8;
            value.set_word(word, value.word(word) | (load(pointer(i) + off, word_bytes_) << shift));
        }
        return value;
    }
    void write_wide(std::size_t i, WideRegData value) override {
        if (std::endian::native == std::endian::little || (word_bytes_ == 8 && bits_ % 64 == 0)) {
            std::memcpy(pointer(i), value.words_.data(), bits_ / 8);
            return;
        }
        if (bits_ % 64)
            value &= WideRegData::mask(bits_);
        for (unsigned off = 0; off < bits_ / 8; off += word_bytes_)
            store(pointer(i) + off, word_bytes_, value.word(off / 8) >> ((off % 8) * 8));
    }
};
std::unique_ptr<ValueStorage>
make_storage(const StorageType& type, std::size_t count, WideRegData initial) {
    if (type.custom_factory)
        return type.custom_factory(count, initial);
    if (count > SIZE_MAX / type.bytes)
        throw Error("register allocation size overflow", ErrorKind::Configuration);
    return std::make_unique<NativeStorage>(count,
                                           type.bytes,
                                           type.alignment,
                                           type.bits,
                                           type.word_bytes,
                                           type.token,
                                           type.construct,
                                           initial,
                                           type.wide_pointer);
}
} // namespace reg::detail
