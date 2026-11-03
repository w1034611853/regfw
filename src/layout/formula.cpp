#include "internal/runtime_impl.hpp"
#include <sstream>
#include <set>
#include <tuple>
namespace reg::detail {
// ---------------------------------------------------------------------------
// FormulaDecoder: address -> (binding, element, byte) by arithmetic.
// ---------------------------------------------------------------------------
int FormulaDecoder::compare(std::uint32_t a, std::uint32_t b) const noexcept {
    if (a == b)
        return 0;
    std::array<std::uint32_t, 64> x{}, y{};
    unsigned nx = 0, ny = 0;
    for (auto p = a; p != STATIC_NONE; p = scopes_[p].parent) {
        if (nx == x.size())
            return 0;
        x[nx++] = p;
    }
    for (auto p = b; p != STATIC_NONE; p = scopes_[p].parent) {
        if (ny == y.size())
            return 0;
        y[ny++] = p;
    }
    unsigned k = 0;
    while (k < std::min(nx, ny) && x[nx - 1 - k] == y[ny - 1 - k])
        ++k;
    if (k == nx || k == ny)
        return nx < ny ? -1 : nx > ny ? 1 : 0;
    // First divergent view level; each priority indexed from its own chain
    // end. (The 0.35.0 release indexed y with nx, silently comparing against
    // the wrong level whenever the two chains differ in depth.)
    const auto xp = scopes_[x[nx - 1 - k]].priority, yp = scopes_[y[ny - 1 - k]].priority;
    return xp < yp ? -1 : xp > yp ? 1 : 0;
}
namespace {
// Element interval of one array intersecting a mount-relative [low,high).
// Installation-time validation only; the hot decoder never builds these.
struct ElementRef {
    std::uint64_t start;
    std::uint32_t linear, occupied;
};
void collect_intervals(const FormulaArraySpec& array,
                       std::uint64_t low,
                       std::uint64_t high,
                       std::vector<ElementRef>& out) {
    out.clear();
    const auto occupied = array.occupied;
    switch (array.kind) {
        case FormulaDecodeKind::Scalar:
            if (low < occupied && high > 0)
                out.push_back({0, 0, occupied});
            return;
        case FormulaDecodeKind::Dense: {
            auto start = (low / occupied) * occupied;
            if (start + occupied <= low)
                start += occupied;
            for (; start < high; start += occupied) {
                if (start >= array.span)
                    break;
                out.push_back({start, static_cast<std::uint32_t>(start / occupied), occupied});
            }
            return;
        }
        case FormulaDecodeKind::Table: {
            const auto& records = array.sparse;
            const auto threshold = low >= occupied ? low - occupied + 1 : 0;
            std::size_t lo = 0, hi = records.size();
            while (lo < hi) {
                const auto mid = lo + (hi - lo) / 2;
                if (records[mid].offset < threshold)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            for (auto k = lo; k < records.size() && records[k].offset < high; ++k)
                out.push_back({records[k].offset, records[k].linear, occupied});
            return;
        }
        case FormulaDecodeKind::Interleaved2: {
            if (array.rank > 2)
                break; // generalized ladders stream via floor/next below
            const auto d0 = array.inter_axes[0], d1 = array.inter_axes[1];
            for (std::uint32_t i = 0; i < array.extents_decl[d0]; ++i) {
                const auto base = std::uint64_t(i) * array.strides_decl[d0];
                if (base >= high)
                    break;
                for (std::uint32_t j = 0; j < array.extents_decl[d1]; ++j) {
                    const auto start = base + std::uint64_t(j) * array.strides_decl[d1];
                    if (start < high && start + occupied > low)
                        out.push_back({start,
                                       static_cast<std::uint32_t>(i * array.multiplier_decl[d0] +
                                                                  j * array.multiplier_decl[d1]),
                                       occupied});
                }
            }
            std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
                return a.start < b.start;
            });
            return;
        }
        case FormulaDecodeKind::Monotone:
            break;
    }
    auto start = formula_floor_offset(array, low);
    if (start == UINT64_MAX)
        return;
    if (start + occupied <= low) {
        start = formula_next_offset(array, start);
        if (start == UINT64_MAX)
            return;
    }
    if (start >= high)
        return; // never report an element starting past the window
    for (;;) {
        std::uint32_t linear = 0, byte = 0;
        if (!formula_decode_offset(array, start, linear, byte))
            break;
        out.push_back({start, linear, occupied});
        start = formula_next_offset(array, start);
        if (start == UINT64_MAX || start >= high)
            break;
    }
}

// All overlap validators use the same absolute-address coordinates. Keeping
// this merge in one place prevents the relative-mount offset regressions that
// affected earlier formula backends. Return true from the visitor to stop.
struct ByteOverlap {
    const ElementRef& first;
    const ElementRef& second;
    std::uint64_t first_start, second_start, begin, end;
    bool different_bytes(std::uint32_t first_binding, std::uint32_t second_binding) const {
        return first_binding != second_binding || first.linear != second.linear ||
               first_start != second_start;
    }
};
template <class F>
bool for_each_overlap(const FormulaArraySpec& a,
                      std::uint64_t abase,
                      const FormulaArraySpec& b,
                      std::uint64_t bbase,
                      std::uint64_t low,
                      std::uint64_t high,
                      std::vector<ElementRef>& left,
                      std::vector<ElementRef>& right,
                      F&& visit) {
    collect_intervals(a, low - abase, high - abase, left);
    collect_intervals(b, low - bbase, high - bbase, right);
    std::size_t i = 0, j = 0;
    while (i < left.size() && j < right.size()) {
        const auto as = abase + left[i].start, ae = as + left[i].occupied;
        const auto bs = bbase + right[j].start, be = bs + right[j].occupied;
        const auto begin = std::max(as, bs), end = std::min(ae, be);
        if (begin < end && visit(ByteOverlap{left[i], right[j], as, bs, begin, end}))
            return true;
        if (ae <= be)
            ++i;
        else
            ++j;
    }
    return false;
}
// Reserve intersections must inspect occupied register elements, not the
// potentially enormous outer span of a sparse array.
template <class Reserves, class Mounts, class Arrays, class F>
void for_each_reserved_occupied(const Reserves& reserves,
                                const Mounts& mounts,
                                const Arrays& arrays,
                                F&& visit) {
    std::vector<ElementRef> elements;
    for (const auto& reserve : reserves)
        for (const auto& mount : mounts) {
            if (mount.base >= reserve.end || reserve.base >= mount.end)
                continue;
            const auto lo = std::max(reserve.base, mount.base),
                       hi = std::min(reserve.end, mount.end);
            collect_intervals(arrays[mount.array], lo - mount.base, hi - mount.base, elements);
            if (!elements.empty())
                visit(reserve, mount);
        }
}
} // namespace

// Runtime (AddressSpace/RegView) install. The view compiler has already
// resolved canonical addresses and one scalar mount per mapped element; this
// builds the same zones/page directory/runs the compiled path uses. It is a
// descriptor set proportional to mapped elements, never to address words.
void FormulaDecoder::install_dynamic(RegisterMap* map,
                                     std::uint32_t identity_base,
                                     std::vector<Scope> scopes,
                                     std::vector<Mount> mounts,
                                     std::vector<Binding> bindings,
                                     std::vector<FormulaArraySpec> arrays,
                                     std::vector<std::vector<ElementRecord>> record_storage,
                                     std::vector<Reserve> reserves,
                                     std::vector<LayoutWarning>* warnings) {
    owner_ = map;
    identity_base_ = identity_base;
    scopes_.swap(scopes);
    mounts_.swap(mounts);
    bindings_.swap(bindings);
    arrays_.swap(arrays);
    reserves_.swap(reserves);
    // Table-record storage must outlive arrays_: the specs' spans point here.
    // Inner vectors never relocate, so spans taken before the move stay valid.
    record_storage_ = std::move(record_storage);
    std::sort(mounts_.begin(), mounts_.end(), [](const Mount& a, const Mount& b) {
        return a.base < b.base;
    });
    std::sort(reserves_.begin(), reserves_.end(), [](const Reserve& a, const Reserve& b) {
        return a.base < b.base;
    });
    // A reserve is terminal; equal priority remains a configuration error,
    // while a higher-priority reserve intentionally shadows register bytes.
    // With declaration-pattern mounts a reserve range may cross element
    // holes, so the byte overlap is verified against the real intervals.
    for_each_reserved_occupied(reserves_, mounts_, arrays_, [&](const auto& r, const auto& m) {
        if (compare(r.scope, m.scope) == 0)
            throw Error("same-priority entity/reserved mapping conflict:\n  " +
                            std::string(scopes_[r.scope].name) + "\n  " +
                            std::string(scopes_[m.scope].name),
                        ErrorKind::Configuration);
    });
    // Prepare the shared EntityLayout hashes and per-instance bindings.
    finish_dispatch_tables();
    // No Zone/Candidate/Page/Run tables or per-access caches.
    // Validate actual occupied bytes directly; outer View envelopes are not
    // treated as occupied, and a higher-priority mount must actually have a
    // register at the conflicting address to conceal a lower conflict.
    {
        std::vector<ElementRef> left, right;
        std::set<std::pair<std::size_t, std::size_t>> reported;
        for (std::size_t ai = 0; ai < mounts_.size(); ++ai) {
            const auto& a = mounts_[ai];
            for (std::size_t bi = ai + 1; bi < mounts_.size(); ++bi) {
                const auto& b = mounts_[bi];
                if (b.base >= a.end)
                    break;
                const auto lo = std::max(a.base, b.base), hi = std::min(a.end, b.end);
                if (lo >= hi)
                    continue;
                for_each_overlap(
                    arrays_[a.array],
                    a.base,
                    arrays_[b.array],
                    b.base,
                    lo,
                    hi,
                    left,
                    right,
                    [&](const ByteOverlap& overlap) {
                        if (!overlap.different_bytes(a.binding, b.binding))
                            return false;
                        for (auto word = overlap.begin; word < overlap.end; word += 4) {
                            bool hidden = false;
                            const auto highest = compare(a.scope, b.scope) > 0 ? a.scope : b.scope;
                            for (std::size_t ci = 0; ci < mounts_.size(); ++ci) {
                                if (ci == ai || ci == bi)
                                    continue;
                                const auto& upper = mounts_[ci];
                                if (compare(upper.scope, highest) <= 0 || word < upper.base ||
                                    word >= upper.end)
                                    continue;
                                std::uint32_t linear = 0, byte = 0;
                                if (formula_decode_offset(
                                        arrays_[upper.array], word - upper.base, linear, byte)) {
                                    hidden = true;
                                    break;
                                }
                            }
                            if (!hidden)
                                for (const auto& r : reserves_)
                                    if (r.base <= word && word < r.end &&
                                        compare(r.scope, highest) > 0) {
                                        hidden = true;
                                        break;
                                    }
                            if (hidden)
                                continue;
                            if (compare(a.scope, b.scope) == 0)
                                throw Error("same-priority register address conflict:\n  " +
                                                std::string(scopes_[a.scope].name) + "\n  " +
                                                std::string(scopes_[b.scope].name) +
                                                "\n  address=" + std::to_string(word),
                                            ErrorKind::Configuration);
                            if (warnings && reported.emplace(ai, bi).second) {
                                const auto& lower = compare(a.scope, b.scope) < 0 ? a : b;
                                const auto& higher = compare(a.scope, b.scope) > 0 ? a : b;
                                const auto& low_name = bindings_[lower.binding].node->name;
                                const auto& high_name = bindings_[higher.binding].node->name;
                                std::ostringstream message;
                                message << "entity " << low_name << " in "
                                        << scopes_[lower.scope].name << " is shadowed by "
                                        << high_name << " in " << scopes_[higher.scope].name
                                        << " at [0x" << std::hex << word << ", 0x" << word + 4
                                        << ")" << std::dec << "; priority paths "
                                        << scopes_[lower.scope].priority << " -> "
                                        << scopes_[higher.scope].priority;
                                warnings->push_back({"REGFW_ENTITY_SHADOWED",
                                                     low_name,
                                                     message.str(),
                                                     std::source_location::current()});
                            }
                        }
                        return false;
                    });
            }
        }
    }
    // A reserve is terminal. Preserve the word backend's diagnostic for the
    // register bytes it actually shadows (one warning per shadowed node).
    if (warnings) {
        std::set<std::pair<EntityState*, std::uint32_t>> seen;
        for_each_reserved_occupied(reserves_, mounts_, arrays_, [&](const auto& r, const auto& m) {
            if (compare(r.scope, m.scope) <= 0)
                return;
            auto* n = bindings_[m.binding].node;
            if (!seen.emplace(n, r.scope).second)
                return;
            warnings->push_back({"REGFW_ENTITY_SHADOWED",
                                 n->name,
                                 "register bytes are shadowed by a reserved view",
                                 std::source_location::current()});
        });
    }
}

// ---------------------------------------------------------------------------
// FormulaInstaller: binds entities, installs scopes/identities/pre-access and
// the compact decoder. No per-word AliasEntry/CompiledAccess is produced.
// ---------------------------------------------------------------------------
class FormulaInstaller {
    static void install_identity_families(RegisterMap& map,
                                          FormulaPlanView plan,
                                          const std::vector<std::unique_ptr<std::string>>& names,
                                          std::size_t& identity_base) {
        identity_base = map.identities_.size();
        for (const auto& i : plan.identities) {
            IdentityWindow w{i.base,
                             i.count,
                             i.stride,
                             i.size,
                             0,
                             i.size,
                             i.domain,
                             IdentityRemapMode::RedirectBank,
                             {},
                             *names[i.scope],
                             {},
                             plan.scopes[i.scope].location};
            if (i.resolver_key) {
                auto& slot = map.access_resolvers_->identities[i.resolver_key];
                if (!slot) {
                    slot = std::make_shared<IdentityResolverSlot>();
                    slot->name = i.resolver_name;
                }
                slot->required = true;
                w.resolver = slot;
            } else if (!map.has_identity_resolver())
                map.set_identity_resolver("identity passthrough",
                                          [](const IdentityResolveRequest& r) {
                                              return r.request_id;
                                          });
            map.identities_.push_back(std::move(w));
        }
    }
    // Map every scope to its identity family (same ordering as the layout tree).
    static std::vector<std::uint32_t> scope_identities(FormulaPlanView plan) {
        std::vector<std::uint32_t> identities_for(plan.scopes.size(), STATIC_NONE);
        for (std::uint32_t id = 0; id < plan.scopes.size(); ++id) {
            const auto& s = plan.scopes[id];
            if (s.parent != STATIC_NONE)
                identities_for[id] = identities_for[s.parent];
            if (!s.domain)
                continue;
            if (identities_for[id] != STATIC_NONE)
                throw Error("nested static identity domains unsupported", ErrorKind::Configuration);
            const auto stride = s.repeat_stride ? s.repeat_stride : s.size;
            const auto base = s.base - s.repeat_index * stride;
            std::uint32_t index = 0;
            for (; index < plan.identities.size(); ++index) {
                const auto& v = plan.identities[index];
                if (v.base == base && v.count == s.repeat_count && v.stride == stride &&
                    v.size == s.size && v.domain == *s.domain && v.resolver_type == s.resolver_type)
                    break;
            }
            if (index == plan.identities.size())
                throw Error("static identity family missing", ErrorKind::Configuration);
            identities_for[id] = index;
        }
        return identities_for;
    }

  public:
    static void install(RegisterMap& map, void* root, FormulaPlanView plan) {
        map.mutable_layout();
        bool legacy = false;
        map.lookup_.for_each([&](auto, const auto&) {
            legacy = true;
        });
        if (legacy || map.compiled_view_mode_ || plan.scopes.empty())
            throw Error("static layout requires fresh storage-only entities",
                        ErrorKind::Configuration);
        std::vector<EntityState*> nodes;
        nodes.reserve(plan.bindings.size());
        for (const auto& b : plan.bindings) {
            auto* n = b.get(root, b.selectors);
            if (!n || n->owner != &map || n->element_count() != b.elements ||
                n->value_size != b.width)
                throw Error("static entity binding mismatch", ErrorKind::Configuration);
            nodes.push_back(n);
        }
        std::vector<std::unique_ptr<std::string>> names;
        names.reserve(plan.scopes.size());
        std::vector<std::size_t> scope_ids;
        scope_ids.reserve(plan.scopes.size());
        std::vector<std::unique_ptr<ViewPreAccessChain>> chains;
        std::vector<const ViewPreAccessChain*> pre(plan.scopes.size());
        std::vector<std::int32_t> priorities(plan.scopes.size());
        for (std::size_t id = 0; id < plan.scopes.size(); ++id) {
            const auto& s = plan.scopes[id];
            priorities[id] =
                s.parent != STATIC_NONE && priorities[s.parent] ? priorities[s.parent] : s.priority;
            const auto size = id ? s.size : map.size_;
            if (s.base >= map.size_ || size > map.size_ - s.base)
                throw Error("static view outside runtime address space",
                            ErrorKind::Configuration,
                            s.location);
            std::string name =
                s.parent == STATIC_NONE && s.name == map.name_prefix
                    ? map.name_prefix
                    : join_name(s.parent == STATIC_NONE ? std::string_view(map.name_prefix)
                                                        : std::string_view(*names[s.parent]),
                                s.name);
            if (s.repeated)
                name += '[' + std::to_string(s.repeat_index) + ']';
            names.push_back(std::make_unique<std::string>(std::move(name)));
            scope_ids.push_back(map.register_layout_scope(
                s.base,
                size,
                *names.back(),
                {},
                s.instance,
                s.parent == STATIC_NONE ? ~std::size_t{} : scope_ids[s.parent],
                s.repeated ? s.repeat_count : 1,
                s.repeated ? s.repeat_stride : 0,
                s.repeated ? s.repeat_index : 0));
            auto& scope = map.layout_scopes_[scope_ids.back()];
            scope.recursive_window = true;
            scope.purpose = s.reserved ? "RESERVED" : "VIEW";
            if (s.mapping) {
                const auto* bundle = s.bundle(root, s.selectors);
                if (Access::owner(*bundle) != &map ||
                    Access::entity_nodes(*bundle).size() != s.target_nodes)
                    throw Error("REG_SCHEMA must list every entity member exactly once",
                                ErrorKind::Configuration,
                                s.location);
                const auto region = bundle->region();
                const auto* n = nodes[s.target_binding];
                scope.purpose += " → entity: " + region.name() + "; reset=" +
                                 (n->reset_scope.kind == ResetScope::Pf   ? "PF"
                                  : n->reset_scope.kind == ResetScope::Vf ? "VF"
                                                                          : "Global");
            }
            if (s.pre_key)
                scope.purpose += "; pre_access=" + std::string(s.pre_name);
            scope.purpose += "; priority=" + std::to_string(s.priority);
            const auto* parent = s.parent == STATIC_NONE ? nullptr : pre[s.parent];
            pre[id] = parent;
            if (s.pre_key) {
                auto slot = map.view_pre_access_slot(s.pre_key, s.pre_name);
                slot->required = true;
                auto chain = parent ? std::make_unique<ViewPreAccessChain>(*parent)
                                    : std::make_unique<ViewPreAccessChain>();
                chain->steps.push_back({std::move(slot), *names.back()});
                pre[id] = chain.get();
                chains.push_back(std::move(chain));
            }
        }
        std::size_t identity_base = 0;
        install_identity_families(map, plan, names, identity_base);
        // Storage identities and names remain those of normal REG entities; only
        // canonical addresses/names are rewritten, exactly like the word backend.
        for (std::size_t id = 0; id < plan.bindings.size(); ++id) {
            const auto& b = plan.bindings[id];
            auto& n = *nodes[id];
            auto& l = n.layout();
            const auto prefix =
                *names[b.scope] + '.' +
                n.name.substr(n.name.rfind('.') == std::string::npos ? 0 : n.name.rfind('.') + 1);
            for (std::size_t k = 0; k < l.elements.size(); ++k) {
                auto& e = l.elements[k];
                e.address = b.canonical_base + l.local_addresses[k] - l.local_addresses.front();
                e.name = prefix;
                if (n.element_count() > 1)
                    for (auto coord : e.index.coordinates())
                        e.name += '[' + std::to_string(coord) + ']';
            }
            l.element_mapped.assign(n.element_count(), true);
            l.mapped = true;
            l.layout_scope = scope_ids[b.scope];
            l.display_name = *names[b.scope];
        }
        // ---- decoder tables -------------------------------------------------
        auto decoder = std::make_unique<FormulaDecoder>();
        decoder->owner_ = &map;
        decoder->identity_base_ = static_cast<std::uint32_t>(identity_base);
        const auto identities_for = scope_identities(plan);
        decoder->scopes_.resize(plan.scopes.size());
        std::vector<AliasAccessPolicy> effective(plan.scopes.size());
        std::vector<bool> effective_pre(plan.scopes.size());
        for (std::size_t id = 0; id < plan.scopes.size(); ++id) {
            const auto& s = plan.scopes[id];
            effective[id] =
                s.parent == STATIC_NONE ? s.policy : s.policy.intersect(effective[s.parent]);
            effective_pre[id] =
                !s.pre_type.empty() || (s.parent != STATIC_NONE && effective_pre[s.parent]);
            auto& out = decoder->scopes_[id];
            out.parent = s.parent;
            out.base = s.base;
            out.size = s.size;
            out.priority = s.priority;
            out.outer_priority = priorities[id];
            out.instance = s.instance;
            out.route = s.route;
            out.policy = effective[id];
            out.pre_access_chain = effective_pre[id] ? pre[id] : nullptr;
            out.identity = identities_for[id] == STATIC_NONE ? STATIC_NONE : identities_for[id];
            out.name = *names[id];
        }
        for (std::uint32_t id = 0; id < plan.scopes.size(); ++id) {
            const auto& s = plan.scopes[id];
            if (!s.reserved)
                continue;
            if (s.size > UINT64_MAX - s.base)
                throw "formula layout: ReserveRangeOverflow";
            decoder->reserves_.push_back({s.base, s.base + s.size, id});
        }
        std::sort(
            decoder->reserves_.begin(), decoder->reserves_.end(), [](const auto& a, const auto& b) {
                return a.base < b.base;
            });
        decoder->bindings_.resize(plan.bindings.size());
        for (std::size_t id = 0; id < plan.bindings.size(); ++id)
            decoder->bindings_[id].node = nodes[id],
            decoder->bindings_[id].canonical_base = plan.bindings[id].canonical_base;
        decoder->arrays_.assign(plan.arrays.begin(), plan.arrays.end());
        decoder->mounts_.resize(plan.mounts.size());
        for (std::size_t id = 0; id < plan.mounts.size(); ++id) {
            const auto& m = plan.mounts[id];
            decoder->mounts_[id] = {m.base, m.end, m.binding, m.scope, m.array, m.scalar, 0};
        }
        // A reserve is terminal. Preserve the word backend's diagnostic for the
        // register bytes it actually shadows (one warning per shadowed mount).
        std::set<std::pair<EntityState*, std::uint32_t>> reported_reserve;
        for_each_reserved_occupied(decoder->reserves_,
                                   decoder->mounts_,
                                   decoder->arrays_,
                                   [&](const auto& r, const auto& m) {
                                       if (decoder->compare(r.scope, m.scope) <= 0)
                                           return;
                                       auto* n = decoder->bindings_[m.binding].node;
                                       if (!reported_reserve.emplace(n, r.scope).second)
                                           return;
                                       map.mapping_warnings_.push_back(
                                           {"REGFW_ENTITY_SHADOWED",
                                            n->name,
                                            "register bytes are shadowed by a reserved view",
                                            plan.scopes[r.scope].location});
                                   });
        // ---- overlap validation ---------------------------------------------
        // Mounts may overlap only where they expose the identical register bytes;
        // the winner keeps the legacy scope ordering. Real byte conflicts and
        // same-priority duplicates are configuration errors.
        validate_overlaps(map, *decoder, plan);
        // ---- decode traits, zones, dense runs, page directory ----------------
        decoder->finish_dispatch_tables();
        map.compiled_names_.swap(names);
        map.view_pre_access_chains_.swap(chains);
        map.formula_ = std::move(decoder);
        map.compiled_view_mode_ = true;
    }

  private:
    // Interval collection lives in the anonymous namespace above and is shared
    // by the dynamic installer and this static-path validator.
    static void
    validate_overlaps(RegisterMap& map, const FormulaDecoder& decoder, FormulaPlanView plan) {
        (void)map;
        (void)plan;
        std::vector<ElementRef> left, right;
        for (std::size_t i = 0; i < decoder.mounts_.size(); ++i) {
            const auto& a = decoder.mounts_[i];
            for (std::size_t j = i + 1; j < decoder.mounts_.size(); ++j) {
                const auto& b = decoder.mounts_[j];
                if (b.base >= a.end)
                    break; // mounts are base-sorted
                const auto lo = std::max(a.base, b.base), hi = std::min(a.end, b.end);
                if (lo >= hi)
                    continue;
                bool shared = false;
                for_each_overlap(decoder.arrays_[a.array],
                                 a.base,
                                 decoder.arrays_[b.array],
                                 b.base,
                                 lo,
                                 hi,
                                 left,
                                 right,
                                 [&](const ByteOverlap& overlap) {
                                     shared = true;
                                     if (overlap.different_bytes(a.binding, b.binding))
                                         throw Error(
                                             std::string("static register address conflict:\n  ") +
                                                 std::string(decoder.scopes_[a.scope].name) + " " +
                                                 std::to_string(overlap.begin) + "\n  " +
                                                 std::string(decoder.scopes_[b.scope].name) +
                                                 " maps different registers to the same bytes",
                                             ErrorKind::Configuration);
                                     return false;
                                 });
                if (shared && decoder.compare(a.scope, b.scope) == 0)
                    throw Error(std::string("same-priority static register address conflict:\n  ") +
                                    std::string(decoder.scopes_[a.scope].name) + "\n  " +
                                    std::string(decoder.scopes_[b.scope].name),
                                ErrorKind::Configuration);
            }
        }
    }
};
void install_formula_layout(RegisterMap& map, void* root, FormulaPlanView plan) {
    FormulaInstaller::install(map, root, plan);
}
} // namespace reg::detail
