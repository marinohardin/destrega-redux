/* Native high-level renderer (docs/HLR_PLAN.md). Phase 0: read-only probes. */
#pragma once
#include "psx.h"

extern int g_hlr_audit;                 /* --hlr audit: stage packet audit (hlr_audit.c) */
void hlr_audit_pre(u32 mode);           /* before the real pass of stage_render; mode = its a0 */
void hlr_audit_post(void);              /* after it */
void hlr_audit_report(void);            /* exit summary line "hlr-audit: ..." */
