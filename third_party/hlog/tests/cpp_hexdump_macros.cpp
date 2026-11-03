// ABI boundary tests; real-runtime rendering is tested by test_hexdump_runtime.py.
#include "hlog.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#if __cplusplus >= 202002L
#include <span>
#endif

struct HLogLogger { uint32_t level; };
static HLogLogger native_logger{HLOG_LEVEL_TRACE}, default_logger{HLOG_LEVEL_TRACE};
struct Call { const HLogLogger *logger; uint32_t level, format; const uint8_t *data; size_t bytes; bool console; };
static std::vector<Call> calls;
static unsigned logger_reads, buffer_reads, format_reads, length_reads, filter_reads;
extern "C" HLogLogger *hlog_default_logger() { return &default_logger; }
extern "C" bool hlog_enabled(const HLogLogger *logger, uint32_t level) {
    ++filter_reads;return logger && level < HLOG_LEVEL_OFF && level >= logger->level;
}
static int32_t capture(const HLogLogger *logger, uint32_t level,
    const uint8_t *function, size_t function_len, const uint8_t *file,
    size_t file_len, uint32_t line, const uint8_t *data, size_t bytes, uint32_t format, bool console)
{
    assert(logger && function && function_len && file && file_len && line);
    calls.push_back({logger,level,format,data,bytes,console});return HLOG_OK;
}
extern "C" int32_t hlog_hexdump_ex(const HLogLogger *logger, uint32_t level,
    const uint8_t *function, size_t function_len, const uint8_t *file,
    size_t file_len, uint32_t line, const uint8_t *data, size_t bytes, uint32_t format)
{
    return capture(logger,level,function,function_len,file,file_len,line,data,bytes,format,false);
}
extern "C" int32_t hlog_console_hexdump_ex(const HLogLogger *logger, uint32_t level,
    const uint8_t *function, size_t function_len, const uint8_t *file,
    size_t file_len, uint32_t line, const uint8_t *data, size_t bytes, uint32_t format)
{
    return capture(logger,level,function,function_len,file,file_len,line,data,bytes,format,true);
}
extern "C" int32_t hlog_hexdump(const HLogLogger *logger, uint32_t level,
    const uint8_t *function, size_t function_len, const uint8_t *file,
    size_t file_len, uint32_t line, const uint8_t *data, size_t bytes)
{
    return capture(logger,level,function,function_len,file,file_len,line,data,bytes,1,false);
}
extern "C" int32_t hlog_console_hexdump(const HLogLogger *logger, uint32_t level,
    const uint8_t *function, size_t function_len, const uint8_t *file,
    size_t file_len, uint32_t line, const uint8_t *data, size_t bytes)
{
    return capture(logger,level,function,function_len,file,file_len,line,data,bytes,1,true);
}
static hlog::Logger logger_expr() { ++logger_reads;return hlog::Logger::from_handle(&native_logger); }
static const std::array<uint32_t,3> &buffer_expr() {
    static const std::array<uint32_t,3> data{{0x12345678,0xabcdef01,0x89abcdef}};
    ++buffer_reads;return data;
}
static hlog::HexdumpFormat format_expr() { ++format_reads;return hlog::u32; }
static size_t length_expr() { ++length_reads;return 5; }
int main()
{
    auto logger=hlog::Logger::from_handle(&native_logger);
    const uint8_t bytes[19]={1,2,3};
    const std::array<uint16_t,5> halfwords{{1,2,3,4,5}};
    const std::vector<uint64_t> words{1,2,3};
    const std::array<std::byte,7> raw{};
    HLOG_HEXDUMP_DEBUG(logger_expr(),format_expr(),buffer_expr());
    assert(logger_reads==1 && buffer_reads==1 && format_reads==1);
    assert(calls.back().format==4 && calls.back().bytes==12 && calls.back().data==reinterpret_cast<const uint8_t *>(buffer_expr().data()));
    HLOG_HEXDUMP_TRACE(logger,hlog::u8,bytes);
    assert(calls.back().bytes==sizeof(bytes) && calls.back().level==0);
    HLOG_HEXDUMP_INFO(logger,hlog::u16,halfwords);
    assert(calls.back().bytes==10 && calls.back().format==2 && calls.back().level==2);
    HLOG_HEXDUMP_WARN(logger,hlog::u64,words);
    assert(calls.back().bytes==24 && calls.back().format==8 && calls.back().level==3);
    HLOG_HEXDUMP_ERROR(logger,hlog::u32,raw);
    assert(calls.back().bytes==7 && calls.back().level==4);
    HLOG_HEXDUMP_CRITICAL(logger,hlog::u64,bytes+1,length_expr());
    assert(calls.back().bytes==5 && calls.back().data==bytes+1 && length_reads==1 && calls.back().level==5);
    HLOG_HEXDUMP_DEBUG(&native_logger,HLOG_DUMP_U32,bytes);
    assert(calls.back().logger==&native_logger && calls.back().format==4);
    HLOG_HEXDUMP_DEBUG(hlog::u16,bytes);
    assert(calls.back().logger==&default_logger && calls.back().format==2);
    HLOG_HEXDUMP(logger,HLOG_LEVEL_DEBUG,bytes,sizeof(bytes));
    assert(calls.back().format==1 && calls.back().bytes==sizeof(bytes));

#if __cplusplus >= 202002L
    HLOG_HEXDUMP_DEBUG(logger,hlog::u16,std::span(bytes).subspan(1,17));
    assert(calls.back().data==bytes+1 && calls.back().bytes==17);
    HLOG_HEXDUMP_DEBUG(logger,hlog::u64,std::span(words));
    assert(calls.back().bytes==24);
    HLOG_HEXDUMP_DEBUG(logger,hlog::u32,std::span<const uint8_t>{});
    assert(calls.back().bytes==0);
#endif
    const auto before=calls.size();
    logger_reads=buffer_reads=format_reads=length_reads=0;
    native_logger.level=HLOG_LEVEL_OFF;
    HLOG_HEXDUMP_DEBUG(logger_expr(),format_expr(),buffer_expr());
    HLOG_HEXDUMP_DEBUG(logger,format_expr(),bytes+1,length_expr());
    assert(logger_reads==1 && buffer_reads==0 && format_reads==0 && length_reads==0 && calls.size()==before);
    default_logger.level=HLOG_LEVEL_OFF;
    HLOG_HEXDUMP_DEBUG(format_expr(),buffer_expr());
    HLOG_HEXDUMP_DEBUG(nullptr,format_expr(),buffer_expr());
    assert(buffer_reads==0 && format_reads==0 && calls.size()==before);
    const auto filters=filter_reads;
    HLOG_CONSOLE_HEXDUMP_DEBUG(logger_expr(),format_expr(),buffer_expr());
    assert(filter_reads==filters && calls.size()==before+1 && calls.back().console);
    assert(logger_reads==2 && buffer_reads==1 && format_reads==1);
    HLOG_CONSOLE_HEXDUMP_INFO(hlog::u8,bytes);
    assert(calls.back().logger==&default_logger && calls.back().console);
    HLOG_CONSOLE_HEXDUMP_TRACE(logger,hlog::u8,bytes);
    HLOG_CONSOLE_HEXDUMP_WARN(logger,hlog::u16,bytes);
    HLOG_CONSOLE_HEXDUMP_ERROR(logger,hlog::u32,bytes);
    HLOG_CONSOLE_HEXDUMP_CRITICAL(logger,hlog::u64,bytes);
    assert(calls.back().level==5 && filter_reads==filters);
    std::puts("C++ hexdump macros: C arrays/array/vector/span, native/default loggers, byte sizes, levels and lazy arguments passed");
}
