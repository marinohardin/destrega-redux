/* Hand-written replacements for recompiled guest functions.
 *
 * A function listed in analysis/overrides.txt is still generated, but under the
 * name f_<addr>_orig; f_<addr> is then defined here, by readable C that follows
 * the same register-level calling convention (args in R4-R7, result in R2).
 *
 * With --verify-overrides every call runs BOTH versions on the same inputs and
 * reports any difference in the result, the callee-saved registers, or the
 * guest memory the function is documented to write.
 */
#ifndef OVERRIDE_H
#define OVERRIDE_H

#include "recomp.h"

extern int g_verify_overrides;

/* Call a recompiled guest function with up to four register arguments. */
u32 guest_call(RecompFn fn, u32 a0, u32 a1, u32 a2, u32 a3);

/* Run `fresh` (or, with --verify-overrides, compare it against `orig`: result
 * register, callee-saved registers, and all of guest RAM/scratchpad outside the
 * dead stack below sp). */
void override_call(const char *name, RecompFn orig, RecompFn fresh);

void override_report(void);

#endif
