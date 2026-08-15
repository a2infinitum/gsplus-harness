/**********************************************************************/
/*                    GSplus - Apple //gs Emulator                    */
/*                    Based on KEGS by Kent Dickey                    */
/*                    Copyright 2002-2023 Kent Dickey                 */
/*                    Copyright 2025-2026 GSplus Contributors         */
/*                                                                    */
/*      This code is covered by the GNU GPL v3                        */
/*      See the file COPYING.txt or https://www.gnu.org/licenses/     */
/**********************************************************************/

/* Headless test-harness instrumentation.  Activated by any -h* flag; see
 * harness.c for the flag list.  While active the emulator runs unattended:
 * a CPU halt (breakpoint, BRK trap, halt_printf) or a stalled screen dumps
 * machine state plus a screenshot into the artifact directory and quits
 * with a distinct exit code instead of dropping into the interactive
 * debugger. */

#ifndef HARNESS_H
#define HARNESS_H

int harness_parse_argv(int argc, char **argv, int *i_ptr);
void harness_usage(void);
int harness_tick(void);
int harness_exit_code(void);

/* Set by -hbrk: halt on the BRK opcode in both native and emulation mode
 * (checked from the engine's instruction dispatch). */
extern int g_harness_trap_brk;

/* Set by -hturbo: skip the per-VBL pacing sleep so emulated time runs as
 * fast as the host allows (audio will starve; use with -audio 0). */
extern int g_harness_turbo;

#endif /* HARNESS_H */
