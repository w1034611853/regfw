#ifndef HLOG_HPP
#define HLOG_HPP

#include "hlog.h"

#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(HLOG_USE_SPDLOG_FMT)
#include <spdlog/fmt/fmt.h>
#else
#include <fmt/format.h>
#endif

namespace hlog {

enum class HexdumpFormat : uint32_t { u8 = 1, u16 = 2, u32 = 4, u64 = 8 };
inline constexpr auto u8 = HexdumpFormat::u8;
inline constexpr auto u16 = HexdumpFormat::u16;
inline constexpr auto u32 = HexdumpFormat::u32;
inline constexpr auto u64 = HexdumpFormat::u64;

class Logger {
public:
    Logger() = default;

    Logger(const char *module_name, HLogLevel level = HLOG_LEVEL_INFO, const char *configured_name = nullptr)
        : handle_(hlog_register_logger(module_name, configured_name, static_cast<uint32_t>(level)))
    {
    }

    static Logger from_handle(HLogLogger *handle)
    {
        Logger logger;
        logger.handle_ = handle;
        return logger;
    }

    bool valid() const noexcept
    {
        return handle_ != nullptr;
    }

    explicit operator bool() const noexcept
    {
        return valid();
    }

    HLogLogger *raw_handle() const noexcept
    {
        return handle_;
    }

    bool enabled(HLogLevel level) const noexcept
    {
        return handle_ != nullptr && hlog_enabled(handle_, static_cast<uint32_t>(level));
    }

    HLogLevel level() const noexcept
    {
        return handle_ == nullptr ? HLOG_LEVEL_OFF : static_cast<HLogLevel>(hlog_logger_level(handle_));
    }

    bool set_level(HLogLevel level) const noexcept
    {
        return handle_ != nullptr && hlog_logger_set_level(handle_, static_cast<uint32_t>(level)) == HLOG_OK;
    }

    bool config_name(const char *new_name) const noexcept
    {
        return handle_ != nullptr && new_name != nullptr && hlog_logger_config_name(handle_, new_name) == HLOG_OK;
    }

    void flush() const noexcept
    {
        (void)hlog_flush();
    }

    // Const references permit the language's temporary-value binding for bit
    // fields. Ordinary objects remain referenced, including noncopyable values.
    // Formatting is synchronous; no reference escapes this call.
    template <typename... Args>
    void write_unchecked(HLogLevel level, const char *function, const char *file, uint32_t line, fmt::format_string<Args...> format,
                         const Args &...args) const
    {
        if (handle_ == nullptr) {
            return;
        }

        fmt::memory_buffer message;
        fmt::vformat_to(std::back_inserter(message), format, fmt::make_format_args(args...));
        (void)hlog_write(handle_, static_cast<uint32_t>(level), reinterpret_cast<const uint8_t *>(function),
                         function ? std::strlen(function) : 0, reinterpret_cast<const uint8_t *>(file),
                         file ? std::strlen(file) : 0, line, reinterpret_cast<const uint8_t *>(message.data()),
                         message.size());
    }

    template <typename... Args>
    void console_write(HLogLevel level, const char *function, const char *file, uint32_t line,
                       fmt::format_string<Args...> format, const Args &...args) const
    {
        if (handle_ == nullptr || level == HLOG_LEVEL_OFF) {
            return;
        }
        fmt::memory_buffer message;
        fmt::vformat_to(std::back_inserter(message), format, fmt::make_format_args(args...));
        (void)hlog_console_write(handle_, static_cast<uint32_t>(level),
            reinterpret_cast<const uint8_t *>(function), function ? std::strlen(function) : 0,
            reinterpret_cast<const uint8_t *>(file), file ? std::strlen(file) : 0, line,
            reinterpret_cast<const uint8_t *>(message.data()), message.size());
    }

    void console_hexdump(HLogLevel level, const char *function, const char *file,
                         uint32_t line, const void *data, size_t length) const noexcept
    {
        if (handle_ == nullptr || level == HLOG_LEVEL_OFF) {
            return;
        }
        (void)hlog_console_hexdump(handle_, static_cast<uint32_t>(level),
            reinterpret_cast<const uint8_t *>(function), function ? std::strlen(function) : 0,
            reinterpret_cast<const uint8_t *>(file), file ? std::strlen(file) : 0, line,
            static_cast<const uint8_t *>(data), length);
    }

    void write_message_unchecked(HLogLevel level, const char *function, const char *file, uint32_t line,
                                 std::string_view message) const noexcept
    {
        if (handle_ == nullptr) {
            return;
        }

        (void)hlog_write(handle_, static_cast<uint32_t>(level), reinterpret_cast<const uint8_t *>(function),
                         function ? std::strlen(function) : 0, reinterpret_cast<const uint8_t *>(file),
                         file ? std::strlen(file) : 0, line, reinterpret_cast<const uint8_t *>(message.data()),
                         message.size());
    }

    void hexdump_unchecked(HLogLevel level, const char *function, const char *file, uint32_t line, const void *data,
                           size_t length) const noexcept
    {
        if (handle_ == nullptr) {
            return;
        }

        (void)hlog_hexdump(handle_, static_cast<uint32_t>(level), reinterpret_cast<const uint8_t *>(function),
                           function ? std::strlen(function) : 0, reinterpret_cast<const uint8_t *>(file),
                           file ? std::strlen(file) : 0, line, static_cast<const uint8_t *>(data), length);
    }

private:
    HLogLogger *handle_{nullptr};
};

namespace detail {
inline const HLogLogger *dump_handle(const Logger &logger) noexcept { return logger.raw_handle(); }
inline const HLogLogger *dump_handle(const HLogLogger *logger) noexcept { return logger; }

inline void dump_bytes(const HLogLogger *logger, HLogLevel level, HexdumpFormat format,
                       const char *function, const char *file, uint32_t line,
                       const void *data, size_t length, bool console_only) noexcept
{
    (void)(console_only ? hlog_console_hexdump_ex : hlog_hexdump_ex)(
        logger, static_cast<uint32_t>(level),
        reinterpret_cast<const uint8_t *>(function), std::strlen(function),
        reinterpret_cast<const uint8_t *>(file), std::strlen(file), line,
        static_cast<const uint8_t *>(data), length, static_cast<uint32_t>(format));
}

template <class Buffer>
void dump_buffer(const HLogLogger *logger, HLogLevel level, HexdumpFormat format,
                 const char *function, const char *file, uint32_t line,
                 const Buffer &buffer, bool console_only)
{
    const auto *data = std::data(buffer);
    using Element = std::remove_pointer_t<decltype(data)>;
    static_assert(std::is_trivially_copyable_v<Element> && !std::is_volatile_v<Element>,
                  "hexdump needs contiguous, trivially copyable, non-volatile elements");
    const auto count = std::size(buffer);
    if (count > std::numeric_limits<size_t>::max() / sizeof(Element)) {
        throw std::length_error("hexdump buffer size overflow");
    }
    dump_bytes(logger, level, format, function, file, line, data, count * sizeof(Element), console_only);
}
} // namespace detail

struct QueryParam {
    HLogQueryParamSpec raw;
};

inline QueryParam required_string(const char *name, const char *description = "")
{
    return {{name, HLOG_QUERY_PARAM_STRING, HLOG_QUERY_PARAM_REQUIRED, description}};
}

inline QueryParam optional_string(const char *name, const char *description = "")
{
    return {{name, HLOG_QUERY_PARAM_STRING, 0, description}};
}

inline QueryParam required_u32(const char *name, const char *description = "")
{
    return {{name, HLOG_QUERY_PARAM_U32, HLOG_QUERY_PARAM_REQUIRED, description}};
}

inline QueryParam optional_u32(const char *name, const char *description = "")
{
    return {{name, HLOG_QUERY_PARAM_U32, 0, description}};
}

inline QueryParam repeatable_u32(const char *name, const char *description = "")
{
    return {{name, HLOG_QUERY_PARAM_U32, HLOG_QUERY_PARAM_REPEATABLE, description}};
}

inline QueryParam required_u64(const char *name, const char *description = "")
{
    return {{name, HLOG_QUERY_PARAM_U64, HLOG_QUERY_PARAM_REQUIRED, description}};
}

inline QueryParam optional_u64(const char *name, const char *description = "")
{
    return {{name, HLOG_QUERY_PARAM_U64, 0, description}};
}

inline QueryParam required_i64(const char *name, const char *description = "")
{
    return {{name, HLOG_QUERY_PARAM_I64, HLOG_QUERY_PARAM_REQUIRED, description}};
}

inline QueryParam optional_i64(const char *name, const char *description = "")
{
    return {{name, HLOG_QUERY_PARAM_I64, 0, description}};
}

inline QueryParam required_bool(const char *name, const char *description = "")
{
    return {{name, HLOG_QUERY_PARAM_BOOL, HLOG_QUERY_PARAM_REQUIRED, description}};
}

inline QueryParam optional_bool(const char *name, const char *description = "")
{
    return {{name, HLOG_QUERY_PARAM_BOOL, 0, description}};
}

class QueryArgs {
public:
    explicit QueryArgs(const HLogQueryRequest *request) : request_(request) {}

    size_t count(const char *name) const noexcept
    {
        return hlog_query_arg_count(request_, name);
    }

    std::string_view string(const char *name, size_t occurrence = 0) const
    {
        const char *value = hlog_query_arg_at(request_, name, occurrence);
        if (value == nullptr) {
            throw std::runtime_error(last_error_text());
        }
        return value;
    }

    std::optional<std::string_view> optional_string(const char *name) const
    {
        return count(name) == 0 ? std::nullopt : std::optional<std::string_view>(string(name));
    }

    uint32_t u32(const char *name) const
    {
        uint32_t value = 0;
        if (hlog_query_arg_u32(request_, name, &value) != HLOG_OK) {
            throw std::runtime_error(last_error_text());
        }
        return value;
    }

    std::optional<uint32_t> optional_u32(const char *name) const
    {
        return count(name) == 0 ? std::nullopt : std::optional<uint32_t>(u32(name));
    }

    uint64_t u64(const char *name) const
    {
        uint64_t value = 0;
        if (hlog_query_arg_u64(request_, name, &value) != HLOG_OK) {
            throw std::runtime_error(last_error_text());
        }
        return value;
    }

    std::optional<uint64_t> optional_u64(const char *name) const
    {
        return count(name) == 0 ? std::nullopt : std::optional<uint64_t>(u64(name));
    }

    int64_t i64(const char *name) const
    {
        int64_t value = 0;
        if (hlog_query_arg_i64(request_, name, &value) != HLOG_OK) {
            throw std::runtime_error(last_error_text());
        }
        return value;
    }

    std::optional<int64_t> optional_i64(const char *name) const
    {
        return count(name) == 0 ? std::nullopt : std::optional<int64_t>(i64(name));
    }

    bool boolean(const char *name) const
    {
        bool value = false;
        if (hlog_query_arg_bool(request_, name, &value) != HLOG_OK) {
            throw std::runtime_error(last_error_text());
        }
        return value;
    }

    std::optional<bool> optional_bool(const char *name) const
    {
        return count(name) == 0 ? std::nullopt : std::optional<bool>(boolean(name));
    }

private:
    static const char *last_error_text() noexcept
    {
        const char *message = hlog_last_error();
        return message ? message : "unknown hlog query error";
    }

    const HLogQueryRequest *request_;
};

class QueryOutput {
public:
    QueryOutput(HLogQueryWriteFn write, void *opaque) : write_(write), opaque_(opaque) {}

    void write(std::string_view text)
    {
        int32_t rc = write_(opaque_, reinterpret_cast<const uint8_t *>(text.data()), text.size());
        if (rc != HLOG_OK) {
            throw std::runtime_error("hlog query output writer failed");
        }
    }

    template <typename... Args>
    void print(fmt::format_string<Args...> format, const Args &...args)
    {
        fmt::memory_buffer buffer;
        fmt::vformat_to(std::back_inserter(buffer), format, fmt::make_format_args(args...));
        write(std::string_view(buffer.data(), buffer.size()));
    }

private:
    HLogQueryWriteFn write_;
    void *opaque_;
};

namespace detail {

using QueryHandler = std::function<void(const QueryArgs &, QueryOutput &)>;

struct QueryContext {
    QueryHandler handler;
};

inline int32_t set_query_error(int32_t code, const char *message) noexcept
{
    return hlog_query_set_error(code, message ? message : "unknown C++ query error");
}

inline int32_t query_trampoline(void *opaque, const HLogQueryRequest *request, HLogQueryWriteFn write,
                                void *write_opaque) noexcept
{
    try {
        auto *context = static_cast<QueryContext *>(opaque);
        if (context == nullptr || request == nullptr || write == nullptr) {
            return set_query_error(HLOG_ERR_INTERNAL, "hlog supplied an invalid C++ query callback context");
        }
        QueryArgs args(request);
        QueryOutput output(write, write_opaque);
        context->handler(args, output);
        return HLOG_OK;
    } catch (const std::exception &error) {
        return set_query_error(HLOG_ERR_INVALID_ARGUMENT, error.what());
    } catch (...) {
        return set_query_error(HLOG_ERR_INTERNAL, "C++ query callback threw an unknown exception");
    }
}

} // namespace detail

class QueryRegistration {
public:
    QueryRegistration() = default;
    QueryRegistration(const QueryRegistration &) = delete;
    QueryRegistration &operator=(const QueryRegistration &) = delete;

    QueryRegistration(QueryRegistration &&other) noexcept : handle_(other.handle_), context_(other.context_)
    {
        other.handle_ = HLOG_QUERY_INVALID_HANDLE;
        other.context_ = nullptr;
    }

    QueryRegistration &operator=(QueryRegistration &&other) noexcept
    {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            context_ = other.context_;
            other.handle_ = HLOG_QUERY_INVALID_HANDLE;
            other.context_ = nullptr;
        }
        return *this;
    }

    ~QueryRegistration()
    {
        reset();
    }

    explicit operator bool() const noexcept
    {
        return handle_ != HLOG_QUERY_INVALID_HANDLE;
    }

    HLogQueryHandle handle() const noexcept
    {
        return handle_;
    }

    void reset() noexcept
    {
        if (handle_ == HLOG_QUERY_INVALID_HANDLE) {
            return;
        }
        if (hlog_query_unregister(handle_) == HLOG_OK) {
            delete context_;
        }
        handle_ = HLOG_QUERY_INVALID_HANDLE;
        context_ = nullptr;
    }

private:
    QueryRegistration(HLogQueryHandle handle, detail::QueryContext *context) : handle_(handle), context_(context) {}

    HLogQueryHandle handle_{HLOG_QUERY_INVALID_HANDLE};
    detail::QueryContext *context_{nullptr};

    template <typename Handler>
    friend QueryRegistration register_query(const char *, const char *, const char *, const char *,
                                            std::initializer_list<QueryParam>, Handler &&);
};

template <typename Handler>
QueryRegistration register_query(const char *module, const char *instance, const char *name, const char *description,
                                 std::initializer_list<QueryParam> params, Handler &&handler)
{
    std::vector<HLogQueryParamSpec> raw_params;
    raw_params.reserve(params.size());
    for (const QueryParam &param : params) {
        raw_params.push_back(param.raw);
    }

    auto *context = new detail::QueryContext{detail::QueryHandler(std::forward<Handler>(handler))};
    HLogQueryHandle handle = hlog_query_register(
        module, instance, name, description, raw_params.empty() ? nullptr : raw_params.data(), raw_params.size(),
        detail::query_trampoline, context);
    if (handle == HLOG_QUERY_INVALID_HANDLE) {
        delete context;
        return {};
    }
    return QueryRegistration(handle, context);
}

inline const char *last_error() noexcept
{
    const char *message = hlog_last_error();
    return message ? message : "unknown hlog error";
}

inline bool load_level_config(const char *path) noexcept
{
    return path != nullptr && hlog_load_level_config(path) == HLOG_OK;
}

} // namespace hlog

#define HLOG_CONSOLE_IMPL(logger_expr, level_value, format_string, ...)          \
    do {                                                                      \
        auto &&hlog_logger_value = (logger_expr);                               \
        HLogLevel hlog_level_value = (level_value);                             \
        if (hlog_logger_value.valid() && hlog_level_value != HLOG_LEVEL_OFF) {   \
            hlog_logger_value.console_write(hlog_level_value, __func__,         \
                __FILE__, static_cast<uint32_t>(__LINE__),                      \
                FMT_STRING(format_string), ##__VA_ARGS__);                      \
        }                                                                     \
    } while (0)

#define HLOG_CONSOLE_HEXDUMP(logger_expr, level_value, data_value, data_length)  \
    do {                                                                      \
        auto &&hlog_logger_value = (logger_expr);                               \
        HLogLevel hlog_level_value = (level_value);                             \
        if (hlog_logger_value.valid() && hlog_level_value != HLOG_LEVEL_OFF) {   \
            hlog_logger_value.console_hexdump(hlog_level_value, __func__,       \
                __FILE__, static_cast<uint32_t>(__LINE__),                      \
                (data_value), static_cast<size_t>(data_length));                \
        }                                                                     \
    } while (0)

#define HLOG_IMPL(logger_expr, level_value, format_string, ...)                                         \
    do {                                                                                               \
        auto &&hlog_logger_value = (logger_expr);                                                       \
        HLogLevel hlog_level_value = (level_value);                                                     \
        if (hlog_logger_value.enabled(hlog_level_value)) {                                             \
            hlog_logger_value.write_unchecked(hlog_level_value, __func__, __FILE__,                    \
                                               static_cast<uint32_t>(__LINE__), FMT_STRING(format_string), \
                                               ##__VA_ARGS__);                                         \
        }                                                                                              \
    } while (0)

#define HLOG_CONSOLE_TRACE(logger, format_string, ...) \
    HLOG_CONSOLE_IMPL((logger), HLOG_LEVEL_TRACE, format_string, ##__VA_ARGS__)
#define HLOG_CONSOLE_DEBUG(logger, format_string, ...) \
    HLOG_CONSOLE_IMPL((logger), HLOG_LEVEL_DEBUG, format_string, ##__VA_ARGS__)
#define HLOG_CONSOLE_INFO(logger, format_string, ...) \
    HLOG_CONSOLE_IMPL((logger), HLOG_LEVEL_INFO, format_string, ##__VA_ARGS__)
#define HLOG_CONSOLE_WARN(logger, format_string, ...) \
    HLOG_CONSOLE_IMPL((logger), HLOG_LEVEL_WARN, format_string, ##__VA_ARGS__)
#define HLOG_CONSOLE_ERROR(logger, format_string, ...) \
    HLOG_CONSOLE_IMPL((logger), HLOG_LEVEL_ERROR, format_string, ##__VA_ARGS__)
#define HLOG_CONSOLE_CRITICAL(logger, format_string, ...) \
    HLOG_CONSOLE_IMPL((logger), HLOG_LEVEL_CRITICAL, format_string, ##__VA_ARGS__)

#define HLOG_TRACE(logger, format_string, ...) HLOG_IMPL((logger), HLOG_LEVEL_TRACE, format_string, ##__VA_ARGS__)
#define HLOG_DEBUG(logger, format_string, ...) HLOG_IMPL((logger), HLOG_LEVEL_DEBUG, format_string, ##__VA_ARGS__)
#define HLOG_INFO(logger, format_string, ...) HLOG_IMPL((logger), HLOG_LEVEL_INFO, format_string, ##__VA_ARGS__)
#define HLOG_WARN(logger, format_string, ...) HLOG_IMPL((logger), HLOG_LEVEL_WARN, format_string, ##__VA_ARGS__)
#define HLOG_ERROR(logger, format_string, ...) HLOG_IMPL((logger), HLOG_LEVEL_ERROR, format_string, ##__VA_ARGS__)
#define HLOG_CRITICAL(logger, format_string, ...)                                                           \
    HLOG_IMPL((logger), HLOG_LEVEL_CRITICAL, format_string, ##__VA_ARGS__)

#define HLOG_HEXDUMP(logger_expr, level_value, data_value, data_length)                              \
    do {                                                                                            \
        auto &&hlog_logger_value = (logger_expr);                                                   \
        HLogLevel hlog_level_value = (level_value);                                                 \
        if (hlog_logger_value.enabled(hlog_level_value)) {                                         \
            hlog_logger_value.hexdump_unchecked(hlog_level_value, __func__, __FILE__,               \
                                                 static_cast<uint32_t>(__LINE__), (data_value),     \
                                                 static_cast<size_t>(data_length));                 \
        }                                                                                          \
    } while (0)

// (format, buffer), (logger, format, buffer), or (logger, format, pointer, bytes).
// The logger expression is evaluated once; format/buffer are evaluated only
// after the level check. Both hlog::Logger and native C handles are accepted.
#define HLOG_DETAIL_DUMP_GUARD(level_value, console_only, logger_expr, ...) \
    do { \
        auto &&hlog_dump_logger_ref = (logger_expr); \
        const HLogLogger *hlog_dump_logger = ::hlog::detail::dump_handle(hlog_dump_logger_ref); \
        const HLogLevel hlog_dump_level = (level_value); \
        if ((console_only) ? (hlog_dump_logger && hlog_dump_level != HLOG_LEVEL_OFF) \
                           : hlog_enabled(hlog_dump_logger, static_cast<uint32_t>(hlog_dump_level))) { \
            __VA_ARGS__; \
        } \
    } while (0)
#define HLOG_DETAIL_DUMP_BUFFER(level, console_only, logger, format, buffer) \
    HLOG_DETAIL_DUMP_GUARD(level, console_only, logger, \
        ::hlog::detail::dump_buffer(hlog_dump_logger, hlog_dump_level, static_cast<::hlog::HexdumpFormat>(format), \
            __func__, __FILE__, static_cast<uint32_t>(__LINE__), (buffer), console_only))
#define HLOG_DETAIL_DUMP_POINTER(level, console_only, logger, format, data, length) \
    HLOG_DETAIL_DUMP_GUARD(level, console_only, logger, \
        ::hlog::detail::dump_bytes(hlog_dump_logger, hlog_dump_level, static_cast<::hlog::HexdumpFormat>(format), \
            __func__, __FILE__, static_cast<uint32_t>(__LINE__), (data), static_cast<size_t>(length), console_only))
#define HLOG_DETAIL_DUMP_DEFAULT(level, console_only, format, buffer) \
    HLOG_DETAIL_DUMP_BUFFER(level, console_only, ::hlog::Logger::from_handle(hlog_default_logger()), format, buffer)
#define HLOG_DETAIL_DUMP_SELECT(_1, _2, _3, _4, NAME, ...) NAME
#define HLOG_DETAIL_DUMP_LEVEL(level, console_only, ...) \
    HLOG_DETAIL_DUMP_SELECT(__VA_ARGS__, HLOG_DETAIL_DUMP_POINTER, HLOG_DETAIL_DUMP_BUFFER, HLOG_DETAIL_DUMP_DEFAULT, unused)(level, console_only, __VA_ARGS__)

#define HLOG_HEXDUMP_TRACE(...) HLOG_DETAIL_DUMP_LEVEL(HLOG_LEVEL_TRACE, false, __VA_ARGS__)
#define HLOG_HEXDUMP_DEBUG(...) HLOG_DETAIL_DUMP_LEVEL(HLOG_LEVEL_DEBUG, false, __VA_ARGS__)
#define HLOG_HEXDUMP_INFO(...) HLOG_DETAIL_DUMP_LEVEL(HLOG_LEVEL_INFO, false, __VA_ARGS__)
#define HLOG_HEXDUMP_WARN(...) HLOG_DETAIL_DUMP_LEVEL(HLOG_LEVEL_WARN, false, __VA_ARGS__)
#define HLOG_HEXDUMP_ERROR(...) HLOG_DETAIL_DUMP_LEVEL(HLOG_LEVEL_ERROR, false, __VA_ARGS__)
#define HLOG_HEXDUMP_CRITICAL(...) HLOG_DETAIL_DUMP_LEVEL(HLOG_LEVEL_CRITICAL, false, __VA_ARGS__)
#define HLOG_CONSOLE_HEXDUMP_TRACE(...) HLOG_DETAIL_DUMP_LEVEL(HLOG_LEVEL_TRACE, true, __VA_ARGS__)
#define HLOG_CONSOLE_HEXDUMP_DEBUG(...) HLOG_DETAIL_DUMP_LEVEL(HLOG_LEVEL_DEBUG, true, __VA_ARGS__)
#define HLOG_CONSOLE_HEXDUMP_INFO(...) HLOG_DETAIL_DUMP_LEVEL(HLOG_LEVEL_INFO, true, __VA_ARGS__)
#define HLOG_CONSOLE_HEXDUMP_WARN(...) HLOG_DETAIL_DUMP_LEVEL(HLOG_LEVEL_WARN, true, __VA_ARGS__)
#define HLOG_CONSOLE_HEXDUMP_ERROR(...) HLOG_DETAIL_DUMP_LEVEL(HLOG_LEVEL_ERROR, true, __VA_ARGS__)
#define HLOG_CONSOLE_HEXDUMP_CRITICAL(...) HLOG_DETAIL_DUMP_LEVEL(HLOG_LEVEL_CRITICAL, true, __VA_ARGS__)

#endif
