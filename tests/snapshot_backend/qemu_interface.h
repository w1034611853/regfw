#pragma once
// Test backend only. Production uses the device project's qemu_interface.h.
#include <cstdint>
extern "C" void module_var_save(void*, const std::uint8_t*, std::uint32_t);
extern "C" void module_var_load(void*, std::uint8_t*, std::uint32_t);
