#ifndef TEST_QLIST_H
#define TEST_QLIST_H

#include "qemu/osdep.h"
#include "qobject/qobject.h"

typedef struct QListEntry {
    QObject *value;
    struct QListEntry *next;
} QListEntry;

typedef struct QList {
    QObject base;
    QListEntry *head;
    QListEntry *tail;
    size_t length;
} QList;

QList *qlist_new(void);
void qlist_destroy(QList *list);
void qlist_append(QList *list, void *value);
size_t qlist_size(const QList *list);
QObject *qlist_get(const QList *list, size_t index);
static inline const QListEntry *qlist_first(const QList *list) { return list->head; }
static inline const QListEntry *qlist_next(const QListEntry *entry) { return entry->next; }
static inline QObject *qlist_entry_obj(const QListEntry *entry) { return entry->value; }

static inline void test_autoptr_cleanup_QList(QList **list)
{
    qlist_destroy(*list);
}

#endif
