#pragma once
// Private implementation aggregation point. Not installed as a public header.
// The subheaders mirror the three layers of the runtime state:
//   EntityState -> bound AccessContext -> shared FormulaDecoder.
#include "formula_decoder.hpp"
