#pragma once
#include "dsl.hpp"
#include <ranges>
#include <utility>
namespace reg {
struct RegDumpFieldValue {
    RegData value{};
    unsigned width_bits{};
    std::string debug;
    WideRegData wide_value{};
};
struct RegDumpField {
    std::string name;
    RegDumpFieldValue value;
};
struct RegDumpEntry {
    std::string name;
    RegData raw{};
    unsigned width_bits{};
    std::vector<RegDumpField> fields;
    WideRegData wide_raw{};
};
class RegDump {
    std::vector<RegDumpEntry> entries_;

  public:
    bool empty() const {
        return entries_.empty();
    }
    auto size() const {
        return entries_.size();
    }
    const auto& entries() const {
        return entries_;
    }
    template <class T, std::size_t R, bool V>
    RegDump& push(const RegView<T, R, V>& reg)
        requires(V)
    {
        const auto& value = reg.ref();
        auto raw = detail::to_bits(value);
        RegDumpEntry e{reg.diagnostic_name(),
                       detail::storage_cast<RegData>(raw),
                       detail::value_bits<T>,
                       {},
                       WideRegData(raw)};
        if constexpr (requires { T::field_descriptors; }) {
            for (const auto& field : T::field_descriptors) {
                if (!field.width || field.lsb >= detail::value_bits<T> ||
                    field.width > detail::value_bits<T> - field.lsb)
                    throw Error("dump field outside register width", ErrorKind::Configuration);
                if constexpr (detail::wide_value_v<T>) {
                    auto part = (e.wide_raw >> field.lsb) & WideRegData::mask(field.width);
                    e.fields.push_back({std::string(field.name),
                                        {part.low_u64(),
                                         field.width,
                                         field.debug ? field.debug(e.wide_raw) : std::string{},
                                         part}});
                } else
                    e.fields.push_back({std::string(field.name),
                                        {(e.raw >> field.lsb) & detail::low_mask(field.width),
                                         field.width,
                                         field.debug ? field.debug(e.raw) : std::string{}}});
            }
        } else if constexpr (requires { value.reg_dump_fields(); })
            e.fields = value.reg_dump_fields();
        entries_.push_back(std::move(e));
        return *this;
    }
    template <class R>
    RegDump& push_all(const R& r) {
        for (std::size_t i = 0; i < r.size(); ++i)
            push(r.at_element(i));
        return *this;
    }
    template <class R>
    RegDump& push_range(const R& r, std::size_t begin, std::size_t end) {
        if (end < begin || end > r.size())
            throw Error("dump range outside register");
        for (auto i = begin; i < end; ++i)
            push(r.at_element(i));
        return *this;
    }
    template <class R, std::ranges::input_range I>
    RegDump& push_range(const R& r, I&& indices) {
        for (auto i : indices)
            push(r.at_element(static_cast<std::size_t>(i)));
        return *this;
    }
    std::string str() const;
};
std::ostream& operator<<(std::ostream& out, const RegDump& dump);
template <class... R>
RegDump reg_dump(const R&... r) {
    RegDump dump;
    (dump.push(r), ...);
    return dump;
}
} // namespace reg
// namespace reg
