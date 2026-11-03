#include "internal/runtime_impl.hpp"

// Snapshot serialization, schema and restore operations.
namespace reg {
namespace {
[[noreturn]] void snapshot_error(std::string message) {
    throw Error(std::move(message), ErrorKind::Snapshot);
}
std::vector<RegData> pack_words(const WideRegData& value, unsigned bits) {
    std::vector<RegData> out;
    out.reserve((bits + 63) / 64);
    for (unsigned i = 0; i < (bits + 63) / 64; ++i)
        out.push_back(value.word(i));
    return out;
}
void check_words(std::span<const RegData> words, unsigned bits) {
    if (words.size() != (bits + 63) / 64)
        snapshot_error("snapshot payload width mismatch");
    if (bits % 64 && (words.back() & ~detail::low_mask(bits % 64)))
        snapshot_error("snapshot payload has bits beyond register width");
}
WideRegData unpack_words(const std::vector<RegData>& words, unsigned bits) {
    check_words(words, bits);
    WideRegData out;
    for (unsigned i = 0; i < words.size(); ++i)
        out.set_word(i, words[i]);
    return out;
}
template <class States>
void check_indices(const States& states, std::size_t count) {
    if (states.size() > count)
        snapshot_error("too many snapshot element states");
    std::optional<std::uint64_t> previous;
    for (const auto& s : states) {
        if (s.element >= count || (previous && s.element <= *previous))
            snapshot_error("snapshot element states must be unique, ascending and in range");
        previous = s.element;
    }
}
} // namespace

SnapshotSchema RegisterMap::snapshot_schema() const {
    SnapshotSchema out;
    for (auto* n : snapshot_nodes()) {
        SnapshotLayoutDesc slot;
        slot.stable_id = n->snapshot.stable_id;
        slot.order = n->snapshot.order;
        slot.start_word = out.total_words;
        slot.len_words = n->snapshot_words();
        slot.wide_width_bits = n->is_wide() ? n->value_size * 8 : 0;
        slot.value_width_bits = n->value_size * 8;
        slot.dimensions.assign(n->dims.begin(), n->dims.end());
        slot.strides.assign(n->layout().strides.begin(), n->layout().strides.end());
        slot.transaction_read = static_cast<std::uint32_t>(n->transaction_policy.read);
        slot.transaction_write = static_cast<std::uint32_t>(n->transaction_policy.write);
        out.total_words += slot.len_words;
        out.slots.push_back(std::move(slot));
    }
    out.layout_hash = snapshot_layout_hash(out.slots);
    return out;
}

std::uint64_t RegisterMap::snapshot_layout_hash(std::span<const SnapshotLayoutDesc> slots) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    auto byte = [&](unsigned char b) {
        h ^= b;
        h *= 0x100000001b3ull;
    };
    auto le = [&](std::uint64_t value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i)
            byte(static_cast<unsigned char>(value >> (i * 8)));
    };
    le(SNAPSHOT_FORMAT_VERSION, 4);
    le(slots.size(), 8);
    for (const auto& s : slots) {
        le(s.stable_id.size(), 8);
        for (unsigned char c : s.stable_id)
            byte(c);
        le(s.order.has_value(), 1);
        le(s.order.value_or(0), 4);
        le(s.start_word, 8);
        le(s.len_words, 8);
        le(s.wide_width_bits, 4);
        le(s.value_width_bits, 4);
        le(s.dimensions.size(), 8);
        for (auto d : s.dimensions)
            le(d, 8);
        le(s.strides.size(), 8);
        for (auto stride : s.strides)
            le(stride, 8);
        le(s.transaction_read, 4);
        le(s.transaction_write, 4);
    }
    return h;
}

VersionedSnapshot RegisterMap::dump_state_versioned() const {
    require_initialized();
    if (dispatching_)
        snapshot_error("cannot checkpoint a map during MMIO dispatch");
    VersionedSnapshot out;
    out.schema = snapshot_schema();
    out.data.resize(out.schema.total_words);
    dump_state(out.data);
    const auto nodes = snapshot_nodes();
    out.runtime.resize(nodes.size());
    for (std::size_t slot = 0; slot < nodes.size(); ++slot) {
        const auto& n = *nodes[slot];
        auto& runtime = out.runtime[slot];
        for (std::size_t i = 0; i < n.transaction_state.size(); ++i) {
            const auto& s = n.transaction_state[i];
            if (!s.has_read_snapshot && !s.write_mask && s.read_source_ok)
                continue;
            SnapshotTransactionState record;
            record.element = i;
            record.has_read_snapshot = s.has_read_snapshot;
            record.read_source_ok = s.read_source_ok;
            if (s.has_read_snapshot)
                record.read_snapshot = pack_words(s.read_snapshot, n.value_size * 8);
            if (s.write_mask) {
                record.write_pending = pack_words(s.write_pending, n.value_size * 8);
                record.write_mask = pack_words(s.write_mask, n.value_size * 8);
            }
            runtime.transactions.push_back(std::move(record));
        }
    }
    return out;
}

void RegisterMap::restore_state_versioned(const VersionedSnapshot& snapshot) {
    require_initialized();
    if (dispatching_)
        snapshot_error("cannot restore a map during MMIO dispatch");
    const auto schema = snapshot_schema();
    if (snapshot.schema != schema)
        snapshot_error("snapshot version/hash/layout mismatch");
    if (snapshot.data.size() != schema.total_words ||
        snapshot.runtime.size() != schema.slots.size())
        snapshot_error("snapshot data/runtime length mismatch");
    const auto nodes = snapshot_nodes();
    struct Prepared {
        std::unique_ptr<detail::PreparedRestore> values;
        std::vector<detail::RegTransactionState> transactions;
    };
    std::vector<Prepared> staged;
    staged.reserve(nodes.size());
    // Validate and allocate everything before touching live values or pending
    // traffic. Swaps and value commits below cannot throw; view references stay valid.
    for (std::size_t slot = 0; slot < nodes.size(); ++slot) {
        auto& n = *nodes[slot];
        n.check_borrow();
        const auto& desc = schema.slots[slot];
        const auto& saved = snapshot.runtime[slot];
        const auto bits = n.value_size * 8, per = (bits + 63) / 64;
        const auto data = std::span(snapshot.data).subspan(desc.start_word, desc.len_words);
        for (std::size_t i = 0; i < n.element_count(); ++i)
            check_words(data.subspan(i * per, per), bits);
        Prepared next;
        next.transactions.resize(n.transaction_state.size());
        check_indices(saved.transactions, next.transactions.size());
        for (const auto& r : saved.transactions) {
            auto& s = next.transactions[r.element];
            if ((r.has_read_snapshot || !r.read_source_ok) &&
                n.transaction_policy.read == SplitReadMode::Independent)
                snapshot_error("read snapshot conflicts with independent policy");
            s.has_read_snapshot = r.has_read_snapshot;
            s.read_source_ok = r.read_source_ok;
            if (r.has_read_snapshot)
                s.read_snapshot = unpack_words(r.read_snapshot, bits);
            else if (!r.read_snapshot.empty())
                snapshot_error("read payload without an active snapshot");
            if (r.write_pending.empty() != r.write_mask.empty())
                snapshot_error("incomplete pending write payload");
            if (!r.write_mask.empty()) {
                if (n.transaction_policy.write == SplitWriteMode::Independent)
                    snapshot_error("write payload conflicts with independent policy");
                s.write_mask = unpack_words(r.write_mask, bits);
                s.write_pending = unpack_words(r.write_pending, bits);
                if (!s.write_mask || bool(s.write_pending & ~s.write_mask))
                    snapshot_error("invalid pending write mask/data");
                for (unsigned offset = 0; offset < bits; offset += 8) {
                    const auto byte = (s.write_mask >> offset).low_u64() & 0xff;
                    if (byte != 0 && byte != 0xff)
                        snapshot_error("pending write mask must cover whole bytes");
                }
            }
        }
        next.values = n.values->prepare_restore(data);
        staged.push_back(std::move(next));
    }
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        staged[i].values->commit();
        nodes[i]->transaction_state.swap(staged[i].transactions);
    }
}
} // namespace reg
