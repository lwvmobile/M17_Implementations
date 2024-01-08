//--------------------------------------------------------------------
// M17 C library - encode/symbols.c
//
// Wojciech Kaczmarski, SP5WWP
// M17 Project, 5 January 2024
//--------------------------------------------------------------------
#include "symbols.h"

//dibits-symbols map (TX)
const int8_t symbol_map[4]={+1, +3, -1, -3};

//End of Transmission symbol pattern
const float eot_symbols[8]={+3, +3, +3, +3, +3, +3, -3, +3};