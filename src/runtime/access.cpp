#include "internal/runtime_impl.hpp"
#include <sstream>
#include <iomanip>
#include <unordered_set>

namespace reg {

// Address helpers and alias resolution.
std::uint64_t RegisterMap::checked_map_size(std::uint64_t size) {
    if (!size || size % 4)
        throw Error("invalid map size", ErrorKind::Configuration);
    return size;
}

std::optional<std::uint32_t> RegisterMap::source_instance(std::uint64_t address) const {
    for (auto it = instances_.rbegin(); it != instances_.rend(); ++it)
        if (address >= it->base && address - it->base < it->size)
            return it->instance;
    return {};
}

const detail::AddressWindow* RegisterMap::window_at(std::uint64_t address) const {
    const detail::AddressWindow* best = nullptr;
    for (const auto& w : windows_)
        if (address >= w.source_base && address - w.source_base < w.size &&
            (!best || w.size < best->size))
            best = &w;
    return best;
}

void RegisterMap::require_initialized() const {
    if (!size_)
        throw Error("register map has not been initialized", ErrorKind::Configuration);
}

void RegisterMap::mutable_layout() const {
    require_initialized();
    if (finalized_)
        throw Error("register layout is finalized", ErrorKind::Configuration);
}

void RegisterMap::region_check(std::uint64_t base, std::uint64_t n) const {
    if (!n || base % 4 || n % 4 || base > size_ || n > size_ - base)
        throw Error("invalid/out-of-range 4-byte-aligned region", ErrorKind::Configuration);
}

RegisterMap::AliasEntry
RegisterMap::walk_alias(std::uint64_t original, unsigned size, std::vector<AliasHop>* hops) const {
    AliasEntry out;
    out.target = original;
    out.available = size_ - original;
    out.instance = source_instance(original);
    std::array<std::uint64_t, 32> visited{};
    unsigned depth = 0;
    for (;;) {
        for (unsigned j = 0; j < depth; ++j)
            if (visited[j] == out.target)
                throw Error("alias cycle", ErrorKind::Decode);
        if (depth)
            out.policy = out.policy.intersect(view_policy(out.target, size));
        auto* w = window_at(out.target);
        if (!w)
            return out;
        if (depth == visited.size())
            throw Error("alias depth exceeded", ErrorKind::Decode);
        visited[depth++] = out.target;
        auto remaining = w->size - (out.target - w->source_base);
        for (const auto& child : windows_)
            if (child.source_base > out.target && child.source_base < w->source_base + w->size)
                remaining = std::min(remaining, child.source_base - out.target);
        if (size > remaining)
            throw Error("access crosses alias window", ErrorKind::Decode);
        out.available = std::min(out.available, remaining);
        if (!out.used)
            out.name = w->name;
        if (!w->terminal)
            out.used = true;
        if (out.route.is_direct())
            out.route = w->route;
        out.policy = out.policy.intersect(w->policy);
        if (!out.instance && !w->route.is_direct() && w->route.kind != AccessRouteKind::Alias)
            out.instance = w->route.instance;
        if (w->mapped_offsets) {
            const auto offset = out.target - w->source_base;
            const auto& ranges = *w->mapped_offsets;
            auto it = std::upper_bound(
                ranges.begin(), ranges.end(), offset, [](auto value, const auto& r) {
                    return value < r.first;
                });
            out.defined =
                it != ranges.begin() && offset - std::prev(it)->first < std::prev(it)->second;
        }
        if (hops)
            hops->push_back(
                {out.target,
                 w->terminal ? out.target : w->target_base + (out.target - w->source_base),
                 w});
        if (w->terminal)
            return out;
        out.target = w->target_base + (out.target - w->source_base);
        // A typed entity mapping ends at stored bytes. Other windows
        // attached to the first address entrance are not part of it.
        if (w->mapped_offsets)
            return out;
    }
}

RegisterMap::AliasEntry RegisterMap::resolve_static(std::uint64_t address, unsigned size) const {
    if (formula_) {
        if (auto* p = formula_->decode(address, access_scratch_->decoded[0])) {
            auto out = *p;
            const auto lane = address & 3;
            if (lane >= out.available || size > out.available - lane)
                throw Error("access crosses selected view target", ErrorKind::Decode);
            out.target += lane;
            out.available -= lane;
            return out;
        }
        AliasEntry hole;
        hole.compiled = true;
        hole.target = address;
        hole.available = size_ - address;
        hole.defined = false;
        return hole;
    }
    if (finalized_) {
        if (auto p = aliases_.get(address)) {
            auto out = *p;
            auto lane = address & 3;
            if (size > out.available - lane)
                throw Error("access crosses alias window", ErrorKind::Decode);
            out.target += lane;
            out.available -= lane;
            return out;
        }
        AliasEntry out;
        out.target = address;
        out.available = size_ - address;
        if (auto p = lookup_.get(address)) {
            out.instance = p->source_instance;
            return out;
        }
        // Only aliases ending at stored registers are cached. Holes and
        // decoded command ranges use the original window resolver.
        return walk_alias(address, size);
    }
    return walk_alias(address, size);
}

const RegisterMap::AliasEntry* RegisterMap::view_entry(std::uint64_t address,
                                                       AliasEntry& scratch) const {
    // The formula decoder is the only view dispatcher.
    return formula_ ? formula_->decode(address, scratch) : nullptr;
}

RegisterMap::AddressResolution RegisterMap::resolve_metadata(std::uint64_t address, unsigned size) {
    if (!RegisterAccessWidthPolicy::size_bit(size) || (address & (size - 1)))
        throw Error("invalid/unaligned access", ErrorKind::Unsupported);
    if (address >= size_ || size > size_ - address) {
        require_initialized();
        throw Error("address outside map", ErrorKind::Decode);
    }
    auto alias = resolve_static(address, size);
    if (!alias.compiled)
        alias.policy = alias.policy.intersect(view_policy(address, size));
    auto alias_address = alias.target;
    auto hint = alias.instance;
    if (!alias.compiled && !hint)
        if (auto p = lookup_.get(address))
            hint = p->source_instance;
    std::optional<AccessIdentity> identity;
    const detail::IdentityWindow* hit = nullptr;
    std::pair<std::uint32_t, std::uint64_t> match{};
    IdentityAddressSource source = IdentityAddressSource::Bus;
    if (alias.compiled && alias.identity_index) {
        const auto& w = identities_.at(*alias.identity_index);
        if (auto m = w.match(address, size, hint)) {
            hit = &w;
            match = *m;
        }
    }
    if (!alias.compiled)
        for (auto& w : identities_)
            if (auto m = w.match(address, size, hint)) {
                hit = &w;
                match = *m;
                break;
            }
    const bool tree_entrance =
        alias.compiled ||
        std::any_of(view_policies_.begin(), view_policies_.end(), [&](const auto& w) {
            return address >= w.base && address - w.base < w.size;
        });
    if (!hit && alias.target != address && !tree_entrance) {
        std::optional<std::uint32_t> ch;
        if (auto p = lookup_.get(alias.target))
            ch = p->source_instance;
        for (auto& w : identities_)
            if (auto m = w.match(alias.target, size, ch)) {
                hit = &w;
                match = *m;
                source = IdentityAddressSource::Canonical;
                break;
            }
    }
    if (hit) {
        auto fn = (hit->resolver ? hit->resolver->callback : identity_resolver_);
        if (!fn)
            throw Error("identity resolver is not configured", ErrorKind::Configuration);
        IdentityResolveRequest req{hit->domain,
                                   match.first,
                                   address,
                                   alias.target,
                                   match.second,
                                   static_cast<std::uint8_t>(size),
                                   alias.route,
                                   source};
        std::uint32_t id;
        try {
            id = (*fn)(req);
        } catch (const Error&) {
            throw; // An explicit device rejection keeps its error category.
        } catch (...) {
            throw Error("identity resolver failed",
                        ErrorKind::Device,
                        std::source_location::current(),
                        std::current_exception());
        }
        if (id >= hit->count)
            throw Error("resolved identity out of range", ErrorKind::Identity);
        identity =
            AccessIdentity{hit->domain, match.first, id, hit->mode, source, hit->trace_labels};
        if (hit->mode == IdentityRemapMode::RedirectBank) {
            auto target =
                resolve_static(hit->base + std::uint64_t(id) * hit->stride + match.second, size);
            if (alias.route.is_direct())
                alias.route = target.route;
            alias.policy = alias.policy.intersect(target.policy);
            if (!target.compiled)
                alias.policy = alias.policy.intersect(
                    view_policy(hit->base + std::uint64_t(id) * hit->stride + match.second, size));
            if (!alias.instance)
                alias.instance = hint ? hint : std::optional(match.first);
            alias.used |= target.used;
            if (alias.name.empty())
                alias.name = target.name;
            alias.target = target.target;
            alias.defined = target.defined;
            alias.target_entry = target.target_entry;
            alias.available = target.available;
            alias.pre_access_chain = target.pre_access_chain;
            if (target.compiled) {
                alias.view_name = target.view_name;
                alias.priority = target.priority;
            }
        }
    }
    return {alias, identity, alias_address};
}

// Access context: resolve a local/banked address before executing callbacks.
RegisterMap::Resolution& RegisterMap::resolve(std::uint64_t address,
                                              unsigned size,
                                              RegTransactionKind kind,
                                              RegData value,
                                              const AliasEntry* selected) {
    if (!RegisterAccessWidthPolicy::size_bit(size) || (address & (size - 1)))
        throw Error("invalid/unaligned access", ErrorKind::Unsupported);
    if (address >= size_ || size > size_ - address) {
        require_initialized();
        throw Error("address outside map", ErrorKind::Decode);
    }
    if (compiled_view_mode_) {
        const auto* source = selected;
        if (!source)
            source = view_entry(address, access_scratch_->decoded[0]);
        if (!source)
            throw Error("unmapped entity view address", ErrorKind::Decode);
        if (source->execute)
            return (this->*source->execute)(address, size, kind, value, source);
        if (source->identity_index)
            return resolve_plan<2>(address, size, kind, value, source);
        return resolve_plan<1>(address, size, kind, value, source);
    }
    return resolve_plan<0>(address, size, kind, value, nullptr);
}

template <unsigned Mode>
RegisterMap::Resolution& RegisterMap::resolve_plan(std::uint64_t address,
                                                   unsigned size,
                                                   RegTransactionKind kind,
                                                   RegData value,
                                                   const AliasEntry* selected) {
    auto& out = access_scratch_->resolution;
    auto& alias = out.alias;
    auto& identity = out.identity;
    identity.reset();
    std::uint64_t alias_address;
    const Entry* p{};
    std::optional<std::uint32_t> instance;
    std::string_view view_name;
    bool used = false, compiled = false;
    if constexpr (Mode != 0) {
        const auto* source = selected;
        if (!source)
            throw Error("unmapped entity view address", ErrorKind::Decode);
        const auto lane = address & 3;
        if (lane >= source->available || size > source->available - lane)
            throw Error("access crosses selected view target", ErrorKind::Decode);
        const auto* target = source;
        auto target_lane = lane;
        alias.target = source->target + lane;
        alias_address = alias.target;
        alias.route = source->route;
        alias.policy = source->policy;
        instance = source->instance;
        used = source->used;
        if constexpr (Mode == 2) {
            const auto& w = identities_.at(*source->identity_index);
            const auto matched =
                size == 4 && source->redirect_targets
                    ? std::optional(std::pair{source->identity_request, source->identity_inner})
                    : w.match(address, size, instance);
            if (auto match = matched) {
                auto fn = (w.resolver ? w.resolver->callback : identity_resolver_);
                if (!fn)
                    throw Error("identity resolver is not configured", ErrorKind::Configuration);
                IdentityResolveRequest req{w.domain,
                                           match->first,
                                           address,
                                           alias.target,
                                           match->second,
                                           static_cast<std::uint8_t>(size),
                                           alias.route,
                                           IdentityAddressSource::Bus};
                std::uint32_t id;
                try {
                    id = (*fn)(req);
                } catch (const Error&) {
                    throw;
                } catch (...) {
                    throw Error("identity resolver failed",
                                ErrorKind::Device,
                                std::source_location::current(),
                                std::current_exception());
                }
                if (id >= w.count)
                    throw Error("resolved identity out of range", ErrorKind::Identity);
                identity = AccessIdentity{
                    w.domain, match->first, id, w.mode, IdentityAddressSource::Bus, w.trace_labels};
                if (w.mode == IdentityRemapMode::RedirectBank) {
                    const auto address2 = w.base + std::uint64_t(id) * w.stride + match->second;
                    target = source->redirect_targets
                                 ? source->redirect_targets[id]
                                 : view_entry(address2, access_scratch_->decoded[1]);
                    if (!target)
                        throw Error("unmapped entity view address", ErrorKind::Decode);
                    target_lane = address2 & 3;
                    if (target_lane >= target->available || size > target->available - target_lane)
                        throw Error("access crosses selected view target", ErrorKind::Decode);
                    if (alias.route.is_direct())
                        alias.route = target->route;
                    alias.policy = alias.policy.intersect(target->policy);
                    if (!instance)
                        instance = match->first;
                    used |= target->used;
                    alias.target = target->target + target_lane;
                }
            }
        }
        if (!target->defined)
            throw Error("unmapped entity view address", ErrorKind::Decode);
        alias.pre_access_chain = target->pre_access_chain;
        view_name = target->view_name;
        compiled = target->compiled;
        p = target->target_entry ? &*target->target_entry : lookup_.get(alias.target);
    } else {
        auto metadata = resolve_metadata(address, size);
        const auto& a = metadata.alias;
        alias = {a.target, a.route, a.policy, a.pre_access_chain};
        identity = metadata.identity;
        alias_address = metadata.alias_address;
        instance = a.instance;
        used = a.used;
        view_name = a.view_name;
        compiled = a.compiled;
        if (!a.defined)
            throw Error("unmapped entity view address", ErrorKind::Decode);
        // The legacy metadata owns its Entry; copy it before metadata dies.
        out.entry = a.target_entry ? *a.target_entry : Entry{};
        p = a.target_entry ? &out.entry : lookup_.get(a.target);
    }
    if (!p)
        throw Error("unmapped register address", ErrorKind::Decode);
    auto e = *p;
    auto& n = *e.node;
    n.check_borrow();
    if (size == 8) {
        auto hi = compiled ? nullptr : lookup_.get(alias.target + 4);
        if (alias.target % 8 || e.byte_offset + 8 > n.value_size ||
            (!compiled && (!hi || hi->node != e.node || hi->linear != e.linear ||
                           hi->byte_offset != e.byte_offset + 4)))
            throw Error("invalid 64-bit access pair", ErrorKind::Unsupported);
    } else {
        auto lane = unsigned(alias.target & 3);
        if (lane + size > 4)
            throw Error("access crosses lookup word", ErrorKind::Unsupported);
        e.byte_offset += lane;
    }
    out.entry = e;
    auto& i = out.info;
    i.index_fields.size = i.mapped_fields.size = 0;
    i.access_group = {};
    i.wide_values = nullptr;
    i.kind = kind;
    i.view_path = view_name;
    i.address = address;
    i.alias_resolved_address = alias_address;
    i.resolved_address = alias.target;
    i.size = static_cast<std::uint8_t>(size);
    i.internal_byte_offset = static_cast<std::uint8_t>(e.byte_offset);
    const auto& coordinates =
        e.precomputed_index ? *e.precomputed_index : e.view->elements[e.linear].index;
    detail::copy_index(i.request_index, coordinates);
    detail::copy_index(i.index, i.request_index);
    i.raw_data = kind == RegTransactionKind::Write ? value & detail::low_mask(size * 8) : 0;
    i.register_width_bits = n.value_size * 8;
    i.bus_data = e.byte_offset < 8 ? i.raw_data << (e.byte_offset * 8) : 0;
    i.bus_mask = e.byte_offset < 8 ? detail::low_mask(size * 8) << (e.byte_offset * 8) : 0;
    i.stored_before = i.stored_after = n.read_scalar(e.linear);
    i.route = alias.route;
    i.alias_access_policy = alias.policy;
    i.source_instance = instance ? instance : e.source_instance;
    i.is_alias = used;
    if (identity) {
        i.request_identity = identity->request_id;
        i.resolved_identity = identity->resolved_id;
        i.identity_domain = identity->domain;
        i.identity_mode = identity->mode;
        i.identity_source = identity->source;
    } else {
        i.request_identity.reset();
        i.resolved_identity.reset();
        i.identity_domain.reset();
        i.identity_mode.reset();
        i.identity_source.reset();
    }
    return out;
}

void RegisterMap::validate_access(const Resolution& r) const {
    auto& n = *r.entry.node;
    auto& i = r.info;
    if (!n.widths.allows(i.size) || unsigned(i.internal_byte_offset) + i.size > n.value_size ||
        (n.widths.alignment == RegisterAlignment::RegisterBase && i.internal_byte_offset))
        throw Error("register width/alignment denied", ErrorKind::Unsupported);
}

void RegisterMap::derive(Resolution& r) {
    auto& n = *r.entry.node;
    if (n.derivations.empty())
        return;
    auto& i = r.info;
    if (const auto* plan = n.single_index.get()) {
        i.access_group = n.derivations.front().name;
        const auto original = i.request_index.values[plan->dimension];
        i.index_fields.entries.activate();
        i.mapped_fields.entries.activate();
        i.index_fields.entries[0] = {plan->source, plan->source_name, original, plan->dimension};
        i.index_fields.size = 1;
        const auto value = (*plan->resolver)(i);
        i.mapped_fields.entries[0] = {plan->output,
                                      plan->output_name,
                                      value,
                                      plan->metadata_only ? std::nullopt
                                                          : std::optional(plan->dimension)};
        i.mapped_fields.size = 1;
        if (!plan->metadata_only) {
            if (value >= plan->extent)
                throw RegAccessDeriveError::access_error("mapped element out of range");
            if (value != original) {
                const auto linear = r.entry.linear - original * plan->coefficient +
                                    static_cast<std::uint32_t>(value) * plan->coefficient;
                r.entry.linear = linear;
                i.index.linear_value = linear;
                i.index.values[plan->dimension] = static_cast<std::uint32_t>(value);
                i.stored_before = i.stored_after = n.read_scalar(linear);
            }
        }
        return;
    }
    for (const auto& d : n.derivations) {
        i.access_group = d.name;
        d.callback(i);
    }
    auto coords = i.request_index.values;
    std::array<std::optional<std::uint32_t>, MAX_REG_DIMS> chosen{};
    bool changed = false;
    for (std::size_t j = 0; j < i.mapped_fields.size; ++j) {
        auto& f = i.mapped_fields.entries[j];
        if (!f.dimension)
            continue;
        auto d = *f.dimension;
        if (d >= n.dims.size())
            throw RegAccessDeriveError::device_error("mapped dimension outside rank");
        if (f.value > UINT32_MAX || f.value >= n.dims[d])
            throw RegAccessDeriveError::access_error("mapped element out of range");
        if (chosen[d] && *chosen[d] != f.value)
            throw RegAccessDeriveError::device_error("conflicting physical dimension mappings");
        chosen[d] = static_cast<std::uint32_t>(f.value);
        changed |= coords[d] != f.value;
        coords[d] = static_cast<std::uint32_t>(f.value);
    }
    if (changed) {
        std::size_t linear = 0;
        for (std::size_t d = 0; d < n.dims.size(); ++d)
            linear = linear * n.dims[d] + coords[d];
        r.entry.linear = static_cast<std::uint32_t>(linear);
        detail::copy_index(i.index, r.entry.view->elements[linear].index);
        // Match Rust: access-group remap changes element/index; resolved_address
        // remains the bus/alias/identity address (not a re-encoded element address).
        i.stored_before = i.stored_after = n.read_scalar(linear);
    }
}

std::optional<RegMemTxResult> RegisterMap::prepare(Resolution& r) {
    try {
        validate_access(r);
        derive(r);
        if (access_admission_) {
            auto result = access_admission_(*r.entry.node, r.info);
            if (result != RegMemTxResult::Ok)
                return result;
        }
        return {};
    } catch (const Error& e) {
        log_error("access mapping/validation", r.entry.node->name, &r.info);
        return e.result;
    } catch (...) {
        log_error("access mapping/validation", r.entry.node->name, &r.info);
        return RegMemTxResult::DeviceError;
    }
}

bool RegisterMap::pre(Resolution& r) {
    const auto* fn = pre_access_.get();
    const auto* chain = r.alias.pre_access_chain;
    if (!fn && !chain)
        return true;
    PreAccessGuard guard(*this);
    if (fn)
        try {
            (*fn)(r.info);
        } catch (...) {
            log_error("pre_access", r.entry.node->name, &r.info);
            return false;
        }
    if (chain)
        for (const auto& step : chain->steps) {
            try {
                const auto* callback = step.slot->callback.get();
                if (!callback)
                    throw Error("view pre_access is not bound: " + step.slot->name,
                                ErrorKind::Configuration);
                (*callback)(r.info);
            } catch (...) {
                log_error("view pre_access " + step.slot->name + " @ " + std::string(step.view),
                          r.entry.node->name,
                          &r.info);
                return false;
            }
        }
    return true;
}

bool RegisterMap::access_log_enabled(const Node& n) const noexcept {
    if (!logger_ || n.log_policy == LogPolicy::Hidden)
        return false;
    return log_enabled(HLOG_LEVEL_TRACE) ||
           (n.log_policy != LogPolicy::Trace && log_enabled(HLOG_LEVEL_DEBUG));
}

void RegisterMap::access_log(const Resolution& r,
                             RegData value,
                             std::string_view action) const noexcept {
    if (!logger_)
        return;
    const auto& n = *r.entry.node;
    if (!access_log_enabled(n))
        return;
    auto level = log_enabled(HLOG_LEVEL_TRACE) ? HLOG_LEVEL_TRACE : HLOG_LEVEL_DEBUG;
    if (n.log_policy == LogPolicy::Trace)
        level = HLOG_LEVEL_TRACE;
    emit_log({level,
              r.info.is_read() ? "Read" : "Write",
              r.entry.view->elements[r.entry.linear].name,
              &r.info,
              value,
              action,
              {},
              r.identity ? &*r.identity : nullptr});
}

void RegisterMap::log_error(std::string_view phase,
                            std::string_view name,
                            const RegAccessInfo* info) const noexcept {
    if (!log_enabled(HLOG_LEVEL_ERROR))
        return;
    try {
        auto text = format_error_chain(std::current_exception());
        emit_log({HLOG_LEVEL_ERROR, phase, name, info, 0, {}, text});
    } catch (...) {
    }
}

void RegisterMap::complete_read_event(Resolution& r, WideRegData value, bool source_ok) {
    if (!source_ok)
        return;
    auto& n = *r.entry.node;
    if (n.is_wide()) {
        auto callback = n.wide->wide_read_complete_callback;
        if (callback)
            try {
                (*callback)(value, r.info);
            } catch (...) {
                log_error("on_read_complete", n.name, &r.info);
            }
    } else {
        auto callback = n.read_complete_callback;
        if (callback)
            try {
                (*callback)(value.low_u64(), r.info);
            } catch (...) {
                log_error("on_read_complete", n.name, &r.info);
            }
    }
}

void RegisterMap::refresh_wide(Resolution& r, WideAccessValues& values) {
    auto& i = r.info;
    auto& n = *r.entry.node;
    values.raw_data = WideRegData(i.raw_data);
    values.bus_mask = WideRegData(detail::low_mask(i.size * 8)) << (i.internal_byte_offset * 8);
    values.bus_data = values.raw_data << (i.internal_byte_offset * 8);
    values.stored_before = values.stored_after = n.read_wide(r.entry.linear);
    i.stored_before = i.stored_after = values.stored_before.low_u64();
    i.wide_values = &values;
}

// Wide-register read/write and multi-beat transaction execution.
RegMmioWriteResult RegisterMap::write_wide(Resolution& r) {
    auto& n = *r.entry.node;
    auto& i = r.info;
    WideAccessValues values;
    if (!n.derivations.empty() || log_enabled(HLOG_LEVEL_ERROR))
        refresh_wide(r, values);
    if (auto error = prepare(r))
        return {*error};
    refresh_wide(r, values);
    if (!pre(r))
        return {RegMemTxResult::Ignored};
    auto old = n.read_wide(r.entry.linear), mask = values.bus_mask & n.wide->wide_writable_mask;
    WideRegData result;
    {
        detail::NodeBorrow borrow(n);
        result = n.apply_wide_write(old, (old & ~values.bus_mask) | values.bus_data, mask);
    }
    auto after = ((old & ~mask) | (result & mask)) & n.wide_mask();
    n.write_wide(r.entry.linear, after);
    values.stored_before = old;
    values.stored_after = after;
    i.stored_before = old.low_u64();
    i.stored_after = after.low_u64();
    access_log(r, i.stored_after);
    auto callback = n.wide->wide_write_callback;
    if (callback)
        try {
            (*callback)(old, i);
        } catch (...) {
            log_error("on_write", n.name, &i);
        }
    return {};
}

RegMmioReadResult RegisterMap::read_wide(Resolution& r) {
    auto& n = *r.entry.node;
    auto& i = r.info;
    WideAccessValues values;
    if (!n.derivations.empty() || log_enabled(HLOG_LEVEL_ERROR))
        refresh_wide(r, values);
    if (auto error = prepare(r))
        return {INVALID_READ_VALUE & detail::low_mask(i.size * 8), *error};
    refresh_wide(r, values);
    if (!pre(r))
        return {REG_READ_CALLBACK_ERROR_MAGIC & detail::low_mask(i.size * 8)};
    auto before = n.read_wide(r.entry.linear), after = before;
    WideRegData returned;
    {
        detail::NodeBorrow borrow(n);
        returned = n.apply_wide_read(after, values.bus_mask) & n.wide_mask();
    }
    after &= n.wide_mask();
    n.write_wide(r.entry.linear, after);
    values.stored_before = before;
    values.stored_after = after;
    i.stored_before = before.low_u64();
    i.stored_after = after.low_u64();
    auto callback = n.wide->wide_read_callback;
    bool source_ok = true;
    if (callback)
        try {
            returned = (*callback)(returned, i) & n.wide_mask();
        } catch (...) {
            log_error("on_read", n.name, &i);
            source_ok = false;
            returned = WideRegData(REG_READ_CALLBACK_ERROR_MAGIC);
        }
    auto lane = (returned >> (i.internal_byte_offset * 8)).low_u64() & detail::low_mask(i.size * 8);
    access_log(r, lane);
    complete_read_event(r, returned, source_ok);
    return {lane};
}

WideRegData RegisterMap::read_storage(const Node& n, std::size_t index) const {
    return n.is_wide() ? n.read_wide(index) : WideRegData(n.read_scalar(index));
}

void RegisterMap::write_storage(Node& n, std::size_t index, WideRegData value) {
    value &= WideRegData::mask(n.value_size * 8);
    if (n.is_wide())
        n.write_wide(index, value);
    else
        n.write_scalar(index, value.low_u64());
}

void RegisterMap::transaction_values(Resolution& r, WideAccessValues& values) {
    auto& i = r.info;
    auto& n = *r.entry.node;
    values.raw_data = WideRegData(i.raw_data);
    values.bus_mask = WideRegData(detail::low_mask(i.size * 8)) << (i.internal_byte_offset * 8);
    values.bus_data = values.raw_data << (i.internal_byte_offset * 8);
    values.stored_before = values.stored_after = read_storage(n, r.entry.linear);
    if (n.is_wide())
        i.wide_values = &values;
    i.stored_before = i.stored_after = values.stored_before.low_u64();
}

RegMmioWriteResult RegisterMap::write_transaction(Resolution& r) {
    auto& n = *r.entry.node;
    auto& i = r.info;
    WideAccessValues values;
    if (!n.derivations.empty() || log_enabled(HLOG_LEVEL_ERROR))
        transaction_values(r, values);
    if (auto error = prepare(r))
        return {*error};
    transaction_values(r, values);
    if (!pre(r))
        return {RegMemTxResult::Ignored};
    auto& state = n.transaction_state[r.entry.linear];
    auto policy = n.transaction_policy.write;
    bool complete = policy == SplitWriteMode::CommitOnHigh
                        ? unsigned(i.internal_byte_offset) + i.size == n.value_size
                        : i.internal_byte_offset == 0;
    auto pending = (state.write_pending & ~values.bus_mask) | values.bus_data,
         mask = state.write_mask | values.bus_mask;
    if (!complete) {
        state.write_pending = pending;
        state.write_mask = mask;
        access_log(r, i.raw_data, "stage-write");
        return {};
    }
    auto old = read_storage(n, r.entry.linear);
    values.bus_data = pending;
    values.bus_mask = mask;
    i.bus_data = pending.low_u64();
    i.bus_mask = mask.low_u64();
    auto effective =
        mask & (n.is_wide() ? n.wide->wide_writable_mask : WideRegData(n.writable_mask));
    WideRegData result;
    {
        detail::NodeBorrow borrow(n);
        auto merged = (old & ~mask) | pending;
        if (n.is_wide())
            result = n.apply_wide_write(old, merged, effective);
        else
            result =
                WideRegData(n.apply_write(old.low_u64(), merged.low_u64(), effective.low_u64()));
    }
    auto after = (old & ~effective) | (result & effective);
    write_storage(n, r.entry.linear, after);
    after = read_storage(n, r.entry.linear);
    state.write_pending = {};
    state.write_mask = {};
    values.stored_before = old;
    values.stored_after = after;
    i.stored_before = old.low_u64();
    i.stored_after = after.low_u64();
    access_log(r, i.stored_after, "commit-write");
    if (n.is_wide()) {
        auto callback = n.wide->wide_write_callback;
        if (callback)
            try {
                (*callback)(old, i);
            } catch (...) {
                log_error("on_write", n.name, &i);
            }
    } else {
        auto callback = n.write_callback;
        if (callback)
            try {
                (*callback)(old.low_u64(), i);
            } catch (...) {
                log_error("on_write", n.name, &i);
            }
    }
    return {};
}

RegMmioReadResult RegisterMap::read_transaction(Resolution& r) {
    auto& n = *r.entry.node;
    auto& i = r.info;
    WideAccessValues values;
    if (!n.derivations.empty() || log_enabled(HLOG_LEVEL_ERROR))
        transaction_values(r, values);
    if (auto error = prepare(r))
        return {INVALID_READ_VALUE & detail::low_mask(i.size * 8), *error};
    transaction_values(r, values);
    if (!pre(r))
        return {REG_READ_CALLBACK_ERROR_MAGIC & detail::low_mask(i.size * 8)};
    auto& state = n.transaction_state[r.entry.linear];
    auto policy = n.transaction_policy.read;
    bool start = policy == SplitReadMode::SnapshotOnLow
                     ? i.internal_byte_offset == 0
                     : unsigned(i.internal_byte_offset) + i.size == n.value_size;
    bool complete = policy == SplitReadMode::SnapshotOnLow
                        ? unsigned(i.internal_byte_offset) + i.size == n.value_size
                        : i.internal_byte_offset == 0;
    if (!start && !state.has_read_snapshot) {
        if (log_enabled(HLOG_LEVEL_WARN)) {
            const auto expected =
                policy == SplitReadMode::SnapshotOnLow ? 0 : n.value_size - i.size;
            std::ostringstream reason;
            reason << "read-without-start: register=" << n.name << " width=" << n.value_size * 8
                   << " bits; received byte offset=" << unsigned(i.internal_byte_offset)
                   << " size=" << unsigned(i.size)
                   << "; required first beat byte offset=" << expected << " ("
                   << (policy == SplitReadMode::SnapshotOnLow ? "LOW_TO_HIGH" : "HIGH_TO_LOW")
                   << "); no active read snapshot";
            const auto message = reason.str();
            emit_log({HLOG_LEVEL_WARN, "read-without-start", n.name, &i, 0, {}, message});
        }
        return {INVALID_READ_VALUE & detail::low_mask(i.size * 8), RegMemTxResult::Unsupported};
    }
    WideRegData returned;
    if (start) {
        state.has_read_snapshot = false;
        auto before = read_storage(n, r.entry.linear), after = before;
        auto all = WideRegData::mask(n.value_size * 8);
        {
            detail::NodeBorrow borrow(n);
            if (n.is_wide())
                returned = n.apply_wide_read(after, all) & all;
            else {
                auto scalar = after.low_u64();
                returned = WideRegData(n.apply_read(scalar, all.low_u64()));
                after = WideRegData(scalar);
            }
        }
        write_storage(n, r.entry.linear, after);
        after = read_storage(n, r.entry.linear);
        values.stored_before = before;
        values.stored_after = after;
        i.stored_before = before.low_u64();
        i.stored_after = after.low_u64();
        bool failed = false;
        if (n.is_wide()) {
            auto callback = n.wide->wide_read_callback;
            if (callback)
                try {
                    returned = (*callback)(returned, i) & all;
                } catch (...) {
                    log_error("on_read", n.name, &i);
                    failed = true;
                }
        } else {
            auto callback = n.read_callback;
            if (callback)
                try {
                    returned = WideRegData((*callback)(returned.low_u64(), i)) & all;
                } catch (...) {
                    log_error("on_read", n.name, &i);
                    failed = true;
                }
        }
        if (failed)
            returned = WideRegData(REG_READ_CALLBACK_ERROR_MAGIC);
        state.read_snapshot = returned;
        state.has_read_snapshot = !complete;
        state.read_source_ok = !failed;
    } else {
        returned = state.read_snapshot;
        if (complete)
            state.has_read_snapshot = false;
    }
    auto lane = (returned >> (i.internal_byte_offset * 8)).low_u64() & detail::low_mask(i.size * 8);
    access_log(r, lane, start ? "snapshot-read" : complete ? "complete-read" : "snapshot-lane");
    if (complete)
        complete_read_event(r, returned, state.read_source_ok);
    return {lane};
}

// Scalar and mapped fast paths (preserve the same callback/transaction semantics).
template <bool Writing>
std::optional<RegData>
RegisterMap::fast_scalar(std::uint64_t address, RegData value, const CompiledAccess* selected) {
    // Entry validation and immutable path selection were done by dispatch.
    constexpr bool writing = Writing;
    auto& n = *selected->node;
    if (n.borrowed)
        return {};
    if (logger_ && n.log_policy != LogPolicy::Hidden &&
        (log_enabled(HLOG_LEVEL_DEBUG) || log_enabled(HLOG_LEVEL_TRACE)))
        return {};
    auto* p = selected->scalar;
    std::uint32_t stored;
    std::memcpy(&stored, p, 4);
    auto mask = selected->mask;
    auto after = (stored & ~mask) | (std::uint32_t(value) & mask);
    if (writing && !n.write_callback) {
        std::memcpy(p, &after, 4);
        return RegData{};
    }
    if (!writing && !n.read_callback && !n.read_complete_callback)
        return RegData(stored);

    const auto* a = selected->metadata;
    detail::ScalarAccessFrame frame{a,
                                    address,
                                    writing ? std::uint32_t(value) : 0,
                                    stored,
                                    writing ? after : stored,
                                    writing ? RegTransactionKind::Write : RegTransactionKind::Read};
    auto report = [&](std::string_view phase) {
        if (log_enabled(HLOG_LEVEL_ERROR)) {
            RegAccessInfo i;
            frame.fill(i);
            log_error(phase, n.name, &i);
        }
    };
    if (writing) {
        std::memcpy(p, &after, 4);
        try {
            n.write_callback->from_frame(stored, frame);
        } catch (...) {
            report("on_write");
        }
        return RegData{};
    }
    RegData returned = stored;
    bool source_ok = true;
    if (auto* fn = n.read_callback.get())
        try {
            returned = fn->from_frame(returned, frame) & 0xffffffff;
        } catch (...) {
            report("on_read");
            returned = REG_READ_CALLBACK_ERROR_MAGIC & 0xffffffff;
            source_ok = false;
        }
    if (logger_ && n.log_policy != LogPolicy::Hidden) {
        auto level = log_enabled(HLOG_LEVEL_TRACE) ? HLOG_LEVEL_TRACE : HLOG_LEVEL_DEBUG;
        if (n.log_policy == LogPolicy::Trace)
            level = HLOG_LEVEL_TRACE;
        if (log_enabled(level)) {
            RegAccessInfo i;
            frame.fill(i);
            const auto& e = *a->target_entry;
            emit_log({level, "Read", e.view->elements[e.linear].name, &i, returned, {}, {}});
        }
    }
    if (source_ok)
        if (auto* fn = n.read_complete_callback.get())
            try {
                fn->from_frame(returned, frame);
            } catch (...) {
                report("on_read_complete");
            }
    return returned;
}

template <bool Writing>
std::optional<RegMmioReadResult>
RegisterMap::mapped_scalar(std::uint64_t address, RegData value, const CompiledAccess* selected) {
    constexpr bool writing = Writing;
    const auto* source = selected->metadata;
    const auto* target = source;
    auto route = source->route;
    auto policy = source->policy;
    auto instance = source->instance;
    bool used = source->used;
    std::optional<AccessIdentity> identity;
    if (source->identity_index) {
        const auto& w = identities_[*source->identity_index];
        auto fn = (w.resolver ? w.resolver->callback : identity_resolver_);
        if (!fn)
            throw Error("identity resolver is not configured", ErrorKind::Configuration);
        IdentityResolveRequest req{w.domain,
                                   source->identity_request,
                                   address,
                                   source->target,
                                   source->identity_inner,
                                   4,
                                   route,
                                   IdentityAddressSource::Bus};
        std::uint32_t id;
        try {
            id = (*fn)(req);
        } catch (const Error&) {
            throw;
        } catch (...) {
            throw Error("identity resolver failed",
                        ErrorKind::Device,
                        std::source_location::current(),
                        std::current_exception());
        }
        if (id >= w.count)
            throw Error("resolved identity out of range", ErrorKind::Identity);
        identity = AccessIdentity{
            w.domain, req.request_id, id, w.mode, IdentityAddressSource::Bus, w.trace_labels};
        if (w.mode == IdentityRemapMode::RedirectBank) {
            target =
                source->redirect_targets
                    ? source->redirect_targets[id]
                    : view_entry(w.base + std::uint64_t(id) * w.stride + source->identity_inner,
                                 access_scratch_->decoded[1]);
            if (!target || !target->defined || !target->target_entry)
                throw Error("unmapped entity view address", ErrorKind::Decode);
            if (route.is_direct())
                route = target->route;
            policy = policy.intersect(target->policy);
            if (!instance)
                instance = req.request_id;
            used |= target->used;
        }
    }
    auto entry = *target->target_entry;
    auto& n = *entry.node;
    n.check_borrow();
    detail::MappedAccessFrame mapped;
    mapped.source = source;
    mapped.target = target;
    mapped.identity = identity ? &*identity : nullptr;
    mapped.request = target->callback_index;
    mapped.linear = entry.linear;
    mapped.route = route;
    mapped.policy = policy;
    mapped.instance = instance ? instance : entry.source_instance;
    mapped.used = used;
    mapped.index_scratch = &access_scratch_->index;
    mapped.mapped_scratch = &access_scratch_->mapped;
    detail::ScalarAccessFrame frame{target,
                                    address,
                                    writing ? std::uint32_t(value) : 0,
                                    n.read_scalar(entry.linear),
                                    0,
                                    writing ? RegTransactionKind::Write : RegTransactionKind::Read,
                                    &mapped};
    frame.after = frame.before;
    auto resolution = [&]() -> Resolution& {
        auto& r = access_scratch_->resolution;
        r.entry = entry;
        r.identity = identity;
        r.alias = {target->target, route, policy, target->pre_access_chain};
        frame.fill(r.info);
        return r;
    };
    const auto invalid = INVALID_READ_VALUE & 0xffffffff;
    if (writing ? !policy.allow_write : !policy.allow_read) {
        auto& r = resolution();
        emit_log({HLOG_LEVEL_WARN,
                  writing ? "alias write denied" : "alias read denied",
                  n.name,
                  &r.info,
                  writing ? value : 0,
                  {},
                  {},
                  nullptr});
        const auto action = writing ? policy.denied_write : policy.denied_read;
        return RegMmioReadResult{writing ? 0 : invalid,
                                 action == DeniedAccessAction::Ignore
                                     ? RegMemTxResult::Ignored
                                     : RegMemTxResult::AccessError};
    }
    if (const auto* plan = n.single_index.get()) {
        mapped.mapping = plan;
        mapped.group = n.derivations.front().name;
        mapped.logical = mapped.request->values[plan->dimension];
        try {
            mapped.mapped = plan->resolver->from_frame(frame);
            mapped.has_mapped = true;
            if (mapped.mapped >= plan->extent)
                throw RegAccessDeriveError::access_error("mapped element out of range");
            mapped.index_valid = true;
            const auto old = mapped.request->values[plan->dimension];
            if (mapped.mapped != old) {
                entry.linear = entry.linear - old * plan->coefficient +
                               static_cast<std::uint32_t>(mapped.mapped) * plan->coefficient;
                mapped.linear = entry.linear;
                frame.before = frame.after = n.read_scalar(entry.linear);
            }
        } catch (const Error& error) {
            auto& r = resolution();
            log_error("access mapping/validation", n.name, &r.info);
            return RegMmioReadResult{writing ? 0 : invalid, error.result};
        } catch (...) {
            auto& r = resolution();
            log_error("access mapping/validation", n.name, &r.info);
            return RegMmioReadResult{writing ? 0 : invalid, RegMemTxResult::DeviceError};
        }
    }
    if (pre_access_ || target->pre_access_chain) {
        auto& r = resolution();
        if (!pre(r))
            return RegMmioReadResult{writing ? 0 : REG_READ_CALLBACK_ERROR_MAGIC & 0xffffffff,
                                     writing ? RegMemTxResult::Ignored : RegMemTxResult::Ok};
    }
    auto* p = n.scalar_data + std::size_t(entry.linear) * n.scalar_stride;
    std::uint32_t before;
    std::memcpy(&before, p, 4);
    frame.before = frame.after = before;
    auto log = [&](RegData returned) {
        if (access_log_enabled(n)) {
            auto& r = resolution();
            access_log(r, returned);
        }
    };
    auto error = [&](std::string_view phase) {
        if (log_enabled(HLOG_LEVEL_ERROR)) {
            auto& r = resolution();
            log_error(phase, n.name, &r.info);
        }
    };
    if (writing) {
        const auto mask = std::uint32_t(n.writable_mask),
                   after = (before & ~mask) | (std::uint32_t(value) & mask);
        std::memcpy(p, &after, 4);
        frame.after = after;
        log(after);
        if (auto* fn = n.write_callback.get())
            try {
                fn->from_frame(before, frame);
            } catch (...) {
                error("on_write");
            }
        return RegMmioReadResult{0};
    }
    RegData returned = before;
    bool source_ok = true;
    if (auto* fn = n.read_callback.get())
        try {
            returned = fn->from_frame(returned, frame) & 0xffffffff;
        } catch (...) {
            error("on_read");
            returned = REG_READ_CALLBACK_ERROR_MAGIC & 0xffffffff;
            source_ok = false;
        }
    log(returned);
    if (source_ok)
        if (auto* fn = n.read_complete_callback.get())
            try {
                fn->from_frame(returned, frame);
            } catch (...) {
                error("on_read_complete");
            }
    return RegMmioReadResult{returned};
}

std::optional<RegData> RegisterMap::fast_scalar_resolved(Resolution& r, bool writing) {
    const auto& e = r.entry;
    auto& n = *e.node;
    if (r.info.size != 4 || e.byte_offset || !(n.scalar32_directions & (writing ? 2 : 1)) ||
        n.borrowed || pre_access_ || r.alias.pre_access_chain)
        return {};
    if (logger_ && n.log_policy != LogPolicy::Hidden &&
        (log_enabled(HLOG_LEVEL_DEBUG) || log_enabled(HLOG_LEVEL_TRACE)))
        return {};
    if (writing ? !n.write_callback : (!n.read_callback && !n.read_complete_callback)) {
        auto* p = n.scalar_data + std::size_t(e.linear) * n.scalar_stride;
        std::uint32_t stored;
        std::memcpy(&stored, p, 4);
        if (!writing)
            return RegData(stored);
        auto mask = std::uint32_t(n.writable_mask);
        auto after = (stored & ~mask) | (std::uint32_t(r.info.raw_data) & mask);
        std::memcpy(p, &after, 4);
        return RegData{};
    }
    return scalar32_events(e, r.info, r.identity ? &*r.identity : nullptr, writing);
}

RegData RegisterMap::scalar32_events(const Entry& e,
                                     RegAccessInfo& i,
                                     const AccessIdentity* identity,
                                     bool writing) {
    auto& n = *e.node;
    auto* p = n.scalar_data + std::size_t(e.linear) * n.scalar_stride;
    std::uint32_t stored;
    std::memcpy(&stored, p, 4);
    // Resolvers may update storage. Sample again at the same point as
    // the complete path, after validation/derivation and before behavior.
    i.stored_before = i.stored_after = stored;
    if (writing) {
        auto mask = std::uint32_t(n.writable_mask);
        auto after = (stored & ~mask) | (std::uint32_t(i.raw_data) & mask);
        std::memcpy(p, &after, 4);
        i.stored_after = after;
        if (auto* fn = n.write_callback.get())
            try {
                (*fn)(stored, i);
            } catch (...) {
                log_error("on_write", n.name, &i);
            }
        return {};
    }
    RegData returned = stored;
    bool source_ok = true;
    if (auto* fn = n.read_callback.get())
        try {
            returned = (*fn)(returned, i) & 0xffffffff;
        } catch (...) {
            log_error("on_read", n.name, &i);
            returned = REG_READ_CALLBACK_ERROR_MAGIC & 0xffffffff;
            source_ok = false;
        }
    // An on_read callback may enable logging before this read is logged.
    if (logger_ && n.log_policy != LogPolicy::Hidden) {
        auto level = log_enabled(HLOG_LEVEL_TRACE) ? HLOG_LEVEL_TRACE : HLOG_LEVEL_DEBUG;
        if (n.log_policy == LogPolicy::Trace)
            level = HLOG_LEVEL_TRACE;
        if (log_enabled(level))
            emit_log(
                {level, "Read", e.view->elements[e.linear].name, &i, returned, {}, {}, identity});
    }
    if (source_ok)
        if (auto* fn = n.read_complete_callback.get())
            try {
                (*fn)(returned, i);
            } catch (...) {
                log_error("on_read_complete", n.name, &i);
            }
    return returned;
}

// Public MMIO entry points and QEMU adapters.
RegMmioWriteResult
RegisterMap::try_mmio_write_sized(std::uint64_t address, RegData value, unsigned size) {
    try {
        DispatchGuard guard(*this);
        warn_truncated_write(address, value, size);
        const CompiledAccess* plan = nullptr;
        if (compiled_view_mode_ && finalized_ && size == 4 && !(address & 3))
            plan = formula_->fast_access(address, *access_scratch_);
        const auto* selected = plan ? plan->metadata : nullptr;
        if (plan && !pre_access_ && !access_admission_) {
            switch (plan->write_path) {
                case CompiledAccess::Path::Scalar:
                    if (fast_scalar<true>(address, value, plan))
                        return {};
                    break;
                case CompiledAccess::Path::Mapped:
                    if (auto fast = mapped_scalar<true>(address, value, plan))
                        return {fast->result};
                    break;
                case CompiledAccess::Path::Generic:
                    break;
            }
        }
        auto& r = resolve(address, size, RegTransactionKind::Write, value, selected);
        auto& n = *r.entry.node;
        auto& i = r.info;
        if (!r.alias.policy.allow_write) {
            emit_log({HLOG_LEVEL_WARN, "alias write denied", n.name, &i, value, {}, {}, nullptr});
            return {r.alias.policy.denied_write == DeniedAccessAction::Ignore
                        ? RegMemTxResult::Ignored
                        : RegMemTxResult::AccessError};
        }
        if (!n.custom_behavior && n.builtin == Access::Ro) {
            validate_access(r);
            access_log(r, i.raw_data, "ignored-read-only");
            return {RegMemTxResult::Ignored};
        }
        if (n.transaction_policy.write != SplitWriteMode::Independent)
            return write_transaction(r);
        if (n.is_wide())
            return write_wide(r);
        if (auto error = prepare(r))
            return {*error};
        if (fast_scalar_resolved(r, true))
            return {};
        if (!pre(r))
            return {RegMemTxResult::Ignored};
        auto index = r.entry.linear;
        auto old = n.read_scalar(index), mask = i.bus_mask & n.writable_mask;
        RegData result;
        {
            detail::NodeBorrow borrow(n);
            result = n.apply_write(old, (old & ~i.bus_mask) | i.bus_data, mask);
        }
        n.write_scalar(index, ((old & ~mask) | (result & mask)) & n.value_mask());
        i.stored_before = old;
        i.stored_after = n.read_scalar(index);
        access_log(r, i.stored_after);
        auto fn = n.write_callback;
        if (fn)
            try {
                (*fn)(old, i);
            } catch (...) {
                log_error("on_write", n.name, &i);
            }
        return {};
    } catch (const Error& e) {
        log_request_error(RegTransactionKind::Write, address, value, size);
        return {e.kind == ErrorKind::Identity &&
                        identity_rejection_policy_ == IdentityRejectionPolicy::Ignore
                    ? RegMemTxResult::Ignored
                    : e.result};
    } catch (...) {
        log_request_error(RegTransactionKind::Write, address, value, size);
        return {RegMemTxResult::DeviceError};
    }
}

RegMmioReadResult RegisterMap::try_mmio_read_sized(std::uint64_t address, unsigned size) {
    const auto invalid = RegisterAccessWidthPolicy::size_bit(size)
                             ? INVALID_READ_VALUE & detail::low_mask(size * 8)
                             : 0;
    try {
        DispatchGuard guard(*this);
        const CompiledAccess* plan = nullptr;
        if (compiled_view_mode_ && finalized_ && size == 4 && !(address & 3))
            plan = formula_->fast_access(address, *access_scratch_);
        const auto* selected = plan ? plan->metadata : nullptr;
        if (plan && !pre_access_ && !access_admission_) {
            switch (plan->read_path) {
                case CompiledAccess::Path::Scalar:
                    if (auto fast = fast_scalar<false>(address, 0, plan))
                        return {*fast};
                    break;
                case CompiledAccess::Path::Mapped:
                    if (auto fast = mapped_scalar<false>(address, 0, plan))
                        return *fast;
                    break;
                case CompiledAccess::Path::Generic:
                    break;
            }
        }
        auto& r = resolve(address, size, RegTransactionKind::Read, 0, selected);
        auto& n = *r.entry.node;
        auto& i = r.info;
        if (!r.alias.policy.allow_read) {
            emit_log({HLOG_LEVEL_WARN, "alias read denied", n.name, &i, 0, {}, {}, nullptr});
            return {invalid,
                    r.alias.policy.denied_read == DeniedAccessAction::Ignore
                        ? RegMemTxResult::Ignored
                        : RegMemTxResult::AccessError};
        }
        if (n.transaction_policy.read != SplitReadMode::Independent)
            return read_transaction(r);
        if (n.is_wide())
            return read_wide(r);
        if (auto error = prepare(r))
            return {invalid, *error};
        if (auto fast = fast_scalar_resolved(r, false))
            return {*fast};
        if (!pre(r))
            return {REG_READ_CALLBACK_ERROR_MAGIC & detail::low_mask(size * 8), RegMemTxResult::Ok};
        auto index = r.entry.linear;
        auto shift = i.internal_byte_offset * 8;
        auto before = n.read_scalar(index), after = before;
        RegData returned;
        {
            detail::NodeBorrow borrow(n);
            returned = n.apply_read(after, i.bus_mask) & n.value_mask();
        }
        if (!n.scalar_data || n.custom_behavior || after != before)
            n.write_scalar(index, after & n.value_mask());
        i.stored_before = before;
        i.stored_after = n.scalar_data ? (after & n.value_mask()) : n.read_scalar(index);
        auto fn = n.read_callback;
        bool source_ok = true;
        if (fn)
            try {
                returned = (*fn)(returned, i) & n.value_mask();
            } catch (...) {
                log_error("on_read", n.name, &i);
                source_ok = false;
                returned = REG_READ_CALLBACK_ERROR_MAGIC & n.value_mask();
            }
        auto lane = (returned >> shift) & detail::low_mask(size * 8);
        access_log(r, lane);
        if (n.read_complete_callback)
            complete_read_event(r, WideRegData(returned), source_ok);
        return {lane};
    } catch (const Error& e) {
        log_request_error(RegTransactionKind::Read, address, 0, size);
        return {invalid,
                e.kind == ErrorKind::Identity &&
                        identity_rejection_policy_ == IdentityRejectionPolicy::Ignore
                    ? RegMemTxResult::Ignored
                    : e.result};
    } catch (...) {
        log_request_error(RegTransactionKind::Read, address, 0, size);
        return {invalid, RegMemTxResult::DeviceError};
    }
}

RegData RegisterMap::mmio_read_sized(std::uint64_t a, unsigned s) {
    return try_mmio_read_sized(a, s).value;
}

RegData RegisterMap::mmio_read(std::uint64_t a) {
    return mmio_read_sized(a, 4);
}

std::uint32_t RegisterMap::qemu_mmio_write_sized(std::uint64_t a, RegData v, unsigned s) noexcept {
    try {
        return try_mmio_write_sized(a, v, s).qemu_bits();
    } catch (...) {
        return 1;
    }
}

std::uint32_t
RegisterMap::qemu_mmio_read_sized(std::uint64_t a, RegData* data, unsigned s) noexcept {
    if (!data)
        return 1;
    try {
        auto r = try_mmio_read_sized(a, s);
        *data = r.value;
        return r.qemu_bits();
    } catch (...) {
        *data = 0;
        return 1;
    }
}

} // namespace reg
