/* video.c -- VDC (HuC6270) + VCE device model and frame renderer.
 *
 * Semantics follow a reference core's VDC and VCE (Mednafen `huc6270/vdc.cpp` and
 * `pce/vce.cpp`) for the register interface, and the empirically measured format for
 * the picture (see video.h).  Known differences from the emulator this port now
 * targets are listed in docs/REFERENCE.md.
 *
 * Deliberate simplifications, each stated rather than hidden:
 *   - no dot-level sprite/BG interleaving: a visible line is rendered when the
 *     raster enters it, using the registers as they stand at that moment.  A
 *     register changed *during* a line therefore shows up on the next line, which
 *     is what a program writing during hblank expects;
 *   - mid-frame SAT DMA is applied at the start of vblank (and immediately if the
 *     write lands during vblank), not bit-exactly at the write, and its 1,024-dot
 *     duration is NOT modelled as time (the reference holds
 *     `CheckAndCommitPending` off for it, vdc.cpp:565/1187, which would block VRAM
 *     commits for three lines).  Measured: this game never has a SAT DMA pending
 *     at the test point -- the reference's own DEV record says `burst_mode` -- so
 *     the omission is not reachable on this route;
 *   - VRAM->VRAM DMA runs to completion in one step, so its commit points are not
 *     events here (the game does not use it: `dma=0` on every route);
 *   - hi-res (512-dot) modes and the 4bpp->2bpp sprites of the SuperGrafx are not
 *     implemented: this game runs 256-wide.
 *
 * The VDC's VRAM access wait states ARE modelled, in master clocks, with the
 * reference's own event granularity and its 3x steal ratio -- see the note on
 * vdc_vram_wait below.
 */
#include "video.h"

#include <string.h>

static const int inc_tab[4] = { 1, 32, 64, 128 };
static const int bat_width_tab[4] = { 32, 64, 128, 128 };
static const int bat_width_shift_tab[4] = { 5, 6, 7, 7 };

/* ------------------------------------------------- VRAM access wait states --
 *
 * The HuC6270 has one VRAM port and the CPU shares it with the display.  A CPU
 * write to the data port does not reach VRAM when it is issued: the word is
 * latched into the chip, committed at the chip's next access opportunity, and
 * until it is committed the chip reports BSY and stalls the CPU.
 *
 * The reference core (hw_video/huc6270/vdc.cpp) models exactly that:
 *
 *     case 0x02: if(!msb) write_latch = V;
 *                else { DoWaitStates();
 *                       pending_write = true; pending_write_addr = MAWR;
 *                       pending_write_latch = write_latch | (V << 8);
 *                       MAWR += vram_inc_tab[(CR >> 11) & 0x3];
 *                       CheckAndCommitPending(); }
 *
 *     void VDC::CheckAndCommitPending(void) {
 *         if(sat_dma_counter <= 0 && !DMARunning && !mystery_phase) { ...commit... } }
 *
 *     void VDC::DoWaitStates(void) {
 *         while(VDC_IS_BSY)                      // pending_read || pending_write
 *             if(!WSHook || !WSHook(-1)) { ...force a commit...; break; } }
 *
 * and `WSHook` is VCE::WS_Hook, which steals the time to the chip's NEXT EVENT:
 *
 *     to_steal = CalcNextEvent();   // VCE::CalcNextEvent: hblank / VDC H phase
 *     HuCPU.StealCycles(to_steal);  // ADDCYC: the CPU clock jumps forward, and
 *                                   // crossing the next event runs the VDC (HappySync)
 *
 * Two things follow, and both matter:
 *
 *  1. `mystery_phase` is a window in which the chip will not commit a queued word
 *     (VDC::HSync arms it: `mystery_counter = 48; mystery_phase = false;`, and
 *     VDC::Run() then opens the port for 48 dots and closes it for one 16-dot
 *     window -- vdc.cpp:469, 536-547, 1185).
 *  2. The stall is not "until the window ends": it is the sum of WHOLE event
 *     distances until the VDC has run past the commit point, because each steal
 *     jumps the CPU to the next event boundary and the VDC is only run then.
 *
 * The numbers, all from the chip's own clock (master = 21.477 MHz; CPU = master/3;
 * one VDC dot = 4 master clocks in the 5 MHz dot-clock mode Rondo selects, VCE
 * CR = $04):
 *
 *   line        1365 master = 455 CPU cycles = 341.25 dots
 *   hblank      237 master  (VCE::SyncSub: `hblank_counter = hblank ? 237 : 1128`)
 *   window      48 dots open (192 master), then 16 dots closed (64 master),
 *               so the port is shut from master 192 to 256 of every line
 *   H phases    resynced at the hblank-low edge (VDC::HSync(false): HSW, 8 dots),
 *               then HDS 1, HDS_PART2 2, HDS_PART3 21, HDW 252, HDW_FINAL 4, HDE 40
 *               dots; HPHASE_HDW is a commit point (vdc.cpp:685)
 *
 * WHERE THAT SITS IN THIS MODEL'S LINE.  This renderer draws a line at its own
 * dot 0 and latches CR/BYR/BXR at dots 431/432/434 (vdc_tick below).  The
 * reference draws a line at HPHASE_HDW (32 dots after the display start = master
 * 365) and latches the shadows 8/9/11 dots after that (master 269/273/281).
 * Taking dot 0 = master 365 puts this model's three latches at master 293/296/302,
 * within 8 dots of the reference's -- the closest of the alignments available, and
 * the one the raster already implies.  So `ws_master` starts at 365, the closed
 * window is this model's dots 397..419, and both fall before the line's render
 * point, which is why no separate "pending word is visible to the display" rule
 * is needed.
 *
 * WHAT IS NOT MODELLED, stated rather than hidden: the read queue (a MARR write
 * queues a prefetch in the reference and stalls the same way), SAT DMA and
 * VRAM->VRAM DMA as time (this model runs both to completion in one step, so
 * their commit points are not events here), and the per-slot access pattern of
 * MWR bits 1-0 (the chip grants one CPU access per 2 dots in width 0; the
 * reference does not model that either). */
#define VD_WS_MASTER_PER_CYCLE 3
#define VD_WS_LINE_MASTER      1365     /* 455 CPU cycles */
#define VD_WS_HSYNC_MASTER     237      /* hblank high 0..236, low 237..1364 */
#define VD_WS_OPEN_MASTER      192      /* 48 dots, port open */
#define VD_WS_CLOSED_MASTER    64       /* 16 dots, port closed */
#define VD_WS_DOT0_MASTER      365
#define VD_WS_BUDGET_MASTER    (455 * 64)   /* VCE::WS_Hook's per-frame cap */

/* A sprite-attribute transfer is 256 words at four VDC clocks a word -- 1,024
 * clocks, most of a scanline (huc6270.cpp: Clock()/SATTransfer(), pending 1024 ->
 * 0 with a word landed on every fourth clock). */
#define VD_SAT_TRANSFER_CLOCKS 1024

/* The horizontal phase machine, in master clocks (VDC::Run / IncRCR). */
#define WS_PH_HSW  0
#define WS_PH_HDS  1
#define WS_PH_HDS2 2
#define WS_PH_HDS3 3
#define WS_PH_HDW  4
#define WS_PH_HDWF 5
#define WS_PH_HDE  6
#define WS_HSW_RESYNC_MASTER  (8 * 4)       /* HSync(false): HSW with 8 dots left */

static const int ws_phase_len[7] = {
    (2 + 1) * 8 * 4,      /* HSW: (HSW+1)*8 dots */
    1 * 4,                /* HDS: TimeFromHDSStartToBYRLatch, HDS <= 2 -> 1 dot */
    2 * 4,                /* HDS_PART2 */
    ((2 + 1) * 8 - 1 - 2) * 4,   /* HDS_PART3 */
    ((31 + 1) * 8 - 4) * 4,      /* HDW: (HDW+1)*8 - 4 dots */
    4 * 4,                /* HDW_FINAL */
    (4 + 1) * 8 * 4       /* HDE */
};

static uint16_t vdc_bus_read(Vdc *v, unsigned addr);

static void vdc_commit_pending(Vdc *v)
{
    if (v->pending_write) {
        if (v->pending_waddr < VD_VRAM_WORDS)
            v->vram[v->pending_waddr] = v->pending_wval;
        v->pending_write = 0;
    }
    if (v->pending_read) {
        v->read_buffer = vdc_bus_read(v, v->pending_raddr);
        v->pending_read = 0;
    }
}

/* One chunk of the chip's port clock: advance `master` clocks, stopping at every
 * event (hblank edge, H-phase boundary, window boundary) exactly as VDC::Run and
 * VCE::SyncSub do, and commit the queued word where they would. */
static void vdc_ws_advance(Vdc *v, int master)
{
    while (master > 0) {
        int hb = (v->ws_master < VD_WS_HSYNC_MASTER)
                 ? (VD_WS_HSYNC_MASTER - v->ws_master)
                 : (VD_WS_LINE_MASTER - v->ws_master);
        int chunk = master;

        if (chunk > hb)
            chunk = hb;
        if (chunk > v->ws_hp_left)
            chunk = v->ws_hp_left;
        if (v->ws_mystery > 0 && chunk > v->ws_mystery)
            chunk = v->ws_mystery;
        if (chunk <= 0)
            chunk = 1;

        v->ws_master += chunk;
        master -= chunk;

        if (v->ws_mystery > 0) {
            v->ws_mystery -= chunk;
            if (v->ws_mystery <= 0) {
                /* VDC::Run: the window alternates once, and the reference never
                 * re-arms the counter inside a line (vdc.cpp:536-547). */
                if (!v->ws_blocked) {
                    v->ws_blocked = 1;
                    v->ws_mystery = VD_WS_CLOSED_MASTER;
                } else {
                    v->ws_blocked = 0;
                    vdc_commit_pending(v);
                }
            }
        }

        v->ws_hp_left -= chunk;
        if (v->ws_hp_left <= 0) {
            switch (v->ws_phase) {
            case WS_PH_HDS:  v->ws_phase = WS_PH_HDS2; break;
            case WS_PH_HDS2: v->ws_phase = WS_PH_HDS3; break;
            case WS_PH_HDS3: v->ws_phase = WS_PH_HDW;  break;
            case WS_PH_HDW:  v->ws_phase = WS_PH_HDWF; break;
            case WS_PH_HDWF: v->ws_phase = WS_PH_HDE;  break;
            case WS_PH_HDE:  v->ws_phase = WS_PH_HSW;  break;
            default:         v->ws_phase = WS_PH_HDS;  break;
            }
            if (v->ws_phase == WS_PH_HDW) {
                /* HPHASE_HDW: the chip commits the queued word before it starts
                 * fetching the line (vdc.cpp:685). */
                vdc_commit_pending(v);
            }
            v->ws_hp_left = ws_phase_len[v->ws_phase];
        }

        if (v->ws_master >= VD_WS_LINE_MASTER) {
            /* hblank goes high: the line starts, and HSync(true) arms the window
             * (48 dots open, then 16 closed). */
            v->ws_master -= VD_WS_LINE_MASTER;
            v->ws_hblank = 1;
            v->ws_mystery = VD_WS_OPEN_MASTER;
            v->ws_blocked = 0;
        } else if (v->ws_master >= VD_WS_HSYNC_MASTER && v->ws_hblank) {
            /* hblank goes low: the display line begins and HSync(false) resyncs
             * the phase machine to HSW with 8 dots left. */
            v->ws_hblank = 0;
            v->ws_phase = WS_PH_HSW;
            v->ws_hp_left = WS_HSW_RESYNC_MASTER;
        }
    }
}

unsigned vdc_vram_wait(const Vdc *v)
{
    /* DoWaitStates: steal the distance to each next event until the chip has run
     * past the commit point.  Run on a copy, because the caller charges the
     * cycles through its own clock and must not see the state move twice.
     *
     * TWO UNITS, AND THE RATIO BETWEEN THEM IS THE WHOLE COST.  `CalcNextEvent()`
     * returns MASTER clocks (VCE::CalcNextEvent: `hblank_counter` is 237/1128 of
     * the 1365-clock line, and the VDC's own H phase is converted by
     * `child_event[0] * dot_clock_ratio - clock_divider`).  `WS_Hook` then calls
     * `HuCPU.StealCycles(to_steal)`, and `StealCycles` is `ADDCYC(count)`, whose
     * argument is in CPU CYCLES: `int master = (x * 3) << speed_shift_cache`
     * (huc6280.h:227, and FORMAT-CYC.md's "ADDCYC(n) = +3n master clocks").  So a
     * stall of d master clocks moves the CPU clock by 3*d master clocks -- the
     * CPU loses d CYCLES for an event d master clocks away.  The chip is advanced
     * by the same 3*d, exactly as `HappySync` runs the VCE over the stolen span.
     *
     * The previous version divided by 3 (`ceil(steal/3)`) and advanced the chip by
     * `steal`, so every stall was three times too cheap.  That is the residual that
     * showed up on the $5BC2 fill loop (16.19 against 16.442) and as missing long
     * inter-write gaps in the menu's burst frame: on console
     * frame 7,604 all nine long gaps are exactly 3 x an event distance to the
     * port's reopen, and the quotient lands inside the closed window (master
     * 192..256 of the line) in every case.
     *
     * The read port queues as well, so the loop condition is the reference's
     * `VDC_IS_BSY` = pending_read || pending_write, not the write alone. */
    Vdc s = *v;
    unsigned cycles = 0;
    int budget = 0;

    while (s.pending_write || s.pending_read) {
        int hb = (s.ws_master < VD_WS_HSYNC_MASTER)
                 ? (VD_WS_HSYNC_MASTER - s.ws_master)
                 : (VD_WS_LINE_MASTER - s.ws_master);
        int steal = hb;

        if (steal > s.ws_hp_left)
            steal = s.ws_hp_left;
        if (steal <= 0)
            steal = 1;
        if (budget + steal > VD_WS_BUDGET_MASTER)
            break;                       /* VCE::WS_Hook's cap: force a commit */
        cycles += (unsigned)steal;       /* StealCycles: d master -> d CPU cycles */
        budget += steal;
        vdc_ws_advance(&s, steal * VD_WS_MASTER_PER_CYCLE);
    }
    return cycles;
}

void vdc_reset(Vdc *v)
{
    memset(v, 0, sizeof *v);
    v->mwr = 0;
    v->cr = 0;
    v->cr_sh = 0;
    v->bxr_sh = 0;
    v->byr_sh = 0;
    v->rcr_count = 0;
    v->ws_master = VD_WS_DOT0_MASTER;
    v->ws_phase = WS_PH_HDW;           /* dot 0 is HPHASE_HDW */
    v->ws_hp_left = ws_phase_len[WS_PH_HDW] - VD_WS_DOT0_MASTER;
    v->ws_hblank = 0;                  /* master 365 is inside the display line */
    v->ws_mystery = 0;                 /* the window (192..256) is already past */
    v->ws_blocked = 0;
}

/* ---------------------------------------------------------------- VDC I/O -- */

/* One VRAM word read as the chip's bus sees it.
 *
 * The VDC has 32,768 connected words but a 16-bit pointer, so half of the
 * address space is undriven.  A driven read updates the "retained bus value";
 * an undriven read ($8000..$FFFF) leaves VRAM alone and returns that retained
 * word -- it does not wrap to low VRAM and does not invent zero.  (Measured on the
 * chip's bus; the VDC's 16-bit pointer cannot reach the upper half of its own
 * address space.)
 *
 * The retained value is shared: a CPU prefetch, a VRAM->VRAM DMA source read and
 * a SATB DMA word all drive it.  Only the CPU prefetch path can be observed by a
 * program through VRR, so it is the one the tests pin.
 *
 * Divergence, stated rather than hidden: Mednafen's VRAM array is 65,536 words
 * and never written above 32K, so its out-of-range read returns 0 rather than the
 * retained word (huc6270/vdc.cpp, CheckAndCommitPending: it calls the read
 * unimplemented and still stores VRAM[pending_read_addr]).  We follow the
 * documented bus because no program in this tree reads above 32K -- if one ever
 * does, this is the line to revisit.
 */
static uint16_t vdc_bus_read(Vdc *v, unsigned addr)
{
    if (addr < VD_VRAM_WORDS)
        v->vram_latch = v->vram[addr];
    return v->vram_latch;
}

/* The read-data prefetch.
 *
 * Writing MARR's high byte starts one, and so does reading VRR's high byte after
 * returning the old buffer.  The prefetch removes the word at MARR into the read
 * buffer and advances MARR by the increment selected in CR bits 11-12 -- one
 * advance per WORD, never per byte.  A program then reads the low byte and the
 * high byte of that one word, and gets that one word.
 * (measured; the reference core implements the same two prefetch triggers,
 * huc6270/vdc.cpp case 0x01 / case 0x3.)
 *
 * It is QUEUED, not performed: `pending_read` holds the address and
 * `vdc_commit_pending` fills `read_buffer` at the chip's next access opportunity
 * (vdc.cpp:1185-1210, `CheckAndCommitPending`).  That is not a refinement -- it is
 * what makes the port report BSY after a read and therefore what makes a
 * read-modify-write loop over the data port pay a second stall per iteration. */

static void vdc_sat_start(Vdc *v)
{
    /* START a sprite-attribute transfer.  The words do NOT move here.
     *
     * Geargrafx moves the table ONE WORD PER FOUR VDC CLOCKS while the chip is
     * clocked, 256 words, so `m_sat_transfer_pending` counts 1,024 down to 0
     * (`huc6270.cpp: Clock() -> SATTransfer()`: `m_sat_transfer_pending--; if
     * ((m_sat_transfer_pending & 3) == 0) { i = 255 - (pending >> 2); m_sat[i] =
     * ReadVRAM(satb + i); ...}`), and raises SAT_END -- status bit 3 -- only when
     * the last word has landed (`huc6270.cpp:613-617`).
     *
     * Copying the whole table in one go, which this did, makes bit 3 visible
     * 1,024 clocks too early: the game's vblank hook reads the status register at
     * its first instruction (`lda $0000 / sta $3A` at $69:$4186/$4189) only a few
     * hundred clocks after the vblank edge, so it saw "sprite DMA finished" on
     * every frame here and never on the console.  That is measured, not argued:
     * with the bulk copy the port held $3A = $28 (SAT-DMA end | vblank) where
     * Geargrafx holds $20 (vblank) on 4,778 of 5,000 frames.
     *
     * A DVSSR near the top of VRAM transfers only the words that exist; the rest
     * of the table keeps its old contents (Geargrafx's `ReadVRAM` wraps, this
     * model's does not -- the bounds test lives in vdc_sat_step). */
    v->sat_pending = 0;
    v->sat_clock = VD_SAT_TRANSFER_CLOCKS;
    v->sat_dma_runs++;
}

/* One step of the transfer, `master` clocks of chip time.  Called from vdc_tick()
 * once per VDC dot (three master clocks), which is the finest granularity the
 * chip model has; a word therefore lands within one dot of where Geargrafx puts
 * it, which is 1/341 of the transfer. */
static void vdc_sat_step(Vdc *v, unsigned master)
{
    while (master-- > 0 && v->sat_clock > 0) {
        v->sat_clock--;
        if ((v->sat_clock & 3u) == 0) {
            unsigned base = v->dvssr;
            unsigned i = 255u - ((unsigned)v->sat_clock >> 2);
            if (base + i < VD_VRAM_WORDS)
                v->sat[i] = vdc_bus_read(v, base + i);
            if (v->sat_clock == 0 && (v->dcr & 0x01)) {
                /* DS (bit 3) is the "sprite DMA finished" flag the kernel's
                 * handler polls after writing DVSSR ($13), and it is raised by
                 * the completion, not by the request.  A status read clears it
                 * (see vdc_read). */
                v->status |= VDS_DS;
                v->irq_line = 1;
            }
        }
    }
}

static void vdc_vram_dma(Vdc *v)
{
    /* VRAM -> VRAM transfer, length in words, direction from DCR bits 2/3.
     * A destination the chip does not drive is left alone; the pointer still
     * moves, which is what the reference implementation does. */
    unsigned len = v->lenr ? v->lenr : 0x10000;
    unsigned src = v->sour & 0xFFFF, dst = v->desr & 0xFFFF;
    int src_inc = (v->dcr & 0x04) ? -1 : 1;
    int dst_inc = (v->dcr & 0x08) ? -1 : 1;
    unsigned i;
    for (i = 0; i < len; i++) {
        uint16_t w = vdc_bus_read(v, src);
        if (dst < VD_VRAM_WORDS)
            v->vram[dst] = w;
        src = (unsigned)(((int)src + src_inc) & 0xFFFF);
        dst = (unsigned)(((int)dst + dst_inc) & 0xFFFF);
    }
    v->sour = (uint16_t)src;
    v->desr = (uint16_t)dst;
    v->lenr = 0;
    v->dma_runs++;
    v->dma_pending = 0;
    if (v->dcr & 0x02)
        v->status |= VDS_DV;
}

/* This chip's registers are BYTE-addressable, not word-committed.
 *
 * The oracle updates exactly one byte of the register on every write and leaves
 * the other byte as the register already held:
 *
 *     reg = (reg & (0xFF << (msb ? 0 : 8))) | (val << (msb ? 8 : 0));
 *     (huc6270/vdc.h, VDC_REGSETP)
 *
 * and the console depends on it: it sets MWR by writing the LOW BYTE ALONE --
 * trace pc $E291, port $0002, value $50 at frame 1080, with no high-byte write
 * anywhere near it.  A model that latched both bytes and only committed on the
 * second one threw that write away and kept MWR at $0030, which is a 128-cell
 * map stride instead of 64.  The background was then fetched at double the row
 * stride and capped to 32 map rows, so the whole picture was drawn at half
 * vertical scale in the top half of the frame -- the defect seen on the title
 * screen, which no memory comparison could show because VRAM itself was right.
 *
 * So: value updates are byte-wise and immediate.  The side effects that start a
 * transfer (the VRAM data port, the CR interrupt check, the DMA triggers) stay on
 * the second byte, where they already were, so this changes values, not timing.
 */
#define REGSET(r, v, is_msb) \
    ((r) = (uint16_t)(((r) & (0xFFu << ((is_msb) ? 0 : 8))) | \
                      ((uint32_t)(v) << ((is_msb) ? 8 : 0))))

void vdc_write(Vdc *v, uint16_t off, uint8_t val)
{
    unsigned a = off & 0x3;
    int msb = off & 1;
    unsigned sel = v->select & 0x1F;

    v->writes++;

    if (a == 0) {                          /* register select */
        v->select = val & 0x1F;
        return;
    }
    if (a == 1)                            /* $0001: nothing (mirror of status) */
        return;

    /* THE DATA-PORT LATCH IS THE VWR REGISTER, AND NOTHING BUT THE DATA PORT
     * WRITES IT.
     *
     * Geargrafx puts every write to $0002/$0003 in the register the address
     * register currently selects -- no separate write latch exists:
     *
     *   huc6270.cpp:358-363
     *       if (msb) m_register[m_address_register] = (m_register[..] & 0x00FF) | (value << 8);
     *       else     m_register[m_address_register] = (m_register[..] & 0xFF00) | value;
     *
     * and a VRAM store then reads register 2 and nothing else:
     *
     *   huc6270_inline.h:266   m_vram[MAWR] = m_register[HUC6270_REG_VWR];
     *
     * So a write to BXR ($07), BYR ($08), CR ($05) or any other register while
     * the data port is selected leaves a half-written data-port word untouched.
     *
     * This model kept ONE latch that every write to $0002/$0003 overwrote,
     * whichever register was selected -- so an interrupt landing between
     * `sta $0002` and `sta $0003` substituted its own byte for the low half of
     * the word in flight.  MEASURED, frame 7,836, the wave generator's fill
     * ($6C:$81ED) writing VRAM word $1985:
     *
     *   pc $81F4  sta $0002 = $0F      vwr becomes $FF0F   <- the real low byte
     *   pc $42C8  sta $0002 = $08 sel $05 (CR)    an IRQ handler in bank $69
     *   pc $4301  sta $0002 = $00 sel $07 (BXR)   vwr becomes $FF00  <- low byte GONE
     *   pc $4307  sta $0003 = $00 sel $07 (BXR)
     *   pc $4312  sta $0002 = $00 sel $08 (BYR)   vwr becomes $0000
     *   pc $4318  sta $0003 = $00 sel $08 (BYR)
     *   pc $81FA  sta $0003 = $F0                 vwr becomes $F000 -> stored
     *
     * The data on the disc says the word is $F00F (payload offset $DB3FB: the
     * script byte pair is 0F F0), the console holds $F00F, and this port held
     * $F000 -- one nibble of one tile row, drawn as the dark 4-pixel notch in the
     * backdrop's bright line.  The interrupt is the game's own vblank handler
     * setting up its scroll split; it is not a timing accident to be tuned away.
     */
    if (sel == 0x02) {
        if (msb)
            v->vwr = (uint16_t)((v->vwr & 0x00FF) | ((uint16_t)val << 8));
        else
            v->vwr = (uint16_t)((v->vwr & 0xFF00) | val);
    }

    /* The register's own value takes the byte now, whichever byte it is. */
    switch (sel) {
    case 0x00: REGSET(v->mawr, val, msb); break;
    case 0x01: REGSET(v->marr, val, msb); break;
    case 0x02: break;                      /* VRAM data: latched, written below */
    case 0x05: REGSET(v->cr, val, msb); break;
    case 0x06: REGSET(v->rcr, val, msb); v->rcr &= 0x3FF; break;
    case 0x07: REGSET(v->bxr, val, msb); v->bxr &= 0x3FF; break;
    case 0x08:
        REGSET(v->byr, val, msb);
        v->byr &= 0x1FF;
        /* A BYR write also reloads the running vertical offset, on EITHER byte
         * (oracle: huc6270/vdc.cpp case 0x08 does `BG_YMoo = BYR` on the low and
         * the high byte, with the comment "only changing on MSB breaks Youkai
         * Douchuuki").  The running counter is what the picture uses; the
         * register itself only reloads it at the first visible line.  Games use
         * the immediate reload to place each horizontal strip: the stage code
         * here writes BYR eight times a frame. */
        v->bg_y = v->byr;
        break;
    case 0x09: REGSET(v->mwr, val, msb); break;
    case 0x0A: REGSET(v->hsr, val, msb); break;
    case 0x0B: REGSET(v->hdr, val, msb); break;
    case 0x0C: REGSET(v->vsr, val, msb); break;
    case 0x0D: REGSET(v->vdr, val, msb); break;
    case 0x0E: REGSET(v->vcr, val, msb); break;
    case 0x0F: REGSET(v->dcr, val, msb); break;
    case 0x10: REGSET(v->sour, val, msb); break;
    case 0x11: REGSET(v->desr, val, msb); break;
    case 0x12: REGSET(v->lenr, val, msb); break;
    case 0x13: REGSET(v->dvssr, val, msb); break;
    default: break;
    }

    if (!msb)                              /* side effects wait for the second byte */
        return;

    v->reg_writes[sel & 0x1F]++;

    switch (sel) {
    case 0x01:
        /* Writing MARR's high byte starts a prefetch: the word at MARR goes into
         * the read buffer and MARR advances by one increment.  The low byte only
         * changes the address -- it does not read anything.
         *
         * The prefetch is QUEUED, not performed: the reference sets
         * `pending_read` and lets `CheckAndCommitPending` fill `read_buffer` at the
         * chip's next access opportunity (vdc.cpp:1227-1238).  That is what makes
         * a read-modify-write loop over the data port stall: the read leaves the
         * port busy, and the next access's `DoWaitStates` waits for it. */
        v->pending_read = 1;
        v->pending_raddr = v->marr;
        v->marr = (uint16_t)(v->marr + inc_tab[(v->cr >> 11) & 3]);
        if (!v->ws_blocked)
            vdc_commit_pending(v);
        break;
    case 0x02: {                           /* VRAM write at MAWR, then increment */
        /* The word is QUEUED, not stored: it commits at the chip's next access
         * opportunity (see the wait-state note at the top of this file).  When the
         * port is open that is now, which is the common case; inside the closed
         * window the word waits and the next data-port access stalls for it.
         *
         * Only the low 32K is driven.  A write above it is discarded -- it must
         * NOT be wrapped down, because word 0 of VRAM is the background map, and
         * a masked write would silently overwrite it.  The pointer still moves,
         * exactly as it does for a normal write. */
        v->pending_write = 1;
        v->pending_waddr = v->mawr;
        v->pending_wval = v->vwr;
        v->mawr = (uint16_t)(v->mawr + inc_tab[(v->cr >> 11) & 3]);
        if (!v->ws_blocked)
            vdc_commit_pending(v);
        break;
    }
    case 0x05:
        if ((v->cr & 0x01) && (v->status & VDS_CR))
            v->irq_line = 1;
        break;
    case 0x12:
        v->status &= (uint16_t)~VDS_DV;     /* a new transfer consumes the old flag */
        v->dma_pending = 1;
        vdc_vram_dma(v);
        break;
    case 0x13:
        v->sat_pending = 1;
        v->status &= (uint16_t)~VDS_DS;     /* a new transfer consumes the old flag */
        if (v->vblank)
            vdc_sat_start(v);
        break;
    default:
        break;
    }
}

uint8_t vdc_read(Vdc *v, uint16_t off)
{
    unsigned a = off & 0x3;
    unsigned sel = v->select & 0x1F;
    uint8_t ret = 0xFF;

    v->reads++;

    if (a == 0) {                          /* status, and clears the IRQ latch */
        /* BSY (bit 6) is the VRAM port: it is live, and unlike the low six bits
         * a status read does NOT clear it (huc6270/vdc.cpp, `ret = status |
         * (VDC_IS_BSY ? 0x40 : 0x00)` and `status &= ~0x3F`). */
        ret = (uint8_t)(v->status | ((v->pending_write || v->pending_read) ? VDS_BSY : 0));
        /* Reading the status register acknowledges every flag it returned, all
         * six low bits (oracle: huc6270/vdc.cpp, `status &= ~0x3F` on a read of
         * A==0).  That includes the two DMA-completion flags and VD, the
         * vertical-blank flag -- an earlier model kept DV/VD set until the next
         * transfer, which is not the chip and hides a handler that polls. */
        v->status &= (uint16_t)~0x3F;
        v->irq_line = 0;
        return ret;
    }
    if (a == 2) {
        /* huc6270.cpp:298-305: the DATA PORT read returns the low byte of the
         * read buffer whatever the address register happens to be --
         *
         *     case 2: if (m_pending_memory_read) WaitForVramAccess();
         *             return m_read_buffer & 0xFF;
         *
         * and no prefetch is queued.  This model returned the write latch when
         * another register was selected; with the latch now living in VWR alone
         * (see vdc_write) that value would be a word the game never read, so the
         * return is the reference's. */
        return (uint8_t)(v->read_buffer & 0xFF);
    }
    if (a == 3) {
        /* huc6270.cpp:306-321: the high byte of the read buffer, and the next
         * prefetch is queued only when the data port is the selected register
         * (`if (m_address_register == HUC6270_REG_VRR) QueueMemoryRead();`). */
        uint8_t hi = (uint8_t)(v->read_buffer >> 8);
        if (sel == 0x02) {
            v->pending_read = 1;
            v->pending_raddr = v->marr;
            v->marr = (uint16_t)(v->marr + inc_tab[(v->cr >> 11) & 3]);
            if (!v->ws_blocked)
                vdc_commit_pending(v);
        }
        return hi;
    }
    return ret;
}

/* ---------------------------------------------------------------- VCE I/O -- */

/* VCE::FixPCache, exactly: the display cache is only written for entries whose
 * low nibble is non-zero, and a write to an entry with a zero low byte seeds the
 * whole low-nibble-0 row of its half from that entry.  See the note on Vce.cache. */
static void vce_fix_cache(Vce *v, unsigned entry)
{
    entry &= 0x1FF;
    if (!(entry & 0xFF)) {
        unsigned x;
        for (x = 0; x < 16; x++)
            v->cache[(entry & 0x100) | (x << 4)] = v->pal[entry & 0x100];
    }
    if (!(entry & 0x0F))
        return;
    v->cache[entry] = v->pal[entry];
}

void vce_fix_all(Vce *v)
{
    unsigned i;
    memset(v->cache, 0, sizeof v->cache);
    for (i = 0; i < 512; i++)
        vce_fix_cache(v, i);
}

/* VCE::Reset -- the chip's POWER-ON colour table, not zero.
 *
 * pce/vce.cpp, VCE::Reset():
 *
 *     memset(color_table_cache, 0, sizeof(color_table_cache));
 *     for(i = 0; i < 0x200; i++) {
 *         color_table[i] = ((i ^ (i >> 3)) & 1) ? 0x000 : 0x1FF;
 *         FixPCache(i);
 *     }
 *
 * i.e. a checkerboard of $1FF/$000 in 8-entry groups, entry 0 white, and the
 * display cache built from it by FixPCache (so the low-nibble-0 entries of each
 * half take entry 0's colour: white).  The VDC's display is off at power-on, so
 * every pixel is colour index 0 and the screen is PURE WHITE -- which is what the
 * console's own first two frames are (255.0, one colour, frames 0 and 1).  A VCE
 * zeroed at power-on makes them black, and the card's first palette write then
 * looks like a no-op instead of the white->black transition it is.
 *
 * This is also what a console RESET restores: PCE_Power() calls vce->Reset(). */
void vce_reset(Vce *v)
{
    unsigned i;
    memset(v, 0, sizeof *v);
    for (i = 0; i < 512; i++)
        v->pal[i] = (uint16_t)(((i ^ (i >> 3)) & 1) ? 0x000 : 0x1FF);
    vce_fix_all(v);
}

void vdc_write_vce(Vce *v, uint16_t off, uint8_t val)
{
    switch (off & 0x7) {
    case 0: v->cr = val; break;                       /* VCECR: dot clock / bw */
    case 2: v->cta = (uint16_t)((v->cta & 0x100) | val); break;
    case 3: v->cta = (uint16_t)((v->cta & 0x0FF) | ((val & 1) << 8)); break;
    case 4:
        v->pal[v->cta & 0x1FF] = (uint16_t)((v->pal[v->cta & 0x1FF] & 0x100) | val);
        vce_fix_cache(v, v->cta);
        v->writes++;
        break;
    case 5:
        v->pal[v->cta & 0x1FF] = (uint16_t)((v->pal[v->cta & 0x1FF] & 0x0FF) | ((val & 1) << 8));
        vce_fix_cache(v, v->cta);
        v->cta = (uint16_t)((v->cta + 1) & 0x1FF);
        v->writes++;
        break;
    default:
        break;
    }
}

uint8_t vdc_read_vce(Vce *v, uint16_t off)
{
    switch (off & 0x7) {
    case 4:
        return (uint8_t)(v->pal[v->cta & 0x1FF] & 0xFF);
    case 5: {
        uint8_t hi = (uint8_t)((v->pal[v->cta & 0x1FF] >> 8) & 1);
        v->cta = (uint16_t)((v->cta + 1) & 0x1FF);
        return (uint8_t)(hi | 0xFE);
    }
    default:
        return 0xFF;
    }
}

/* --------------------------------------------------------------- palette -- */

void vce_rgb(const Vce *v, unsigned index, uint8_t rgb[3])
{
    /* ----GGGRRRBBB; each channel 3 bits, scaled 0..255 (measured conversion).
     * The word comes from the DISPLAY CACHE, not from the palette table: see the
     * FixPCache note on Vce.cache -- an index ending in 0 shows the last
     * low-byte-0 write of its half, not its own entry. */
    uint16_t w = v->cache[index & 0x1FF];
    unsigned r = (w >> 3) & 0x07;
    unsigned g = (w >> 6) & 0x07;
    unsigned b = w & 0x07;
    rgb[0] = (uint8_t)((r * 255u) / 7u);
    rgb[1] = (uint8_t)((g * 255u) / 7u);
    rgb[2] = (uint8_t)((b * 255u) / 7u);
}

/* -------------------------------------------------------------- renderer -- */

/* One pixel of an 8x8 4bpp background tile.
 * words: vram[base + row]   = planes 0/1 (byte 0 = plane 0, byte 1 = plane 1)
 *        vram[base + row+8] = planes 2/3
 * Bit 0 of a byte is the rightmost pixel.
 */
static unsigned bg_pixel(const Vdc *v, unsigned base, int row, int px)
{
    uint16_t w01 = v->vram[(base + (unsigned)row) & 0x7FFF];
    uint16_t w23 = v->vram[(base + (unsigned)row + 8) & 0x7FFF];
    /* Bit 0 of a plane byte is the *rightmost* pixel.  Measured, not assumed:
     * mirroring this dropped the correlation against the emulator's own frame
     * from 0.999 to 0.683 (tests/test_video.py keeps the numbers). */
    int bit = 7 - px;
    unsigned p = 0;
    p |= (unsigned)((w01 >> bit) & 1);
    p |= (unsigned)(((w01 >> (bit + 8)) & 1) << 1);
    p |= (unsigned)(((w23 >> bit) & 1) << 2);
    p |= (unsigned)(((w23 >> (bit + 8)) & 1) << 3);
    return p;
}

/* One pixel of a 16x16 sprite: four planes, one 16-bit word per plane per row. */
static unsigned spr_pixel(const Vdc *v, unsigned n, int row, int px)
{
    unsigned base = (n * 64u + (unsigned)(row & 15)) & 0x7FFF;
    unsigned p = 0;
    p |= (unsigned)((v->vram[base] >> px) & 1);
    p |= (unsigned)(((v->vram[(base + 16) & 0x7FFF] >> px) & 1) << 1);
    p |= (unsigned)(((v->vram[(base + 32) & 0x7FFF] >> px) & 1) << 2);
    p |= (unsigned)(((v->vram[(base + 48) & 0x7FFF] >> px) & 1) << 3);
    return p;
}

/* Render one visible output row into `linebuf` as 9-bit VDC pixel values
 * (bits 0-3 colour index, bits 4-7 palette).  Row 0 is the top of the display
 * window.
 *
 * Coordinates are *display-relative*, which was measured rather than assumed:
 * treating rows as frame-absolute (adding VDS, the vertical display start) scored
 * 0.51 against the emulator's frame; display-relative scored 0.999.  So VDS/VSW
 * are video timing, and the background offset and sprite Y are counted from the
 * top of the visible area.
 */
void vdc_render_line(Vdc *v, Vce *vce, int out_row, uint16_t *linebuf)
{
    int bat_width = bat_width_tab[(v->mwr >> 4) & 3];
    int bat_shift = bat_width_shift_tab[(v->mwr >> 4) & 3];
    int bat_height = (v->mwr & 0x40) ? 64 : 32;
    int width = ((v->hdr & 0x7F) + 1) * 8;
    int frame_line = out_row;
    int i;
    (void)vce;

    if (width > VD_W)
        width = VD_W;

    /* THE BACKDROP IS THE VCE'S BACKGROUND COLOUR WHENEVER THE BACKGROUND IS OFF.
     *
     * The oracle core's HPHASE_HDW case fills the line when `!(CR_cache & 0x80)`:
     *
     *     // BG off, sprite on: fill = 0x100.  bg off, sprite off: fill = 0x000
     *     if(!(CR_cache & 0x80)) {
     *         if(!(CR_cache & 0xC0)) fill_val = 0x000;
     *         else                   fill_val = 0x100;
     *
     * The both-off arm is DEAD CODE: the VDW phase sets `burst_mode = !(CR & 0xC0)`
     * on entry (vdc.cpp:358) and the whole fill/copy block is inside `if(!burst_mode)`
     * (vdc.cpp:690), so with both layers off the chip never runs it.  What runs is
     * the `else` at vdc.cpp:610, whose pixel is `uint16 pix = 0x100` -- the VCE's
     * background colour.  So the rule is one line, not two: with the background
     * off the output is entry $100, whether sprites are on or not.
     *
     * Measured, not argued.  The cutscene holds the display off for 15 frames
     * around our 4700-4714 (CR = $0008).  This model filled those lines with
     * entry 0, which the game had just set to $011, and painted the whole screen
     * dark magenta (72,0,36) where the console is black: 57,344 differing pixels
     * on 8 of the 13 sampled frames, the largest single defect left in the intro.
     * With entry $100 -- black -- the same frames are 0. */
    {
        uint16_t fill = 0;
        if (!(v->cr_sh & VDC_BG_ON))
            fill = 0x100;
        for (i = 0; i < VD_W; i++)
            linebuf[i] = fill;
    }

    /* ---- background ---- */
    if (v->cr_sh & VDC_BG_ON) {
        /* The vertical offset is a COUNTER, not "BYR + row".
         *
         * The chip keeps a running line offset (`BG_YMoo` in the oracle core):
         * it is reloaded from BYR at the first visible line and incremented once
         * per line after that.  A BYR write reloads the counter immediately (see
         * vdc_write), so a write that lands during line N shows up on line N+1 as
         * BYR+1 -- which is the Hudson Tech Note rule "the write takes effect on
         * the next line, plus one".
         *
         * "BYR + row" is the same thing ONLY while BYR never changes inside a
         * frame.  The stage code changes it eight times a frame to lay down
         * horizontal strips; with the old formula strip k's map row was BYR_k +
         * screen row, up to 219 rows away from where the console puts it.
         */
        int yoff = (int)(v->bg_y & 0x1FF);
        int bg_row = (yoff >> 3) & (bat_height - 1);
        int row_in_tile = yoff & 7;
        int cell_base = bg_row << bat_shift;
        for (i = 0; i < width; i++) {
            int bx = v->bxr_sh + i;
            int cell = (bx >> 3) & (bat_width - 1);
            uint16_t bat = v->vram[(unsigned)(cell_base + cell) & 0x7FFF];
            /* Bits 0-11 are a word address measured in 16-word units: that is the
             * tile's base word (a 32-byte tile is 16 words). */
            unsigned tile_base = (unsigned)(((bat & 0x0FFF) * 16u) & 0x7FFFu);
            unsigned pal = (unsigned)(bat >> 12);
            unsigned px = bg_pixel(v, tile_base, row_in_tile, bx & 7);
            linebuf[i] = (uint16_t)((pal << 4) | px);
        }
    }

    /* ---- sprites, drawn back to front so sprite 0 wins ---- */
    if (v->cr_sh & VDC_SPR_ON) {
        int active[VD_SPR_MAX_PER_LINE];
        int nactive = 0;
        int s, k;
        int y_line = frame_line;
        /* The sprite layer is built on its own and merged afterwards, because
         * whether a sprite pixel is visible depends on the background pixel
         * underneath it (see the composite at the end of this block). */
        uint16_t sprbuf[VD_W];

        for (i = 0; i < VD_W; i++)
            sprbuf[i] = 0;

        for (s = 0; s < 64; s++) {
            int sy = (int)(v->sat[s * 4 + 0] & 0x3FF) - 0x40;
            int flags = v->sat[s * 4 + 3];
            int h = 16;
            if (flags & 0x1000)
                h = (flags & 0x2000) ? 64 : 32;
            if (y_line >= sy && y_line < sy + h) {
                if (nactive == VD_SPR_MAX_PER_LINE)
                    break;                 /* 16 per line, as the chip does */
                active[nactive++] = s;
            }
        }

        for (k = nactive - 1; k >= 0; k--) {
            int s2 = active[k];
            int sy = (int)(v->sat[s2 * 4 + 0] & 0x3FF) - 0x40;
            int sx = (int)(v->sat[s2 * 4 + 1] & 0x3FF) - 0x20;
            unsigned n = (unsigned)((v->sat[s2 * 4 + 2] >> 1) & 0x3FF);
            int flags = v->sat[s2 * 4 + 3];
            int h = 16, w = (flags & 0x0100) ? 32 : 16;
            int pal = (flags & 0x0F) << 4;
            int yoff = y_line - sy;
            int x;

            if (flags & 0x1000)
                h = (flags & 0x2000) ? 64 : 32;
            if (flags & 0x8000)                       /* vertical flip */
                yoff = h - 1 - yoff;

            /* THE 16-ROW BLOCK INSIDE A 32/64-HIGH SPRITE LIVES IN PATTERN BITS 1-2.
             *
             * Mednafen's FetchSpriteData (hw_video/huc6270/vdc.cpp) is the reference:
             *
             *     no &= sprite_height_no_mask[(flags >> 12) & 3];   // 16: ~0, 32: ~2, 64: ~6
             *     no |= (y_offset & 0x30) >> 3;                     // 16 -> 2, 32 -> 4, 48 -> 6
             *     if (width == 32) no &= ~1;
             *     if (second_half)  no |= 1;
             *
             * so the vertical block occupies bit 1 (32-high) or bits 1-2 (64-high), and
             * bit 0 is the horizontal half.
             *
             * This code used to shift by 4 instead of 3 and to mask with ~1/~3, which put
             * the block index into bit 0 and bit 1.  Bit 0 is then overwritten by the
             * horizontal-half selection below, so *every* row group fetched block 0: a
             * 64-high sprite drew its first 16 rows four times, 16 rows apart, and a
             * 32-high one drew its first 16 rows twice.  Measured on the logo card: the
             * wordmark band appeared three times at y=46/62/78 (spacing 16) and the
             * `血の輪廻` banner -- which lives in blocks 2-4 of the same sprites -- was
             * never fetched at all. */
            if (h == 32)
                n = (n & ~2u) | (unsigned)((yoff & 0x10) >> 3);
            else if (h == 64)
                n = (n & ~6u) | (unsigned)((yoff & 0x30) >> 3);

            for (x = 0; x < w; x++) {
                unsigned pattern = n;
                int px, bit;
                unsigned raw;
                if (w == 32) {
                    if (flags & 0x0800)               /* hflip swaps the halves too */
                        pattern = (n & ~1u) | (unsigned)((x >= 16) ? 0 : 1);
                    else
                        pattern = (n & ~1u) | (unsigned)((x >= 16) ? 1 : 0);
                }
                px = x & 0x0F;
                bit = (flags & 0x0800) ? px : (15 - px);
                raw = spr_pixel(v, pattern, yoff, bit);
                if (raw) {
                    int tx = sx + x;
                    uint16_t prio = (uint16_t)((flags & 0x80) ? 0x200 : 0);
                    if (tx >= 0 && tx < width)
                        sprbuf[tx] = (uint16_t)(0x100u | (unsigned)pal | raw | prio);
                }
            }
        }

        /* SPRITE PRIORITY.  Attribute bit 7 ($80) says "in front of the
         * background"; without it the sprite is behind, and is only seen where
         * the background pixel is colour 0 (transparent).  The oracle composites
         * exactly that way (huc6270/vdc.cpp, end of DrawSprites):
         *
         *     if(sprite_line_buf[x] & 0x0F)
         *       if(!(target[x] & 0x0F) || (sprite_line_buf[x] & 0x200))
         *         target[x] = sprite_line_buf[x];
         *
         * This model used to draw every sprite over the background regardless.
         * Measured on the stage-1 screen: a 32x16 low-priority sprite at
         * (96,116) -- the near-side bridge ornament -- was painted over 128
         * pixels of bridge that the console leaves alone, every frame, on rows
         * 121-127. */
        for (i = 0; i < width; i++)
            if ((sprbuf[i] & 0x0F) &&
                (!(linebuf[i] & 0x0F) || (sprbuf[i] & 0x200)))
                linebuf[i] = (uint16_t)(sprbuf[i] & 0x1FF);
    }
}

/* Composite one rendered row (VDC pixel values) into RGB pixels.
 * A VDC pixel value's bits 0-3 are the colour index and bits 4-7 the palette;
 * bit 8 marks a sprite pixel, which is exactly the +16 palette offset the VCE
 * uses, so the value itself is the colour-table index (0..511). */
static void emit_line(Vdc *v, Vce *vce, int out_row, const uint16_t *linebuf)
{
    int x;
    for (x = 0; x < VD_W; x++) {
        uint8_t rgb[3];
        vce_rgb(vce, (unsigned)(linebuf[x] & 0x1FF), rgb);
        v->frame_pixels[(out_row * VD_W + x) * 3 + 0] = rgb[0];
        v->frame_pixels[(out_row * VD_W + x) * 3 + 1] = rgb[1];
        v->frame_pixels[(out_row * VD_W + x) * 3 + 2] = rgb[2];
    }
}

/* Render the display's top line from the current register state.
 *
 * The renderer draws a line when the line BEGINS (vdc_tick, "render the line we
 * are about to display"), and a frame boundary is a vertical sync, so every row is
 * drawn by the time its frame ends -- except row 0 of the very first frame after a
 * power-on, because the raster starts at the top of line 0 and no line has begun
 * yet.  That leaves one black row at the top of the console's first white screen.
 * A power-on that leaves the raster at line 0 has begun line 0, so draw it. */
void vdc_render_top_line(Vdc *v, Vce *vce)
{
    uint16_t linebuf[VD_W];
    vdc_render_line(v, vce, 0, linebuf);
    emit_line(v, vce, 0, linebuf);
}

void vdc_render_frame(Vdc *v, Vce *vce)
{
    uint16_t linebuf[VD_W];
    int vdw = v->vdr & 0x1FF;
    int row;
    if (vdw <= 0 || vdw > VD_H)
        vdw = VD_H;
    /* Static render: no time passes, so the shadows are the live registers. */
    v->cr_sh = v->cr;
    v->bxr_sh = v->bxr;
    v->byr_sh = v->byr;
    for (row = 0; row < VD_H; row++) {
        /* Static render: no time passes, so synthesise what the running counter
         * would have reached by this line (BYR at the first visible line, +1 per
         * line).  The raster path in vdc_tick does the same thing by counting. */
        v->bg_y = (uint16_t)((v->byr + row) & 0x1FF);
        if (row < vdw)
            vdc_render_line(v, vce, row, linebuf);
        else
            memset(linebuf, 0, sizeof linebuf);
        emit_line(v, vce, row, linebuf);
    }
}

/* ----------------------------------------------------------------- raster -- */

void vdc_tick(Vdc *v, Vce *vce, uint32_t cycles)
{
    uint16_t linebuf[VD_W];

    while (cycles--) {
        vdc_ws_advance(v, VD_WS_MASTER_PER_CYCLE);
        /* The sprite-attribute transfer runs off the chip's own clock, four
         * clocks a word -- it does not wait for a register write to complete it.
         * See vdc_sat_start() for why this is not a bulk copy. */
        if (v->sat_clock)
            vdc_sat_step(v, VD_WS_MASTER_PER_CYCLE);
        v->dot++;
        if (v->dot == 252) {
            if (v->line == VD_LINES - 1)
                v->rcr_count = 0;
            else
                v->rcr_count++;
            if ((v->cr & VDC_RCR_IRQ) &&
                v->rcr_count == (uint16_t)((v->rcr - 0x40) & 0x3FF)) {
                v->status |= VDS_RR;
                v->irq_line = 1;
                v->rcr_irqs++;
            }
        }
        if (v->dot == 431)
            v->cr_sh = v->cr;
        if (v->dot == 432) {
            v->byr_sh = v->byr;
            /* BG_YMoo: reloaded from BYR at the first display line, +1 for every
             * line after it (oracle: IncRCR()/HDS_PART2 -- `if(0 == RCRCount)
             * BG_YMoo = BYR; else BG_YMoo++;`, then BG_YOffset = BG_YMoo).  A BYR
             * write reloads it at once, so a write during line N is drawn as
             * BYR+1 on line N+1. */
            if (v->rcr_count == 0)
                v->bg_y = v->byr;
            else
                v->bg_y = (uint16_t)((v->bg_y + 1) & 0x1FF);
        }
        if (v->dot == 434)
            v->bxr_sh = v->bxr;

        if (v->dot < VD_DOTS)
            continue;
        v->dot = 0;

        /* a line completed */
        v->line++;
        if (v->line >= VD_LINES) {
            v->line = 0;
            v->vblank = 0;
        }

        /* THE FRAME BOUNDARY HERE COMES FROM THE FIRST REFERENCE THIS PORT WAS
         * CALIBRATED AGAINST (BizHawk 2.9.1 / TurboNyma), WHOSE VCE ENDS THE FRAME
         * AT `scanline == 14 + 240`.
         *
         * That core ends a frame in its *VCE*, not in its VDC: Emulate() returns
         * when the VCE's line counter reaches `14 + 240` = line 254 of the field
         * (`pce/vce.cpp`: `if(scanline == 14 + 240) FrameDone = true;`, hard-coded
         * rather than derived from the display window, so `slend` only sets the
         * rendered rect).  The emulator this port targets now declares its frame at
         * the wrap onto line 256 instead, so this boundary is a known difference
         * from the target, recorded in docs/REFERENCE.md rather than fixed.
         *
         * 254 is NINE lines before the end of the 263-line field, not six: an
         * earlier constant here was 6, taken from the 0.9.38-era libretro fork of
         * the same code (`14 + max(240, slend + 1)`, default 257), three lines
         * later.  Every measurement made against the old console trace inherited
         * those three lines.
         *
         * `scanline` in the reference is counted from the first line after the
         * vertical sync pulse, and the pulse is three lines long (VCE: vblank high
         * for 4,095 master clocks, then `NeedSLReset`), so
         *   scanline 0            = this model's `field_end`
         *   scanline 254          = field_end - 9 lines
         * With Rondo's VSR = $0F02 (VSW = 2, VDS = 15) `field_end` is line 246 of
         * this model's counter and the boundary is line 237.
         *
         * WHAT THIS MOVES, AND WHAT IT DOES NOT.  The boundary only decides where
         * the host samples work RAM, applies the pad schedule, fires scheduled
         * resets and writes a trace frame marker; the machine is identical either
         * way.  Moving it three lines (4,095 master clocks) earlier therefore
         * changes no pixel and no work-RAM field that is written inside the
         * handler -- and it does not make an input schedule agree frame for frame
         * where a screen transition starts a frame early, because the redraw burst
         * then lands on the frame an input is pressed.  It is a fidelity
         * correction, not a fix for that.  What it does change, measurably: the
         * vblank IRQ's distance from the frame end becomes 12 lines, as the
         * reference's is, instead of 15; and `$2040:$80` at frame 7,592 stops
         * differing from the reference's.
         *
         * The port's trace against the old console trace is still the check that
         * the *machine* is right: with this boundary the port's IRQ lands 12 lines
         * before its frame marker, and the old console's (Beetle) lands 15 before
         * its own -- the 3-line difference between the two emulators' frame ends,
         * which is exactly what this note is about. */
        {
            /* VSW+1 + VDS+2 lines from the VSW start to the top of the display
             * (huc6270/vdc.cpp: the VPHASE_VSW counter is VSW+1, VPHASE_VDS is
             * VDS+2).  Rondo programs VSR = $0F02 -- VSW = 2, VDS = 15 -- i.e. 20
             * lines; the value is taken from the register, not from that sum, so a
             * game that reprograms its vertical timing moves the boundary with it.
             *
             * `field_end` is the first line after the sync pulse, i.e. the VCE's
             * `scanline == 0`; the reference's frame ends 9 lines before the field
             * does (14 + 240 = 254 of 263). */
            int vsw = (v->vsr & 0x1F) + 1;
            int vds = ((v->vsr >> 8) & 0xFF) + 2;
            int field_end = VD_LINES - (vsw + vds) + vsw;
            int frame_line = field_end - 9;
            if (frame_line <= 0 || frame_line >= VD_LINES)
                frame_line = VD_LINES - 23;
            if (v->line == frame_line)
                v->frame++;
        }

        {
        /* THE VERTICAL BLANK BEGINS WHERE THE DISPLAY WINDOW ENDS, and that is the
         * VDW register, not a constant.
         *
         * The reference core raises the vertical-blank interrupt at the first line
         * *after* the display window (huc6270/vdc.cpp: `NeedVBIRQTest` is armed on
         * entering VPHASE_VDW and consumed by DoVBIRQTest() at the first line where
         * `VPhase != VPHASE_VDW`), and the length of that window is VDW.  This model
         * used a fixed line 240 -- a number nothing measured.  Rondo programs
         * VDW = 224, so the blank (and with it VD, the vblank IRQ, the SAT DMA and
         * every `lda $0000 / and #$20` wait in the BIOS) was starting 16 lines late,
         * 7,280 cycles of frame phase at ~6% of a frame.  A wait that means "the
         * next vblank edge" costs a whole frame or none depending on which side of
         * the edge it starts on, so a phase error of this size flips frames at
         * exactly the game moments whose work straddles the boundary.
         *
         * VDW is clamped the same way the renderer clamps it: the display window is
         * rows 0..vdw-1, so the first blanked line is vdw. */
        /* THE DISPLAY WINDOW IS VDW+1 LINES, NOT VDW.
         *
         * The reference core sets `VPhaseCounter = VDW_cache + 1` on entering
         * VPHASE_VDW (huc6270/vdc.cpp, IncRCR), so with VDW = 224 the display
         * phase covers 225 lines and the blank -- VD, the vblank IRQ, the SAT DMA
         * and every `lda $0000` wait -- begins on the 226th.  This model treated
         * VDW as the line count, so all of that happened one line (455 cycles)
         * early.  The 225th line is off the bottom of the visible area, so this
         * moves no pixel. */
        int vbl_line = (int)(v->vdr & 0x1FF) + 1;
        if (vbl_line <= 1 || vbl_line >= VD_LINES)
            vbl_line = VD_VBLANK_LINE;
        if (v->line == vbl_line) {
            v->vblank = 1;
            if (v->sat_pending || (v->dcr & 0x10))
                vdc_sat_start(v);
            /* VD (bit 5) is "the vertical-blank interrupt occurred": it is set at
             * the same moment the vblank IRQ is raised (oracle: DoVBIRQTest sets
             * VDCS_VD under `CR & 0x08`), and cleared by a status read -- not by
             * the end of vblank.  The game's handler at $4186 tests exactly this
             * bit to decide whether there is a frame to draw. */
            /* Gate ONE of the two the video interrupt needs: CR bit 3 (`VD`/IE).
             * This is the chip side -- "the VDC may assert its IRQ1 line".  Gate
             * two is the CPU side, $1402 bit 1 clear (1 = disabled), checked where
             * the interrupt is delivered (runtime.c, xl_service_irq).  Modelling
             * only one of them is a real error in either direction: with CR bit 3
             * clear the line is never asserted, and with $1402 bit 1 set the line
             * is asserted but the CPU never takes it -- $1403 still reports it,
             * which is how a program that polls the VDC instead of taking the
             * interrupt can work (research §5.4, last bullet). */
            if (v->cr & VDC_VBL_IRQ) {
                v->status |= VDS_VD;
                v->irq_line = 1;
                v->vblank_irqs++;
            }
        }
        }
        /* THE SHADOW LATCHES AND THE RASTER COMPARE LIVE INSIDE THE LINE.
         *
         * The chip latches CR, BYR and BXR one VDC cycle apart at the start of the
         * line's active period (HDS, HDS_PART2, HDS_PART3 = dots 8, 9, 11 after the
         * end of horizontal blank; measured on hardware by dshadoff as "BXR, BYR,
         * CR shadowed in that order, one VDC clock apart, relative to the display
         * start minus the pipeline delay").  This model's dot 0 is the display
         * start, which is dot 32 of the chip's line, so those three points are
         * dots 431, 432 and 434 here -- 21 to 24 dots before the line they apply
         * to is drawn.
         *
         * The raster compare fires at HPHASE_HDW_FINAL, i.e. 4 dots before the end
         * of the active display (dot 284 of the chip's line = dot 252 here), not at
         * the start of the line.  That matters because the game's raster handler
         * writes BXR/BYR/RCR: with the compare at the line start every one of those
         * writes landed one scanline later than the console's, which put each of
         * the prologue's seven background strips one row low.
         *
         * RCRCount runs from 0 at the start of the display window and is reloaded
         * when the window begins again (the VDS->VDW transition at our line 262). */

        /* render the line we are about to display */
        {
            int vdw = v->vdr & 0x1FF;
            int row = v->line;
            if (vdw <= 0 || vdw > VD_H)
                vdw = VD_H;
            if (row >= 0 && row < vdw) {
                if (!v->vblank) {
                    vdc_render_line(v, vce, row, linebuf);
                } else {
                    /* INSIDE THE VISIBLE AREA BUT OUTSIDE THE DISPLAY WINDOW, the
                     * chip outputs its non-display pixel, which the oracle core
                     * fills with `pix = 0x100` (the same HPHASE_HDW block, the
                     * path taken when there is no pixel to copy).  This model
                     * emitted nothing and left the previous frame's pixels in the
                     * buffer, so a frame whose display window is short -- the
                     * first frames after a console reset, where VDR is still 0 --
                     * was a mosaic of the frame before it instead of a blank
                     * screen.  With VDR = 224 (every frame of the game) this arm
                     * is never reached. */
                    int x;
                    for (x = 0; x < VD_W; x++)
                        linebuf[x] = 0x100;
                }
                emit_line(v, vce, row, linebuf);
            }
        }
    }
}
