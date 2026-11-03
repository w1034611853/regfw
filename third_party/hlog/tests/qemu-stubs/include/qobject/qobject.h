#ifndef TEST_QOBJECT_H
#define TEST_QOBJECT_H

typedef enum QObjectType {
    TEST_QTYPE_QDICT,
    TEST_QTYPE_QLIST,
} QObjectType;

typedef struct QObject {
    QObjectType type;
} QObject;

#define QOBJECT(object) ((QObject *)(object))

void qobject_unref(QObject *object);

#endif
