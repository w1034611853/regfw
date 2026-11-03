#include "layout_separate.hpp"
namespace separate {
void other(reg::RegisterBank<Root>& b) {
    b->regs.TABLE.at(2).ref() = 73;
}
} // namespace separate
