#ifndef TEST_QDICT_H
#define TEST_QDICT_H

#include "qemu/osdep.h"
#include "qobject/qobject.h"
#include "qobject/qlist.h"

typedef enum TestQValueType {
    TEST_QVALUE_STRING,
    TEST_QVALUE_BOOL,
    TEST_QVALUE_OBJECT,
} TestQValueType;

typedef struct QDictEntry {
    char *key;
    TestQValueType value_type;
    union {
        char *string;
        bool boolean;
        QObject *object;
    } value;
    struct QDictEntry *next;
} QDictEntry;

typedef struct QDict {
    QObject base;
    QDictEntry *head;
    QDictEntry *tail;
} QDict;

QDict *qdict_new(void);
void qdict_destroy(QDict *dict);
void qdict_put_bool(QDict *dict, const char *key, bool value);
void qdict_put_str(QDict *dict, const char *key, const char *value);
void qdict_put(QDict *dict, const char *key, void *value);
const char *qdict_get_try_str(const QDict *dict, const char *key);
bool qdict_get_bool(const QDict *dict, const char *key);
QObject *qdict_get(const QDict *dict, const char *key);
const QDictEntry *qdict_first(const QDict *dict);
const QDictEntry *qdict_next(const QDict *dict, const QDictEntry *entry);
const char *qdict_entry_key(const QDictEntry *entry);

#define qobject_to(type, obj) ((type *)(obj))
static inline const char *qdict_get_str(const QDict *dict, const char *key)
{
    const char *value = qdict_get_try_str(dict, key);
    assert(value);
    return value;
}
static inline QDict *qdict_get_qdict(const QDict *dict, const char *key)
{
    return (QDict *)qdict_get(dict, key);
}
static inline QList *qdict_get_qlist(const QDict *dict, const char *key)
{
    return (QList *)qdict_get(dict, key);
}

static inline bool qdict_haskey(const QDict *dict, const char *key)
{
    const QDictEntry *entry;
    for (entry = qdict_first(dict); entry; entry = qdict_next(dict, entry)) {
        if (!strcmp(qdict_entry_key(entry), key)) {
            return true;
        }
    }
    return false;
}

static inline void test_autoptr_cleanup_QDict(QDict **dict)
{
    qdict_destroy(*dict);
}

#endif
