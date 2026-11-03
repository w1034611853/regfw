#include "internal/runtime_impl.hpp"
#include <sstream>
#include <iomanip>
#include <set>

namespace reg {
std::vector<LayoutWarning> RegisterMap::layout_warnings() const {
    require_initialized();
    std::vector<LayoutWarning> out = mapping_warnings_;
    // Only named identity-domain predicates are statically inspectable. Arbitrary
    // predicates/resolvers are never invoked. A missing domain is suspicious,
    // while a write-only command or 32-bit wide-register beat is legitimate.
    std::set<std::pair<std::string, std::uint32_t>> seen;
    for (const auto* node : nodes_)
        for (const auto& group : node->derivations)
            if (group.descriptions)
                for (const auto& rule : *group.descriptions)
                    if (rule.condition_domain) {
                        const auto domain = *rule.condition_domain;
                        const bool declared =
                            std::any_of(identities_.begin(), identities_.end(), [&](const auto& w) {
                                return w.domain == domain;
                            });
                        if (!declared && seen.emplace(group.name, domain).second)
                            out.push_back({"REGFW_MISSING_IDENTITY_DOMAIN",
                                           group.name,
                                           "identity_domain(" + std::to_string(domain) +
                                               ") has no declared identity window; conditional "
                                               "mapping always falls back",
                                           rule.condition_description.location});
                    }
    return out;
}

std::string RegisterMap::explain_address(std::uint64_t address,
                                         unsigned size,
                                         RegTransactionKind kind,
                                         std::optional<std::uint32_t> known_identity) const {
    require_initialized();
    std::ostringstream out;
    auto hex = [](std::uint64_t v) {
        std::ostringstream s;
        s << "0x" << std::hex << std::setfill('0') << std::setw(8) << v;
        return s.str();
    };
    out << "ADDRESS EXPLANATION (static; user callbacks are not executed)\n"
        << "BUS " << (kind == RegTransactionKind::Write ? "write" : "read") << ' ' << hex(address)
        << " size=" << size << " bytes\n";
    try {
        if (!RegisterAccessWidthPolicy::size_bit(size) || (address & (size - 1)))
            throw Error("invalid/unaligned access", ErrorKind::Unsupported);
        if (address >= size_ || size > size_ - address)
            throw Error("address outside map", ErrorKind::Decode);
        std::vector<AliasHop> hops;
        auto trace = [&](std::uint64_t from) {
            hops.clear();
            auto alias =
                compiled_view_mode_ ? resolve_static(from, size) : walk_alias(from, size, &hops);
            if (alias.compiled) {
                out << "  -> VIEW " << alias.view_name << " priority=" << alias.priority << ": "
                    << hex(from) << " -> " << hex(alias.target) << '\n';
                return alias;
            }
            alias.policy = alias.policy.intersect(view_policy(from, size));
            for (const auto& hop : hops)
                out << "  -> " << (hop.window->terminal ? "WINDOW " : "ALIAS ") << hop.window->name
                    << ": " << hex(hop.from) << " -> " << hex(hop.to)
                    << " route=" << static_cast<unsigned>(hop.window->route.kind) << '\n';
            if (hops.empty())
                out << "  -> DIRECT " << hex(alias.target) << '\n';
            return alias;
        };
        auto alias = trace(address);
        auto hint = alias.instance;
        if (!hint)
            if (auto* entry = lookup_.get(address))
                hint = entry->source_instance;
        const detail::IdentityWindow* identity = nullptr;
        std::pair<std::uint32_t, std::uint64_t> match{};
        auto match_identity = [&](std::uint64_t a, std::optional<std::uint32_t> h) {
            for (const auto& w : identities_)
                if (auto m = w.match(a, size, h)) {
                    identity = &w;
                    match = *m;
                    return;
                }
        };
        if (alias.compiled && alias.identity_index) {
            const auto& w = identities_.at(*alias.identity_index);
            if (auto m = w.match(address, size, hint)) {
                identity = &w;
                match = *m;
            }
        } else if (!alias.compiled)
            match_identity(address, hint);
        const bool tree_entrance =
            alias.compiled ||
            std::any_of(view_policies_.begin(), view_policies_.end(), [&](const auto& w) {
                return address >= w.base && address - w.base < w.size;
            });
        if (!identity && alias.target != address && !tree_entrance) {
            std::optional<std::uint32_t> canonical_hint;
            if (auto* entry = lookup_.get(alias.target))
                canonical_hint = entry->source_instance;
            match_identity(alias.target, canonical_hint);
        }
        bool unresolved_identity = false;
        if (identity) {
            const auto& w = *identity;
            const auto& description = w.resolver ? w.resolver->description : identity_description_;
            const auto key = w.resolver ? w.resolver->name : std::string("global identity");
            const bool bound = w.resolver ? bool(w.resolver->callback) : bool(identity_resolver_);
            out << "  -> IDENTITY " << w.name << " domain=" << w.domain
                << " request_id=" << match.first << " resolver=" << key << " ("
                << (bound ? "bound" : "unbound") << ")";
            if (!description.name.empty())
                out << " function=" << description.name;
            out << '\n';
            if (known_identity) {
                if (*known_identity >= w.count)
                    throw Error("supplied resolved identity out of range", ErrorKind::Identity);
                out << "     resolved_id=" << *known_identity
                    << " (supplied assumption; resolver not executed)\n";
                if (w.mode == IdentityRemapMode::RedirectBank) {
                    const auto target_address =
                        w.base + std::uint64_t(*known_identity) * w.stride + match.second;
                    auto target = trace(target_address);
                    alias.policy = alias.policy.intersect(target.policy);
                    alias.target = target.target;
                    alias.defined = target.defined;
                    alias.target_entry = target.target_entry;
                    alias.pre_access_chain = target.pre_access_chain;
                }
            } else {
                unresolved_identity = true;
                out << "     resolved_id=? in [0," << w.count - 1 << "]\n";
                if (w.mode == IdentityRemapMode::RedirectBank)
                    out << "     address formula: " << hex(w.base) << " + resolved_id * "
                        << hex(w.stride) << " + " << hex(match.second)
                        << " -> aliases -> register\n"
                        << "     following target is the REQUEST BRANCH ONLY, not a resolved "
                           "destination\n";
            }
        } else
            out << "  -> IDENTITY none\n";
        if (alias.pre_access_chain)
            for (const auto& step : alias.pre_access_chain->steps)
                out << "  -> VIEW PRE_ACCESS " << step.view << " key=" << step.slot->name << " ("
                    << (step.slot->callback ? "bound" : "unbound") << ")\n";
        const bool allowed =
            kind == RegTransactionKind::Write ? alias.policy.allow_write : alias.policy.allow_read;
        out << "  -> WINDOW ACCESS " << (allowed ? "allowed" : "denied") << '\n';
        if (!allowed) {
            const auto action = kind == RegTransactionKind::Write ? alias.policy.denied_write
                                                                  : alias.policy.denied_read;
            out << "     result="
                << (action == DeniedAccessAction::Ignore ? "Ignored" : "AccessError")
                << "; index and device callbacks skipped\n";
        }
        if (!alias.defined)
            throw Error("unmapped entity view address", ErrorKind::Decode);
        const auto* entry = alias.target_entry ? &*alias.target_entry : lookup_.get(alias.target);
        if (!entry) {
            throw Error(unresolved_identity ? "request branch is unmapped; resolved branch unknown"
                                            : "unmapped register address",
                        ErrorKind::Decode);
        }
        const auto& n = *entry->node;
        const auto offset = entry->byte_offset + unsigned(alias.target & 3);
        out << "  -> REGISTER " << entry->view->elements[entry->linear].name
            << " width=" << n.value_size * 8 << " bits byte_offset=" << offset << '\n'
            << "     entity=" << n.name << " request_element=" << entry->linear << " at "
            << n.declaration_location.file_name() << ':' << n.declaration_location.line() << '\n';
        const bool ro =
            kind == RegTransactionKind::Write && !n.custom_behavior && n.builtin == Access::Ro;
        if (ro)
            out << "     read-only write: Ignored; index, pre_access, transaction staging and "
                   "on_write skipped\n";
        bool remapped = false;
        for (const auto& group : n.derivations)
            if (group.descriptions)
                for (const auto& rule : *group.descriptions) {
                    out << "  -> INDEX " << group.name << ": " << rule.source << "=index["
                        << rule.dimension << "] -> " << rule.output;
                    if (rule.condition_domain)
                        out << " when identity_domain(" << *rule.condition_domain << ")";
                    else if (rule.conditional)
                        out << " when user predicate";
                    if (!rule.fallback.empty())
                        out << " otherwise " << rule.fallback;
                    const auto resolver =
                        rule.resolver_key ? rule.resolver_name : rule.resolver_description.name;
                    out << " via " << (resolver.empty() ? "inline resolver" : resolver)
                        << " (not executed" << (rule.metadata_only ? ", metadata only" : "")
                        << ")\n";
                    remapped |= !rule.metadata_only;
                }
        Resolution validation{
            *entry, {alias.target, alias.route, alias.policy, alias.pre_access_chain}, {}, {}};
        validation.info.kind = kind;
        validation.info.size = static_cast<std::uint8_t>(size);
        validation.info.internal_byte_offset = static_cast<std::uint8_t>(offset);
        validate_access(validation);
        if (size == 8) {
            const auto* hi = lookup_.get(alias.target + 4);
            if (alias.target % 8 ||
                (!alias.compiled &&
                 (!hi || hi->node != entry->node || hi->linear != entry->linear ||
                  hi->byte_offset != entry->byte_offset + 4)))
                throw Error("invalid 64-bit access pair", ErrorKind::Unsupported);
        }
        const auto read_policy = n.transaction_policy.read;
        if (kind == RegTransactionKind::Read && read_policy != SplitReadMode::Independent) {
            const auto first =
                read_policy == SplitReadMode::SnapshotOnLow ? 0 : n.value_size - size;
            out << "  -> TRANSACTION first beat byte_offset=" << first << " size=" << size
                << "; continuation needs an active snapshot\n";
        }
        out << "  -> DESTINATION " << n.name << " element=";
        if (remapped && allowed && !ro)
            out << "? (index resolver result required)";
        else
            out << entry->linear;
        if (unresolved_identity)
            out << "; identity result also unresolved";
        out << "\n";

    } catch (const Error& error) {
        out << "  -> STOP: " << error.what() << '\n';
    }
    return out.str();
}
} // namespace reg
