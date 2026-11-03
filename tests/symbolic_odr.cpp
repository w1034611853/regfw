#include "symbolic_inventory.hpp"
int main() {
    InventoryBank b(reg::space(0x2000));
    b.finish();
    b.map().mmio_write(0x1200, 42);
    return other_translation_unit(b) != 42 || b->words.R128.ref() != 42;
}
