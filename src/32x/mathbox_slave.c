// The math box's drawing code again, for the slave SH-2: a second copy has
// its own state, so the master can draw a command itself when the slave is
// behind. Each SH-2 has its own cache, so they must not share that state.
//
// Copyright 2026 the irobot 32X port authors. GPL-3.0-or-later, see LICENSE.

#define ir_mathbox_start ir_mathbox_start_slave
#define ir_mathbox_draws ir_mathbox_draws_slave
#define ir_mathbox_draw  ir_mathbox_draw_slave
#include "../common/mathbox.c"
