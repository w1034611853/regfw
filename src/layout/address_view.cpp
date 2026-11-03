#include "internal/runtime_impl.hpp"
#include <map>
#include <set>
#include <tuple>
#include <sstream>

namespace reg {
namespace detail {
ViewDescription ViewDescription::with_target(detail::WindowTarget target) const {
    if (use_ != Use::Container)
        throw Error("window already has an access purpose", ErrorKind::Configuration);
    auto out = *this;
    out.use_ = Use::Mapping;
    out.target_ = std::move(target);
    return out;
}
ViewDescription ViewDescription::reserved() const {
    if (use_ != Use::Container)
        throw Error("window already has an access purpose", ErrorKind::Configuration);
    auto out = *this;
    out.use_ = Use::Reserved;
    return out;
}

std::uint64_t window_target_span(const WindowTarget& target) {
    if (target.lifetime.expired() || target.nodes.empty() || !target.slice || !target.count)
        throw Error("view requires a live nonempty register span", ErrorKind::Configuration);
    const auto& n = *target.nodes.front();
    const auto& addresses = n.layout().local_addresses;
    if (target.first >= addresses.size() || target.count > addresses.size() - target.first)
        throw Error("register span outside entity", ErrorKind::Configuration);
    const auto start = addresses[target.first];
    std::uint64_t end = start;
    for (std::size_t i = target.first; i < target.first + target.count; ++i) {
        if (addresses[i] < start)
            throw Error("register span has addresses below its origin", ErrorKind::Configuration);
        end = std::max(end, addresses[i]);
    }
    return end - start + std::max((n.value_size + 3u) & ~3u, 4u);
}
WindowTarget Access::window_target(const Bundle& bundle) {
    WindowTarget out;
    out.owner = bundle.scope_.map;
    out.lifetime = bundle.scope_.reset_set->lifetime;
    out.nodes = bundle.scope_.reset_set->nodes;
    out.base = bundle.region().base();
    out.name = bundle.region().name();
    return out;
}

class ViewCompiler {
    RegisterMap& map_;
    using Use = ViewDescription::Use;
    static constexpr auto none = ~std::size_t{};
    struct Element {
        EntityState* node;
        std::size_t index;
        std::uint64_t offset;
    };
    struct Flat {
        const ViewDescription* window;
        std::uint64_t base;
        std::size_t parent, scope = none;
        std::string name;
        std::optional<std::uint32_t> instance;
        AccessRouteKind route = AccessRouteKind::Direct;
        std::uint64_t repeat_count{1}, repeat_stride{}, repeat_index{};
        std::vector<Element> elements;
    };
    struct Canonical {
        std::vector<std::uint64_t> addresses;
        std::vector<bool> mapped;
        std::vector<std::size_t> owners;
    };
    std::vector<Flat> flat_;
    std::unordered_map<EntityState*, Canonical> canonical_;
    std::unordered_map<const void*, std::size_t> groups_;

    [[noreturn]] static void fail(std::string message) {
        throw Error(std::move(message), ErrorKind::Configuration);
    }
    [[noreturn]] static void conflict(std::string_view reason,
                                      const Flat& current,
                                      std::string_view other,
                                      std::uint64_t base,
                                      std::uint64_t size,
                                      std::source_location at) {
        throw Error(
            std::string(reason) + ":\n  " +
                declaration(
                    current.name, current.base, current.window->size_, current.window->location_) +
                "\n  " + declaration(other, base, size, at),
            ErrorKind::Configuration,
            current.window->location_);
    }
    static unsigned occupied(const EntityState& n) {
        return std::max((n.value_size + 3u) & ~3u, 4u);
    }
    static bool fits(std::uint64_t offset, std::uint64_t size, std::uint64_t parent) {
        return size && offset <= parent && size <= parent - offset;
    }
    static std::string path(std::string_view parent, const ViewDescription& w) {
        auto out = join_name(parent, w.name_);
        if (w.repeat_group_)
            out += "[" + std::to_string(w.repeat_index_) + "]";
        return out;
    }
    static bool same_shape(const ViewDescription& a, const ViewDescription& b) {
        const auto offset_a = a.repeat_group_ ? a.template_offset_ : a.offset_;
        const auto offset_b = b.repeat_group_ ? b.template_offset_ : b.offset_;
        if (a.name_ != b.name_ || a.size_ != b.size_ || offset_a != offset_b || a.use_ != b.use_ ||
            a.domain_ != b.domain_ || a.resolver_key_ != b.resolver_key_ ||
            a.pre_access_key_ != b.pre_access_key_ ||
            bool(a.pre_access_inline_) != bool(b.pre_access_inline_) || a.route_ != b.route_ ||
            a.children_.size() != b.children_.size() || a.priority_ != b.priority_ ||
            a.repeat_count_ != b.repeat_count_ || a.repeat_stride_ != b.repeat_stride_ ||
            a.policy_.allow_read != b.policy_.allow_read ||
            a.policy_.allow_write != b.policy_.allow_write ||
            a.policy_.denied_read != b.policy_.denied_read ||
            a.policy_.denied_write != b.policy_.denied_write)
            return false;
        for (std::size_t i = 0; i < a.children_.size(); ++i)
            if (!same_shape(a.children_[i], b.children_[i]))
                return false;
        return true;
    }
    Canonical& canonical(EntityState* node) {
        auto [it, inserted] = canonical_.try_emplace(node);
        if (inserted) {
            const auto& primary = node->layout();
            for (const auto& e : primary.elements)
                it->second.addresses.push_back(e.address);
            it->second.mapped = primary.element_mapped;
            it->second.owners.assign(primary.elements.size(), none);
        }
        return it->second;
    }
    void target(Flat& f) {
        const auto& t = f.window->target_;
        if (t.lifetime.expired() || t.owner != &map_)
            fail("view must reference live entities in its own map: " + f.name);
        if (t.nodes.empty())
            fail("cannot map an empty entity group: " + f.name);
        std::uint64_t origin = t.base;
        if (t.slice) {
            const auto& n = *t.nodes.front();
            if (!t.count || t.first >= n.element_count() || t.count > n.element_count() - t.first)
                fail("invalid empty/out-of-range mapped RegView");
            origin = n.layout().local_addresses.at(t.first);
        }
        std::uint64_t extent = 0;
        for (auto* n : t.nodes) {
            if (n->owner != &map_)
                fail("cross-map entity in view");
            const auto first = t.slice ? t.first : 0;
            const auto count = t.slice ? t.count : n->element_count();
            for (std::size_t k = first; k < first + count; ++k) {
                const auto a = n->layout().local_addresses[k];
                if (a < origin || !fits(a - origin, occupied(*n), f.window->size_))
                    conflict("mapped entity outside view (entity range uses its local offsets)",
                             f,
                             n->name,
                             a,
                             occupied(*n),
                             n->declaration_location);
                f.elements.push_back({n, k, a - origin});
                extent = std::max(extent, a - origin + occupied(*n));
            }
        }
        if (t.slice && extent != f.window->size_)
            fail("mapped RegView size differs from view span: " + f.name);
    }
    void flatten(const ViewDescription& w,
                 std::uint64_t parent_base,
                 std::uint64_t parent_size,
                 std::size_t parent,
                 std::string parent_name,
                 std::optional<std::uint32_t> instance,
                 AccessRouteKind route = AccessRouteKind::Direct) {
        if (w.name_.empty() || w.offset_ % 4 || w.size_ % 4 ||
            !fits(w.offset_, w.size_, parent_size))
            throw Error(
                "invalid/out-of-parent view: " +
                    declaration(
                        path(parent_name, w), parent_base + w.offset_, w.size_, w.location_) +
                    "; parent base=" + std::to_string(parent_base) +
                    " size=" + std::to_string(parent_size),
                ErrorKind::Configuration,
                w.location_);
        if (w.resolver_key_ && !w.domain_)
            fail("using_resolver requires view.identity(domain): " + w.name_);
        if ((w.use_ == Use::Reserved) && !w.children_.empty())
            fail("reserved window cannot contain children");
        const auto base = parent_base + w.offset_;
        auto name = parent == none && w.name_ == map_.name_prefix ? w.name_ : path(parent_name, w);
        if (w.repeat_group_)
            instance = static_cast<std::uint32_t>(w.repeat_index_);
        if (route == AccessRouteKind::Direct)
            route = w.route_;
        const auto count = w.repeat_group_  ? w.repeat_count_
                           : parent == none ? 1
                                            : flat_[parent].repeat_count;
        const auto stride = w.repeat_group_  ? w.repeat_stride_
                            : parent == none ? 0
                                             : flat_[parent].repeat_stride;
        const auto index = w.repeat_group_  ? w.repeat_index_
                           : parent == none ? 0
                                            : flat_[parent].repeat_index;
        const auto id = flat_.size();
        flat_.push_back(
            {&w, base, parent, none, std::move(name), instance, route, count, stride, index, {}});
        if (w.use_ == Use::Mapping)
            target(flat_.back());
        if (w.repeat_group_) {
            auto [it, inserted] = groups_.try_emplace(w.repeat_group_.get(), id);
            if (!inserted) {
                const auto& first = *flat_[it->second].window;
                if (!same_shape(first, w))
                    throw Error("repeated windows must have the same sub-window geometry, policies "
                                "and route declarations:\n  " +
                                    declaration(flat_[it->second].name,
                                                flat_[it->second].base,
                                                first.size_,
                                                first.location_) +
                                    "\n  " +
                                    declaration(flat_[id].name, base, w.size_, w.location_),
                                ErrorKind::Configuration,
                                w.location_);
            }
        }
        for (const auto& child : w.children_)
            if (!fits(child.offset_, child.size_, w.size_))
                throw Error(
                    "child outside view:\n  " +
                        declaration(path(flat_[id].name, child),
                                    base + child.offset_,
                                    child.size_,
                                    child.location_) +
                        "\nparent: " + declaration(flat_[id].name, base, w.size_, w.location_),
                    ErrorKind::Configuration,
                    child.location_);
        // Reacquire the name after every recursive append; vector storage moves.
        const auto current_name = flat_[id].name;
        for (const auto& child : w.children_)
            flatten(child, base, w.size_, id, current_name, instance, route);
    }
    // Resolve static region composition once. Runtime entries carry the selected
    // entity directly; a physical address is never used as a storage identity.
    void compile_views() {
        bool legacy = false;
        map_.lookup_.for_each([&](auto, const auto&) {
            legacy = true;
        });
        if (legacy || map_.compiled_view_mode_)
            fail("AddressSpace requires a fresh storage-only entity layout");
        for (std::size_t id = 0; id < flat_.size(); ++id) {
            const auto& f = flat_[id];
            for (const auto& e : f.elements) {
                auto& c = canonical(e.node);
                if (!c.mapped[e.index]) {
                    c.addresses[e.index] = f.base + e.offset;
                    c.mapped[e.index] = true;
                    c.owners[e.index] = id;
                }
            }
        }
        struct Identity {
            IdentityWindow window;
            const ViewDescription* declaration;
        };
        std::vector<Identity> identities;
        std::vector<std::optional<std::size_t>> identity_for(flat_.size());
        for (std::size_t id = 0; id < flat_.size(); ++id) {
            const auto& f = flat_[id];
            const auto& w = *f.window;
            if (f.parent != none)
                identity_for[id] = identity_for[f.parent];
            if (!w.domain_)
                continue;
            if (identity_for[id])
                fail("nested identity transforms are not supported in one view path: " + f.name);
            const auto stride = f.repeat_stride ? f.repeat_stride : w.size_;
            const auto base = f.base - f.repeat_index * stride;
            std::size_t index = 0;
            for (; index < identities.size(); ++index) {
                const auto& x = identities[index];
                if (x.window.base == base && x.window.count == f.repeat_count &&
                    x.window.stride == stride && x.window.aperture_size == w.size_ &&
                    x.window.domain == *w.domain_ &&
                    x.declaration->resolver_key_ == w.resolver_key_)
                    break;
            }
            if (index == identities.size())
                identities.push_back({{base,
                                       f.repeat_count,
                                       stride,
                                       w.size_,
                                       0,
                                       w.size_,
                                       *w.domain_,
                                       IdentityRemapMode::RedirectBank,
                                       {},
                                       f.name,
                                       {},
                                       w.location_},
                                      &w});
            identity_for[id] = map_.identities_.size() + index;
        }
        std::vector<std::unique_ptr<std::string>> names;
        for (const auto& f : flat_)
            names.push_back(std::make_unique<std::string>(f.name));
        std::vector<std::unique_ptr<ViewPreAccessChain>> pre_chains;
        std::vector<const ViewPreAccessChain*> pre_for(flat_.size());
        for (std::size_t id = 0; id < flat_.size(); ++id) {
            const auto& f = flat_[id];
            const auto& w = *f.window;
            const auto* parent = f.parent == none ? nullptr : pre_for[f.parent];
            pre_for[id] = parent;
            auto slot = w.pre_access_inline_;
            if (w.pre_access_key_) {
                slot = map_.view_pre_access_slot(w.pre_access_key_, w.pre_access_name_);
                slot->required = true;
            }
            if (!slot)
                continue;
            auto chain = parent ? std::make_unique<ViewPreAccessChain>(*parent)
                                : std::make_unique<ViewPreAccessChain>();
            chain->steps.push_back({std::move(slot), *names[id]});
            pre_for[id] = chain.get();
            pre_chains.push_back(std::move(chain));
        }
        for (auto& f : flat_) {
            f.scope = map_.register_layout_scope(f.base,
                                                 f.window->size_,
                                                 f.name,
                                                 {},
                                                 f.instance,
                                                 f.parent == none ? none : flat_[f.parent].scope,
                                                 f.window->repeat_count_,
                                                 f.window->repeat_stride_,
                                                 f.window->repeat_index_);
            auto& scope = map_.layout_scopes_[f.scope];
            scope.recursive_window = true;
            scope.purpose = f.window->use_ == Use::Reserved ? "RESERVED" : "VIEW";
            if (f.window->use_ == Use::Mapping) {
                const auto& target = f.window->target_;
                scope.purpose +=
                    " → entity: " + (target.slice ? target.nodes.front()->name : target.name);
            }
            if (f.window->pre_access_key_)
                scope.purpose += "; pre_access=" + f.window->pre_access_name_;
            else if (f.window->pre_access_inline_)
                scope.purpose += "; pre_access=inline";
            scope.purpose += "; priority=" + std::to_string(f.window->priority_);
            if (!f.window->target_.nodes.empty())
                scope.purpose +=
                    "; reset=" +
                    std::string(
                        f.window->target_.nodes.front()->reset_scope.kind == ResetScope::Pf ? "PF"
                        : f.window->target_.nodes.front()->reset_scope.kind == ResetScope::Vf
                            ? "VF"
                            : "Global");
        }
        std::vector<AliasAccessPolicy> effective(flat_.size());
        std::vector<std::int32_t> outer_priority(flat_.size());
        for (std::size_t id = 0; id < flat_.size(); ++id) {
            const auto& f = flat_[id];
            const auto& w = *f.window;
            effective[id] = f.parent == none ? w.policy_ : w.policy_.intersect(effective[f.parent]);
            outer_priority[id] =
                w.priority_ ? w.priority_ : (f.parent == none ? 0 : outer_priority[f.parent]);
        }
        const auto identity_base = static_cast<std::uint32_t>(map_.identities_.size());
        std::vector<FormulaDecoder::Scope> formula_scopes(flat_.size());
        for (std::size_t id = 0; id < flat_.size(); ++id) {
            const auto& f = flat_[id];
            auto& s = formula_scopes[id];
            s.parent = f.parent == none ? STATIC_NONE : static_cast<std::uint32_t>(f.parent);
            s.base = f.base;
            s.size = f.window->size_;
            s.priority = outer_priority[id];
            s.outer_priority = outer_priority[id];
            s.instance = f.instance;
            s.route = f.route;
            s.policy = effective[id];
            s.pre_access_chain = pre_for[id];
            s.identity = identity_for[id]
                             ? static_cast<std::uint32_t>(*identity_for[id] - identity_base)
                             : STATIC_NONE;
            s.name = *names[id];
        }
        std::vector<FormulaDecoder::Mount> formula_mounts;
        std::vector<FormulaDecoder::Binding> formula_bindings;
        std::vector<FormulaArraySpec> formula_arrays;
        std::vector<FormulaDecoder::Reserve> formula_reserves;
        // Storage for Table-kind record tables; inner vectors are stable, so
        // spans handed to the array specs stay valid.
        std::vector<std::vector<ElementRecord>> record_tables;
        // Rule arrays share one formula per declaration shape (never one per
        // element, never one per mapped word). Key: full lattice geometry.
        struct ArrayKey {
            FormulaDecodeKind kind;
            std::uint32_t count, occupied, rank;
            std::array<std::uint64_t, MAX_REG_DIMS> strides;
            std::array<std::uint32_t, MAX_REG_DIMS> extents;
            bool operator<(const ArrayKey& o) const {
                if (kind != o.kind)
                    return kind < o.kind;
                if (count != o.count)
                    return count < o.count;
                if (occupied != o.occupied)
                    return occupied < o.occupied;
                if (rank != o.rank)
                    return rank < o.rank;
                if (strides != o.strides)
                    return strides < o.strides;
                return extents < o.extents;
            }
        };
        std::map<ArrayKey, std::uint32_t> array_for;
        std::map<std::tuple<EntityState*, std::uint64_t, std::uint32_t>, std::uint32_t> binding_for;
        auto binding_id =
            [&](EntityState* node, std::uint64_t canonical_base, std::uint32_t index) {
                const auto key = std::make_tuple(node, canonical_base, index);
                auto bit = binding_for.find(key);
                if (bit != binding_for.end())
                    return bit->second;
                const auto binding = static_cast<std::uint32_t>(formula_bindings.size());
                formula_bindings.push_back({node, canonical_base, index});
                binding_for.emplace(key, binding);
                return binding;
            };
        auto push_array = [&](const FormulaArraySpec& spec) {
            // Table records are *not* determined by count/width/rank/strides.
            // In a dynamic View, two independently mapped sequences can have
            // identical shape metadata but different physical element offsets.
            // Until the complete offset/linear record sequence is compared,
            // a Table must have its own FormulaArraySpec and backing records.
            if (spec.kind == FormulaDecodeKind::Table) {
                const auto id = static_cast<std::uint32_t>(formula_arrays.size());
                formula_arrays.push_back(spec);
                return id;
            }
            ArrayKey key{spec.kind, spec.count, spec.occupied, spec.rank, {}, {}};
            for (std::size_t d = 0; d < spec.rank && d < MAX_REG_DIMS; ++d) {
                key.strides[d] = spec.strides_decl[d];
                key.extents[d] = spec.extents_decl[d];
            }
            auto it = array_for.find(key);
            if (it != array_for.end())
                return it->second;
            const auto id = static_cast<std::uint32_t>(formula_arrays.size());
            formula_arrays.push_back(spec);
            array_for.emplace(key, id);
            return id;
        };
        auto scalar_array = [&](std::uint32_t occupied) {
            FormulaArraySpec a{};
            a.count = 1;
            a.occupied = occupied;
            a.span = occupied;
            a.kind = FormulaDecodeKind::Scalar;
            return push_array(a);
        };
        // Emit one mount per maximal same-node segment whose mapped offsets
        // form a decodable lattice; a segment that cannot be classified falls
        // back to the shared sorted record table (still one mount, per-shape
        // storage). This is what keeps dynamic install cost proportional to
        // REG declarations instead of element counts.
        auto emit_segment = [&](std::size_t id, const Flat& f, std::size_t k, std::size_t j) {
            auto* node = f.elements[k].node;
            const auto occupied =
                static_cast<std::uint32_t>(std::max((node->value_size + 3u) & ~3u, 4u));
            const auto count = static_cast<std::uint32_t>(j - k);
            const std::uint64_t first = f.base + f.elements[k].offset;
            // Canonical addresses of the segment must move in lockstep with
            // the view offsets, otherwise the pattern cannot describe them.
            auto& c0 = canonical(node);
            const auto canonical_ok = [&] {
                for (std::size_t m = k; m < j; ++m) {
                    auto& c = canonical(f.elements[m].node);
                    if (c.addresses[f.elements[m].index] - c0.addresses[f.elements[k].index] !=
                        f.elements[m].offset - f.elements[k].offset)
                        return false;
                }
                return true;
            }();
            FormulaArraySpec spec{};
            spec.count = count;
            spec.occupied = occupied;
            bool decodable = false;
            if (count == 1) {
                spec.kind = FormulaDecodeKind::Scalar;
                spec.span = occupied;
                decodable = true;
            } else if (canonical_ok) {
                // Rank-1 affine: constant stride between consecutive elements.
                std::uint64_t stride = f.elements[k + 1].offset - f.elements[k].offset;
                bool affine = stride >= occupied;
                for (std::size_t m = k + 2; affine && m < j; ++m)
                    if (f.elements[m].offset - f.elements[m - 1].offset != stride)
                        affine = false;
                if (affine) {
                    spec.rank = 1;
                    spec.monotone = true;
                    spec.dense = stride == occupied;
                    spec.strides_decl[0] = stride;
                    spec.extents_decl[0] = count;
                    spec.multiplier_decl[0] = 1;
                    spec.strides_asc[0] = stride;
                    spec.extents_asc[0] = count;
                    spec.axis_asc[0] = 0;
                    spec.multiplier[0] = 1;
                    if (spec.dense) {
                        spec.kind = FormulaDecodeKind::Dense;
                        spec.span = std::uint64_t(count) * occupied;
                    } else {
                        spec.kind = FormulaDecodeKind::Monotone;
                        spec.span = std::uint64_t(count - 1) * stride + occupied;
                    }
                    decodable = true;
                } else {
                    // Whole-entity lattice: dims x strides verified element by
                    // element against the mapped offsets.
                    std::uint64_t product = 1;
                    for (auto d : node->dims)
                        product *= d;
                    const auto& strides = node->layout().strides;
                    const auto rank = node->dims.size();
                    if (count == product && rank >= 1 && rank <= MAX_REG_DIMS &&
                        strides.size() == rank) {
                        bool lattice = true;
                        std::uint64_t expected = 0;
                        std::array<std::uint32_t, MAX_REG_DIMS> coords{};
                        for (std::size_t m = k + 1; lattice && m < j; ++m) {
                            // Element order is declaration-linear: the LAST
                            // declaration axis is the fastest-varying digit.
                            for (std::size_t d = rank; d-- > 0;) {
                                if (++coords[d] < node->dims[d])
                                    break;
                                coords[d] = 0;
                            }
                            expected = 0;
                            for (std::size_t d = 0; d < rank; ++d)
                                expected += std::uint64_t(coords[d]) * strides[d];
                            if (f.elements[m].offset - f.elements[k].offset != expected)
                                lattice = false;
                        }
                        if (lattice) {
                            spec.rank = static_cast<std::uint8_t>(rank);
                            std::array<std::size_t, MAX_REG_DIMS> order{};
                            for (std::size_t d = 0; d < rank; ++d)
                                order[d] = d;
                            // rank <= MAX_REG_DIMS (<= 8): insertion sort keeps
                            // GCC's array-bounds analysis honest.
                            for (std::size_t d = 1; d < rank; ++d)
                                for (std::size_t q = d;
                                     q > 0 && strides[order[q]] < strides[order[q - 1]];
                                     --q)
                                    std::swap(order[q], order[q - 1]);
                            // Declaration-order linear index: digit d
                            // contributes its coordinate times the product of
                            // the inner (higher-index) declaration extents.
                            std::uint64_t carry = 1;
                            for (std::size_t d = rank; d-- > 0;) {
                                spec.multiplier_decl[d] = carry;
                                carry *= node->dims[d];
                            }
                            spec.monotone = true;
                            for (std::size_t d = 1; d < rank; ++d)
                                if (strides[order[d]] <
                                    strides[order[d - 1]] * node->dims[order[d - 1]])
                                    spec.monotone = false;
                            for (std::size_t d = 0; d < rank; ++d) {
                                const auto axis = order[d];
                                spec.strides_asc[d] = strides[axis];
                                spec.extents_asc[d] = static_cast<std::uint32_t>(node->dims[axis]);
                                spec.axis_asc[d] = static_cast<std::uint32_t>(axis);
                                spec.multiplier[d] = spec.multiplier_decl[axis];
                                spec.strides_decl[axis] = strides[axis];
                                spec.extents_decl[axis] =
                                    static_cast<std::uint32_t>(node->dims[axis]);
                            }
                            if (spec.monotone) {
                                spec.kind = FormulaDecodeKind::Monotone;
                                std::uint64_t span = occupied;
                                for (std::size_t d = 0; d < rank; ++d)
                                    span += std::uint64_t(node->dims[d] - 1) * strides[d];
                                spec.span = span;
                                decodable = true;
                            } else {
                                // Reuse the compiled-static classifier to keep
                                // stride feasibility and collision rules identical.
                                switch (classify_interleaved(spec)) {
                                    case FormulaClassifyResult::Arithmetic:
                                        decodable = true;
                                        break;
                                    case FormulaClassifyResult::SelfOverlap:
                                        fail("view maps one declaration onto the same bytes twice "
                                             "(self-overlapping strides): " +
                                             f.name);
                                    case FormulaClassifyResult::BadInverse:
                                        fail("view strides have no modular inverse for decoding: " +
                                             f.name);
                                    case FormulaClassifyResult::NeedsTable:
                                        break;
                                }
                            }
                        }
                    }
                }
            }
            if (!decodable) {
                // General fallback: the shared sorted record table. It is per
                // declaration shape, never per word; identical-bytes-twice is
                // rejected (a lattice with duplicate offsets cannot be decoded).
                if (count > (1u << 20))
                    fail("view maps a non-arithmetic register shape with more than 1048576 "
                         "elements: " +
                         f.name);
                record_tables.emplace_back();
                auto& records = record_tables.back();
                records.reserve(count);
                for (std::size_t m = k; m < j; ++m)
                    records.push_back({f.elements[m].offset - f.elements[k].offset,
                                       static_cast<std::uint32_t>(m - k)});
                std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) {
                    return a.offset < b.offset;
                });
                for (std::size_t m = 1; m < records.size(); ++m)
                    if (records[m].offset == records[m - 1].offset)
                        fail("view maps one declaration onto the same bytes twice: " + f.name);
                spec = FormulaArraySpec{};
                spec.kind = FormulaDecodeKind::Table;
                spec.count = count;
                spec.occupied = occupied;
                spec.sparse = records;
                spec.span =
                    records.empty() ? std::uint64_t(occupied) : records.back().offset + occupied;
            }
            const auto array =
                spec.kind == FormulaDecodeKind::Scalar ? scalar_array(occupied) : push_array(spec);
            const auto binding = binding_id(node,
                                            c0.addresses[f.elements[k].index],
                                            static_cast<std::uint32_t>(f.elements[k].index));
            formula_mounts.push_back({first,
                                      first + spec.span,
                                      binding,
                                      static_cast<std::uint32_t>(id),
                                      array,
                                      static_cast<bool>(node->value_size == 4 && node->scalar_data),
                                      0});
        };
        for (std::size_t id = 0; id < flat_.size(); ++id) {
            const auto& f = flat_[id];
            if (f.window->use_ == Use::Reserved) {
                if (f.window->size_ > UINT64_MAX - f.base)
                    throw Error("formula layout: ReserveRangeOverflow", ErrorKind::Configuration);
                formula_reserves.push_back(
                    {f.base, f.base + f.window->size_, static_cast<std::uint32_t>(id)});
                continue;
            }
            // One mount per maximal same-node segment of the mapped span.
            for (std::size_t k = 0; k < f.elements.size();) {
                std::size_t j = k + 1;
                while (j < f.elements.size() && f.elements[j].node == f.elements[k].node)
                    ++j;
                emit_segment(id, f, k, j);
                k = j;
            }
        }
        std::vector<LayoutWarning> warnings;
        auto decoder = std::make_unique<FormulaDecoder>();
        decoder->install_dynamic(&map_,
                                 identity_base,
                                 std::move(formula_scopes),
                                 std::move(formula_mounts),
                                 std::move(formula_bindings),
                                 std::move(formula_arrays),
                                 std::move(record_tables),
                                 std::move(formula_reserves),
                                 &warnings);
        for (auto& [node, c] : canonical_) {
            auto& layout = node->layout();
            for (std::size_t k = 0; k < c.owners.size(); ++k)
                if (c.owners[k] != none) {
                    const auto& f = flat_[c.owners[k]];
                    layout.elements[k].address = c.addresses[k];
                    layout.layout_scope = f.scope;
                    layout.elements[k].name =
                        f.name + "." +
                        node->name.substr(node->name.rfind('.') == std::string::npos
                                              ? 0
                                              : node->name.rfind('.') + 1);
                    if (node->element_count() > 1)
                        for (auto coord : layout.elements[k].index.coordinates())
                            layout.elements[k].name += '[' + std::to_string(coord) + ']';
                    layout.display_name = f.name;
                }
            layout.element_mapped = std::move(c.mapped);
            layout.mapped = true;
        }
        for (auto& item : identities) {
            const auto& w = *item.declaration;
            if (w.resolver_key_) {
                auto& slot = map_.access_resolvers_->identities[w.resolver_key_];
                if (!slot) {
                    slot = std::make_shared<IdentityResolverSlot>();
                    slot->name = w.resolver_name_;
                }
                slot->required = true;
                item.window.resolver = slot;
            } else if (!map_.has_identity_resolver())
                map_.set_identity_resolver("identity passthrough",
                                           [](const IdentityResolveRequest& r) {
                                               return r.request_id;
                                           });
            map_.identities_.push_back(std::move(item.window));
        }
        map_.compiled_names_.swap(names);
        map_.view_pre_access_chains_.swap(pre_chains);
        map_.mapping_warnings_ = std::move(warnings);
        map_.formula_ = std::move(decoder);
        map_.compiled_view_mode_ = true;
    }

  public:
    explicit ViewCompiler(RegisterMap& map) : map_(map) {
    }
    void install(const ViewDescription& root) {
        map_.mutable_layout();
        auto bounded = root;
        bounded.size_ = map_.size_;
        flatten(bounded, 0, map_.size_, none, map_.name_prefix, {});
        compile_views();
    }
};
} // namespace detail

void apply_layout(RegisterMap& map, const AddressSpace& space) {
    detail::ViewCompiler(map).install(space.root_);
}

AliasAccessPolicy RegisterMap::view_policy(std::uint64_t address, unsigned size) const {
    auto policy = AliasAccessPolicy::READ_WRITE;
    for (const auto& w : view_policies_) {
        if (address >= w.base && address - w.base < w.size) {
            if (size > w.size - (address - w.base))
                throw Error("access crosses sub_window boundary", ErrorKind::Decode);
            policy = policy.intersect(w.policy);
        } else if (w.base > address && w.base - address < size)
            throw Error("access crosses sub_window boundary", ErrorKind::Decode);
    }
    return policy;
}

void RegisterMap::bind_window_resolver(const void* key,
                                       std::string_view name,
                                       std::shared_ptr<const IdentityFn> callback,
                                       detail::CallbackDescription description) {
    require_initialized();
    auto& slot = access_resolvers_->identities[key];
    if (!slot) {
        slot = std::make_shared<detail::IdentityResolverSlot>();
        slot->name = name;
    }
    slot->callback = std::move(callback);
    slot->description = std::move(description);
    emit_resolver_update("identity:" + slot->name, slot->description, bool(slot->callback));
}
void RegisterMap::clear_window_resolver(const void* key) {
    require_initialized();
    auto it = access_resolvers_->identities.find(key);
    if (it != access_resolvers_->identities.end()) {
        it->second->callback.reset();
        emit_resolver_update("identity:" + it->second->name, it->second->description, false);
    }
}
bool RegisterMap::has_window_resolver(const void* key) const {
    if (!access_resolvers_)
        return false;
    auto it = access_resolvers_->identities.find(key);
    return it != access_resolvers_->identities.end() && bool(it->second->callback);
}
void RegisterMap::defer_window_resolver(const void* key, std::string_view name) {
    mutable_layout();
    auto& slot = access_resolvers_->identities[key];
    if (!slot) {
        slot = std::make_shared<detail::IdentityResolverSlot>();
        slot->name = name;
    }
    slot->deferred = true;
}
} // namespace reg
