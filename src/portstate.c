/* portstate.c -- the port's side of the state envelope.
 *
 * WHAT THIS IS FOR.  "The graphics differ" is not a diagnosis.  A reference capture
 * rig turns a machine's complete observable state into one document per checkpoint,
 * hashes each subsystem separately, and names the first field that differs between
 * two of them.  This file makes the same document from THIS machine, in the same
 * field layout, so that two captures speak one language.
 *
 * It deliberately does NOT hash anything.  Hash encoding, the format tag, the manifest
 * fingerprint and the comparator all live in the capture rig -- one implementation,
 * used verbatim.  A second implementation of the format would be a second
 * definition of "the same state", and two definitions that drift apart look exactly
 * like a machine difference.  So this file emits *fields*, and the Python comparator
 * turns them into a capture with pce_state.make_capture().
 *
 * TWO THINGS IT MUST NOT DO:
 *   - it must not silently omit a field the provider has.  Every field the port cannot
 *     supply is listed in `unavailable` with a written reason, and the comparator
 *     refuses to run if a provider field is neither supplied nor declared;
 *   - it must not invent a value.  A field the port does not model is declared, never
 *     filled with a plausible constant.
 *
 * ALIGNMENT.  The two machines do not agree on frame numbers: our boot reaches the
 * game after its own disc reads and Geargrafx's BIOS paces its loads against host
 * time, so the difference is not a constant of the machines.  Captures are therefore
 * taken at a NAMED ANCHOR EVENT plus a delta -- not at a frame number.  The anchor is
 * "the first frame at which a chosen work-RAM byte is non-zero" (the $31
 * RUN/game-start latch), searched from a stated window, and the offset is
 * written into every capture so a reader can see what was assumed.
 *
 * Env (one getenv per variable, resolution happens on the first frame boundary):
 *   XLATE_PORTSTATE_DIR=DIR        write DIR/f<frame>.json, one capture per checkpoint
 *   XLATE_PORTSTATE_AT=0,60,240    capture at anchor+delta for each delta
 *   XLATE_PORTSTATE_ANCHOR=2031:FF anchor byte address:mask, high byte = mask
 *   XLATE_PORTSTATE_ANCHOR_FROM=n  first frame of the anchor search (default 0)
 *   XLATE_PORTSTATE_ANCHOR_TO=n    last frame of the search (default: the last capture)
 *   XLATE_PORTSTATE_STOP=1         stop the run after the last capture
 *   XLATE_PORTSTATE_POKE=a:v:f     TEST HOOK: store v at WRAM address a at the top of
 *                                  frame f, before capturing.  Same role as the
 *                                  oracle's --inject: it proves a planted byte is named.
 *
 * Cost when off: one branch per frame boundary (the same class as XLATE_FRAMELOG).
 */
#include "runtime.h"

#include <stdlib.h>
#include <string.h>

#define PS_MAX_AT 32
#define PS_DOTS_PER_FRAME 119665u          /* 455 dots x 263 lines, the envelope's clock */

static int      ps_init_done;              /* 0 until the env has been read */
static int      ps_on;                     /* a capture list was supplied */
static char     ps_dir[480];
static unsigned ps_at[PS_MAX_AT];
static int      ps_nat;
static int      ps_anchor_on;
static uint16_t ps_anchor_addr;
static uint8_t  ps_anchor_mask;
static unsigned ps_anchor_from;
static unsigned ps_anchor_to;
static int      ps_anchor_found;
static unsigned ps_anchor_frame;
static int      ps_stop_after;
static int      ps_done;
static int      ps_poke_on;
static int      ps_raster_line = -1;   /* sample at this line, not at the frame wrap */
static unsigned ps_raster_frame = 0xFFFFFFFFu;
static uint16_t ps_poke_addr;
static uint8_t  ps_poke_val;
static unsigned ps_poke_frame;

/* --------------------------------------------------------------------- helpers -- */

static void ps_json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\')
            fprintf(f, "\\%c", ch);
        else if (ch < 0x20)
            fprintf(f, "\\u%04x", ch);
        else
            fputc((int)ch, f);
    }
    fputc('"', f);
}

static void ps_json_bool(FILE *f, int v)
{
    fputs(v ? "true" : "false", f);
}

/* A blob is emitted in the envelope's own encoding: {"hex":"..."}.  pce_state._as_bytes
 * reads exactly this shape, and its hasher type-tags it as raw bytes, not as text. */
static void ps_json_hex(FILE *f, const uint8_t *d, size_t n)
{
    static const char hx[] = "0123456789abcdef";
    size_t i;
    fputs("{\"hex\":\"", f);
    for (i = 0; i < n; i++) {
        fputc(hx[d[i] >> 4], f);
        fputc(hx[d[i] & 0x0F], f);
    }
    fputs("\"}", f);
}

static uint8_t ps_wram(Cpu *c, uint16_t addr)
{
    return c->mem[(size_t)XL_WORKRAM_BANK * XL_BANK_SIZE + (size_t)(addr & 0x1FFF)];
}

/* ---------------------------------------------------------------- unavailability -- *
 * The fields this machine cannot answer for, with the reason.  This table is the
 * written declaration the comparator reads: anything not supplied and not listed here
 * is a hard error there, so a field can never disappear from a comparison silently.
 * The parenthesised evidence is what was checked when the entry was written. */
typedef struct { const char *field; const char *reason; } PsGap;

static const PsGap ps_gaps[] = {
    /* cpu */
    { "cpu.io",
      "the port has no $1400 I/O-register latch: a read of $1400/$1401 is open bus "
      "(runtime.c xl_rd). Geargrafx reports the register the input device holds, so "
      "there is nothing on this side to compare it with." },
    { "cpu.clock.line",
      "the two machines advance their frame counter at different raster positions "
      "(Geargrafx at line 256, this port at the line-0 wrap), so an absolute raster "
      "line is a property of the driver's sampling instant, not of the machine." },
    { "cpu.clock.dot",
      "same as cpu.clock.line: the dot within the line is the sampling instant. "
      "Measured on Geargrafx to vary between 0 and 15 across captures. Frame drift is "
      "caught by clock.frame and clock.dots_nominal instead." },
    /* vdc */
    { "vdc.vdc1.h_state",
      "the port models the raster as a line/dot counter, not as the chip's four-state "
      "horizontal sequencer (HDS/HDW/HDE/HSW)." },
    { "vdc.vdc1.v_state",
      "the port models the raster as a line/dot counter, not as the chip's four-state "
      "vertical sequencer (VDS/VDW/VCR/VSW)." },
    { "vdc.vdc1.hpos",
      "the chip's x position at the sampling instant; see cpu.clock.dot. The port's "
      "own equivalent is vdc.dot, which is not the same instant." },
    { "vdc.vdc1.vpos",
      "the chip's y position at the sampling instant; see cpu.clock.line." },
    { "vdc.vdc2.ar",   "this machine has one VDC. The port has no second chip, and "
                       "does not fabricate one." },
    { "vdc.vdc2.sr",   "this machine has one VDC (see vdc.vdc2.ar)." },
    { "vdc.vdc2.r00",  "this machine has one VDC (see vdc.vdc2.ar)." },
    { "vdc.vdc2.r01",  "this machine has one VDC (see vdc.vdc2.ar)." },
    { "vdc.vdc2.r02",  "this machine has one VDC (see vdc.vdc2.ar)." },
    { "vdc.vdc2.r03",  "this machine has one VDC (see vdc.vdc2.ar)." },
    { "vdc.vdc2.r04",  "this machine has one VDC (see vdc.vdc2.ar)." },
    { "vdc.vdc2.r05",  "this machine has one VDC (see vdc.vdc2.ar)." },
    { "vdc.vdc2.r06",  "this machine has one VDC (see vdc.vdc2.ar)." },
    { "vdc.vdc2.r07",  "this machine has one VDC (see vdc.vdc2.ar)." },
    { "vdc.vdc2.r08",  "this machine has one VDC (see vdc.vdc2.ar)." },
    { "vdc.vdc2.r09",  "this machine has one VDC (see vdc.vdc2.ar)." },
    { "vdc.vdc2.r10",  "this machine has one VDC (see vdc.vdc2.ar)." },
    { "vdc.vdc2.r11",  "this machine has one VDC (see vdc.vdc2.ar)." },
    { "vdc.vdc2.r12",  "this machine has one VDC (see vdc.vdc2.ar)." },
    { "vdc.vdc2.r13",  "this machine has one VDC (see vdc.vdc2.ar)." },
    { "vdc.vdc2.r14",  "this machine has one VDC (see vdc.vdc2.ar)." },
    { "vdc.vdc2.r15",  "this machine has one VDC (see vdc.vdc2.ar)." },
    { "vdc.vdc2.r16",  "this machine has one VDC (see vdc.vdc2.ar)." },
    { "vdc.vdc2.r17",  "this machine has one VDC (see vdc.vdc2.ar)." },
    { "vdc.vdc2.r18",  "this machine has one VDC (see vdc.vdc2.ar)." },
    { "vdc.vdc2.r19",  "this machine has one VDC (see vdc.vdc2.ar)." },
    /* vce */
    { "vce.ctrl.hsync",
      "the first line of the analogue signal, sampled at the frame boundary: it "
      "depends on where each machine's boundary sits in the raster, and the port has "
      "no hsync signal at all (it renders lines, it does not emit a video signal)." },
    { "vce.ctrl.vsync",  "see vce.ctrl.hsync." },
    { "vce.ctrl.speed",
      "derived inside Geargrafx from the VCE's own speed value. The underlying register "
      "(ctrl.control_reg) is compared directly, so this adds no state -- and the two "
      "derivations are not documented as identical." },
    /* psg: the port has no sound engine. */
    { "psg.ch0.amplitude", "the port models no PSG: no amplifier, no DAC, no wave "
                           "playback. Only the four whole-register fields are compared." },
    { "psg.ch0.dda",     "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch0.enabled", "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch0.frequency", "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch0.mute",    "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch0.noise",   "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch0.vol_left",  "the port models no PSG: the channel volume latches are "
                           "inside the chip and the port keeps no such state." },
    { "psg.ch0.vol_right", "the port models no PSG (see psg.ch0.vol_left)." },
    { "psg.ch0.wave_index", "the port models no PSG: it has no wave cursor." },
    { "psg.ch1.amplitude", "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch1.dda",     "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch1.enabled", "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch1.frequency", "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch1.mute",    "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch1.noise",   "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch1.vol_left",  "the port models no PSG (see psg.ch0.vol_left)." },
    { "psg.ch1.vol_right", "the port models no PSG (see psg.ch0.vol_left)." },
    { "psg.ch1.wave_index", "the port models no PSG (see psg.ch0.wave_index)." },
    { "psg.ch2.amplitude", "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch2.dda",     "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch2.enabled", "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch2.frequency", "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch2.mute",    "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch2.noise",   "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch2.vol_left",  "the port models no PSG (see psg.ch0.vol_left)." },
    { "psg.ch2.vol_right", "the port models no PSG (see psg.ch0.vol_left)." },
    { "psg.ch2.wave_index", "the port models no PSG (see psg.ch0.wave_index)." },
    { "psg.ch3.amplitude", "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch3.dda",     "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch3.enabled", "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch3.frequency", "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch3.mute",    "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch3.noise",   "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch3.vol_left",  "the port models no PSG (see psg.ch0.vol_left)." },
    { "psg.ch3.vol_right", "the port models no PSG (see psg.ch0.vol_left)." },
    { "psg.ch3.wave_index", "the port models no PSG (see psg.ch0.wave_index)." },
    { "psg.ch4.amplitude", "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch4.dda",     "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch4.enabled", "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch4.frequency", "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch4.mute",    "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch4.noise",   "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch4.vol_left",  "the port models no PSG (see psg.ch0.vol_left)." },
    { "psg.ch4.vol_right", "the port models no PSG (see psg.ch0.vol_left)." },
    { "psg.ch4.wave_index", "the port models no PSG (see psg.ch0.wave_index)." },
    { "psg.ch5.amplitude", "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch5.dda",     "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch5.enabled", "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch5.frequency", "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch5.mute",    "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch5.noise",   "the port models no PSG (see psg.ch0.amplitude)." },
    { "psg.ch5.vol_left",  "the port models no PSG (see psg.ch0.vol_left)." },
    { "psg.ch5.vol_right", "the port models no PSG (see psg.ch0.vol_left)." },
    { "psg.ch5.wave_index", "the port models no PSG (see psg.ch0.wave_index)." },
    /* cd */
    { "cd.scsi.sel",
      "the port keeps the SCSI conversation as a phase + REQ/ACK, not as nine bus "
      "lines: SEL/ATN/RST/MSG/CD/IO are not held as state (see runtime.h's phase table, "
      "where the $1800 byte is composed from the phase)." },
    { "cd.scsi.bsy",  "the port derives 'bus busy' from its phase byte; it is not a "
                      "separately held line (see cd.scsi.sel)." },
    { "cd.scsi.msg",  "not a held line in the port (see cd.scsi.sel)." },
    { "cd.scsi.io",   "not a held line in the port (see cd.scsi.sel)." },
    { "cd.scsi.cd",   "not a held line in the port (see cd.scsi.sel)." },
    { "cd.scsi.atn",  "not a held line in the port (see cd.scsi.sel)." },
    { "cd.scsi.rst",  "the port holds $1804 (the register the host writes) and performs "
                      "the reset; it does not hold the RST line separately." },
    { "cd.irq.active",
      "compared as its four named bits (irq.status/data/adpcm_end/adpcm_half) instead of "
      "as a raw byte: the port's $1803 register also carries bits 1 and 4 (the read-toggle "
      "and 'subchannel byte available'), which Geargrafx keeps in a different place. The "
      "same register is therefore compared bit for bit on the bits that name an IRQ." },
    { "cd.lba",
      "COORDINATE SYSTEMS DIFFER. The port is driven by the extracted data track (21 MB "
      "of MODE1 payload sectors) and addresses sectors inside it; Geargrafx reports a "
      "0-based disc LBA over the whole 49:12 disc. Comparing the two raw would compare "
      "two address spaces, not two machines." },
    { "cd.track",       "see cd.lba: the port has no TOC." },
    { "cd.track_lba",   "see cd.lba: the port has no TOC." },
    { "cd.track_start_lba", "see cd.lba: the port has no TOC." },
    { "cd.pos_msf",     "see cd.lba: the port has no TOC and no MSF coordinate." },
    { "cd.track_pos_msf", "see cd.lba: the port has no TOC." },
    { "cd.sector_count", "a property of the media image, not of the machine: the port is "
                         "given the data track, Geargrafx the whole disc (see cd.lba)." },
    { "cd.media_type",  "a property of the file given to each side (cue vs raw payload)." },
    { "cd.disc_length", "a TOC property; see cd.lba." },
    { "cd.tracks",      "a TOC property; the port has no TOC (see cd.lba)." },
    { "cd.next_event",
      "the provider's own event scheduler (a name from its internal state machine) has no "
      "counterpart in the port, which drives the same mechanism from master-clock "
      "deadlines (cd.next_fill / cd.drain_end)." },
    { "cd.cycles_to_event", "see cd.next_event." },
    { "cd.cycles_to_load",  "see cd.next_event." },
    { "cd.next_sector_load", "see cd.next_event." },
    { "cd.reset",       "the provider's internal reset latch; the port performs a reset "
                        "rather than holding a flag for it." },
    { "cd.fader",       "part of the provider's audio mixer; the port has no audio path." },
    { "cd.audio.state",           "the port has no CD-audio output: it models the "
                                  "readable state (cdda_status, position) but not the "
                                  "provider's audio mixer state." },
    { "cd.audio.output_state",    "see cd.audio.state." },
    { "cd.audio.audible",         "see cd.audio.state." },
    { "cd.audio.audio_sector",    "see cd.audio.state." },
    { "cd.audio.seek_cycles",     "see cd.audio.state." },
    { "cd.audio.seek_ms",         "see cd.audio.state." },
    { "cd.audio.playback_delay_cycles", "see cd.audio.state." },
    { "cd.audio.playback_delay_ms",     "see cd.audio.state." },
    { "cd.audio.start_lba",       "see cd.audio.state." },
    { "cd.audio.stop_lba",        "see cd.audio.state." },
    { "cd.audio.stop_event",      "see cd.audio.state." },
    { "cd.audio.left_sample",     "the port produces no PCM samples at all." },
    { "cd.audio.right_sample",    "the port produces no PCM samples at all." },
    { "cd.audio.fader_enabled",   "see cd.audio.state." },
    { "cd.audio.fader_raw",       "see cd.audio.state." },
    { "cd.audio.fader_gain",      "see cd.audio.state." },
    { "cd.audio.fader_speed",     "see cd.audio.state." },
    { "cd.audio.fader_target",    "see cd.audio.state." },
    { "cd.audio.frame_samples",   "see cd.audio.state." },
    { "cd.audio.samples_per_sector", "see cd.audio.state." },
    { "cd.adpcm.length",
      "the two models keep the same counter (samples still to play) in different "
      "initial states: Geargrafx starts at 65,536, the port at 0, and nothing in the "
      "corpus had started a sample before this comparison. Measured: 65536 vs 0 at "
      "every checkpoint of the baseline, 2026-10-30." },
    { "cd.adpcm.dma",     "the port decodes the $180E mode byte's direction but keeps no "
                          "separate DMA register; the DMA bit is a field of `mode`." },
    { "cd.adpcm.status_register",
      "the port composes $180C from its own state bits (end/half/playing/pending) at read "
      "time rather than storing a snapshot register." },
    { "cd.adpcm.frequency_khz",
      "derived by the provider from its clock and the sample-rate nibble. The nibble "
      "itself is not in the envelope, and the port's derivation is not the same formula." },
    { "cd.adpcm.frame_samples", "a provider clock artefact, not machine state." },
    { "cd.adpcm_ram",
      "the port models the ADPCM engine's registers and counters but not its 64 KiB "
      "sample RAM: nothing in the measured corpus reads those bytes back, so the port "
      "never kept them (runtime.h)." }
};

static int ps_is_gap(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof ps_gaps / sizeof ps_gaps[0]; i++)
        if (strcmp(ps_gaps[i].field, name) == 0)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------- the capture -- */

/* r00..r19 in the chip's own register order.  A register the port does not implement
 * is 0, which is what the chip reads back for the two unused slots ($03/$04).
 *
 * r19 IS REGISTER $13, AND REGISTER $13 IS ONE REGISTER.  The chip has one
 * sprite-table base (DVSSR): a write to register $13 stores it (video.c, the
 * REGSET on 0x13) and a SATB DMA takes its source word from it (video.c, `dvssr`).
 * This function used to report `v->satb`, a second copy of that register that
 * nothing on the chip's path reads or writes -- only the power-on seeding
 * (xl_video_init), the checkpoint image (xl_ckpt_save/xl_load_state_file) and the
 * cold reset (vdc_reset, reached through xl_boot_reset) touch it.  After a reset
 * with the card executed, `satb` therefore stays 0 while the chip holds $1000, and
 * every capture carried one permanent false difference.  Report the register the
 * chip uses. */
static void ps_vdc_regs(Cpu *c, uint16_t r[20])
{
    const Vdc *v = &c->vdc;
    r[0] = v->mawr; r[1] = v->marr; r[2] = v->vwr; r[3] = 0;     r[4] = 0;
    r[5] = v->cr;   r[6] = v->rcr;  r[7] = v->bxr; r[8] = v->byr; r[9] = v->mwr;
    r[10] = v->hsr; r[11] = v->hdr; r[12] = v->vsr; r[13] = v->vdr; r[14] = v->vcr;
    r[15] = v->dcr; r[16] = v->sour; r[17] = v->desr; r[18] = v->lenr; r[19] = v->dvssr;
}

static const char *ps_scsi_phase(uint8_t phase)
{
    switch (phase) {
    case CD_PHASE_BUS_FREE: return "BUS FREE";
    case CD_PHASE_COMMAND:  return "COMMAND";
    case CD_PHASE_DATA_IN:  return "DATA IN";
    case CD_PHASE_STATUS:   return "STATUS";
    case CD_PHASE_MESSAGE:  return "MESSAGE IN";
    default:                return "UNKNOWN";
    }
}

static void ps_write_capture(Cpu *c, unsigned frame)
{
    char path[600];
    FILE *f;
    uint16_t regs[20];
    unsigned i, aligned;

    snprintf(path, sizeof path, "%s/f%06u.json", ps_dir[0] ? ps_dir : ".", frame);
    f = fopen(path, "w");
    if (!f) {
        if (c->log) {
            fprintf(c->log, "portstate: cannot write %s\n", path);
            fflush(c->log);
        }
        return;
    }
    /* The aligned frame is the coordinate both machines share: frame minus the anchor.
     * -1 when no anchor was found, which the comparator treats as "not comparable". */
    aligned = ps_anchor_found ? (frame - ps_anchor_frame) : 0u;

    ps_vdc_regs(c, regs);

    fputs("{\n", f);
    fputs(" \"exporter\": \"rondorecomp/portstate/1\",\n", f);
    fprintf(f, " \"frame\": %u,\n", frame);
    if (ps_anchor_found)
        fprintf(f, " \"anchor\": %u,\n", ps_anchor_frame);
    else
        fputs(" \"anchor\": null,\n", f);
    fprintf(f, " \"aligned_frame\": %d,\n", ps_anchor_found ? (int)aligned : -1);
    fputs(" \"notes\": {", f);
    fprintf(f, " \"cycles\": %llu, \"pc\": %u, \"anchor_rule\": ",
            (unsigned long long)c->cycles, (unsigned)xl_rd(c, c->pc));
    if (ps_anchor_on)
        fprintf(f, "\"first frame where $%04X & $%02X is non-zero\"",
                (unsigned)ps_anchor_addr, (unsigned)ps_anchor_mask);
    else
        fputs("\"none\"", f);
    fputs("},\n", f);

    /* ---- fields ---- */
    fputs(" \"fields\": {\n", f);

    /* cpu */
    fputs("  \"cpu\": {\n", f);
    fprintf(f, "   \"pc\": %u,\n", (unsigned)c->pc);
    fprintf(f, "   \"a\": %u,\n", (unsigned)c->a);
    fprintf(f, "   \"x\": %u,\n", (unsigned)c->x);
    fprintf(f, "   \"y\": %u,\n", (unsigned)c->y);
    fprintf(f, "   \"s\": %u,\n", (unsigned)c->s);
    fprintf(f, "   \"p\": %u,\n", (unsigned)c->p);
    fprintf(f, "   \"sp_full\": %u,\n", (unsigned)(XL_STACK_BASE + (unsigned)c->s));
    fprintf(f, "   \"pc_physical\": %u,\n",
            (unsigned)(((unsigned)c->mpr[c->pc >> 13] << 13) | (unsigned)(c->pc & 0x1FFFu)));
    for (i = 0; i < 8; i++)
        fprintf(f, "   \"mpr%u\": %u,\n", i, (unsigned)c->mpr[i]);
    fputs("   \"io_sel\": ", f);  ps_json_bool(f, c->joypad_sel != 0);
    fputs(",\n   \"io_clr\": ", f); ps_json_bool(f, c->joypad_clr != 0);
    fprintf(f, ",\n   \"irr\": %u,\n", (unsigned)c->irq_status);
    fprintf(f, "   \"idr\": %u,\n", (unsigned)c->irq_mask);
    fputs("   \"irq1_enabled\": ", f);  ps_json_bool(f, (c->irq_mask & XL_IQ_IRQ1) == 0);
    fputs(",\n   \"irq1_asserted\": ", f); ps_json_bool(f, (c->irq_status & XL_IQ_IRQ1) != 0);
    fputs(",\n   \"irq2_enabled\": ", f);  ps_json_bool(f, (c->irq_mask & XL_IQ_IRQ2) == 0);
    fputs(",\n   \"irq2_asserted\": ", f); ps_json_bool(f, (c->irq_status & XL_IQ_IRQ2) != 0);
    fputs(",\n   \"timer_irq_enabled\": ", f);
    ps_json_bool(f, (c->irq_mask & XL_IQ_TIMER) == 0);
    fputs(",\n   \"timer_irq_asserted\": ", f);
    ps_json_bool(f, (c->irq_status & XL_IQ_TIMER) != 0);
    fprintf(f, ",\n   \"timer_counter\": %u,\n", (unsigned)c->timer.value);
    fprintf(f, "   \"timer_reload\": %u,\n", (unsigned)c->timer.load);
    fputs("   \"timer_flag\": ", f);
    ps_json_bool(f, (c->timer.status & 1u) != 0);
    /* THE CPU'S CLOCK MODE IS STATE (CSL/CSH, $54/$D4).  The provider reports
     * Geargrafx's own `SPEED` member as a string, and `c->speed` is the same
     * quantity with the same encoding (1 = 7.16 MHz, 0 = 1.79 MHz), so this is the
     * field the oracle is already asking for -- not a new one.  Omitting it was the
     * one hole through which "stuck in the wrong mode" could pass a state comparison
     * unnoticed, which is precisely the error class the speed census was written to
     * hunt. */
    fprintf(f, ",\n   \"clock_mode\": \"%u\"", (unsigned)(c->speed & 1u));
    fprintf(f, ",\n   \"clock.frame\": %u,\n", aligned);
    fprintf(f, "   \"clock.dots_nominal\": %lu\n",
            (unsigned long)aligned * (unsigned long)PS_DOTS_PER_FRAME);
    fputs("  },\n", f);

    /* wram */
    fputs("  \"wram\": {\n", f);
    fputs("   \"ram\": ", f);
    ps_json_hex(f, &c->mem[(size_t)XL_WORKRAM_BANK * XL_BANK_SIZE], XL_BANK_SIZE);
    fputs("\n  },\n", f);

    /* vdc */
    fputs("  \"vdc\": {\n", f);
    for (i = 0; i < 20; i++)
        fprintf(f, "   \"vdc1.r%02u\": %u,\n", i, (unsigned)regs[i]);
    fprintf(f, "   \"vdc1.ar\": %u,\n", (unsigned)c->vdc.select);
    fprintf(f, "   \"vdc1.sr\": %u,\n", (unsigned)(c->vdc.status & 0xFFu));
    fputs("   \"vdc1.bg_enabled\": ", f);
    ps_json_bool(f, (c->vdc.cr & 0x0080u) != 0);
    fputs(",\n   \"vdc1.sprites_enabled\": ", f);
    ps_json_bool(f, (c->vdc.cr & 0x0040u) != 0);
    fputs(",\n   \"vdc1.busy\": ", f);
    ps_json_bool(f, (c->vdc.status & VDS_BSY) != 0);
    fputs(",\n   \"vdc1.display_output\": ", f);
    {
        static const char *disp[] = { "DISP", "~BURST", "~INTHSYNC", "INVALID" };
        ps_json_string(f, disp[(c->vdc.cr >> 8) & 0x03u]);
    }
    fprintf(f, ",\n   \"vdc1.rw_increment\": %u,\n",
            (unsigned)(1u << (((c->vdc.cr >> 11) & 0x03u) * 5u)));
    fputs("   \"vdc1.int_collision\": ", f);
    ps_json_bool(f, (c->vdc.cr & 0x0001u) != 0);
    fputs(",\n   \"vdc1.int_overflow\": ", f);
    ps_json_bool(f, (c->vdc.cr & 0x0002u) != 0);
    fputs(",\n   \"vdc1.int_scanline\": ", f);
    ps_json_bool(f, (c->vdc.cr & VDC_RCR_IRQ) != 0);
    fputs(",\n   \"vdc1.int_vblank\": ", f);
    ps_json_bool(f, (c->vdc.cr & VDC_VBL_IRQ) != 0);
    fputs(",\n   \"vdc1.collision_detected\": ", f);
    ps_json_bool(f, (c->vdc.status & VDS_CR) != 0);
    fputs(",\n   \"vdc1.overflow_detected\": ", f);
    ps_json_bool(f, (c->vdc.status & VDS_OR) != 0);
    fputs(",\n   \"vdc1.scanline_interrupt\": ", f);
    ps_json_bool(f, (c->vdc.status & VDS_RR) != 0);
    fputs(",\n   \"vdc1.vblank_active\": ", f);
    ps_json_bool(f, (c->vdc.status & VDS_VD) != 0);
    fputs(",\n   \"vdc1.vram_dma_end\": ", f);
    ps_json_bool(f, (c->vdc.status & VDS_DV) != 0);
    fputs(",\n   \"vdc1.sat_dma_end\": ", f);
    ps_json_bool(f, (c->vdc.status & VDS_DS) != 0);
    fputs("\n  },\n", f);

    /* vram: 32,768 words as 65,536 little-endian bytes, the provider's own layout. */
    fputs("  \"vram\": {\n   \"data\": ", f);
    {
        static uint8_t buf[65536];
        for (i = 0; i < VD_VRAM_WORDS; i++) {
            buf[2 * i] = (uint8_t)(c->vdc.vram[i] & 0xFFu);
            buf[2 * i + 1] = (uint8_t)(c->vdc.vram[i] >> 8);
        }
        ps_json_hex(f, buf, sizeof buf);
    }
    fputs("\n  },\n", f);

    /* sat */
    fputs("  \"sat\": {\n   \"data\": ", f);
    {
        static uint8_t buf[VD_SAT_WORDS * 2];
        for (i = 0; i < VD_SAT_WORDS; i++) {
            buf[2 * i] = (uint8_t)(c->vdc.sat[i] & 0xFFu);
            buf[2 * i + 1] = (uint8_t)(c->vdc.sat[i] >> 8);
        }
        ps_json_hex(f, buf, sizeof buf);
    }
    fputs("\n  },\n", f);

    /* vce */
    fputs("  \"vce\": {\n   \"palette\": ", f);
    {
        static uint8_t buf[1024];
        for (i = 0; i < 512; i++) {
            buf[2 * i] = (uint8_t)(c->vce.pal[i] & 0xFFu);
            buf[2 * i + 1] = (uint8_t)(c->vce.pal[i] >> 8);
        }
        ps_json_hex(f, buf, sizeof buf);
    }
    fprintf(f, ",\n   \"ctrl.control_reg\": %u,\n", (unsigned)c->vce.cr);
    fprintf(f, "   \"ctrl.cta\": %u,\n", (unsigned)c->vce.cta);
    fputs("   \"ctrl.blur\": ", f);
    ps_json_bool(f, (c->vce.cr & 0x04u) != 0);
    fputs(",\n   \"ctrl.black_white\": ", f);
    ps_json_bool(f, (c->vce.cr & 0x80u) != 0);
    fprintf(f, ",\n   \"ctrl.lines\": %u\n", (c->vce.cr & 0x04u) ? 263u : 262u);
    fputs("  },\n", f);

    /* psg: only the four whole-register fields; see the gap table. */
    fputs("  \"psg\": {\n", f);
    fprintf(f, "   \"channel_select\": %u,\n", (unsigned)c->hw_regs[0x0800]);
    fprintf(f, "   \"main_amplitude\": %u,\n", (unsigned)c->hw_regs[0x0801]);
    fprintf(f, "   \"lfo_frequency\": %u,\n", (unsigned)c->hw_regs[0x0808]);
    fprintf(f, "   \"lfo_control\": %u\n", (unsigned)c->hw_regs[0x0809]);
    fputs("  },\n", f);

    /* cd */
    fputs("  \"cd\": {\n", f);
    fputs("   \"scsi.phase\": ", f);
    ps_json_string(f, ps_scsi_phase(c->cd.phase));
    fprintf(f, ",\n   \"scsi.data_bus\": %u,\n", (unsigned)c->cd.db);
    fputs("   \"scsi.req\": ", f);  ps_json_bool(f, c->cd.req != 0);
    fputs(",\n   \"scsi.ack\": ", f); ps_json_bool(f, c->cd.ack != 0);
    fprintf(f, ",\n   \"irq.enabled\": %u,\n", (unsigned)c->cd.irq_mask);
    fputs("   \"irq.status\": ", f);  ps_json_bool(f, (c->cd.port3 & 0x20u) != 0);
    fputs(",\n   \"irq.data\": ", f); ps_json_bool(f, (c->cd.port3 & 0x40u) != 0);
    fputs(",\n   \"irq.adpcm_end\": ", f); ps_json_bool(f, (c->cd.port3 & 0x08u) != 0);
    fputs(",\n   \"irq.adpcm_half\": ", f); ps_json_bool(f, (c->cd.port3 & 0x04u) != 0);
    fprintf(f, ",\n   \"sectors_left\": %u,\n", (unsigned)c->cd.sectors_left);
    fputs("   \"bram_enabled\": ", f); ps_json_bool(f, c->bram_wprot == 0);
    fputs(",\n   \"cdram\": ", f);
    ps_json_hex(f, &c->mem[(size_t)XL_CDRAM_BASE * XL_BANK_SIZE], 65536);
    fputs(",\n   \"adpcm.playing\": ", f); ps_json_bool(f, c->cd.adpcm_playing != 0);
    fprintf(f, ",\n   \"adpcm.address\": %u,\n", (unsigned)c->cd.adpcm_addr);
    fprintf(f, "   \"adpcm.read_address\": %u,\n", (unsigned)c->cd.adpcm_readaddr);
    fprintf(f, "   \"adpcm.write_address\": %u,\n", (unsigned)c->cd.adpcm_writeaddr);
    fprintf(f, "   \"adpcm.control\": %u\n", (unsigned)c->cd.adpcm_lastcmd);
    fputs("  }\n", f);
    fputs(" },\n", f);

    /* ---- what this machine cannot answer for ---- */
    fputs(" \"unavailable\": {\n", f);
    for (i = 0; i < sizeof ps_gaps / sizeof ps_gaps[0]; i++) {
        fputs("  ", f);
        ps_json_string(f, ps_gaps[i].field);
        fputs(": ", f);
        ps_json_string(f, ps_gaps[i].reason);
        fputs(i + 1 < sizeof ps_gaps / sizeof ps_gaps[0] ? ",\n" : "\n", f);
    }
    fputs(" }\n}\n", f);
    fclose(f);
    if (c->log) {
        fprintf(c->log, "portstate: frame %u (aligned %d) -> %s\n", frame,
                ps_anchor_found ? (int)aligned : -1, path);
        fflush(c->log);
    }
    printf("PORTSTATE frame=%u aligned=%d file=%s\n", frame,
           ps_anchor_found ? (int)aligned : -1, path);
    fflush(stdout);
}

/* ------------------------------------------------------------------------ hooks -- */

static void ps_init(Cpu *c)
{
    const char *dir = getenv("XLATE_PORTSTATE_DIR");
    const char *at = getenv("XLATE_PORTSTATE_AT");
    const char *anc = getenv("XLATE_PORTSTATE_ANCHOR");
    const char *from = getenv("XLATE_PORTSTATE_ANCHOR_FROM");
    const char *to = getenv("XLATE_PORTSTATE_ANCHOR_TO");
    const char *stop = getenv("XLATE_PORTSTATE_STOP");
    const char *poke = getenv("XLATE_PORTSTATE_POKE");
    const char *line = getenv("XLATE_PORTSTATE_LINE");

    ps_init_done = 1;
    if (line && line[0])
        ps_raster_line = atoi(line);
    if (!dir || !dir[0] || !at || !at[0])
        return;
    snprintf(ps_dir, sizeof ps_dir, "%s", dir);
    {
        char tmp[256];
        char *p;
        snprintf(tmp, sizeof tmp, "%s", at);
        for (p = strtok(tmp, ","); p && ps_nat < PS_MAX_AT; p = strtok(NULL, ","))
            ps_at[ps_nat++] = (unsigned)strtoul(p, NULL, 10);
    }
    if (ps_nat == 0)
        return;
    ps_on = 1;
    if (anc && anc[0]) {
        const char *colon = strchr(anc, ':');
        if (colon) {
            ps_anchor_on = 1;
            ps_anchor_addr = (uint16_t)strtoul(anc, NULL, 16);
            ps_anchor_mask = (uint8_t)strtoul(colon + 1, NULL, 16);
        }
    }
    ps_anchor_from = from ? (unsigned)strtoul(from, NULL, 10) : 0u;
    ps_anchor_to = to ? (unsigned)strtoul(to, NULL, 10) : 0xFFFFFFFFu;
    ps_stop_after = (stop && stop[0] && stop[0] != '0') ? 1 : 0;
    if (poke && poke[0]) {
        unsigned a = 0, v = 0, fr = 0;
        if (sscanf(poke, "%x:%x:%u", &a, &v, &fr) == 3) {
            ps_poke_on = 1;
            ps_poke_addr = (uint16_t)a;
            ps_poke_val = (uint8_t)v;
            ps_poke_frame = fr;
        }
    }
    if (c->log) {
        fprintf(c->log, "portstate: on, %d checkpoint delta(s), anchor %s, "
                        "stop-after-last %d\n", ps_nat,
                ps_anchor_on ? "$%04X latch" : "none (frames are absolute)",
                ps_stop_after);
        fflush(c->log);
    }
}

int xl_portstate_done(void)
{
    return ps_done;
}

/* Called once per frame boundary, at the same instant XLATE_FRAMELOG samples: the
 * first instruction boundary after the frame counter changed, which is the only
 * instant that is both "start of frame N" and safe to snapshot. */
static void ps_sample(Cpu *c, unsigned frame)
{
    int i;

    if (ps_anchor_on && !ps_anchor_found) {
        if (frame >= ps_anchor_from && frame <= ps_anchor_to &&
            (uint8_t)(ps_wram(c, ps_anchor_addr) & ps_anchor_mask) != 0) {
            ps_anchor_found = 1;
            ps_anchor_frame = frame;
            if (c->log) {
                fprintf(c->log, "portstate: anchor found at frame %u "
                                "($%04X & $%02X != 0)\n", frame,
                        (unsigned)ps_anchor_addr, (unsigned)ps_anchor_mask);
                fflush(c->log);
            }
        }
    }

    /* Test hook: the same role as the oracle's --inject.  Deliberately before the
     * capture, so the planted byte is in the capture the comparator reads. */
    if (ps_poke_on && frame == ps_poke_frame)
        xl_wr(c, ps_poke_addr, ps_poke_val);

    if (ps_anchor_on && !ps_anchor_found)
        return;

    for (i = 0; i < ps_nat; i++) {
        unsigned want = (ps_anchor_on ? ps_anchor_frame : 0u) + ps_at[i];
        if (frame != want)
            continue;
        ps_write_capture(c, frame);
        if (ps_stop_after && ps_at[i] == ps_at[ps_nat - 1])
            ps_done = 1;
    }
}

/* The frame-wrap hook (called once per frame boundary, as before).  With a raster
 * sampling line configured there is nothing to do here: the sample is taken inside
 * the frame instead, at the same raster position the provider samples at. */
void xl_portstate_maybe(Cpu *c)
{
    if (!ps_init_done)
        ps_init(c);
    if (!ps_on || ps_done)
        return;
    if (ps_raster_line >= 0)
        return;
    /* A search window that has run out is a refusal, not a reason to emulate for
     * another eleven minutes: stop, and let the caller say the anchor was not found. */
    if (ps_anchor_on && !ps_anchor_found && (unsigned)c->vdc.frame > ps_anchor_to) {
        ps_done = 1;
        if (c->log) {
            fprintf(c->log, "portstate: anchor not found in frames %u..%u; stopping\n",
                    ps_anchor_from, ps_anchor_to);
            fflush(c->log);
        }
        return;
    }
    ps_sample(c, (unsigned)c->vdc.frame);
}

/* THE SAMPLING INSTANT IS PART OF THE EXPERIMENT.
 *
 * The two machines do not increment their frame counter at the same point in the
 * raster: Geargrafx reports its frame boundary at line 256 (measured on every
 * capture it has taken), this port counts frames at the line-0 wrap.  Seven raster
 * lines, about 3,185 CPU cycles, is enough for the CPU to be several instructions
 * further on -- so a comparison at "the frame boundary" is comparing two different
 * instants in the frame and reports that difference as a machine difference.  That
 * is precisely the class of mistake this tool exists to avoid, so the sample moves
 * to the line the provider uses.  Called from xl_tick, on the instruction boundary
 * after each clock advance; costs one branch when no sample is being taken. */
void xl_portstate_raster(Cpu *c)
{
    if (!ps_init_done)
        ps_init(c);
    if (!ps_on || ps_done || ps_raster_line < 0)
        return;
    if (ps_anchor_on && !ps_anchor_found && (unsigned)c->vdc.frame > ps_anchor_to) {
        ps_done = 1;
        return;
    }
    if (c->vdc.line < ps_raster_line)
        return;
    if (ps_raster_frame == (unsigned)c->vdc.frame)
        return;                                  /* once per frame */
    ps_raster_frame = (unsigned)c->vdc.frame;
    ps_sample(c, ps_raster_frame);
}

/* Cheap gate for the hot path: 0 until a capture list is configured. */
int xl_portstate_on(void)
{
    return ps_on && !ps_done;
}

/* Report-only: the declaration table, for `--manifest`-style output.  The comparator
 * reads the same list out of each capture, so this exists to make it visible in the
 * port's own terminal as well. */
void xl_portstate_manifest(FILE *out)
{
    size_t i;
    fprintf(out, "PORTSTATE_GAPS n=%lu\n",
            (unsigned long)(sizeof ps_gaps / sizeof ps_gaps[0]));
    for (i = 0; i < sizeof ps_gaps / sizeof ps_gaps[0]; i++)
        fprintf(out, "  %s\n", ps_gaps[i].field);
    (void)ps_is_gap;
}
