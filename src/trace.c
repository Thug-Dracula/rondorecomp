/* trace.c -- this machine's own rce-trace v1 emitter.
 *
 * WHY THIS EXISTS.  A reference emulator's tracer writes one record per I/O-page
 * access, in one format -- rce-trace v1 plus the optional `c=` master-clock field.
 * Emitting the same records here turns "is our machine doing what the reference
 * did" into a diff between two text files instead of a tool per question:
 *
 *   trace_cmp.py ours.log reference.log
 *
 * SWITCHES (all off by default; nothing below runs unless XLATE_TRACE_OUT is set):
 *
 *   XLATE_TRACE_OUT=PATH       write the trace here (truncated at start)
 *   XLATE_TRACE_FROM=<frame>   first frame to record (default 0)
 *   XLATE_TRACE_TO=<frame>     last frame to record (default: end of run)
 *   XLATE_TRACE_REGIONS=...    replace the region mask (vdc,vce,psg,timer,joy,irq,cd,exp)
 *   XLATE_TRACE_ALL=1          record every region
 *   XLATE_TRACE_CYCLE=0        omit `c=` (byte-comparable with a v1 trace)
 *   XLATE_TRACE_COLLAPSE=0     one line per access, no rep=/cyc= collapsing
 *
 * The default region mask is vdc,vce,timer,joy,irq,cd: the regions a reference
 * trace carries, so a diff cannot come from a filtering difference.  PSG and EXP
 * are opt-in because a reference trace does not contain them.
 *
 * CONVENTIONS THAT MATTER, each measured from a reference trace rather than taken
 * from the format description alone:
 *
 *  - `pc` is the instruction's own address.  The console records `HuCPU.TracePC()`,
 *    which is that, and our c->pc holds the same thing while the instruction runs.
 *  - `pa` is always the I/O page: 0x1FE000 | off.
 *  - `la` is the address the CPU used: the logical address inside the window that
 *    mapped bank $FF for an ordinary access, and 0/2/3 for ST0/ST1/ST2 -- those
 *    instructions address the page directly and the console records their operand
 *    as the logical address (`t=W pc=5B6B pa=1FE000 la=0000 ...`, frame 7591).
 *  - VDC data-port reads ($0002/$0003) are dropped.  The console's recorder drops
 *    them (`want_log` in pce_trace.cpp); keeping them here would manufacture a
 *    divergence on every upload.
 *  - `c` is 3 * c->cycles, i.e. master clocks (one CPU cycle is three master
 *    clocks, one 263-line frame is 358,995).  It is stamped at the same
 *    point the console stamps: the instruction boundary the access was made on,
 *    before that instruction's own cycles are added.
 *  - `n` counts records the way the console's `trace_seq` does: one per recorded
 *    access and one per FRAME/IRQ/IRQE/CDCMD event.
 *
 * COLLAPSING.  Same algorithm as the console's recorder (a window of up to 2*8
 * records; when the last 2p are identical, the last p are a cycle and everything
 * before them is emitted verbatim).  Two runs of the same code may still collapse
 * differently -- that is why the comparator expands before comparing -- but matching
 * the algorithm keeps our files the same size and shape as theirs, which is what
 * makes them readable side by side.  A cycle's `rep`/`cyc` group carries the cycle
 * of its first repetition; the records after a broken cycle carry the cycle of the
 * last repetition that really happened (`clast`), as the format requires.
 */
#include "runtime.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* Region index from the offset inside the I/O page: the same 1 KB split the
 * format uses ($0000 vdc, $0400 vce, $0800 psg, $0C00 timer, $1000 joy,
 * $1400 irq, $1800 cd, $1C00 exp). */
enum { TR_VDC, TR_VCE, TR_PSG, TR_TIMER, TR_JOY, TR_IRQ, TR_CD, TR_EXP };

static const char *const tr_region_name[8] = {
    "vdc", "vce", "psg", "timer", "joy", "irq", "cd", "exp"
};

#define TR_CWMAX 8              /* longest cycle the recorder collapses, in records */

typedef struct {
    uint16_t off;               /* offset in the I/O page */
    uint16_t la;                /* logical address the CPU used */
    uint16_t pc;                /* instruction address */
    uint8_t  v;                 /* byte read / written */
    uint8_t  wr;                /* 1 = write */
    uint8_t  reg;               /* region index */
    uint64_t c;                 /* master clocks at the access */
} TrRec;

static FILE    *tr_fp;
static int      tr_on = -1;     /* -1: not resolved yet, 0: off, 1: recording */
static long     tr_from = 0;
static long     tr_to = -1;
static unsigned tr_mask = (1u << TR_VDC) | (1u << TR_VCE) | (1u << TR_TIMER) |
                          (1u << TR_JOY) | (1u << TR_IRQ) | (1u << TR_CD);
static int      tr_cycle = 1;   /* emit the `c=` field */
static int      tr_collapse = 1;
static uint64_t tr_seq;         /* `n=`: records the console's trace_seq counts */
static long     tr_frame = -1;  /* frame the lines being written belong to */
static int      tr_announced;

static TrRec    cw[2 * TR_CWMAX];   /* pending window (cycle search) */
static TrRec    clast[TR_CWMAX];    /* newest repetition of the current cycle */
static int      cwlen, cperiod, cpos;
static unsigned creps;

static char     obuf[1 << 16];
static size_t   olen;
static uint64_t tr_bytes, tr_lines, tr_collapsed;

/* ------------------------------------------------------------------ output -- */

static void tr_flush(void)
{
    if (tr_fp && olen) {
        if (fwrite(obuf, 1, olen, tr_fp) == olen)
            tr_bytes += olen;
        olen = 0;
        fflush(tr_fp);          /* a killed run leaves a usable trace */
    }
}

static void tr_write(const char *s, size_t n)
{
    if (n >= sizeof obuf)   /* cannot happen with the lines below; be exact anyway */
        return;
    if (olen + n > sizeof obuf)
        tr_flush();
    memcpy(obuf + olen, s, n);
    olen += n;
}

static void tr_line(const char *fmt, ...)
{
    char line[192];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if ((size_t)n >= sizeof line)
        n = (int)sizeof line - 1;
    tr_write(line, (size_t)n);
    tr_lines++;
}

static uint64_t tr_now(const Cpu *c) { return c->cycles * 3u; }

/* The console records a bus access with the CPU's PC *after* the instruction's
 * operand bytes were consumed -- `HuCPU.TracePC()` is Mednafen's PC register, and
 * `ST_AB`/`GET_AB` advance it before the write handler runs.  So its `pc=` is the
 * instruction's start + its width, and `t=W pc=5B6B` is the port's `sta $0000` at
 * $5B68.  This reproduces that exactly: the width comes from the opcode byte at the
 * instruction's start, read through the live mapping with no device side effects
 * (an opcode fetch is a plain memory read; it must not touch a register).
 *
 * The FRAME/IRQ/IRQE/CDCMD markers do NOT get this treatment: there the console's PC
 * is the address of the next instruction, which is what c->pc already holds. */
static uint16_t tr_pc_bus(const Cpu *c)
{
    uint8_t bank = c->mpr[c->pc >> 13];
    uint8_t op;

    if (bank == XL_IO_BANK || bank == XL_BRAM_BANK)
        return c->pc;
    op = c->mem[(size_t)bank * XL_BANK_SIZE + (c->pc & 0x1FFF)];
    return (uint16_t)(c->pc + xl_op_size(op));
}

static void tr_mpr(char out[17], const Cpu *c)
{
    int i;
    for (i = 0; i < 8; i++) {
        static const char hx[] = "0123456789ABCDEF";
        out[i * 2] = hx[(c->mpr[i] >> 4) & 0xF];
        out[i * 2 + 1] = hx[c->mpr[i] & 0xF];
    }
    out[16] = '\0';
}

/* --------------------------------------------------------------- collapsing -- */

static int tr_same(const TrRec *a, const TrRec *b)
{
    return a->off == b->off && a->la == b->la && a->pc == b->pc &&
           a->v == b->v && a->wr == b->wr && a->reg == b->reg;
}

static void tr_emit_rec(const TrRec *r, unsigned rep, unsigned cyc)
{
    if (tr_cycle) {
        if (rep > 1)
            tr_line("t=%c c=%010llX f=%ld pc=%04X pa=%06X la=%04X off=%04X r=%s "
                    "v=%02X rep=%u cyc=%u\n",
                    r->wr ? 'W' : 'R', (unsigned long long)r->c, tr_frame,
                    (unsigned)r->pc, 0x1FE000u | (unsigned)r->off, (unsigned)r->la,
                    (unsigned)r->off, tr_region_name[r->reg], (unsigned)r->v, rep, cyc);
        else
            tr_line("t=%c c=%010llX f=%ld pc=%04X pa=%06X la=%04X off=%04X r=%s "
                    "v=%02X\n",
                    r->wr ? 'W' : 'R', (unsigned long long)r->c, tr_frame,
                    (unsigned)r->pc, 0x1FE000u | (unsigned)r->off, (unsigned)r->la,
                    (unsigned)r->off, tr_region_name[r->reg], (unsigned)r->v);
    } else {
        if (rep > 1)
            tr_line("t=%c f=%ld pc=%04X pa=%06X la=%04X off=%04X r=%s v=%02X "
                    "rep=%u cyc=%u\n",
                    r->wr ? 'W' : 'R', tr_frame, (unsigned)r->pc,
                    0x1FE000u | (unsigned)r->off, (unsigned)r->la, (unsigned)r->off,
                    tr_region_name[r->reg], (unsigned)r->v, rep, cyc);
        else
            tr_line("t=%c f=%ld pc=%04X pa=%06X la=%04X off=%04X r=%s v=%02X\n",
                    r->wr ? 'W' : 'R', tr_frame, (unsigned)r->pc,
                    0x1FE000u | (unsigned)r->off, (unsigned)r->la, (unsigned)r->off,
                    tr_region_name[r->reg], (unsigned)r->v);
    }
    tr_seq++;
}

/* Emit everything pending.  Called at every frame boundary and before every event
 * that is not a bus access, so a group is never split across records of another
 * kind -- the same reason the console's recorder does it. */
static void tr_emit_pending(void)
{
    int i;
    if (cperiod) {
        for (i = 0; i < cperiod; i++)
            tr_emit_rec(&cw[i], creps, (unsigned)cperiod);
        /* Whatever was seen of the next (incomplete) cycle happened for real, in
         * the LAST repetition, so it carries that repetition's cycle number. */
        for (i = 0; i < cpos; i++)
            tr_emit_rec(&clast[i], 1, 1);
        if (creps > 1)
            tr_collapsed += (uint64_t)cperiod * (uint64_t)(creps - 1);
    } else {
        for (i = 0; i < cwlen; i++)
            tr_emit_rec(&cw[i], 1, 1);
    }
    cperiod = 0;
    cpos = 0;
    creps = 0;
    cwlen = 0;
}

static void tr_push(const TrRec *r)
{
    int p, i;

    if (!tr_collapse) {
        tr_emit_rec(r, 1, 1);
        return;
    }

    if (cperiod) {
        if (tr_same(&cw[cpos], r)) {
            clast[cpos] = *r;
            cpos++;
            if (cpos == cperiod) {
                cpos = 0;
                creps++;
            }
            return;
        }
        tr_emit_pending();
    }

    if (cwlen == 2 * TR_CWMAX) {
        TrRec t = cw[0];
        tr_emit_rec(&t, 1, 1);
        memmove(&cw[0], &cw[1], (size_t)(cwlen - 1) * sizeof cw[0]);
        cwlen--;
    }
    cw[cwlen++] = *r;

    for (p = 1; p <= TR_CWMAX; p++) {
        int match = 1;
        if (cwlen < 2 * p)
            break;
        for (i = 0; i < p; i++) {
            if (!tr_same(&cw[cwlen - 2 * p + i], &cw[cwlen - p + i])) {
                match = 0;
                break;
            }
        }
        if (!match)
            continue;
        /* Anything before the two identical cycles is a real access that is not
         * part of the cycle: emit it rather than dropping it. */
        for (i = 0; i < cwlen - 2 * p; i++) {
            TrRec t = cw[i];
            tr_emit_rec(&t, 1, 1);
        }
        memmove(&cw[0], &cw[cwlen - p], (size_t)p * sizeof cw[0]);
        cwlen = p;
        cperiod = p;
        cpos = 0;
        creps = 2;
        for (i = 0; i < p; i++)     /* the second repetition is the newest one */
            clast[i] = cw[i];
        return;
    }
}

/* ---------------------------------------------------------------- switches -- */

static void tr_parse_regions(const char *s)
{
    unsigned mask = 0;
    const char *p = s;

    while (*p) {
        const char *comma = strchr(p, ',');
        size_t n = comma ? (size_t)(comma - p) : strlen(p);
        int i;
        for (i = 0; i < 8; i++) {
            if (strlen(tr_region_name[i]) == n && strncmp(p, tr_region_name[i], n) == 0) {
                mask |= 1u << i;
                break;
            }
        }
        if (!comma)
            break;
        p = comma + 1;
    }
    if (mask)
        tr_mask = mask;
}

static void tr_close(void)
{
    if (tr_fp) {
        tr_emit_pending();
        tr_flush();
        fclose(tr_fp);
        tr_fp = NULL;
        tr_on = 0;
    }
}

static int tr_init(void)
{
    const char *p = getenv("XLATE_TRACE_OUT");
    const char *e;

    tr_on = 0;
    if (!p || !*p || strcmp(p, "0") == 0)
        return 0;

    e = getenv("XLATE_TRACE_FROM");
    if (e && *e)
        tr_from = strtol(e, NULL, 0);
    e = getenv("XLATE_TRACE_TO");
    if (e && *e)
        tr_to = strtol(e, NULL, 0);
    e = getenv("XLATE_TRACE_CYCLE");
    if (e && *e && *e == '0' && e[1] == '\0')
        tr_cycle = 0;
    e = getenv("XLATE_TRACE_COLLAPSE");
    if (e && *e && *e == '0' && e[1] == '\0')
        tr_collapse = 0;
    e = getenv("XLATE_TRACE_ALL");
    if (e && *e && *e != '0')
        tr_mask = 0xFFu;
    else {
        e = getenv("XLATE_TRACE_REGIONS");
        if (e && *e)
            tr_parse_regions(e);
    }

    tr_fp = fopen(p, "wb");
    if (!tr_fp) {
        fprintf(stderr, "[trace] cannot open %s -- the trace is off\n", p);
        return 0;
    }
    fputs("# rce-trace v1 core=rondorecomp collapse=1\n", tr_fp);
    fflush(tr_fp);
    atexit(tr_close);
    tr_on = 1;
    tr_announced = 1;
    if (tr_to < 0)
        fprintf(stderr, "[trace] rce-trace v1 -> %s  frames %ld..end  regions mask 0x%02X"
                        "  c= %s  collapse %s\n",
                p, tr_from, tr_mask, tr_cycle ? "on" : "off",
                tr_collapse ? "on" : "off");
    else
        fprintf(stderr, "[trace] rce-trace v1 -> %s  frames %ld..%ld  regions mask "
                        "0x%02X  c= %s  collapse %s\n",
                p, tr_from, tr_to, tr_mask, tr_cycle ? "on" : "off",
                tr_collapse ? "on" : "off");
    return 1;
}

/* ------------------------------------------------------------------ events -- */

static int tr_active(void)
{
    return tr_frame >= tr_from && (tr_to < 0 || tr_frame <= tr_to);
}

/* Make sure the lines we are about to write carry the frame the CPU is in, and
 * write the FRAME marker for it.  Everything the console's recorder labels with
 * `f=` goes through here, so no access can be labelled with the frame after the
 * one it happened in. */
static void tr_frame_sync(const Cpu *c)
{
    long f = (long)c->vdc.frame;
    char mpr[17];

    if (f == tr_frame)
        return;
    tr_emit_pending();
    tr_flush();                 /* the previous frame is complete on disk */
    tr_frame = f;
    if (!tr_active())
        return;
    tr_mpr(mpr, c);
    if (tr_cycle)
        tr_line("t=FRAME c=%010llX f=%ld n=%llu pc=%04X mpr=%s\n",
                (unsigned long long)tr_now(c), f, (unsigned long long)tr_seq,
                (unsigned)c->pc, mpr);
    else
        tr_line("t=FRAME f=%ld n=%llu pc=%04X mpr=%s\n", f,
                (unsigned long long)tr_seq, (unsigned)c->pc, mpr);
    tr_seq++;
    tr_flush();                 /* a watcher uses this marker to see progress */
}

static int tr_ready(void)
{
    if (tr_on < 0)
        tr_init();
    return tr_on == 1;
}

void xl_trace_frame(Cpu *c)
{
    if (!tr_ready())
        return;
    tr_frame_sync(c);
}

void xl_trace_bus(Cpu *c, int wr, uint16_t la, uint8_t v)
{
    uint16_t off, pc;
    TrRec r;

    if (!tr_ready())
        return;
    off = (uint16_t)(la & 0x1FFF);
    r.reg = (uint8_t)((off >> 10) & 7);

    /* Filter first: most accesses on a busy frame are dropped, and a dropped access
     * must not force a frame marker or touch the collapse window. */
    if (!(tr_mask & (1u << r.reg)))
        return;
    if (r.reg == TR_VDC && !wr && (off == 0x0002 || off == 0x0003))
        return;

    tr_frame_sync(c);
    if (!tr_active())
        return;

    pc = tr_pc_bus(c);
    r.off = off;
    r.la = la;
    r.pc = pc;
    r.v = v;
    r.wr = (uint8_t)(wr ? 1 : 0);
    r.c = tr_now(c);
    tr_push(&r);
}

/* A write to $1800, the CD unit's start/SEL register.  The console records it as a
 * bus write AND as a discrete event; this is the second half, and the caller has
 * already written the bus line. */
void xl_trace_cdcmd(Cpu *c, uint8_t cmd)
{
    char mpr[17];

    if (!tr_ready())
        return;
    tr_frame_sync(c);
    if (!tr_active())
        return;
    tr_emit_pending();
    tr_mpr(mpr, c);
    if (tr_cycle)
        tr_line("t=CDCMD c=%010llX f=%ld n=%llu pc=%04X mpr=%s cmd=%02X\n",
                (unsigned long long)tr_now(c), tr_frame, (unsigned long long)tr_seq,
                (unsigned)c->pc, mpr, (unsigned)cmd);
    else
        tr_line("t=CDCMD f=%ld n=%llu pc=%04X mpr=%s cmd=%02X\n", tr_frame,
                (unsigned long long)tr_seq, (unsigned)c->pc, mpr, (unsigned)cmd);
    tr_seq++;
}

static const char *tr_irq_kind(int kind)
{
    switch (kind) {
    case XL_IRQ_CD:    return "irq2";
    case XL_IRQ_VDC:   return "irq1";
    case XL_IRQ_TIMER: return "timer";
    }
    return "brk";
}

static uint16_t tr_irq_vec(int kind)
{
    switch (kind) {
    case XL_IRQ_TIMER: return 0xFFFAu;
    case XL_IRQ_VDC:   return 0xFFF8u;
    }
    return 0xFFF6u;
}

void xl_trace_irq(Cpu *c, int kind, uint16_t vec)
{
    char mpr[17];

    if (!tr_ready())
        return;
    tr_frame_sync(c);
    if (!tr_active())
        return;
    tr_emit_pending();
    tr_mpr(mpr, c);
    if (!vec)
        vec = tr_irq_vec(kind);
    if (tr_cycle)
        tr_line("t=IRQ c=%010llX f=%ld n=%llu pc=%04X mpr=%s vec=%04X k=%s\n",
                (unsigned long long)tr_now(c), tr_frame, (unsigned long long)tr_seq,
                (unsigned)c->pc, mpr, (unsigned)vec, tr_irq_kind(kind));
    else
        tr_line("t=IRQ f=%ld n=%llu pc=%04X mpr=%s vec=%04X k=%s\n", tr_frame,
                (unsigned long long)tr_seq, (unsigned)c->pc, mpr, (unsigned)vec,
                tr_irq_kind(kind));
    tr_seq++;
}

/* The RTI that returns from an interrupt.  Called from xl_rti(), which is where the
 * three pop statements live now, so translated code, the interpreter and any C
 * caller all produce the same record. */
void xl_trace_irqe(Cpu *c)
{
    char mpr[17];

    if (!tr_ready())
        return;
    tr_frame_sync(c);
    if (!tr_active())
        return;
    tr_emit_pending();
    tr_mpr(mpr, c);
    if (tr_cycle)
        tr_line("t=IRQE c=%010llX f=%ld n=%llu pc=%04X mpr=%s\n",
                (unsigned long long)tr_now(c), tr_frame, (unsigned long long)tr_seq,
                (unsigned)c->pc, mpr);
    else
        tr_line("t=IRQE f=%ld n=%llu pc=%04X mpr=%s\n", tr_frame,
                (unsigned long long)tr_seq, (unsigned)c->pc, mpr);
    tr_seq++;
}

void xl_trace_summary(FILE *out)
{
    if (!out || !tr_announced)
        return;
    fprintf(out, "TRACE lines=%llu bytes=%llu collapsed=%llu frame=%ld\n",
            (unsigned long long)tr_lines, (unsigned long long)tr_bytes,
            (unsigned long long)tr_collapsed, tr_frame);
}
