#include "internal/runtime_impl.hpp"
#include "internal/snapshot_context.hpp"

namespace reg {
namespace {
constexpr RegData compact_magic = 0x5245474657434d50ull; // REGFWCMP
constexpr RegData compact_version = 1;
constexpr std::size_t header_bytes = 6 * sizeof(RegData);
[[noreturn]] void invalid(std::string message) {
    throw Error(std::move(message), ErrorKind::Snapshot);
}
void add_bytes(std::size_t& total, std::size_t count, std::size_t width) {
    if (count > (std::numeric_limits<std::size_t>::max() - total) / width)
        invalid("compact snapshot size overflow");
    total += count * width;
}
std::size_t word_count(std::size_t bytes) {
    return bytes / 8 + (bytes % 8 != 0);
}
struct Writer {
    std::vector<RegData> words;
    std::size_t position{};
    explicit Writer(std::size_t bytes) : words(word_count(bytes)) {
    }
    void put(RegData value, unsigned bytes = 8) {
        const auto lane = unsigned(position % 8);
        const auto index = position / 8;
        value &= detail::low_mask(bytes * 8);
        words[index] |= value << (lane * 8);
        if (bytes > 8 - lane)
            words[index + 1] |= value >> ((8 - lane) * 8);
        position += bytes;
    }
    void wide(const WideRegData& value, unsigned bytes) {
        for (unsigned offset = 0; offset < bytes; offset += 8)
            put(value.word(offset / 8), std::min(8u, bytes - offset));
    }
};
struct Reader {
    std::span<const RegData> words;
    std::size_t position{}, limit;
    explicit Reader(std::span<const RegData> data) : words(data), limit(data.size_bytes()) {
    }
    RegData get(unsigned bytes = 8) {
        if (bytes > limit - position)
            invalid("truncated compact snapshot");
        const auto lane = unsigned(position % 8);
        const auto index = position / 8;
        auto value = words[index] >> (lane * 8);
        if (bytes > 8 - lane)
            value |= words[index + 1] << ((8 - lane) * 8);
        position += bytes;
        return value & detail::low_mask(bytes * 8);
    }
    std::vector<RegData> wide(unsigned bytes) {
        std::vector<RegData> out;
        out.reserve(word_count(bytes));
        for (unsigned offset = 0; offset < bytes; offset += 8)
            out.push_back(get(std::min(8u, bytes - offset)));
        return out;
    }
};
std::uint64_t layout_hash(std::span<detail::EntityState* const> nodes) {
    detail::SnapshotLayoutHasher hash(nodes.size());
    std::size_t start = 0;
    for (const auto* n : nodes) {
        hash.slot(n->snapshot.stable_id,
                  n->snapshot.order,
                  start,
                  n->snapshot_words(),
                  n->is_wide() ? n->value_size * 8 : 0,
                  n->value_size * 8,
                  n->dims,
                  n->layout().strides,
                  static_cast<std::uint32_t>(n->transaction_policy.read),
                  static_cast<std::uint32_t>(n->transaction_policy.write));
        start += n->snapshot_words();
    }
    return hash.value();
}
unsigned transaction_flags(const detail::RegTransactionState& state) {
    return unsigned(state.has_read_snapshot) | (unsigned(!state.read_source_ok) << 1) |
           (unsigned(bool(state.write_mask)) << 2);
}
} // namespace

std::size_t RegisterMap::max_compact_snapshot_words() const {
    require_initialized();
    std::size_t bytes = header_bytes;
    for (const auto& n : nodes_)
        if (n->snapshot.kind == SnapshotKind::Migratable) {
            add_bytes(bytes, n->element_count(), n->value_size);
            add_bytes(bytes, n->transaction_state.size(), 24 + 3 * n->value_size);
        }
    return word_count(bytes);
}

std::vector<RegData> RegisterMap::dump_state_compact() const {
    const auto nodes = snapshot_nodes();
    std::size_t value_bytes = 0, transaction_bytes = 0;
    std::uint64_t records = 0;
    for (const auto* n : nodes) {
        add_bytes(value_bytes, n->element_count(), n->value_size);
        for (const auto& state : n->transaction_state) {
            const auto flags = transaction_flags(state);
            if (!flags)
                continue;
            ++records;
            add_bytes(transaction_bytes, 1, 24);
            add_bytes(transaction_bytes, (flags & 1 ? 1 : 0) + (flags & 4 ? 2 : 0), n->value_size);
        }
    }
    auto bytes = header_bytes;
    add_bytes(bytes, value_bytes, 1);
    add_bytes(bytes, transaction_bytes, 1);
    Writer out(bytes);
    out.put(compact_magic);
    out.put(compact_version);
    out.put(layout_hash(nodes));
    out.put(bytes);
    out.put(value_bytes);
    out.put(records);
    for (const auto* n : nodes) {
        const auto count = n->element_count();
        for (std::size_t i = 0; i < count; ++i) {
            if (n->is_wide())
                out.wide(n->read_wide(i), n->value_size);
            else
                out.put(n->read_scalar(i), n->value_size);
        }
    }
    for (std::size_t slot = 0; slot < nodes.size(); ++slot) {
        const auto& n = *nodes[slot];
        for (std::size_t element = 0; element < n.transaction_state.size(); ++element) {
            const auto& state = n.transaction_state[element];
            const auto flags = transaction_flags(state);
            if (!flags)
                continue;
            out.put(slot);
            out.put(element);
            out.put(flags);
            if (flags & 1)
                out.wide(state.read_snapshot, n.value_size);
            if (flags & 4) {
                out.wide(state.write_pending, n.value_size);
                out.wide(state.write_mask, n.value_size);
            }
        }
    }
    return std::move(out.words);
}

void RegisterMap::restore_state_compact(std::span<const RegData> payload) {
    if (payload.size() < 6 || payload.size() > max_compact_snapshot_words())
        invalid("invalid compact register snapshot length");
    Reader in(payload);
    if (in.get() != compact_magic || in.get() != compact_version)
        invalid("invalid compact register snapshot header/version");
    const auto nodes = snapshot_nodes();
    if (in.get() != layout_hash(nodes))
        invalid("compact snapshot layout hash mismatch");
    const auto bytes = in.get(), value_bytes = in.get(), records = in.get();
    if (bytes < header_bytes || bytes > payload.size_bytes() || word_count(bytes) != payload.size())
        invalid("invalid compact snapshot byte length");
    if (bytes % 8 && (payload.back() >> ((bytes % 8) * 8)))
        invalid("nonzero compact snapshot padding");
    in.limit = static_cast<std::size_t>(bytes);
    std::size_t expected_bytes = 0, expected_words = 0;
    for (const auto* n : nodes) {
        add_bytes(expected_bytes, n->element_count(), n->value_size);
        expected_words += n->snapshot_words();
    }
    if (value_bytes != expected_bytes || value_bytes > in.limit - in.position)
        invalid("compact snapshot value length mismatch");
    std::vector<RegData> values(expected_words);
    auto* next = values.data();
    for (const auto* n : nodes) {
        const auto count = n->element_count();
        const auto width = n->value_size;
        for (std::size_t i = 0; i < count; ++i)
            for (unsigned offset = 0; offset < width; offset += 8)
                *next++ = in.get(std::min(8u, width - offset));
    }
    if (records > (in.limit - in.position) / 24)
        invalid("invalid compact snapshot transaction count");
    std::vector<SnapshotEntityRuntime> runtime(nodes.size());
    std::optional<std::pair<std::uint64_t, std::uint64_t>> previous;
    for (std::uint64_t i = 0; i < records; ++i) {
        const auto slot = in.get(), element = in.get(), flags = in.get();
        const auto key = std::pair{slot, element};
        if (slot >= nodes.size() || element >= nodes[slot]->transaction_state.size() ||
            (previous && key <= *previous) || !flags || (flags & ~7ull))
            invalid("invalid compact snapshot transaction record");
        previous = key;
        SnapshotTransactionState state;
        state.element = element;
        state.has_read_snapshot = flags & 1;
        state.read_source_ok = !(flags & 2);
        const auto width = nodes[slot]->value_size;
        if (flags & 1)
            state.read_snapshot = in.wide(width);
        if (flags & 4) {
            state.write_pending = in.wide(width);
            state.write_mask = in.wide(width);
        }
        runtime[slot].transactions.push_back(std::move(state));
    }
    if (in.position != in.limit)
        invalid("extra bytes in compact snapshot");
    restore_snapshot_payload(nodes, values, runtime);
}
} // namespace reg
