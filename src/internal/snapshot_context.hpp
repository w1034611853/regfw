#pragma once
#include "regfw/types.hpp"

namespace reg::detail {
// The in-memory schema and compact stream use exactly the same layout digest.
class SnapshotLayoutHasher {
    std::uint64_t hash_ = 0xcbf29ce484222325ull;
    void byte(unsigned char value) {
        hash_ = (hash_ ^ value) * 0x100000001b3ull;
    }
    void integer(std::uint64_t value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i)
            byte(static_cast<unsigned char>(value >> (i * 8)));
    }

  public:
    explicit SnapshotLayoutHasher(std::size_t slots) {
        integer(SNAPSHOT_FORMAT_VERSION, 4);
        integer(slots, 8);
    }
    template <class Dimensions, class Strides>
    void slot(std::string_view id,
              std::optional<std::uint32_t> order,
              std::size_t start,
              std::size_t words,
              unsigned wide_bits,
              unsigned bits,
              const Dimensions& dimensions,
              const Strides& strides,
              std::uint32_t read,
              std::uint32_t write) {
        integer(id.size(), 8);
        for (unsigned char c : id)
            byte(c);
        integer(order.has_value(), 1);
        integer(order.value_or(0), 4);
        integer(start, 8);
        integer(words, 8);
        integer(wide_bits, 4);
        integer(bits, 4);
        integer(dimensions.size(), 8);
        for (auto d : dimensions)
            integer(d, 8);
        integer(strides.size(), 8);
        for (auto stride : strides)
            integer(stride, 8);
        integer(read, 4);
        integer(write, 4);
    }
    std::uint64_t value() const {
        return hash_;
    }
};
} // namespace reg::detail
