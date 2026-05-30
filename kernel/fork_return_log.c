#include "serial.h"
/* Called from fork_return_asm on each forked child's first entry to ring 3.
 * Kept as a (now silent) hook so the assembly call site stays valid; enable the
 * trace below when debugging the fork return path. */
void fork_return_log(void) {
#ifdef LARIAT_TRACE_FORK
    serial_print(SERIAL_COM1, "[FORK_RETURN] child entering userspace via iretq\n");
#endif
}
