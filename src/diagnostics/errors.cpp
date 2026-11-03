#include "internal/runtime_impl.hpp"
#include <sstream>
#include <iomanip>
#include <unordered_set>

namespace reg {
std::string format_error_chain(std::exception_ptr error, unsigned depth) {
    if (!error || depth >= 64)
        return {};
    try {
        std::rethrow_exception(error);
    } catch (const Error& e) {
        std::ostringstream s;
        s << e.what() << "\n  at " << e.location.file_name() << ':' << e.location.line() << ':'
          << e.location.column();
        if (!e.register_dump.empty())
            s << "\n" << e.register_dump;
        if (e.cause)
            s << "\n  caused by[" << depth + 1 << "]: " << format_error_chain(e.cause, depth + 1);
        return s.str();
    } catch (const std::exception& e) {
        std::string text = e.what();
#if defined(__cpp_rtti)
        if (auto* n = dynamic_cast<const std::nested_exception*>(&e); n && n->nested_ptr())
            text += "\n  caused by: " + format_error_chain(n->nested_ptr(), depth + 1);
#endif
        return text;
    } catch (...) {
        return "non-standard C++ exception";
    }
}


std::string RegDump::str() const {
    std::ostringstream o;
    if (entries_.empty())
        return "  <no registers>";
    for (auto& e : entries_) {
        o << "  " << e.name << " = 0x";
        if (e.width_bits > 64) {
            auto text = e.wide_raw.hex(true);
            o << text.substr(text.size() - (e.width_bits + 3) / 4);
        } else
            o << std::hex << std::setfill('0')
              << std::setw(e.width_bits <= 8    ? 2
                           : e.width_bits <= 16 ? 4
                           : e.width_bits <= 32 ? 8
                                                : 16)
              << e.raw << std::setfill(' ');
        if (!e.fields.empty()) {
            o << " {";
            bool first = true;
            for (auto& f : e.fields) {
                if (!std::exchange(first, false))
                    o << ", ";
                o << f.name << '=';
                if (!f.value.debug.empty())
                    o << f.value.debug;
                else if (f.value.width_bits > 64)
                    o << "0x" << f.value.wide_value;
                else if (f.value.value <= 1)
                    o << std::dec << f.value.value;
                else
                    o << "0x" << std::hex << f.value.value;
            }
            o << '}';
        }
        o << '\n';
    }
    return o.str();
}

std::ostream& operator<<(std::ostream& out, const RegDump& dump) {
    return out << dump.str();
}
} // namespace reg
namespace reg::detail {
std::ostream& write_wide_text(std::ostream& out, const std::string& text) {
    return out << text;
}
} // namespace reg::detail
