enum { counter_a = __COUNTER__, counter_b = __COUNTER__, counter_c = __COUNTER__ };
#include "symbolic_inventory.hpp"
std::uint32_t other_translation_unit(InventoryBank& b) {
    return b.map().mmio_read(0x1200);
}
