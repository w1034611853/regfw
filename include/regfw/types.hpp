#pragma once
#include "value.hpp"
#include <algorithm>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace reg {
namespace detail {
struct FormulaBuild;
struct MappedAccessFrame;
} // namespace detail
inline constexpr unsigned REGISTER_CALLBACK_API_VERSION = 4;
inline constexpr unsigned SNAPSHOT_FORMAT_VERSION = 2;
inline constexpr RegData REG_READ_CALLBACK_ERROR_MAGIC = 0;
// User's current Rust setting.
inline constexpr RegData INVALID_READ_VALUE = 0xDEADBEEFDEADBEEFull;
inline constexpr std::size_t MAX_REG_DIMS = 8, MAX_REG_ACCESS_FIELDS = 16;
enum class RegMemTxResult { Ok, Ignored, DecodeError, AccessError, Unsupported, DeviceError };
constexpr bool is_success(RegMemTxResult r) {
    return r == RegMemTxResult::Ok || r == RegMemTxResult::Ignored;
}
constexpr std::uint32_t qemu_memtx_result_bits(RegMemTxResult r) {
    switch (r) {
        case RegMemTxResult::Ok:
        case RegMemTxResult::Ignored:
            return 0;
        case RegMemTxResult::DecodeError:
            return 2;
        case RegMemTxResult::AccessError:
            return 4;
        default:
            return 1;
    }
}
template <class T>
constexpr T map_qemu(RegMemTxResult r, T ok, T error, T decode, T access) {
    switch (qemu_memtx_result_bits(r)) {
        case 0:
            return ok;
        case 2:
            return decode;
        case 4:
            return access;
        default:
            return error;
    }
}
struct LayoutWarning {
    std::string code, subject, message;
    std::source_location location;
};
namespace detail {
class ViewCompiler;
class ViewDescription;
class FormulaInstaller;
std::string declaration(std::string_view name,
                        std::uint64_t base,
                        std::uint64_t size,
                        std::source_location location);
} // namespace detail
enum class ErrorKind {
    Configuration,
    Decode,
    Unsupported,
    Access,
    Device,
    Identity,
    Callback,
    Snapshot
};
class Error : public std::runtime_error {
  public:
    ErrorKind kind;
    RegMemTxResult result;
    std::source_location location;
    std::exception_ptr cause;
    std::string register_dump;
    explicit Error(std::string message,
                   ErrorKind k = ErrorKind::Device,
                   std::source_location loc = std::source_location::current(),
                   std::exception_ptr source = {})
        : std::runtime_error(std::move(message)), kind(k),
          result(k == ErrorKind::Decode        ? RegMemTxResult::DecodeError
                 : k == ErrorKind::Unsupported ? RegMemTxResult::Unsupported
                 : k == ErrorKind::Access || k == ErrorKind::Identity
                     ? RegMemTxResult::AccessError
                     : RegMemTxResult::DeviceError),
          location(loc), cause(std::move(source)) {
    }
    Error& with_dump(std::string dump) {
        register_dump = std::move(dump);
        return *this;
    }
};
using RegCallbackError = Error;
using IdentityResolveError = Error;
class RegAccessDeriveError : public Error {
  public:
    RegAccessDeriveError(RegMemTxResult r,
                         std::string message,
                         std::source_location loc = std::source_location::current())
        : Error(std::move(message), ErrorKind::Device, loc) {
        result = r;
    }
    static RegAccessDeriveError
    access_error(std::string s, std::source_location l = std::source_location::current()) {
        return {RegMemTxResult::AccessError, std::move(s), l};
    }
    static RegAccessDeriveError
    device_error(std::string s, std::source_location l = std::source_location::current()) {
        return {RegMemTxResult::DeviceError, std::move(s), l};
    }
};
std::string format_error_chain(std::exception_ptr error, unsigned depth = 0);
[[noreturn]] inline void
rethrow_located(std::string message, std::source_location loc = std::source_location::current()) {
    throw Error(std::move(message), ErrorKind::Callback, loc, std::current_exception());
}
// A foreign error is wrapped as a located terminal cause, matching Rust located().
template <class F>
decltype(auto) located(F&& call, std::source_location loc = std::source_location::current()) {
    try {
        return std::invoke(std::forward<F>(call));
    } catch (const Error&) {
        throw;
    } catch (const std::exception& e) {
        throw Error(e.what(), ErrorKind::Callback, loc);
    } catch (...) {
        throw Error("foreign non-standard exception", ErrorKind::Callback, loc);
    }
}
template <class F>
decltype(auto)
context(std::string message, F&& call, std::source_location loc = std::source_location::current()) {
    try {
        return std::invoke(std::forward<F>(call));
    } catch (...) {
        throw Error(std::move(message), ErrorKind::Callback, loc, std::current_exception());
    }
}
struct RegMmioReadResult {
    RegData value{};
    RegMemTxResult result{RegMemTxResult::Ok};
    constexpr bool success() const {
        return is_success(result);
    }
    constexpr auto qemu_bits() const {
        return qemu_memtx_result_bits(result);
    }
};
struct RegMmioWriteResult {
    RegMemTxResult result{RegMemTxResult::Ok};
    constexpr bool success() const {
        return is_success(result);
    }
    constexpr auto qemu_bits() const {
        return qemu_memtx_result_bits(result);
    }
};
enum class RegTransactionKind { Read, Write };
struct RegElementIndex {
    std::uint32_t linear_value{};
    std::uint8_t rank_value{1};
    std::array<std::uint32_t, MAX_REG_DIMS> values{};
    static constexpr RegElementIndex from_dims(std::span<const std::size_t> dims,
                                               std::size_t linear) {
        if (dims.empty() || dims.size() > MAX_REG_DIMS)
            throw Error("invalid register rank", ErrorKind::Configuration);
        std::size_t total = 1;
        for (auto d : dims) {
            if (!d || total > UINT32_MAX / d)
                throw Error("invalid/overflowing register dimensions", ErrorKind::Configuration);
            total *= d;
        }
        if (linear >= total)
            throw Error("element index out of range", ErrorKind::Access);
        RegElementIndex out;
        out.linear_value = static_cast<std::uint32_t>(linear);
        out.rank_value = static_cast<std::uint8_t>(dims.size());
        for (auto i = dims.size(); i-- > 0;) {
            out.values[i] = static_cast<std::uint32_t>(linear % dims[i]);
            linear /= dims[i];
        }
        return out;
    }
    constexpr std::uint32_t linear() const {
        return linear_value;
    }
    constexpr std::size_t rank() const {
        return rank_value;
    }
    constexpr auto coordinates() const {
        return std::span(values.data(), rank_value);
    }
    constexpr std::optional<std::uint32_t> dimension(std::size_t i) const {
        return i < rank_value ? std::optional(values[i]) : std::nullopt;
    }
    bool operator==(const RegElementIndex&) const = default;
};
namespace detail {
struct TrustedAccessTag {};
} // namespace detail
template <std::size_t Rank>
struct TypedRegElementIndex {
    static_assert(Rank >= 1 && Rank <= MAX_REG_DIMS, "index rank must be 1..MAX_REG_DIMS");
    const RegElementIndex* value;
    constexpr explicit TypedRegElementIndex(const RegElementIndex& v) : value(&v) {
        if constexpr (Rank != 0)
            if (v.rank() != Rank)
                throw Error("typed index rank mismatch");
    }
    constexpr TypedRegElementIndex(const RegElementIndex& v, detail::TrustedAccessTag) : value(&v) {
    }
    constexpr RegElementIndex raw() const {
        return *value;
    }
    constexpr auto linear() const {
        return value->linear();
    }
    constexpr auto rank() const {
        return value->rank();
    }
    constexpr auto coordinates() const {
        return value->coordinates();
    }
    constexpr auto dimension(std::size_t i) const {
        return value->dimension(i);
    }
    constexpr auto get() const
        requires(Rank > 0)
    {
        if constexpr (Rank == 1)
            return value->values[0];
        else
            return [this]<std::size_t... I>(std::index_sequence<I...>) {
                return std::tuple{value->values[I]...};
            }(std::make_index_sequence<Rank>{});
    }
};
template <class...>
class StaticView;
class Groups;
class RegisterMap;
class AddressView;
struct RegAccessInfo;
struct RegAccessFieldSet;
namespace detail {
struct LogRecord;
std::string format_log_record(const LogRecord&);
} // namespace detail
template <class Tag, class T = std::uint32_t>
struct FieldKey {
    using Value = T;
    std::string_view name;

  private:
    friend class Groups;
    friend class RegisterMap;
    friend struct RegAccessInfo;
    friend struct detail::MappedAccessFrame;
    friend struct RegAccessFieldSet;
    friend class AccessCoordinate;
    friend class detail::ViewDescription;
    template <class...>
    friend class StaticView;
    template <class, class>
    friend class IndexMappingBuilder;
    static inline const unsigned char identity = 0;
    constexpr const void* id() const {
        return &identity;
    }
};
template <class Tag>
struct PreAccessKey {
    std::string_view name;

  private:
    friend class RegisterMap;
    friend class AddressView;
    template <class...>
    friend class StaticView;
    static inline const unsigned char identity = 0;
    constexpr const void* id() const {
        return &identity;
    }
};
namespace detail {
struct StoredField {
    const void* key{};
    std::string_view name;
    RegData value{};
    std::optional<unsigned> dimension;
};
} // namespace detail
struct RegAccessFieldSet {
  private:
    friend class Groups;
    friend class RegisterMap;
    friend struct RegAccessInfo;
    friend struct detail::MappedAccessFrame;
    friend std::string detail::format_log_record(const detail::LogRecord&);

    // MMIO borrows a map-local scratch array. Copies own their records so
    // a saved RegAccessInfo remains independent of subsequent accesses.
    struct FieldArray {
        using Array = std::array<detail::StoredField, MAX_REG_ACCESS_FIELDS>;
        std::unique_ptr<Array> owned;
        detail::StoredField* values{};
        void activate() {
            if (!values) {
                owned = std::make_unique<Array>();
                values = owned->data();
            }
        }
        auto& operator[](std::size_t i) {
            return values[i];
        }
        const auto& operator[](std::size_t i) const {
            return values[i];
        }
        static constexpr std::size_t size() {
            return MAX_REG_ACCESS_FIELDS;
        }
    } entries;
    std::size_t size{};
    void borrow(FieldArray::Array& storage) {
        entries.owned.reset();
        entries.values = storage.data();
        size = 0;
    }
    const detail::StoredField* get(const void* key) const {
        for (std::size_t i = 0; i < size; ++i)
            if (entries[i].key == key)
                return &entries[i];
        return nullptr;
    }
    void insert(const void* key,
                std::string_view name,
                RegData value,
                std::optional<unsigned> dim = {}) {
        for (std::size_t i = 0; i < size; ++i)
            if (entries[i].key == key) {
                auto& e = entries[i];
                if (e.value != value || (e.dimension && dim && e.dimension != dim))
                    throw RegAccessDeriveError::device_error("conflicting access field: " +
                                                             std::string(name));
                if (!e.dimension)
                    e.dimension = dim;
                return;
            }
        if (size == entries.size())
            throw RegAccessDeriveError::device_error("access field capacity exceeded");
        if (!size)
            entries.activate();
        entries[size++] = {key, name, value, dim};
    }
    template <class K>
    auto read(K key) const -> typename K::Value {
        auto p = get(key.id());
        if (!p)
            throw RegAccessDeriveError::device_error("missing access field: " +
                                                     std::string(key.name));
        return detail::value_from_data<typename K::Value>(p->value);
    }

  public:
    RegAccessFieldSet() noexcept = default;
    RegAccessFieldSet(const RegAccessFieldSet& other) : size(other.size) {
        if (size) {
            entries.activate();
            std::copy_n(other.entries.values, size, entries.values);
        }
    }
    RegAccessFieldSet& operator=(const RegAccessFieldSet& other) {
        if (this == &other)
            return *this;
        RegAccessFieldSet copy(other);
        swap(copy);
        return *this;
    }
    RegAccessFieldSet(RegAccessFieldSet&& other) noexcept {
        swap(other);
    }
    RegAccessFieldSet& operator=(RegAccessFieldSet&& other) noexcept {
        if (this != &other) {
            RegAccessFieldSet moved(std::move(other));
            swap(moved);
        }
        return *this;
    }
    void swap(RegAccessFieldSet& other) noexcept {
        entries.owned.swap(other.entries.owned);
        std::swap(entries.values, other.entries.values);
        std::swap(size, other.size);
    }
};
enum class AccessRouteKind : std::uint32_t {
    Direct = 0,
    Alias = 1,
    VfToPf = 0x100,
    PfToVf = 0x101,
    VfToVf = 0x102,
    PfToPf = 0x103
};
struct AccessRoute {
    AccessRouteKind kind{AccessRouteKind::Direct};
    std::uint32_t instance{};
    constexpr bool is_direct() const {
        return kind == AccessRouteKind::Direct;
    }
    static constexpr AccessRoute vf_to_pf(std::uint32_t i) {
        return {AccessRouteKind::VfToPf, i};
    }
    static constexpr AccessRoute pf_to_vf(std::uint32_t i) {
        return {AccessRouteKind::PfToVf, i};
    }
    bool operator==(const AccessRoute&) const = default;
    static const AccessRoute DIRECT, ALIAS;
};
inline constexpr AccessRoute AccessRoute::DIRECT{};
inline constexpr AccessRoute AccessRoute::ALIAS{AccessRouteKind::Alias, 0};
enum class DeniedAccessAction { Ignore, Error };
struct AliasAccessPolicy {
    friend class detail::ViewCompiler;
    friend struct detail::FormulaBuild;
    friend class RegisterMap;
    friend class detail::FormulaInstaller;
    bool allow_read{true}, allow_write{true};
    DeniedAccessAction denied_read{DeniedAccessAction::Error},
        denied_write{DeniedAccessAction::Error};

  private:
    constexpr AliasAccessPolicy intersect(AliasAccessPolicy b) const {
        auto action = [](bool a_allow, DeniedAccessAction a, bool b_allow, DeniedAccessAction c) {
            if (!a_allow && !b_allow)
                return a == DeniedAccessAction::Error || c == DeniedAccessAction::Error
                           ? DeniedAccessAction::Error
                           : DeniedAccessAction::Ignore;
            return !a_allow ? a : c;
        };
        return {allow_read && b.allow_read,
                allow_write && b.allow_write,
                action(allow_read, denied_read, b.allow_read, b.denied_read),
                action(allow_write, denied_write, b.allow_write, b.denied_write)};
    }

  public:
    static const AliasAccessPolicy READ_WRITE, READ_ONLY_IGNORE_WRITES, READ_ONLY_ERROR_WRITES,
        WRITE_ONLY_ERROR_READS, NO_ACCESS;
};
inline constexpr AliasAccessPolicy AliasAccessPolicy::READ_WRITE{};
inline constexpr AliasAccessPolicy AliasAccessPolicy::READ_ONLY_IGNORE_WRITES{
    true, false, DeniedAccessAction::Error, DeniedAccessAction::Ignore};
inline constexpr AliasAccessPolicy AliasAccessPolicy::READ_ONLY_ERROR_WRITES{true, false};
inline constexpr AliasAccessPolicy AliasAccessPolicy::WRITE_ONLY_ERROR_READS{false, true};
inline constexpr AliasAccessPolicy AliasAccessPolicy::NO_ACCESS{false, false};
enum class IdentityRemapMode { IdentityOnly, RedirectBank };
// Applies only to explicit identity rejections, never to resolver bugs.
enum class IdentityRejectionPolicy { AccessError, Ignore };
enum class LayoutDetail { Summary, Full };
enum class IdentityAddressSource { Bus, Canonical };
struct IdentityTraceLabels {
    std::string_view request{"reqid"}, resolved{"resid"};
};
struct IdentityResolveRequest {
    std::uint32_t domain{}, request_id{};
    std::uint64_t bus_offset{}, canonical_offset{}, offset_in_instance{};
    std::uint8_t access_size{};
    AccessRoute route;
    IdentityAddressSource source{};
};
struct AccessIdentity {
    std::uint32_t domain{}, request_id{}, resolved_id{};
    IdentityRemapMode mode{};
    IdentityAddressSource source{};
    IdentityTraceLabels trace_labels;
    constexpr bool was_remapped() const {
        return request_id != resolved_id;
    }
};
struct WideAccessValues {
    WideRegData raw_data{}, bus_data{}, bus_mask{}, stored_before{}, stored_after{};
};
struct RegAccessInfo {
    RegTransactionKind kind{};
    std::uint64_t address{}, alias_resolved_address{}, resolved_address{};
    std::uint8_t size{}, internal_byte_offset{};
    RegElementIndex request_index, index;
    RegData raw_data{}, bus_data{}, bus_mask{}, stored_before{}, stored_after{};
    // Scalar fields above are the low 64 bits; wide_values exposes full values.
    unsigned register_width_bits{};
    const WideAccessValues* wide_values{};
    bool is_wide() const {
        return register_width_bits > 64;
    }
    const WideAccessValues& wide() const {
        if (!wide_values)
            throw Error("access has no wide values");
        return *wide_values;
    }
    AccessRoute route;
    AliasAccessPolicy alias_access_policy;
    std::optional<std::uint32_t> source_instance, request_identity, resolved_identity,
        identity_domain;
    std::optional<IdentityRemapMode> identity_mode;
    std::optional<IdentityAddressSource> identity_source;
    std::string_view access_group;
    std::string_view
        view_path; // Effective selected AddressView path; original bus address stays in address.
    RegAccessFieldSet index_fields, mapped_fields;
    bool is_alias{};
    bool is_read() const {
        return kind == RegTransactionKind::Read;
    }
    bool is_write() const {
        return kind == RegTransactionKind::Write;
    }
    bool has_identity() const {
        return identity_domain.has_value();
    }
    bool is_identity_domain(std::uint32_t d) const {
        return identity_domain == d;
    }
    auto request_id_or_instance() const {
        return request_identity ? request_identity : source_instance;
    }
    bool was_identity_remapped() const {
        return request_identity && resolved_identity && request_identity != resolved_identity;
    }
    std::uint32_t request_id() const {
        if (!request_identity)
            throw RegAccessDeriveError::device_error("no request identity");
        return *request_identity;
    }
    std::uint32_t resolved_id() const {
        if (!resolved_identity)
            throw RegAccessDeriveError::device_error("no resolved identity");
        return *resolved_identity;
    }
    template <class K>
    auto index_as(K key) const {
        return index_fields.read(key);
    }
    template <class K>
    auto mapped_as(K key) const {
        return mapped_fields.read(key);
    }
};
template <std::size_t R>
struct TypedRegAccessInfo {
    const RegAccessInfo& raw;
    TypedRegElementIndex<R> request_index, index;
    const RegTransactionKind& kind = raw.kind;
    const std::uint64_t &address = raw.address,
                        &alias_resolved_address = raw.alias_resolved_address,
                        &resolved_address = raw.resolved_address;
    const std::uint8_t &size = raw.size, &internal_byte_offset = raw.internal_byte_offset;
    const RegData &raw_data = raw.raw_data, &bus_data = raw.bus_data, &bus_mask = raw.bus_mask,
                  &stored_before = raw.stored_before, &stored_after = raw.stored_after;
    const AccessRoute& route = raw.route;
    const AliasAccessPolicy& alias_access_policy = raw.alias_access_policy;
    const std::optional<std::uint32_t>&source_instance = raw.source_instance,
          &identity_domain = raw.identity_domain;
    const std::optional<IdentityRemapMode>& identity_mode = raw.identity_mode;
    const std::optional<IdentityAddressSource>& identity_source = raw.identity_source;
    const std::string_view& access_group = raw.access_group;
    const RegAccessFieldSet &index_fields = raw.index_fields, &mapped_fields = raw.mapped_fields;
    const bool& is_alias = raw.is_alias;
    explicit TypedRegAccessInfo(const RegAccessInfo& i)
        : raw(i), request_index(i.request_index), index(i.index) {
    }
    TypedRegAccessInfo(const RegAccessInfo& i, detail::TrustedAccessTag tag)
        : raw(i), request_index(i.request_index, tag), index(i.index, tag) {
    }
    operator const RegAccessInfo&() const noexcept {
        return raw;
    }
    auto request_id() const {
        return raw.request_id();
    }
    auto resolved_id() const {
        return raw.resolved_id();
    }
    auto request_id_or_instance() const {
        return raw.request_id_or_instance();
    }
    bool has_identity() const {
        return raw.has_identity();
    }
    bool is_identity_domain(std::uint32_t d) const {
        return raw.is_identity_domain(d);
    }
    bool was_identity_remapped() const {
        return raw.was_identity_remapped();
    }
    bool is_read() const {
        return raw.is_read();
    }
    bool is_write() const {
        return raw.is_write();
    }
    template <class K>
    auto index_as(K key) const {
        return raw.index_as(key);
    }
    template <class K>
    auto mapped_as(K key) const {
        return raw.mapped_as(key);
    }
};
template <std::size_t R>
struct WideTypedRegAccessInfo : TypedRegAccessInfo<R> {
    const WideRegData &raw_data, &bus_data, &bus_mask, &stored_before, &stored_after;
    explicit WideTypedRegAccessInfo(const RegAccessInfo& i)
        : TypedRegAccessInfo<R>(i), raw_data(i.wide().raw_data), bus_data(i.wide().bus_data),
          bus_mask(i.wide().bus_mask), stored_before(i.wide().stored_before),
          stored_after(i.wide().stored_after) {
    }
};
enum class ResetDomains : std::uint32_t {
    NONE = 0,
    POWER_ON = 1,
    COLD = 2,
    WARM = 4,
    FUNCTION = 8,
    VF_FLR = 16,
    SOFTWARE = 32,
    STANDARD = 63,
    ALL = 63
};
constexpr ResetDomains operator|(ResetDomains a, ResetDomains b) {
    return static_cast<ResetDomains>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
constexpr ResetDomains& operator|=(ResetDomains& a, ResetDomains b) {
    return a = a | b;
}
namespace detail {
constexpr bool contains(ResetDomains a, ResetDomains b) {
    return (static_cast<unsigned>(a) & static_cast<unsigned>(b)) == static_cast<unsigned>(b);
}
} // namespace detail

struct ResetScope {
    enum Kind { Global, Pf, Vf } kind{Global};
    std::uint32_t vf_id{};
    static constexpr ResetScope vf(std::uint32_t id) {
        return {Vf, id};
    }
    bool operator==(const ResetScope&) const = default;
};
struct ResetRequest {
    friend class RegisterMap;
    enum Kind { PowerOn, Cold, Warm, PfFunction, VfFlr, Software } kind{PowerOn};
    std::uint32_t vf_id{};

  private:
    constexpr ResetDomains domain() const {
        constexpr ResetDomains d[] = {ResetDomains::POWER_ON,
                                      ResetDomains::COLD,
                                      ResetDomains::WARM,
                                      ResetDomains::FUNCTION,
                                      ResetDomains::VF_FLR,
                                      ResetDomains::SOFTWARE};
        return d[kind];
    }

  private:
    constexpr bool matches_scope(ResetScope s) const {
        return kind == PfFunction ? (s.kind == ResetScope::Global || s.kind == ResetScope::Pf)
               : kind == VfFlr    ? (s.kind == ResetScope::Vf && s.vf_id == vf_id)
                                  : true;
    }
};
enum class RegisterAlignment { Natural, RegisterBase };
struct RegisterAccessWidthPolicy {
    friend struct detail::FormulaBuild;
    friend class RegisterMap;
    unsigned allowed{15};
    RegisterAlignment alignment{RegisterAlignment::Natural};

  private:
    static constexpr unsigned size_bit(unsigned n) {
        return n == 1 ? 1 : n == 2 ? 2 : n == 4 ? 4 : n == 8 ? 8 : 0;
    }

  private:
    constexpr bool allows(unsigned n) const {
        return (allowed & size_bit(n)) != 0;
    }

  public:
    static constexpr RegisterAccessWidthPolicy only(unsigned n) {
        return {size_bit(n), RegisterAlignment::RegisterBase};
    }
    static constexpr RegisterAccessWidthPolicy sizes_1_2_4() {
        return {7};
    }
    static constexpr RegisterAccessWidthPolicy sizes_4_8() {
        return {12};
    }
    constexpr auto with_alignment(RegisterAlignment a) const {
        auto p = *this;
        p.alignment = a;
        return p;
    }
    static const RegisterAccessWidthPolicy LEGACY, U8_ONLY, U16_ONLY, U32_ONLY, U64_ONLY;
};
inline constexpr RegisterAccessWidthPolicy RegisterAccessWidthPolicy::LEGACY{};
inline constexpr RegisterAccessWidthPolicy RegisterAccessWidthPolicy::U8_ONLY = only(1),
                                           RegisterAccessWidthPolicy::U16_ONLY = only(2),
                                           RegisterAccessWidthPolicy::U32_ONLY = only(4),
                                           RegisterAccessWidthPolicy::U64_ONLY = only(8);
enum class SplitReadMode { Independent, SnapshotOnLow, SnapshotOnHigh };
enum class SplitWriteMode { Independent, CommitOnHigh, CommitOnLow };
struct TransactionPolicy {
    SplitReadMode read{SplitReadMode::Independent};
    SplitWriteMode write{SplitWriteMode::Independent};
    constexpr bool independent() const {
        return read == SplitReadMode::Independent && write == SplitWriteMode::Independent;
    }
    static const TransactionPolicy INDEPENDENT, LOW_TO_HIGH, HIGH_TO_LOW;
};
inline constexpr TransactionPolicy TransactionPolicy::INDEPENDENT{};
inline constexpr TransactionPolicy TransactionPolicy::LOW_TO_HIGH{SplitReadMode::SnapshotOnLow,
                                                                  SplitWriteMode::CommitOnHigh};
inline constexpr TransactionPolicy TransactionPolicy::HIGH_TO_LOW{SplitReadMode::SnapshotOnHigh,
                                                                  SplitWriteMode::CommitOnLow};
namespace detail {
struct RegTransactionState {
    WideRegData read_snapshot{}, write_pending{}, write_mask{};
    bool has_read_snapshot{};
    bool read_source_ok{true};
};
} // namespace detail
enum class LogPolicy { Normal, Trace, Hidden };

enum class SnapshotKind { Migratable, Derived, Transient };
struct SnapshotMeta {
    std::string stable_id;
    std::optional<std::uint32_t> order;
    SnapshotKind kind{SnapshotKind::Migratable};
};
// Serialized layout. Policy/shape checks prevent restoring pending traffic
// into an entity with a different register width or transaction protocol.
struct SnapshotLayoutDesc {
    std::string stable_id;
    std::optional<std::uint32_t> order;
    std::size_t start_word{}, len_words{};
    unsigned wide_width_bits{};
    unsigned value_width_bits{};
    std::vector<std::uint64_t> dimensions, strides;
    std::uint32_t transaction_read{}, transaction_write{};
    bool operator==(const SnapshotLayoutDesc&) const = default;
    template <class Archive>
    void VmstateFields(Archive& ar) {
        ar(stable_id,
           order,
           start_word,
           len_words,
           wide_width_bits,
           value_width_bits,
           dimensions,
           strides,
           transaction_read,
           transaction_write);
    }
};
struct SnapshotSchema {
    std::uint32_t format_version{SNAPSHOT_FORMAT_VERSION};
    std::uint64_t layout_hash{};
    std::size_t total_words{};
    std::vector<SnapshotLayoutDesc> slots;
    bool operator==(const SnapshotSchema&) const = default;
    template <class Archive>
    void VmstateFields(Archive& ar) {
        ar(format_version);
        if (format_version != SNAPSHOT_FORMAT_VERSION)
            throw Error("unsupported register snapshot schema version", ErrorKind::Snapshot);
        ar(layout_hash, total_words, slots);
    }
};
struct SnapshotTransactionState {
    std::uint64_t element{};
    bool has_read_snapshot{}, read_source_ok{true};
    // Little-limb order. Absent read/write state uses empty vectors;
    // present payloads have exactly ceil(register_bits/64) words.
    std::vector<RegData> read_snapshot, write_pending, write_mask;
    bool operator==(const SnapshotTransactionState&) const = default;
    template <class Archive>
    void VmstateFields(Archive& ar) {
        ar(element, has_read_snapshot, read_source_ok, read_snapshot, write_pending, write_mask);
    }
};
struct SnapshotEntityRuntime {
    // Sparse, ascending element indices. One runtime entry per schema slot.
    std::vector<SnapshotTransactionState> transactions;
    bool operator==(const SnapshotEntityRuntime&) const = default;
    template <class Archive>
    void VmstateFields(Archive& ar) {
        ar(transactions);
    }
};
struct VersionedSnapshot {
    SnapshotSchema schema;
    std::vector<RegData> data;
    std::vector<SnapshotEntityRuntime> runtime;
    bool operator==(const VersionedSnapshot&) const = default;
    template <class Archive>
    void VmstateFields(Archive& ar) {
        std::uint64_t magic = 0x5245474657534e50ull; // REGFWSNP
        std::uint32_t wire_version = SNAPSHOT_FORMAT_VERSION;
        ar(magic, wire_version);
        if (magic != 0x5245474657534e50ull || wire_version != SNAPSHOT_FORMAT_VERSION)
            throw Error("invalid register snapshot header/version", ErrorKind::Snapshot);
        ar(schema, data, runtime);
    }
};
struct RegisterMetadata {
    std::string name;
    std::uint64_t address{};
    unsigned width_bits{};
    RegData reset_value{}, writable_mask{};
    std::vector<std::size_t> dimensions;
    std::vector<std::uint64_t> strides;
    ResetDomains reset_domains{};
    ResetScope reset_scope;
    RegisterAccessWidthPolicy access_widths;
    LogPolicy log_policy{};
    SnapshotMeta snapshot;
    WideRegData wide_reset_value{}, wide_writable_mask{};
    TransactionPolicy transaction_policy;
};
} // namespace reg
// namespace reg
#define REG_ACCESS_FIELD(NAME, TYPE)                                                               \
    struct NAME##_field_tag {};                                                                    \
    inline constexpr ::reg::FieldKey<NAME##_field_tag, TYPE> NAME {                                \
        #NAME                                                                                      \
    }

#define REG_PRE_ACCESS(NAME)                                                                       \
    struct NAME##_pre_access_tag {};                                                               \
    inline constexpr ::reg::PreAccessKey<NAME##_pre_access_tag> NAME {                             \
        #NAME                                                                                      \
    }
