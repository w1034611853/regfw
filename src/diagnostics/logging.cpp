#include "internal/runtime_impl.hpp"
#include <cstring>
#include <sstream>
#include <iomanip>

namespace reg {
std::string detail::declaration(std::string_view name,
                                std::uint64_t base,
                                std::uint64_t size,
                                std::source_location location) {
    std::ostringstream out;
    out << name << " [0x" << std::hex << base << ", 0x" << base + size << ")" << std::dec << " ("
        << size << " bytes) at " << location.file_name() << ':' << location.line();
    return out.str();
}

void RegisterMap::warn_truncated_write(std::uint64_t address,
                                       RegData value,
                                       unsigned size) const noexcept {
    if (size >= 8 || !RegisterAccessWidthPolicy::size_bit(size) ||
        !(value & ~detail::low_mask(size * 8)) || !log_enabled(HLOG_LEVEL_WARN))
        return;
    const detail::FailedAccess request{RegTransactionKind::Write, address, value, size};
    detail::LogRecord record{HLOG_LEVEL_WARN,
                             "write-truncation",
                             name_prefix,
                             nullptr,
                             value,
                             {},
                             "write value exceeds access size; high bits discarded. mmio_write "
                             "defaults to 4 bytes; use mmio_write_sized for an explicit width"};
    record.request = &request;
    emit_log(record);
}

void RegisterMap::log_request_error(RegTransactionKind kind,
                                    std::uint64_t address,
                                    RegData value,
                                    unsigned size) const noexcept {
    if (!log_enabled(HLOG_LEVEL_ERROR))
        return;
    try {
        const auto error = format_error_chain(std::current_exception());
        const detail::FailedAccess request{kind, address, value, size};
        detail::LogRecord record{HLOG_LEVEL_ERROR,
                                 kind == RegTransactionKind::Write ? "write" : "read",
                                 name_prefix,
                                 nullptr,
                                 value,
                                 {},
                                 error};
        record.request = &request;
        emit_log(record);
    } catch (...) {
    }
}

bool RegisterMap::log_enabled(HLogLevel level) const noexcept {
    return logger_ && hlog_enabled(logger_, static_cast<std::uint32_t>(level));
}

void RegisterMap::emit_log(const detail::LogRecord& record) const noexcept {
    // Ask the native runtime before allocating or formatting any text.
    if (!log_enabled(record.level))
        return;
    try {
        const auto text = detail::format_log_record(record);
        const auto& location = record.location;
        const auto* file = location.file_name();
        (void)hlog_write(logger_,
                         static_cast<std::uint32_t>(record.level),
                         reinterpret_cast<const std::uint8_t*>(""),
                         0,
                         reinterpret_cast<const std::uint8_t*>(file),
                         std::strlen(file),
                         location.line(),
                         reinterpret_cast<const std::uint8_t*>(text.data()),
                         text.size());
    } catch (...) {
    } // Diagnostics must not turn a valid MMIO transaction into a fault.
}

namespace {
std::string hex_value(RegData value, unsigned digits = 0) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setfill('0') << std::setw(digits) << value;
    return out.str();
}
std::string coordinates(const RegElementIndex& index) {
    std::ostringstream out;
    for (auto n : index.coordinates())
        out << '[' << n << ']';
    return out.str();
}
std::string_view route_name(AccessRouteKind route) {
    switch (route) {
        case AccessRouteKind::Direct:
            return "DIRECT";
        case AccessRouteKind::Alias:
            return "ALIAS";
        case AccessRouteKind::VfToPf:
            return "VF_TO_PF";
        case AccessRouteKind::PfToVf:
            return "PF_TO_VF";
        case AccessRouteKind::VfToVf:
            return "VF_TO_VF";
        case AccessRouteKind::PfToPf:
            return "PF_TO_PF";
    }
    return "UNKNOWN";
}
void identity_text(std::ostream& out, const RegAccessInfo& i) {
    if (i.request_identity) {
        const bool vf = i.route.kind == AccessRouteKind::VfToPf ||
                        i.route.kind == AccessRouteKind::PfToVf ||
                        i.route.kind == AccessRouteKind::VfToVf;
        out << " | " << (vf ? "vf=" : "id=") << *i.request_identity;
        if (i.resolved_identity && i.resolved_identity != i.request_identity)
            out << "->" << *i.resolved_identity;
    }
    if (i.source_instance && (!i.request_identity || i.source_instance != i.request_identity))
        out << " | src=" << *i.source_instance;
    if (i.request_index != i.index)
        out << " | index=" << coordinates(i.request_index) << "->" << coordinates(i.index);
}
} // namespace

std::string detail::format_log_record(const detail::LogRecord& r) {
    if (!r.message.empty() && !r.info && !r.request)
        return std::string(r.message);
    std::ostringstream out;
    const auto dot = r.name.rfind('.');
    const auto short_name = dot == std::string_view::npos ? r.name : r.name.substr(dot + 1);
    const bool access = r.phase == "Write" || r.phase == "Read";
    const bool detailed =
        r.level == HLOG_LEVEL_TRACE || r.level == HLOG_LEVEL_ERROR || r.level == HLOG_LEVEL_WARN;
    if (access && r.info) {
        const auto& i = *r.info;
        out << (i.is_write() ? "WRITE " : "READ ") << short_name;
        if (i.is_write()) {
            if (i.wide_values)
                out << ": 0x" << i.wide().stored_before.hex() << " -> 0x"
                    << i.wide().stored_after.hex();
            else {
                const auto digits = (i.register_width_bits + 3) / 4;
                out << ": " << hex_value(i.stored_before, digits) << " -> "
                    << hex_value(i.stored_after, digits);
            }
        } else
            out << " -> " << hex_value(r.value, i.size * 2);
        if (!r.transaction_action.empty())
            out << " | txn=" << r.transaction_action;
        if (!detailed) {
            out << " | addr=" << hex_value(i.address, 8) << " | size=" << unsigned(i.size) << 'B';
            if (i.is_write())
                out << " | write=" << hex_value(i.raw_data, i.size * 2);
            identity_text(out, i);
        }
    } else {
        if (r.level == HLOG_LEVEL_ERROR)
            out << "ERROR ";
        else if (r.level == HLOG_LEVEL_WARN)
            out << "WARN ";
        out << r.phase << ' ' << short_name;
        if (!r.message.empty())
            out << ": " << r.message;
    }
    if (r.request) {
        const auto& q = *r.request;
        out << " | op=" << (q.kind == RegTransactionKind::Write ? "write" : "read")
            << " | addr=" << hex_value(q.address, 8) << " | size=" << q.size << 'B';
        if (q.kind == RegTransactionKind::Write)
            out << " | write=" << hex_value(q.value);
    }
    if (r.info && detailed) {
        const auto& i = *r.info;
        out << (r.level == HLOG_LEVEL_TRACE ? " | addr: " : "\n  addr: ")
            << hex_value(i.address, 8);
        if (i.alias_resolved_address != i.address && i.alias_resolved_address != i.resolved_address)
            out << " -> alias=" << hex_value(i.alias_resolved_address, 8);
        if (i.resolved_address != i.address)
            out << " -> " << hex_value(i.resolved_address, 8);
        out << " | size=" << unsigned(i.size) << 'B';
        if (i.is_write())
            out << " | write=" << hex_value(i.raw_data, i.size * 2);
        if (i.wide_values) {
            out << " | mask=0x" << i.wide().bus_mask.hex();
            if (i.wide().bus_data != WideRegData(i.raw_data) || r.level == HLOG_LEVEL_ERROR)
                out << " | data=0x" << i.wide().bus_data.hex();
        } else {
            out << " | mask=" << hex_value(i.bus_mask);
            if (i.bus_data != i.raw_data || r.level == HLOG_LEVEL_ERROR)
                out << " | data=" << hex_value(i.bus_data);
        }
        out << (r.level == HLOG_LEVEL_TRACE ? " | route=" : "\n  route=")
            << route_name(i.route.kind);
        identity_text(out, i);
        if (i.identity_domain)
            out << " | domain=" << *i.identity_domain;
        if (!i.access_group.empty())
            out << " | group=" << i.access_group;
        for (const auto* fields : {&i.index_fields, &i.mapped_fields})
            for (std::size_t n = 0; n < fields->size; ++n)
                out << " | " << fields->entries[n].name << '=' << fields->entries[n].value;
        if (r.level == HLOG_LEVEL_ERROR) {
            out << "\n  path=" << r.name << " | index=" << coordinates(i.index);
            if (i.wide_values)
                out << " | before=0x" << i.wide().stored_before.hex() << " after=0x"
                    << i.wide().stored_after.hex();
            else
                out << " | before=" << hex_value(i.stored_before)
                    << " after=" << hex_value(i.stored_after);
        }
    }
    if (!r.error.empty())
        out << "\n  error: " << r.error;
    return out.str();
}
} // namespace reg
