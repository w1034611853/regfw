/* Header/wrapper test: the ABI functions below are intentionally mocked.
 * Runtime routing and colors are tested separately by test_console_runtime.py.
 */
#include "hlog.h"
#include <assert.h>

static int calls;
static char last_message[4096];
static unsigned last_level;
static int logger_evaluations;
static HLogLogger *test_logger(void)
{
    logger_evaluations++;
    return (HLogLogger *)(uintptr_t)1;
}
bool hlog_enabled(const HLogLogger *logger, uint32_t level)
{
    (void)logger; (void)level;
    assert(!"console macros must not use the file level filter");
    return false;
}
int32_t hlog_console_write(const HLogLogger *logger, uint32_t level,
    const uint8_t *function, size_t function_len, const uint8_t *file,
    size_t file_len, uint32_t line, const uint8_t *message, size_t size)
{
    assert(logger && function && function_len && file && file_len && line);
    assert(size < sizeof(last_message));
    memcpy(last_message, message, size);
    last_message[size] = 0;
    calls++;
    last_level = level;
    return HLOG_OK;
}
int32_t hlog_console_hexdump(const HLogLogger *logger, uint32_t level,
    const uint8_t *function, size_t function_len, const uint8_t *file,
    size_t file_len, uint32_t line, const uint8_t *data, size_t size)
{
    return hlog_console_write(logger, level, function, function_len, file,
                               file_len, line, data, size);
}
int main(void)
{
    char long_message[2048];
    int side_effect = 0;
    memset(long_message, 'x', sizeof(long_message) - 1);
    long_message[sizeof(long_message) - 1] = 0;
    HLOG_CONSOLE_TRACE(test_logger(), "trace");
    assert(last_level == 0);
    HLOG_CONSOLE_DEBUG(test_logger(), "vf=%u", 3u);
    assert(last_level == 1 && !strcmp(last_message, "vf=3"));
    HLOG_CONSOLE_INFO(test_logger(), "%s", long_message);
    assert(last_level == 2 && !strcmp(last_message, long_message));
    HLOG_CONSOLE_WARN(test_logger(), "warn");
    assert(last_level == 3);
    HLOG_CONSOLE_ERROR(test_logger(), "error");
    assert(last_level == 4);
    HLOG_CONSOLE_CRITICAL(test_logger(), "critical");
    assert(last_level == 5);
    HLOG_CONSOLE_HEXDUMP(test_logger(), HLOG_LEVEL_DEBUG, "abc", 3);
    assert(!strcmp(last_message, "abc"));
    assert(calls == 7 && logger_evaluations == 7);
    HLOG_CONSOLE_IMPL(test_logger(), HLOG_LEVEL_OFF, "%d", ++side_effect);
    HLOG_CONSOLE_INFO(NULL, "%d", ++side_effect);
    assert(calls == 7 && side_effect == 0);
    puts("C console wrapper tests passed (mock ABI)");
    return 0;
}
