#pragma once
#include "access_context.hpp"

namespace reg {
namespace detail {
// Shared prepared-access decoder: View range + one immutable EntityLayout
// hash + an instance-bound context. No Zone, Page, Run or per-access caches.
class FormulaDecoder {
  public:
    struct Scope {
        std::uint32_t parent = STATIC_NONE;
        std::uint64_t base{}, size{};
        std::int32_t priority{}, outer_priority{};
        std::optional<std::uint32_t> instance;
        AccessRouteKind route{AccessRouteKind::Direct};
        AliasAccessPolicy policy; // effective down the chain
        const ViewPreAccessChain* pre_access_chain{};
        std::uint32_t identity = STATIC_NONE; // index into families below
        std::string_view name;
    };
    struct Mount {
        std::uint64_t base{}, end{};
        std::uint32_t binding{}, scope{}, array{};
        bool scalar{};
        std::uint8_t fast_mapped{}; // identity/single-index eligibility, precomputed at finalize()
    };
    struct Reserve {
        std::uint64_t base = 0, end = 0;
        std::uint32_t scope = 0;
    };
    // Node traits snapshotted at finalize(); the hot path must not chase the
    // node pointer just to select an execution path. Only immutable-after-
    // finish fields live here. Callbacks, logger and borrow are checked live.
    struct Binding {
        EntityState* node{};
        std::uint64_t canonical_base{};
        std::uint32_t linear{};
        std::uint32_t writable_mask{std::uint32_t(~std::uint32_t{0})};
        std::size_t scalar_stride{};
        std::byte* scalar_data{};
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
    // to n-dimensional index and immutable access metadata.
    // VF aliases share the geometry map, not callback/storage instances.
    // Shared, immutable register-location metadata. An index has a dynamic
    // rank (1..MAX_REG_DIMS), not hard-coded index0/index1. Callback and
    // storage objects belong to a *mounted instance*, never to this layout.
    struct PreparedAccessInfo {
        std::uint32_t ordinal{}, linear{}, byte{}, available{};
        RegElementIndex index{};
    };
    struct EntityHashLayout {
        std::unordered_map<std::uint64_t, PreparedAccessInfo> words;
        bool has_priority_aliases{};
    };
    // Bound once per mount. These stable references account for hooks,
    // per-instance storage, route, identity and access policy. A callback
    // slot may be changed by the user, so we never freeze its function value.
    struct PreparedMountContext {
        const Mount* mount{};
        const Binding* binding{};
        EntityState* node{};
        const RegViewLayout* node_layout{};
        const ViewPreAccessChain* pre_access_chain{};
        AccessRoute route{};
        AliasAccessPolicy policy{};
        std::optional<std::uint32_t> instance;
        std::string_view view_name;
        std::uint32_t identity_index{STATIC_NONE};
        std::int32_t priority{};
        // The node owns live callback slots, transaction/snapshot state, and
        // storage. No function pointer or mutable state is snapshotted here.
    };
    struct EntityView {
        std::uint64_t base{}, end{}, prefix_max_end{};
        std::uint32_t scope{};
        std::vector<std::uint32_t> mounts; // mount ordinal -> per-instance binding
        std::vector<PreparedMountContext> prepared;
        std::shared_ptr<const EntityHashLayout> layout;
    };
    struct PreparedHit {
        const EntityView* view{};
        const PreparedMountContext* context{};
        const PreparedAccessInfo* access{};
    };
    std::vector<EntityView> entity_views_;
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
    void finish_entity_hash() {
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
            while (scopes_[group].parent != STATIC_NONE && scope_words[group] < min_view_keys)
                group = scopes_[group].parent;
            auto [it, created] =
                scopes_to_view.try_emplace(group, static_cast<std::uint32_t>(entity_views_.size()));
            if (created) {
                const auto& sc = scopes_[group];
                if (sc.size > UINT64_MAX - sc.base)
                    throw Error("entity hash: view range overflow", ErrorKind::Configuration);
                entity_views_.push_back({sc.base, sc.base + sc.size, 0, group, {}, {}, {}});
            }
            entity_views_[it->second].mounts.push_back(id);
        }
        for (auto& view : entity_views_) {
            // The mount vector is base-sorted, so the same Bundle declaration
            // produces matching ordinals under different mount bases.
            for (const auto& earlier : entity_views_) {
                if (&earlier == &view)
                    break;
                if (!earlier.layout->has_priority_aliases && same_entity_geometry(earlier, view)) {
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
            layout->words.reserve(count);
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
                        if (actual_linear >= binding.node->layout().elements.size())
                            throw Error("prepared access: invalid array element",
                                        ErrorKind::Configuration);
                        PreparedAccessInfo access{};
                        access.ordinal = ordinal;
                        access.linear = linear;
                        access.byte = byte;
                        access.available = arr.occupied - byte;
                        access.index = binding.node->layout().elements[actual_linear].index;
                        const auto [it, inserted] = layout->words.try_emplace(key, access);
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
            entity_hash_bytes_ +=
                layout->words.size() *
                    (sizeof(std::uint64_t) + sizeof(PreparedAccessInfo) + sizeof(void*) * 2) +
                layout->words.bucket_count() * sizeof(void*);
            view.layout = std::move(layout);
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
                view.prepared.push_back({&mount,
                                         &binding,
                                         binding.node,
                                         &binding.node->layout(),
                                         scope.pre_access_chain,
                                         AccessRoute{scope.route, scope.instance.value_or(0)},
                                         scope.policy,
                                         scope.instance,
                                         scope.name,
                                         identity_index,
                                         scope.outer_priority});
            }
        }
        std::sort(entity_views_.begin(), entity_views_.end(), [](const auto& a, const auto& b) {
            return a.base < b.base || (a.base == b.base && a.scope < b.scope);
        });
        std::uint64_t max_end = 0;
        for (auto& v : entity_views_) {
            max_end = std::max(max_end, v.end);
            v.prefix_max_end = max_end;
        }
        if (std::getenv("REGFW_ENTITY_HASH_STATS")) {
            std::size_t keys = 0;
            for (const auto& v : entity_views_)
                if (v.layout)
                    keys += v.layout->words.size();
            std::fprintf(stderr,
                         "ENTITY_HASH_STATS views=%zu unique_layouts=%zu total_view_keys=%zu "
                         "approx_allocated_bytes=%zu\n",
                         entity_views_.size(),
                         entity_layout_count_,
                         keys,
                         entity_hash_bytes_);
        }
    }
    bool lookup_prepared(std::uint64_t word, PreparedHit& hit) const noexcept {
        std::size_t low = 0, high = entity_views_.size();
        while (low < high) {
            const auto mid = low + (high - low) / 2;
            if (entity_views_[mid].base <= word)
                low = mid + 1;
            else
                high = mid;
        }
        const Mount* winner = nullptr;
        while (low) {
            const auto& view = entity_views_[--low];
            if (view.prefix_max_end <= word)
                break;
            if (word >= view.end)
                continue;
            const auto it = view.layout->words.find(word - view.base);
            if (it == view.layout->words.end())
                continue;
            const auto& access = it->second;
            const auto& bound = view.prepared[access.ordinal];
            if (!winner || compare(bound.mount->scope, winner->scope) > 0) {
                winner = bound.mount;
                hit = {&view, &bound, &access};
            }
        }
        return winner != nullptr;
    }
    const Mount* find_entity_hash(std::uint64_t word,
                                  std::uint32_t& linear,
                                  std::uint32_t& byte) const noexcept {
        PreparedHit hit{};
        if (!lookup_prepared(word, hit))
            return nullptr;
        linear = hit.access->linear;
        byte = hit.access->byte;
        return hit.context->mount;
    }
    bool find_prepared(std::uint64_t word, PreparedHit& hit) const noexcept {
        if (!lookup_prepared(word, hit))
            return false;
        // An occupied REG can be masked by a higher-priority Reserve.
        for (const auto& r : reserves_) {
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
  public:
    FormulaDecoder() = default;

  private:
    // Decode an offset inside one mount. Dense tilings (occupied is always a
    // power of two bytes) resolve with one shift/mask pair from the inlined
    // mount traits, exactly matching formula_decode_offset's Dense/Scalar
    // branches; every other shape defers to the shared array table. The
    // caller guarantees offset < mount end - base, so the span guard inside
    // formula_decode_offset is subsumed for dense shapes.
    // Only used for installing redirects and verifying element continuity.
    // Ordinary MMIO already has the full prepared access context.
    const Mount*
    find_mount(std::uint64_t word, std::uint32_t& linear, std::uint32_t& byte) const noexcept {
        const Mount* found = find_entity_hash(word, linear, byte);
        if (!found || reserves_.empty())
            return found;
        for (const auto& r : reserves_) {
            if (word < r.base)
                break;
            if (word >= r.end)
                continue;
            if (compare(r.scope, found->scope) >= 0)
                return nullptr;
        }
        return found;
    }

  public:
    // Fill the alias context from an already-located mount. Every field is
    // written explicitly — no whole-entry reset on the hot path. The scratch
    // entries keep their target_entry optionals permanently engaged, so this
    // assigns through them without optional churn. Fields only consumed by
    // rare paths (identity trio, redirect, name) get their neutral values.
    void fill_entry(const Mount& mount,
                    std::uint64_t word,
                    RegisterMap::AliasEntry& out,
                    std::uint32_t linear,
                    std::uint32_t byte,
                    const RegElementIndex* prepared_index = nullptr) const {
        const auto& winner = &mount;
        const auto& binding = bindings_[winner->binding];
        const auto& scope = scopes_[winner->scope];
        auto* const node = binding.node;
        if (!out.target_entry)
            out.target_entry.emplace(); // scratch slots stay engaged
        out.compiled = true;
        out.defined = true;
        out.target = binding.canonical_base + (word - winner->base);
        out.used = out.target != word;
        out.available = arrays_[winner->array].occupied - byte;
        // A bus access may only continue into the next word when the same
        // element is selected there. Elements no wider than the current word
        // can never continue, so the next-word probe is skipped entirely —
        // min(available,4) is the identity when available<=4. Wider elements
        // (overlays, reserves, cross-element boundaries) still probe.
        if (out.available > 4) {
            std::uint32_t next_linear = 0, next_byte = 0;
            const auto* next = find_mount(word + 4, next_linear, next_byte);
            if (next != &mount || next_linear != linear || next_byte != byte + 4)
                out.available = 4;
        }
        out.route = {scope.route, scope.instance.value_or(0)};
        out.policy = scope.policy;
        out.instance = scope.instance;
        out.view_name = scope.name;
        out.pre_access_chain = scope.pre_access_chain;
        out.priority = scope.outer_priority;
        out.redirect_targets = nullptr;
        out.identity_index.reset();
        out.name = {};
        if (scope.identity != STATIC_NONE) {
            const auto index = identity_base_ + scope.identity;
            out.identity_index = index;
            const auto& window = owner_->identities_[index];
            const auto offset = word - window.base;
            out.identity_request = static_cast<std::uint32_t>(offset / window.stride);
            out.identity_inner = offset % window.stride;
            out.execute = &RegisterMap::resolve_plan<2>;
        } else
            out.execute = &RegisterMap::resolve_plan<1>;
        auto& entry = *out.target_entry;
        entry.node = node;
        entry.linear = binding.linear + linear;
        entry.byte_offset = byte;
        entry.source_instance = scope.instance;
        entry.view = &node->layout();
        entry.precomputed_index = prepared_index;
        out.callback_index = prepared_index
                                 ? prepared_index
                                 : &node->layout().elements[binding.linear + linear].index;
        out.callback_instance = scope.instance;
        out.scalar32_directions = 0;
    }
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
        out.compiled = true;
        out.defined = true;
        out.target = binding.canonical_base + (word - mount.base);
        out.used = out.target != word;
        out.available = info.available;
        if (out.available > 4) {
            // View overlap/reserve can interrupt an otherwise continuous
            // wide register. Validate only this rare, multi-word case.
            std::uint32_t next_linear = 0, next_byte = 0;
            const auto* next = find_mount(word + 4, next_linear, next_byte);
            if (next != &mount || next_linear != info.linear || next_byte != info.byte + 4)
                out.available = 4;
        }
        out.route = bound.route;
        out.policy = bound.policy;
        out.instance = bound.instance;
        out.view_name = bound.view_name;
        out.pre_access_chain = bound.pre_access_chain;
        out.priority = bound.priority;
        out.redirect_targets = nullptr;
        out.identity_index.reset();
        out.name = {};
        if (bound.identity_index != STATIC_NONE) {
            const auto index = bound.identity_index;
            out.identity_index = index;
            const auto& window = owner_->identities_[index];
            const auto offset = word - window.base;
            out.identity_request = static_cast<std::uint32_t>(offset / window.stride);
            out.identity_inner = offset % window.stride;
            out.execute = &RegisterMap::resolve_plan<2>;
        } else
            out.execute = &RegisterMap::resolve_plan<1>;
        if (!out.target_entry)
            out.target_entry.emplace();
        auto& entry = *out.target_entry;
        entry.node = bound.node;
        entry.linear = binding.linear + info.linear;
        entry.byte_offset = info.byte;
        entry.source_instance = bound.instance;
        entry.view = bound.node_layout;
        entry.precomputed_index = &info.index;
        out.callback_index = &info.index;
        out.callback_instance = bound.instance;
        out.scalar32_directions = 0;
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
                         std::vector<LayoutWarning>* warnings);
    void finish_dispatch_tables() {
        finish_entity_hash();
    }
    // Resolve identity/single-index fast-path eligibility once at finalize().
    // The per-access path used to decode bank 0 (or the source element) on
    // every MMIO beat; geometry and node traits cannot change after finish.
    // Binding-level execution traits are snapshotted in the same pass: the
    // hot path then selects its route without chasing the node pointer.
    void prepare_fast_paths() {
        for (auto& b : bindings_) {
            const auto& n = *b.node;
            b.writable_mask = static_cast<std::uint32_t>(n.writable_mask);
            b.scalar_stride = n.scalar_stride;
            b.scalar_data = n.scalar_data;
            b.scalar32_directions = n.scalar32_directions;
            b.has_derivations = !n.derivations.empty();
            b.single_index = bool(n.single_index);
            b.single_metadata_only = b.single_index && n.single_index->metadata_only;
        }
        auto eligible = [](const RegisterMap::AliasEntry* target) -> std::uint8_t {
            if (!target || !target->defined || !target->target_entry)
                return 3;
            const auto& e = *target->target_entry;
            const auto& t = *e.node;
            if (target->available < 4 || e.byte_offset ||
                (!t.derivations.empty() && (!t.single_index || t.single_index->metadata_only)))
                return 0;
            return t.scalar32_directions;
        };
        RegisterMap::AliasEntry source{}, target{};
        for (auto& mount : mounts_) {
            mount.fast_mapped = 0;
            const auto& scope = scopes_[mount.scope];
            std::uint32_t linear = 0, byte = 0;
            std::uint64_t word = mount.base;
            if (scope.identity != STATIC_NONE) {
                const auto& window = owner_->identities_[identity_base_ + scope.identity];
                if (window.mode != IdentityRemapMode::RedirectBank || window.count > 64 ||
                    mount.base < window.base || window.stride == 0)
                    continue;
                word = window.base + (mount.base - window.base) % window.stride;
            }
            const Mount* src = find_mount(word, linear, byte);
            if (!src)
                continue;
            fill_entry(*src, word, source, linear, byte);
            if (source.identity_index) {
                const auto& window = owner_->identities_[*source.identity_index];
                std::uint32_t tl = 0, tb = 0;
                if (const Mount* dst = find_mount(window.base + source.identity_inner, tl, tb)) {
                    fill_entry(*dst, window.base + source.identity_inner, target, tl, tb);
                    mount.fast_mapped = eligible(&target);
                }
            } else {
                if (bindings_[mount.binding].single_index)
                    mount.fast_mapped = eligible(&source);
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
    // Scalar32/Mapped/Generic execution plan for a 4-byte aligned word, or
    // nullptr for holes (the generic resolve path then reports them). Path
    // selection mirrors finalize()'s per-word compilation; the difference is
    // that it is derived per access from node traits, not a per-word table.
    const RegisterMap::CompiledAccess* fast_access(std::uint64_t address,
                                                   RegisterMap::AccessScratch& scratch) const {
        auto& plan = scratch.decoded_access;
        const auto word = address & ~std::uint64_t{3};
        PreparedHit hit{};
        if (!find_prepared(word, hit))
            return nullptr;
        const Mount* mount = hit.context->mount;
        const auto linear = hit.access->linear;
        const auto byte = hit.access->byte;
        const auto& context = *hit.context;
        const auto& scope = scopes_[mount->scope];
        const auto& binding = *context.binding;
        auto& n = *binding.node;
        plan.metadata = nullptr;
        plan.node = &n;
        plan.mask = binding.writable_mask;
        plan.directions = 0;
        plan.mapped_directions = 0;
        plan.scalar = nullptr;
        // Common scalar register with no callback, pre-access, identity or
        // derivation: the entire execution plan is node + scalar pointer.
        // This is the hot Scalar32 shape and must not build an AliasEntry.
        // Frozen geometry/traits come from the mount and binding snapshots;
        // borrow, callbacks and the logger stay live per access.
        if (mount->scalar && byte == 0 && binding.scalar_data && !scope.pre_access_chain &&
            scope.identity == STATIC_NONE && !binding.has_derivations) {
            const auto policy_bits = static_cast<std::uint8_t>((scope.policy.allow_read ? 1 : 0) |
                                                               (scope.policy.allow_write ? 2 : 0));
            const auto directions =
                static_cast<std::uint8_t>(binding.scalar32_directions & policy_bits);
            if (directions && !n.borrowed && !n.read_callback && !n.write_callback &&
                !n.read_complete_callback && !owner_->logger_) {
                plan.directions = directions;
                plan.scalar = binding.scalar_data +
                              (std::size_t(binding.linear) + linear) * binding.scalar_stride;
                using Path = RegisterMap::CompiledAccess::Path;
                plan.read_path = (directions & 1) ? Path::Scalar : Path::Generic;
                plan.write_path = (directions & 2) ? Path::Scalar : Path::Generic;
                return &plan;
            }
        }
        // Use the already resolved hit to prepare the full context for
        // callbacks, overlays, identity remapping and the generic engine.
        auto& out = scratch.decoded[0];
        fill_prepared(hit, word, out);
        plan.metadata = &out;
        const auto& e = *out.target_entry;
        auto& target = *e.node;
        const auto& target_binding = bindings_[mount->binding];
        plan.node = &target;
        plan.mask = target_binding.writable_mask;
        const auto policy_bits = static_cast<std::uint8_t>((scope.policy.allow_read ? 1 : 0) |
                                                           (scope.policy.allow_write ? 2 : 0));
        if (out.available >= 4) {
            if (out.identity_index) {
                const auto& window = owner_->identities_[*out.identity_index];
                if (window.mode == IdentityRemapMode::RedirectBank && window.count <= 64)
                    plan.mapped_directions = mount->fast_mapped;
            } else {
                if (!out.pre_access_chain && e.byte_offset == 0 && !target_binding.has_derivations)
                    plan.directions =
                        static_cast<std::uint8_t>(target_binding.scalar32_directions & policy_bits);
                if (target_binding.single_index)
                    plan.mapped_directions = mount->fast_mapped;
            }
        }
        plan.scalar = plan.directions ? target_binding.scalar_data +
                                            std::size_t(e.linear) * target_binding.scalar_stride
                                      : nullptr;
        using Path = RegisterMap::CompiledAccess::Path;
        plan.read_path = (plan.directions & 1)          ? Path::Scalar
                         : (plan.mapped_directions & 1) ? Path::Mapped
                                                        : Path::Generic;
        plan.write_path = (plan.directions & 2)          ? Path::Scalar
                          : (plan.mapped_directions & 2) ? Path::Mapped
                                                         : Path::Generic;
        return &plan;
    }
    // Legacy scope ordering: deeper scopes win ties, higher priority wins at
    // the first divergent view level.
    int compare(std::uint32_t a, std::uint32_t b) const noexcept;
    // Diagnostics: every mapped word in ascending address order with its
    // winner context, in ascending address order.
    template <class F>
    void for_each_word(F f) const;
    std::size_t mount_count() const noexcept {
        return mounts_.size();
    }
    std::size_t zone_count() const noexcept {
        return 0;
    }
    std::size_t binding_count() const noexcept {
        return bindings_.size();
    }
    friend class FormulaInstaller;
};
template <class F>
void FormulaDecoder::for_each_word(F f) const {
    std::vector<std::uint64_t> addresses;
    RegisterMap::AliasEntry entry;
    // Diagnostic/reporting enumeration only, never on MMIO hot path.
    for (const auto& view : entity_views_)
        for (const auto& [local, access] : view.layout->words) {
            (void)access;
            addresses.push_back(view.base + local);
        }
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
    for (const auto address : addresses)
        if (decode(address, entry))
            f(address, entry);
}
} // namespace detail
} // namespace reg
