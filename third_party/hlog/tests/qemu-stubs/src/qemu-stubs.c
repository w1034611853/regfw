#include "qemu/osdep.h"

#include <dlfcn.h>

#include "gmodule.h"
#include "monitor/monitor-internal.h"
#include "qapi/error.h"
#include "qemu/config-file.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "system/system.h"

struct GModule {
    void *handle;
    bool resident;
};

static const char *test_gmodule_error;
QmpCommandList qmp_commands;
static Notifier *test_machine_init_done_notifiers;
static QemuOptsList *test_opts_lists[16];
static size_t test_opts_list_count;

void qemu_add_opts(QemuOptsList *list)
{
    assert(test_opts_list_count < sizeof(test_opts_lists) / sizeof(test_opts_lists[0]));
    test_opts_lists[test_opts_list_count++] = list;
}

static QemuOptsList *test_find_opts_list(const char *name)
{
    size_t i;

    for (i = 0; i < test_opts_list_count; i++) {
        if (!strcmp(test_opts_lists[i]->name, name)) {
            return test_opts_lists[i];
        }
    }
    return NULL;
}

QemuOpts *qemu_opts_find(QemuOptsList *list, const char *id)
{
    QemuOpts *opts;

    QTAILQ_FOREACH(opts, &list->head, next) {
        if ((!id && !opts->id) || (id && opts->id && !strcmp(id, opts->id))) {
            return opts;
        }
    }
    return NULL;
}

const char *qemu_opts_id(QemuOpts *opts)
{
    return opts->id;
}

const char *qemu_opt_get(QemuOpts *opts, const char *name)
{
    QemuOptValue *value;

    for (value = opts->values; value; value = value->next) {
        if (!strcmp(value->name, name)) {
            return value->value;
        }
    }
    return NULL;
}

int qemu_opts_foreach(QemuOptsList *list, qemu_opts_loopfunc func, void *opaque, Error **errp)
{
    QemuOpts *opts;

    QTAILQ_FOREACH(opts, &list->head, next) {
        int rc = func(opaque, opts, errp);

        if (rc) {
            return rc;
        }
    }
    return 0;
}

static char *test_trim(char *text)
{
    char *end;

    while (isspace((unsigned char)*text)) {
        text++;
    }
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return text;
}

static bool test_option_known(QemuOptsList *list, const char *name)
{
    size_t i;

    for (i = 0; list->desc[i].name; i++) {
        if (!strcmp(list->desc[i].name, name)) {
            return true;
        }
    }
    return false;
}

static QemuOpts *test_create_opts(QemuOptsList *list, const char *id, unsigned int line, Error **errp)
{
    QemuOpts *opts = qemu_opts_find(list, id);

    if (opts) {
        if (list->merge_lists) {
            return opts;
        }
        error_setg(errp, "line %u: duplicate section [%s \"%s\"]", line, list->name, id ? id : "");
        return NULL;
    }

    opts = calloc(1, sizeof(*opts));
    opts->id = id ? strdup(id) : NULL;
    QTAILQ_INSERT_TAIL(&list->head, opts, next);
    return opts;
}

static bool test_parse_section(char *line, QemuOpts **current, unsigned int line_number, Error **errp)
{
    char *body;
    char *id = NULL;
    char *space;
    char *end;
    QemuOptsList *list;

    end = strrchr(line, ']');
    if (line[0] != '[' || !end || end[1] != '\0') {
        error_setg(errp, "line %u: malformed section", line_number);
        return false;
    }
    *end = '\0';
    body = test_trim(line + 1);
    space = strchr(body, ' ');
    if (space) {
        *space = '\0';
        id = test_trim(space + 1);
        if (id[0] != '"' || strlen(id) < 2 || id[strlen(id) - 1] != '"') {
            error_setg(errp, "line %u: malformed section id", line_number);
            return false;
        }
        id[strlen(id) - 1] = '\0';
        id++;
    }

    list = test_find_opts_list(body);
    if (!list) {
        error_setg(errp, "line %u: unknown option group '%s'", line_number, body);
        return false;
    }
    if (!space) {
        error_setg(errp, "line %u: [%s] requires an id", line_number, body);
        return false;
    }
    *current = test_create_opts(list, id, line_number, errp);
    return *current != NULL;
}

static bool test_parse_option(char *line, QemuOpts *current, unsigned int line_number, Error **errp)
{
    char *separator = strchr(line, '=');
    char *name;
    char *value;
    QemuOptValue *option;
    QemuOptsList *owner = NULL;
    size_t i;

    if (!current || !separator || strchr(separator + 1, '=')) {
        error_setg(errp, "line %u: expected name = \"value\"", line_number);
        return false;
    }
    *separator = '\0';
    name = test_trim(line);
    value = test_trim(separator + 1);
    if (value[0] != '"' || strlen(value) < 2 || value[strlen(value) - 1] != '"') {
        error_setg(errp, "line %u: option value must be quoted", line_number);
        return false;
    }
    value[strlen(value) - 1] = '\0';
    value++;

    for (i = 0; i < test_opts_list_count; i++) {
        QemuOpts *candidate;

        QTAILQ_FOREACH(candidate, &test_opts_lists[i]->head, next) {
            if (candidate == current) {
                owner = test_opts_lists[i];
            }
        }
    }
    if (!owner || !test_option_known(owner, name)) {
        error_setg(errp, "line %u: unknown option '%s'", line_number, name);
        return false;
    }
    if (qemu_opt_get(current, name)) {
        error_setg(errp, "line %u: duplicate option '%s'", line_number, name);
        return false;
    }

    option = calloc(1, sizeof(*option));
    option->name = strdup(name);
    option->value = strdup(value);
    option->next = current->values;
    current->values = option;
    return true;
}

bool test_qemu_read_config(const char *path, Error **errp)
{
    FILE *stream = fopen(path, "r");
    QemuOpts *current = NULL;
    char line[512];
    unsigned int line_number = 0;

    if (!stream) {
        error_setg_errno(errp, errno, "cannot read config '%s'", path);
        return false;
    }
    while (fgets(line, sizeof(line), stream)) {
        char *text;
        char *comment;

        line_number++;
        comment = strchr(line, '#');
        if (comment) {
            *comment = '\0';
        }
        text = test_trim(line);
        if (!*text) {
            continue;
        }
        if (text[0] == '[') {
            if (!test_parse_section(text, &current, line_number, errp)) {
                fclose(stream);
                return false;
            }
        } else if (!test_parse_option(text, current, line_number, errp)) {
            fclose(stream);
            return false;
        }
    }
    fclose(stream);
    return true;
}

bool g_module_supported(void)
{
    return true;
}

GModule *g_module_open(const char *path, int flags)
{
    GModule *module;

    (void)flags;
    dlerror();
    module = calloc(1, sizeof(*module));
    module->handle = dlopen(path, RTLD_LAZY | RTLD_GLOBAL);
    if (!module->handle) {
        test_gmodule_error = dlerror();
        free(module);
        return NULL;
    }
    return module;
}

bool g_module_close(GModule *module)
{
    int result = 0;

    if (!module) {
        return true;
    }
    if (!module->resident) {
        result = dlclose(module->handle);
    }
    free(module);
    return result == 0;
}

bool g_module_symbol(GModule *module, const char *symbol_name, gpointer *symbol)
{
    dlerror();
    *symbol = dlsym(module->handle, symbol_name);
    test_gmodule_error = dlerror();
    return test_gmodule_error == NULL;
}

const char *g_module_error(void)
{
    return test_gmodule_error ? test_gmodule_error : "unknown GModule error";
}

void g_module_make_resident(GModule *module)
{
    module->resident = true;
}

static QDictEntry *qdict_find(const QDict *dict, const char *key)
{
    QDictEntry *entry;

    for (entry = dict->head; entry; entry = entry->next) {
        if (!strcmp(entry->key, key)) {
            return entry;
        }
    }
    return NULL;
}

static QDictEntry *qdict_add(QDict *dict, const char *key, TestQValueType value_type)
{
    QDictEntry *entry = calloc(1, sizeof(*entry));

    entry->key = strdup(key);
    entry->value_type = value_type;
    if (dict->tail) {
        dict->tail->next = entry;
    } else {
        dict->head = entry;
    }
    dict->tail = entry;
    return entry;
}

QDict *qdict_new(void)
{
    QDict *dict = calloc(1, sizeof(*dict));

    dict->base.type = TEST_QTYPE_QDICT;
    return dict;
}

void qdict_destroy(QDict *dict)
{
    QDictEntry *entry;

    if (!dict) {
        return;
    }
    while ((entry = dict->head)) {
        dict->head = entry->next;
        free(entry->key);
        if (entry->value_type == TEST_QVALUE_STRING) {
            free(entry->value.string);
        } else if (entry->value_type == TEST_QVALUE_OBJECT) {
            qobject_unref(entry->value.object);
        }
        free(entry);
    }
    free(dict);
}

void qdict_put_bool(QDict *dict, const char *key, bool value)
{
    qdict_add(dict, key, TEST_QVALUE_BOOL)->value.boolean = value;
}

void qdict_put_str(QDict *dict, const char *key, const char *value)
{
    qdict_add(dict, key, TEST_QVALUE_STRING)->value.string = strdup(value);
}

void qdict_put(QDict *dict, const char *key, void *value)
{
    qdict_add(dict, key, TEST_QVALUE_OBJECT)->value.object = value;
}

const char *qdict_get_try_str(const QDict *dict, const char *key)
{
    QDictEntry *entry = qdict_find(dict, key);

    return entry && entry->value_type == TEST_QVALUE_STRING ? entry->value.string : NULL;
}

bool qdict_get_bool(const QDict *dict, const char *key)
{
    QDictEntry *entry = qdict_find(dict, key);

    return entry && entry->value_type == TEST_QVALUE_BOOL && entry->value.boolean;
}

QObject *qdict_get(const QDict *dict, const char *key)
{
    QDictEntry *entry = qdict_find(dict, key);

    return entry && entry->value_type == TEST_QVALUE_OBJECT ? entry->value.object : NULL;
}

const QDictEntry *qdict_first(const QDict *dict)
{
    return dict->head;
}

const QDictEntry *qdict_next(const QDict *dict, const QDictEntry *entry)
{
    (void)dict;
    return entry->next;
}

const char *qdict_entry_key(const QDictEntry *entry)
{
    return entry->key;
}

QList *qlist_new(void)
{
    QList *list = calloc(1, sizeof(*list));

    list->base.type = TEST_QTYPE_QLIST;
    return list;
}

void qlist_destroy(QList *list)
{
    QListEntry *entry;

    if (!list) {
        return;
    }
    while ((entry = list->head)) {
        list->head = entry->next;
        qobject_unref(entry->value);
        free(entry);
    }
    free(list);
}

void qlist_append(QList *list, void *value)
{
    QListEntry *entry = calloc(1, sizeof(*entry));

    entry->value = value;
    if (list->tail) {
        list->tail->next = entry;
    } else {
        list->head = entry;
    }
    list->tail = entry;
    list->length++;
}

size_t qlist_size(const QList *list)
{
    return list->length;
}

QObject *qlist_get(const QList *list, size_t index)
{
    QListEntry *entry = list->head;

    while (entry && index--) {
        entry = entry->next;
    }
    return entry ? entry->value : NULL;
}

void qobject_unref(QObject *object)
{
    if (!object) {
        return;
    }
    if (object->type == TEST_QTYPE_QDICT) {
        qdict_destroy((QDict *)object);
    } else if (object->type == TEST_QTYPE_QLIST) {
        qlist_destroy((QList *)object);
    }
}

void error_setg(Error **errp, const char *fmt, ...)
{
    va_list ap;

    if (!errp) {
        return;
    }
    *errp = calloc(1, sizeof(**errp));
    va_start(ap, fmt);
    if (vasprintf(&(*errp)->msg, fmt, ap) < 0) {
        (*errp)->msg = strdup("error formatting failed");
    }
    va_end(ap);
}

void error_free(Error *error)
{
    if (error) {
        free(error->msg);
        free(error);
    }
}

void error_setg_errno(Error **errp, int os_error, const char *fmt, ...)
{
    va_list ap;
    char *message = NULL;

    if (!errp) {
        return;
    }
    va_start(ap, fmt);
    if (vasprintf(&message, fmt, ap) < 0) {
        message = strdup("error formatting failed");
    }
    va_end(ap);
    error_setg(errp, "%s: %s", message, strerror(os_error));
    free(message);
}

void qemu_add_machine_init_done_notifier(Notifier *notifier)
{
    notifier->next = test_machine_init_done_notifiers;
    test_machine_init_done_notifiers = notifier;
}

void test_qemu_machine_init_done(void)
{
    Notifier *notifier;

    for (notifier = test_machine_init_done_notifiers; notifier; notifier = notifier->next) {
        notifier->notify(notifier, NULL);
    }
}

const QmpCommand *qmp_find_command(const QmpCommandList *commands, const char *name)
{
    QmpCommand *command;

    for (command = commands->head; command; command = command->next) {
        if (!strcmp(command->name, name)) {
            return command;
        }
    }
    return NULL;
}

void qmp_register_command(QmpCommandList *commands, const char *name, QmpCommandFunc handler, int options,
                          int features)
{
    QmpCommand *command = calloc(1, sizeof(*command));

    (void)options;
    (void)features;
    command->name = strdup(name);
    command->handler = handler;
    command->next = commands->head;
    commands->head = command;
}
