#ifndef TEST_QEMU_NOTIFY_H
#define TEST_QEMU_NOTIFY_H

typedef struct Notifier {
    void (*notify)(struct Notifier *notifier, void *data);
    struct Notifier *next;
} Notifier;

#endif
