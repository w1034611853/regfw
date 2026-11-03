#ifndef TEST_QEMU_OSDEP_H
#define TEST_QEMU_OSDEP_H

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/queue.h>

#define QTAILQ_HEAD TAILQ_HEAD
#define QTAILQ_ENTRY TAILQ_ENTRY
#define QTAILQ_HEAD_INITIALIZER TAILQ_HEAD_INITIALIZER
#define QTAILQ_FOREACH TAILQ_FOREACH
#define QTAILQ_INSERT_TAIL TAILQ_INSERT_TAIL
#define QTAILQ_EMPTY TAILQ_EMPTY

#define G_GNUC_PRINTF(a, b) __attribute__((format(printf, a, b)))
#define g_ascii_strcasecmp strcasecmp
#define g_ascii_isspace isspace
#define g_malloc malloc
#define g_free free
#define g_strdup strdup
#define g_assert_not_reached() abort()

static inline void *g_realloc_n(void *pointer, size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) {
        abort();
    }
    return realloc(pointer, count * size);
}

static inline void test_g_freep(void *pointer)
{
    void **value = pointer;

    free(*value);
}

#define g_autofree __attribute__((cleanup(test_g_freep)))
#define g_autoptr(Type) __attribute__((cleanup(test_autoptr_cleanup_##Type))) Type *

#define g_steal_pointer(pointer)                                                                    \
    ({                                                                                              \
        __typeof__(*(pointer)) test_stolen = *(pointer);                                            \
        *(pointer) = NULL;                                                                          \
        test_stolen;                                                                                \
    })

#endif
