#include "internal/runtime_impl.hpp"
#include <regfw/layout_titles.hpp>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace reg {
namespace {
constexpr auto no_scope = ~std::size_t{};
using Row = std::vector<std::string>;
std::string hex(std::uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setfill('0') << std::setw(8) << value;
    return out.str();
}
std::string range(std::uint64_t base, std::uint64_t size) {
    return hex(base) + ".." + hex(base + size - 1);
}
std::size_t columns(std::string_view text) {
    std::size_t count = 0;
    for (unsigned char c : text)
        if ((c & 0xc0) != 0x80)
            ++count;
    return count;
}
std::string pad(std::string_view text, std::size_t width) {
    return std::string(text) + std::string(width - columns(text), ' ');
}
std::string repeat(std::string_view text, std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i)
        out += text;
    return out;
}
std::string cell(std::string_view text) {
    std::string out;
    for (char c : text) {
        if (c == '\n')
            out += "\\n";
        else if (c == '\r' || c == '\t')
            out += ' ';
        else
            out += c;
    }
    return out;
}
void table(std::ostream& out, std::string_view title, Row header, std::vector<Row> rows) {
    if (rows.empty()) {
        out << "\n" << title << ": (none)\n";
        return;
    }
    out << "\n" << title << "\n";
    std::vector<std::size_t> widths(header.size());
    for (auto& row : rows)
        for (auto& value : row)
            value = cell(value);
    for (std::size_t i = 0; i < header.size(); ++i) {
        widths[i] = columns(header[i]);
        for (const auto& row : rows)
            widths[i] = std::max(widths[i], columns(row[i]));
    }
    auto border = [&](std::string_view left, std::string_view joint, std::string_view right) {
        out << left;
        for (std::size_t i = 0; i < widths.size(); ++i)
            out << repeat("─", widths[i] + 2) << (i + 1 == widths.size() ? right : joint);
        out << '\n';
    };
    auto emit = [&](const Row& row) {
        out << "│";
        for (std::size_t i = 0; i < row.size(); ++i)
            out << ' ' << pad(row[i], widths[i]) << " │";
        out << '\n';
    };
    border("┌", "┬", "┐");
    emit(header);
    border("├", "┼", "┤");
    for (const auto& row : rows)
        emit(row);
    border("└", "┴", "┘");
}
std::string callback_name(const detail::CallbackDescription& description) {
    if (!description.name.empty())
        return description.name;
    auto name = description.type;
    auto begin = name.find("F = ");
    if (begin != std::string_view::npos) {
        name.remove_prefix(begin + 4);
        const auto end = name.find_first_of(";]");
        if (end != std::string_view::npos)
            name = name.substr(0, end);
    }
    return name.empty() ? "<unbound>" : std::string(name);
}
std::string callback_location(const detail::CallbackDescription& description) {
    if (!description.location.line())
        return "-";
    std::string_view file = description.location.file_name();
    const auto slash = file.find_last_of("/\\");
    if (slash != std::string_view::npos)
        file.remove_prefix(slash + 1);
    return std::string(file) + ":" + std::to_string(description.location.line());
}
std::string reset_name(ResetScope scope) {
    switch (scope.kind) {
        case ResetScope::Global:
            return "Global";
        case ResetScope::Pf:
            return "PF";
        case ResetScope::Vf:
            return "VF(" + std::to_string(scope.vf_id) + ")";
    }
    return "?";
}
std::string access_name(Access access) {
    switch (access) {
        case Access::Rw:
            return "rw";
        case Access::Ro:
            return "ro";
        case Access::W1c:
            return "w1c";
        case Access::W1s:
            return "w1s";
        case Access::W0c:
            return "w0c";
        case Access::W0s:
            return "w0s";
        case Access::Rc:
            return "rc";
        case Access::WriteOnly:
            return "write_only";
        case Access::W1Pulse:
            return "w1_pulse";
        case Access::Rwsc:
            return "rwsc";
        case Access::RwscImmediate:
            return "rwsc_immediate";
    }
    return "custom";
}
std::string policy_name(const AliasAccessPolicy& policy) {
    auto denied = [](DeniedAccessAction action) {
        return action == DeniedAccessAction::Ignore ? "ignore" : "error";
    };
    return std::string(policy.allow_read ? "R" : "R:") +
           (!policy.allow_read ? denied(policy.denied_read) : "") + ", " +
           (policy.allow_write ? "W" : std::string("W:") + denied(policy.denied_write));
}
std::string route_name(AccessRouteKind kind) {
    switch (kind) {
        case AccessRouteKind::Direct:
            return "direct";
        case AccessRouteKind::Alias:
            return "alias";
        case AccessRouteKind::VfToPf:
            return "VF→PF";
        case AccessRouteKind::PfToVf:
            return "PF→VF";
        case AccessRouteKind::VfToVf:
            return "VF→VF";
        case AccessRouteKind::PfToPf:
            return "PF→PF";
    }
    return "route(" + std::to_string(static_cast<unsigned>(kind)) + ")";
}
template <class T>
std::string list(const std::vector<T>& values, bool hexadecimal = false) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i)
            out += ',';
        if (hexadecimal) {
            std::ostringstream number;
            number << "0x" << std::hex << values[i];
            out += number.str();
        } else
            out += std::to_string(values[i]);
    }
    return out + ']';
}
struct Axis {
    std::string symbol;
    std::uint64_t count, stride, index;
};
class ScopeView {
    const std::vector<detail::LayoutScope>& scopes_;
    std::string root_;

  public:
    ScopeView(const std::vector<detail::LayoutScope>& scopes, std::string root)
        : scopes_(scopes), root_(std::move(root)) {
    }
    std::size_t owner(std::uint64_t base, std::uint64_t size) const {
        std::size_t best = no_scope;
        std::uint64_t best_size = UINT64_MAX;
        for (std::size_t i = 0; i < scopes_.size(); ++i) {
            const auto& s = scopes_[i];
            if (base >= s.base && base - s.base <= s.size && size <= s.size - (base - s.base) &&
                s.size <= best_size) {
                best = i;
                best_size = s.size;
            }
        }
        return best;
    }
    std::vector<Axis> axes(std::size_t id) const {
        std::vector<Axis> out;
        for (auto i = id; i < scopes_.size(); i = scopes_[i].parent) {
            const auto& s = scopes_[i];
            if (s.repeat_count > 1)
                out.push_back({{}, s.repeat_count, s.repeat_stride, s.repeat_index});
        }
        std::reverse(out.begin(), out.end());
        for (std::size_t i = 0; i < out.size(); ++i)
            out[i].symbol = i < 3 ? std::string(1, "ijk"[i]) : "n" + std::to_string(i);
        return out;
    }
    std::vector<Axis> address_axes(std::uint64_t base, std::uint64_t size) const {
        return axes(owner(base, size));
    }
    bool visible(std::size_t id) const {
        for (auto i = id; i < scopes_.size(); i = scopes_[i].parent)
            if (scopes_[i].repeat_count > 1 && scopes_[i].repeat_index)
                return false;
        return true;
    }
    std::string short_name(std::string name) const {
        const auto prefix = root_ + ".";
        if (name.starts_with(prefix))
            name.erase(0, prefix.size());
        return name;
    }
    std::string symbolic(std::string name) const {
        // Repeat identity comes from declaration metadata, not a guessed name.
        // Generated [number] suffixes are replaced only for those known scopes.
        std::map<std::size_t, std::pair<std::size_t, std::string>, std::greater<>> edits;
        for (std::size_t i = 0; i < scopes_.size(); ++i) {
            const auto& s = scopes_[i];
            if (s.repeat_count <= 1 || s.name.empty() || !name.starts_with(s.name))
                continue;
            if (name.size() > s.name.size() && name[s.name.size()] != '.' &&
                name[s.name.size()] != '+')
                continue;
            const auto suffix = "[" + std::to_string(s.repeat_index) + "]";
            if (!s.name.ends_with(suffix))
                continue;
            edits[s.name.size() - suffix.size()] = {suffix.size(),
                                                    "[" + axes(i).back().symbol + "]"};
        }
        for (const auto& [at, edit] : edits)
            name.replace(at, edit.first, edit.second);
        return name;
    }
    std::string formula(std::uint64_t base, std::uint64_t size, bool compact) const {
        auto a = compact ? address_axes(base, size) : std::vector<Axis>{};
        for (const auto& axis : a)
            base -= axis.index * axis.stride;
        auto out = range(base, size);
        for (const auto& axis : a)
            out += " + " + axis.symbol + "*" + hex(axis.stride);
        return out;
    }
    static std::string domains(const std::vector<Axis>& axes) {
        std::string out;
        for (const auto& axis : axes) {
            if (!out.empty())
                out += ", ";
            out += axis.symbol + "=0.." + std::to_string(axis.count - 1);
        }
        return out;
    }
    static std::uint64_t combinations(const std::vector<Axis>& axes) {
        std::uint64_t count = 1;
        for (const auto& axis : axes) {
            if (axis.count > UINT64_MAX / count)
                return 0;
            count *= axis.count;
        }
        return count;
    }
    // Preserve the affine relation to the source/reference instance. A
    // permutation or a partial family must not masquerade as an i→i mapping.
    std::string
    endpoint_key(std::uint64_t base, std::uint64_t size, const std::vector<Axis>& reference) const {
        std::int64_t intercept = static_cast<std::int64_t>(base);
        std::string extra;
        for (const auto& axis : address_axes(base, size)) {
            auto it = std::find_if(reference.begin(), reference.end(), [&](const auto& a) {
                return a.symbol == axis.symbol && a.count == axis.count;
            });
            if (it == reference.end())
                extra += " fixed=" + std::to_string(axis.index);
            else
                intercept -= static_cast<std::int64_t>(it->index * axis.stride);
            extra += " axis=" + axis.symbol + ":" + std::to_string(axis.count) + ":" +
                     std::to_string(axis.stride);
        }
        return std::to_string(intercept) + ":" + std::to_string(size) + extra;
    }
};
struct Item {
    std::uint64_t base{}, size{};
    std::string kind, name, details;
    std::size_t parent{}, scope = no_scope;
    bool hidden{};
};
struct Mapping {
    std::uint64_t source, target, size;
    std::vector<const detail::AddressWindow*> hops;
    AliasAccessPolicy policy;
};
struct MappingFamily {
    std::vector<Mapping> members;
    bool compact{};
};
std::string repeat_label(std::string label, const std::vector<Axis>& axes) {
    if (!axes.empty()) {
        const auto suffix = "[" + std::to_string(axes.back().index) + "]";
        if (label.ends_with(suffix))
            label.replace(
                label.size() - suffix.size(), suffix.size(), "[" + axes.back().symbol + "]");
    }
    return label;
}
std::string hop_key(const Mapping& path, const ScopeView& view) {
    std::string out;
    auto address = path.source;
    for (const auto* hop : path.hops) {
        auto axes = view.address_axes(address, path.size);
        out += route_name(hop->route.kind);
        if (hop->route.kind != AccessRouteKind::Alias && hop->route.kind != AccessRouteKind::Direct)
            out += !axes.empty() && axes.back().index == hop->route.instance
                       ? "(" + axes.back().symbol + ")"
                       : "(" + std::to_string(hop->route.instance) + ")";
        // Labels may carry their own generated instance suffix.
        auto label = repeat_label(view.symbolic(hop->name), axes);
        out += ":" + label + ";";
        address = hop->target_base + (address - hop->source_base);
    }
    return out + policy_name(path.policy);
}
std::string path_route(const Mapping& path, const ScopeView& view, bool compact) {
    std::string out;
    auto address = path.source;
    for (const auto* hop : path.hops) {
        if (!out.empty())
            out += " → ";
        out += route_name(hop->route.kind);
        if (hop->route.kind != AccessRouteKind::Alias &&
            hop->route.kind != AccessRouteKind::Direct) {
            const auto axes = view.address_axes(address, path.size);
            out += "(" +
                   (!axes.empty() && compact && axes.back().index == hop->route.instance
                        ? axes.back().symbol
                        : std::to_string(hop->route.instance)) +
                   ")";
        }
        address = hop->target_base + (address - hop->source_base);
    }
    return out;
}
template <class T, class Key, class Axes>
std::vector<std::pair<std::vector<T>, bool>>
families(const std::vector<T>& values, Key key, Axes axes) {
    std::map<std::string, std::vector<T>> grouped;
    for (const auto& value : values)
        grouped[key(value)].push_back(value);
    std::vector<std::pair<std::vector<T>, bool>> out;
    for (auto& [name, members] : grouped) {
        (void)name;
        const auto count = ScopeView::combinations(axes(members.front()));
        // Every declaration instance must be represented, exactly once.
        std::set<std::vector<std::uint64_t>> coordinates;
        for (const auto& m : members) {
            std::vector<std::uint64_t> c;
            for (const auto& a : axes(m))
                c.push_back(a.index);
            coordinates.insert(std::move(c));
        }
        if (count > 1 && members.size() == count && coordinates.size() == count)
            out.push_back({std::move(members), true});
        else
            for (auto& m : members)
                out.push_back({{std::move(m)}, false});
    }
    return out;
}
template <class Name>
std::vector<MappingFamily>
compact_mappings(const std::vector<Mapping>& paths, const ScopeView& view, Name name) {
    auto grouped = families(
        paths,
        [&](const Mapping& p) {
            const auto axes = view.address_axes(p.source, p.size);
            return view.symbolic(name(p.source, p.size)) + "|" +
                   view.endpoint_key(p.source, p.size, axes) + "|" +
                   view.symbolic(name(p.target, p.size)) + "|" +
                   view.endpoint_key(p.target, p.size, axes) + "|" + hop_key(p, view);
        },
        [&](const Mapping& p) {
            return view.address_axes(p.source, p.size);
        });
    std::vector<MappingFamily> out;
    for (auto& [members, compact] : grouped)
        out.push_back({std::move(members), compact});
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.members.front().source < b.members.front().source;
    });
    return out;
}
struct Convergence {
    std::uint64_t target, size;
    std::vector<Mapping> paths;
};
} // namespace

std::size_t RegisterMap::register_layout_scope(std::uint64_t base,
                                               std::uint64_t size,
                                               std::string name,
                                               ResetScope reset,
                                               std::optional<std::uint32_t> instance,
                                               std::size_t parent,
                                               std::uint64_t repeat_count,
                                               std::uint64_t repeat_stride,
                                               std::uint64_t repeat_index) {
    mutable_layout();
    layout_scopes_.push_back({base,
                              size,
                              std::move(name),
                              reset,
                              instance,
                              parent,
                              repeat_count,
                              repeat_stride,
                              repeat_index,
                              false,
                              {}});
    return layout_scopes_.size() - 1;
}

std::string RegisterMap::describe_layout(LayoutDetail detail) const {
    require_initialized();
    if (compiled_view_mode_) {
        std::ostringstream out;
        out << "COMPOSED ENTITY / VIEW LAYOUT (static; resolvers are not executed)\n";
        out << "Rows describe address views; entity: names referenced storage, not a new "
               "allocation. "
               "Ranges are mapping bounds, not occupied register spans.\n";
        out << "Folded repeats show the entity target/reset of the representative i=j=k=0 "
               "instance; "
               "a shared entity keeps the same target name across entrances.\n";
        const ScopeView scopes(layout_scopes_, name_prefix);
        std::vector<Row> regions;
        std::unordered_map<std::string_view, std::size_t> scope_ids;
        for (std::size_t i = 0; i < layout_scopes_.size(); ++i) {
            const auto& scope = layout_scopes_[i];
            scope_ids.emplace(scope.name, i);
            if (!scopes.visible(i))
                continue;
            const auto axes = scopes.axes(i);
            auto addresses = scopes.formula(scope.base, scope.size, !axes.empty());
            auto purpose = scope.purpose;
            if (!axes.empty())
                purpose += "; " + ScopeView::domains(axes);
            regions.push_back(
                {scopes.symbolic(scope.name), std::move(addresses), std::move(purpose)});
        }
        table(out,
              layout_titles::bundle,
              {"Address view", "Address range / formula", "Purpose / storage target"},
              std::move(regions));
        if (detail == LayoutDetail::Full) {
            struct Group {
                const detail::EntityState* node{};
                AliasEntry entry{}; // by value: formula enumeration is transient
                std::uint64_t origin{}, low = UINT64_MAX, high{}, words{};
                std::map<std::size_t, std::uint64_t> elements, lanes;
            };
            // Group by selected mapping and affine local address translation,
            // never by individual element. Partial overlays retain selection counts.
            std::map<std::tuple<std::string_view, std::string_view, std::uint64_t, const char*>,
                     Group>
                groups;
            auto consume = [&](std::uint64_t address, const AliasEntry& entry) {
                if (!entry.target_entry)
                    return; // Reserved regions are in the view table.
                const auto& target = *entry.target_entry;
                const auto* node = target.node;
                const auto origin =
                    address - node->layout().local_addresses[target.linear] - target.byte_offset;
                auto& group = groups[{entry.view_name, node->name, origin, entry.view_name.data()}];
                group.node = node;
                group.entry = entry;
                group.origin = origin;
                group.low = std::min(group.low, address);
                group.high = std::max(group.high, address + 3);
                ++group.words;
                ++group.elements[target.linear];
                group.lanes[target.linear] |= std::uint64_t{1} << (target.byte_offset / 4);
            };
            formula_->for_each_word(consume);
            auto selection = [](const auto& elements) {
                std::string text;
                std::size_t runs = 0, omitted = 0;
                for (auto it = elements.begin(); it != elements.end();) {
                    const auto first = it->first;
                    auto last = first;
                    ++it;
                    while (it != elements.end() && it->first == last + 1) {
                        last = it->first;
                        ++it;
                    }
                    if (runs++ >= 8) {
                        ++omitted;
                        continue;
                    }
                    if (!text.empty())
                        text += ',';
                    text += std::to_string(first);
                    if (first != last)
                        text += ".." + std::to_string(last);
                }
                if (omitted)
                    text += "; +" + std::to_string(omitted) + " more runs";
                return text;
            };
            // Collapse repeats only when every instance has identical visible
            // elements/lanes/policies. An instance-specific overlay stays explicit.
            std::map<std::tuple<std::string, std::string, std::uint64_t>, std::vector<const Group*>>
                families;
            for (const auto& [key, g] : groups) {
                (void)key;
                const auto id = scope_ids.find(g.entry.view_name);
                auto origin = g.origin;
                auto name = std::string(g.entry.view_name);
                if (id != scope_ids.end()) {
                    for (const auto& axis : scopes.axes(id->second))
                        origin -= axis.index * axis.stride;
                    name = scopes.symbolic(name);
                }
                const auto dot = g.node->name.rfind('.');
                families[{name,
                          g.node->name.substr(dot == std::string::npos ? 0 : dot + 1),
                          origin}]
                    .push_back(&g);
            }
            struct VisibleRow {
                std::uint64_t first_address;
                Row columns;
            };
            std::vector<VisibleRow> ordered;
            for (const auto& [family, members] : families) {
                (void)family;
                const auto& first = *members.front();
                const auto scope = scope_ids.find(first.entry.view_name);
                const auto axes =
                    scope == scope_ids.end() ? std::vector<Axis>{} : scopes.axes(scope->second);
                const bool compact =
                    !axes.empty() && members.size() == ScopeView::combinations(axes) &&
                    std::all_of(members.begin(), members.end(), [&](const auto* g) {
                        const auto& a = first.entry;
                        const auto& b = g->entry;
                        return g->lanes == first.lanes && g->node->dims == first.node->dims &&
                               g->node->layout().strides == first.node->layout().strides &&
                               g->node->value_size == first.node->value_size &&
                               a.priority == b.priority &&
                               a.policy.allow_read == b.policy.allow_read &&
                               a.policy.allow_write == b.policy.allow_write &&
                               a.policy.denied_read == b.policy.denied_read &&
                               a.policy.denied_write == b.policy.denied_write;
                    });
                for (std::size_t member = 0; member < (compact ? 1 : members.size()); ++member) {
                    const auto& g = *members[member];
                    const auto& n = *g.node;
                    const auto& e = g.entry;
                    auto address = hex(g.origin + n.layout().local_addresses.front());
                    for (std::size_t d = 0; d < n.dims.size(); ++d)
                        address += " + x" + std::to_string(d) + "*" + hex(n.layout().strides[d]);
                    const auto id = scope_ids.find(e.view_name);
                    std::string domains;
                    if (compact && id != scope_ids.end()) {
                        for (const auto& axis : scopes.axes(id->second))
                            address += " + " + axis.symbol + "*" + hex(axis.stride);
                        domains = ScopeView::domains(scopes.axes(id->second));
                    }
                    auto shape = std::to_string(n.value_size * 8) + "-bit";
                    if (n.declared_array)
                        shape += "; dims=" + list(n.dims) +
                                 "; strides=" + list(n.layout().strides, true);
                    const auto words = std::max((n.value_size + 3u) / 4u, 1u);
                    std::size_t partial = 0;
                    for (const auto& [index, count] : g.elements) {
                        (void)index;
                        if (count != words)
                            ++partial;
                    }
                    auto selected = "elements=" + std::to_string(g.elements.size()) + "/" +
                                    std::to_string(n.element_count()) + "; linear={" +
                                    selection(g.elements) +
                                    "}; visible words=" + std::to_string(g.words);
                    if (partial)
                        selected += "; partial elements=" + std::to_string(partial) +
                                    " (use explain_address for lanes)";
                    if (!domains.empty())
                        selected += "; " + domains;
                    ordered.push_back({g.low,
                                       {compact ? scopes.symbolic(std::string(e.view_name))
                                                : std::string(e.view_name),
                                        n.name,
                                        std::move(address),
                                        std::move(shape),
                                        std::move(selected),
                                        std::to_string(e.priority),
                                        policy_name(e.policy)}});
                }
            }
            // Presentation order is numeric bus address, independent of names,
            // family grouping, and a potentially hidden array origin.
            std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
                return a.first_address != b.first_address ? a.first_address < b.first_address
                                                          : a.columns < b.columns;
            });
            std::vector<Row> visible;
            visible.reserve(ordered.size());
            for (auto& row : ordered)
                visible.push_back(std::move(row.columns));
            table(out,
                  "VISIBLE REGISTER MAP (compact arrays)",
                  {"Selected view",
                   "Register entity",
                   "Element address formula",
                   "Shape",
                   "Visible selection",
                   "Priority",
                   "Access"},
                  std::move(visible));
            out << "x0..xN are register coordinates (0 <= xd < dims[d]); add a byte lane within "
                   "the register. "
                   "Formulas describe declared geometry, not continuous coverage; visible "
                   "selection excludes shadowed elements/words. "
                   "Repeats with identical visible selections show one representative per family; "
                   "exceptions stay explicit.\n";
        }
        if (detail == LayoutDetail::Full) {
            std::vector<Row> hooks;
            std::set<std::pair<std::string, const detail::PreAccessSlot*>> seen;
            for (const auto& chain : view_pre_access_chains_)
                for (const auto& step : chain->steps) {
                    if (!seen.emplace(step.view, step.slot.get()).second)
                        continue;
                    const auto& slot = *step.slot;
                    hooks.push_back({std::string(step.view),
                                     slot.name,
                                     callback_name(slot.description),
                                     callback_location(slot.description),
                                     slot.callback   ? "bound"
                                     : slot.deferred ? "deferred"
                                                     : "unbound"});
                }
            if (!hooks.empty())
                table(out,
                      "VIEW PRE_ACCESS BINDINGS",
                      {"View", "Key", "Function / callable", "Registered at", "State"},
                      std::move(hooks));
        }
        std::vector<Row> warnings;
        for (const auto& warning : layout_warnings())
            warnings.push_back({warning.code, warning.subject, warning.message});
        if (!warnings.empty())
            table(out, layout_titles::warnings, {"Code", "Entity", "Details"}, std::move(warnings));
        return out.str();
    }
    const bool full = detail == LayoutDetail::Full;
    const std::string root = name_prefix.empty() ? "<map>" : name_prefix;
    const ScopeView view(layout_scopes_, root);
    auto address_name = [&](std::uint64_t base, std::uint64_t size) {
        auto index = view.owner(base, size);
        const auto start = index == no_scope ? 0 : layout_scopes_[index].base;
        auto name = index == no_scope ? root : layout_scopes_[index].name;
        if (name.empty())
            name = root + ".bundle@" + hex(start);
        return base == start ? name : name + "+" + hex(base - start);
    };
    auto endpoint = [&](std::uint64_t base, std::uint64_t size, bool compact) {
        auto name = address_name(base, size);
        return view.short_name(compact ? view.symbolic(std::move(name)) : std::move(name));
    };
    auto role = [&](std::uint64_t base, std::uint64_t size) {
        const bool storage = std::any_of(nodes_.begin(), nodes_.end(), [&](const auto& node) {
            const auto& l = node->layout();
            for (std::size_t i = 0; i < l.elements.size(); ++i)
                if (l.element_mapped[i] &&
                    detail::overlaps(base,
                                     size,
                                     l.elements[i].address,
                                     std::max((node->value_size + 3u) & ~3u, 4u)))
                    return true;
            return false;
        });
        const bool aliases = std::any_of(windows_.begin(), windows_.end(), [&](const auto& window) {
            return !window.terminal &&
                   detail::overlaps(base, size, window.source_base, window.size);
        });
        std::string text;
        for (const auto& part : {std::pair{storage, "STORAGE"}, std::pair{aliases, "VIEW"}})
            if (part.first) {
                if (!text.empty())
                    text += '+';
                text += part.second;
            }
        return text.empty() ? std::string("APERTURE") : text;
    };
    std::vector<Mapping> direct;
    for (const auto& window : windows_)
        if (!window.terminal)
            direct.push_back({window.source_base,
                              window.target_base,
                              window.size,
                              {&window},
                              window.policy.intersect(view_policy(window.source_base, 4))});
    const auto links = compact_mappings(direct, view, address_name);
    std::unordered_set<const detail::AddressWindow*> symbolic_aliases;
    for (const auto& family : links)
        if (family.compact)
            for (const auto& mapping : family.members)
                symbolic_aliases.insert(mapping.hops.front());
    std::vector<Item> items{{0, size_, "MAP", root, "inclusive byte addresses", 0}};
    std::vector<std::size_t> scopes(layout_scopes_.size());
    for (std::size_t i = 0; i < layout_scopes_.size(); ++i) {
        const auto& scope = layout_scopes_[i];
        const auto parent = scope.parent < i ? scopes[scope.parent] : 0;
        if (scope.parent == no_scope && scope.base == 0 && scope.size == size_ &&
            scope.name == root) {
            scopes[i] = 0;
            continue;
        }
        scopes[i] = items.size();
        auto details = "reset=" + reset_name(scope.reset);
        if (scope.recursive_window && scope.reset.kind == ResetScope::Vf && !view.axes(i).empty())
            details = "reset=VF(" + view.axes(i).front().symbol + ")";
        if (scope.repeat_count > 1) {
            const auto axis = view.axes(i).back();
            details = (scope.reset.kind == ResetScope::Vf
                           ? "reset=VF(" + view.axes(i).front().symbol + ")"
                           : details) +
                      "; " + std::to_string(scope.repeat_count) + " copies; " + axis.symbol +
                      "=0.." + std::to_string(scope.repeat_count - 1) +
                      "; stride=" + hex(scope.repeat_stride);
        }
        if (scope.recursive_window) {
            if (scope.purpose == "COMMAND")
                details = "decoder; no storage";
            else if (scope.purpose == "RESERVED")
                details = "reserved";
            else
                for (const auto& w : windows_)
                    if (!w.terminal && w.name == scope.name && w.source_base == scope.base &&
                        w.size == scope.size) {
                        details += "; → " +
                                   endpoint(w.target_base, w.size, symbolic_aliases.contains(&w)) +
                                   "; " +
                                   policy_name(w.policy.intersect(view_policy(w.source_base, 4)));
                        break;
                    }
        }
        items.push_back({scope.base,
                         scope.size,
                         scope.purpose.empty() ? "BUNDLE" : scope.purpose,
                         scope.name.empty() ? address_name(scope.base, scope.size) : scope.name,
                         details,
                         parent,
                         i});
    }
    auto parent_for = [&](std::uint64_t base, std::uint64_t size) {
        auto index = view.owner(base, size);
        return index == no_scope ? 0 : scopes[index];
    };
    for (const auto& node : nodes_) {
        if (!full)
            continue;
        if (node->layout().elements.empty() || !node->layout().mapped)
            continue;
        std::uint64_t low = UINT64_MAX, high = 0;
        std::size_t mapped_count = 0;
        for (std::size_t i = 0; i < node->layout().elements.size(); ++i)
            if (node->layout().element_mapped[i]) {
                const auto a = node->layout().elements[i].address;
                low = std::min(low, std::uint64_t(a));
                high = std::max(high, std::uint64_t(a));
                ++mapped_count;
            }
        const auto occupied = std::max((node->value_size + 3u) & ~3u, 4u);
        auto scope = node->layout().layout_scope;
        if (scope >= layout_scopes_.size() || low < layout_scopes_[scope].base ||
            high + occupied > layout_scopes_[scope].base + layout_scopes_[scope].size)
            scope = view.owner(low, high - low + occupied);
        const auto parent =
            scope < scopes.size() ? scopes[scope] : parent_for(low, high - low + occupied);
        auto details = std::to_string(node->value_size * 8) + "-bit " +
                       (node->custom_behavior ? "custom" : access_name(node->builtin));
        if (node->declared_array)
            details += "; dims=" + list(node->dims) +
                       "; stride=" + list(node->layout().strides, true) +
                       "; count=" + std::to_string(node->layout().elements.size());
        if (mapped_count < node->element_count())
            details += "; mapped=" + std::to_string(mapped_count) + "/" +
                       std::to_string(node->element_count());
        items.push_back({low,
                         high - low + occupied,
                         node->declared_array ? "ARRAY" : "REGISTER",
                         node->layout().display_name,
                         details,
                         parent,
                         scope});
    }
    auto represented = [&](std::uint64_t base, std::uint64_t size, const std::string& name) {
        return std::any_of(layout_scopes_.begin(), layout_scopes_.end(), [&](const auto& scope) {
            return scope.recursive_window && scope.base == base && scope.size == size &&
                   scope.name == name;
        });
    };
    for (const auto& w : windows_)
        if (!w.terminal && !represented(w.source_base, w.size, w.name))
            items.push_back({w.source_base,
                             w.size,
                             "ALIAS",
                             w.name.empty()
                                 ? address_name(w.source_base, w.size)
                                 : repeat_label(w.name, view.address_axes(w.source_base, w.size)),
                             "→ " + endpoint(w.target_base, w.size, symbolic_aliases.contains(&w)) +
                                 "; " + policy_name(w.policy),
                             parent_for(w.source_base, w.size),
                             view.owner(w.source_base, w.size)});
    for (const auto& r : reserved_) {
        if (represented(r.base, r.size, r.name))
            continue;
        auto name = r.name.empty() ? "<reserved>" : r.name;
        std::string details = "reserved";
        if (r.repeat_count > 1) {
            const auto suffix = "[" + std::to_string(r.repeat_index) + "]";
            if (name.ends_with(suffix))
                name.replace(name.size() - suffix.size(), suffix.size(), "[i]");
            details += "; " + std::to_string(r.repeat_count) + " copies; i=0.." +
                       std::to_string(r.repeat_count - 1) + "; stride=" + hex(r.repeat_stride);
        }
        items.push_back({r.base,
                         r.size,
                         "RESERVED",
                         std::move(name),
                         std::move(details),
                         parent_for(r.base, r.size),
                         view.owner(r.base, r.size),
                         r.repeat_count > 1 && r.repeat_index != 0});
    }

    // Array rows cover their total span in this summary. Internal stride gaps
    // are explicitly labelled, rather than expanded into per-element HOLE rows.
    const auto existing = items.size();
    std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>> coverage(existing);
    for (std::size_t i = 1; i < existing; ++i)
        coverage[items[i].parent].push_back({items[i].base, items[i].base + items[i].size});
    for (std::size_t i = 0; i < existing; ++i)
        if ((items[i].kind == "MAP" || items[i].kind == "BUNDLE") && view.visible(items[i].scope)) {
            const auto scope = items[i].scope;
            if (scope < layout_scopes_.size() && layout_scopes_[scope].recursive_window) {
                const bool mapped =
                    std::any_of(windows_.begin(), windows_.end(), [&](const auto& w) {
                        return w.mapped_offsets && w.source_base == items[i].base &&
                               w.size == items[i].size && w.name == items[i].name;
                    });
                const bool owns = std::any_of(nodes_.begin(), nodes_.end(), [&](const auto* n) {
                    return n->layout().mapped && n->layout().layout_scope == scope;
                });
                // Referenced storage is described at its backing window. A view
                // with no local REGISTER rows is not an unmapped address region.
                if (mapped && (!full || !owns))
                    continue;
            }
            auto& ranges = coverage[i];
            std::sort(ranges.begin(), ranges.end());
            auto cursor = items[i].base;
            const auto end = cursor + items[i].size;
            auto gap = [&](std::uint64_t to) {
                if (to > cursor)
                    items.push_back(
                        {cursor, to - cursor, "HOLE", "<unmapped>", "", i, items[i].scope});
            };
            for (auto [begin, last] : ranges) {
                gap(begin);
                cursor = std::max(cursor, last);
            }
            gap(end);
        }
    std::vector<std::vector<std::size_t>> children(items.size());
    for (std::size_t i = 1; i < items.size(); ++i)
        if (!items[i].hidden && view.visible(items[i].scope))
            children[items[i].parent].push_back(i);
    for (auto& ids : children)
        std::stable_sort(ids.begin(), ids.end(), [&](auto a, auto b) {
            return items[a].base != items[b].base ? items[a].base < items[b].base
                                                  : items[a].kind < items[b].kind;
        });
    std::vector<Row> layout;
    auto visit = [&](auto&& self, std::size_t id, std::string prefix, bool last) -> void {
        const auto& item = items[id];
        auto name = view.symbolic(item.name);
        const auto& parent = items[item.parent];
        if (id && name.starts_with(view.symbolic(parent.name) + "."))
            name.erase(0, view.symbolic(parent.name).size() + 1);
        else
            name = view.short_name(std::move(name));
        const auto tree = id ? prefix + (last ? "└─ " : "├─ ") : "";
        layout.push_back({item.kind == "BUNDLE" ? role(item.base, item.size) : item.kind,
                          tree + name,
                          range(item.base, item.size),
                          hex(item.size),
                          item.details});
        const auto next = id ? prefix + (last ? "   " : "│  ") : "";
        const auto& nested = children[id];
        for (std::size_t n = 0; n < nested.size(); ++n)
            self(self, nested[n], next, n + 1 == nested.size());
    };
    visit(visit, 0, "", true);
    std::ostringstream out;
    out << "REGISTER LAYOUT: " << root << " [" << range(0, size_) << "] " << size_ << " bytes; "
        << (finalized_ ? "finalized" : "not finalized") << "\n";
    out << "Arrays: one span (may include stride gaps). Repeats: one representative; child "
           "addresses use i=j=k=0.\n";
    out << "STORAGE = register backing; VIEW = address aliases.\n";
    out << "RegEntity: " << entity_count() << " owned entities; RegView: " << mapped_view_count()
        << " automatic address mappings; copies/slices add no storage.\n";
    table(out,
          layout_titles::bundle,
          {"Kind", "Bundle / declaration", "Address range (representative)", "Span", "Definition"},
          std::move(layout));

    std::vector<Mapping> paths;
    for (const auto& window : windows_) {
        if (window.terminal)
            continue;
        // Split at every downstream alias boundary. This exposes narrow shared
        // subranges inside a broad VF business alias without decoding elements.
        auto walk = [&](auto&& self,
                        std::uint64_t source,
                        std::uint64_t target,
                        std::uint64_t size,
                        std::vector<const detail::AddressWindow*> hops,
                        AliasAccessPolicy policy) -> void {
            if (hops.size() > 32)
                throw Error("alias depth exceeded in layout report", ErrorKind::Configuration);
            std::uint64_t offset = 0;
            while (offset < size) {
                const auto address = target + offset;
                const detail::AddressWindow* hit = nullptr;
                auto length = size - offset;
                for (const auto& candidate : windows_) {
                    if (address >= candidate.source_base &&
                        address - candidate.source_base < candidate.size) {
                        if (!hit || candidate.size < hit->size)
                            hit = &candidate;
                        length =
                            std::min(length, candidate.size - (address - candidate.source_base));
                    }
                    if (candidate.source_base > address)
                        length = std::min(length, candidate.source_base - address);
                }
                if (hit && !hit->terminal) {
                    auto chain = hops;
                    chain.push_back(hit);
                    const auto next = hit->target_base + (address - hit->source_base);
                    if (hit->mapped_offsets)
                        paths.push_back({source + offset,
                                         next,
                                         length,
                                         std::move(chain),
                                         policy.intersect(hit->policy)});
                    else
                        self(self,
                             source + offset,
                             next,
                             length,
                             std::move(chain),
                             policy.intersect(hit->policy));
                } else
                    paths.push_back({source + offset, address, length, hops, policy});
                offset += length;
            }
        };
        // A nested source window replaces the corresponding part of the
        // parent's default mapping. Do not count the hidden path a second time.
        std::vector<std::uint64_t> cuts{window.source_base, window.source_base + window.size};
        for (const auto& child : windows_)
            if (child.size < window.size && child.source_base >= window.source_base &&
                child.source_base - window.source_base <= window.size &&
                child.size <= window.size - (child.source_base - window.source_base)) {
                cuts.push_back(child.source_base);
                cuts.push_back(child.source_base + child.size);
            }
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        for (std::size_t i = 1; i < cuts.size(); ++i)
            if (window_at(cuts[i - 1]) == &window) {
                const auto target = window.target_base + cuts[i - 1] - window.source_base;
                const auto size = cuts[i] - cuts[i - 1];
                auto policy = window.policy.intersect(view_policy(cuts[i - 1], 4));
                if (window.mapped_offsets)
                    paths.push_back({cuts[i - 1], target, size, {&window}, policy});
                else
                    walk(walk, cuts[i - 1], target, size, {&window}, policy);
            }
    }
    std::vector<Row> edges;
    for (std::size_t n = 0; n < links.size(); ++n) {
        const auto& family = links[n];
        const auto& p = family.members.front();
        edges.push_back({"A" + std::to_string(n + 1),
                         endpoint(p.source, p.size, family.compact) + " → " +
                             endpoint(p.target, p.size, family.compact),
                         view.formula(p.source, p.size, family.compact),
                         view.formula(p.target, p.size, family.compact),
                         policy_name(p.policy),
                         path_route(p, view, family.compact)});
    }
    if (full)
        table(out,
              layout_titles::aliases,
              {"ID",
               "Bundle connection",
               "From address range",
               "To address range",
               "Access",
               "Route"},
              std::move(edges));

    // Partition overlapping canonical ranges, so a partial alias is shown only
    // on the subrange it really shares. No identity/index resolver is evaluated.
    std::set<std::uint64_t> boundaries;
    for (const auto& p : paths) {
        boundaries.insert(p.target);
        boundaries.insert(p.target + p.size);
    }
    std::vector<Convergence> regions;
    for (auto it = boundaries.begin(); it != boundaries.end();) {
        const auto base = *it++;
        if (it == boundaries.end())
            break;
        const auto size = *it - base;
        Convergence region{base, size, {}};
        for (const auto& p : paths)
            if (base >= p.target && base - p.target < p.size &&
                size <= p.size - (base - p.target)) {
                const auto* last = p.hops.empty() ? nullptr : p.hops.back();
                if (last && last->mapped_offsets &&
                    !std::any_of(last->mapped_offsets->begin(),
                                 last->mapped_offsets->end(),
                                 [&](const auto& r) {
                                     return detail::overlaps(
                                         base, size, last->target_base + r.first, r.second);
                                 }))
                    continue;
                auto part = p;
                part.source += base - p.target;
                part.target = base;
                part.size = size;
                region.paths.push_back(std::move(part));
            }
        const bool decoded = std::any_of(nodes_.begin(), nodes_.end(), [&](const auto& node) {
            const auto& l = node->layout();
            for (std::size_t i = 0; i < l.elements.size(); ++i)
                if (l.element_mapped[i] &&
                    detail::overlaps(base,
                                     size,
                                     l.elements[i].address,
                                     std::max((node->value_size + 3u) & ~3u, 4u)))
                    return true;
            return false;
        });
        if (decoded && !region.paths.empty())
            regions.push_back(std::move(region));
    }
    auto shared = families(
        regions,
        [&](const Convergence& r) {
            const auto axes = view.address_axes(r.target, r.size);
            auto key = view.symbolic(address_name(r.target, r.size)) + "|" +
                       view.endpoint_key(r.target, r.size, axes);
            std::vector<std::string> routes;
            for (const auto& p : r.paths)
                routes.push_back(view.symbolic(address_name(p.source, p.size)) + "|" +
                                 view.endpoint_key(p.source, p.size, axes) + "|" +
                                 hop_key(p, view));
            std::sort(routes.begin(), routes.end());
            for (const auto& route : routes)
                key += "\n" + route;
            return key;
        },
        [&](const Convergence& r) {
            return view.address_axes(r.target, r.size);
        });
    std::sort(shared.begin(), shared.end(), [](const auto& a, const auto& b) {
        return a.first.front().target < b.first.front().target;
    });
    std::vector<Row> targets;
    for (std::size_t n = 0; n < shared.size(); ++n) {
        const auto& [members, compact] = shared[n];
        const auto& r = members.front();
        const auto axes = view.address_axes(r.target, r.size);
        const auto per_instance = members.front().paths.size() + 1;
        targets.push_back(
            {"R" + std::to_string(n + 1),
             endpoint(r.target, r.size, compact),
             view.formula(r.target, r.size, compact),
             std::to_string(per_instance) + " views" +
                 (compact ? " per instance; separate backing; " + ScopeView::domains(axes)
                          : " share one backing range")});
    }
    table(out,
          full ? layout_titles::canonical : layout_titles::shared,
          {"Target", "Backing bundle / subrange", "Backing address range", "Sharing"},
          std::move(targets));
    if (!shared.empty())
        out << "\n" << layout_titles::paths << "\n";
    for (std::size_t n = 0; n < shared.size(); ++n) {
        const auto& [members, compact] = shared[n];
        const auto& r = members.front();
        std::vector<Mapping> incoming;
        for (const auto& m : members)
            incoming.insert(incoming.end(), m.paths.begin(), m.paths.end());
        auto routes = compact_mappings(incoming, view, address_name);
        const auto target = "R" + std::to_string(n + 1);
        out << "\n[" << target << "] " << endpoint(r.target, r.size, compact) << "\n";
        std::vector<std::pair<std::string, std::string>> lines;
        lines.push_back({endpoint(r.target, r.size, compact) + "  [DIRECT]",
                         view.formula(r.target, r.size, compact)});
        for (const auto& family : routes) {
            const auto& p = family.members.front();
            auto addresses = view.formula(p.source, p.size, family.compact);
            if (family.compact)
                addresses += "; " + ScopeView::domains(view.address_axes(p.source, p.size));
            lines.push_back(
                {endpoint(p.source, p.size, family.compact) + "  [" +
                     (full ? path_route(p, view, family.compact) + "; " : std::string{}) +
                     policy_name(p.policy) + "]",
                 std::move(addresses)});
        }
        std::size_t width = 0;
        for (const auto& [label, detail] : lines)
            width = std::max({width, columns(label), columns(detail) + 2});
        for (const auto& [label, detail] : lines)
            out << "  " << pad(label, width) << " ──┤\n  " << pad("  " + detail, width) << "   │\n";
        out << std::string(width + 5, ' ') << "└──▶ [" << target << "] "
            << view.formula(r.target, r.size, compact) << "\n";
    }
    out << "\nStatic paths show address routing to regions with registers/decoders; internal holes "
           "remain undecoded.\n";
    out << "The shared paths above describe static aliases. Identity/index mapping can select "
           "another instance/element.\n";

    if (!identities_.empty()) {
        out << "\n" << layout_titles::effective << "\n";
        if (std::any_of(identities_.begin(), identities_.end(), [](const auto& w) {
                return !w.resolver;
            }))
            out << "  j = " << callback_name(identity_description_) << "(request); "
                << (identity_resolver_   ? "bound"
                    : identity_deferred_ ? "deferred"
                                         : "unbound")
                << "\n";
        std::vector<std::pair<std::string, std::string>> selections;
        for (const auto& identity : identities_) {
            const auto base = identity.base + identity.region_offset;
            auto source = endpoint(base, identity.region_size, identity.count > 1);
            auto selected = source;
            const auto axes = view.address_axes(base, identity.region_size);
            const auto symbol = axes.empty() ? std::string("i") : axes.back().symbol;
            const auto key = "[" + symbol + "]";
            const auto at = selected.rfind(key);
            const auto effective = symbol == "j" ? std::string("resolved") : std::string("j");
            if (at != std::string::npos)
                selected.replace(at, key.size(), "[" + effective + "]");
            else if (identity.count > 1) {
                source += " {instance i}";
                selected += " {instance j}";
            }
            const bool redirect = identity.mode == IdentityRemapMode::RedirectBank;
            auto from = source + " [" + role(base, identity.region_size) + "]";
            auto to =
                (redirect ? selected : source) +
                (redirect ? "; same offset, then aliases" : "; address unchanged, j is metadata") +
                "; domain=" + std::to_string(identity.domain) + "; request_id=" + symbol + "; " +
                symbol + "=0.." + std::to_string(identity.count - 1);
            if (identity.resolver)
                to += "; j = identity:" + identity.resolver->name + " / " +
                      callback_name(identity.resolver->description) +
                      (identity.resolver->callback   ? " [bound]"
                       : identity.resolver->deferred ? " [deferred]"
                                                     : " [unbound]");
            if (effective != "j")
                to += "; " + effective + "=j";
            selections.push_back({std::move(from), std::move(to)});
        }
        std::size_t width = 0;
        for (const auto& entry : selections)
            width = std::max(width, columns(entry.first));
        for (const auto& entry : selections)
            out << "  " << pad(entry.first, width) << " ──▶ " << entry.second << "\n";
        out << "  RedirectBank selects an instance within each entrance family before following "
               "that instance's aliases.\n";
    }
    const auto warnings = layout_warnings();
    if (!warnings.empty()) {
        std::vector<Row> rows;
        for (const auto& warning : warnings)
            rows.push_back({warning.code,
                            warning.subject,
                            warning.message,
                            std::string(warning.location.file_name()) + ":" +
                                std::to_string(warning.location.line())});
        table(out,
              layout_titles::warnings,
              {"Code", "Declaration", "Reason", "Declared at"},
              std::move(rows));
    }
    if (!full) {
        out << "\nDetails: describe_layout(reg::LayoutDetail::Full) adds alias, identity, index "
               "and callback tables.\n";
        return out.str();
    }

    std::vector<Row> identity_rows;
    for (const auto& identity : identities_) {
        const auto base = identity.base + identity.region_offset;
        auto addresses = range(base, identity.region_size);
        auto instances = std::string("1");
        if (identity.count > 1) {
            addresses += " + i*" + hex(identity.stride);
            instances = "i=0.." + std::to_string(identity.count - 1);
        }
        identity_rows.push_back(
            {view.short_name(view.symbolic(identity.name)),
             endpoint(base, identity.region_size, identity.count > 1),
             role(base, identity.region_size),
             addresses,
             instances,
             std::to_string(identity.domain),
             identity.mode == IdentityRemapMode::RedirectBank
                 ? "request_id → resolved_id (redirect bank)"
                 : "identity metadata only",
             identity.resolver ? "identity:" + identity.resolver->name : "global identity"});
    }
    table(out,
          layout_titles::identity,
          {"Name",
           "Entrance bundle",
           "Role",
           "Address range",
           "Instances",
           "Domain",
           "Resolver effect",
           "Resolver"},
          std::move(identity_rows));

    std::vector<Row> rules, callbacks;
    if (std::any_of(identities_.begin(),
                    identities_.end(),
                    [](const auto& w) {
                        return !w.resolver;
                    }) ||
        identity_resolver_)
        callbacks.push_back({"identity",
                             callback_name(identity_description_),
                             callback_location(identity_description_),
                             identity_resolver_   ? "bound"
                             : identity_deferred_ ? "deferred"
                                                  : "unbound"});
    std::vector<const detail::IdentityResolverSlot*> identity_slots;
    for (const auto& [key, slot] : access_resolvers_->identities) {
        (void)key;
        identity_slots.push_back(slot.get());
    }
    std::sort(identity_slots.begin(), identity_slots.end(), [](auto a, auto b) {
        return a->name < b->name;
    });
    for (const auto* slot : identity_slots)
        callbacks.push_back({"identity:" + slot->name,
                             callback_name(slot->description),
                             callback_location(slot->description),
                             slot->callback   ? "bound"
                             : slot->deferred ? "deferred"
                                              : "unbound"});
    std::vector<const detail::AccessResolverSlot*> slots;
    for (const auto& [key, slot] : access_resolvers_->slots) {
        (void)key;
        slots.push_back(slot.get());
    }
    std::sort(slots.begin(), slots.end(), [](auto a, auto b) {
        return a->name < b->name;
    });
    for (const auto* slot : slots)
        callbacks.push_back({"index:" + slot->name,
                             callback_name(slot->description),
                             callback_location(slot->description),
                             slot->callback   ? "bound"
                             : slot->deferred ? "deferred"
                                              : "unbound"});
    std::unordered_set<std::string> inline_seen;
    for (const auto& node : nodes_)
        for (const auto& group : node->derivations)
            if (group.descriptions)
                for (const auto& rule : *group.descriptions) {
                    std::string resolver;
                    if (rule.resolver_key)
                        resolver = "index:" + rule.resolver_name;
                    else {
                        resolver = group.name + "." + rule.output;
                        const auto name = callback_name(rule.resolver_description),
                                   location = callback_location(rule.resolver_description);
                        const auto token = resolver + '\n' + name + '\n' + location;
                        if (inline_seen.insert(token).second)
                            callbacks.push_back({resolver, name, location, "inline"});
                    }
                    if (!view.visible(node->layout().layout_scope))
                        continue;
                    const auto condition =
                        rule.condition_domain
                            ? "identity_domain(" + std::to_string(*rule.condition_domain) + ")"
                        : rule.conditional ? callback_name(rule.condition_description)
                                           : "always";
                    rules.push_back(
                        {view.short_name(view.symbolic(node->layout().display_name)),
                         group.name,
                         rule.source + "=index[" + std::to_string(rule.dimension) + "]",
                         "→ " + rule.output,
                         condition,
                         rule.fallback.empty() ? "-" : "passthrough(" + rule.fallback + ")",
                         rule.metadata_only ? "metadata only" : "physical element",
                         resolver});
                }
    table(out,
          layout_titles::index,
          {"Register",
           "Group",
           "Source coordinate",
           "Mapped field",
           "When",
           "Otherwise",
           "Routing",
           "Resolver"},
          std::move(rules));
    table(out,
          layout_titles::callbacks,
          {"Resolver / key", "Function / callable", "Registered at", "State"},
          std::move(callbacks));
    return out.str();
}

void RegisterMap::emit_layout() const noexcept {
    if (!log_enabled(HLOG_LEVEL_DEBUG) && !log_enabled(HLOG_LEVEL_TRACE))
        return;
    try {
        auto emit = [&](LayoutDetail detail, HLogLevel level) {
            const auto text = describe_layout(detail);
            std::string_view remaining = text;
            while (!remaining.empty()) {
                const auto end = remaining.find('\n');
                const auto line = remaining.substr(0, end);
                detail::LogRecord record{level,
                                         "Layout",
                                         name_prefix,
                                         nullptr,
                                         0,
                                         {},
                                         {},
                                         nullptr,
                                         line.empty() ? std::string_view{" "} : line};
                emit_log(record);
                if (end == std::string_view::npos)
                    break;
                remaining.remove_prefix(end + 1);
            }
        };
        if (log_enabled(HLOG_LEVEL_DEBUG))
            emit(LayoutDetail::Summary, HLOG_LEVEL_DEBUG);
        if (log_enabled(HLOG_LEVEL_TRACE))
            emit(LayoutDetail::Full, HLOG_LEVEL_TRACE);
    } catch (...) {
    }
}
void RegisterMap::emit_resolver_update(std::string_view key,
                                       const detail::CallbackDescription& description,
                                       bool bound) const noexcept {
    if (!finalized_ || !log_enabled(HLOG_LEVEL_DEBUG))
        return;
    try {
        auto text = "LAYOUT CALLBACK UPDATE: " + name_prefix + " | " + std::string(key) + " → " +
                    callback_name(description) + " | " + callback_location(description) +
                    (bound ? " | bound" : " | unbound");
        detail::LogRecord record{HLOG_LEVEL_DEBUG,
                                 "LayoutResolverUpdate",
                                 name_prefix,
                                 nullptr,
                                 0,
                                 {},
                                 {},
                                 nullptr,
                                 text};
        emit_log(record);
    } catch (...) {
    }
}
} // namespace reg
