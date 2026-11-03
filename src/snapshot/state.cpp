#include "internal/runtime_impl.hpp"
#include <sstream>
#include <iomanip>
#include <unordered_set>

namespace reg {

std::vector<RegisterMap::Node*> RegisterMap::snapshot_nodes() const {
    require_initialized();
    std::vector<Node*> out;
    std::unordered_set<std::string> ids;
    std::unordered_set<std::uint32_t> orders;
    for (auto& p : nodes_)
        if (p->snapshot.kind == SnapshotKind::Migratable) {
            if (p->snapshot.stable_id.empty() || !ids.insert(p->snapshot.stable_id).second ||
                (p->snapshot.order && !orders.insert(*p->snapshot.order).second))
                throw Error("duplicate/empty snapshot id or order", ErrorKind::Snapshot);
            out.push_back(p);
        }
    std::sort(out.begin(), out.end(), [](auto a, auto b) {
        auto& x = a->snapshot;
        auto& y = b->snapshot;
        if (x.order && y.order)
            return *x.order != *y.order ? *x.order < *y.order : x.stable_id < y.stable_id;
        if (x.order || y.order)
            return bool(x.order);
        return x.stable_id < y.stable_id;
    });
    return out;
}

std::size_t RegisterMap::reset() {
    require_initialized();
    for (auto& n : nodes_)
        n->reset();
    return nodes_.size();
}

std::size_t RegisterMap::reset_matching(ResetRequest request) {
    require_initialized();
    std::size_t count = 0;
    for (auto& n : nodes_)
        if (detail::contains(n->reset_domains, request.domain()) &&
            request.matches_scope(n->reset_scope)) {
            n->reset();
            ++count;
        }
    return count;
}

void RegisterMap::reset_all() {
    (void)reset();
}

std::size_t RegisterMap::total_snapshot_data_count() const {
    return snapshot_schema().total_words;
}

std::size_t RegisterMap::total_node_data_count() const {
    require_initialized();
    std::size_t total = 0;
    for (auto& n : nodes_)
        total += n->snapshot_words();
    return total;
}

std::size_t RegisterMap::dump_state(std::span<RegData> data) const {
    auto nodes = snapshot_nodes();
    std::size_t count = 0;
    for (auto n : nodes)
        count += n->snapshot_words();
    if (data.size() < count)
        throw Error("snapshot buffer too small", ErrorKind::Snapshot);
    std::size_t at = 0;
    for (auto n : nodes) {
        n->check_borrow();
        for (std::size_t i = 0; i < n->element_count(); ++i) {
            if (n->is_wide()) {
                auto value = n->read_wide(i);
                for (unsigned w = 0; w < (n->value_size + 7) / 8; ++w)
                    data[at++] = value.word(w);
            } else
                data[at++] = n->read_scalar(i);
        }
    }
    return at;
}

void RegisterMap::restore_state(std::span<const RegData> data) {
    auto nodes = snapshot_nodes();
    std::size_t count = 0;
    for (auto n : nodes)
        count += n->snapshot_words();
    if (data.size() != count)
        throw Error("snapshot size mismatch", ErrorKind::Snapshot);
    for (auto n : nodes)
        n->check_borrow();
    std::size_t at = 0;
    std::vector<std::unique_ptr<detail::PreparedRestore>> staged;
    staged.reserve(nodes.size());
    for (auto n : nodes) {
        auto words = n->snapshot_words();
        staged.push_back(n->values->prepare_restore(data.subspan(at, words)));
        at += words;
    }
    // Convert every value before modifying any object; references remain valid.
    for (auto& batch : staged)
        batch->commit();
    for (auto n : nodes) {
        std::fill(n->transaction_state.begin(),
                  n->transaction_state.end(),
                  detail::RegTransactionState{});
    }
}

} // namespace reg
