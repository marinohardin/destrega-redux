/* SIO0: controller / memory card serial port (register level).
 *
 * Destrega drives the pads through Psy-Q libpad, which talks to SIO0 directly
 * from a SysEnqIntRP VBlank handler and waits on the ACK interrupt (IRQ7).
 * Both ports report a digital pad (id 0x41); memory cards are absent.
 */
#include "psx.h"

static u16 s_mode, s_ctrl, s_baud;
static u32 s_stat_irq;
static bool s_rx_full;
static u8 s_rx;
static int s_step;          /* position in the current pad transaction */
static int s_device;        /* 0 none, 1 pad, 2 memory card (absent) */
static u8 s_pending_rx;
static bool s_pending_ack;

#define XFER_CYCLES 1100
#define ACK_CYCLES 400

static void ack_cb(void) {
    if (s_ctrl & 0x1000) {
        s_stat_irq = 1;
        irq_raise(IRQ_SIO0);
    }
}

static void xfer_cb(void) {
    s_rx = s_pending_rx;
    s_rx_full = true;
    if (s_pending_ack) sched_event(EVT_SIO, ACK_CYCLES, ack_cb);
}

static u8 pad_byte(u8 tx, bool *ack) {
    int port = (s_ctrl >> 13) & 1;
    u16 b = g_pad_buttons[port];
    *ack = true;
    switch (s_step++) {
    case 0:
        if (tx == 0x01) { s_device = 1; return 0xff; }
        if (tx == 0x81) { s_device = 2; *ack = false; return 0xff; }  /* no memory card */
        s_device = 0; *ack = false; return 0xff;
    case 1: if (tx == 0x42) return 0x41; *ack = false; return 0xff;
    case 2: return 0x5a;
    case 3: return b & 0xff;
    case 4: *ack = false; return b >> 8;
    default: *ack = false; return 0xff;
    }
}

static void sio_tx(u8 v) {
    bool ack = false;
    u8 r = 0xff;
    if ((s_ctrl & 2) && s_device != 2) r = pad_byte(v, &ack);
    s_pending_rx = r;
    s_pending_ack = ack;
    sched_event(EVT_SIO, XFER_CYCLES, xfer_cb);
}

u32 sio_read(u32 off, int size) {
    (void)size;
    switch (off) {
    case 0x0: {  /* RX data */
        u8 v = s_rx_full ? s_rx : 0xff;
        s_rx_full = false;
        return v;
    }
    case 0x4: return 0x5 | (s_rx_full ? 2 : 0) | (s_stat_irq ? 0x200 : 0);
    case 0x8: return s_mode;
    case 0xa: return s_ctrl;
    case 0xe: return s_baud;
    }
    return 0;
}

void sio_write(u32 off, u32 v, int size) {
    (void)size;
    switch (off) {
    case 0x0: sio_tx((u8)v); break;
    case 0x8: s_mode = (u16)v; break;
    case 0xa:
        if (v & 0x40) { s_stat_irq = 0; s_rx_full = false; s_step = 0; s_device = 0; v &= ~0x40u; }
        if (v & 0x10) s_stat_irq = 0;
        if (!(v & 2)) { s_step = 0; s_device = 0; }      /* /JOY deselected: transaction ends */
        else if (!(s_ctrl & 2)) s_step = 0;
        if (((v >> 13) & 1) != ((s_ctrl >> 13) & 1)) s_step = 0;
        s_ctrl = (u16)(v & ~0x10u);
        break;
    case 0xe: s_baud = (u16)v; break;
    }
}

/* ---- save states (savestate.h) ---------------------------------------------- */
#include "savestate.h"
void sio_state_io(StateIO *io) {
    SS_VAR(io, s_mode); SS_VAR(io, s_ctrl); SS_VAR(io, s_baud); SS_VAR(io, s_stat_irq);
    SS_VAR(io, s_rx_full); SS_VAR(io, s_rx); SS_VAR(io, s_step); SS_VAR(io, s_device);
    SS_VAR(io, s_pending_rx); SS_VAR(io, s_pending_ack);
}
