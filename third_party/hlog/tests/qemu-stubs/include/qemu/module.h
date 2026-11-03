#ifndef TEST_QEMU_MODULE_H
#define TEST_QEMU_MODULE_H

#define opts_init(function)                                                      \
    static void __attribute__((constructor)) test_opts_init_##function(void)     \
    {                                                                            \
        function();                                                              \
    }

#endif
