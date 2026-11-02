/* video.h -- VDC (HuC6270) + VCE (colour) device model and frame renderer.
 *
 * The point is not to emulate the chips cycle-exactly; it is to let *translated
 * game code* drive a picture, and to render that picture the way the hardware
 * would. So:
 *
 *   - the register interface is faithful (address/data ports with the LSB/MSB
 *     latch, the memory-increment modes, SAT DMA, VRAM-to-VRAM DMA, status bits);
 *   - the raster advances with CPU time, line by line, so a register written
 *     during vblank or mid-frame takes effect on the right line;
 *   - the renderer follows the measured format, not a guess.
 *
 * Measured format facts, verified in tests/test_video.py:
 *   - background map = VRAM word 0; row stride = bat_width from MWR bits 4-5
 *     (32 / 64 / 128 cells -- this game sets MWR=$50 -> 64, and the visible
 *     32 cells are the first half of each row);
 *   - a BAT word's bits 0-11 are a VRAM *word address* in units of 16 words
 *     (tile bytes start at (word & $FFF0) * 2), bits 12-15 pick one of 16
 *     background palettes;
 *   - an 8x8 4bpp tile is 32 bytes = 16 words: word[y] holds bitplanes 0/1 for
 *     row y (low byte = plane 0, high byte = plane 1), word[y+8] holds 2/3.
 *     Within a byte, bit 0 is the *rightmost* pixel;
 *   - sprite attribute table = 64 entries of 4 words (y, x, pattern, attributes)
 *     copied out of VRAM from SATB; y and x carry -0x40 / -0x20 offsets;
 *   - a 16x16 sprite pattern is 64 words, laid out as four 16-row bitplanes:
 *     word[n*64 + row] = plane 0 across 16 pixels, +16 = plane 1, +32 = 2,
 *     +48 = 3;
 *   - VCE palette word = ----GGGRRRBBB (red 3-5, green 6-8, blue 0-2).
 */
#ifndef XLATE_VIDEO_H
#define XLATE_VIDEO_H

#include <stdint.h>

#define VD_VRAM_WORDS 32768
#define VD_SAT_WORDS  256            /* 64 sprites x 4 words */
#define VD_W          256
#define VD_H          224
#define VD_LINES      263            /* lines per frame */
#define VD_DOTS       455            /* dot clocks per line */
#define VD_VBLANK_LINE 240           /* first line of vblank */

#define VD_SPR_MAX_PER_LINE 16

/* VDC status register bits, numbered as the hardware (and as the oracle core,
 * huc6270/vdc.cpp: VDCS_CR/OR/RR/DS/DV/VD).  This numbering was wrong in this
 * model: bit 5 was labelled "sprite DMA complete" when the hardware's bit 5 is
 * VD, "the vertical-blank interrupt occurred", and bit 3 is DS, the VRAM->SAT
 * DMA completion.  The game's own vblank handler branches on bit 5
 * (`bbs5 $3A` at $4191, kernel bank $69), so with bit 5 never set our machine
 * took the "nothing to do" path every frame and never started a sprite DMA --
 * a self-perpetuating deadlock that parked the booted machine at $4174. */
#define VDS_CR 0x01      /* sprite #0 collision */
#define VDS_OR 0x02      /* sprite overflow */
#define VDS_RR 0x04      /* raster (RCR) match */
#define VDS_DS 0x08      /* VRAM -> SAT (sprite) DMA complete */
#define VDS_DV 0x10      /* VRAM -> VRAM DMA complete */
#define VDS_VD 0x20      /* vertical blank occurred */
#define VDS_BSY 0x40     /* VRAM busy (64-word DMA in progress) */

/* Control register bits */
#define VDC_BG_ON   0x80
#define VDC_SPR_ON  0x40
#define VDC_RCR_IRQ 0x04
#define VDC_VBL_IRQ 0x08

typedef struct Vdc {
    uint16_t vram[VD_VRAM_WORDS];
    uint16_t sat[VD_SAT_WORDS];

    uint16_t select, mawr, marr, vwr;
    uint16_t read_buffer;            /* VRR prefetch buffer (one whole word) */
    uint16_t vram_latch;             /* retained VRAM bus value (see video.c) */
    uint16_t cr, rcr, bxr, byr, mwr, hsr, hdr, vsr, vdr, vcr, dcr;
    /* SHADOWS THE PICTURE IS ACTUALLY DRAWN FROM (see vdc_tick).
     *
     * The chip does not draw a line from the registers as they stand when the
     * line begins: it latches them into internal copies at the display start,
     * one VDC cycle apart, and the pixel pipeline uses those.  Measured on
     * hardware by dshadoff (MiSTer PCE work): BXR, BYR and CR are shadowed in
     * that order, one VDC clock apart, relative to the display start minus the
     * pipeline delay.  The reference core has the same three points -- CR at
     * HPHASE_HDS, BYR at HDS_PART2, BXR at HDS_PART3 (huc6270/vdc.cpp) -- which
     * is 8/9/11 VDC dots after the start of the line's active period.
     *
     * A register written after its shadow point therefore shows up on the NEXT
     * line, and one written before it on THIS line.  Drawing from the live
     * register instead moves that boundary by up to 24 dots, which is enough to
     * put a mid-frame scroll write on the wrong side of a line -- and this game
     * rewrites BXR/BYR seven times a frame to lay down horizontal strips. */
    uint16_t cr_sh, bxr_sh, byr_sh;
    uint16_t rcr_count;              /* display lines since the VDW window began */
    uint16_t sour, desr, lenr, satb, dvssr;
    uint16_t status;
    uint8_t  msb;                    /* 0 = next data byte is low, 1 = high */
    uint8_t  sat_pending;
    uint8_t  dma_pending;
    /* The sprite-attribute transfer in flight, in VDC clocks.  1024 at the start,
     * counted down one per clock, one word landed every fourth -- Geargrafx's
     * `m_sat_transfer_pending` (huc6270.cpp SATTransfer/Clock).  Zero means no
     * transfer is running.  See vdc_sat_step(). */
    uint16_t sat_clock;

    /* ---- VRAM ACCESS WAIT STATES (see video.c) -------------------------
     *
     * The chip has one VRAM port.  A CPU write to the data port does not land in
     * VRAM when it is issued: the word is queued and committed at the chip's next
     * access opportunity, and while the queue is still full the CPU is stalled
     * (status bit 6, BSY).  This model had none of that, so every VRAM write cost
     * exactly its instruction's cycles.
     *
     * `ws_master` is the position in the line in MASTER clocks (1365 per line =
     * 455 CPU cycles) measured from the hsync edge; `ws_phase`/`ws_hp_left` are
     * the chip's horizontal phase machine, `ws_mystery` the closed window, and
     * `pending_*` the queued word. */
    int      ws_master;
    int      ws_hp_left;
    int      ws_mystery;
    uint8_t  ws_phase;
    uint8_t  ws_blocked;
    uint8_t  ws_hblank;
    uint8_t  pending_write;
    uint16_t pending_waddr;
    uint16_t pending_wval;
    /* THE READ PORT QUEUES TOO, and that is not a detail: the reference's
     * `VDC_IS_BSY` is `pending_read || pending_write`, and a data-port READ calls
     * DoWaitStates() exactly as a write does (huc6270/vdc.cpp:1108-1145).  A
     * read-modify-write loop over the data port (this game's VRAM block copy at
     * $6C:$8114 is one) therefore stalls twice per iteration, and the second
     * stall only exists because the read left the port busy. */
    uint8_t  pending_read;
    uint16_t pending_raddr;

    /* raster */
    int      line, dot, frame;
    uint8_t  vblank;
    uint16_t bg_y;                   /* running BG vertical offset (BG_YMoo) */
    int      irq_line;               /* 1 while a VDC IRQ source is asserted */

    /* observation */
    uint32_t writes, reads, dma_runs, sat_dma_runs, vblank_irqs, rcr_irqs;
    uint32_t reg_writes[32];         /* per VDC register, for diagnosis */
    uint8_t  frame_pixels[VD_W * VD_H * 3];
} Vdc;

typedef struct Vce {
    uint16_t pal[512];
    /* THE DISPLAYED COLOUR IS NOT palette[i].
     *
     * VCE::FixPCache (pce/vce.cpp) maintains a separate cache, and a write only
     * lands in it when the entry's low nibble is non-zero:
     *
     *     if(!(entry & 0xFF))  for(x) cache[(entry & 0x100) | (x << 4)] = color_table[entry & 0x100];
     *     if(!(entry & 0xF))   return;                 // low nibble 0: never written
     *     cache[entry] = color_table[entry];
     *
     * So an entry whose index ends in 0 (other than 0x000/0x100) shows whatever
     * the last low-byte-0 write put there, and that is 0 -- black -- until one
     * happens.  Rondo's prologue draws its background with palette 1 colour 0
     * (index $10) where the picture must be black: reading palette[$10] straight
     * from the table painted a dark purple band across the whole level.  The
     * STATUS entry "the palette's FixPCache rule, worth ~30,000 px/frame if
     * wrong" is this rule; it was implemented in the trace tool and not here. */
    uint16_t cache[512];
    uint16_t cta;
    uint8_t  cr;
    uint8_t  hi_latch;
    uint32_t writes;
} Vce;

/* --- devices ------------------------------------------------------------- */
void    vdc_reset(Vdc *v);
void    vdc_write(Vdc *v, uint16_t off, uint8_t val);
uint8_t vdc_read(Vdc *v, uint16_t off);
void    vdc_write_vce(Vce *v, uint16_t off, uint8_t val);
uint8_t vdc_read_vce(Vce *v, uint16_t off);
/* Rebuild the whole display cache from `pal` (VCE::FixPCache over 0..511).  Called
 * after the palette is loaded from an injected state, which writes `pal` wholesale. */
void    vce_fix_all(Vce *v);
/* The chip's power-on state: the checkerboard colour table of VCE::Reset with the
 * display cache built from it (entry 0 white).  Not the same as a zeroed VCE --
 * see the note on the implementation. */
void    vce_reset(Vce *v);

/* Advance the raster by `cycles` CPU cycles.  Renders finished lines into
 * frame_pixels and raises IRQ sources as lines go by. */
void    vdc_tick(Vdc *v, Vce *vce, uint32_t cycles);

/* CPU cycles a VDC data-port access must wait before the chip will take it:
 * non-zero only while a queued word is still waiting for the port (the chip's
 * BSY state).  Charged by the caller, which owns the clock. */
unsigned vdc_vram_wait(const Vdc *v);

/* Render the whole visible area from the *current* register/VRAM state, without
 * advancing time.  Used by the video tests (which feed in a captured state). */
void    vdc_render_frame(Vdc *v, Vce *vce);
/* Draw row 0 from the current state -- the line a power-on raster has just begun,
 * which vdc_tick would otherwise never draw (see the implementation). */
void    vdc_render_top_line(Vdc *v, Vce *vce);

/* Compiled-out-ish helpers used by the renderer and the tests. */
void    vdc_render_line(Vdc *v, Vce *vce, int out_row, uint16_t *linebuf);
void    vce_rgb(const Vce *v, unsigned index, uint8_t rgb[3]);

/* Scanline counter for the CPU side: frame-relative line the raster is on. */
static inline int vdc_line(const Vdc *v) { return v->line; }

#endif /* XLATE_VIDEO_H */
