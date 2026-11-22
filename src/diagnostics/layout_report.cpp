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
std::string policy_name(const AliasAccessPolicy& policy) {
    auto denied = [](DeniedAccessAction action) {
        return action == DeniedAccessAction::Ignore ? "ignore" : "error";
    };
    return std::string(policy.allow_read ? "R" : "R:") +
           (!policy.allow_read ? denied(policy.denied_read) : "") + ", " +
           (policy.allow_write ? "W" : std::string("W:") + denied(policy.denied_write));
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

  public:
    explicit ScopeView(const std::vector<detail::LayoutScope>& scopes) : scopes_(scopes) {
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
};
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
                              {}});
    return layout_scopes_.size() - 1;
}

std::string RegisterMap::describe_layout(LayoutDetail detail) const {
    require_initialized();
    std::ostringstream out;
    out << "COMPOSED ENTITY / VIEW LAYOUT (static; resolvers are not executed)\n";
    out << "Rows describe address views; entity: names referenced storage, not a new "
           "allocation. "
           "Ranges are mapping bounds, not occupied register spans.\n";
    out << "Folded repeats show the entity target/reset of the representative i=j=k=0 "
           "instance; "
           "a shared entity keeps the same target name across entrances.\n";
    const ScopeView scopes(layout_scopes_);
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
        regions.push_back({scopes.symbolic(scope.name), std::move(addresses), std::move(purpose)});
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
        std::map<std::tuple<std::string_view, std::string_view, std::uint64_t, const char*>, Group>
            groups;
        auto consume = [&](std::uint64_t address, const AliasEntry& entry) {
            if (!entry.target_entry.node)
                return; // Reserved regions are in the view table.
            const auto& target = entry.target_entry;
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
        if (formula_)
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
            families[{name, g.node->name.substr(dot == std::string::npos ? 0 : dot + 1), origin}]
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
                           a.priority == b.priority && a.policy.allow_read == b.policy.allow_read &&
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
                    shape +=
                        "; dims=" + list(n.dims) + "; strides=" + list(n.layout().strides, true);
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
