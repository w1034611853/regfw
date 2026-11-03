#include "internal/runtime_impl.hpp"
namespace reg {
void Groups::apply_index(const detail::StaticIndexRule& rule,
                         const detail::AccessResolverSlot& slot,
                         RegAccessInfo& info) {
    const auto coordinate = info.request_index.dimension(rule.dimension);
    if (!coordinate)
        throw RegAccessDeriveError::device_error("coordinate dimension missing");
    info.index_fields.insert(rule.source, rule.source_name, *coordinate, rule.dimension);
    const auto value = slot(info);
    info.mapped_fields.insert(rule.output, rule.output_name, value, rule.dimension);
}
void Groups::install_index(detail::EntityState* node, detail::StaticIndexRule rule) {
    if (!node || node->owner->is_finalized() || rule.rank != node->dims.size() ||
        rule.dimension >= node->dims.size() || node->dims[rule.dimension] != rule.extent)
        throw Error("static index binding mismatch", ErrorKind::Configuration);
    for (const auto& d : node->derivations)
        if (d.name == rule.name)
            throw Error("access group already installed", ErrorKind::Configuration);
    std::uint64_t coefficient = 1;
    for (std::size_t k = rule.dimension + 1; k < node->dims.size(); ++k)
        coefficient *= node->dims[k];
    if (!rule.source || !rule.output || rule.name.empty() || coefficient != rule.coefficient)
        throw Error("invalid static index rule", ErrorKind::Configuration);
    auto slot = node->owner->access_resolver_slot(rule.output, rule.output_name);
    slot->required = true;
    auto descriptions = std::make_shared<std::vector<detail::IndexRuleDescription>>();
    detail::IndexRuleDescription description;
    description.source = rule.source_name;
    description.output = rule.output_name;
    description.dimension = rule.dimension;
    description.resolver_key = rule.output;
    description.resolver_name = rule.output_name;
    descriptions->push_back(std::move(description));
    node->single_index.reset();
    if (node->derivations.empty())
        node->single_index = std::make_unique<detail::SingleIndexPlan>(detail::SingleIndexPlan{
            {rule.source, rule.output, rule.source_name, rule.output_name, rule.dimension},
            rule.extent,
            rule.coefficient,
            slot.get(),
            false});
    node->derivations.push_back({std::string(rule.name),
                                 [rule, slot](RegAccessInfo& info) {
                                     apply_index(rule, *slot, info);
                                 },
                                 std::move(descriptions)});
}
void Groups::GroupBuilder::add_member(detail::EntityState* node) {
    plan_->members.push_back({node, node->owner->lifetime()});
}
void Groups::Plan::run(RegAccessInfo& info) const {
    // The common one-coordinate named mapping has no field search
    // or conversion work. It still invokes the live resolver slot
    // and leaves range/conflict validation to the normal derive step.
    if (coordinates.size() == 1 && derivations.size() == 1 && info.index_fields.size == 0 &&
        info.mapped_fields.size == 0) {
        const auto& c = coordinates[0];
        const auto& d = *derivations[0];
        if (c.identity && d.source == c.id && d.resolver_slot && !d.condition) {
            auto v = info.request_index.dimension(c.dimension);
            if (!v)
                throw RegAccessDeriveError::device_error("coordinate dimension missing");
            info.index_fields.entries.activate();
            info.mapped_fields.entries.activate();
            info.index_fields.entries[0] = {c.id, c.name, *v, c.dimension};
            info.index_fields.size = 1;
            const auto value = (*d.resolver_slot)(info);
            info.mapped_fields.entries[0] = {d.output,
                                             d.output_name,
                                             value,
                                             d.metadata_only ? std::nullopt
                                                             : std::optional(c.dimension)};
            info.mapped_fields.size = 1;
            return;
        }
    }
    for (const auto& c : coordinates) {
        auto v = info.request_index.dimension(c.dimension);
        if (!v)
            throw RegAccessDeriveError::device_error("coordinate dimension missing");
        info.index_fields.insert(c.id, c.name, c.identity ? *v : c.convert(*v), c.dimension);
    }
    for (const auto& p : derivations) {
        const auto& d = *p;
        RegData value;
        if (!d.condition || d.condition(info))
            value = d.resolver_slot ? (*d.resolver_slot)(info) : d.resolver(info);
        else {
            auto f = info.index_fields.get(d.fallback);
            if (!f)
                throw RegAccessDeriveError::device_error("missing derivation fallback coordinate");
            value = f->value;
        }
        auto source = info.index_fields.get(d.source);
        if (!source || !source->dimension)
            throw RegAccessDeriveError::device_error("missing derivation source coordinate");
        info.mapped_fields.insert(
            d.output, d.output_name, value, d.metadata_only ? std::nullopt : source->dimension);
    }
}
void Groups::install() {
    // Validate all plans before attaching the first callback.
    for (auto& p : plans_) {
        if (p->members.empty())
            throw Error("empty access group", ErrorKind::Configuration);
        for (const auto& member : p->members)
            if (member.lifetime.expired())
                throw Error("access group member outlived its map", ErrorKind::Configuration);
        auto* owner = p->members.front().node->owner;
        for (const auto& member : p->members)
            if (member.node->owner != owner)
                throw Error("access group members must belong to one map",
                            ErrorKind::Configuration);
        std::unordered_set<const void*> coords;
        for (auto& c : p->coordinates) {
            if (!coords.insert(c.id).second)
                throw Error("duplicate coordinate field", ErrorKind::Configuration);
            for (const auto& member : p->members)
                if (c.dimension >= member.node->dims.size())
                    throw Error("access group rank mismatch", ErrorKind::Configuration);
        }
        for (auto& d : p->derivations)
            if (!d->source || !coords.contains(d->source) || (!d->resolver && !d->bind) ||
                (d->condition && (!d->fallback || !coords.contains(d->fallback))))
                throw Error("incomplete access derivation", ErrorKind::Configuration);
        std::unordered_set<detail::EntityState*> members;
        for (const auto& member : p->members) {
            auto* n = member.node;
            if (n->owner->is_finalized() || !members.insert(n).second)
                throw Error("duplicate/finalized access group member", ErrorKind::Configuration);
            for (auto& d : n->derivations)
                if (d.name == p->name)
                    throw Error("access group already installed", ErrorKind::Configuration);
        }
    }
    for (auto& p : plans_)
        for (auto& d : p->derivations)
            if (d->bind)
                d->bind(*p->members.front().node->owner);
    for (auto& p : plans_) {
        auto descriptions = std::make_shared<std::vector<detail::IndexRuleDescription>>();
        descriptions->reserve(p->derivations.size());
        for (const auto& d : p->derivations) {
            detail::IndexRuleDescription item;
            for (const auto& c : p->coordinates) {
                if (c.id == d->source) {
                    item.source = c.name;
                    item.dimension = c.dimension;
                }
                if (c.id == d->fallback)
                    item.fallback = c.name;
            }
            item.output = d->output_name;
            item.conditional = bool(d->condition);
            item.condition_domain = d->condition_domain;
            item.metadata_only = d->metadata_only;
            item.condition_description = d->condition_description;
            item.resolver_description = d->resolver_description;
            item.resolver_key = d->resolver_key;
            item.resolver_name = d->resolver_name;
            descriptions->push_back(std::move(item));
        }
        for (const auto& member : p->members) {
            auto& node = *member.node;
            node.single_index.reset();
            if (node.derivations.empty() && p->coordinates.size() == 1 &&
                p->derivations.size() == 1) {
                const auto& c = p->coordinates.front();
                const auto& d = *p->derivations.front();
                if (c.identity && d.source == c.id && d.resolver_slot && !d.condition) {
                    std::uint32_t coefficient = 1;
                    for (std::size_t k = c.dimension + 1; k < node.dims.size(); ++k)
                        coefficient *= node.dims[k];
                    node.single_index =
                        std::make_unique<detail::SingleIndexPlan>(detail::SingleIndexPlan{
                            {c.id, d.output, c.name, d.output_name, c.dimension},
                            static_cast<std::uint32_t>(node.dims[c.dimension]),
                            coefficient,
                            d.resolver_slot.get(),
                            d.metadata_only});
                }
            }
            node.derivations.push_back({p->name,
                                        [p](RegAccessInfo& i) {
                                            p->run(i);
                                        },
                                        descriptions});
        }
    }
}
std::size_t detail::ResetSet::reset() const {
    if (lifetime.expired())
        throw Error("bundle outlived map");
    for (auto n : nodes)
        n->reset();
    return nodes.size();
}
std::vector<std::uint64_t> RegScope::prepare_reg(std::string_view name,
                                                 const detail::RegisterConfig& spec,
                                                 unsigned value_bits) {
    std::uint64_t extent = spec.offset;
    for (std::size_t d = 0; d < spec.dimensions.size(); ++d) {
        if (!spec.dimensions[d])
            throw Error("zero dimension", ErrorKind::Configuration);
        auto c = spec.dimensions[d] - 1;
        if (c && spec.stride_bytes[d] > (UINT64_MAX - extent) / c)
            throw Error("scope extent overflow", ErrorKind::Configuration);
        extent += c * spec.stride_bytes[d];
    }
    auto occupied = std::max(((value_bits + 7) / 8 + 3) & ~3u, 4u);
    if (extent > size() || occupied > size() - extent)
        throw Error("register outside scope: " +
                        detail::declaration(detail::join_name(region_.name(), name),
                                            base() + spec.offset,
                                            extent - spec.offset + occupied,
                                            spec.location) +
                        "\nparent: " +
                        detail::declaration(region_.name(), base(), size(), region_.location()),
                    ErrorKind::Configuration,
                    spec.location);
    std::size_t count = 1;
    for (auto d : spec.dimensions) {
        if (d > UINT32_MAX || count > UINT32_MAX / d)
            throw Error("scope dimension overflow", ErrorKind::Configuration);
        count *= d;
    }
    std::vector<std::uint64_t> addresses;
    addresses.reserve(count);
    for (std::size_t k = 0; k < count; ++k) {
        auto index = RegElementIndex::from_dims(spec.dimensions, k);
        auto address = spec.offset;
        for (std::size_t d = 0; d < spec.dimensions.size(); ++d)
            address += std::uint64_t(index.values[d]) * spec.stride_bytes[d];
        check_claim(address, occupied, name, spec.location);
        addresses.push_back(address);
    }
    return addresses;
}

void RegScope::finish_reg(std::string_view name,
                          std::span<const std::uint64_t> addresses,
                          unsigned occupied,
                          detail::EntityState* node) {
    for (auto address : addresses)
        claims.try_emplace(
            address, address, occupied, std::string(name), node->declaration_location);
    for (auto set = reset_set; set; set = set->parent)
        set->nodes.push_back(node);
}

detail::EntityState* RegScope::reg_node(std::string_view name,
                                        const detail::RegisterConfig& spec,
                                        const detail::StorageType& type) {
    if (view_only_)
        throw Error("RegViewBundle cannot create RegEntity storage: " + std::string(name),
                    ErrorKind::Configuration);
    auto addresses = prepare_reg(name, spec, type.bits);
    auto* node = map->create_node(base() + spec.offset,
                                  detail::join_name(region_.name(), name),
                                  spec,
                                  type,
                                  instance,
                                  default_reset_scope,
                                  storage_only_);
    node->layout().layout_scope = reset_set->layout_scope;
    finish_reg(name, addresses, std::max(((type.bits + 7) / 8 + 3) & ~3u, 4u), node);
    return node;
}
std::vector<RegisterMetadata> Bundle::metadata() const {
    std::vector<RegisterMetadata> out;
    for (auto n : scope_.reset_set->nodes)
        out.push_back(detail::node_metadata(*n));
    return out;
}
} // namespace reg

namespace reg {
std::span<detail::EntityState* const> detail::Access::entity_nodes(const Bundle& bundle) {
    return bundle.scope_.reset_set->nodes;
}
const RegisterMap* detail::Access::owner(const Bundle& bundle) {
    return bundle.scope_.map;
}
std::weak_ptr<void> detail::Access::lifetime(const Bundle& bundle) {
    return bundle.scope_.reset_set->lifetime;
}
RegScope::RegScope(RegisterMap& m,
                   std::uint64_t base,
                   std::uint64_t size,
                   std::string name,
                   ResetScope rs,
                   std::optional<std::uint32_t> inst,
                   std::shared_ptr<detail::ResetSet> parent,
                   std::uint64_t repeat_count,
                   std::uint64_t repeat_stride,
                   std::uint64_t repeat_index,
                   bool view_only,
                   bool storage_only)
    : map(&m), region_(base, size, std::move(name)), instance(inst), default_reset_scope(rs),
      reset_set(std::make_shared<detail::ResetSet>()), view_only_(view_only),
      storage_only_(storage_only) {
    if (m.is_finalized() || !size || base % 4 || size % 4 || base > m.size_bytes() ||
        size > m.size_bytes() - base)
        throw Error("invalid scope", ErrorKind::Configuration);
    reset_set->parent = std::move(parent);
    reset_set->lifetime = m.lifetime();
    if (!storage_only_)
        reset_set->layout_scope = m.register_layout_scope(
            base,
            size,
            region_.name(),
            rs,
            inst,
            reset_set->parent ? reset_set->parent->layout_scope : ~std::size_t{},
            repeat_count,
            repeat_stride,
            repeat_index);
}

void RegScope::check_claim(std::uint64_t offset,
                           std::uint64_t size,
                           std::string_view name,
                           std::source_location at) const {
    if (map->is_finalized())
        throw Error("scope is finalized", ErrorKind::Configuration);
    if (!size || offset % 4 || size % 4 || offset > region_.size() ||
        size > region_.size() - offset)
        (void)region_.subregion(offset, size, std::string(name), at);
    auto next = claims.lower_bound(offset);
    const RegRegion* conflict = nullptr;
    if (next != claims.end() &&
        detail::overlaps(offset, size, next->second.base(), next->second.size()))
        conflict = &next->second;
    if (next != claims.begin()) {
        const auto& previous = std::prev(next)->second;
        if (detail::overlaps(offset, size, previous.base(), previous.size()))
            conflict = &previous;
    }
    if (conflict) {
        const auto& c = *conflict;
        throw Error("overlapping scope declarations:\n  " +
                        detail::declaration(detail::join_name(region_.name(), c.name()),
                                            base() + c.base(),
                                            c.size(),
                                            c.location()) +
                        "\n  " +
                        detail::declaration(
                            detail::join_name(region_.name(), name), base() + offset, size, at),
                    ErrorKind::Configuration,
                    at);
    }
}

void RegScope::claim(std::uint64_t offset,
                     std::uint64_t size,
                     std::string name,
                     std::source_location at) {
    check_claim(offset, size, name, at);
    claims.try_emplace(offset, offset, size, std::move(name), at);
}

void RegLayout::claim(std::uint64_t base,
                      std::uint64_t size,
                      std::string name,
                      std::source_location at) {
    if (map_.is_finalized() || !size || base % 4 || size % 4 || base > map_.size_bytes() ||
        size > map_.size_bytes() - base)
        throw Error("invalid layout region", ErrorKind::Configuration);
    for (auto& c : claims_)
        if (detail::overlaps(base, size, c.base(), c.size()))
            throw Error("overlapping apertures:\n  " +
                            detail::declaration(c.name(), c.base(), c.size(), c.location()) +
                            "\n  " + detail::declaration(name, base, size, at),
                        ErrorKind::Configuration,
                        at);
    claims_.emplace_back(base, size, std::move(name), at);
}

} // namespace reg
