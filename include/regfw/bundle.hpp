#pragma once
// Declaration-only public entry: entity/register declarations, handles,
// callbacks and RegisterBank, without the constexpr layout compiler.
// Device headers include this together with REG_LAYOUT_DECL(); the .cpp that
// defines REG_LAYOUT_IMPL or REG_STATIC_LAYOUT includes <regfw/regfw.hpp>.
// REG_LAYOUT with a compiled factory requires the full entry and will not
// compile from this header alone.
#include "diagnostics.hpp"
#include "bank.hpp"
#include "detail/declarations_base.hpp"
#include "address_view.hpp"
