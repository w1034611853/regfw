#include "internal/runtime_impl.hpp"
#include <regfw/declarations.hpp>

namespace reg {
void Groups::group(std::string name, const AccessGroupSpec& spec) {
    group(std::move(name));
    auto& plan = *plans_.back();
    for (const auto& m : spec.members)
        plan.members.push_back({m.node_, m.lifetime_});
    for (const auto& c : spec.coordinates)
        plan.coordinates.push_back({c.id_, c.name_, c.dimension_, c.convert_, c.identity_});
    for (const auto& mapping : spec.derive) {
        auto d = std::make_shared<Derivation>();
        d->source = mapping.source_;
        d->output = mapping.output_;
        d->output_name = mapping.output_name_;
        d->condition = mapping.condition_;
        d->resolver = mapping.resolver_;
        d->fallback = mapping.fallback_;
        d->metadata_only = mapping.metadata_only_;
        d->condition_domain = mapping.condition_domain_;
        d->condition_description = mapping.condition_description_;
        d->resolver_description = mapping.resolver_description_;
        d->resolver_key = mapping.resolver_key_;
        d->resolver_name = mapping.resolver_name_;
        if (mapping.resolver_key_) {
            d->bind = [ptr = d.get(), key = mapping.resolver_key_, name = mapping.resolver_name_](
                          RegisterMap& map) {
                auto slot = map.access_resolver_slot(key, name);
                slot->required = true;
                ptr->resolver_slot = std::move(slot);
            };
        }
        plan.derivations.push_back(std::move(d));
    }
}
detail::DeclaredAccessGroup detail::declare_access_group(std::string_view name,
                                                         const AccessGroupSpec& spec) {
    Groups groups;
    groups.group(std::string(name), spec);
    groups.install();
    return {};
}
void apply_layout(RegisterMap& map, const LayoutSpec& spec) {
    if (spec.root.value)
        apply_layout(map, *spec.root.value);
}
} // namespace reg
