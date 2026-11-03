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
namespace {
void check_window(const RegisterMap& map, PhysicalRepeatAddressWindow w) {
    if (!w.count || w.count > UINT32_MAX || !w.size || !w.stride || w.size > w.stride ||
        w.base % 4 || w.stride % 4 || w.size % 4 || w.base > map.size_bytes() ||
        w.size > map.size_bytes() - w.base ||
        (w.count - 1) > (map.size_bytes() - w.base - w.size) / w.stride)
        throw Error("invalid declarative layout window", ErrorKind::Configuration);
}
void check_range(PhysicalRepeatAddressWindow w, std::uint64_t offset, std::uint64_t size) {
    if (!size || offset % 4 || size % 4 || offset > w.size || size > w.size - offset)
        throw Error("declarative layout range is outside its window", ErrorKind::Configuration);
}
std::string instance_name(const std::string& name, std::uint64_t count, std::uint64_t i) {
    if (name.empty() || count == 1)
        return name;
    return name + "[" + std::to_string(i) + "]";
}
} // namespace
void apply_layout(RegisterMap& map, const LayoutSpec& spec) {
    if (spec.root.value) {
        if (spec.views.size() || spec.aliases.size() || spec.identities.size() ||
            spec.reserved.size())
            throw Error("recursive root cannot be mixed with legacy layout lists",
                        ErrorKind::Configuration);
        apply_layout(map, *spec.root.value);
        return;
    }
    if (!map || map.is_finalized())
        throw Error("declarative layout requires an initialized, unfinished map",
                    ErrorKind::Configuration);
    // Validate all arithmetic and pairings before installing the first window.
    std::vector<const RegViewSpec*> views;
    views.reserve(spec.views.size() + spec.aliases.size());
    for (const auto& value : spec.views)
        views.push_back(&value);
    for (const auto& value : spec.aliases)
        views.push_back(&value);
    for (const auto* declaration : views) {
        const auto& a = *declaration;
        if ((a.source_owner && (a.source_owner != &map || a.source_lifetime.expired())) ||
            (a.target_owner && (a.target_owner != &map || a.target_lifetime.expired())))
            throw Error("RegView must reference a live RegEntity in the same map",
                        ErrorKind::Configuration);
        check_window(map, a.source);
        check_range(a.source, a.offset, a.size);
        if (!a.has_target)
            throw Error("alias declaration requires a target", ErrorKind::Configuration);
        check_window(map, a.target);
        if (a.target.size != a.size || (a.target.count != 1 && a.target.count != a.source.count))
            throw Error("alias target size/count does not match its source",
                        ErrorKind::Configuration);
    }
    for (const auto& i : spec.identities) {
        check_window(map, i.window);
        check_range(i.window, i.offset, i.size);
    }
    for (const auto& r : spec.reserved)
        check_window(map, {r.base, r.count, r.stride, r.size});
    for (const auto* declaration : views)
        for (std::uint64_t i = 0; i < declaration->source.count; ++i) {
            const auto& a = *declaration;
            const auto route = AccessRoute{a.route_kind, static_cast<std::uint32_t>(i)};
            map.map_routed_alias_window_with_policy(
                a.source.base + i * a.source.stride + a.offset,
                a.target.base + (a.target.count == 1 ? 0 : i * a.target.stride),
                a.size,
                route,
                a.policy,
                instance_name(a.label, a.source.count, i),
                a.location);
        }
    RegLayout layout(map);
    for (const auto& i : spec.identities)
        layout.identity_repeat_window(
            i.window, i.domain, i.mode, i.offset, i.size, i.label, {}, i.location);
    // The declarative identity form starts with an identity mapping. Device code
    // may bind/replace its dynamic resolver before guest execution.
    if (spec.identities.size() && !map.has_identity_resolver())
        map.set_identity_resolver("identity passthrough",
                                  [](const IdentityResolveRequest& request) {
                                      return request.request_id;
                                  });
    for (const auto& r : spec.reserved)
        for (std::uint64_t i = 0; i < r.count; ++i)
            map.reserve_region(r.base + i * r.stride,
                               r.size,
                               instance_name(r.label, r.count, i),
                               r.count,
                               r.stride,
                               i,
                               r.location);
}
} // namespace reg
