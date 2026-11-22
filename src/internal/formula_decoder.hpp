#pragma once
#include "access_context.hpp"

namespace reg {
namespace detail {
// Shared prepared-access decoder: View range + one immutable EntityLayout
// dense table or hash + an instance-bound context. No per-access caches.
class FormulaDecoder {
  public:
    enum class Path : std::uint8_t { Generic, Scalar, Mapped };
    static constexpr std::size_t max_redirect_targets = 64;
    struct Scope {
        std::uint32_t parent = STATIC_NONE;
        std::uint64_t base{}, size{};
        std::int32_t priority{}, outer_priority{};
        std::optional<std::uint32_t> instance;
        AccessRouteKind route{AccessRouteKind::Direct};
        AliasAccessPolicy policy; // effective down the chain
        const ViewPreAccessChain* pre_access_chain{};
        std::uint32_t identity = STATIC_NONE; // index into families below
        bool preserve_identity_view{};
        std::string_view name;
    };
    struct Mount {
        std::uint64_t base{}, end{};
        std::uint32_t binding{}, scope{}, array{};
    };
    struct Reserve {
        std::uint64_t base = 0, end = 0;
        std::uint32_t scope = 0;
    };
    // Node traits snapshotted at finalize(); the hot path must not chase the
    // node pointer just to select an execution path. Only immutable-after-
    // finish fields live here. The storage and logger state remain live.
    struct Binding {
        EntityState* node{};
        std::uint64_t canonical_base{};
        std::uint32_t linear{};
        std::uint8_t scalar32_directions{};
        bool has_derivations{}, single_index{}, single_metadata_only{};
    };

  private:
    RegisterMap* owner_{};
    std::uint32_t identity_base_{}; // offset into owner_->identities_
    std::vector<Scope> scopes_;
    std::vector<Mount> mounts_; // base-sorted
    std::vector<Binding> bindings_;
    std::vector<FormulaArraySpec> arrays_;
    // Owning storage for dynamically generated Table-kind record tables.
    // Static declaration shapes hold their own once-initialized records.
    std::vector<std::vector<ElementRecord>> record_storage_;
    std::vector<Reserve> reserves_; // base-sorted, highest priority wins
    // Prepared shared EntityLayout. Each local 4-byte address maps directly
    // to an element ordinal and immutable access metadata. Coordinates remain
    // in the entity layout and are borrowed only when the access needs them.
    // VF aliases share the geometry map, not callback/storage instances.
    // Shared, immutable register-location metadata. An index has a dynamic
    // rank (1..MAX_REG_DIMS), not hard-coded index0/index1. Callback and
    // storage objects belong to a *mounted instance*, never to this layout.
    struct PreparedAccessInfo {
        std::uint32_t ordinal{STATIC_NONE}, linear{}, byte{}, available{};
    };
    static_assert(sizeof(PreparedAccessInfo) == 16);
    struct EntityHashLayout {
        std::unordered_map<std::uint64_t, PreparedAccessInfo> words;
        bool has_priority_aliases{};
        // Compact spans use direct indexing; sparse/high addresses keep the
        // hash. This table is built once at installation, never by consteval.
        std::vector<PreparedAccessInfo> dense;
        std::uint64_t dense_base{};
        std::size_t key_count{};
        const PreparedAccessInfo* find(std::uint64_t key) const noexcept {
            if (!dense.empty()) {
                const auto index = (key - dense_base) >> 2;
                if (index >= dense.size())
                    return nullptr;
                const auto& access = dense[index];
                return access.ordinal == STATIC_NONE ? nullptr : &access;
            }
            const auto it = words.find(key);
            return it == words.end() ? nullptr : &it->second;
        }
        void compact() {
            key_count = words.size();
            if (words.empty())
                return;
            std::uint64_t first = UINT64_MAX, last = 0;
            for (const auto& [key, access] : words) {
                (void)access;
                first = std::min(first, key);
                last = std::max(last, key);
            }
            const auto slots = ((last - first) >> 2) + 1;
            if (slots / 2 + slots % 2 > key_count)
                return;
            if (slots > dense.max_size())
                throw Error("dense entity lookup size overflow", ErrorKind::Configuration);
            dense_base = first;
            dense.resize(static_cast<std::size_t>(slots));
            for (const auto& [key, access] : words)
                dense[(key - first) >> 2] = access;
            words.clear();
            words.rehash(0);
        }
    };
    // Bound once per mount. These stable references account for hooks,
    // per-instance storage, route, identity and access policy.
    struct PreparedMountContext : ScalarAccessSite {
        const Mount* mount{};
        const Binding* binding{};
        EntityState* node{};
        const RegViewLayout* node_layout{};
        const ViewPreAccessChain* pre_access_chain{};
        std::uint32_t identity_index{STATIC_NONE};
        std::int32_t priority{};
        Path read_path{}, write_path{};
        // A fast identity mount lies entirely within one window instance.
        std::uint32_t request_id{};
        std::uint64_t mount_inner{};
        // The node owns live callback slots, transaction/snapshot state, and
        // storage. No function pointer or mutable state is snapshotted here.
    };
    struct EntityView {
        std::uint64_t base{}, end{};
        std::uint32_t scope{};
        std::vector<std::uint32_t> mounts; // mount ordinal -> per-instance binding
        std::vector<PreparedMountContext> prepared;
        std::shared_ptr<const EntityHashLayout> layout;
        std::vector<const EntityView*> identity_targets;
        std::size_t reserve_begin{}, reserve_count{};
    };

  public:
    struct PreparedHit {
        const EntityView* view{};
        const PreparedMountContext* context{};
        const PreparedAccessInfo* access{};
        PreparedHit redirected(std::uint32_t id) const {
            const auto* target = view->identity_targets[id];
            return {target, &target->prepared[access->ordinal], access};
        }
        std::uint64_t canonical(std::uint64_t bus_address) const {
            return context->target + (bus_address - context->mount->base);
        }
    };

  private:
    std::vector<EntityView> entity_views_;
    // Compact search keys and overlap metadata, parallel to sorted entity_views_.
    // Reserve slices contain indices into the immutable global reserve list.
    struct ViewMeta {
        std::uint64_t end{}, prefix_max_end{};
        bool isolated{};
    };
    std::vector<std::uint64_t> view_bases_;
    std::vector<ViewMeta> view_metas_;
    std::vector<std::size_t> view_reserve_indices_;
    std::size_t entity_layout_count_{}, entity_hash_bytes_{};
    bool same_array_shape(std::uint32_t a_id, std::uint32_t b_id) const {
        const auto& a = arrays_[a_id];
        const auto& b = arrays_[b_id];
        if (a.count != b.count || a.occupied != b.occupied || a.kind != b.kind ||
            a.rank != b.rank || a.span != b.span || a.strides_decl != b.strides_decl ||
            a.extents_decl != b.extents_decl)
            return false;
        if (a.kind == FormulaDecodeKind::Table) {
            if (a.sparse.size() != b.sparse.size())
                return false;
            for (std::size_t i = 0; i < a.sparse.size(); ++i)
                if (a.sparse[i].offset != b.sparse[i].offset ||
                    a.sparse[i].linear != b.sparse[i].linear)
                    return false;
        }
        return true;
    }
    bool same_entity_geometry(const EntityView& a, const EntityView& b) const {
        if (a.mounts.size() != b.mounts.size())
            return false;
        for (std::size_t i = 0; i < a.mounts.size(); ++i) {
            const auto& x = mounts_[a.mounts[i]];
            const auto& y = mounts_[b.mounts[i]];
            const auto& xb = bindings_[x.binding];
            const auto& yb = bindings_[y.binding];
            if (x.base - a.base != y.base - b.base || x.end - x.base != y.end - y.base ||
                xb.linear != yb.linear || !same_array_shape(x.array, y.array) ||
                xb.node->dims != yb.node->dims)
                return false;
        }
        return true;
    }
    std::uint64_t entity_geometry_hash(const EntityView& view) const {
        std::uint64_t hash = 0xcbf29ce484222325ULL;
        const auto mix = [&](std::uint64_t value) {
            hash ^= value;
            hash *= 0x100000001b3ULL;
        };
        mix(view.mounts.size());
        for (const auto id : view.mounts) {
            const auto& mount = mounts_[id];
            const auto& binding = bindings_[mount.binding];
            const auto& array = arrays_[mount.array];
            mix(mount.base - view.base);
            mix(mount.end - mount.base);
            mix(binding.linear);
            mix(array.count);
            mix(array.occupied);
            mix(static_cast<std::uint64_t>(array.kind));
            mix(array.rank);
            mix(array.span);
            for (const auto stride : array.strides_decl)
                mix(stride);
            for (const auto extent : array.extents_decl)
                mix(extent);
            if (array.kind == FormulaDecodeKind::Table) {
                mix(array.sparse.size());
                for (const auto& element : array.sparse) {
                    mix(element.offset);
                    mix(element.linear);
                }
            }
            mix(binding.node->dims.size());
            for (const auto dim : binding.node->dims)
                mix(dim);
        }
        return hash;
    }
    void finish_entity_hash(bool implicit_root) {
        // One EntityView per actual mapping scope (not per full AddressSpace
        // View). The key is an entity-local offset, so aliases use identical
        // hashes and carry only their own binding/mount ID array.
        // Prefer a leaf Entity bundle when it has enough keys to amortize
        // range selection. Small one-REG repeat leaves are grouped under the
        // nearest sufficiently populated parent View, avoiding thousands of
        // separate one-element hash tables and range probes.
        constexpr std::size_t min_view_keys = 64;
        std::vector<std::size_t> scope_words(scopes_.size());
        for (const auto& m : mounts_) {
            const auto& a = arrays_[m.array];
            const auto count = std::size_t(a.count) * (a.occupied / 4);
            for (auto p = m.scope; p != STATIC_NONE; p = scopes_[p].parent)
                scope_words[p] += count;
        }
        std::unordered_map<std::uint32_t, std::uint32_t> scopes_to_view;
        for (std::uint32_t id = 0; id < mounts_.size(); ++id) {
            const auto& m = mounts_[id];
            auto group = m.scope;
            // A small VF still needs its own binding table so a redirected
            // coordinate selects the matching instance without decoding again.
            while (scopes_[group].parent != STATIC_NONE && scope_words[group] < min_view_keys &&
                   (!scopes_[group].preserve_identity_view ||
                    scopes_[scopes_[group].parent].identity == scopes_[group].identity))
                group = scopes_[group].parent;
            auto [it, created] =
                scopes_to_view.try_emplace(group, static_cast<std::uint32_t>(entity_views_.size()));
            if (created) {
                const auto& sc = scopes_[group];
                if (sc.size > UINT64_MAX - sc.base)
                    throw Error("entity hash: view range overflow", ErrorKind::Configuration);
                entity_views_.push_back({sc.base, sc.base + sc.size, group, {}, {}, {}, {}});
            }
            entity_views_[it->second].mounts.push_back(id);
        }
        for (auto& view : entity_views_) {
            // The lookup interval need only cover mounted registers, not the
            // entire parent View aperture (which may include other VF views).
            view.base = mounts_[view.mounts.front()].base;
            view.end = view.base;
            for (const auto id : view.mounts)
                view.end = std::max(view.end, mounts_[id].end);
        }
        std::unordered_map<std::uint64_t, std::vector<std::size_t>> geometry_buckets;
        for (auto& view : entity_views_) {
            // The mount vector is base-sorted, so the same Bundle declaration
            // produces matching ordinals under different mount bases.
            auto& candidates = geometry_buckets[entity_geometry_hash(view)];
            for (const auto id : candidates) {
                const auto& earlier = entity_views_[id];
                if (same_entity_geometry(earlier, view)) {
                    view.layout = earlier.layout;
                    break;
                }
            }
            if (view.layout)
                continue;
            auto layout = std::make_shared<EntityHashLayout>();
            std::size_t count = 0;
            for (auto id : view.mounts) {
                const auto& spec = arrays_[mounts_[id].array];
                if (spec.count > SIZE_MAX / (spec.occupied / 4))
                    throw Error("entity hash: size overflow", ErrorKind::Configuration);
                count += std::size_t(spec.count) * (spec.occupied / 4);
            }
            auto& words = layout->words;
            words.reserve(count);
            for (std::uint32_t ordinal = 0; ordinal < view.mounts.size(); ++ordinal) {
                const auto& m = mounts_[view.mounts[ordinal]];
                const auto& arr = arrays_[m.array];
                for (std::uint32_t slot = 0; slot < arr.count; ++slot) {
                    const auto off = formula_element_offset(arr, slot);
                    if (off == UINT64_MAX)
                        throw Error("entity hash: bad element offset", ErrorKind::Configuration);
                    const auto linear =
                        arr.kind == FormulaDecodeKind::Table ? arr.sparse[slot].linear : slot;
                    for (std::uint32_t byte = 0; byte < arr.occupied; byte += 4) {
                        if (off > UINT64_MAX - byte || m.base - view.base > UINT64_MAX - off - byte)
                            throw Error("entity hash: word address overflow",
                                        ErrorKind::Configuration);
                        const auto key = m.base - view.base + off + byte;
                        const auto& binding = bindings_[m.binding];
                        const auto actual_linear = std::size_t(binding.linear) + linear;
                        if (actual_linear >= binding.node->layout().indices.size())
                            throw Error("prepared access: invalid array element",
                                        ErrorKind::Configuration);
                        PreparedAccessInfo access{};
                        access.ordinal = ordinal;
                        access.linear = linear;
                        access.byte = byte;
                        access.available = arr.occupied - byte;
                        const auto [it, inserted] = words.try_emplace(key, access);
                        if (!inserted) {
                            layout->has_priority_aliases = true;
                            const auto& previous = mounts_[view.mounts[it->second.ordinal]];
                            // Preserve the original scope winner inside a
                            // composite View. Legality is checked by the
                            // existing full overlap validator (not this map).
                            if (compare(m.scope, previous.scope) > 0)
                                it->second = access;
                        }
                    }
                }
            }
            layout->compact();
            entity_hash_bytes_ += layout->dense.capacity() * sizeof(PreparedAccessInfo);
            entity_hash_bytes_ +=
                layout->words.size() *
                    (sizeof(std::uint64_t) + sizeof(PreparedAccessInfo) + sizeof(void*) * 2) +
                layout->words.bucket_count() * sizeof(void*);
            view.layout = std::move(layout);
            // Only distinct, shareable representatives enter the temporary
            // buckets. Fingerprint matches still require exact geometry equality.
            if (!view.layout->has_priority_aliases)
                candidates.push_back(&view - entity_views_.data());
            ++entity_layout_count_;
        }
        for (auto& view : entity_views_) {
            view.prepared.reserve(view.mounts.size());
            for (const auto id : view.mounts) {
                const auto& mount = mounts_[id];
                const auto& binding = bindings_[mount.binding];
                const auto& scope = scopes_[mount.scope];
                const auto identity_index =
                    scope.identity == STATIC_NONE ? STATIC_NONE : identity_base_ + scope.identity;
                const auto instance = implicit_root ? binding.node->instance : scope.instance;
                view.prepared.push_back({{binding.canonical_base,
                                          AccessRoute{scope.route, scope.instance.value_or(0)},
                                          scope.policy,
                                          instance,
                                          scope.name,
                                          binding.canonical_base != mount.base},
                                         &mount,
                                         &binding,
                                         binding.node,
                                         &binding.node->layout(),
                                         scope.pre_access_chain,
                                         identity_index,
                                         scope.outer_priority});
            }
        }
        std::sort(entity_views_.begin(), entity_views_.end(), [](const auto& a, const auto& b) {
            return a.base < b.base || (a.base == b.base && a.scope < b.scope);
        });
        view_bases_.reserve(entity_views_.size());
        view_metas_.reserve(entity_views_.size());
        std::uint64_t max_end = 0;
        for (auto& view : entity_views_) {
            max_end = std::max(max_end, view.end);
            view_bases_.push_back(view.base);
            view_metas_.push_back({view.end, max_end});
            view.reserve_begin = view_reserve_indices_.size();
            for (std::size_t r = 0; r < reserves_.size(); ++r) {
                if (reserves_[r].base >= view.end)
                    break;
                if (reserves_[r].end > view.base)
                    view_reserve_indices_.push_back(r);
            }
            view.reserve_count = view_reserve_indices_.size() - view.reserve_begin;
        }
        for (std::size_t i = 0; i < entity_views_.size(); ++i)
            view_metas_[i].isolated =
                (i == 0 || view_metas_[i - 1].prefix_max_end <= view_bases_[i]) &&
                (i + 1 == entity_views_.size() || view_bases_[i + 1] >= view_metas_[i].end);
        if (std::getenv("REGFW_ENTITY_HASH_STATS")) {
            std::size_t keys = 0;
            for (const auto& v : entity_views_)
                if (v.layout)
                    keys += v.layout->key_count;
            std::fprintf(stderr,
                         "ENTITY_HASH_STATS views=%zu unique_layouts=%zu total_view_keys=%zu "
                         "approx_allocated_bytes=%zu\n",
                         entity_views_.size(),
                         entity_layout_count_,
                         keys,
                         entity_hash_bytes_);
        }
    }
    void prepare_identity_targets() {
        for (auto& view : entity_views_) {
            if (view.prepared.empty())
                continue;
            const auto identity = view.prepared.front().identity_index;
            if (identity == STATIC_NONE ||
                std::any_of(view.prepared.begin(), view.prepared.end(), [&](const auto& c) {
                    return c.identity_index != identity;
                }))
                continue;
            const auto& window = owner_->identities_[identity];
            if (window.mode != IdentityRemapMode::RedirectBank ||
                window.count > max_redirect_targets || !window.stride || view.base < window.base)
                continue;
            const auto inner = (view.base - window.base) % window.stride;
            const auto span = view.end - view.base;
            if (span > window.stride - inner)
                continue;
            for (std::size_t id = 0; id < window.count; ++id) {
                const auto base = window.base + id * window.stride + inner;
                const auto it = std::lower_bound(view_bases_.begin(), view_bases_.end(), base);
                if (it == view_bases_.end() || *it != base)
                    continue;
                const auto index = static_cast<std::size_t>(it - view_bases_.begin());
                const auto& candidate = entity_views_[index];
                const auto& meta = view_metas_[index];
                if (meta.end - base != span || candidate.layout != view.layout || !meta.isolated ||
                    candidate.reserve_count)
                    continue;
                if (view.identity_targets.empty())
                    view.identity_targets.resize(window.count);
                view.identity_targets[id] = &candidate;
            }
        }
        if (std::getenv("REGFW_ENTITY_HASH_STATS")) {
            std::size_t slots = 0, usable = 0;
            for (const auto& view : entity_views_) {
                slots += view.identity_targets.size();
                for (const auto* target : view.identity_targets)
                    usable += target != nullptr;
            }
            std::fprintf(stderr,
                         "ENTITY_REDIRECT_STATS slots=%zu usable=%zu bytes=%zu\n",
                         slots,
                         usable,
                         slots * sizeof(const EntityView*));
        }
    }
    bool lookup_prepared(std::uint64_t word, PreparedHit& hit) const noexcept {
        auto in_view = [&](const EntityView& view) {
            const auto* found = view.layout->find(word - view.base);
            if (!found)
                return false;
            hit = {&view, &view.prepared[found->ordinal], found};
            return true;
        };
        if (entity_views_.size() == 1) {
            const auto& view = entity_views_.front();
            return word >= view.base && word < view.end && in_view(view);
        }
        std::size_t low = 0, high = view_bases_.size();
        while (low < high) {
            const auto mid = low + (high - low) / 2;
            if (view_bases_[mid] <= word)
                low = mid + 1;
            else
                high = mid;
        }
        if (low == 0)
            return false;
        const auto top = low - 1;
        if (view_metas_[top].isolated) {
            if (word >= view_metas_[top].end)
                return false;
            return in_view(entity_views_[top]);
        }
        const Mount* winner = nullptr;
        while (low) {
            const auto index = --low;
            const auto& meta = view_metas_[index];
            if (meta.prefix_max_end <= word)
                break;
            if (word >= meta.end)
                continue;
            const auto& view = entity_views_[index];
            const auto* found = view.layout->find(word - view_bases_[index]);
            if (!found)
                continue;
            const auto& access = *found;
            const auto& bound = view.prepared[access.ordinal];
            if (!winner || compare(bound.mount->scope, winner->scope) > 0) {
                winner = bound.mount;
                hit = {&view, &bound, &access};
            }
        }
        return winner != nullptr;
    }

  public:
    bool find_prepared(std::uint64_t word, PreparedHit& hit) const noexcept {
        if (!lookup_prepared(word, hit))
            return false;
        // Only reserves intersecting the winning View can mask its occupied REG.
        const auto& view = *hit.view;
        const auto end = view.reserve_begin + view.reserve_count;
        for (auto i = view.reserve_begin; i < end; ++i) {
            const auto& r = reserves_[view_reserve_indices_[i]];
            if (word < r.base)
                break;
            if (word >= r.end)
                continue;
            if (compare(r.scope, hit.context->mount->scope) >= 0)
                return false;
        }
        return true;
    }
    // Formula geometry is immutable after installation; address lookup has no mutable caches.
    FormulaDecoder() = default;

    // One prepared Hash hit supplies the REG address, all coordinates, and a
    // per-View binding context. Construct only the *per-request* AliasEntry;
    // policy, pre-access, route, instance and callback-slot ownership were
    // already bound when the View was installed.
    void
    fill_prepared(const PreparedHit& hit, std::uint64_t word, RegisterMap::AliasEntry& out) const {
        const auto& info = *hit.access;
        const auto& bound = *hit.context;
        const auto& mount = *bound.mount;
        const auto& binding = *bound.binding;
        out.target = binding.canonical_base + (word - mount.base);
        out.used = out.target != word;
        out.available = info.available;
        if (out.available > 4) {
            // View overlap/reserve can interrupt an otherwise continuous
            // wide register. Validate only this rare, multi-word case.
            PreparedHit next{};
            if (!find_prepared(word + 4, next) || next.context->mount != &mount ||
                next.access->linear != info.linear || next.access->byte != info.byte + 4)
                out.available = 4;
        }
        out.route = bound.route;
        out.policy = bound.policy;
        out.instance = bound.instance;
        out.view_name = bound.view_name;
        out.pre_access_chain = bound.pre_access_chain;
        out.priority = bound.priority;
        if (bound.identity_index != STATIC_NONE)
            out.identity_index = bound.identity_index;
        else
            out.identity_index.reset();
        out.target_entry = {bound.node, binding.linear + info.linear, info.byte, bound.node_layout};
    }
    // Dynamic (AddressSpace/RegView) layouts use the same decoder as compiled
    // static layouts. The compiler emits one pattern mount per mapped REG
    // declaration segment; Table-kind record storage must be transferred with
    // the arrays (the spans inside FormulaArraySpec point into it).
    void install_dynamic(RegisterMap* map,
                         std::uint32_t identity_base,
                         std::vector<Scope> scopes,
                         std::vector<Mount> mounts,
                         std::vector<Binding> bindings,
                         std::vector<FormulaArraySpec> arrays,
                         std::vector<std::vector<ElementRecord>> record_storage,
                         std::vector<Reserve> reserves,
                         std::vector<LayoutWarning>* warnings,
                         bool implicit_root = false);
    void finish_dispatch_tables(bool implicit_root = false) {
        finish_entity_hash(implicit_root);
    }
    // Resolve identity/single-index fast-path eligibility once at finalize().
    // The per-access path used to decode bank 0 (or the source element) on
    // every MMIO beat; geometry and node traits cannot change after finish.
    // Binding-level execution traits are snapshotted in the same pass: the
    // hot path then selects its route without chasing the node pointer.
    void prepare_fast_paths() {
        prepare_identity_targets();
        for (auto& b : bindings_) {
            const auto& n = *b.node;
            b.scalar32_directions = n.scalar32_directions;
            b.has_derivations = !n.derivations.empty();
            b.single_index = bool(n.single_index);
            b.single_metadata_only = b.single_index && n.single_index->metadata_only;
        }
        // A resolver may select any VF on any beat. Intersect fast directions for
        // every possible winner, including behavior, width and split policy.
        // Overlapping views, reserves and heterogeneous layouts keep full decode.
        auto eligible = [&](const PreparedMountContext& target) -> unsigned {
            const auto& b = *target.binding;
            if (arrays_[target.mount->array].occupied != 4 ||
                (b.has_derivations && (!b.single_index || b.single_metadata_only)))
                return 0;
            return b.scalar32_directions;
        };
        for (auto& view : entity_views_) {
            for (std::size_t ordinal = 0; ordinal < view.prepared.size(); ++ordinal) {
                auto& context = view.prepared[ordinal];
                const auto& b = *context.binding;
                auto scalar = eligible(context);
                unsigned mapped = 0;
                if (context.identity_index != STATIC_NONE) {
                    scalar = 0;
                    const auto& window = owner_->identities_[context.identity_index];
                    if (view.identity_targets.size() == window.count &&
                        !view.identity_targets.empty()) {
                        const auto& mount = *context.mount;
                        const auto offset = mount.base - window.base;
                        const auto inner = offset % window.stride;
                        const auto span = mount.end - mount.base;
                        if (inner >= window.region_offset && span <= window.region_size &&
                            inner - window.region_offset <= window.region_size - span) {
                            context.request_id = static_cast<std::uint32_t>(offset / window.stride);
                            context.mount_inner = inner;
                            mapped = 3;
                            for (const auto* dst : view.identity_targets) {
                                if (!dst) {
                                    mapped = 0;
                                    break;
                                }
                                mapped &= eligible(dst->prepared[ordinal]);
                            }
                        }
                    }
                } else if (b.has_derivations) {
                    mapped = scalar;
                    scalar = 0;
                }
                const auto policy =
                    (context.policy.allow_read ? 1 : 0) | (context.policy.allow_write ? 2 : 0);
                scalar &= policy;
                const auto scalar_path =
                    owner_->pre_access_ || context.pre_access_chain ? Path::Mapped : Path::Scalar;
                auto path = [&](unsigned direction) {
                    if (scalar & direction)
                        return scalar_path;
                    return mapped & direction ? Path::Mapped : Path::Generic;
                };
                context.read_path = path(1);
                context.write_path = path(2);
            }
        }
    }
    RegisterMap::AliasEntry* decode(std::uint64_t address, RegisterMap::AliasEntry& out) const {
        const auto word = address & ~std::uint64_t{3};
        PreparedHit hit{};
        if (!find_prepared(word, hit))
            return nullptr;
        fill_prepared(hit, word, out);
        return &out;
    }
    // Legacy scope ordering: deeper scopes win ties, higher priority wins at
    // the first divergent view level.
    int compare(std::uint32_t a, std::uint32_t b) const noexcept;
    // Diagnostics: every mapped word in ascending address order with its
    // winner context, in ascending address order.
    template <class F>
    void for_each_word(F f) const;
    friend class FormulaInstaller;
};
template <class F>
void FormulaDecoder::for_each_word(F f) const {
    std::vector<std::uint64_t> addresses;
    RegisterMap::AliasEntry entry;
    // Diagnostic/reporting enumeration only, never on MMIO hot path.
    for (const auto& view : entity_views_) {
        const auto& layout = *view.layout;
        if (!layout.dense.empty()) {
            for (std::size_t i = 0; i < layout.dense.size(); ++i)
                if (layout.dense[i].ordinal != STATIC_NONE)
                    addresses.push_back(view.base + layout.dense_base + i * 4);
        } else {
            for (const auto& [local, access] : layout.words) {
                (void)access;
                addresses.push_back(view.base + local);
            }
        }
    }
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
    for (const auto address : addresses)
        if (decode(address, entry))
            f(address, entry);
}
} // namespace detail
} // namespace reg
