/* runtime.c -- HuC6280 helpers plus the machine stubs, for recompiled code.
 *
 * Semantics follow a reference emulator's HuC6280 core (Mednafen's
 * `pce/huc6280.cpp` + `huc6280_ops.inc`), including its quirks:
 *
 *   - zero page lives at $2000-$20FF, the stack at $2100-$21FF;
 *   - the T flag makes AND/ORA/EOR/ADC operate on $2000+X and restore A
 *     afterwards; T is cleared at the end of nearly every instruction (the
 *     recompiler emits that clear, because it is a property of the instruction
 *     stream, not of a helper);
 *   - SBC has no T-mode prefix (the oracle behaves this way; mirrored, and
 *     flagged in README as a deviation-free copy rather than a design choice);
 *   - TST takes N and V from the memory byte and Z from (byte & immediate) == 0;
 *   - decimal ADC leaves V alone.
 *
 * Deliberately not modelled: interrupts, timers, video, sound, CD timing,
 * cycle counts, and BRK (the recompiler emits a trap for it).
 */
#include "runtime.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ misc -- */

static uint32_t crc_table[256];
static int crc_table_ready;

static void crc_init(void)
{
    uint32_t i, j, v;
    for (i = 0; i < 256; i++) {
        v = i;
        for (j = 0; j < 8; j++)
            v = (v & 1) ? (0xEDB88320u ^ (v >> 1)) : (v >> 1);
        crc_table[i] = v;
    }
    crc_table_ready = 1;
}

uint32_t xl_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    if (!crc_table_ready)
        crc_init();
    for (i = 0; i < len; i++)
        crc = crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t xl_crc_phys(Cpu *c)
{
    return xl_crc32(c->mem, XL_PHYS_SIZE);
}

void xl_set_log(Cpu *c, FILE *f) { c->log = f; }

__attribute__((format(printf, 2, 3)))
static void xl_logf(Cpu *c, const char *fmt, ...)
{
    va_list ap;
    if (!c->log)
        return;
    /* Every line carries its frame: without it, "243 calls" cannot be told apart
     * from "243 calls in one frame", which is the difference between a rate and a
     * stuck retry loop. */
    fprintf(c->log, "f=%05u ", (unsigned)c->vdc.frame);
    va_start(ap, fmt);
    vfprintf(c->log, fmt, ap);
    va_end(ap);
    fputc('\n', c->log);
    fflush(c->log);          /* flushed per line: a hang must not lose the trail */
}

/* -------------------------------------------------- stale-bank instrument -- */
/* REPORT-ONLY.  This exists to answer one question with evidence instead of a guess:
 * *when* do the bytes the translator built code from stop being the bytes the machine
 * runs?  Nothing here takes part in a dispatch decision, refuses to run, or writes to
 * the machine image -- every function below only reads `c->mem` and appends a line to
 * the log.  Installing it must be provable not to move a gate; `XLATE_STALE=0` turns
 * it off, and the gates are run with it on so that "unchanged" is measured, not
 * assumed.
 *
 * Two halves, because one without the other cannot answer the question:
 *
 *  1. A WRITE BARRIER.  Every store already funnels through xl_wr -> xl_wr_phys, so
 *     that is the one place single-byte writes are caught (block moves, read-modify-
 *     write and the CD byte-copy loop all go through it).  The other ways into the
 *     2 MB image are raw memcpy calls: the generated init (xl_load_phys), the disc
 *     block load at the boot hand-off (xl_boot_game), and the mis-modelled mode
 *     $FE/$FF upload (xl_bios_call).  Each of those calls xl_stale_note() too.
 *     Per bank it keeps: a dirty flag, a generation counter, and the frame, PC,
 *     writing bank and source of the FIRST write in the current generation.
 *
 *  2. A COMPARISON ON BANK ENTRY.  When the live bank register selects a bank that
 *     is dirty, the live 8 KB is compared against the image that bank's translation
 *     was built from, and the event is logged ONCE PER GENERATION (the dirty flag is
 *     cleared; the next write starts a new generation).  The log line carries bank,
 *     frame, the first differing offset with its before/after bytes, the run of
 *     differing bytes around it, how many bytes differ, and who wrote first.
 *
 * THE REFERENCE IMAGE.  "The image the translation was built from" is not the image
 * in memory when the run starts: the boot chain's game banks are zero until the boot
 * hand-off copies disc blocks into them, so a start-of-run snapshot would report
 * every one of those banks as "stale" the moment it was legitimately loaded.  The
 * generated init therefore registers the compiled bytes per bank through
 * xl_stale_image().  Banks that have no compiled image (the BIOS ROM, an injected
 * savestate, work RAM) fall back to whatever was in memory at the first executed
 * instruction, which is the only honest reference they have.
 *
 * Filtered out on purpose: the I/O page ($FF, not memory at all), the work-RAM bank
 * ($F8, written by every stack push and zero-page store and never translated as a
 * code bank), and the ROM banks below $20 (writes there are ignored by the mapper,
 * see xl_wr_phys).  What is left is exactly the CD-RAM window where game code
 * lives. */

enum { XL_SW_STORE = 0, XL_SW_CDREAD = 1, XL_SW_BOOTLOAD = 2, XL_SW_LOADPHYS = 3 };
static const char *const xl_sw_name[] = {
    "code store", "CD read", "boot disc-block load", "generated init"
};

typedef struct {
    uint32_t gen;                /* generations seen in this bank                */
    uint32_t ndiff;              /* bytes that differed at the last comparison    */
    uint32_t scanned_gen;        /* generation `ndiff` was measured for           */
    uint32_t wframe;             /* frame of the first write of this generation   */
    uint16_t wpc;                /* PC that performed it                          */
    uint8_t  wbank;              /* bank that PC was executing in                 */
    uint8_t  wpages;             /* MPR pages that mapped this bank at write time  */
    uint8_t  wself;              /* 1 = the bank being written was the executing one */
    uint8_t  wsrc;               /* XL_SW_*                                       */
    uint8_t  dirty, due;         /* due = compare at the next instruction (self-write) */
    uint32_t wrec, wsec;         /* CD read: record (start sector) / sector count */
    uint32_t waddr;              /* offset in the bank the write landed at        */
    uint16_t wcall;              /* CD read: the caller's return address          */
    uint8_t  wcallbank;          /* and the bank it was executing in              */
} StaleBank;

static uint8_t   xl_stale_ref[XL_PHYS_SIZE];   /* the compiled image, per bank */
static uint8_t   xl_stale_have[XL_BANK_COUNT];
static StaleBank xl_stale_rec[XL_BANK_COUNT];
static uint8_t   xl_stale_armed;
static int       xl_stale_on = -1;
static uint32_t  xl_stale_events;     /* comparisons that found a difference */

/* ---------------------------------------------- the guard's own state -- */
/* One entry per bank: which banks may be guarded at all, the cached verdict for the
 * generation it was measured in, and the numbers the report has to carry (ADR O-5):
 * interpreted instructions, frames the bank was interpreted, and the distinct
 * contents it was seen to hold.  See the rule below xl_stale_enter(). */
typedef struct {
    unsigned long interp_steps;      /* instructions this bank ran in the interpreter */
    uint32_t interp_frames;          /* distinct frames in which it did               */
    uint32_t last_frame;             /* last frame counted                            */
    uint32_t routed_from, routed_to; /* first/last frame the bank was routed          */
    uint32_t routings;               /* times the verdict flipped to "interpret"      */
    uint32_t unknown_images;         /* distinct non-compiled contents executed       */
    uint32_t digests[8];             /* and their CRCs (first eight, for the report)   */
    uint32_t images_untracked;       /* distinct contents beyond the eight             */
} GuardBank;

static uint8_t   xl_guard_compiled[XL_BANK_COUNT]; /* a compiled image is registered */
static uint8_t   xl_guard_can[XL_BANK_COUNT];      /* ...and this bank is a candidate */
static uint8_t   xl_guard_v[XL_BANK_COUNT];        /* cached verdict: 1 = interpret   */
static uint32_t  xl_guard_vgen[XL_BANK_COUNT];     /* generation the verdict is for   */
static uint8_t   xl_guard_seen[XL_BANK_COUNT];     /* a verdict has been measured     */
static GuardBank xl_guard_g[XL_BANK_COUNT];
static uint32_t  xl_guard_routings;                /* verdict flips to "interpret"    */
static uint32_t  xl_guard_scans;                   /* 8 KB scans actually performed   */
static int       xl_guard_first;                   /* first firing already printed    */
static int       xl_guard_on = -1;

static int  xl_stale_enabled(void);
static int  xl_guard_enabled(void);
static int  xl_stale_interesting(uint8_t bank);
/* Set around a CD transfer so the barrier can say which read a write came from
 * rather than blaming the BIOS stub's own address. */
static struct { int on; unsigned rec, sec, dest; } xl_cd_ctx;

static int xl_stale_enabled(void)
{
    if (xl_stale_on < 0) {
        const char *e = getenv("XLATE_STALE");
        xl_stale_on = (e && e[0] == '0') ? 0 : 1;
    }
    return xl_stale_on;
}

/* The guard is an authority, so it has its own switch (XLATE_GUARD=0 turns the
 * routing off and leaves the report-only instrument in place - that is how gate
 * O-0's "guard on and off must agree on a run that never touches a changed bank"
 * is measured).  On by default: a run that silently executes a translation of bytes
 * that are not there is the bug this round exists to remove. */
static int xl_guard_enabled(void)
{
    if (xl_guard_on < 0) {
        const char *e = getenv("XLATE_GUARD");
        xl_guard_on = (e && e[0] == '0') ? 0 : 1;
    }
    return xl_guard_on;
}

/* Called once from xl_reset(): a fresh machine starts with no history. */
static void xl_stale_clear(void){
    unsigned b;

    memset(xl_stale_have, 0, sizeof xl_stale_have);
    memset(xl_stale_rec, 0, sizeof xl_stale_rec);
    /* Generation 0 has never been compared, so it must not look measured: -1 forces
     * the first verdict (or the first entry comparison) to scan the bytes instead of
     * inheriting a "clean" that nothing established. */
    for (b = 0; b < XL_BANK_COUNT; b++)
        xl_stale_rec[b].scanned_gen = 0xFFFFFFFFu;
    xl_stale_armed = 0;
    xl_stale_events = 0;
    memset(xl_guard_compiled, 0, sizeof xl_guard_compiled);
    memset(xl_guard_can, 0, sizeof xl_guard_can);
    memset(xl_guard_v, 0, sizeof xl_guard_v);
    memset(xl_guard_vgen, 0, sizeof xl_guard_vgen);
    memset(xl_guard_seen, 0, sizeof xl_guard_seen);
    memset(xl_guard_g, 0, sizeof xl_guard_g);
    xl_guard_routings = 0;
    xl_guard_first = 0;
    xl_cd_ctx.on = 0;
    xl_cd_ctx.rec = xl_cd_ctx.sec = xl_cd_ctx.dest = 0;
}

/* Registration of one compiled image: the bytes the translation was built from.
 * Called from the generated xlate_init_state(), after xl_reset(). */
void xl_stale_image(Cpu *c, uint8_t bank, uint16_t off, const uint8_t *data, size_t len)
{
    size_t o = (size_t)(off & 0x1FFF);
    (void)c;
    if (o >= XL_BANK_SIZE || !len)
        return;
    if (o + len > XL_BANK_SIZE)
        len = XL_BANK_SIZE - o;
    if (!xl_stale_have[bank]) {
        /* An image that covers only part of a bank leaves the rest at whatever the
         * machine holds now, so the reference is never a half-initialised bank. */
        memcpy(&xl_stale_ref[(size_t)bank * XL_BANK_SIZE],
               &c->mem[(size_t)bank * XL_BANK_SIZE], XL_BANK_SIZE);
        xl_stale_have[bank] = 1;
    }
    memcpy(&xl_stale_ref[(size_t)bank * XL_BANK_SIZE + o], data, len);
    /* A registered image is what makes a bank *guardable*: only a bank whose
     * translation was built from known bytes can be found to disagree with them.
     * A bank with no image (the BIOS ROM, an injected savestate's untouched bank,
     * a bank the translator never covered) is never routed - see xl_guard_can. */
    xl_guard_compiled[bank] = 1;
    xl_guard_can[bank] = (uint8_t)(xl_guard_enabled() && xl_stale_interesting(bank));
}

/* The start-of-run image, for every bank that has no compiled image.  Taken at the
 * first executed instruction -- after the generated init, a savestate load and any
 * --poke, and before the program can have written anything itself. */
static void xl_stale_arm(Cpu *c)
{
    unsigned b;
    if (xl_stale_armed)
        return;
    /* Armed for the guard even when the report is switched off: the guard reads the
     * write barrier's generations, and without arming it would measure nothing. */
    if (!xl_stale_enabled() && !xl_guard_enabled())
        return;
    xl_stale_armed = 1;
    for (b = 0; b < XL_BANK_COUNT; b++)
        if (!xl_stale_have[b])
            memcpy(&xl_stale_ref[(size_t)b * XL_BANK_SIZE],
                   &c->mem[(size_t)b * XL_BANK_SIZE], XL_BANK_SIZE);
}

static int xl_stale_interesting(uint8_t bank)
{
    return bank != XL_IO_BANK && bank != XL_WORKRAM_BANK && bank >= XL_ROM_BANKS;
}

/* The write barrier's bookkeeping.  Called BEFORE the bytes are written, from every
 * path that can reach the image, so the record describes the first write of a
 * generation and the comparison at entry sees the finished result. */
void xl_stale_note(Cpu *c, uint8_t bank, uint16_t off, int src)
{
    StaleBank *s;
    unsigned p;

    if (!xl_stale_armed || !xl_stale_interesting(bank))
        return;
    s = &xl_stale_rec[bank];
    if (s->dirty)                       /* still the same generation */
        return;
    s->dirty = 1;
    s->gen++;
    s->wframe = (uint32_t)c->vdc.frame;
    s->wpc    = c->pc;
    s->wbank  = c->mpr[c->pc >> 13];
    s->waddr  = (uint32_t)(off & 0x1FFF);
    s->wsrc   = (uint8_t)src;
    s->wpages = 0;
    for (p = 0; p < 8; p++)
        if (c->mpr[p] == bank)
            s->wpages |= (uint8_t)(1u << p);
    s->wself = (uint8_t)(c->mpr[c->pc >> 13] == bank);
    s->due   = s->wself;                /* compare without waiting for a bank change */
    s->wcall = 0;
    s->wcallbank = 0;
    if (xl_cd_ctx.on) {
        /* The copy loop runs inside the BIOS stub, so c->pc is $E009/$E045 and says
         * nothing about who asked.  The return address is on the stack (hi at S+2,
         * lo at S+1 -- xl_push16 pushes high first). */
        s->wsrc  = XL_SW_CDREAD;
        s->wrec  = xl_cd_ctx.rec;
        s->wsec  = xl_cd_ctx.sec;
        s->waddr = (uint32_t)(off & 0x1FFF);
        s->wcall = (uint16_t)((xl_rd(c, (uint16_t)(XL_STACK_BASE | (uint8_t)(c->s + 1))) |
                               ((uint16_t)xl_rd(c, (uint16_t)(XL_STACK_BASE | (uint8_t)(c->s + 2))) << 8)) + 1u);
        s->wcallbank = c->mpr[s->wcall >> 13];
    }
}

/* Compare one bank against its compiled image.  Returns the number of differing
 * bytes and records that number in the bank's record, which is what makes the
 * result reusable: a comparison is done once per (bank, generation), not once per
 * instruction.  `force` scans even when the barrier has nothing new to say - the
 * guard needs a measured answer for a generation whose writes were already
 * accounted for by the report-only path, and for generation 0, where there has
 * been no write at all but the loaded memory may still not be the image (an
 * injected savestate).  The log line is emitted only for a generation the barrier
 * recorded, so forcing a scan never duplicates it. */
static unsigned xl_stale_compare(Cpu *c, uint8_t bank, int force)
{
    StaleBank *s = &xl_stale_rec[bank];
    const uint8_t *live = &c->mem[(size_t)bank * XL_BANK_SIZE];
    const uint8_t *img  = &xl_stale_ref[(size_t)bank * XL_BANK_SIZE];
    unsigned i, n = 0, first = 0, rs = 0, re = 0;
    uint8_t was_dirty;

    if (!s->dirty && !force)
        return s->ndiff;
    was_dirty = s->dirty;
    xl_guard_scans++;                   /* one 8 KB comparison, whoever asked for it */
    for (i = 0; i < XL_BANK_SIZE; i++) {
        if (live[i] != img[i]) {
            if (n == 0)
                first = i;
            n++;
        }
    }
    s->dirty = 0;
    s->due = 0;
    s->ndiff = n;
    s->scanned_gen = s->gen;
    if (!n)                             /* written back to the same bytes: not stale */
        return 0;
    rs = first;
    while (rs > 0 && live[rs - 1] != img[rs - 1])
        rs--;
    re = first;
    while (re + 1 < XL_BANK_SIZE && live[re + 1] != img[re + 1])
        re++;
    if (!was_dirty || !xl_stale_enabled())
        return n;
    xl_stale_events++;
    if (!c->log)
        return n;
    {
        char where[32];
        int k = 0, q;
        where[0] = '\0';
        for (q = 0; q < 8; q++)
            if (s->wpages & (1u << q))
                k += snprintf(where + k, sizeof where - (size_t)k, "%s%d",
                              k ? "," : "", q);
        if (!k)
            snprintf(where, sizeof where, "none");
        xl_logf(c, "stale: bank $%02X gen %u frame %u -- %u of %d bytes differ; "
                   "first diff offset $%04X before $%02X after $%02X; run $%04X-$%04X "
                   "(%u bytes); writer pc $%04X (bank $%02X) at frame %u via %s; "
                   "mapped at page(s) %s%s",
                bank, s->gen, (unsigned)c->vdc.frame, n, XL_BANK_SIZE,
                first, img[first], live[first], rs, re, re - rs + 1u,
                s->wpc, s->wbank, s->wframe, xl_sw_name[s->wsrc],
                where, s->wself ? ", EXECUTING" : "");
        if (s->wsrc == XL_SW_CDREAD)
            xl_logf(c, "stale:   that write came from a CD transfer of record %u "
                       "(%u sectors) to $%04X, landing at bank offset $%04X; "
                       "requested from pc $%04X (bank $%02X)",
                    s->wrec, s->wsec, xl_cd_ctx.dest, s->waddr, s->wcall, s->wcallbank);
        /* The run of differing bytes, so "6000 bytes differ" can be seen as a shape
         * (an overlay header, a table, or the whole bank) instead of a number. */
        {
            unsigned show = re - rs + 1u, i2;
            char b1[3 * 32 + 4], b2[3 * 32 + 4];
            int k1 = 0, k2 = 0;
            if (show > 32)
                show = 32;
            for (i2 = 0; i2 < show; i2++) {
                k1 += snprintf(b1 + k1, sizeof b1 - (size_t)k1, "%02X ", img[rs + i2]);
                k2 += snprintf(b2 + k2, sizeof b2 - (size_t)k2, "%02X ", live[rs + i2]);
            }
            xl_logf(c, "stale:   run $%04X..$%04X (%u bytes)%s", rs, re, re - rs + 1u,
                    (re - rs + 1u) > 32 ? ", first 32:" : ":");
            xl_logf(c, "stale:     built from: %s", b1);
            xl_logf(c, "stale:     live now:   %s", b2);
        }
    }
    return n;
}

/* Bank entry: the dispatcher has resolved the live bank register to `bank`. */
static void xl_stale_enter(Cpu *c, uint8_t bank)
{
    static uint8_t last;
    static int have_last;
    uint8_t changed = (uint8_t)(!have_last || bank != last);

    have_last = 1;
    last = bank;
    if (!xl_stale_armed || !xl_stale_interesting(bank))
        return;
    if (xl_stale_rec[bank].dirty && (changed || xl_stale_rec[bank].due))
        (void)xl_stale_compare(c, bank, 0);
}

/* ------------------------------------------------------------------ guard -- */
/* THE GUARD'S RULE, in one sentence: a bank's translated units may run only while the
 * live 8 KB of that bank still equals the image they were translated from; when it does
 * not, every instruction whose bank register selects that bank is executed by the
 * interpreter instead, and translation resumes by itself as soon as a reload puts the
 * image back.
 *
 * Per bank, not per unit, deliberately: the ADR argues for the unit-level invariant and
 * a bank comparison is coarser, but a bank is the granularity at which the game streams
 * (a whole 8 KB window at a time), it is the granularity the write barrier already
 * records, and it costs one 8 KB scan per bank instead of one per translated unit.  The
 * finer key is the ADR's step 2 (content-addressed translation), not this round.
 *
 * Where the comparison happens: on BANK ENTRY, never per instruction.  The barrier
 * bumps a per-bank generation counter on the first write after a comparison, so a
 * verdict is cached as (bank, generation) and reused until the generation changes.
 * That is the whole cost model: one 8 KB scan per bank per write burst, plus two array
 * reads and a compare per dispatched instruction.
 *
 * WHICH BANKS MAY BE ROUTED - three conditions, and each of them matters:
 *
 *  1. The bank must have a compiled image, i.e. the generated init registered bytes for
 *     it through xl_stale_image() (xl_guard_compiled).  A bank with no image has nothing
 *     to disagree with, so "stale" is not a thing that can be true there; routing it
 *     would turn every "no translation for this address" into a silent interpretation
 *     and destroy the UNTRANSLATED signal that gate O-2 depends on.
 *
 *  2. The bank must be one the barrier tracks at all (xl_stale_interesting): not the I/O
 *     page ($FF, which is not memory) and not the ROM banks below $20 (writes there are
 *     ignored by the mapper).
 *
 *  3. The bank must NOT be work RAM ($F8).  See the trampoline note below.
 *
 * WORK RAM IS THE TRAMPOLINE CASE, and this is how it is distinguished.  Bank $F8 holds
 * code the game builds at run time - most importantly a block-move routine whose shape
 * the port models directly in xl_ram_trampoline(), which the dispatcher consults before
 * it refuses an address and which the interpreter calls from the same place.  $F8 is
 * written by every stack push and zero-page store, so a bank-level byte comparison there
 * would be permanently "stale" (it is already excluded from the barrier for that reason)
 * and routing it would swap a modelled one-step routine for a slow interpreted one and
 * change cycle costs by 1,308 over the boot -- a real behaviour difference, measured.
 * So $F8 is
 * never routed: its translated loader units keep running, and the built routine keeps
 * being caught by xl_ram_trampoline on both engines, exactly as before.
 *
 * A bank that is routed keeps being interpreted until a later comparison says the live
 * bytes match the image again; nothing is remembered across a reload, so returning to an
 * overlay and later coming back is handled by the same two comparisons. */

/* Record the distinct non-compiled contents a guarded bank has been seen to hold, so
 * O-5's "distinct images per bank" is a measured number and not a claim.  Only called
 * when a comparison found a difference, so a clean bank costs nothing. */
static void xl_guard_note_image(Cpu *c, uint8_t bank)
{
    GuardBank *g = &xl_guard_g[bank];
    uint32_t d = xl_crc32(&c->mem[(size_t)bank * XL_BANK_SIZE], XL_BANK_SIZE);
    unsigned i;

    for (i = 0; i < g->unknown_images && i < 8u; i++)
        if (g->digests[i] == d)
            return;
    if (g->unknown_images < 8u)
        g->digests[g->unknown_images] = d;
    else
        g->images_untracked++;
    g->unknown_images++;
}

/* Measure this bank for the current generation and cache the verdict. */
static int xl_guard_refresh(Cpu *c, uint8_t bank)
{
    StaleBank *s = &xl_stale_rec[bank];
    GuardBank *g = &xl_guard_g[bank];
    uint8_t prev = xl_guard_v[bank];
    uint8_t had  = xl_guard_seen[bank];
    unsigned n;

    if (s->dirty || s->scanned_gen != s->gen) {
        n = xl_stale_compare(c, bank, 1);   /* the measurement, once per generation */
    } else {
        n = s->ndiff;                       /* already measured for this generation */
    }
    xl_guard_v[bank] = (uint8_t)(n != 0);
    xl_guard_vgen[bank] = s->gen;
    xl_guard_seen[bank] = 1;
    if (!n)
        goto report;
    xl_guard_note_image(c, bank);
    if (had && prev)                        /* still routed: nothing changed */
        return 1;
    xl_guard_routings++;
    g->routings++;
    if (!g->routed_from)
        g->routed_from = (uint32_t)c->vdc.frame;
    g->routed_to = (uint32_t)c->vdc.frame;
    if (c->log)
        xl_logf(c, "guard: bank $%02X -> interpreter at frame %u (pc $%04X, gen %u): "
                   "%u of %d bytes differ from the compiled image; first write by pc "
                   "$%04X in bank $%02X at frame %u via %s",
                bank, (unsigned)c->vdc.frame, c->pc, s->gen, n, XL_BANK_SIZE,
                s->wpc, s->wbank, s->wframe, xl_sw_name[s->wsrc]);
    if (!xl_guard_first) {
        /* First firing, on stdout: the acceptance report quotes this line, so it is
         * not hidden in a log file that a run may not have enabled. */
        xl_guard_first = 1;
        printf("GUARD first bank=%02X frame=%u pc=%04X gen=%u differ=%u "
               "writer_pc=%04X writer_bank=%02X writer_frame=%u writer=%s\n",
               bank, (unsigned)c->vdc.frame, c->pc, s->gen, n,
               s->wpc, s->wbank, s->wframe, xl_sw_name[s->wsrc]);
        fflush(stdout);
    }
    return 1;
report:
    if (had && !prev)                       /* already running translated: nothing to say */
        return 0;
    if (had || prev) {
        if (c->log)
            xl_logf(c, "guard: bank $%02X -> translation at frame %u (pc $%04X, gen %u): "
                       "live bytes match the compiled image again",
                    bank, (unsigned)c->vdc.frame, c->pc, s->gen);
        g->routed_to = (uint32_t)c->vdc.frame;
    }
    return 0;
}

/* The guard, asked once per dispatched instruction.  One array read and a compare in
 * the common case; the 8 KB scan happens only when the bank's generation has moved. */
static int xl_guard_stale(Cpu *c, uint8_t bank)
{
    StaleBank *s = &xl_stale_rec[bank];
    if (xl_guard_seen[bank] && xl_guard_vgen[bank] == s->gen)
        return (int)xl_guard_v[bank];
    return xl_guard_refresh(c, bank);
}

/* Per-bank interpreted accounting (ADR O-5): instructions and the distinct frames in
 * which the bank actually ran interpreted.  Called only on an interpreted step, so the
 * translated path pays nothing for it. */
static void xl_guard_note_interp(Cpu *c, uint8_t bank)
{
    GuardBank *g = &xl_guard_g[bank];
    uint32_t f = (uint32_t)c->vdc.frame;

    g->interp_steps++;
    if (g->interp_frames == 0 || g->last_frame != f) {
        g->interp_frames++;
        g->last_frame = f;
    }
    if (!g->routed_from)
        g->routed_from = f;
    g->routed_to = f;
}

/* The translated/interpreted split, as a first-class number.  A run that leaned on
 * the interpreter must never be quoted as a pure translated run, so this line is
 * printed on every run, including one that never fired.
 *
 * Instructions interpreted because the pc had no translated unit at all -- code the
 * card loaded off the disc, or that the program built in work RAM.  Per bank, so
 * "how much of the loaded code ran interpreted" has a bank attached. */
static unsigned long xl_unt_interp[256];

void xl_guard_summary(Cpu *c, FILE *out)
{
    unsigned b, routed = 0;
    unsigned long interp = (unsigned long)c->interp_steps;
    unsigned long total  = (unsigned long)c->steps;
    unsigned long trans  = total > interp ? total - interp : 0;

    for (b = 0; b < 256; b++)
        if (xl_guard_g[b].interp_steps)
            routed++;
    fprintf(out, "GUARD firings=%u routed_banks=%u scans=%u\n",
            xl_guard_routings, routed, xl_guard_scans);
    for (b = 0; b < 256; b++) {
        const GuardBank *g = &xl_guard_g[b];
        unsigned i;
        if (!g->interp_steps && !g->routings)
            continue;
        fprintf(out, "GUARD bank=%02X routings=%u frames=%u..%u interpreted_frames=%u "
                     "interpreted=%lu images=1+%u",
                b, g->routings, g->routed_from, g->routed_to, g->interp_frames,
                g->interp_steps, g->unknown_images);
        if (g->unknown_images) {
            fprintf(out, " digests=");
            for (i = 0; i < g->unknown_images && i < 8u; i++)
                fprintf(out, "%s%08X", i ? "," : "", g->digests[i]);
            if (g->images_untracked)
                fprintf(out, ",+%u", g->images_untracked);
        }
        fprintf(out, "\n");
    }
    fprintf(out, "SPLIT translated=%lu interpreted=%lu total=%lu "
                 "translated_pct=%.4f interpreted_pct=%.4f\n",
            trans, interp, total,
            total ? 100.0 * (double)trans / (double)total : 100.0,
            total ? 100.0 * (double)interp / (double)total : 0.0);
    /* Of the interpreted share, how much was "this pc has no translation at all"
     * (the card's own disc-loaded code) rather than the guard routing a bank.  Two
     * different reasons, two different costs; they must not be quoted as one. */
    if (c->interp_untranslated_steps) {
        int first = 1;
        fprintf(out, "SPLIT_UNTRANSLATED interpreted=%lu of=%lu banks=",
                c->interp_untranslated_steps, interp);
        for (b = 0; b < 256; b++) {
            if (!xl_unt_interp[b])
                continue;
            fprintf(out, "%s%02X:%lu", first ? "" : ",", b, xl_unt_interp[b]);
            first = 0;
        }
        fprintf(out, "\n");
    }
    fflush(out);
}

/* ------------------------------------------------------------- lifecycle -- */

/* THE SPINDLE'S SPIN-UP, IN MASTER CLOCKS.  See the contract at the top of
 * cd_dispatch(): a CD drive is not ready until its motor is at speed, and the
 * System Card's boot asks Test Unit Ready and retries every 70 frames until it is.
 * One second, in this machine's own frame (263 lines x 455 dots = 119,665 CPU
 * cycles = 358,995 master clocks).  The exact figure is unmeasured -- the card's
 * 70-frame retry cadence is coarser than any error in it -- and the reference only
 * pins it between "not ready at the first question" and "ready at the retry". */
#define XL_CD_SPINUP_FRAMES 60u
/* A frame is VD_LINES * VD_DOTS CPU cycles; the drive's timeline is in MASTER
 * clocks, three to a CPU cycle (XL_CD_M3 below does the same multiply). */
#define XL_CD_SPINUP_MASTER \
    ((uint64_t)XL_CD_SPINUP_FRAMES * 3u * VD_LINES * VD_DOTS)

uint32_t xl_adpcm_cycles_per_sample(uint8_t rate);   /* the ADPCM section below */

void xl_reset(Cpu *c)
{
    memset(c, 0, sizeof *c);
    /* Power-on is when the motor starts; a console RESET (xl_boot_reset) does not
     * touch this, because a reset does not spin the drive down. */
    c->cd.ready_at = XL_CD_SPINUP_MASTER;
    /* Geargrafx `Adpcm::Reset` (`adpcm.cpp:142-172`): the sample rate register powers
     * up as 0xF and the sample period is derived from it.  The RAM is already zero
     * (`memset(m_adpcm_ram, 0, sizeof(m_adpcm_ram))`, `:171`). */
    c->cd.adpcm_freq = 0x0F;
    c->cd.adpcm_cycles_per_sample = xl_adpcm_cycles_per_sample(0x0F);
    xl_stale_clear();
    c->s = 0xFF;
    c->p = XL_I | XL_BF;                 /* $24, as the H/W leaves it */
    /* THE SPEED THE GAME RUNS AT, AND WHY IT IS SEEDED HERE.
     *
     * The hardware comes out of RESET in slow mode -- the reference core's reset arm
     * is `speed = 0; REDOSPEEDCACHE();` (huc6280.cpp:567, inside `IQRESET`) -- and
     * the System Card's very first two instructions are
     *
     *     E0F3  sei          ; (this runs slow)
     *     E0F4  csh          ; switch to 7.16 MHz, and never go back on this path
     *
     * so the machine every game is handed boots and then *stays* normal.  This
     * function does not build a power-on machine: it builds the one the game is
     * handed, after the card has set the video chip up, seeded the BRAM signature and
     * taken the CPU to normal speed (see the two blocks below, which are the same
     * class of seed).  Seeding `fast` here therefore matches the reference from the
     * second instruction onward and differs from it only for `sei` and the two cycles
     * of `csh` that precede the switch -- 18 + 18 = 36 master clocks, 0.03 % of a
     * frame, once per power-on.
     *
     * The other entry, `xl_boot_reset()` below (the console RESET of the reset-vector
     * path),
     * is the one that must come up *slow*, because there the card really does execute
     * `sei; csh` itself.  It sets the mode explicitly for that reason. */
    xl_speed_set(c, 1);
    /* Resolved here, once, so no instruction pays for getenv(): the switch that makes
     * this machine execute the supplied System Card instead of modelling it. */
    {
        const char *e = getenv("XLATE_BIOS_EXEC");
        c->bios_exec = (uint8_t)((e && e[0] && e[0] != '0') ? 1 : 0);
    }
    /* The reset-vector entry is a second switch, not a replacement for the first:
     * it means nothing without the card executing, so it is resolved as a
     * conjunction here and never read again on any path. */
    {
        const char *e = getenv("XLATE_BIOS_RESET");
        c->bios_reset = (uint8_t)(c->bios_exec && e && e[0] && e[0] != '0');
    }
    /* IPL state: I/O page at $0000, work RAM at $2000, CD RAM at $4000..,
     * BIOS at $E000.  $FF free (hardware).  This is the mapping the loader is
     * entered with; it re-maps $4000-$9FFF itself from the BIOS constant $FFF5. */
    c->mpr[0] = XL_IO_BANK;
    c->mpr[1] = XL_WORKRAM_BANK;
    c->mpr[2] = XL_CDRAM_BASE + 0;
    c->mpr[3] = XL_CDRAM_BASE + 1;
    c->mpr[4] = XL_CDRAM_BASE + 2;
    c->mpr[5] = XL_CDRAM_BASE + 3;
    c->mpr[6] = XL_CDRAM_BASE + 4;
    c->mpr[7] = 0x00;                    /* BIOS ROM */
    c->pc = 0x0000;
    /* A machine that has just been switched on has its save RAM write-protected
     * (the card unlocks it explicitly, see XL_BRAM_BANK).  xl_reset() is the
     * power-on path only: a console RESET is XLATE_BIOS_RESET, which enters at the
     * card's vector and must NOT touch this, because the battery keeps the save
     * data across a reset. */
    c->bram_wprot = 1;
    c->reset_resume = 0;         /* a power-on starts here, nothing is pending */
    /* THE BACKUP RAM POWERS ON FORMATTED, NOT BLANK.  Same class of fact as the VDC
     * registers above: the machine a CD program is handed is not a virgin one.  The
     * reference core's own machine (mednafen/pce/huc.cpp, `HuC_PCE::Load`, the
     * `UseBRAM` branch) fills the 2 KB with zeros and then puts its eight-byte
     * signature `HUBM 00 88 10 80` at $000 -- "so users don't have to manually
     * initialize the file cabinet in the CD BIOS screen", in its own comment.
     *
     * This is not cosmetic.  The card's file-exists test (System Card $FB96, reached
     * from bm_read.impl) looks for that signature; a blank device reads as an
     * UNFORMATTED card, so the game takes the format-then-create path instead of the
     * write-one-file path.  Measured at the file menu (frames 7,585-7,595): with a
     * blank device this port enters the card's backup-RAM block FIVE times, each
     * ending in the card's dual vertical-blank wait at ROM $FCB8/$FCBF, and the
     * game's frame counter $2047 stalls five frames; the console performs the block
     * ONCE and stalls two.  That is the whole of the residual step at the menu load.
     *
     * `--sram` still wins: xl_load_sram() runs after this and replaces the image. */
    memset(c->bram, 0, sizeof c->bram);
    memcpy(c->bram, "\x48\x55\x42\x4D\x00\x88\x10\x80", 8);   /* "HUBM" 00 88 10 80 */
    xl_video_init(c);
    xl_ramstream_begin(c);        /* block 0 of XLATE_RAMSTREAM: the power-on state */
    xl_framelog_begin(c);         /* frame 0 of XLATE_FRAMELOG: the same instant */
}

void xl_video_init(Cpu *c)
{
    vdc_reset(&c->vdc);
    /* The machine a CD program is handed is not a power-on machine: the CD BIOS has
     * already programmed the video chip.  These are the values measured from the
     * emulator's captured state at the intro (HDR/VDR/VSR/MWR/SATB) plus the control
     * register the BIOS leaves set (background on, sprites on, vblank interrupt on).
     * Without them a program that only writes tile data would draw nothing, because
     * it is entitled to assume the BIOS did its part. */
    c->vdc.hdr = 0x041F;         /* 256 pixels wide */
    c->vdc.vdr = 224;            /* 224 lines */
    c->vdc.vsr = 0x0F02;         /* VDS=15, VSW=2 */
    c->vdc.mwr = 0x0050;         /* background map 64 cells wide */
    c->vdc.satb = 0x1000;        /* sprite table at VRAM word $1000 */
    c->vdc.dvssr = 0x1000;
    c->vdc.cr = 0x00C8;          /* BG on | sprites on | vblank interrupt on */
    memset(&c->vce, 0, sizeof c->vce);
    memset(&c->timer, 0, sizeof c->timer);
    c->irq_status = 0;
    c->irq_mask = 0x07;
    /* NOT SET HERE, AND THIS IS THE NEXT GAP THE DRIVE UNCOVERED.  With the card
     * executed the machine reaches the kernel's idle loop at $4174 and stays there:
     * it arrives with I set and $1402 = $07, so no IRQ1 is ever taken.  The console
     * takes IRQ1 at that same pc on frame 1,957, because on the console the card's
     * own init has run first (cdcmd.md §8: pc $CB7B writes $1402 = $00 at frame 0,
     * and the trace takes IRQ1 from frame 1).  This port enters the disc loader
     * directly instead of through the card's reset vector, so it never runs that
     * init -- and seeding the register here is not enough on its own: the card's
     * IRQ1 housekeeping then reloads CR from the work-RAM shadows $F3/$F4, which the
     * skipped init also never wrote, and turns the display off at frame 1,702.  Both
     * belong to "the state the card's init leaves", not to the drive. */
    memset(c->pad, 0, sizeof c->pad);
    c->joypad_sel = 0;
    c->joypad_clr = 0;
    c->joypad_index = 0;         /* PCEINPUT_Power: read_index = 0, SEL = 0, CLR = 0 */
}

void xl_set_pad(Cpu *c, unsigned port, uint8_t state)
{
    if (port < 5)
        c->pad[port] = state;
}

void xl_pad_press(Cpu *c, uint32_t from_frame, uint32_t to_frame, uint8_t mask)
{
    if (c->npads >= (int)(sizeof c->pads / sizeof c->pads[0])) {
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "runtime: pad table full at %d windows; later input "
                            "is being DROPPED\n", c->npads);
        }
        return;
    }
    c->pads[c->npads].from = from_frame;
    c->pads[c->npads].to = to_frame;
    c->pads[c->npads].mask = mask;
    c->npads++;
}

void xl_pad_stream(Cpu *c, const uint8_t *held, uint32_t frames, uint32_t from_frame,
                   int32_t lag)
{
    c->pad_hold = held;
    c->pad_hold_n = frames;
    c->pad_hold_from = from_frame;
    c->pad_hold_lag = lag;
}

/* Apply the schedule for the current frame.  Called at instruction boundaries; it
 * compares the frame counter first, so the per-instruction cost is one compare. */
void xl_pad_tick(Cpu *c)
{
    uint32_t frame = (uint32_t)c->vdc.frame;
    uint8_t want = 0;
    int i;
    if (c->pad_hold) {
        /* Console-driven input.  Applied when the frame counter turns over: on
         * this chip that is the start of the vertical sync, the same instant the
         * vblank interrupt fires and ~1.2 % of a frame before the game reaches
         * its pad routine -- so the state installed here is the state the game's
         * poll sees, on this frame, without the poll's own position mattering.
         * Outside the recording (before entry 0, or past the last entry) the pad
         * keeps its last value rather than falling to zero: an input that
         * silently became "nothing held" would look like the player letting go. */
        {
            /* lag > 0 means the stream is from a machine that reached this
             * frame's content `lag` frames earlier, so port frame N reads entry
             * N - lag. */
            int64_t idx = (int64_t)frame - (int64_t)c->pad_hold_from
                          - (int64_t)c->pad_hold_lag;
            const uint8_t *h;
            if (idx < 0 || idx >= (int64_t)c->pad_hold_n)
                return;
            h = c->pad_hold + (size_t)idx * 5;
            /* Pads 1..4 are set whole; pad 0 goes through `want` below so the
             * RUN-gated boot path sees the edge exactly as it does for a window. */
            for (i = 1; i < 5; i++)
                c->pad[i] = h[i];
            want = h[0];
        }
    } else {
        if (!c->npads)
            return;
        for (i = 0; i < c->npads; i++) {
            if (frame >= c->pads[i].from && frame < c->pads[i].to) {
                want = c->pads[i].mask;
                break;
            }
        }
    }
    if (getenv("XLATE_PAD_DEBUG") && frame > 1690 && frame < 1704)
        fprintf(stderr, "pad_tick frame=%u want=%02X cur=%02X npads=%d\n",
                frame, want, c->pad[0], c->npads);
    if (want != c->pad[0]) {
        c->pad[0] = want;
        if (c->boot_game_on_run && (want & 0x08u)) {
            /* RUN newly pressed.  Two things are gated on it, and both are
             * measured facts about the console, not conveniences:
             *
             *  1. THE DISC BOOT DOES NOT START UNTIL RUN.  In the trace there
             *     is no CD command before frame 1700; the first one is at 1700
             *     (the start pulse at $E91B) and the first read at 1701.  Our
             *     machine used to run the loader, the boot program and its own
             *     disc read at frame 0 and then wait inside the boot program's
             *     own PRESS RUN loop for 1,700 frames -- the same screen, but a
             *     different machine doing it, which is why every later frame was
             *     shifted.  Releasing the boot on the press puts the loader,
             *     the boot program and its CD load at the point in the game's
             *     life where the console puts them.
             *  2. The hand-off is the EVENT "the boot program has finished its
             *     start-up and asks for control", observed at pc $41E9 in bank
             *     $80 -- see xl_boot_game's caller.
             *
             * Neither applies to the reset-vector entry (bios_reset): there the
             * machine is already running the card's own PRESS RUN loop, and the
             * card -- not this port -- reads the loader and the game off the disc.
             * Marking the gate open would also silence the gate for a later RUN. */
            if (c->bios_reset) {
                if (c->log)
                    xl_logf(c, "pad: RUN at frame %u to the card's own boot loop "
                               "(reset entry: no port gate, no port hand-off)", frame);
                return;
            }
            c->boot_pressed = 1;
            c->boot_gate_open = 1;
            if (c->log)
                xl_logf(c, "boot: RUN at frame %u; releasing the disc boot, "
                           "hand-off waits for the boot program (pc $41E9)", frame);
            return;
        }
        if (c->log)
            xl_logf(c, "pad: frame %u -> $%02X%s", frame, want,
                    (want & 0x08) ? " (RUN pressed)" : (want ? "" : " (released)"));
        printf("PAD frame=%u mask=%02X pc=%04X bank=%02X\n", frame, want,
               c->pc, c->mpr[c->pc >> 13]);
        fflush(stdout);
    }
}
uint8_t *xl_frame_rgb(Cpu *c) { return c->vdc.frame_pixels; }

void xl_attach_disc(Cpu *c, const uint8_t *data, size_t len)
{
    c->disc = data;
    c->disc_len = len;
    c->disc_attached = (data != NULL);
}

void xl_load_rom(Cpu *c, const uint8_t *rom, size_t len)
{
    size_t n = len < (size_t)(XL_ROM_BANKS * XL_BANK_SIZE) ? len : (size_t)(XL_ROM_BANKS * XL_BANK_SIZE);
    memcpy(c->mem, rom, n);
    c->rom_present = 1;
}

/* The battery-backed backup RAM.  A store passes only while the card has unlocked
 * the device; a read of $1803 locks it again (see XL_BRAM_BANK in runtime.h). */
void xl_load_sram(Cpu *c, const uint8_t *data, size_t len)
{
    size_t n = len < (size_t)XL_BRAM_SIZE ? len : (size_t)XL_BRAM_SIZE;
    memset(c->bram, 0, sizeof c->bram);
    memcpy(c->bram, data, n);
    if (c->log)
        xl_logf(c, "bram: %u-byte image loaded (%02X %02X %02X %02X ...)",
                (unsigned)n, c->bram[0], c->bram[1], c->bram[2], c->bram[3]);
}

/* Used by generated init code: put bytes straight into a physical bank. */
void xl_load_phys(Cpu *c, uint8_t bank, uint16_t off, const uint8_t *data, size_t len)
{
    size_t pos = 0;
    unsigned b = bank, o = (unsigned)(off & 0x1FFF);
    while (pos < len) {
        size_t chunk = XL_BANK_SIZE - o;
        if (chunk > len - pos)
            chunk = len - pos;
        xl_stale_note(c, (uint8_t)b, (uint16_t)o, XL_SW_LOADPHYS);
        memcpy(&c->mem[(size_t)b * XL_BANK_SIZE + o], data + pos, chunk);
        pos += chunk;
        b = (b + 1) & 0xFF;
        o = 0;
    }
}

/* --------------------------------------------------------------- memory -- */

/* Hardware page decode.  $FF:0000-$0007 VDC ports, $FF:0400-$0407 VCE,
 * $FF:0C00-$0C03 timer + interrupt controller, $FF:1000-$17FF joypad.
 * Everything else in the page stays a counted stub register, so a write to an
 * unimplemented port is visible in the log instead of silently succeeding. */
/* Who writes the pattern half of VRAM (words $1000 and above)?  The intro's tile
 * upload is the one copy in this port that never happens at the right moment, so the
 * instrument records, per writing PC: how many writes, and the first and last frame
 * it was seen.  Env-gated (XLATE_VRAMHI=1) and off by default. */
typedef struct { uint16_t pc; uint8_t bank; uint32_t n; uint32_t first, last; } VramHiSlot;
static VramHiSlot vramhi[256];
static int vramhi_on = -1;

void xl_vramhi_note(Cpu *c, uint16_t pc, unsigned addr)
{
    int i, free_slot = -1;
    unsigned bank = c->mpr[pc >> 13];
    static uint32_t last_frame;
    if (vramhi_on < 0) {
        const char *e = getenv("XLATE_VRAMHI");
        vramhi_on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (!vramhi_on)
        return;
    {
        static int at = -2;
        static int done;
        if (at == -2) {
            const char *e = getenv("XLATE_VRAMHI_AT");
            at = e ? atoi(e) : -1;
        }
        {
            static int want_pc = -2;
            if (want_pc == -2) {
                const char *e = getenv("XLATE_VRAMHI_PC");
                want_pc = e ? (int)strtol(e, NULL, 16) : -1;
            }
            if (want_pc >= 0 && pc != (uint16_t)want_pc)
                return;                     /* only the named writer is of interest */
        }
        if (at >= 0 && !done && (int)c->vdc.frame >= at) {
            uint8_t lo = xl_rd(c, (uint16_t)(XL_STACK_BASE | ((c->s + 1) & 0xFF)));
            uint8_t hi = xl_rd(c, (uint16_t)(XL_STACK_BASE | ((c->s + 2) & 0xFF)));
            uint16_t ret = (uint16_t)((lo | (hi << 8)) + 1);
            done = 1;
            if (c->log) {
                xl_logf(c, "first pattern write at frame %u: pc=$%04X bank $%02X "
                           "mawr=$%04X caller=$%04X(bank $%02X) in_irq=%d",
                        c->vdc.frame, pc, bank, c->vdc.mawr & 0x7FFF, ret,
                        c->mpr[ret >> 13], c->in_irq);
                xl_logf(c, "  interpreter: $13:$12=%02X%02X $15:$14=%02X%02X "
                           "$17:$16=%02X%02X $11=%02X",
                        xl_rd(c, 0x2013), xl_rd(c, 0x2012), xl_rd(c, 0x2015),
                        xl_rd(c, 0x2014), xl_rd(c, 0x2017), xl_rd(c, 0x2016),
                        xl_rd(c, 0x2011));
                xl_logf(c, "  tasks $3C80=$%02X $3C88=$%02X $3C90=$%02X $3C98=$%02X",
                        xl_rd(c, 0x3C80), xl_rd(c, 0x3C88), xl_rd(c, 0x3C90),
                        xl_rd(c, 0x3C98));
            }
        }
    }
    last_frame = (uint32_t)c->vdc.frame;
    for (i = 0; i < 256; i++) {
        if (vramhi[i].n && vramhi[i].pc == pc && vramhi[i].bank == bank) {
            vramhi[i].n++;
            vramhi[i].last = last_frame;
            return;
        }
        if (!vramhi[i].n && free_slot < 0)
            free_slot = i;
    }
    if (free_slot >= 0) {
        vramhi[free_slot].pc = pc;
        vramhi[free_slot].bank = (uint8_t)bank;
        vramhi[free_slot].n = 1;
        vramhi[free_slot].first = last_frame;
        vramhi[free_slot].last = last_frame;
    }
    (void)addr;
}

void xl_vramhi_summary(FILE *out)
{
    int i;
    if (vramhi_on <= 0)
        return;
    fprintf(out, "VRAMHI");
    for (i = 0; i < 256; i++)
        if (vramhi[i].n)
            fprintf(out, " %04X@%02X:n=%u,frames=%u-%u", vramhi[i].pc, vramhi[i].bank,
                    vramhi[i].n, vramhi[i].first, vramhi[i].last);
    fprintf(out, "\n");
    fflush(out);
}

/* Refill the CD data port from the disc: the drive hands the program one sector at
 * a time, and `sectors_left` is how many the last command asked for.  This is the
 * same data the CD read service moved into CD RAM, so the two paths cannot disagree
 * about what the disc holds. */
static void cd_refill(Cpu *c)
{
    Cd *cd = &c->cd;
    if (cd->pos < cd->len || !cd->sectors_left)
        return;
    if (!c->disc_attached) {
        cd->sectors_left = 0;
        return;
    }
    {
        size_t off = ((size_t)cd->next_sector + 225u) * 2048u;
        if (off + 2048u > c->disc_len) {
            cd->sectors_left = 0;
            return;
        }
        memcpy(cd->buf, c->disc + off, 2048u);
        cd->len = 2048;
        cd->pos = 0;
        cd->next_sector++;
        cd->sectors_left--;
    }
}

void xl_cd_prime(Cpu *c, unsigned record, unsigned sectors)
{
    Cd *cd = &c->cd;
    cd->next_sector = record;
    cd->sectors_left = sectors;
    cd->len = cd->pos = 0;
    cd->sectors_queued = sectors;
    cd_refill(c);
}

/* ------------------------------------------------------------ transfer rate --
 *
 * THE DRIVE MOVES ONE SECTOR PER PERIOD, AND THE PERIOD IS NOT A FRAME.
 *
 * The reference core's drive is a 2048-byte FIFO filled by a countdown
 * (cdrom/scsicd.cpp, `RunCDRead`).  A Read(6) sets
 *
 *     CDReadTimer = 3 * 2048 * System_Clock / CD_DATA_TRANSFER_RATE;
 *
 * and every fill after that does `CDReadTimer += 2048 * System_Clock / rate`: the
 * cadence is one sector per period, anchored to the command, not to the video frame.
 * With the constants this core is built with,
 *
 *     System_Clock = 21,477,272   (PCE_MASTER_CLOCK, truncated by SCSICD_Init's
 *                                  uint32_t parameter)
 *     rate         = 126,000 * 1  (pcecd.cpp: 126000 * settings->CD_Speed, and
 *                                  pce.cdspeed defaults to 1)
 *     period       = 2048 * 21,477,272 / 126,000 = 349,090 master clocks
 *                  = 116,363.3 CPU cycles = 0.9724 of a frame.
 *
 * THIS PORT USED A WHOLE FRAME (120,000 cycles), WHICH IS 3.1% TOO SLOW.  It looks
 * harmless -- the console's trace reads "one sector per frame, without exception" --
 * but it is not: over the 171 sectors of the boot load it is 5 frames of accumulating
 * drift, and that is what leaves the game's frame counter three frames behind the
 * console's.  The console's own sector trace agrees with the core's number and not
 * with a round frame: read #27 (accepted f=9874, 32 sectors) hands sectors over
 * f=9876..f=9907 -- 33 frames for 32 sectors -- and its READEND is f=9908.
 *
 * Everything is driven by xl_tick() -- the emulated clock -- so the transfer is a
 * function of emulated time and nothing else.  `XLATE_READTIME=1` enables it (it is
 * the configuration every measurement in out/ was taken with).
 */
/* ONE FRAME, and it is only the *modelled* path's coarse unit (xl_cd_move, used
 * when the System Card is not executed): it is NOT the drive's rate, and the 335
 * cycles it is longer than a real frame (119,665) are exactly the error described
 * above.  The decoder path below uses the drive's own period instead. */
#define XL_CD_FRAME_CYCLES 120000u
/* One sector's worth of the drive's own clock, in master-clock units (see above).
 *
 * NOW GEARGRAFX'S NUMBER, NOT MEDNAFEN'S.  Above is Mednafen's 126,000 B/s, which is
 * a 1x drive reading 2,048 bytes a sector at 61.5 sectors a second -- 22% slow.  The
 * target is Geargrafx (see docs/REFERENCE.md), and its sector clock is
 *
 *   src/cdrom_media_inline.h:192-196
 *     u32 CdRomMedia::SectorTransferCycles()
 *     {   // Standard CD-ROM 1x speed: 75 sectors/sec
 *         return GG_MASTER_CLOCK_RATE / 75;
 *     }
 *
 * 21,477,273 / 75 = 286,363 master clocks = 0.7976 of this model's frame: a seek
 * plus one sector period, which is the whole cost of a `Read(6)` here.  The 126,000
 * figure this replaces was never a measurement of this drive: it is one core's
 * `126000 * settings->CD_Speed`, and the trace that looked like it agreed with it
 * ("33 frames for 32 sectors") came from an emulator running that same constant. */
#define XL_CD_PERIOD_MASTER 286363u
/* A BYTE LEAVES THE DRIVE'S FIFO ONE AT A TIME, AND THE CLOCK IS THE ACK DELAY.
 *
 * The reference core hands the host a byte in `read_1808` and then holds ACK for
 * `ClearACKDelay = 15 * 3` master clocks (pcecd.cpp), so a 2,048-byte sector takes
 * 2,048 * 45 = 92,160 master clocks = 30,720 CPU cycles = 0.2567 of a frame to leave
 * the FIFO, whoever is reading it -- the CPU through $1808, or the drive itself when
 * `$180B & 3` is set and PCECD_Run drains it into the ADPCM sample RAM.
 *
 * This is the tail the port did not model: it completed a drive-side transfer the
 * instant the last sector LANDED.  Measured on the console's own trace, frame
 * 24,433 (the 32-sector ad_trans that starts the second cutscene): the drive's
 * transfer-done ($1803 bit 5, read at pc $F3D9) rises at phase 0.847 of the frame;
 * ours rose at 0.590 -- 0.257 of a frame early, which is this number exactly. */
#define XL_CD_DRAIN_MASTER  (2048u * 45u)
/* One byte of that drain, in master clocks: the FIFO's acknowledge delay.  The
 * reference core holds ACK for `ClearACKDelay = 15 * 3` after every byte handed out
 * by `read_1808`, whoever reads it. */
#define XL_CD_BYTE_MASTER   45u
/* c->cycles counts CPU cycles; the reference's countdown is in master clocks, three
 * to a CPU cycle, so the period's third of a cycle survives the comparison. */
#define XL_CD_M3(cyc) ((uint64_t)(cyc) * 3u)

/* ------------------------------------------------------------------ seeking --
 *
 * HOW LONG THE HEAD TAKES TO REACH A SECTOR IS A MEASURED QUANTITY.  It is not a
 * constant, and it is not an emulator's shortcut either.
 *
 * The numbers below are pce-devel/PCECD_seek (`seektime.c`, Dave Shadoff, 2018):
 * seek time measured on re-capped PC Engine CD hardware at VSYNC resolution, in
 * fourteen CLV zone groups.  The zone table is that file's `sector_list` verbatim
 * (sectors per revolution, first/last sector, one revolution in VSYNCs), and the
 * step function is its algorithm, unchanged:
 *
 *   |delta| < 2 sectors      9                                 (= 150 ms)
 *   |delta| < 5              9  + rotation/2                   (= 150 ms + rot/2)
 *   tracks <= 80             18 + rotation/2                   (= 300 ms + rot/2)
 *   tracks <= 160            22 + rotation/2                   (= 366 ms + rot/2)
 *   tracks <= 644            22 + rotation/2 + (tracks - 161) / 80
 *   otherwise                48 + (tracks - 644) / 195          (VSYNCs)
 *
 * The table above was transcribed as 3 / 16 / 36 for the first, third and last
 * bands -- a third, an eighth and a third of Geargrafx's numbers -- and that is the
 * defect this comment used to hide: every short seek came out 6 frames early and
 * every long one 12.  Geargrafx's own bands are quoted in `xl_gg_seek_ms()` below
 * (`cdrom_media.cpp:387-399`), and every seek in the port now goes through it.
 *
 * `tracks` is the head travel in revolutions of the destination zone; a seek
 * within one zone is |delta| / sectors-per-revolution, across zones the partial
 * zones plus 1,606.48 revolutions for every whole zone crossed.  One VSYNC is one
 * field, i.e. this model's frame (VD_LINES * VD_DOTS master clocks).
 *
 * WHAT THIS REPLACES, AND WHY THAT WAS WORSE.  Both emulators this project has
 * been measured against throw the distance away:
 *
 *   Mednafen 1.29 (the code TurboNyma runs)  CDReadTimer = 8 sector periods
 *                                            (cdrom/scsicd.cpp, DoREADBase)
 *   0.9.38-era libretro fork (Beetle PCE)    3 sector periods
 *
 * Neither is a measurement of anything: a read at the sector the head is already
 * on and a read a hundred sectors away cost exactly the same, and the game cannot
 * tell.  Hardware does not behave that way, and the difference lands precisely on
 * the reads games care about -- the jump into a new area of the disc, which on
 * this disc is 6-10 revolutions and 20 VSYNCs against the emulators' 3.
 *
 * Keep the streaming rate as it is: one sector per XL_CD_PERIOD_MASTER (2048 bytes
 * at 126,000 B/s), which is the drive's real 1x rate and is what the table's
 * rotations-per-sector imply.  Only the *first* sector of a command moves. */
/* ONE VSYNC IS ONE FRAME, AND A FRAME IS 358,995 MASTER CLOCKS, NOT 119,665.
 * VD_LINES * VD_DOTS counts DOTS (455 x 263), and a dot is THREE master clocks --
 * which is the very arithmetic XL_CD_PERIOD_MASTER above is built on
 * (2048 * 21,477,272 / 126,000 = 349,090 master clocks, and 21,477,272 / 59.83 =
 * 358,995 master clocks a frame).  The seek table's values are hundredths of a
 * VSYNC, so converting them with a third of a frame charged every seek at a THIRD
 * of the measured curve -- and the log line, dividing by the same constant, printed
 * the intended figure, which is why the machine and its instrument disagreed by 3x
 * while both looked right. */
#define XL_CD_VSYNC_MASTER ((uint64_t)VD_LINES * VD_DOTS * 3u)   /* 358,995 master clocks */

typedef struct {
    uint8_t  sec_per_rev;       /* sectors per revolution in this zone */
    uint32_t first, last;       /* its sector range */
    uint16_t rot_centi;         /* one revolution, in 1/100 VSYNC */
} XlCdZone;

static const XlCdZone xl_cd_zone[] = {
    { 10,      0u,   12572u,  800u },
    { 11,  12573u,   30244u,  881u },
    { 12,  30245u,   49523u,  961u },
    { 13,  49524u,   70408u, 1041u },
    { 14,  70409u,   92900u, 1121u },
    { 15,  92901u,  116998u, 1201u },
    { 16, 116999u,  142703u, 1281u },
    { 17, 142704u,  170014u, 1361u },
    { 18, 170015u,  198932u, 1442u },
    { 19, 198933u,  229456u, 1522u },
    { 20, 229457u,  261587u, 1602u },
    { 21, 261588u,  295324u, 1682u },
    { 22, 295325u,  330668u, 1762u },
    { 23, 330669u,  333012u, 1842u },
};
#define XL_CD_ZONES (sizeof xl_cd_zone / sizeof xl_cd_zone[0])

static unsigned xl_cd_zone_of(uint32_t lba)
{
    unsigned i;
    for (i = 0; i < XL_CD_ZONES; i++)
        if (lba <= xl_cd_zone[i].last)
            return i;
    return XL_CD_ZONES - 1u;
}

/* THE SEEK ITSELF IS `xl_gg_seek_ms()` BELOW -- one implementation, Geargrafx's.
 *
 * Until 2026-11-01 there were two: a hand transcription of Shadoff's step function
 * in hundredths of a VSYNC, used by `Read(6)`, and `xl_gg_seek_ms()` in Geargrafx's
 * milliseconds, used by `$D8`.  The transcription's bands were 3/16/22/36 where
 * Geargrafx's are 9/18/22/48, so every short `Read(6)` seek was 6 frames short and
 * every long one 12, and the port's `Read(6)` burst in the intro finished 40 frames
 * before the console's while the console's own counter was in step with the
 * port's -- a 65-frame disagreement between the picture and the state. */
/* Geargrafx's seek arithmetic, in ITS units -- and now the ONLY seek arithmetic in
 * this file.  `CdRomMedia::SeekTime()` (`cdrom_media.cpp:359-398`) returns
 * MILLISECONDS from Shadoff's zone table, and `TimeToCycles()` (`cdrom_common.h:74`)
 * turns that into master clocks at 21,477,273 Hz.
 *
 * Every command that seeks uses it: `$D8`/`$D9` (below) and `Read(6)` (`0x08`).  The
 * alternative this comment used to defend -- a copy of the same algorithm rounding
 * to hundredths of a VSYNC -- is why a measured 36-frame stall came out 37 and why
 * every short `Read(6)` seek was six frames short; see the note above on
 * `xl_gg_seek_ms()`'s callers, and docs/REFERENCE.md "Disc drive". */
#define XL_GG_MASTER_CLOCK 21477273u
static uint32_t xl_gg_time_to_cycles(uint32_t us)
{
    return (uint32_t)(((uint64_t)us * XL_GG_MASTER_CLOCK) / 1000000ULL);
}

/* `CdRomAudio::k_playback_delay_us` (cdrom_audio.h:125): the head has this long to
 * be up to speed after a seek, and it is SUBTRACTED from the seek, not added to it
 * (`cdrom_audio_inline.h:152-157`). */
#define XL_CD_PLAYBACK_DELAY_US 224000u

/* `rotation_ms` from Geargrafx's table (`cdrom_media.h:119-136`), derived from this
 * file's own `rot_centi`: one zone revolution is that many hundredths of a VSYNC and
 * a VSYNC is 16.683 ms, which is the same 133.47 ms they print for zone 0. */
static double xl_gg_rotation_ms(unsigned zone)
{
    return (double)xl_cd_zone[zone].rot_centi * 16.683 / 100.0;
}

/* Geargrafx's SeekTime(), milliseconds, in its own arithmetic: `track_difference` is
 * a double, the integer divisions inside it are integer divisions there, and the
 * whole band expression is cast to u32 once at the end. */
static uint32_t xl_gg_seek_ms(uint32_t from, uint32_t to)
{
    unsigned si = xl_cd_zone_of(from), ti = xl_cd_zone_of(to);
    uint32_t d = (from > to) ? from - to : to - from;
    double td = 0.0;                /* head travel, revolutions */
    double rot = xl_gg_rotation_ms(ti);

    if (ti == si) {
        td = (double)(d / xl_cd_zone[ti].sec_per_rev);
    } else if (ti > si) {
        td  = (double)((xl_cd_zone[si].last - from) / xl_cd_zone[si].sec_per_rev);
        td += (double)((to - xl_cd_zone[ti].first) / xl_cd_zone[ti].sec_per_rev);
        td += 1606.48 * (double)(ti - si - 1u);
    } else {
        td  = (double)((from - xl_cd_zone[si].first) / xl_cd_zone[si].sec_per_rev);
        td += (double)((xl_cd_zone[ti].last - to) / xl_cd_zone[ti].sec_per_rev);
        td += 1606.48 * (double)(si - ti - 1u);
    }

    if (d < 2u)
        return (uint32_t)(9 * 1000 / 60);
    if (d < 5u)
        return (uint32_t)((9 * 1000 / 60) + rot / 2.0);
    if (td <= 80.0)
        return (uint32_t)((18 * 1000 / 60) + rot / 2.0);
    if (td <= 160.0)
        return (uint32_t)((22 * 1000 / 60) + rot / 2.0);
    if (td <= 644.0)
        return (uint32_t)((22 * 1000 / 60) + rot / 2.0
                          + ((td - 161.0) * 16.66 / 80.0));
    return (uint32_t)((48 * 1000 / 60) + ((td - 644.0) * 16.66 / 195.0));
}

/* What `$D8` holds the bus for: the seek, less the playback delay. */
static uint64_t xl_gg_audio_seek_cycles(uint32_t from, uint32_t to)
{
    uint32_t total = xl_gg_time_to_cycles(xl_gg_seek_ms(from, to) * 1000u);
    uint32_t delay = xl_gg_time_to_cycles(XL_CD_PLAYBACK_DELAY_US);
    return (total > delay) ? (uint64_t)(total - delay) : 0u;
}

static int xl_cd_paced(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XLATE_READTIME");
        on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return on;
}

/* Where a CD transfer's bytes go.  XL_CD_TO_RAM is the normal case -- the CPU address
 * in $FA/$FB, stored through the MPRs.  XL_CD_TO_CDRAM is the held-back wrong
 * destination for the boot program's VRAM modes (see the note at its call site).
 * XL_CD_TO_ADPCM is ad_trans ($E033): the drive fills its own ADPCM sample RAM, so the
 * bytes cost their flight time but touch no CPU memory at all (measured -- see $E033). */
enum { XL_CD_TO_RAM = 0, XL_CD_TO_CDRAM = 1, XL_CD_TO_ADPCM = 2 };

/* Copy `n` bytes of the disc image from `disc_off` into the machine, one 2 KB sector
 * per frame; `dest` is the CPU address the program asked for (`$FA/$FB`, or the
 * boot program's VRAM word address -- see the mode-$FE/$FF note at the call site).
 * Returns the number of frames the transfer consumed. */
static unsigned xl_cd_move(Cpu *c, size_t disc_off, size_t n, uint16_t dest,
                           int where)
{
    size_t pos;
    unsigned frames = 0;
    int paced = xl_cd_paced();

    if (paced) {
        xl_tick(c, XL_CD_FRAME_CYCLES);           /* drive latency: first sector */
        xl_tick(c, XL_CD_FRAME_CYCLES);           /* lands two frames after accept */
        frames += 2;
    }
    for (pos = 0; pos < n; pos += 2048u) {
        size_t chunk = (n - pos < 2048u) ? n - pos : 2048u;
        if (where == XL_CD_TO_CDRAM) {
            memcpy(&c->mem[(size_t)XL_CDRAM_BASE * XL_BANK_SIZE + (dest & 0x1FFF) + pos],
                   c->disc + disc_off + pos, chunk);
        } else if (where == XL_CD_TO_RAM) {
            size_t i;
            /* A sector is a burst of stores through the MPRs, exactly as the BIOS's
             * own copy loop does them (`sta [_bl],Y`, ROM $EA8B): the bank is
             * whichever one is mapped over the address at that moment. */
            for (i = 0; i < chunk; i++)
                xl_wr(c, (uint16_t)(dest + (uint16_t)(pos + i)),
                      c->disc[disc_off + pos + i]);
        }
        if (paced) {
            xl_tick(c, XL_CD_FRAME_CYCLES);       /* the next sector, one frame on */
            frames++;
        }
    }
    return frames;
}

/* ------------------------------------------------------------------ the drive --
 *
 * A REAL COMMAND DECODER AT $1800-$180F, SO AN EXECUTED SYSTEM CARD CAN DRIVE IT.
 *
 * Hudson's driver (the card's own code, bank $000 $E905-$E9C4) never touches the
 * disc itself: it selects the drive by writing $1800, pushes the command bytes one
 * at a time against the drive's REQ line, and then reads the answer back out of the
 * same registers.  This is that drive.  Everything below is measured against the
 * core's own decoder, line by line against the console's register trace, not
 * invented:
 *
 *   - the status byte in $1800 is BSY/REQ/MSG/CD/IO in bits 7..3.  The card reads it
 *     with `and #$f8` and compares against $D0 (command+REQ), $D8 (status+REQ),
 *     $C8 (data+REQ), $88 (data, nothing ready), $98 (status, ACK asserted),
 *     $F8/$B8 (message in), $00 (bus free).
 *   - a command byte is latched from $1801 when ACK rises ($1802 bit 7).
 *   - the drive hands its own bytes over on $1801 (status, message, short responses)
 *     or through $1808, the data port that acknowledges as it is read.
 *   - six bytes for $00/$08/$03, ten for $DE and for the audio commands; an unknown
 *     opcode is latched to ten and then rejected (the core's `want=10`).
 *   - a Read(6) is accepted, then the phase stays COMMAND with REQ clear ($90) while
 *     the drive seeks; the first sector appears three frames after the command was
 *     accepted and one more each frame after that (f=1748 command, sectors f=1751..
 *     f=1758, READEND f=1758).  The bytes themselves go out through $1808.
 *
 * The identity pins at $18C0-$18C7 are part of the same device and are what makes
 * the card recognise a Super System Card (its `ex_memopen`, ROM $FE92, reads $18C5
 * then $18C1-$18C3; the console answers $FF, $AA, $55, $03).
 *
 * Nothing here is reached with XLATE_BIOS_EXEC off: with the models in charge, no
 * code in the machine reads or writes a CD register at all -- every one of the
 * console's 1.1 million CD-register accesses in the 9,916-frame trace comes from
 * card code -- so this decoder cannot move a modelled run.  That is checked by the
 * gates, not assumed.
 */

/* LBA of byte 0 of the attached payload.  The payload is the data track's
 * 2048-byte user data starting at its pregap, and this disc's track 1 is 3,665
 * sectors long (`record = lba - 3890`, i.e. `(record + 225) * 2048`).  It is the
 * same fact the model path already encodes as `+225`. */
#define XL_DISC_LBA0 3665u

/* The disc's table of contents, which is what Get Dir Info ($DE) reports.  Track
 * numbers are BCD as the command carries them; `lba` is INDEX 01 in the core's
 * numbering (0 = MSF 00:02:00); `type` is 0 audio / 4 data.
 *
 * Measured from the disc's cue sheet and the track files' sizes, and checked
 * against the console's own answers: mode 2 track 1 = 00:02:00, track 2 =
 * 00:53:65, track $22 = 46:58:62, leadout = 49:12:12 (cdcmd-9916-wall.log, f=1701
 * and f=1711).  The port has no general TOC input yet, so this is the disc it is
 * built for -- the same caveat the payload mapping above already carries. */
typedef struct { uint8_t track; uint32_t lba; uint8_t type; } XlTocEntry;
static const XlTocEntry xl_toc[] = {
    { 0x01,      0u, 0 },   /* 00:02:00 */
    { 0x02,   3890u, 4 },   /* 00:53:65 */
    { 0x03,  14189u, 0 },   /* 03:11:14 */
    { 0x04,  22173u, 0 },   /* 04:57:48 */
    { 0x05,  26917u, 0 },   /* 06:00:67 */
    { 0x06,  34134u, 0 },   /* 07:37:09 */
    { 0x07,  40411u, 0 },   /* 09:00:61 */
    { 0x08,  45260u, 0 },   /* 10:05:35 */
    { 0x09,  58635u, 0 },   /* 13:03:60 */
    { 0x10,  66403u, 0 },   /* 14:47:28 */
    { 0x11,  78455u, 0 },   /* 17:28:05 */
    { 0x12,  86083u, 0 },   /* 19:09:58 */
    { 0x13,  97368u, 0 },   /* 21:40:18 */
    { 0x14, 108444u, 0 },   /* 24:07:69 */
    { 0x15, 124926u, 0 },   /* 27:47:51 */
    { 0x16, 135835u, 0 },   /* 30:13:10 */
    { 0x17, 150506u, 0 },   /* 33:28:56 */
    { 0x18, 157229u, 0 },   /* 34:58:29 */
    { 0x19, 164899u, 0 },   /* 36:40:49 */
    { 0x20, 173900u, 0 },   /* 38:40:50 */
    { 0x21, 192012u, 0 },   /* 42:42:12 */
    { 0x22, 211262u, 4 },   /* 46:58:62 */
};
#define XL_TOC_COUNT   (sizeof xl_toc / sizeof xl_toc[0])
#define XL_TOC_LEADOUT 221262u          /* 49:12:12 */

static const char *cd_op_name(uint8_t op)
{
    switch (op) {
    case 0xFF: return "invalid (probe)";
    case 0x00: return "Test Unit Ready";
    case 0x03: return "Request Sense";
    case 0x08: return "Read(6)";
    case 0xD8: return "Set Audio Playback Start Position";
    case 0xD9: return "Set Audio Playback End Position";
    case 0xDA: return "Pause";
    case 0xDD: return "Read Subchannel Q";
    case 0xDE: return "Get Dir Info";
    }
    return "unknown";
}

static unsigned cd_cmd_bytes(uint8_t op)
{
    switch (op) {
    case 0x00: case 0x03: case 0x08: return 6;
    case 0xD8: case 0xD9: case 0xDE: return 10;
    }
    return 10;                       /* what the core latches before rejecting */
}

/* One byte of the busy/phase encoding, without REQ. */
static uint8_t cd_phase_bits(unsigned phase)
{
    switch (phase) {
    case CD_PHASE_COMMAND: return 0x90;      /* BSY | CD */
    case CD_PHASE_DATA_IN: return 0x88;      /* BSY | IO */
    case CD_PHASE_STATUS:  return 0x98;      /* BSY | CD | IO */
    case CD_PHASE_MESSAGE: return 0xB8;      /* BSY | MSG | CD | IO */
    case CD_PHASE_BUSY:    return 0x80;      /* BSY alone (Geargrafx SetPhase(BUSY)) */
    }
    return 0x00;                             /* bus free */
}

static int cd_req(const Cd *cd)
{
    return cd->req;
}

static uint8_t cd_status_byte(const Cd *cd)
{
    uint8_t b = cd_phase_bits(cd->phase);
    /* REQ is deasserted while the host holds ACK: that is the handshake the card's
     * own driver depends on (`jsr $EA27`: ACK on, then `tst #$40,$1800` until REQ
     * falls, then ACK off).  Without it a data byte never retires and the driver
     * waits for ever -- measured, it cost 52 M steps before this was right. */
    if (cd->req && !cd->ack)
        b |= 0x40;
    return b;
}

/* The drive's status register and its IRQ2 line are the same bits.  $1803 holds
 * the sources (bit 2/3 ADPCM half/end, bit 4 subchannel, bit 5 transfer done,
 * bit 6 transfer ready), $1802 holds the mask, and the line rises while
 * (mask & status) is nonzero -- that is the reference core's `update_irq_state()`
 * literally: `irq = _Port[2] & _Port[0x3] & (0x4|0x8|0x10|0x20|0x40)`.
 *
 * Keeping one variable behind both is the point.  The port used to DERIVE bit 5
 * from the phase ("no command is open"), which made it permanently true and is
 * the save menu's first divergence; a derived summary can always disagree with
 * the register the card reads, an event cannot.
 *
 * The modelled path (`!bios_exec`) keeps the old composition untouched: nothing
 * there reads $1803, and the default path must stay bit for bit what it was. */
static void cd_irq_update(Cpu *c)
{
    Cd *cd = &c->cd;
    if (c->bios_exec) {
        cd->irq_status = (uint8_t)(cd->port3 & 0x7C);
        return;
    }
    {
        uint8_t a = 0;
        if (cd->phase == CD_PHASE_DATA_IN && cd->req)
            a |= 0x40;
        if ((cd->phase == CD_PHASE_STATUS || cd->phase == CD_PHASE_MESSAGE) && cd->req)
            a |= 0x20;
        cd->irq_status = a;
    }
}

/* A dispatch that ends in CHECK CONDITION instead of data: counted once, under both
 * the total and the opcode, so the census can say which command was refused. */
static void cd_reject(Cd *cd)
{
    cd->cmds_bad++;
    cd->op_bad[cd->cmd[0]]++;
}

static void cd_to_status(Cpu *c, uint8_t status)
{
    Cd *cd = &c->cd;
    int had_data = (cd->data_kind != CD_DATA_NONE) || (cd->phase == CD_PHASE_DATA_IN);
    {
        /* Debug aid: XLATE_CD_PHASE_LOG=1 names every move into the status phase,
         * with the frame and the pc that caused it.  It is how "the bus was left
         * busy when the card next selected" is answered from the log instead of
         * from a guess. */
        static int pl = -1;
        if (pl < 0) {
            const char *e = getenv("XLATE_CD_PHASE_LOG");
            pl = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (pl && c->log)
            xl_logf(c, "cdphase f=%u pc=$%04X phase %u -> status $%02X "
                       "(data kind %u, served %u/%u, port3 $%02X)",
                    (unsigned)c->vdc.frame, c->pc, cd->phase, status,
                    cd->data_kind, cd->read_served, cd->read_count, cd->port3);
    }
    cd->phase = CD_PHASE_STATUS;
    cd->status_byte = status;
    cd->db = status;
    cd->req = 1;
    cd->consumed = 0;
    cd->data_kind = CD_DATA_NONE;
    cd->data_have = 0;
    /* The data hand-over is finished: "a byte is ready" falls, and "transfer
     * done" rises.  This is the bit the card's ad_trans polls at ROM $F3D9 before
     * it reads the status phase, and the bit that must NOT stay set after the bus
     * goes free -- that latch was the port's defect.
     *
     * DONE IS ONLY RAISED BY A DATA TRANSFER.  Measured on the console's own trace:
     * only 25 of its 465,919 $1803 reads ever see bit 5, every one of them either
     * in the ad_trans poll ($F3D9) or in the IRQ handler ($E748) right after a
     * sector transfer.  The card abandons the status phase of a command with no
     * data (at frame 7,590 it reads $1800 = $D8 for the SAPEP that follows the
     * pause and never takes it); the console's next poll at 7,591 answers $16,
     * i.e. bit 5 clear -- so a command-only status does not raise it.  Leaving the
     * bit out there is also what keeps `ad_stat` from seeing a permanent "done"
     * (the save menu's first divergence was that bit stuck at 1 through a summary
     * that had nothing to do with the drive). */
    cd->port3 &= (uint8_t)~0x40;
    if (had_data)
        cd->port3 |= 0x20;
    cd_irq_update(c);
}

/* A Read(6) has handed over its last sector: report the frames it took and take the
 * result into the status phase.  The console's own drive is one sector per frame
 * after a three-frame access (cdcmd.md §7), so this line is the measurement of that
 * rate on our side. */
static void cd_read_done(Cpu *c)
{
    Cd *cd = &c->cd;
    if (c->log)
        xl_logf(c, "cd: read lba %u + %u sectors delivered (accepted at frame %u, "
                   "%u frames later, %u blocked fills, end phase %u)",
                cd->read_lba, cd->read_count, cd->read_frame,
                (unsigned)c->vdc.frame - cd->read_frame, cd->blocked_fills,
                (unsigned)(c->cycles % (uint64_t)(VD_LINES * VD_DOTS)));
    cd->blocked_fills = 0;
    cd_to_status(c, 0x00);
}

/* A sector of the Read(6) in flight has left the drive: move on, or finish. */
static void cd_data_next(Cpu *c)
{
    Cd *cd = &c->cd;
    if (cd->data_kind == CD_DATA_RESP) {
        cd->resp_pos++;
        if (cd->resp_pos < cd->resp_len) {
            cd->db = cd->resp[cd->resp_pos];
            cd->req = 1;
            cd->data_have = 1;
        } else {
            cd_to_status(c, 0x00);
            return;
        }
    } else if (cd->data_kind == CD_DATA_SECTORS) {
        cd->data_pos++;
        if (cd->data_pos < 2048u) {
            cd->db = cd->buf[cd->data_pos];
            cd->req = 1;
        } else {
            cd->read_served++;
            cd->data_pos = 0;
            cd->data_have = 0;
            cd->req = 0;
            if (cd->status_b & 0x03) {
                /* The drive was pulling this sector into the chip's sample RAM
                 * (adpcm_inline.h:191-226); it has all gone in, and the next one is
                 * offered by cd_pump. */
                cd->drain_end = 0;
                if (c->log && getenv("XLATE_ADPCM"))
                    xl_logf(c, "cd: sector %u of %u in sample RAM (ADPCM write addr "
                               "$%04X) at frame %u", cd->read_served, cd->read_count,
                            cd->adpcm_writeaddr, (unsigned)c->vdc.frame);
            }
            /* One past the last sector served: `ReadSector()` sets the media's
             * current sector to `lba + 1` (cdrom_cuebin_image.cpp:288). */
            cd->head_lba = cd->read_lba + cd->read_served;
            /* The sector's last byte has left the FIFO: "a byte is ready"
             * ($1803 bit 6) falls here, not when the next sector arrives. */
            cd->port3 &= (uint8_t)~0x40;
            if (cd->read_served >= cd->read_count) {
                cd_read_done(c);
                return;
            }
        }
    }
    cd_irq_update(c);
}

/* Load the sector that is due now.  Returns 0 if the disc cannot supply it. */
static int cd_load_sector(Cpu *c)
{
    Cd *cd = &c->cd;
    unsigned lba = cd->read_lba + cd->read_served;
    size_t off;

    if (!c->disc_attached || lba < XL_DISC_LBA0)
        return 0;
    off = (size_t)(lba - XL_DISC_LBA0) * 2048u;
    if (off + 2048u > c->disc_len)
        return 0;
    memcpy(cd->buf, c->disc + off, 2048u);
    cd->data_pos = 0;
    cd->db = cd->buf[0];
    /* THE HEAD IS ONE PAST THE SECTOR IT JUST SERVED.  `CdRomCueBinImage::ReadSector()`
     * sets `m_current_sector = lba + 1` (cdrom_cuebin_image.cpp:164) -- measured in the
     * same trace as the audio head above: reading 4159 leaves the media at 4160, and
     * after the 8-sector read of 4259..4266 the media reads 4267
     * at frames 8266 and 8343.  A command issued afterwards is
     * therefore priced from lba+1; a read of the sectors that follow this one is a
     * zero-distance move, which `SeekTime()` prices at its 150 ms floor
     * (`lba_difference < 2` -> `9 * 1000 / 60`), not the 216 ms a two-sector move
     * would cost. */
    cd->head_lba = lba + 1u;
    return 1;
}

static void cd_cdda_tick(Cpu *c);   /* the audio head; defined with the CD-DA block */
static void cd_busy_poll(Cpu *c);   /* release a command the drive took its time over */

/* Called whenever the host looks at the drive while a Read(6) is running: a sector
 * becomes available once per emulated frame and its bytes leave at the disc's data
 * rate, so the CPU's copy of a sector costs exactly one frame (see XL_CD_BYTE).
 *
 * THE ADPCM FORM IS NOT A DROP.  While `$180B` bits 0-1 are set the drive is filling
 * its OWN 64 KB sample RAM, and the reference core takes those bytes itself instead of
 * handing them to the CPU (pcecd.cpp, ADPCM_Run):
 *
 *     if(ADPCM.WritePending <= 0) { if(_Port[0xb] & 0x3) { SCSICD_Run(timestamp);
 *         if(!SCSICD_GetCD() && SCSICD_GetIO() && SCSICD_GetREQ() && !SCSICD_GetACK())
 *           { ADPCM.WritePendingValue = read_1808(timestamp, false);
 *             ADPCM.WritePending = 10 * 3; } } }
 *
 * The sector is therefore OFFERED into the same FIFO $1808 reads (that is what makes
 * the card's handshake complete and what raises $1803 bit 6), and then taken byte by
 * byte in cd_adpcm_fill().  It used to be thrown away here, which is why a 64 KB drive
 * transfer left the sample RAM empty and every later `ad_read` copied zeros into work
 * RAM -- the defect that zeroed CD RAM bank $7E (REPORT-freeze.md §4).
 *
 * The `$180B` bit can arrive *after* the first sector is already on the bus -- ad_trans
 * sets it immediately after the command handshake, and the handshake itself does not
 * finish until the first sector is offered -- so it is read at the offer, not before
 * it, or the transfer stalls with the card waiting for a command that never completes
 * (measured: 5.5 M steps of $F3D0-$F3D9, one deadlock). */
static void cd_pump(Cpu *c)
{
    Cd *cd = &c->cd;

    if (cd->data_kind != CD_DATA_SECTORS)
        return;
    for (;;) {
        if (cd->read_served >= cd->read_count) {
            /* A DRIVE-SIDE SECTOR IS NOT FINISHED WHEN IT LANDS.
             *
             * The reference core raises "transfer done" ($1803 bit 5, the bit the
             * card's ad_trans polls at ROM $F3D9) from the LAST BYTE to leave the
             * FIFO, not from the fill.  Finishing at the fill put that bit 0.257 of a
             * frame early -- measured, console frame 24,433: its transfer-done rises
             * at phase 0.847 of the frame, ours at 0.590 (see XL_CD_DRAIN_MASTER).
             * The bytes themselves are what carry that delay now (cd_adpcm_fill takes
             * one per 45 master clocks); this test stays as the backstop for a card
             * that stops touching the unit mid-sector. */
            if (cd->drain_end && XL_CD_M3(c->cycles) < cd->drain_end)
                return;
            cd->drain_end = 0;
            cd_read_done(c);                 /* sets data_kind back to NONE */
            return;
        }
        if (XL_CD_M3(c->cycles) < cd->next_fill)
            return;                          /* the drive's countdown has not expired */
        if (cd->data_have) {
            /* $180B bits 0-1: the ADPCM is pulling this sector off the bus itself
             * (adpcm_inline.h:191-226), so the CPU not draining it is not a stall.
             * It arrives in the FIFO the same way and leaves byte by byte. */
            if (cd->status_b & 0x03)
                return;
            /* The FIFO still holds the sector the CPU has not drained.  The core's
             * answer is `CDReadTimer += period` -- the next attempt is a whole period
             * later, not the instant the CPU catches up (RunCDRead). */
            cd->next_fill += XL_CD_PERIOD_MASTER;
            cd->blocked_fills++;
            if (c->log)
                xl_logf(c, "cd: fill blocked at frame %u: sector %u of %u still on "
                           "the bus (pos %u of 2048); the drive retries one period "
                           "later (%u master clocks)",
                        (unsigned)c->vdc.frame, cd->read_served, cd->read_count,
                        cd->data_pos, XL_CD_PERIOD_MASTER);
            return;
        }
        if (!c->disc_attached)
            return;
        if (!cd_load_sector(c)) {
            /* The read ran off the end of the disc: that is a CHECK CONDITION, and
             * the console's own drive reports it rather than inventing data. */
            if (c->log)
                xl_logf(c, "cd: Read(6) lba %u ran off the disc (lba %u of %u)",
                        cd->read_lba, cd->read_lba + cd->read_served,
                        (unsigned)(c->disc_len / 2048u) + XL_DISC_LBA0 - 1u);
            cd_reject(cd);
            cd->sense_key = 0x05;
            cd->sense_asc = 0x21;
            cd_to_status(c, 0x02);                     /* CHECK CONDITION */
            return;
        }
        /* A whole sector lands in the FIFO at once, and the countdown for the next
         * one starts now (`CDReadTimer += period`), not when the CPU drains it. */
        cd->next_fill += XL_CD_PERIOD_MASTER;
        cd->data_have = 1;
        cd->req = 1;
        cd->phase = CD_PHASE_DATA_IN;
        /* $180B bits 0-1: this sector is the drive's own (the ADPCM pulls it off the
         * bus itself), so the countdown below is only a backstop for a card that
         * stops touching the unit mid-sector. It never fires while the card is
         * running the transfer. */
        if (cd->status_b & 0x03)
            cd->drain_end = XL_CD_M3(c->cycles) + XL_CD_DRAIN_MASTER;
        if (c->log && getenv("XLATE_CD_SECTORS"))
            xl_logf(c, "cd: sector %u of %u %s at frame %u, cycle %llu",
                    cd->read_served + 1, cd->read_count,
                    (cd->status_b & 0x03) ? "offered to the ADPCM's sample RAM" : "offered",
                    (unsigned)c->vdc.frame, (unsigned long long)c->cycles);
        /* $1803 bit 6 rises with the first byte of the sector (the reference
         * core's `CDIRQ(IRQ_DATA_TRANSFER_READY)` in RunCDRead). */
        cd->port3 |= 0x40;
        cd_irq_update(c);
        return;
    }
}

/* ------------------------------------------------------------------ ADPCM -- *
 * GEARGRAFX'S ADPCM ENGINE, AND NOTHING ELSE.
 *
 * The port reproduces Geargrafx (docs/REFERENCE.md), and where the two differ
 * Geargrafx is right by definition.  This section is written from the reference's
 * own `adpcm_inline.h` and `adpcm.cpp`, quoted below with line numbers; numbers
 * taken from other cores are gone even where they looked more sensible.
 *
 * WHAT $180A IS -- the chip's own 64 KB sample RAM port (`adpcm.h:95`):
 *
 *     u8 m_adpcm_ram[0x10000] = {};
 *
 * THE READ PORT (`adpcm_inline.h:64-66`) -- it hands back the byte the *previous*
 * access fetched, and schedules the next one:
 *
 *     case 0x0A:
 *         m_read_cycles = NextSlotCycles(true);
 *         return m_read_value;
 *
 * THE WRITE PORT (`adpcm_inline.h:101-104`):
 *
 *     case 0x0A:
 *         m_write_cycles = NextSlotCycles(false);
 *         m_write_value = value;
 *
 * THE FETCH (`adpcm_inline.h:143-161`):
 *
 *     if (m_read_cycles <= 0) { m_read_cycles = 0;
 *         m_read_value = m_adpcm_ram[m_read_address]; m_read_address++;
 *         if (!IS_SET_BIT(m_control, 4)) {
 *             if (m_length > 0) { m_length--; SetHalfIRQ(m_length < 0x8000); }
 *             else { SetHalfIRQ(false); SetEndIRQ(true); } } }
 *
 * THE STORE (`adpcm_inline.h:170-185`):
 *
 *     if (m_write_cycles <= 0) { m_write_cycles = 0;
 *         m_adpcm_ram[m_write_address] = m_write_value; m_write_address++;
 *         SetHalfIRQ(m_length < 0x8000);
 *         if (m_length == 0) SetEndIRQ(m_length == 0);
 *         if (!IS_SET_BIT(m_control, 4)) { m_length++; m_length &= 0x1FFFF; } }
 *
 * THE DRIVE'S OWN FILL (`adpcm_inline.h:191-226`) -- the ADPCM pulls the byte off
 * the SCSI bus itself and acknowledges it, and gives up only when the bus has nothing
 * left:
 *
 *     if ((m_dma & 0x03) == 0) return;
 *     ... if (m_write_cycles == 0) { m_write_cycles = NextSlotCycles(false);
 *             m_write_value = m_scsi_controller->ReadData();
 *             m_scsi_controller->AutoAck();
 *             if (!m_scsi_controller->IsDataReady()) m_dma &= ~0x01; }
 *     ... if (!ACK && !CD && IO && REQ) m_dma_cycles = 36;
 *
 * and `$180B` itself (`adpcm_inline.h:105-108`):
 *
 *     case 0x0B: m_dma = m_scsi_controller->IsDataReady() ? value : (value & ~0x01);
 *
 * THE CLOCK (`adpcm_inline.h:29-38`) -- the order every one of the above is stepped in:
 *
 *     CheckReset(); CheckLength(); RunAdpcm(cycles); UpdateReadWriteEvents(cycles);
 *     UpdateDMA(cycles); CheckLength(); CheckReset();
 *
 * THE LATENCY IS A SLOT TABLE, NOT A CONSTANT (`adpcm.cpp:215-234`):
 *
 *     int slot = ((offset + d) / 9) & 0x03;  // 0=refresh, 1=write, 2=write, 3=read
 *     ...
 *     if (read) { if (slot == 3) return d; } else { if (slot == 1 || slot == 2) return d; }
 *
 * indexed by `u8 offset = cycles % 36` (`adpcm_inline.h:130-136`).  A `$180A` access
 * therefore costs 1..36 master clocks depending on where in the 36-clock slot frame it
 * lands -- the port used Mednafen's fixed `19 * 3` (57) for a read and `3 * 11` (33)
 * for a write, and both of those numbers are now gone.
 *
 * WHAT THIS PORT DOES NOT MODEL: the audio.  Geargrafx decodes the nibble into a
 * sample and a buffer (`adpcm_inline.h:265-289`, `Sample()`); this port has no mixer,
 * so the state the machine can read back is kept and the waveform is not. */

/* One slot period, in master clocks (`adpcm.cpp:219`: nine clocks to a slot, four
 * slots to the frame).  Named so the catch-up window below cannot drift from it. */
#define XL_ADPCM_SLOT_CLOCKS 9u
#define XL_ADPCM_SLOTS       4u

/* `CalculateCyclesPerSample` (`adpcm_inline.h:124-128`) with the constants this
 * shipped build uses (`defines.h:42` GG_MASTER_CLOCK_RATE = 21,477,273 and `:49`
 * GG_ADPCM_DEFAULT_CLOCK_SPEED = 32,100.0f), kept in integer arithmetic:
 *
 *     frequency = clock_speed / (16 - rate)
 *     return GG_MASTER_CLOCK_RATE * 65536.0 / frequency + 0.5
 *
 * i.e. one sample every (16 - rate) master clocks, held as 16.16 fixed point. */
uint32_t xl_adpcm_cycles_per_sample(uint8_t rate)
{
    uint64_t num = (uint64_t)21477273u * 65536ull * (uint64_t)(16u - (rate & 0x0Fu));
    uint64_t den = 32100u;
    return (uint32_t)((num + den / 2u) / den);
}

/* `adpcm.cpp:215-234` ComputeLatency, plus `adpcm_inline.h:130-136` NextSlotCycles. */
static uint32_t xl_adpcm_slot_latency(Cpu *c, int read)
{
    unsigned offset = (unsigned)(XL_CD_M3(c->cycles)
                                 % (uint64_t)(XL_ADPCM_SLOT_CLOCKS * XL_ADPCM_SLOTS));
    unsigned d;
    for (d = 1; d <= XL_ADPCM_SLOT_CLOCKS * XL_ADPCM_SLOTS; d++) {
        unsigned slot = ((offset + d) / XL_ADPCM_SLOT_CLOCKS) & 0x03u;
        if (read) {
            if (slot == 3u)
                return d;
        } else if (slot == 1u || slot == 2u) {
            return d;
        }
    }
    return XL_ADPCM_SLOT_CLOCKS * XL_ADPCM_SLOTS;      /* ComputeLatency returns 36 */
}

/* Push bits 2/3 of $1803 from the ADPCM's flags (`GetStatusRegisterSnapshot`'s two
 * IRQ bits, `adpcm_inline.h:84-85`).  Bits 5/6 belong to the SCSI engine and are set
 * and cleared by the phase transitions, never here. */
static void cd_adpcm_status(Cpu *c)
{
    Cd *cd = &c->cd;
    cd->port3 = (uint8_t)((cd->port3 & (uint8_t)~0x0C)
                          | (cd->adpcm_half ? 0x04 : 0x00)
                          | (cd->adpcm_end ? 0x08 : 0x00));
    cd_irq_update(c);
}

/* SetHalfIRQ / SetEndIRQ (`adpcm_inline.h:309-327`): one flag each, exported to both
 * $1803 and $180C. */
static void cd_adpcm_set_half(Cpu *c, int asserted)
{
    if (c->cd.adpcm_half == (uint8_t)(asserted ? 1 : 0))
        return;
    c->cd.adpcm_half = (uint8_t)(asserted ? 1 : 0);
    if (c->bios_exec)
        cd_adpcm_status(c);
}

static void cd_adpcm_set_end(Cpu *c, int asserted)
{
    if (c->cd.adpcm_end == (uint8_t)(asserted ? 1 : 0))
        return;
    c->cd.adpcm_end = (uint8_t)(asserted ? 1 : 0);
    if (c->bios_exec)
        cd_adpcm_status(c);
}

/* CheckLength (`adpcm_inline.h:346-353`): while control bit 4 is set the length is
 * *continuously* reloaded from the address register, and the end flag is cleared.
 * Geargrafx does this on every clock, not on a $1808/$1809 write. */
static void cd_adpcm_check_length(Cpu *c)
{
    Cd *cd = &c->cd;
    if (cd->adpcm_lastcmd & 0x10) {
        cd->adpcm_len = cd->adpcm_addr;
        cd_adpcm_set_end(c, 0);
    }
}

/* CheckReset (`adpcm_inline.h:335-344` + `SoftReset` at `:40-58`): control bit 7 is a
 * held reset, applied on every clock while it is set.  It does NOT clear m_control. */
static int cd_adpcm_check_reset(Cpu *c)
{
    Cd *cd = &c->cd;
    if (!(cd->adpcm_lastcmd & 0x80))
        return 0;
    cd->adpcm_read_cycles = 0;
    cd->adpcm_write_cycles = 0;
    cd->adpcm_readaddr = 0;
    cd->adpcm_writeaddr = 0;
    cd->adpcm_addr = 0;
    cd->adpcm_len = 0;
    cd->adpcm_playnibble = 0;
    cd->adpcm_play_pending = 0;
    cd_adpcm_set_end(c, 0);
    cd_adpcm_set_half(c, 0);
    return 1;
}

/* UpdateReadWriteEvents (`adpcm_inline.h:138-189`), byte for byte. */
static void cd_adpcm_rw_events(Cpu *c, uint32_t cycles)
{
    Cd *cd = &c->cd;

    if (cd->adpcm_read_cycles > 0) {
        cd->adpcm_read_cycles -= (int32_t)cycles;
        if (cd->adpcm_read_cycles <= 0) {
            cd->adpcm_read_cycles = 0;
            cd->adpcm_read_value = cd->adpcm_ram[cd->adpcm_readaddr];
            cd->adpcm_readaddr = (uint16_t)(cd->adpcm_readaddr + 1);
            if (!(cd->adpcm_lastcmd & 0x10)) {
                if (cd->adpcm_len > 0) {
                    cd->adpcm_len--;
                    cd_adpcm_set_half(c, cd->adpcm_len < 0x8000u);
                } else {
                    cd_adpcm_set_half(c, 0);
                    cd_adpcm_set_end(c, 1);
                }
            }
        }
    }

    if (cd->adpcm_write_cycles > 0) {
        cd->adpcm_write_cycles -= (int32_t)cycles;
        if (cd->adpcm_write_cycles <= 0) {
            cd->adpcm_write_cycles = 0;
            cd->adpcm_ram[cd->adpcm_writeaddr] = cd->adpcm_write_value;
            cd->adpcm_writeaddr = (uint16_t)(cd->adpcm_writeaddr + 1);
            cd_adpcm_set_half(c, cd->adpcm_len < 0x8000u);
            if (cd->adpcm_len == 0)
                cd_adpcm_set_end(c, 1);
            if (!(cd->adpcm_lastcmd & 0x10)) {
                cd->adpcm_len = (cd->adpcm_len + 1u) & 0x1FFFFu;
            }
        }
    }
}

/* UpdateDMA (`adpcm_inline.h:191-226`): the drive fills the chip's own sample RAM.
 * The byte comes off the same FIFO `$1808` reads (`m_scsi_controller->ReadData()`),
 * is acknowledged by the ADPCM itself (`AutoAck()`), and lands in RAM through the
 * normal write cycle above (`m_write_cycles`), which is why `$180C` bit 2 is up while
 * it is in flight. The pull is armed only when the bus has a byte to give, but once
 * armed it fires on its countdown: Geargrafx does not re-check the bus there. */
static void cd_adpcm_dma(Cpu *c, uint32_t cycles)
{
    Cd *cd = &c->cd;

    if ((cd->status_b & 0x03) == 0)
        return;

    if (cd->adpcm_dma_cycles > 0) {
        cd->adpcm_dma_cycles -= (int32_t)cycles;
        if (cd->adpcm_dma_cycles <= 0) {
            cd->adpcm_dma_cycles = 0;
            if (cd->adpcm_write_cycles == 0) {
                cd->adpcm_write_cycles = (int32_t)xl_adpcm_slot_latency(c, 0);
                /* ReadData() + AutoAck(): the byte on the bus, and the ACK that makes
                 * the SCSI engine offer the next one. */
                cd->adpcm_write_value = cd->buf[cd->data_pos];
                cd->bytes_out++;                    /* the byte did leave the drive */
                cd_data_next(c);
                if (!(cd->data_kind == CD_DATA_SECTORS && cd->phase == CD_PHASE_DATA_IN &&
                      cd->req && cd->data_pos < 2048u))
                    cd->status_b &= (uint8_t)~0x01;  /* !IsDataReady(): m_dma &= ~1 */
            } else {
                cd->adpcm_dma_cycles = 1;
            }
        }
        return;
    }

    /* `!ACK && !CD && IO && REQ`: a data-in byte from a Read is on the bus and nothing
     * has taken it yet.  This port's FIFO says the same thing with a sector in flight
     * (`CD_DATA_SECTORS`, not a short status/sense response: those have CD set in
     * Geargrafx and would not arm the pull). */
    if (cd->data_kind == CD_DATA_SECTORS && cd->phase == CD_PHASE_DATA_IN &&
        cd->req && cd->data_pos < 2048u)
        cd->adpcm_dma_cycles = 36;
}

/* RunAdpcm (`adpcm_inline.h:228-290`), the control flow and the address/length half
 * of the sample step; the waveform is not decoded (nothing here mixes audio). */
static void cd_adpcm_run(Cpu *c, uint32_t cycles)
{
    Cd *cd = &c->cd;

    if (cd->adpcm_lastcmd & 0x80) {
        cd->adpcm_playing = (uint8_t)((cd->adpcm_lastcmd & 0x20) ? 1 : 0);
        cd->adpcm_play_pending = 0;
        return;
    }
    if (!cd->adpcm_playing && !cd->adpcm_play_pending)
        return;
    if (!(cd->adpcm_lastcmd & 0x20) || ((cd->adpcm_lastcmd & 0x40) && cd->adpcm_len == 0)) {
        cd->adpcm_play_pending = 0;
        cd->adpcm_playing = 0;
        return;
    }
    if (cd->adpcm_cycles_per_sample == 0)
        return;                    /* impossible after a reset; never spin on it */

    cd->adpcm_sample_acc += (int64_t)cycles << 16;
    while ((uint64_t)cd->adpcm_sample_acc >= (uint64_t)cd->adpcm_cycles_per_sample) {
        cd->adpcm_sample_acc -= (int64_t)cd->adpcm_cycles_per_sample;
        if (cd->adpcm_play_pending) {
            cd->adpcm_play_pending = 0;
            cd->adpcm_playing = 1;
        }
        cd->adpcm_playnibble ^= 1;
        if (!cd->adpcm_playnibble) {
            /* `adpcm_inline.h:271-279`: it is the LOW nibble that advances the read
             * address and counts the length down. */
            cd->adpcm_readaddr = (uint16_t)(cd->adpcm_readaddr + 1);
            cd->adpcm_len = (cd->adpcm_len - 1u) & 0x1FFFFu;
            cd_adpcm_set_half(c, cd->adpcm_len <= 0x8000u);
            if (cd->adpcm_len == 0)
                cd_adpcm_set_end(c, 1);
        }
    }
}

/* Adpcm::Clock(`cycles`) -- `adpcm_inline.h:29-38`, in its order.  The CheckReset
 * calls are NOT early exits: Geargrafx ignores their result and runs the rest of the
 * clock anyway (a held D7 reset does not stop the read/write countdowns from being
 * stepped). */
static void cd_adpcm_clock(Cpu *c, uint32_t cycles)
{
    (void)cd_adpcm_check_reset(c);
    cd_adpcm_check_length(c);
    cd_adpcm_run(c, cycles);
    cd_adpcm_rw_events(c, cycles);
    cd_adpcm_dma(c, cycles);
    cd_adpcm_check_length(c);
    (void)cd_adpcm_check_reset(c);
}

/* Bring the ADPCM engine and the drive's byte timers up to the current instant.
 * Every read and every write of $1800-$180F does this first.  The port only gets a
 * tick when the card touches the unit, so the window since the last access is
 * replayed in 36-clock steps -- one slot frame, the granularity Geargrafx's own
 * Adpcm::Clock sees the engine at. */
static void cd_advance(Cpu *c)
{
    Cd *cd = &c->cd;
    uint64_t now, elapsed;

    /* The audio head runs in both paths: it is the drive's own clock, and a state
     * can be injected with CD-DA already playing (the console's 24,400 state is). */
    cd_cdda_tick(c);
    cd_busy_poll(c);
    if (!c->bios_exec)
        return;                       /* the modelled path keeps its own timing */
    now = XL_CD_M3(c->cycles);
    if (now <= cd->adpcm_last) {
        cd_pump(c);
        return;                       /* nothing has elapsed: no clock to run */
    }
    elapsed = now - cd->adpcm_last;
    cd->adpcm_last = now;

    while (elapsed > 0) {
        uint64_t step = elapsed > (uint64_t)(XL_ADPCM_SLOT_CLOCKS * XL_ADPCM_SLOTS)
                        ? (uint64_t)(XL_ADPCM_SLOT_CLOCKS * XL_ADPCM_SLOTS) : elapsed;
        cd_pump(c);                   /* what makes a sector available to be pulled */
        cd_adpcm_clock(c, (uint32_t)step);
        elapsed -= step;
    }
    cd_pump(c);
}

/* ------------------------------------------------------------------ CD-DA -- *
 * THE DRIVE'S AUDIO HEAD IS A DEVICE, AND THE GAME READS IT.
 *
 * The port mixes no sound, and it does not need to: what the machine can observe is
 * a small, exact set of registers and events, and this section is that set, taken
 * from the reference core's drive (cdrom/scsicd.cpp: `DoNEC_PCE_SAPSP`,
 * `DoNEC_PCE_SAPEP`, `DoNEC_PCE_PAUSE`, `DoNEC_PCE_READSUBQ`, `RunCDDA`):
 *
 *   $D8 SAPSP  set the play start position.  cdb[9]&$C0 says how the address is
 *              written -- 00: LBA in cdb[3..5]; 40: BCD MSF in cdb[2..4]; 80: a
 *              track number in cdb[2].  cdb[1] = 0 leaves the drive PAUSED and
 *              silent; nonzero starts playback in PLAYMODE_NORMAL.  The end is
 *              reset to the lead-out.
 *   $D9 SAPEP  set the play end position (same three address forms) and the mode:
 *              cdb[1] = 03 normal, 02 interrupt (raise IRQ2 at the end), 01 loop,
 *              00 stop.
 *   $DA PAUSE  pause if anything is playing, else CHECK CONDITION (illegal
 *              request / audio not playing).
 *   $DD RDSBQ  ten bytes of subchannel Q: cdb[0] = 0 playing, 2 paused, 3 stopped.
 *
 * The head then walks one sector every 1/75 s of master clock (21,477,272 / 75 =
 * 286,363), and at `read_sec_end` it stops, loops, or stops and raises the
 * transfer-done event, per the mode.
 *
 * WHAT THE CARD AND THE GAME SEE WHILE IT RUNS:
 *   $1803 bit 4  a subchannel byte is available.  The drive stuffs one per sector
 *                while it holds a position (playing OR paused), which is why the
 *                console's $1803 keeps bit 4 from frame 822 of its 131,356-frame
 *                trace even though the card pauses playback at 7,590; only a read
 *                of $1807 with the FIFO empty clears it.
 *   $1807        the subchannel byte, and the read that clears bit 4.
 *   $1805/$1806  the raw PCM level of the last CD-DA sample, left or right as
 *                $1803 bit 1 selects (the reference caches it on any $1805/$1806
 *                access; with no mixer this port reports the cached value, which is
 *                what a program that reads the register back gets).
 *
 * Measured where it is used: the second cutscene starts it at console frame 24,438
 * with `$D8 00 04 57 48 00 00 00 00 40` (BCD MSF 04:57:48 = LBA 22,173 = track 4)
 * and `$D9 02 06 00 67 00 00 00 00 40` (BCD 06:00:67 = 26,917 = track 5, mode 02 =
 * interrupt), both in the same frame as this port's. */
#define XL_CDDA_LEADOUT   221262u
/* One sector of audio at 75 sectors per second, in master clocks.  Kept as an
 * integer quotient of the master clock so the head cannot drift from the chip. */
#define XL_CDDA_SECTOR_MASTER ((uint64_t)(21477272u / 75u))

static uint32_t cdda_bcd(uint8_t v)
{
    return (uint32_t)((v >> 4) * 10u + (v & 0x0Fu));
}

/* The three address forms a SAPSP/SAPEP carries.  See the contract above. */
static uint32_t cdda_addr(const uint8_t *cdb)
{
    switch (cdb[9] & 0xC0) {
    case 0x40: {
        uint32_t m = cdda_bcd(cdb[2]);
        uint32_t s = cdda_bcd(cdb[3]);
        uint32_t f = cdda_bcd(cdb[4]);
        uint32_t lba = (m * 60u + s) * 75u + f;
        return (lba >= 150u) ? (lba - 150u) : 0u;
    }
    case 0x80: {
        unsigned t = cdda_bcd(cdb[2]);
        unsigned i;
        if (t == 0)
            t = 1;
        for (i = 0; i < XL_TOC_COUNT; i++)
            if (xl_toc[i].track == t)
                return xl_toc[i].lba;
        return XL_CDDA_LEADOUT;
    }
    default:
        return ((uint32_t)cdb[3] << 16) | ((uint32_t)cdb[4] << 8) | (uint32_t)cdb[5];
    }
}

/* LBA -> the BCD MSF the Q channel reports (absolute time, +150). */
static void cdda_msf(uint32_t lba, uint8_t *out)
{
    uint32_t a = lba + 150u;
    uint32_t m = a / 4500u, s = (a / 75u) % 60u, f = a % 75u;
    out[0] = (uint8_t)(((m / 10u) << 4) | (m % 10u));
    out[1] = (uint8_t)(((s / 10u) << 4) | (s % 10u));
    out[2] = (uint8_t)(((f / 10u) << 4) | (f % 10u));
}

/* Build the ten bytes of Q a $DD hands over: control/adr, track, index, the
 * relative time, the absolute time.  The reference reads these from the disc's own
 * subchannel; this port has the TOC, which is what the game asks about. */
static void cdda_fill_subq(Cd *cd)
{
    unsigned i, track = 0;
    for (i = 0; i < XL_TOC_COUNT; i++)
        if (cd->cdda_pos >= xl_toc[i].lba)
            track = xl_toc[i].track;
    cd->subq[0] = (uint8_t)((cd->cdda_status == 1) ? 0x00
                            : (cd->cdda_status == 2) ? 0x02 : 0x03);
    cd->subq[1] = (uint8_t)(((track / 10u) << 4) | (track % 10u));
    cd->subq[2] = 0x01;                                  /* index 1 */
    {
        uint32_t base = 0;
        for (i = 0; i < XL_TOC_COUNT; i++)
            if (xl_toc[i].track == track)
                base = xl_toc[i].lba;
        cdda_msf(cd->cdda_pos - base, &cd->subq[3]);
    }
    cdda_msf(cd->cdda_pos, &cd->subq[6]);
    cd->subq[9] = 0x00;
}

/* Release a command that told the drive to take its time.
 *
 * Geargrafx leaves the bus in SCSI_PHASE_BUSY (BSY alone, `$1800` = $80) on `$D8`
 * while the audio seek runs and on `$D9` modes 1/2 until playback reaches the stop
 * position, and answers GOOD only when the reason is gone:
 *
 *   ScsiController::CommandAudioStartPosition()   `if ((mode == 0) && !IsSeeking())
 *                                                  StartStatus(GOOD); else
 *                                                  SetPhase(SCSI_PHASE_BUSY);`
 *   ScsiController::AudioSeekCompleted()          `if (m_phase == BUSY)
 *                                                  StartStatus(GOOD);`   called by
 *                                                  CdRomAudio::Clock when the seek
 *                                                  counter reaches 0
 *   CdRomAudio (stop event IRQ)                   `m_scsi_controller->StartStatus(GOOD)`
 *                                                  when the head passes m_stop_lba
 *
 * Asked only when something touches the unit, which is exactly what a card polling
 * $1800/$1802 does. */
static void cd_busy_poll(Cpu *c)
{
    Cd *cd = &c->cd;
    if (cd->phase != CD_PHASE_BUSY)
        return;
    if (cd->busy_release == CD_BUSY_ON_SEEK) {
        if (XL_CD_M3(c->cycles) < cd->busy_until)
            return;
    } else if (cd->cdda_status == 1) {
        return;                       /* still playing towards the stop position */
    }
    cd->busy_release = CD_BUSY_NONE;
    cd_to_status(c, 0x00);
}

/* Walk the head.  Called before anything that can observe it, exactly like
 * PCECD_Drive_Run's RunCDDA. */
static void cd_cdda_tick(Cpu *c)
{
    Cd *cd = &c->cd;
    uint64_t now = XL_CD_M3(c->cycles);

    if (cd->cdda_status != 1) {
        cd->cdda_next = 0;
        return;
    }
    if (cd->cdda_next == 0) {
        cd->cdda_next = now + XL_CDDA_SECTOR_MASTER;
        return;
    }
    while (now >= cd->cdda_next) {
        cd->cdda_next += XL_CDDA_SECTOR_MASTER;
        cd->cdda_pos++;
        /* Same head for audio and data: Geargrafx sets the media's current sector
         * from the audio position on every sector it walks
         * (`m_cdrom_media->SetCurrentSector(m_current_lba)`, cdrom_audio_inline.h:333),
         * so a Read(6) after a CD-DA passage is priced from where the head really
         * is, not from where the play started. */
        cd->head_lba = cd->cdda_pos;
        cd->subq_byte = 0x00;                 /* the drive stuffs the next byte */
        cd->port3 |= 0x10;
        if (cd->cdda_pos >= cd->cdda_end || cd->cdda_pos >= XL_CDDA_LEADOUT) {
            switch (cd->cdda_mode) {
            case 0x01:                        /* loop back to the start */
                cd->cdda_pos = cd->cdda_start;
                break;
            case 0x02:                        /* stop and raise the event */
                cd->cdda_status = 0;
                cd->cdda_next = 0;
                cd->port3 |= 0x20;            /* transfer done, as the core's
                                               * CDIRQCallback(DATA_TRANSFER_DONE) */
                cd_irq_update(c);
                return;
            default:                          /* 00 silent, 03 normal: stop */
                cd->cdda_status = 0;
                cd->cdda_next = 0;
                return;
            }
        }
    }
}

static void cd_begin_command(Cpu *c)
{
    Cd *cd = &c->cd;
    cd->phase = CD_PHASE_COMMAND;
    cd->cmd_len = 0;
    cd->cmd_need = 1;                 /* the opcode decides the rest */
    cd->req = 1;
    cd->consumed = 0;
    cd->db = 0x00;
    cd->data_kind = CD_DATA_NONE;
    cd->data_have = 0;
    cd->data_pos = 0;
    cd->resp_pos = cd->resp_len = 0;
    cd_irq_update(c);
}

/* The command is complete: decide what the drive does with it. */
static void cd_dispatch(Cpu *c)
{
    Cd *cd = &c->cd;
    uint8_t op = cd->cmd[0];

    cd->cmds++;
    cd->op_count[op]++;

    /* A DRIVE THAT HAS NOT REACHED SPEED IS NOT READY, AND SAYS SO.
     *
     * This is the machine's own behaviour, not a timing shim, and the System Card
     * is its witness twice over:
     *
     *  - its published error list has `$04 NOT READY -- CD-ROM drive not ready`
     *    (Hu7 CD System BIOS manual, SUB ERROR CODE LIST);
     *  - its boot path tests for exactly that code after Test Unit Ready and, if it
     *    sees it, waits SEVENTY vblanks before asking again:
     *
     *        C8E6  jsr $F091      ; Test Unit Ready
     *        C8E9  bcs  $C8ED     ; nonzero answer = not ready / error
     *        C8EC  rts            ; GOOD: boot continues
     *        C8ED  cmp  #$04      ; NOT READY
     *        C8EF  beq  $C916
     *        C916  ldx  #$46      ; 70 iterations ...
     *        C918  phx / jsr $E3B5 / plx / dex / bne $C918
     *                             ; $E3B5 waits for $2241 (the vblank counter) to
     *                             ; change: one frame each, 70 frames in all
     *
     *  The drive is a CD-ROM whose spindle motor has to come up after power-on; it
     *  answers every command except REQUEST SENSE with CHECK CONDITION and sense
     *  key 2 (NOT READY), ASC $04, until it is up.  Without this the card is told
     *  "ready" at power-on, never enters that retry, and the boot message leaves
     *  the screen one retry -- 70 frames plus the read that follows -- too early.
     *  Measured against the reference (BizHawk/TurboNyma, cold boot): the console's
     *  own Test Unit Ready fails and its retry starts the first Read(6) at frame
     *  76; the picture holds until frame 93.  Our drive said GOOD at frame 3, and
     *  our picture went at frame 17.
     *
     *  THE PERIOD IS NOT MEASURED, AND IS STATED AS SUCH.  All the reference fixes
     *  is a window: ready too late for the first question and ready by the retry
     *  70 frames later.  Every value in that window behaves identically here,
     *  because the card quantises readiness to its own 70-frame cadence.  One
     *  second is the order the drive's own spin-up takes (Charles MacDonald's
     *  port notes put the spindle's spin-down at "one or two seconds"). */
    if (XL_CD_M3(c->cycles) < cd->ready_at && op != 0x03) {
        cd->sense_key = 0x02;                   /* NOT READY */
        cd->sense_asc = 0x04;                   /* logical unit not ready */
        if (c->log)
            xl_logf(c, "cd: $%02X during spin-up (ready at master clock %llu) "
                       "-- CHECK CONDITION, sense $02/$%02X", op,
                    (unsigned long long)cd->ready_at, cd->sense_asc);
        cd_to_status(c, 0x02);
        return;
    }

    switch (op) {
    case 0x00:                                  /* Test Unit Ready */
        cd->cmds_ok++;
        if (c->log)
            xl_logf(c, "cd: $00 Test Unit Ready -- accepted (media present)");
        cd_to_status(c, 0x00);
        return;

    case 0x03: {                                /* Request Sense */
        unsigned alloc = cd->cmd[4] ? cd->cmd[4] : 18u;
        unsigned i;
        cd->cmds_ok++;
        for (i = 0; i < sizeof cd->resp; i++)
            cd->resp[i] = 0x00;
        cd->resp[0] = 0xF0;                     /* valid | current error */
        cd->resp[2] = cd->sense_key;
        cd->resp[7] = 0x0A;                     /* additional sense length */
        cd->resp[12] = cd->sense_asc;
        cd->resp[13] = 0x00;
        cd->resp_len = alloc < 18u ? alloc : 18u;
        cd->resp_pos = 0;
        cd->data_kind = CD_DATA_RESP;
        cd->phase = CD_PHASE_DATA_IN;
        cd->db = cd->resp[0];
        cd->req = 1;
        cd->data_have = 1;
        cd_irq_update(c);
        if (c->log)
            xl_logf(c, "cd: $03 Request Sense -- accepted, %u bytes, key $%02X",
                    cd->resp_len, cd->sense_key);
        return;
    }

    case 0x08: {                                /* Read(6) */
        unsigned lba = ((unsigned)(cd->cmd[1] & 0x1Fu) << 16)
                     | ((unsigned)cd->cmd[2] << 8) | (unsigned)cd->cmd[3];
        unsigned n = cd->cmd[4] ? cd->cmd[4] : 256u;
        size_t off = 0;
        int in_range = 0;
        if (c->disc_attached && lba >= XL_DISC_LBA0) {
            off = (size_t)(lba - XL_DISC_LBA0) * 2048u;
            in_range = (off + (size_t)n * 2048u <= c->disc_len);
        }
        if (!in_range) {
            cd_reject(cd);
            cd->sense_key = 0x05;
            cd->sense_asc = 0x21;
            if (c->log)
                xl_logf(c, "cd: $08 Read(6) lba %u count %u -- REJECTED: %s",
                        lba, n, c->disc_attached ? "past the end of the disc"
                                                 : "no disc attached");
            cd_to_status(c, 0x02);              /* CHECK CONDITION */
            return;
        }
        cd->cmds_ok++;
        cd->read_lba = lba;
        cd->read_count = n;
        cd->read_served = 0;
        /* THE FIRST SECTOR IS DUE WHEN THE HEAD GETS THERE, PLUS ONE SECTOR
         * PERIOD -- and the seek is priced by Geargrafx's own arithmetic.
         *
         *   src/scsi_controller.cpp:531-540   ScsiController::CommandRead()
         *     u32 seek_time = m_cdrom_media->SeekTime(current_lba, lba);
         *     u32 seek_cycles = ReadCycles(TimeToCycles(seek_time * 1000));
         *     u32 transfer_cycles = ReadCycles(m_cdrom_media->SectorTransferCycles());
         *     ...
         *     m_next_load_cycles = seek_cycles + transfer_cycles;
         *
         * `ReadCycles()` is `cycles / m_read_speed` (`:816`) and `m_read_speed` is
         * 1 unless the program has set a faster drive (`:57`, `:99`), so the seek is
         * milliseconds straight from `SeekTime()` and there is NO sector preload:
         * the first sector is one period after the head arrives (docs/REFERENCE.md).
         * This replaces an earlier transcription of Shadoff's table, which charged
         * the short seeks a third of what `SeekTime()` charges
         * (3 VSYNC where Geargrafx says 150 ms = 9, 16 where it says 300 ms = 18,
         * 36 where it says 800 ms = 48).
         * Over the intro's fourteen-read burst that is 88 frames of seek time the
         * port was not spending.  See docs/REFERENCE.md, "Disc drive". */
        cd->read_start = XL_CD_M3(c->cycles);
        cd->read_frame = (uint32_t)c->vdc.frame;
        {
            uint64_t seek = (uint64_t)xl_gg_time_to_cycles(
                                xl_gg_seek_ms(cd->head_lba, lba) * 1000u);
            cd->next_fill = cd->read_start + seek + XL_CD_PERIOD_MASTER;
            if (c->log)
                xl_logf(c, "cd: $08 Read(6) lba %u count %u -- accepted "
                           "(sectors %u-%u, head at %u, Geargrafx seek %u ms = "
                           "%llu master clocks = %.2f frames; first sector one "
                           "period later)",
                        lba, n, lba, lba + n - 1u, cd->head_lba,
                        xl_gg_seek_ms(cd->head_lba, lba),
                        (unsigned long long)seek,
                        (double)seek / (double)XL_CD_VSYNC_MASTER);
        }
        cd->drain_end = 0;               /* nothing of a previous transfer is in flight */
        cd->blocked_fills = 0;
        cd->data_kind = CD_DATA_SECTORS;
        cd->data_have = 0;
        cd->data_pos = 0;
        /* Still COMMAND with REQ clear: that is what the console answers for the
         * whole seek ($90, f=1748-f=1750 in cdcmd-9916-wall.log). */
        cd->phase = CD_PHASE_COMMAND;
        cd->req = 0;
        /* CD-DA STOPS HERE, AND THE HEAD FREEZES WHERE IT WAS.
         *
         *   src/scsi_controller.cpp:550   ScsiController::CommandRead()
         *     m_cdrom_audio->SetIdle();
         *
         * `SetIdle()` zeroes the seek and playback-delay counters and sets the state
         * to CD_AUDIO_STATE_IDLE (cdrom_audio_inline.h:210-216) -- and
         * `CdRomAudio::Sample()` is the only thing that walks `m_current_lba` and
         * pushes it at the media, `m_cdrom_media->SetCurrentSector(m_current_lba)`
         * (cdrom_audio_inline.h:334).  It generates nothing unless the state is
         * PLAYING, so after a Read(6) the audio head stops advancing and the media's
         * current sector stays where the playback left it.  The seek above was
         * priced from exactly that position, and the reads that follow move the head
         * themselves: `CdRomCueBinImage::ReadSector()` sets `m_current_sector =
         * lba + 1` (cdrom_cuebin_image.cpp:288).
         *
         * MEASURED on the console (drive sampled inside the RAM-streaming loop, so
         * trace and RAM are one machine): the intro's burst
         * starts at f8207 with the drive PLAYING and AUDIBLE at media sector 27559;
         * at f8209 the state reads IDLE / SILENT and the audio head sits at 27561 for
         * the rest of the trace, while the MEDIA head follows the reads -- 4171,
         * 4247, 4259, 4267, ... i.e. `lba + 1` after each transfer.  So read 1 pays
         * the audio-head seek and reads 2-14 do not: read 5 pays 21.9 frames from
         * 4267, not 55.9 from the audio head.
         *
         * Without this line the port's CD-DA kept walking the head underneath the
         * burst and re-took it from the read path, so reads 5 and 10 paid a second
         * and a third full-length seek back to the data track, a 78-frame
         * overshoot on the intro's burst alone. */
        cd->cdda_status = 0;             /* Geargrafx: CD_AUDIO_STATE_IDLE */
        cd->cdda_next = 0;
        cd_irq_update(c);
        if (c->log)
            xl_logf(c, "cd: $08 Read(6) lba %u count %u -- accepted "
                       "(sectors %u-%u, command phase %u/%u)",
                    lba, n, lba, lba + n - 1u,
                    (unsigned)(c->cycles % (uint64_t)(VD_LINES * VD_DOTS)),
                    (unsigned)(VD_LINES * VD_DOTS));
        return;
    }

    case 0xDE: {                                /* Get Dir Info */
        unsigned mode = cd->cmd[1];
        unsigned i;
        cd->resp_pos = 0;
        if (mode == 0x00) {
            cd->resp[0] = 0x01;                              /* first track */
            cd->resp[1] = xl_toc[XL_TOC_COUNT - 1].track;    /* last track  */
            cd->resp_len = 2;
        } else if (mode == 0x01) {
            unsigned m = XL_TOC_LEADOUT + 150u;
            cd->resp[0] = (uint8_t)(((m / 4500u) / 10u) << 4 | ((m / 4500u) % 10u));
            cd->resp[1] = (uint8_t)((((m / 75u) % 60u) / 10u) << 4 | (((m / 75u) % 60u) % 10u));
            cd->resp[2] = (uint8_t)(((m % 75u) / 10u) << 4 | ((m % 75u) % 10u));
            cd->resp_len = 3;
        } else if (mode == 0x02) {
            uint8_t want = cd->cmd[2];
            uint32_t lba = XL_TOC_LEADOUT;
            uint8_t type = 0x00;
            for (i = 0; i < XL_TOC_COUNT; i++) {
                if (xl_toc[i].track == want) {
                    lba = xl_toc[i].lba;
                    type = xl_toc[i].type;
                    break;
                }
            }
            {
                uint32_t a = lba + 150u;
                cd->resp[0] = (uint8_t)(((a / 4500u) / 10u) << 4 | ((a / 4500u) % 10u));
                cd->resp[1] = (uint8_t)((((a / 75u) % 60u) / 10u) << 4 | (((a / 75u) % 60u) % 10u));
                cd->resp[2] = (uint8_t)(((a % 75u) / 10u) << 4 | ((a % 75u) % 10u));
            }
            cd->resp[3] = type;
            cd->resp_len = 4;
        } else {
            cd_reject(cd);
            cd->sense_key = 0x05;
            cd->sense_asc = 0x24;
            if (c->log)
                xl_logf(c, "cd: $DE Get Dir Info mode $%02X -- REJECTED: "
                           "only modes 0/1/2 exist", mode);
            cd_to_status(c, 0x02);
            return;
        }
        cd->cmds_ok++;
        cd->data_kind = CD_DATA_RESP;
        cd->phase = CD_PHASE_DATA_IN;
        cd->db = cd->resp[0];
        cd->req = 1;
        cd->data_have = 1;
        cd_irq_update(c);
        if (c->log) {
            char hex[3 * sizeof cd->resp + 1];
            unsigned k, at = 0;
            for (k = 0; k < cd->resp_len && at + 3 < sizeof hex; k++)
                at += (unsigned)snprintf(hex + at, sizeof hex - at, "%02X ", cd->resp[k]);
            hex[at ? at - 1 : 0] = '\0';
            xl_logf(c, "cd: $DE Get Dir Info mode %u%s -- accepted, %u bytes [%s]",
                    mode, mode == 2 ? " (track)" : "", cd->resp_len, hex);
        }
        return;
    }

    case 0xD8:                                  /* Set audio play start position */
    case 0xD9: {                                /* Set audio play end position */
        /* CD-DA.  See the CD-DA contract above: the drive's PLAYBACK STATE IS
         * VISIBLE TO THE CARD -- while it holds a position it stuffs subchannel
         * bytes, so $1803 bit 4 reads set, and the game can read the position back
         * through $DD.  No audio is mixed; every register is the chip's. */
        uint64_t busy = 0;
        int busy_why = CD_BUSY_NONE;
        if (op == 0xD8) {
            uint64_t seek;
            uint32_t from = cd->head_lba;
            uint32_t start = cdda_addr(cd->cmd);
            /* THE SEEK IS PAID FOR, AND THE BUS IS HELD WHILE IT RUNS.
             *
             * `CommandAudioStartPosition()` calls `StartAudio(lba, mode == 0)`, and
             * StartAudio sets `m_seek_cycles = TimeToCycles(SeekTime(...) * 1000) -
             * TimeToCycles(k_playback_delay_us)`; the command then answers GOOD at
             * once only when `mode == 0 && !IsSeeking()`, and otherwise puts the
             * bus in SCSI_PHASE_BUSY until `AudioSeekCompleted()`.
             *
             * Measured on the console (Geargrafx, pinned seed, `get_cdrom_status`
             * frame by frame): this command lands at f1159 and the bus sits in
             * BUSY from f1160 to f1195 -- 36 frames -- with the game's own tick
             * `$2032`, which is `inc $32` in the vblank handler, frozen for exactly
             * those 36 frames while `$2047` keeps counting.  The port answered
             * immediately, so the same game ran 36 game ticks further on by age
             * 4,000, and the intro drew a different still. */
            seek = xl_gg_audio_seek_cycles(from, start);
            /* StartAudio also moves the media's current sector to the new position
             * (`m_cdrom_media->SetCurrentSector(m_current_lba)`), so the NEXT
             * command's seek is measured from here -- see xl_gg_seek_ms(). */
            cd->head_lba = start;
            if (!(cd->cdda_status == 1 && start == cd->cdda_start)) {
                cd->cdda_start = start;
                cd->cdda_end = XL_CDDA_LEADOUT;
                cd->cdda_pos = start;
                cd->cdda_next = 0;
                cd->cdda_status = 2;            /* PAUSED ... */
                cd->cdda_mode = 0x00;           /* ... PLAYMODE_SILENT */
                if (cd->cmd[1]) {
                    cd->cdda_status = 1;        /* ... unless cdb[1] says play */
                    cd->cdda_mode = 0x03;       /* PLAYMODE_NORMAL */
                    cd->cdda_next = XL_CD_M3(c->cycles) + XL_CDDA_SECTOR_MASTER;
                }
            }
            cd->cdda_playing = 1;               /* the drive holds a position */
            cd->subq_byte = 0x00;
            cd->port3 |= 0x10;                  /* a subchannel byte is available */
            cdda_fill_subq(cd);
            cd_irq_update(c);
            if (cd->cmd[1] != 0 || seek > 0) {
                busy = seek;
                busy_why = CD_BUSY_ON_SEEK;
            }
            if (c->log)
                xl_logf(c, "cd: $D8 audio seek from lba %u to %u: %u ms, %llu master "
                           "clocks = %.2f frames%s",
                        from, start, xl_gg_seek_ms(from, start),
                        (unsigned long long)seek,
                        (double)seek / (double)XL_CD_VSYNC_MASTER,
                        busy_why == CD_BUSY_NONE ? " (answered at once: no seek)"
                                                 : "; the drive is BUSY until it ends");
        } else {
            cd->cdda_end = cdda_addr(cd->cmd);
            switch (cd->cmd[1]) {
            default:
            case 0x03: cd->cdda_mode = 0x03; cd->cdda_status = 1; break;
            case 0x02: cd->cdda_mode = 0x02; cd->cdda_status = 1; break;
            case 0x01: cd->cdda_mode = 0x01; cd->cdda_status = 1; break;
            case 0x00: cd->cdda_mode = 0x00; cd->cdda_status = 0; break;
            }
            if (cd->cdda_status == 1 && cd->cdda_next == 0)
                cd->cdda_next = XL_CD_M3(c->cycles) + XL_CDDA_SECTOR_MASTER;
            cd->port3 |= 0x10;
            cdda_fill_subq(cd);
            cd_irq_update(c);
            /* `if ((mode == 1) || (mode == 2)) SetPhase(SCSI_PHASE_BUSY);` -- and
             * the release is the stop LBA being passed, which is cd_busy_poll()'s
             * second case.  The game does not wait on this one (the console runs
             * straight on after its $D9 and only the $D8 froze it), so it is
             * modelled because the bus really is held, not because a wait needs it. */
            if (cd->cmd[1] == 0x01 || cd->cmd[1] == 0x02)
                busy_why = CD_BUSY_ON_AUDIO_END;
        }
        cd->cmds_ok++;
        if (c->log)
            xl_logf(c, "cd: $%02X %s -- accepted; CD-DA %s from lba %u to %u "
                       "(mode %u; no audio is mixed, the registers are the chip's)",
                    op, cd_op_name(op),
                    cd->cdda_status == 1 ? "playing"
                                         : (cd->cdda_status ? "paused" : "stopped"),
                    cd->cdda_pos, cd->cdda_end, cd->cdda_mode);
        if (busy_why == CD_BUSY_NONE) {
            cd_to_status(c, 0x00);
        } else {
            cd->busy_release = (uint8_t)busy_why;
            cd->busy_until = XL_CD_M3(c->cycles) + busy;
            cd->phase = CD_PHASE_BUSY;
            cd->req = 0;
            cd->ack = 0;
            cd->consumed = 0;
            cd->db = 0x00;
            cd->data_kind = CD_DATA_NONE;
            cd->data_have = 0;
            cd_irq_update(c);
        }
        return;
    }

    case 0xDA: {                                /* Pause */
        if (cd->cdda_status != 0) {
            cd->cdda_status = 2;
            cd->cdda_next = 0;
            cd->cmds_ok++;
            if (c->log)
                xl_logf(c, "cd: $DA Pause -- accepted; CD-DA paused at lba %u",
                        cd->cdda_pos);
            cd_to_status(c, 0x00);
        } else {
            /* The core refuses a pause when nothing is playing: ILLEGAL REQUEST,
             * AUDIO NOT PLAYING.  It is a real answer, not a silent no-op. */
            cd_reject(cd);
            cd->sense_key = 0x05;
            cd->sense_asc = 0x2C;
            if (c->log)
                xl_logf(c, "cd: $DA Pause -- REJECTED: audio not playing");
            cd_to_status(c, 0x02);
        }
        return;
    }

    case 0xDD: {                                /* Read Subchannel Q */
        unsigned i;
        cdda_fill_subq(cd);
        for (i = 0; i < 10u; i++)
            cd->resp[i] = cd->subq[i];
        cd->resp_len = 10;
        cd->resp_pos = 0;
        cd->cmds_ok++;
        cd->data_kind = CD_DATA_RESP;
        cd->phase = CD_PHASE_DATA_IN;
        cd->db = cd->resp[0];
        cd->req = 1;
        cd->data_have = 1;
        cd_irq_update(c);
        if (c->log)
            xl_logf(c, "cd: $DD Read Subchannel Q -- accepted, status %u lba %u",
                    cd->subq[0], cd->cdda_pos);
        return;
    }

    default:
        /* AN UNKNOWN COMMAND IS NOT SILENTLY ACCEPTED.  The drive names it, counts
         * it and answers CHECK CONDITION, so the card's own retry/sense path is
         * visible in the log instead of the machine spinning in a poll loop. */
        cd_reject(cd);
        cd->sense_key = 0x05;                   /* ILLEGAL REQUEST */
        cd->sense_asc = 0x20;                   /* invalid command operation */
        if (c->log)
            xl_logf(c, "cd: opcode $%02X (%u bytes: %02X %02X %02X %02X %02X %02X "
                       "%02X %02X %02X %02X) -- REJECTED: not a command this drive "
                       "implements", op, cd->cmd_len,
                    cd->cmd[0], cd->cmd[1], cd->cmd[2], cd->cmd[3], cd->cmd[4],
                    cd->cmd[5], cd->cmd[6], cd->cmd[7], cd->cmd[8], cd->cmd[9]);
        cd_to_status(c, 0x02);                  /* CHECK CONDITION */
        return;
    }
}

/* ACK ($1802 bit 7) edges.  Rising: a wanted command/status/message byte is taken
 * from the host's latch.  Falling: the drive offers the next one, or moves on. */
static void cd_ack_edge(Cpu *c, int on)
{
    Cd *cd = &c->cd;
    if (on) {
        if (cd->ack)
            return;
        cd->ack = 1;
        if (!cd_req(cd))
            return;
        cd->consumed = 1;
        cd->req = 0;
        if (cd->phase == CD_PHASE_COMMAND) {
            if (cd->cmd_len < sizeof cd->cmd)
                cd->cmd[cd->cmd_len] = cd->host_db;
            cd->cmd_len++;
        }
        cd_irq_update(c);
        return;
    }
    if (!cd->ack)
        return;
    cd->ack = 0;
    if (!cd->consumed)
        return;
    cd->consumed = 0;
    switch (cd->phase) {
    case CD_PHASE_COMMAND:
        if (cd->cmd_len == 1)
            cd->cmd_need = cd_cmd_bytes(cd->cmd[0]);
        if (cd->cmd_len < cd->cmd_need) {
            cd->req = 1;
            cd_irq_update(c);
        } else {
            cd_dispatch(c);
        }
        break;
    case CD_PHASE_STATUS:
        cd->phase = CD_PHASE_MESSAGE;
        cd->db = 0x00;                       /* command complete */
        cd->req = 1;
        cd_irq_update(c);
        break;
    case CD_PHASE_MESSAGE:
        cd->phase = CD_PHASE_BUS_FREE;
        cd->req = 0;
        cd->db = 0x00;
        /* The bus is free: "transfer done" falls here (the reference's
         * `ChangePhase(PHASE_BUS_FREE)` issues `CDIRQ(0x8000|DONE)`).  Leaving it
         * set is exactly the defect this register used to have. */
        cd->port3 &= (uint8_t)~0x60;
        cd_irq_update(c);
        {
            static int pl = -1;
            if (pl < 0) {
                const char *e = getenv("XLATE_CD_PHASE_LOG");
                pl = (e && e[0] && e[0] != '0') ? 1 : 0;
            }
            if (pl && c->log)
                xl_logf(c, "cdphase f=%u pc=$%04X message -> bus free (port3 $%02X)",
                        (unsigned)c->vdc.frame, c->pc, cd->port3);
        }
        break;
    case CD_PHASE_DATA_IN:
        if (cd->data_kind == CD_DATA_RESP)
            cd_data_next(c);
        break;
    default:
        break;
    }
}

/* One byte leaves through $1808, which acknowledges as it is read. */
static uint8_t cd_data_port_read(Cpu *c)
{
    Cd *cd = &c->cd;
    uint8_t v;
    if (cd->phase != CD_PHASE_DATA_IN || !cd->req) {
        cd_pump(c);
        if (cd->phase != CD_PHASE_DATA_IN || !cd->req)
            return cd->db;
    }
    if (cd->data_kind == CD_DATA_SECTORS) {
        v = cd->buf[cd->data_pos];
        cd->bytes_out++;
        cd_data_next(c);
        return v;
    }
    /* A short response read through the data port: same byte, no ACK needed. */
    v = cd->db;
    cd_data_next(c);
    return v;
}

void xl_cd_summary(Cpu *c, FILE *out)
{
    unsigned op;
    if (!out)
        return;
    if (!c->cd.cmds)
        return;
    fprintf(out, "CDSUM cmds=%lu accepted=%lu rejected=%lu\n",
            c->cd.cmds, c->cd.cmds_ok, c->cd.cmds_bad);
    for (op = 0; op < 256u; op++) {
        if (!c->cd.op_count[op])
            continue;
        fprintf(out, "CDCMD op=%02X name=%s n=%lu accepted=%lu rejected=%lu\n",
                op, cd_op_name((uint8_t)op), c->cd.op_count[op],
                c->cd.op_count[op] - c->cd.op_bad[op], c->cd.op_bad[op]);
    }
}

/* Per-access trace of the hardware page, for an exhaustive diff against the console
 * trace over a short window.  XLATE_ACC_FROM/TO frame bounds. */
static int acc_from = -3, acc_to = -1;
static void xl_acc_note(Cpu *c, char dir, uint16_t off, uint8_t v)
{
    if (acc_from == -3) {
        const char *a = getenv("XLATE_ACC_FROM");
        const char *b = getenv("XLATE_ACC_TO");
        acc_from = a ? atoi(a) : -1;
        acc_to = b ? atoi(b) : -1;
    }
    if (acc_from < 0 || !c->log || (int)c->vdc.frame < acc_from ||
        (acc_to >= 0 && (int)c->vdc.frame > acc_to))
        return;
    xl_logf(c, "acc f=%u %c %04X = %02X pc=%04X b=%02X", c->vdc.frame, dir, off, v,
            c->pc, c->mpr[c->pc >> 13]);
}

uint8_t xl_rd_phys(Cpu *c, uint8_t bank, uint16_t off)
{
    off &= 0x1FFF;
    if (bank == XL_IO_BANK) {
        c->hw_reads[off]++;
        if (off <= 0x0003) {
            /* THE ACCESS SLOT IS CHARGED ON READS TOO.
             *
             * The reference core's IORead has the same `HuCPU.StealCycle()` on the
             * VDC window as IOWrite (`case 0x0000: HuCPU.StealCycle();
             * trace_ret = vce->ReadVDC(A);`, pce.cpp), and StealCycle is
             * `ADDCYC(1)`.  This model charged it on writes only, so every VDC
             * port read in the game was one cycle cheap -- the second half of the
             * 34-cycle-per-iteration gap in the $72:$8AA6 shift loop (16 `ror $00`
             * at +2 is 32; the two port reads are the other two).
             *
             * The steal comes first, as in the reference: the device is read at
             * the access slot the CPU has already paid for.
             *
             * The DATA PORT then waits as the reference does, and this is the other
             * half of the port model (video.c, vdc_vram_wait):
             * `case 0x2: case 0x3: if(!peek) DoWaitStates();` -- before the byte is
             * returned, not after (vdc.cpp:1108-1121), because the chip's BSY state
             * can be set by the previous access's queued word. */
            xl_tick(c, 1);                 /* StealCycle */
            if (off >= 0x0002) {           /* the data port waits (DoWaitStates) */
                unsigned w = vdc_vram_wait(&c->vdc);
                if (w)
                    xl_tick(c, w);
            }
            uint8_t st = vdc_read(&c->vdc, off);
            if (off == 0x0000 && !c->vdc.irq_line)
                c->irq_status &= (uint8_t)~XL_IQ_IRQ1; /* status read acknowledges */
            /* The access trace must carry the byte the chip returned, not a
             * placeholder: the card polls bit 5 (VD) here, and a wait can only be
             * compared to the console's once the flags it saw are in the log. */
            xl_acc_note(c, 'R', off, st);
            return st;
        }
        if (off < 0x2000 && (off < 0x1800 || off > 0x180F))
            xl_acc_note(c, 'R', off, 0);   /* CD reads log their real value below */
        if (off >= 0x0400 && off <= 0x0407)
            return vdc_read_vce(&c->vce, (uint16_t)(off - 0x0400));
        if (off >= 0x0C00 && off <= 0x0FFF) {
            /* Timer, mirrored over the whole 4 KB: the reload/counter is at even
             * addresses, the control at odd ones (MAME
             * `map(0x1fec00,0x1fec01).mirror(0x3fe)`; research §5.3).  Nothing
             * here is an interrupt mask. */
            if (off & 1)
                return c->timer.status;
            return c->timer.value;
        }
        if (off >= 0x1400 && off <= 0x17FF) {
            /* The CPU's interrupt controller (research §5.4).  The register is
             * selected by the low two bits and the block is mirrored, so $1402,
             * $1406 ... $17FE are all the mask. */
            switch (off & 3) {
            case 2:
                /* The mask as written, 1 = disabled.  Mednafen additionally clears
                 * the timer request on this read (`IRQStatusRead` case 0); that is
                 * recorded as an oddity with no manual behind it, so it is NOT
                 * copied here -- research §5.4 says to model it only if some code is
                 * seen to depend on it, and no such code has been seen. */
                return c->irq_mask;
            case 3:  return c->irq_status;    /* per-source pending flags */
            default: return 0;                /* $1400/$1401: open bus */
            }
        }
        if (off >= 0x1800 && off <= 0x180F) {
            /* CD drive registers: the drive's own answers (see "the drive" above).
             *
             * Every return from this block goes through CD_RET so that the access
             * trace (XLATE_ACC_FROM/TO) records the byte the model actually
             * returned, which is how a poll loop is compared against the console's. */
            Cd *cd = &c->cd;
#define CD_RET(c_, off_, v_) do { \
                uint8_t xl_rv_ = (uint8_t)(v_); \
                xl_acc_note((c_), 'R', (off_), xl_rv_); \
                return xl_rv_; \
            } while (0)
            /* The drive's own clock runs first: a sample may end, a byte may land,
             * a sector may become due.  This is PCECD_Run at the top of the card's
             * read, and it is what makes "did the end bits rise before this read"
             * a timing question instead of an accident. */
            cd_advance(c);
            switch (off) {
            case 0x1800: {
                /* BSY/REQ/MSG/CD/IO.  The one register the card's driver polls:
                 * `tst #$40,$1800` while it waits for a byte, and `and #$f8` /
                 * compare against $D0/$D8/$C8/$88 to follow the phases. */
                static int cd_on = -1;
                if (!c->bios_exec) {
                    /* The modelled path: the model IS the card here, and it reports
                     * the transfer it is performing itself.  Kept byte for byte so a
                     * modelled run cannot move because the drive grew a decoder. */
                    if (cd_on < 0) {
                        const char *e = getenv("XLATE_CD_STATUS");
                        cd_on = (e && e[0] && e[0] != '0') ? 1 : 0;
                    }
                    if (!cd_on)
                        CD_RET(c, off, 0x00);
                    if (cd->pos < cd->len && cd->len)
                        CD_RET(c, off, 0xF8);
                    if (cd->sectors_left)
                        CD_RET(c, off, 0xF8);
                    CD_RET(c, off, 0x88);
                }
                cd_pump(c);
                CD_RET(c, off, cd_status_byte(cd));
            }
            case 0x1801:
                CD_RET(c, off, c->bios_exec ? cd->db : cd->irq_status);
            case 0x1802: CD_RET(c, off, (uint8_t)(cd->irq_mask | (cd->ack ? 0x80 : 0x00)));
            case 0x1803:
                /* IRQ sources (see the contract in runtime.h).  This used to answer
                 * a derived "no command is open, so bit 5" -- permanently $20 --
                 * which is the save menu's first divergence.  Now it answers the
                 * register: bit 5 is an event set when the transfer finishes and
                 * cleared when the bus goes free or the host selects, and bit 1
                 * toggles on every read (the reference core's left/right select).
                 *
                 * READING THIS REGISTER LOCKS THE BACKUP RAM.  That is not an
                 * inference: the card's own close path `bm_close` ($FC7F) restores
                 * the MPRs and then does `lda bram_lock`, where the assembler's label
                 * `bram_lock` IS the address $1803 (syscard/bank0.asm line 27) -- a
                 * read whose only purpose there is to re-protect the card.  It is
                 * the counterpart of the `sta bram_unlock` ($1807 = $80) in
                 * `bm_open` ($FC49). */
                c->bram_wprot = 1;
                if (!c->bios_exec)
                    CD_RET(c, off, 0x20);              /* as the model always answered */
                {
                    uint8_t v = cd->port3;
                    cd->port3 ^= 0x02;                 /* the read itself toggles it */
                    CD_RET(c, off, v);
                }
            case 0x1808:
                if (!c->bios_exec)
                    CD_RET(c, off, cd->dest & 0xFF);   /* ADPCM address, as before */
                CD_RET(c, off, cd_data_port_read(c));
            case 0x1809: CD_RET(c, off, cd->dest >> 8);
            case 0x180A:                           /* ADPCM data port */
                if (!c->bios_exec) {
                    if (cd->pos >= cd->len)
                        cd_refill(c);
                    if (cd->pos < cd->len) {
                        uint8_t v = cd->buf[cd->pos++];
                        cd->bytes_out++;
                        CD_RET(c, off, v);
                    }
                    CD_RET(c, off, 0);
                }
                /* Geargrafx `adpcm_inline.h:64-66`:
                 *
                 *     case 0x0A:
                 *         m_read_cycles = NextSlotCycles(true);
                 *         return m_read_value;
                 *
                 * The byte handed back is the one the *previous* access fetched, and
                 * the access itself schedules the next fetch.  `NextSlotCycles` is the
                 * 36-clock slot table, not a constant (see xl_adpcm_slot_latency). */
                {
                    uint8_t v = cd->adpcm_read_value;
                    cd->adpcm_read_cycles = (int32_t)xl_adpcm_slot_latency(c, 1);
                    CD_RET(c, off, v);
                }
            /* `adpcm_inline.h:67-68`: nothing but the DMA latch. */
            case 0x180B: CD_RET(c, off, cd->status_b);
            case 0x1805:                           /* raw PCM level, left/right */
            case 0x1806: {
                /* The reference answers these from `RawPCMVolumeCache[]`, the level
                 * of the last CD-DA sample it mixed, and refreshes the cache on any
                 * $1805/$1806 access; $1803 bit 1 picks the channel (the same bit the
                 * $1803 read toggles).  This port has no mixer, so the cache holds
                 * the last value it was given -- which is exactly what a program that
                 * reads the register back observes.  Answering 0 for a register the
                 * chip drives would be a different machine. */
                uint16_t v = (cd->port3 & 0x02) ? cd->cdda_vol_r : cd->cdda_vol_l;
                CD_RET(c, off, (uint8_t)(off == 0x1805 ? (v & 0xFF) : (v >> 8)));
            }
            case 0x180C:
                /* Geargrafx `GetStatusRegisterSnapshot` (`adpcm_inline.h:81-89`):
                 *
                 *     status |= (m_playing ? 0x08 : 0x00);
                 *     status |= (m_end_irq ? 0x01 : 0x00);
                 *     status |= (m_read_cycles > 0 ? 0x80 : 0x00);
                 *     status |= (m_write_cycles > 0 ? 0x04 : 0x00);
                 *
                 * Four bits and no others -- bit 2 is the *write* pending and bit 7
                 * the *read* pending, which is the pair the card's `ad_stat` ($F6DB)
                 * and its `ad_read` wait ($F3FE) both poll. */
                if (!c->bios_exec)
                    CD_RET(c, off, cd->pos < cd->len ? 0x00 : 0x80);
                CD_RET(c, off, (uint8_t)((cd->adpcm_playing ? 0x08 : 0x00)
                                         | (cd->adpcm_end ? 0x01 : 0x00)
                                         | (cd->adpcm_read_cycles > 0 ? 0x80 : 0x00)
                                         | (cd->adpcm_write_cycles > 0 ? 0x04 : 0x00)));
            case 0x1804:
                /* Only the executed card drives this register (the model path never
                 * reads it), so the gate keeps the default path byte for byte. */
                if (!c->bios_exec)
                    CD_RET(c, off, 0x00);
                CD_RET(c, off, cd->bus_reset);   /* see the write side */
            case 0x1807:
                /* The subchannel FIFO.  A read takes the byte the drive stuffed and,
                 * when it has no more, clears $1803 bit 4 -- that is the only way
                 * the bit ever goes down (the reference core's `PCECD_Read` case
                 * 0x7).  While CD-DA plays the drive keeps the FIFO fed, so the bit
                 * stays up: measured, the console's $1803 keeps bit 4 from frame 822
                 * to the end of its 131,356-frame trace although the card pauses
                 * playback at 7,590, and nothing in the corpus reads $1807. */
                if (!c->bios_exec)
                    CD_RET(c, off, 0x00);
                {
                    uint8_t v = cd->subq_byte;
                    if (!cd->cdda_playing)
                        cd->port3 &= (uint8_t)~0x10;
                    cd_irq_update(c);
                    CD_RET(c, off, v);
                }
            case 0x180D:
                /* NOT a write-through latch: a read returns the last control byte
                 * written (`ADPCM.LastCmd`), which a D7 reset clears to 0.  The card
                 * reads it at $F6EB in every `ad_stat` and branches on bit 5. */
                CD_RET(c, off, c->bios_exec ? cd->adpcm_lastcmd : cd->control);
            case 0x180E: CD_RET(c, off, cd->mode);
            default:     CD_RET(c, off, 0);
            }
#undef CD_RET
        }
        if (off >= 0x18C0 && off <= 0x18C7) {
            /* The drive's identity pins, which are how the card knows it is talking
             * to a Super System Card.  ROM $FE92 (`ex_memopen`) reads $18C5 first and
             * then $18C1-$18C3; the console answers $FF then $AA, $55, $03
             * (cdcmd-9916-wall.log, f=3), i.e. Super CD-ROM2, BIOS version 3.  Without
             * them the card takes this machine for a plain CD-ROM² and skips the
             * setup the disc is built for. */
            switch (off) {
            case 0x18C1: return 0xAA;
            case 0x18C2: return 0x55;
            case 0x18C3: return 0x03;
            case 0x18C0: return c->hw_regs[off];       /* written, not driven */
            default:     return 0xFF;                  /* no second identification */
            }
        }
        if (off >= 0x1000 && off <= 0x13FF) {
            /* The joypad port is $1000-$13FF, picked by `A & 0x1c00`; $1400-$17FF
             * is the interrupt controller above (research §5.4, Mednafen pce.cpp's
             * `A & 0x1c00` switch).  Treating all of $1000-$17FF as one port is
             * what swallowed the game's $1402 mask writes for so long. */
            /* Select bit 0 chooses the nibble: set = directions, clear = buttons
             * (that is the order the BIOS pad routine reads them in, and the order
             * its 6-button test `cmp #$0C` = SELECT|RUN implies). */
            /* THE READ IS THE REFERENCE CORE'S, LINE FOR LINE (input.cpp,
             * INPUT_Read -> MultiTapRead -> RealPortRead).
             *
             *   read_index > 4  ->  $00 (no controller in that slot)
             *   otherwise       ->  the controller's Read(), which for a gamepad
             *                       is $0F ^ the selected nibble
             * then $30 always, and $80 when no CD unit is attached.
             *
             * The inversion belongs to the *controller*, not to the port: an
             * empty slot answers a plain $0F, which is why the console's trace
             * shows $3F for pads 2-5 of the BIOS routine's five iterations and
             * this model, inverting a fabricated $0F, answered $30 instead. */
            unsigned idx = (unsigned)c->joypad_index;
            unsigned raw;
            if (idx > 4u)
                raw = 0x00;
            else
                raw = (~(unsigned)(((c->joypad_sel & 1) ? (c->pad[idx] >> 4)
                                                       : c->pad[idx]) & 0x0F)) & 0x0F;
            return (uint8_t)(raw | 0x30u | (c->disc_attached ? 0x00u : 0x80u));
            /* The upper four bits are NOT open bus.  They are the machine's identity
             * bits, and the console's own trace says so: the BIOS pad routine reads
             * $1000 at $E4B7/$E4C8 and is answered $3F with nothing pressed, where
             * this model answered $FF (frames 2500 and 2800, pc=$E4B7/$E4C8 in the
             * console's access trace).  So:
             *   bit 4, bit 5  always 1                          ($30)
             *   bit 6         0 = PC Engine, 1 = TurboGrafx      (left clear)
             *   bit 7         1 = no CD-ROM unit attached
             * The reference core composes exactly this (`INPUT_Read`:
             * `ret |= 0x80` when !PCE_IsCD, `ret |= 0x30`), and it is what the
             * hardware manual's port description says the four bits mean.  A game is
             * entitled to ask; answering $F0 tells it "this is a TurboGrafx with no
             * CD drive", which is a different machine. */
        }
        return c->hw_regs[off];
    }
    if (bank == XL_BRAM_BANK) {
        /* XLATE_BRAM_LOG: every access to the save device, with the pc that made
         * it.  The card's file manager is a code path, not a data structure: which
         * byte it reads next says which branch it took (the "HUBM" signature at
         * $0000, the directory entries at $0010, the 12-byte name at +4). */
        static int bram_log = -1;
        if (bram_log < 0) {
            const char *e = getenv("XLATE_BRAM_LOG");
            bram_log = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (bram_log && c->log)
            xl_logf(c, "bram: rd $F7:%04X = $%02X (frame %u, pc $%04X, bank $%02X)",
                    off, c->bram[off & (XL_BRAM_SIZE - 1)], (unsigned)c->vdc.frame,
                    c->pc, c->mpr[c->pc >> 13]);
        return c->bram[off & (XL_BRAM_SIZE - 1)];
    }
    return c->mem[(size_t)bank * XL_BANK_SIZE + off];
}

/* Task creation: the queue lives at $3C80 (type) / $3C88 (parameter), 8 slots.
 * Logging who sets a type byte answers "which code schedules this job". */
static void xl_task_note(Cpu *c, uint16_t a, uint8_t v)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XLATE_TASK_NEW");
        on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (!on || v == 0 || a < 0x3C80 || a > 0x3C87 || !c->log)
        return;
    xl_logf(c, "task: slot %u type $%02X created at frame %u from pc $%04X "
               "(bank $%02X) param $%02X", (unsigned)(a - 0x3C80), v,
            c->vdc.frame, c->pc, c->mpr[c->pc >> 13],
            xl_rd(c, (uint16_t)(a + 8)));
}

void xl_wr_phys(Cpu *c, uint8_t bank, uint16_t off, uint8_t v)
{
    off &= 0x1FFF;
    if (bank == XL_IO_BANK) {
        if (off < 0x2000)
            xl_acc_note(c, 'W', off, v);
        if (off == 0x0017) {
            static int on17 = -1;
            if (on17 < 0) {
                const char *e = getenv("XLATE_IO17");
                on17 = (e && e[0] && e[0] != '0') ? 1 : 0;
            }
            if (on17 && c->log)
                xl_logf(c, "io17 f=%u = $%02X pc=$%04X (bank $%02X)",
                        c->vdc.frame, v, c->pc, c->mpr[c->pc >> 13]);
        }
        c->hw_regs[off] = v;
        c->hw_writes[off]++;
        if (off <= 0x0003) {
            /* Debug aid: XLATE_WATCH_VRAM=1 logs who writes the background map,
             * which is the fastest way to find "why is the picture a fill". */
            static int watch = -1;
            if (watch < 0) {
                const char *e = getenv("XLATE_WATCH_VRAM");
                watch = (e && e[0] && e[0] != '0') ? 1 : 0;
            }
            if (watch && off >= 2 && (c->vdc.select & 0x1F) == 0x02 &&
                (c->vdc.mawr & 0x7FFF) < 2048 && c->vdc.writes < 4000000) {
                xl_logf(c, "VRAM write word $%04X = $%02X from pc $%04X (bank $%02X)",
                        c->vdc.mawr & 0x7FFF, v, c->pc, c->mpr[c->pc >> 13]);
            }
            {
                static int vmin = -1;
                if (vmin < 0) {
                    const char *e = getenv("XLATE_VRAMHI_MIN");
                    vmin = e ? (int)strtol(e, NULL, 0) : 0x1000;
                }
            }
            /* Debug aid: XLATE_VRAM_WATCH=LO:HI names the writer of one VRAM word
             * range, with frame, pc and bank.  It answers "who animates this
             * tile?" from the trace instead of from a guess. */
            {
                static long wlo = -2, whi = 0;
                if (wlo == -2) {
                    const char *e = getenv("XLATE_VRAM_WATCH");
                    wlo = -1;
                    if (e) {
                        char *end;
                        long a = strtol(e, &end, 0);
                        if (*end == ':') {
                            wlo = a;
                            whi = strtol(end + 1, NULL, 0);
                        }
                    }
                }
                if (wlo >= 0 && off == 0x0003 && (c->vdc.select & 0x1F) == 0x02) {
                    long w = (long)(c->vdc.mawr & 0x7FFF);
                    if (w >= wlo && w < whi)
                        xl_logf(c, "vramw f=%u word=$%04X = $%04X pc=$%04X bank=$%02X",
                                c->vdc.frame, (unsigned)w, c->vdc.vwr,
                                c->pc, c->mpr[c->pc >> 13]);
                }
            }
            {
                static int vmin = -1;
                if (vmin < 0) {
                    const char *e = getenv("XLATE_VRAMHI_MIN");
                    vmin = e ? (int)strtol(e, NULL, 0) : 0x1000;
                }
                if (off == 0x0003 && (c->vdc.select & 0x1F) == 0x02 &&
                    (int)(c->vdc.mawr & 0x7FFF) >= vmin)
                    xl_vramhi_note(c, c->pc, c->vdc.mawr & 0x7FFF);
                if (off == 0x0003 && c->log) {
                    static int rf = -2, rt = -1;
                    if (rf == -2) {
                        const char *a = getenv("XLATE_REG_FROM");
                        const char *b = getenv("XLATE_REG_TO");
                        rf = a ? atoi(a) : -1;
                        rt = b ? atoi(b) : -1;
                    }
                    if (rf >= 0 && (int)c->vdc.frame >= rf &&
                        (rt < 0 || (int)c->vdc.frame <= rt))
                        xl_logf(c, "regw f=%u pc=%04X sel=%02X v=%02X",
                                c->vdc.frame, c->pc, c->vdc.select & 0x1F, v);
                }
                if (off == 0x0003 && (c->vdc.select & 0x1F) == 0x00) {
                    static int mf = -2, mt = -1;
                    if (mf == -2) {
                        const char *a = getenv("XLATE_MAWR_FROM");
                        const char *b = getenv("XLATE_MAWR_TO");
                        mf = a ? atoi(a) : -1;
                        mt = b ? atoi(b) : -1;
                    }
                    if (mf >= 0 && c->log && (int)c->vdc.frame >= mf &&
                        (mt < 0 || (int)c->vdc.frame <= mt))
                        xl_logf(c, "mawr f=%u pc=$%04X (bank $%02X) -> $%04X",
                                c->vdc.frame, c->pc, c->mpr[c->pc >> 13],
                                ((unsigned)v << 8 | (c->vdc.vwr & 0xFF)) & 0x7FFF);
                }
            }
            /* THE VDC STALLS THE CPU ONE CYCLE PER ACCESS.
             *
             * `HuCPU.StealCycle()` in the reference core's IOWrite, on the VDC
             * window (`case 0x0000: HuCPU.StealCycle(); vce->WriteVDC(...)`), and
             * StealCycle is `ADDCYC(1)` -- one extra CPU cycle of the current
             * instruction, with every device advancing over it.  This model had no
             * wait states at all (video.h: "VDC access wait states ... are not
             * modelled: the CPU never waits"), so every VDC access in the game was
             * one cycle too cheap.  dshadoff's hierarchy for exactly this is "real
             * hardware wait states > Mednafen > MiSTer > zero"; zero is the wrong
             * end of it.
             *
             * Measured from both traces, the $8280 VRAM upload loop (two VDC
             * writes per iteration, `sta $0002` / `sta $0003`): 15,851 iterations
             * cost 3,236,121 master clocks on the console and 3,065,910 here --
             * 10.7 master clocks (3.57 CPU cycles) more per iteration.  Charging
             * one cycle per access is what puts the cold boot back on its old
             * frame alignment and removes the prologue's late one-frame gain.  The
             * remainder of that 3.57 was not a
             * per-write constant and is not closed by one: part of it is the read
             * steal above, and the rest is the VDC's stall ratio (video.c,
             * vdc_vram_wait: a stall costs d CPU CYCLES for an event d master
             * clocks away, not d/3).
             *
             * Charged here, in the one place both engines write the port, so
             * translated code and the interpreter pay the same.
             *
             * The VCE window ($0400-$0407) steals in the reference core too and is
             * deliberately NOT charged: measured, it moves no frame but puts 4
             * pixels (two 2-px runs, rows 73 and 121) on the booted logo that the
             * console does not have, and this model's line-at-a-time renderer
             * cannot tell a palette write that lands one dot late from one that
             * lands on the wrong side of a line.  Left as a measured open question,
             * not as a silent omission. */
            xl_tick(c, 1);                 /* StealCycle */
            if (off == 0x0003) {
                /* MARR and the VRAM data port are the two registers whose second
                 * byte touches the port (huc6270/vdc.cpp, cases 0x01 and 0x02). */
                unsigned sel = c->vdc.select & 0x1F;
                if (sel == 0x01 || sel == 0x02) {
                    unsigned w = vdc_vram_wait(&c->vdc);
                    if (w)
                        xl_tick(c, w);
                }
            }
            vdc_write(&c->vdc, off, v);
            return;
        }
        if (off >= 0x1800 && off <= 0x180F) {
            Cd *cd = &c->cd;
            /* The drive's clock first (see cd_advance): the ADPCM byte timers and
             * the sample clock advance to this instant before the write lands. */
            cd_advance(c);
            switch (off) {
            case 0x1800:
                /* SELECT: the host takes the bus.  The drive drops the two SCSI
                 * transfer bits here (`_Port[0x3] &= ~(0x20|0x40)` at the top of
                 * the reference's write case 0x0) -- this is one of the two things
                 * that clears the "transfer done" event. */
                cd->port3 &= (uint8_t)~0x60;
                cd_irq_update(c);
                if (c->log && getenv("XLATE_CD_SEL_LOG") && getenv("XLATE_CD_SEL_LOG")[0] != '0')
                    xl_logf(c, "cd: select $%02X at frame %u phase %u/%u pc=$%04X",
                            v, (unsigned)c->vdc.frame,
                            (unsigned)(c->cycles % (uint64_t)(VD_LINES * VD_DOTS)),
                            (unsigned)(VD_LINES * VD_DOTS), c->pc);
                /* Select.  The card writes the start pulse here once the drive has
                 * told it the bus is free (`sta cd_port` at ROM $E947/$F7B5); a
                 * write while a command is already open re-selects, which aborts
                 * what was open rather than leaving the drive stuck mid-command.
                 * Only the executed card drives this register; the modelled path
                 * writes it as part of its own shortcut and must not enqueue a
                 * command for the decoder. */
                cd->last_cmd = v;
                if (c->bios_exec) {
                    if (cd->phase != CD_PHASE_BUS_FREE && c->log)
                        xl_logf(c, "cd: re-select $%02X at phase %u -- aborting the "
                                   "open command", v, cd->phase);
                    cd_begin_command(c);
                }
                break;
            case 0x1801:
                cd->host_db = v;                        /* DB latch, no side effect */
                if (!c->bios_exec)
                    cd->irq_status = 0;                 /* as the model always did */
                break;
            case 0x1802:
                cd->irq_mask = (uint8_t)(v & 0x7Fu);
                if (c->bios_exec)
                    cd_ack_edge(c, (v & 0x80) ? 1 : 0);
                break;
            case 0x1804:
                /* $1804 READS BACK THE BYTE WRITTEN TO IT.  Evidence: the card's own
                 * drive-reset routine at ROM $E8E3 is `lda #$02 / tsb $1804` -- a
                 * read-modify-write -- and then immediately `lda $1804` at $E8F2.
                 * The console answers $02 (f=0, pc=$E8F5), i.e. the register holds
                 * what was last stored, and
                 * this model answered $00 until now.  The card does not branch on it
                 * (it just clears bit 1), so no boot step moved -- but a register
                 * that answers a value it was never given is a model defect either
                 * way, and it is reported here instead of being papered over. */
                cd->bus_reset = v;
                /* Reset (bit 1): back to a free bus with no command, no ACK and no
                 * pending source; the IRQ mask and the disc stay as they were.  The
                 * drive also drops the subchannel/transfer bits ($1803 bits 4-6),
                 * as the reference's `case 0x4` does (`_Port[0x3] &= ~0x70`). */
                if (v & 0x02) {
                    cd->phase = CD_PHASE_BUS_FREE;
                    cd->req = cd->ack = cd->consumed = 0;
                    cd->cmd_len = 0;
                    cd->data_kind = CD_DATA_NONE;
                    cd->data_have = 0;
                    cd->port3 &= (uint8_t)~0x70;
                    cd_irq_update(c);
                }
                break;
            case 0x1807:
                /* The backup-RAM write protect.  $80 is the unlock value the card
                 * stores in `bm_open` (`lda #$80 / sta bram_unlock`, ROM $FC6E);
                 * any other value protects the card again.  It is not stored in
                 * hw_regs: $1807 is a control latch with one meaning. */
                c->bram_wprot = (uint8_t)((v & XL_BRAM_LOCK) ? 0 : 1);
                if (c->log && c->hw_writes[off] < 4)
                    xl_logf(c, "bram: $1807 = $%02X -> %s at frame %u (pc $%04X)",
                            v, c->bram_wprot ? "write-protected" : "unlocked",
                            (unsigned)c->vdc.frame, c->pc);
                break;
            case 0x1808:
                /* Geargrafx `adpcm_inline.h:95-100` -- the address register, with no
                 * gate on the control byte and no length latch here at all (the length
                 * is latched by `CheckLength` on every clock while D4 is set): */
                cd->dest = (uint16_t)((cd->dest & 0xFF00) | v);
                if (c->bios_exec)
                    cd->adpcm_addr = (uint16_t)((cd->adpcm_addr & 0xFF00) | v);
                break;
            case 0x1809:
                cd->dest = (uint16_t)((cd->dest & 0x00FF) | (v << 8));
                if (c->bios_exec)
                    cd->adpcm_addr = (uint16_t)((cd->adpcm_addr & 0x00FF) | (uint16_t)(v << 8));
                break;
            case 0x180A:
                /* `adpcm_inline.h:101-104`:
                 *
                 *     case 0x0A:
                 *         m_write_cycles = NextSlotCycles(false);
                 *         m_write_value = value;
                 *
                 * The byte lands in sample RAM when that slot countdown expires, and
                 * $180C bit 2 is set while it is in flight. */
                if (c->bios_exec) {
                    cd->adpcm_write_cycles = (int32_t)xl_adpcm_slot_latency(c, 0);
                    cd->adpcm_write_value = v;
                }
                break;
            case 0x180B:
                /* `adpcm_inline.h:105-108`:
                 *
                 *     case 0x0B: m_dma = m_scsi_controller->IsDataReady() ? value
                 *                                : (value & ~0x01);
                 *
                 * `IsDataReady()` there is "the SCSI data buffer still has bytes"
                 * (`scsi_controller_inline.h:204-207`); this port's FIFO says the same
                 * thing with a data-in phase and a byte on it. */
                if (c->bios_exec) {
                    int ready = (cd->phase == CD_PHASE_DATA_IN && cd->req &&
                                 cd->data_pos < 2048u);
                    cd->status_b = (uint8_t)(ready ? v : (v & (uint8_t)~0x01));
                } else {
                    cd->status_b = v;
                }
                break;
            case 0x180D:
                /* Geargrafx `WriteControl` (`adpcm_inline.h:292-307`), which is the
                 * whole of what a write to this register does:
                 *
                 *     if (IS_SET_BIT(value, 1) && !IS_SET_BIT(m_control, 1))
                 *         m_write_address = m_address - (IS_SET_BIT(value, 0) ? 0 : 1);
                 *     if (IS_SET_BIT(value, 3) && !IS_SET_BIT(m_control, 3))
                 *         m_read_address = m_address - (IS_SET_BIT(value, 2) ? 0 : 1);
                 *     if (IS_SET_BIT(value, 5) && !m_playing) m_play_pending = true;
                 *     m_control = value;
                 *
                 * Note what is NOT here: D7 is not a one-shot reset (it is a held
                 * reset applied by CheckReset on every clock), and D4 does not latch
                 * anything here either (CheckLength does, also on every clock). */
                cd->control = v;                 /* the modelled path's view, unchanged */
                if (c->bios_exec) {
                    if ((v & 0x02) && !(cd->adpcm_lastcmd & 0x02))
                        cd->adpcm_writeaddr = (uint16_t)((v & 0x01)
                            ? cd->adpcm_addr : (uint16_t)(cd->adpcm_addr - 1u));
                    if ((v & 0x08) && !(cd->adpcm_lastcmd & 0x08))
                        cd->adpcm_readaddr = (uint16_t)((v & 0x04)
                            ? cd->adpcm_addr : (uint16_t)(cd->adpcm_addr - 1u));
                    if ((v & 0x20) && !cd->adpcm_playing)
                        cd->adpcm_play_pending = 1;
                    cd->adpcm_lastcmd = v;
                }
                break;
            case 0x180E:
                /* `adpcm_inline.h:112-115`: the raw byte is kept and the sample period
                 * is recomputed from its low nibble. */
                cd->mode = v;
                if (c->bios_exec) {
                    cd->adpcm_freq = v;
                    cd->adpcm_cycles_per_sample = xl_adpcm_cycles_per_sample(v);
                }
                break;
            default: break;
            }
            return;
        }
        if (off == 0x0003 && (c->vdc.select & 0x1F) == 0x13) {
            static int s13_from = -2, s13_to = -1;
            if (s13_from == -2) {
                const char *ef = getenv("XLATE_S13_FROM");
                const char *et = getenv("XLATE_S13_TO");
                s13_from = ef ? atoi(ef) : -1;
                s13_to = et ? atoi(et) : -1;
            }
            if (s13_from >= 0 && c->vdc.frame >= s13_from &&
                (s13_to < 0 || c->vdc.frame <= s13_to) && c->log)
                xl_logf(c, "SATB write at frame %d from pc $%04X (bank $%02X) "
                           "value=$%04X", c->vdc.frame, c->pc, c->mpr[c->pc >> 13],
                        c->vdc.vwr);
        }
        if (off >= 0x0400 && off <= 0x0407) {
            vdc_write_vce(&c->vce, (uint16_t)(off - 0x0400), v);
            return;
        }
        if (off >= 0x0C00 && off <= 0x0FFF) {
            /* Timer, mirrored over the whole 4 KB (research §5.3).  Even address =
             * reload (write) / counter (read); odd address = control, bit 0 =
             * start/stop.  $0C02 is *another reload*, NOT an interrupt mask --
             * that misreading is what this round removes. */
            if (off & 1) {
                if ((v & 1) && !(c->timer.status & 1)) {
                    c->timer.div = 0;
                    c->timer.value = c->timer.load;
                }
                c->timer.status = (uint8_t)(v & 1);
                return;
            }
            c->timer.load = (uint8_t)(v & 0x7F);
            return;
        }
        if (off >= 0x1400 && off <= 0x17FF) {
            /* The CPU's interrupt controller (research §5.4), mirrored the same
             * way the timer is: low two bits select, the block repeats. */
            switch (off & 3) {
            case 2:
                /* $1402: the mask, INVERTED -- a set bit disables that source.
                 * A write therefore retires nothing; it only stops the CPU taking
                 * an already-pending line, which $1403 still reports. */
                c->irq_mask = (uint8_t)(v & XL_IQ_ALL);
                break;
            case 3:
                /* $1403: any write acknowledges the timer.  It does not touch the
                 * VDC's or the CD's flag -- those are cleared by reading the VDC
                 * status register and the CD's own $1801 respectively. */
                c->irq_status &= (uint8_t)~XL_IQ_TIMER;
                break;
            default:
                /* $1400/$1401: no register there. */
                break;
            }
            return;
        }
        if (off >= 0x1000 && off <= 0x13FF) {
            /* SEL/CLR transitions drive the multitap counter; see runtime.h's note
             * on joypad_index.  The two conditions are the reference core's, in the
             * same order: a SEL-high write that raises CLR resets the counter, and a
             * SEL 0->1 write with CLR low advances it. */
            uint8_t new_sel = (uint8_t)(v & 0x01);
            uint8_t new_clr = (uint8_t)((v >> 1) & 0x01);
            if (new_sel && !c->joypad_clr && new_clr) {
                c->joypad_index = 0;
            } else if (!c->joypad_clr && !new_clr && !c->joypad_sel && new_sel) {
                if (c->joypad_index < 255)
                    c->joypad_index++;
            }
            c->joypad_sel = new_sel;
            c->joypad_clr = new_clr;
            return;
        }
        if (c->log && c->hw_writes[off] <= 4)
            xl_logf(c, "hw write [$%04X] = $%02X (unimplemented port)", off, v);
        return;
    }
    if (bank < XL_ROM_BANKS && c->rom_present) {
        /* THE CARD IS READ-ONLY, and this is the machine's contract, not a
         * convenience: physical banks $00-$1F are the supplied System Card and a
         * store there is dropped by the mapper.  The image must never be corruptible
         * by the code that is executing out of it -- a BIOS that could overwrite
         * itself would produce a machine that no emulator agrees with. */
        c->rom_writes_ignored++;
        if (c->log && c->rom_writes_ignored <= 8)
            xl_logf(c, "rom: write to ROM page $%02X offset $%04X = $%02X ignored "
                       "(pc $%04X, bank $%02X) -- the card is read-only",
                    bank, off, v, c->pc, c->mpr[c->pc >> 13]);
        return;
    }
    if (bank == XL_BRAM_BANK) {
        /* The backup-RAM device (see XL_BRAM_BANK).  The 2 KB SRAM appears four
         * times in the 8 KB page, which is what an 8 KB window over a 2 KB part
         * gives and what the oracle's map does (`A & 0x7FF`); nothing in the corpus
         * addresses past $07FF, so no program's behaviour depends on the mirror. */
        if (c->bram_wprot) {
            c->bram_blocked++;
            if (c->log && c->bram_blocked <= 8)
                xl_logf(c, "bram: store $%02X to $F7:%04X DROPPED -- the card is "
                           "write-protected (frame %u, pc $%04X); write $80 to $1807 "
                           "to unlock it", v, off, (unsigned)c->vdc.frame, c->pc);
            return;
        }
        c->bram[off & (XL_BRAM_SIZE - 1)] = v;
        c->bram_writes++;
        return;
    }
    xl_stale_note(c, bank, off, XL_SW_STORE);
    c->mem[(size_t)bank * XL_BANK_SIZE + off] = v;
}

#ifdef XLATE_BUS_REC
/* See runtime.h: harness-only, compiled in with -DXLATE_BUS_REC, and inert for the
 * machine -- it observes the access xl_rd()/xl_wr() already made and changes
 * nothing.  A fixed 4096-entry ring is far more than any one instruction (the
 * longest block move the vector set carries is 500 bytes, ~1500 accesses). */
typedef struct XlBusRec { uint32_t phys; uint8_t v; uint8_t wr; } XlBusRec;
static XlBusRec xl_bus[4096];
static unsigned xl_bus_n;

void xl_bus_begin(void) { xl_bus_n = 0; }
unsigned xl_bus_count(void) { return xl_bus_n; }
void xl_bus_get(unsigned i, uint32_t *phys, uint8_t *v, int *rw)
{
    if (i >= xl_bus_n) { *phys = 0; *v = 0; *rw = 0; return; }
    *phys = xl_bus[i].phys; *v = xl_bus[i].v; *rw = xl_bus[i].wr;
}
void xl_bus_note(Cpu *c, uint16_t addr, uint8_t v, int wr)
{
    if (xl_bus_n < sizeof xl_bus / sizeof xl_bus[0]) {
        xl_bus[xl_bus_n].phys = (uint32_t)c->mpr[addr >> 13] * XL_BANK_SIZE
                                + (uint32_t)(addr & 0x1FFF);
        xl_bus[xl_bus_n].v = v;
        xl_bus[xl_bus_n].wr = (uint8_t)(wr ? 1 : 0);
        xl_bus_n++;
    }
}
#endif

uint8_t xl_rd(Cpu *c, uint16_t addr)
{
    uint8_t bank = c->mpr[addr >> 13];
    uint8_t v = xl_rd_phys(c, bank, (uint16_t)(addr & 0x1FFF));
    /* A read of the hardware page is the one thing the trace records besides
     * writes and events; on every other bank this is one test of a local. */
    if (bank == XL_IO_BANK)
        xl_trace_bus(c, 0, addr, v);
#ifdef XLATE_BUS_REC
    xl_bus_note(c, addr, v, 0);
#endif
    return v;
}

/* Zero-page word $17 is the count of the bank-$72 clear/fill loop.  Log every write
 * to it with the writing PC, so the chain back to whatever computes it is visible. */
static int zp17_on = -1;
static void xl_zp17_note(Cpu *c, uint8_t v)
{
    if (zp17_on < 0) {
        const char *e = getenv("XLATE_ZP17");
        zp17_on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (!zp17_on || !c->log)
        return;
    xl_logf(c, "zp17 f=%u = $%02X pc=$%04X (bank $%02X) mpr0=$%02X A=$%02X X=$%02X Y=$%02X",
            c->vdc.frame, v, c->pc, c->mpr[c->pc >> 13], c->mpr[0], c->a, c->x, c->y);
}

/* REPORT-ONLY instrument: every store into the work-RAM bank, with the writing PC,
 * for a window of frames.  The question it answers is "which code sets this
 * variable, and on which frame", which no amount of staring at a frame-boundary PC
 * can: the machine is inside the display-list renderer at almost every boundary.
 *
 *   XLATE_RAMW_FROM=<frame>  XLATE_RAMW_TO=<frame>   frame window (inclusive)
 *   XLATE_RAMW=<lo>:<hi>[,<lo>:<hi>...]               address filter, $2000-$3FFF
 *
 * Silent unless the frame window is given. */
static int ramw_from = -2, ramw_to = -1, ramw_nr;
static uint16_t ramw_lo[8], ramw_hi[8];
static int ramw_hit(uint16_t a)
{
    int i;
    if (!ramw_nr)
        return 1;
    for (i = 0; i < ramw_nr; i++)
        if (a >= ramw_lo[i] && a <= ramw_hi[i])
            return 1;
    return 0;
}
static void xl_ramw_note(Cpu *c, uint8_t bank, uint16_t off, uint8_t v)
{
    if (ramw_from == -2) {
        const char *a = getenv("XLATE_RAMW_FROM");
        const char *b = getenv("XLATE_RAMW_TO");
        const char *r = getenv("XLATE_RAMW");
        ramw_from = a ? atoi(a) : -1;
        ramw_to = b ? atoi(b) : -1;
        ramw_nr = 0;
        if (r) {
            const char *p = r;
            while (*p && ramw_nr < 8) {
                char *end;
                long lo = strtol(p, &end, 0);
                long hi = lo;
                if (*end == ':')
                    hi = strtol(end + 1, &end, 0);
                ramw_lo[ramw_nr] = (uint16_t)lo;
                ramw_hi[ramw_nr] = (uint16_t)hi;
                ramw_nr++;
                p = (*end == ',') ? end + 1 : end;
                if (!*p)
                    break;
            }
        }
    }
    if (ramw_from < 0 || !c->log || bank != XL_WORKRAM_BANK)
        return;
    if ((int)c->vdc.frame < ramw_from || (ramw_to >= 0 && (int)c->vdc.frame > ramw_to))
        return;
    if (!ramw_hit((uint16_t)(0x2000 + off)))
        return;
    xl_logf(c, "ramw f=%u $%04X = $%02X pc=$%04X b=%02X",
            (unsigned)c->vdc.frame, (unsigned)(0x2000 + off), v, c->pc,
            c->mpr[c->pc >> 13]);
}

void xl_wr(Cpu *c, uint16_t addr, uint8_t v)
{
    if (addr < 0x0100 && addr == 0x0017)
        xl_zp17_note(c, v);
    if (addr == 0x3F83) {
        /* XLATE_BM_PROBE: the "save file exists" byte the title screen's RUN gate
         * reads.  Who writes it, when, and with what is the whole question. */
        static int on83 = -1;
        if (on83 < 0) {
            const char *e = getenv("XLATE_BM_PROBE");
            on83 = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (on83 && c->log) {
            static int last83 = -1;
            xl_logf(c, "3f83 f=%u $%02X -> $%02X pc=$%04X (bank $%02X)",
                    (unsigned)c->vdc.frame, last83 < 0 ? 0 : last83, v, c->pc,
                    c->mpr[c->pc >> 13]);
            last83 = v;
        }
    }
    if (addr == (uint16_t)(XL_ZP_BASE | 0x01)) {
        static int on1 = -1;
        if (on1 < 0) {
            const char *e = getenv("XLATE_ZP01");
            on1 = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (on1 && c->log) {
            static int last1 = -1;
            xl_logf(c, "zp01 f=%u $%02X -> $%02X pc=$%04X (bank $%02X) A=$%02X",
                    c->vdc.frame, last1 < 0 ? 0 : last1, v, c->pc,
                    c->mpr[c->pc >> 13], c->a);
            last1 = v;
        }
    }
    if (addr == (uint16_t)(XL_ZP_BASE | 0xFD)) {
        static int on = -1;
        if (on < 0) {
            const char *e = getenv("XLATE_ZPFD");
            on = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (on && c->log) {
            static int last = -1;
            xl_logf(c, "zpfd f=%u $%02X -> $%02X pc=$%04X (bank $%02X) A=$%02X",
                    c->vdc.frame, last < 0 ? 0 : last, v, c->pc,
                    c->mpr[c->pc >> 13], c->a);
            last = v;
        }
    }
    /* Logical addresses: $3C80-$3C87 is the task-type byte of slots 0..7 in the
     * work-RAM window, so the bank must be the work-RAM bank, not just the offset. */
    if (addr >= 0x3C80 && addr <= 0x3C87 && c->mpr[addr >> 13] == XL_WORKRAM_BANK)
        xl_task_note(c, addr, v);
    xl_ramw_note(c, c->mpr[addr >> 13], (uint16_t)(addr & 0x1FFF), v);
    xl_wr_phys(c, c->mpr[addr >> 13], (uint16_t)(addr & 0x1FFF), v);
    /* A store to the hardware page is one bus record; a store to $1800 is also the
     * CD unit's start pulse, which the console records as a separate CDCMD event
     * immediately after the bus line. */
    if (c->mpr[addr >> 13] == XL_IO_BANK) {
        xl_trace_bus(c, 1, addr, v);
        if ((addr & 0x1FFF) == 0x1800)
            xl_trace_cdcmd(c, v);
    }
#ifdef XLATE_BUS_REC
    xl_bus_note(c, addr, v, 1);
#endif
}

uint16_t xl_rd16(Cpu *c, uint16_t addr)
{
    return (uint16_t)(xl_rd(c, addr) | ((uint16_t)xl_rd(c, (uint16_t)(addr + 1)) << 8));
}

uint16_t xl_rd16_zp(Cpu *c, uint8_t z)
{
    /* The pointer's high byte wraps inside the zero page (8-bit index). */
    return (uint16_t)(xl_rd(c, xl_zp(z)) |
                      ((uint16_t)xl_rd(c, xl_zp((uint8_t)(z + 1))) << 8));
}

/* THE VECTORS ARE THE CARD'S, read the way the CPU reads them: five 16-bit entries at
 * the top of the address space, through whatever the MPRs map there.  Printed when the
 * System Card is executed so that "the machine takes its vectors from the ROM" is a
 * line in the run's own output.  With bank $00 at $E000 these land in the last ten
 * bytes of the user's file: IRQ2 $FFF6, IRQ1 $FFF8, timer $FFFA, NMI $FFFC, reset
 * $FFFE (the hardware manual's order, and the order XL_HOOK_* documents). */
void xl_report_vectors(Cpu *c, FILE *out)
{
    fprintf(out, "VECTORS irq2=%04X irq1=%04X timer=%04X nmi=%04X reset=%04X "
                 "mpr7=%02X from_rom=%d\n",
            xl_rd16(c, 0xFFF6), xl_rd16(c, 0xFFF8), xl_rd16(c, 0xFFFA),
            xl_rd16(c, 0xFFFC), xl_rd16(c, 0xFFFE), c->mpr[7],
            (c->rom_present && c->mpr[7] < XL_ROM_BANKS) ? 1 : 0);
    fflush(out);
}

/* ---------------------------------------------------------------- stack -- */

void xl_push(Cpu *c, uint8_t v)
{
    xl_wr(c, (uint16_t)(XL_STACK_BASE | c->s), v);
    c->s--;
}

uint8_t xl_pop(Cpu *c)
{
    c->s++;
    return xl_rd(c, (uint16_t)(XL_STACK_BASE | c->s));
}

void xl_push16(Cpu *c, uint16_t v)
{
    xl_push(c, (uint8_t)(v >> 8));
    xl_push(c, (uint8_t)(v & 0xFF));
}

uint16_t xl_pop16(Cpu *c)
{
    uint8_t lo = xl_pop(c);
    uint8_t hi = xl_pop(c);
    return (uint16_t)(lo | ((uint16_t)hi << 8));
}

/* RTI.  Three pops and the interrupt-depth counter, in one place, because the trace
 * records the return here (`t=IRQE`): translated code used to inline these three
 * statements, and an instrument that cannot see every RTI would report a machine
 * that takes interrupts and never leaves them. */
void xl_rti(Cpu *c)
{
    c->p = xl_pop(c);
    c->pc = xl_pop16(c);
    if (c->in_irq)
        c->in_irq--;
    xl_trace_irqe(c);
}

/* ------------------------------------------------------------------ ALU -- */

void xl_nz(Cpu *c, uint8_t v)
{
    c->p &= (uint8_t)~(XL_N | XL_Z);
    if (v == 0)
        c->p |= XL_Z;
    else if (v & 0x80)
        c->p |= XL_N;
}

void xl_lda(Cpu *c, uint8_t v) { c->a = v; xl_nz(c, v); }
void xl_ldx(Cpu *c, uint8_t v) { c->x = v; xl_nz(c, v); }
void xl_ldy(Cpu *c, uint8_t v) { c->y = v; xl_nz(c, v); }

/* THE T FLAG'S THREE EXTRA CYCLES, WHICH THE DATA MOVEMENT ALREADY HAD.
 *
 * With T set, the reference core's AND/ORA/EOR/ADC do this around the operation:
 *
 *     #define TPREFIX { uint8 Abackup=A; if(P&T){ ADDCYC(1); A = RdMem(0x2000+X); }
 *     #define TPOSTFIX   if(P&T){ ADDCYC(1); WrMem(0x2000+X, A); LastCycle(); A = Abackup; } }
 *
 * -- one cycle before the read, one before the write and one for LastCycle: three
 * more than the un-T instruction.  This port moved the data (it had the prefix and
 * postfix) but charged none of the cycles, so every one of these instructions was
 * three cycles cheap whenever T was set.
 *
 * Measured, not reasoned: 20,057 published vectors (SingleStepTests/huc6280) with
 * T set in their initial P disagree with this machine on the cycle count alone, and
 * the difference is +3 in every single one; e.g. `05 #0` (ORA $68, T=1) is 7 cycles
 * on the vector and Mesen, 4 here.  The reference core charges 4 + 3 = 7.
 *
 * The ticks live HERE, not in the cycle table, because the cost is conditional on P:
 * a table entry is one number and the translator emits it as a literal xl_tick(N).
 * Charging it in the shared helper is the only way both engines pay it, and it is
 * the same structure the reference core uses. */
static void xl_tprefix(Cpu *c)
{
    if (c->p & XL_T) {
        c->abackup = c->a;
        xl_tick(c, 1);                                  /* ADDCYC(1) */
        c->a = xl_rd(c, (uint16_t)(XL_ZP_BASE | c->x));
    }
}

static void xl_tpostfix(Cpu *c)
{
    if (c->p & XL_T) {
        xl_wr(c, (uint16_t)(XL_ZP_BASE | c->x), c->a);
        xl_tick(c, 2);                                  /* ADDCYC(1) + LastCycle() */
        c->a = c->abackup;
    }
}

void xl_and(Cpu *c, uint8_t v)
{
    xl_tprefix(c);
    c->a &= v;
    xl_nz(c, c->a);
    xl_tpostfix(c);
}

void xl_ora(Cpu *c, uint8_t v)
{
    xl_tprefix(c);
    c->a |= v;
    xl_nz(c, c->a);
    xl_tpostfix(c);
}

void xl_eor(Cpu *c, uint8_t v)
{
    xl_tprefix(c);
    c->a ^= v;
    xl_nz(c, c->a);
    xl_tpostfix(c);
}

void xl_adc(Cpu *c, uint8_t v)
{
    xl_tprefix(c);
    if (c->p & XL_D) {
        uint32_t tmp = (uint32_t)(c->a & 0x0F) + (v & 0x0F) + (c->p & 1);
        uint8_t carry;
        if (tmp >= 0x0A)
            tmp += 0x06;
        tmp += (uint32_t)(c->a & 0xF0) + (v & 0xF0);
        if (tmp >= 0xA0)
            tmp += 0x60;
        carry = (tmp & 0xFF00) ? 1 : 0;
        c->a = (uint8_t)(tmp & 0xFF);
        c->p &= (uint8_t)~(XL_Z | XL_N | XL_C);
        if (carry)
            c->p |= XL_C;
        xl_nz(c, c->a);
        xl_tick(c, 1);          /* decimal mode costs one more (LastCycle in the ref) */
    } else {
        uint32_t l = (uint32_t)c->a + v + (c->p & 1);
        uint8_t vflag = (uint8_t)(((((c->a ^ v) & 0x80) ^ 0x80) & ((c->a ^ l) & 0x80)) >> 1);
        c->p &= (uint8_t)~(XL_Z | XL_C | XL_N | XL_V);
        c->p |= vflag;
        c->p |= (uint8_t)((l >> 8) & XL_C);
        c->a = (uint8_t)l;
        xl_nz(c, c->a);
    }
    xl_tpostfix(c);
}

void xl_sbc(Cpu *c, uint8_t v)
{
    if (c->p & XL_D) {
        uint8_t m = (uint8_t)((c->a & 0x0F) - (v & 0x0F) - ((c->p & 1) ^ 1));
        uint8_t n = (uint8_t)((c->a >> 4) - (v >> 4) - ((m >> 4) & 1));
        uint8_t res = (uint8_t)(((uint32_t)n << 4) | (m & 0x0F));
        if (m & 0x10)
            res = (uint8_t)(res - 0x06);
        if (n & 0x10)
            res = (uint8_t)(res - 0x60);
        c->a = res;
        c->p &= (uint8_t)~(XL_Z | XL_N | XL_C);
        if (!((n >> 4) & 1))
            c->p |= XL_C;
        xl_nz(c, c->a);
        xl_tick(c, 1);          /* decimal mode costs one more (LastCycle in the ref) */
    } else {
        int32_t l = (int32_t)c->a - v - ((c->p & 1) ^ 1);
        uint8_t vflag = (uint8_t)(((c->a ^ (uint8_t)l) & (c->a ^ v) & 0x80) >> 1);
        c->p &= (uint8_t)~(XL_Z | XL_C | XL_N | XL_V);
        c->a = (uint8_t)l;
        if (l >= 0)
            c->p |= XL_C;
        c->p |= vflag;
        xl_nz(c, c->a);
    }
}

static void xl_cmp_common(Cpu *c, uint8_t reg, uint8_t v)
{
    int16_t t = (int16_t)reg - (int16_t)v;
    xl_nz(c, (uint8_t)t);
    if (t >= 0)
        c->p |= XL_C;
    else
        c->p &= (uint8_t)~XL_C;
}

void xl_cmp(Cpu *c, uint8_t v) { xl_cmp_common(c, c->a, v); }
void xl_cpx(Cpu *c, uint8_t v) { xl_cmp_common(c, c->x, v); }
void xl_cpy(Cpu *c, uint8_t v) { xl_cmp_common(c, c->y, v); }

void xl_bit(Cpu *c, uint8_t v)
{
    c->p &= (uint8_t)~(XL_Z | XL_V | XL_N);
    if ((v & c->a) == 0)
        c->p |= XL_Z;
    c->p |= (uint8_t)(v & (XL_N | XL_V));
}

void xl_tst(Cpu *c, uint8_t memv, uint8_t imm)
{
    c->p &= (uint8_t)~(XL_Z | XL_V | XL_N);
    if ((memv & imm) == 0)
        c->p |= XL_Z;
    c->p |= (uint8_t)(memv & (XL_N | XL_V));
}

uint8_t xl_asl(Cpu *c, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~XL_C) | ((v >> 7) & XL_C));
    v = (uint8_t)(v << 1);
    xl_nz(c, v);
    return v;
}

uint8_t xl_lsr(Cpu *c, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~XL_C) | (v & XL_C));
    v >>= 1;
    xl_nz(c, v);
    return v;
}

uint8_t xl_rol(Cpu *c, uint8_t v)
{
    uint8_t in = (uint8_t)(c->p & XL_C);
    c->p = (uint8_t)((c->p & ~XL_C) | ((v >> 7) & XL_C));
    v = (uint8_t)((v << 1) | in);
    xl_nz(c, v);
    return v;
}

uint8_t xl_ror(Cpu *c, uint8_t v)
{
    uint8_t in = (uint8_t)((c->p & XL_C) ? 0x80 : 0x00);
    c->p = (uint8_t)((c->p & ~XL_C) | (v & XL_C));
    v = (uint8_t)((v >> 1) | in);
    xl_nz(c, v);
    return v;
}

uint8_t xl_inc(Cpu *c, uint8_t v)
{
    v++;
    xl_nz(c, v);
    return v;
}

uint8_t xl_dec(Cpu *c, uint8_t v)
{
    v--;
    xl_nz(c, v);
    return v;
}

/* --------------------------------------------------- mapper and hardware -- */

/* Every MPR change, with the page, the new value, the writing PC and the frame.
 * Env-gated (XLATE_MPRLOG=1): the paging timeline is the instrument for "which
 * overlay is selected when". */
static int mprlog_on = -1;
unsigned long xl_e9_window_count;
int xl_e9_window_from = -1, xl_e9_window_to = -1;
void xl_mprlog(Cpu *c, int page, uint8_t oldv, uint8_t newv)
{
    if (mprlog_on < 0) {
        const char *e = getenv("XLATE_MPRLOG");
        mprlog_on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (!mprlog_on || !c->log)
        return;
    xl_logf(c, "mpr f=%u page=%d $%02X->$%02X pc=$%04X (bank $%02X) A=$%02X%s",
            c->vdc.frame, page, oldv, newv, c->pc, c->mpr[c->pc >> 13], c->a,
            oldv == newv ? " (no change)" : "");
}

/* TAM writes the MPRs from A.  It is an *internal* instruction: the trace the
 * oracle gives us records MPR only at frame markers, so no external instrument
 * can name the instruction that paged a bank in.  Our own machine can, and the
 * page timeline is what "which overlay is selected when" is measured with, so
 * every write is logged with the PC that made it -- including the no-op writes
 * (same value re-written), because a loop that re-writes the same map every
 * frame is exactly what a parked machine looks like.
 *
 * Env-gated (XLATE_MPRLOG=1) like the rest of the instrumentation: a 12,500-frame
 * boot run makes ~188,000 page writes, and all but ~164,000 of them are the same
 * 18-write cycle repeated once per frame.
 */
void xl_tam(Cpu *c, uint8_t mask)
{
    int i;
    for (i = 0; i < 8; i++)
        if (mask & (1 << i)) {
            xl_mprlog(c, i, c->mpr[i], c->a);
            c->mpr[i] = c->a;
        }
}

void xl_tma(Cpu *c, uint8_t mask)
{
    int i;
    for (i = 0; i < 8; i++)
        if (mask & (1 << i))
            c->a = c->mpr[i];
}

/* ST0/ST1/ST2 write to physical bank $FF offsets 0/2/3 whatever the MPRs say. */
/* ST0/ST1/ST2 address the hardware page directly (they do not go through the MPRs),
 * and the console records them with the operand as the logical address: `t=W
 * pc=5B6B pa=1FE000 la=0000`.  The trace is given exactly that. */
void xl_st0(Cpu *c, uint8_t v)
{
    xl_wr_phys(c, XL_IO_BANK, 0x0000, v);
    xl_trace_bus(c, 1, 0x0000, v);
}

void xl_st1(Cpu *c, uint8_t v)
{
    xl_wr_phys(c, XL_IO_BANK, 0x0002, v);
    xl_trace_bus(c, 1, 0x0002, v);
}

void xl_st2(Cpu *c, uint8_t v)
{
    xl_wr_phys(c, XL_IO_BANK, 0x0003, v);
    xl_trace_bus(c, 1, 0x0003, v);
}


void xl_bmt(Cpu *c, int kind, uint16_t src, uint16_t dst, uint16_t len)
{
    /* A/X/Y are saved and restored around the transfer -- via the stack, as the
     * hardware does (so the stack page is scribbled just like the real thing).
     * Flags are untouched. */
    /* The hardware runs a do-while: the body executes `len` times, and a length of
     * zero wraps to a full 65536 (the counter is 16 bits and is decremented to zero).
     * Getting this wrong copies one byte too many, which silently clobbers the byte
     * just past the destination -- the kernel's own jump-table copy does exactly
     * that to a bank-base variable. */
    uint32_t i;
    uint32_t n = len ? (uint32_t)len : 65536u;
    xl_push(c, c->y);
    xl_push(c, c->a);
    xl_push(c, c->x);
    for (i = 0; i < n; i++) {
        switch (kind) {
        case XL_TII:
            xl_wr(c, dst, xl_rd(c, src));
            src++; dst++;
            break;
        case XL_TDD:
            xl_wr(c, dst, xl_rd(c, src));
            src--; dst--;
            break;
        case XL_TIN:
            xl_wr(c, dst, xl_rd(c, src));
            src++;
            break;
        case XL_TIA:
            /* dest alternates, source walks forward */
            xl_wr(c, (uint16_t)(dst + (uint16_t)(i & 1)), xl_rd(c, src));
            src++;
            break;
        case XL_TAI:
            /* dest walks forward, source alternates */
            xl_wr(c, dst, xl_rd(c, (uint16_t)(src + (uint16_t)(i & 1))));
            dst++;
            break;
        default:
            break;
        }
    }
    c->x = xl_pop(c);
    c->a = xl_pop(c);
    c->y = xl_pop(c);
}

/* ----------------------------------------------------------------- stubs -- */

/* The game builds a block-move routine in work RAM and calls it.  Found at bank
 * $75 offset $0E80-$0E9D: it stores a destination high byte to $22EB, $D3 (TAI) to
 * $22E7, $20/$00 to $22EC/$22ED (length $0020) and $60 (RTS) to $22EE, then does
 * `jsr $22E7`.  The routine it builds is
 *
 *      $22E7: D3 ss ss dd dd 20 00 60      tai  src,dst,len ; rts
 *
 * where the source and destination are patched in at the call site.  Code that only
 * exists at run time cannot be translated ahead of time, so the *shape* is modelled
 * here: on reaching work RAM, if the bytes are one of the five block-move opcodes
 * with a 16-bit operands and a trailing RTS, perform the move and return from the
 * call.  Anything else stops as untranslated, which keeps the model honest.
 */
int xl_ram_trampoline(Cpu *c)
{
    int kind;
    uint16_t src, dst, len;
    uint8_t op;

    if (c->pc < 0x2000 || c->pc >= 0x4000)   /* only the work-RAM page */
        return 0;
    op = xl_rd(c, c->pc);
    /* THE OPCODE TABLE IS THE HARDWARE'S, AND THIS ONE WAS WRONG.
     *
     * HuC6280 block moves are 0x73 TII, 0xC3 TDD, 0xD3 TIN, 0xE3 TIA, 0xF3 TAI
     * (huc6280_ops.inc, cases 0x73/0xC3/0xD3/0xE3/0xF3, each dispatching to
     * BMT_TII/TDD/TIN/TIA/TAI in huc6280.cpp).  This switch had 0xE3 as TIN,
     * 0xF3 as TIA and 0xD3 as TAI -- the low two opcodes rotated one place.
     *
     * It mattered: the System Card's palette upload is built at run time in work
     * RAM as `E3 00 C8 04 04 80 00 60` = TIA $C800,$0404,$0080, whose whole point
     * is that the destination ALTERNATES between the VCE's low-byte port $0404
     * and its high-bit port $0405 (huc6280.cpp BMT_TIA: `WrMem(bmt_dest +
     * bmt_alternate, ...)`).  Read as TIN, all 128 bytes landed on $0404, so the
     * palette was never uploaded and only entry 0 changed.  The console's own
     * cycle trace at frame 2 shows the same data written alternately to $0404 and
     * $0405 -- TIA -- and its work RAM at the start of frame 3 holds exactly these
     * bytes, so the opcode is not in doubt. */
    switch (op) {
    case 0x73: kind = XL_TII; break;
    case 0xC3: kind = XL_TDD; break;
    case 0xD3: kind = XL_TIN; break;
    case 0xE3: kind = XL_TIA; break;
    case 0xF3: kind = XL_TAI; break;
    default:
        return 0;
    }
    if (xl_rd(c, (uint16_t)(c->pc + 7)) != 0x60)   /* must be `rts` */
        return 0;
    src = (uint16_t)(xl_rd(c, (uint16_t)(c->pc + 1)) |
                     (xl_rd(c, (uint16_t)(c->pc + 2)) << 8));
    dst = (uint16_t)(xl_rd(c, (uint16_t)(c->pc + 3)) |
                     (xl_rd(c, (uint16_t)(c->pc + 4)) << 8));
    len = (uint16_t)(xl_rd(c, (uint16_t)(c->pc + 5)) |
                     (xl_rd(c, (uint16_t)(c->pc + 6)) << 8));
    if (c->log)
        xl_logf(c, "ram routine at $%04X: block move kind %d src=$%04X dst=$%04X len=%u",
                c->pc, kind, src, dst, (unsigned)len);
    c->pc = (uint16_t)(c->pc + 8);              /* one step, like the move itself */
    xl_bmt(c, kind, src, dst, len);
    c->pc = (uint16_t)(xl_pop16(c) + 1);        /* the routine's `rts` */
    return 1;
}

static uint8_t zp16_lo(Cpu *c, uint8_t z) { return xl_rd(c, xl_zp(z)); }

/* Repeated identical BIOS calls are the normal state of a spin-wait, so the log
 * keeps one line per distinct call signature and a count, instead of thousands of
 * identical lines that bury the interesting ones. */
/* A boot program waiting on a BIOS service calls it thousands of times with
 * slightly different parameters.  Full detail for the first few calls of each
 * entry, then a periodic count: enough to diagnose, not enough to bury the rest. */
#define XL_ENTRY_SLOTS 64
static unsigned xl_entry_hits[XL_ENTRY_SLOTS];
static unsigned xl_entry_total[XL_ENTRY_SLOTS];

static int xl_entry_slot(uint16_t pc)
{
    int idx = (int)((pc - 0xE000u) / 3u);
    if (idx < 0 || idx >= XL_ENTRY_SLOTS)
        return -1;
    return idx;
}

/* 1 = log this call in full */
static int xl_should_log(uint16_t pc)
{
    int idx = xl_entry_slot(pc);
    if (idx < 0)
        return 1;
    xl_entry_total[idx]++;
    xl_entry_hits[idx]++;
    if (xl_entry_hits[idx] <= 3)
        return 1;
    if ((xl_entry_total[idx] % 20000u) == 0)
        return 1;
    return 0;
}

/* Per-entry call counts: "called once" and "polled every frame" look identical in a
 * deduplicated log but mean opposite things about where the game is stuck. */
uint32_t xl_bios_entry_count[256];

void xl_bios_call(Cpu *c)
{
    uint16_t entry = c->pc;
    xl_bios_entry_count[(entry - 0xE000) & 0xFF]++;
    unsigned count = (unsigned)(zp16_lo(c, 0xF8) | ((unsigned)zp16_lo(c, 0xF9) << 8));
    unsigned dest_addr = (unsigned)(zp16_lo(c, 0xFA) | ((unsigned)zp16_lo(c, 0xFB) << 8));
    /* THE RECORD NUMBER IS 24 BITS, $FC/$FD/$FE, MOST SIGNIFICANT BYTE FIRST.
     *
     *   $20FC  CL  record H   bits 16-23
     *   $20FD  CH  record M   bits  8-15
     *   $20FE  DL  record L   bits  0-7    (this is the ONLY byte we used to read)
     *   $20FF  DH  destination class in bits 7-1, count unit in bit 0
     *
     * Source: Hudson's System Card BIOS manual prints the parameter block with these
     * four columns, and the file
     * table in CD_DOC.DIR stores the same three bytes low, middle, high.
     *
     * This port used to read $FC/$FD as a "destination bank", which the routine does
     * not have: the transfer's store is an ordinary `sta [_bl],Y` through $FA/$FB, so
     * the destination bank is whichever one the MPRs map at the moment of the store.
     * Calling those two bytes a bank silently discarded record bits 8-23 -- every
     * record number arrived 256 (or 65,536) too low. */
    unsigned record_h   = zp16_lo(c, 0xFC);
    unsigned record_m   = zp16_lo(c, 0xFD);
    unsigned sector = zp16_lo(c, 0xFE);            /* record L */
    unsigned record_num = (record_h << 16) | (record_m << 8) | sector;
    unsigned sector_mode = zp16_lo(c, 0xFF);       /* transfer mode */
    int modelled = 0, record = -1;
    unsigned long bytes = 0;
    /* Set by a handler whose bytes legitimately do not land in CPU memory, so the
     * caller-facing log can say where they went instead of printing a bank list. */
    const char *stored = NULL;
    char err[128];

    err[0] = '\0';
    c->bios_calls++;
    {
        static int wf = -2, wt = -1;
        
        if (wf == -2) {
            const char *e = getenv("XLATE_CDWIN");
            wf = -1;
            if (e && e[0]) {
                unsigned a = 0, b = 0;
                if (sscanf(e, "%u:%u", &a, &b) == 2) { wf = (int)a; wt = (int)b; }
            }
        }
        xl_e9_window_from = wf; xl_e9_window_to = wt;
        if (wf >= 0 && (int)c->vdc.frame >= wf && (int)c->vdc.frame <= wt) {
            if (entry == 0xE009) xl_e9_window_count++;
        }
    }

    if (entry == 0xE033) {
        /* ROM $F393 -- ad_trans.  Read straight off the routine's own body (disassembled
         * from the System Card image, file offset $1393, using re/huc6280.py); nothing
         * in the parameter layout below is inferred:
         *
         *   $F393  tst #$03,$0B03 / beq $F39D     ; drive-not-ready guard -> A = 4
         *   $F39D  stz $2273                      ; clear the retry/offset index
         *   $F3A0  jsr $F309                      ; stash $F8..$FF at $2260
         *   $F3A3  lda $FF / bne $F3B1            ; destination class
         *   $F3A7  ldx $FA / ldy $FB
         *   $F3AB  jsr $F729                      ; $1808 = $FA, $1809 = $FB
         *   $F3B1  jsr $F0EE                      ; clear $224C..$2255
         *   $F3B4  lda #$08 / sta $224C
         *   $F3BC  ldx #4 / ldy #1 / jsr $F327    ; $2251 = $F9
         *   $F3C3  lda $F8 / sta $2250            ; SECTOR COUNT: $F8 alone, one byte
         *   $F3C8  jsr $E900                      ; issue the drive command
         *   $F3CB  lda #2 / sta $180B
         *   $F3D0  tst #$04,$0C04 / bne $F3D0     ; wait for the transfer to finish
         *   $F3D6  lda $1803 / and #$20 / beq $F3D0
         *   $F3DD  stz $180B / jsr $E9C5          ; result: A = 0 when accepted
         *
         * So the block is the one cd_read takes:
         *   $FC/$FD/$FE  record number, 24 bits, most significant first
         *   $F8          sector count
         *   $FA/$FB      destination address
         *   $FF          destination class (0 = the form modelled here)
         *
         * Confirmed by the machine, not just by the code: the game's loader at bank $6A
         * $6E9D-$6EEA builds $FC/$FD/$FE = 00/01/99 (record 409), $F8 = $20 (32
         * sectors), $FF = 0, $FA/$FB = $0000, and the console's very next CD command is
         * Read(6) LBA $10CB = record 409, 32 sectors, accepted at frame 9,874.  The
         * 32 sectors then arrive one per frame over 34 frames (`t=SECTOR` events in
         * the console's access trace).
         *
         * WHERE THE BYTES GO: not into CPU memory.  Whole-RAM dumps of the console
         * before (9,900) and after (9,910 / 9,920 / 9,940) that transfer show ZERO
         * changed bytes across all 32 Super CD RAM banks, and only ordinary work-RAM
         * activity -- a 64 KB store through the MPRs, which is what cd_read does, would
         * have rewritten pages 0-7 including the I/O page and bank $69 the loader
         * returns into.  $1808/$1809 are the ADPCM address registers, so this fills the
         * drive's own ADPCM sample RAM: the data is sound, it costs the measured flight
         * time, and the CPU map is untouched.  That sample RAM is not modelled yet, so
         * the bytes are read from the disc (a record past the end of the payload still
         * fails honestly) and then dropped, with the parameters logged.
         *
         * This is also what unblocks the 9,835 stop: the old handler called this entry
         * CD-DA play, transferred nothing and waited nothing, so the loader's 32-sector
         * handshake never happened and the machine ran on into stale code. */
        unsigned cls = sector_mode;             /* $FF: destination class */
        unsigned cnt = count & 0xFFu;           /* $F8: sector count */

        record = (int)record_num;
        if (cls != 0) {
            snprintf(err, sizeof err,
                     "$E033 destination class $%02X (only class 0, the ADPCM form, "
                     "is modelled)", cls);
            goto done;
        }
        if (!c->disc_attached) {
            snprintf(err, sizeof err, "no disc attached");
            goto done;
        }
        if (cnt == 0) {
            snprintf(err, sizeof err, "$E033 asked for zero sectors");
            goto done;
        }
        {
            size_t off = ((size_t)record_num + 225u) * 2048u;
            size_t n = (size_t)cnt * 2048u;
            if (off >= c->disc_len) {
                snprintf(err, sizeof err, "record $%02X past end of payload",
                         (unsigned)record_num);
                goto done;
            }
            if (c->disc_len - off < n)
                n = c->disc_len - off;
            /* The ROM's own side effects, in its own order ($F39D-$F3DD). */
            xl_wr(c, 0x2273, 0x00);             /* no retry */
            xl_wr(c, 0x224C, 0x08);
            xl_wr(c, 0x2250, (uint8_t)cnt);
            c->cd.dest = (uint16_t)(dest_addr & 0xFFFFu);   /* $1808/$1809 */
            c->cd.control = (uint8_t)(c->cd.control | 0x02);
            xl_cd_prime(c, record_num, cnt);
            xl_cd_move(c, off, n, (uint16_t)(dest_addr & 0xFFFFu), XL_CD_TO_ADPCM);
            c->a = 0x00;                        /* accepted */
            c->x = 0x00;
            c->p &= (uint8_t)~XL_C;
            bytes = (unsigned long)n;
            modelled = 1;
            c->modelled_reads++;
            c->read_bytes += n;
            stored = "ad_trans: sectors went to the drive's ADPCM sample RAM "
                     "(not modelled), no CPU-memory store";
            if (c->log)
                xl_logf(c, "bios: $E033 ad_trans record $%06X (%u sectors) -> "
                           "$1808/$1809 = $%04X, class $%02X, %lu bytes, "
                           "%u frames of drive time",
                        record_num, cnt, dest_addr & 0xFFFFu, cls,
                        (unsigned long)n, cnt + 2u);
            goto done;
        }
    }
    if (entry == 0xE012) {
        /* ROM $EE10 -- this is `cd_play` (Red Book audio), NOT a sector reader.  The
         * sector reader is $E009 -> $EC05 (`cd_read`), which this port models and has
         * verified byte-for-byte against the boot chain.
         *
         * The call site at $448B is genuinely `jsr $E012` (bytes 20 12 E0), so the game
         * really does call cd_play here; the parameter block it stages at $4498 is
         * read-shaped, which is what misled an earlier round into giving this entry a
         * per-sector data rate.  A play command delivers no sectors, so that rate is
         * withdrawn.
         *
         * Model: accept the command, record the drive control state, transfer nothing.
         * $224C = $D8 and $2255 = ($FB & $C0) are what the ROM itself writes, so they
         * are kept. */
        xl_wr(c, 0x224C, 0xD8);
        xl_wr(c, 0x2255, (uint8_t)(xl_rd(c, 0x20FB) & 0xC0));
        xl_wr(c, 0x1801, 0x81);
        xl_wr(c, 0x1800, 0x60);
        c->cd.control = (uint8_t)(c->cd.control | 0x02);
        c->a = 0x00;
        c->x = 0x00;
        c->p &= (uint8_t)~XL_C;
        if (c->log)
            xl_logf(c, "bios: $E012 = cd_play (audio), accepted; no data transfer");
        modelled = 1;
        c->modelled_reads++;
        goto done;
    }
    if (entry == 0xE02D) {
        /* ROM $F379: two instructions -- `and #$0F; sta $180F`.  A drive control
         * write the boot program issues right after its disc command completes
         * ($4492: cla; jsr $E02D).  It was unmodelled, so the write never happened. */
        xl_wr(c, 0x180F, (uint8_t)(c->a & 0x0F));
        c->a = (uint8_t)(c->a & 0x0F);
        c->p &= (uint8_t)~XL_C;
        modelled = 1;
        c->modelled_reads++;
        goto done;
    }
    if (entry == 0xE06C) {
        /* ROM $E272: set the VDC memory-width register (register $09) from A and
         * the shadow $2242:
         *     $2242 = (A & $8F) | ($70 & $2242) ; VDC $09 = $2242 ; $F7 = 9
         * This is the routine that decides the background map's size and layout,
         * because MWR bits 4-6 are the map dimensions the renderer reads. */
        uint8_t v = (uint8_t)((c->a & 0x8F) | (0x70 & xl_rd(c, 0x2242)));
        xl_wr(c, 0x2242, v);
        xl_wr(c, 0x20F7, 0x09);
        vdc_write(&c->vdc, 0x0000, 0x09);
        vdc_write(&c->vdc, 0x0002, v);
        c->a = v;
        c->p &= (uint8_t)~XL_C;
        modelled = 1;
        c->modelled_reads++;
        goto done;
    }
    if (entry == 0xE06F) {
        /* ROM $E29D: "set screen".  A = flags (bit 0 selects the wide layout, bit 2
         * is carried through), X = width in tiles, Y = height in tiles:
         *   $2244 = A & $05 ; $2245 = X ; $2246 = Y
         * Height must be $0A..$1E and width $0A..$22 ($10..$2C when bit 0 is set);
         * otherwise $E292 substitutes A=0, X=$20, Y=$1E and reports A=1, carry set.
         * Then $E341 programs the vertical timing from the height:
         *   VSR = (2, ((Y<<3)>>1)^$FF + $88)      -> $0F02 for Y=$1E
         *   VDR = (Y<<3)-1, 0                     -> $00EF for Y=$1E
         * and the horizontal timing from the width:
         *   VDC $0A (HSR) = (2 or 3, ((X-1)>>1)^$FF + $13 or $19)
         *   VDC $0B (HDR) = (X-1, ((that + X)^$FF) + $27 or $35)
         * finishing with $E272 (memory-width register) with A=0 for the narrow layout
         * or A=$0A for the wide one.
         *
         * Verified against the console: with the kernel's own call ($410C:
         * A=0, X=$20, Y=$1E) this produces HSR=$0202, HDR=$041F, VSR=$0F02, which
         * are exactly the oracle's values at the logo. */
        unsigned flags = (unsigned)(c->a & 0x05);
        unsigned w = c->x, h = c->y;
        int bad = 0;
        xl_wr(c, 0x2244, (uint8_t)flags);
        xl_wr(c, 0x2245, (uint8_t)w);
        xl_wr(c, 0x2246, (uint8_t)h);
        if (h < 0x0A || h >= 0x1F)
            bad = 1;
        else if ((flags & 1) ? (w < 0x10 || w >= 0x2D) : (w < 0x0A || w >= 0x23))
            bad = 1;
        if (bad) {
            flags = 0;
            w = 0x20;
            h = 0x1E;
            xl_wr(c, 0x2244, 0x00);
            xl_wr(c, 0x2245, 0x20);
            xl_wr(c, 0x2246, 0x1E);
        }
        {
            unsigned xv = (unsigned)((h << 3) & 0xFF);   /* asl a; asl a; asl a */
            unsigned lo = (unsigned)((xv - 1) & 0xFF);
            unsigned hi = (unsigned)(((((xv >> 1) ^ 0xFF) + 0x88)) & 0xFF);
            xl_wr(c, 0x20F7, 0x0C);
            vdc_write(&c->vdc, 0x0000, 0x0C);
            vdc_write(&c->vdc, 0x0002, 0x02);
            vdc_write(&c->vdc, 0x0003, (uint8_t)hi);
            vdc_write(&c->vdc, 0x0000, 0x0D);
            vdc_write(&c->vdc, 0x0002, (uint8_t)lo);
            vdc_write(&c->vdc, 0x0003, 0x00);
        }
        if (flags & 1) {
            unsigned hi = (unsigned)(((((((w - 1) >> 1)) ^ 0xFF)) + 0x19) & 0xFF);
            unsigned y2 = (unsigned)(((((hi + w) & 0xFF)) ^ 0xFF) & 0xFF);
            vdc_write(&c->vdc, 0x0000, 0x0A);
            vdc_write(&c->vdc, 0x0002, 0x03);
            vdc_write(&c->vdc, 0x0003, (uint8_t)hi);
            vdc_write(&c->vdc, 0x0000, 0x0B);
            vdc_write(&c->vdc, 0x0002, (uint8_t)((w - 1) & 0xFF));
            vdc_write(&c->vdc, 0x0003, (uint8_t)((y2 + 0x35) & 0xFF));
            {
                uint8_t v = (uint8_t)((0x0A & 0x8F) | (0x70 & xl_rd(c, 0x2242)));
                xl_wr(c, 0x2242, v);
                xl_wr(c, 0x20F7, 0x09);
                vdc_write(&c->vdc, 0x0000, 0x09);
                vdc_write(&c->vdc, 0x0002, v);
            }
        } else {
            unsigned hi = (unsigned)(((((w >> 1) ^ 0xFF)) + 0x13) & 0xFF);
            unsigned y2 = (unsigned)(((((hi + w) & 0xFF)) ^ 0xFF) & 0xFF);
            vdc_write(&c->vdc, 0x0000, 0x0A);
            vdc_write(&c->vdc, 0x0002, 0x02);
            vdc_write(&c->vdc, 0x0003, (uint8_t)hi);
            vdc_write(&c->vdc, 0x0000, 0x0B);
            vdc_write(&c->vdc, 0x0002, (uint8_t)((w - 1) & 0xFF));
            vdc_write(&c->vdc, 0x0003, (uint8_t)((y2 + 0x27) & 0xFF));
            {
                uint8_t v = (uint8_t)((0x00 & 0x8F) | (0x70 & xl_rd(c, 0x2242)));
                xl_wr(c, 0x2242, v);
                xl_wr(c, 0x20F7, 0x09);
                vdc_write(&c->vdc, 0x0000, 0x09);
                vdc_write(&c->vdc, 0x0002, v);
            }
        }
        /* $E292/$E299: the default path returns A=1 with carry set; a valid call
         * falls through $E2FC/$E33F with A=0 and carry clear. */
        c->a = bad ? 0x01 : 0x00;
        c->p = (uint8_t)((c->p & (uint8_t)~XL_C) | (bad ? XL_C : 0));
        if (c->log)
            xl_logf(c, "bios: $E06F screen %ux%u tiles (flags $%02X)%s -> "
                       "HSR=$%04X HDR=$%04X VSR=$%04X VDR=$%04X MWR=$%04X",
                    w, h, flags, bad ? " [out of range, defaults used]" : "",
                    c->vdc.hsr, c->vdc.hdr, c->vdc.vsr, c->vdc.vdr, c->vdc.mwr);
        modelled = 1;
        c->modelled_reads++;
        goto done;
    }
    if (entry == 0xE04E) {
        /* ROM $F8E3 -- the file-read-by-name service.  The contract, decoded
         * instruction by instruction:
         *
         *   $F8/$F9  pointer to a 12-character name (verified: the game's call at
         *            bank $6A $6C24 points at $6C9A, and those twelve bytes are
         *            "X DRACULA X ").
         *   $FA/$FB  destination address.
         *   $FE/$FF  position inside the file (a cursor the caller keeps).
         *   $FC/$FD  caller's byte budget.
         * Returns A: 0 = transferred, 1 = name not found, 2 = malformed entry,
         *   $FF = no file table present.  $FC/$FD come back as the byte count moved.
         *
         *   $FC32  checks the table signature: $8000..$8003 must read "HUBM".
         *          On mismatch it returns $FF straight to the caller ($FC42).
         *   $FB96  walks a linked list starting at $8010.  Each entry is
         *          [u16 link][u16 spare][12-char name][data...]; the link is
         *          *relative* -- $FBD9 adds it to the entry address -- and 0 ends
         *          the list.  Twelve characters are compared at $FBBF-$FBC8.
         *   $F8F5  if the requested position is at or past the entry length,
         *          there is nothing to move: $FC/$FD = 0, A = 0 ($F943).
         *   $FB46  clamps the transfer: budget' = min(budget, length - 16 - pos),
         *          and $EC/$ED = entry + 16 + pos is the source ($F0+$10).
         *   $FD17  copies budget' bytes from $EC to $EE (= $FA/$FB); $FD97 is the
         *          16-bit down-counter that ends the loop.
         *
         * The payload in this repository has no such table (its boot sector is the
         * "SAMPLE PROGRAM" demo header, and "HUBM" does not occur anywhere on the
         * disc), so the signature path is the one that actually runs here.  It is
         * still implemented to the letter, because returning 0 -- "found and
         * transferred" -- is a different answer that sends the caller down a path
         * it should not take. */
        uint16_t name_ptr = (uint16_t)(xl_rd(c, 0x20F8) | (xl_rd(c, 0x20F9) << 8));
        uint16_t entry_ptr = 0x8010;
        int found = 0, walk;
        if (xl_rd(c, 0x8000) != 'H' || xl_rd(c, 0x8001) != 'U' ||
            xl_rd(c, 0x8002) != 'B' || xl_rd(c, 0x8003) != 'M') {
            if (c->log)
                xl_logf(c, "bios: $E04E file table absent (no \"HUBM\" at $8000) "
                           "-- returning $FF, as ROM $FC42 does");
            c->a = 0xFF;
            c->p = (uint8_t)((c->p & (uint8_t)~XL_C) | XL_N);
            modelled = 1;
            c->modelled_reads++;
            goto done;
        }
        for (walk = 0; walk < 64 && !found; walk++) {
            uint16_t link = (uint16_t)(xl_rd(c, entry_ptr) |
                                       (xl_rd(c, (uint16_t)(entry_ptr + 1)) << 8));
            if (link == 0)
                break;
            {
                int i, same = 1;
                for (i = 0; i < 12; i++)
                    if (xl_rd(c, (uint16_t)(entry_ptr + 4 + i)) !=
                        xl_rd(c, (uint16_t)(name_ptr + i))) {
                        same = 0;
                        break;
                    }
                if (same)
                    found = 1;
                else
                    entry_ptr = (uint16_t)(entry_ptr + link);
            }
        }
        if (!found) {
            if (c->log)
                xl_logf(c, "bios: $E04E name not in the file table -- A=1 (ROM $F94B)");
            c->a = 0x01;
            c->p = (uint8_t)((c->p & (uint8_t)~XL_C) | XL_N);
            modelled = 1;
            c->modelled_reads++;
            goto done;
        }
        {
            uint16_t len = (uint16_t)(xl_rd(c, entry_ptr) |
                                      (xl_rd(c, (uint16_t)(entry_ptr + 1)) << 8));
            uint16_t spare = (uint16_t)(xl_rd(c, (uint16_t)(entry_ptr + 2)) |
                                        (xl_rd(c, (uint16_t)(entry_ptr + 3)) << 8));
            uint16_t dest = (uint16_t)(xl_rd(c, 0x20FA) | (xl_rd(c, 0x20FB) << 8));
            unsigned pos = sector;                 /* $FE/$FF cursor */
            unsigned budget = count;               /* $FC/$FD budget */
            unsigned moved;
            if (pos >= len) {
                xl_wr(c, 0x20FC, 0);
                xl_wr(c, 0x20FD, 0);
                c->a = 0;
                modelled = 1;
                c->modelled_reads++;
                goto done;
            }
            /* $FB46: clamp the budget to what is left in the file. */
            moved = (pos + budget + 16u > len) ? (unsigned)(len - 16u - pos) : budget;
            if (moved > 0x8000u)
                moved = 0x8000u;
            {
                unsigned i;
                for (i = 0; i < moved; i++)
                    xl_wr(c, (uint16_t)(dest + i),
                          xl_rd(c, (uint16_t)(entry_ptr + 16u + pos + i)));
            }
            xl_wr(c, 0x20FC, (uint8_t)(moved & 0xFF));
            xl_wr(c, 0x20FD, (uint8_t)(moved >> 8));
            c->a = spare ? 0x02 : 0x00;            /* $F950: spare field non-zero */
            if (c->log)
                xl_logf(c, "bios: $E04E %u bytes from entry $%04X+16+%u to $%04X "
                           "(file length %u)", moved, entry_ptr, pos, dest, len);
            modelled = 1;
            c->modelled_reads++;
            goto done;
        }
    }
    if (entry == 0xE0BD) {
        /* ROM $FDC6: a 16-bit MULTIPLY, not a file loader.
         *
         * Body: `bsr $FDA9` (bank $03 into $C000), `jsr $CD3A`, `bra $FDB5`
         * (restore the bank).  $CD3A, instruction for instruction:
         *
         *   lda $F8 ; ldx $FA ; sta $FC ; cla
         *   loop 8 times:  asl a / sax / rol a / sax
         *                  bcc skip ; clc ; adc $FC ; bcc skip ; inx
         *   stx $FD ; sta $FC ; rts
         *
         * i.e. `$FC/$FD = $F8 * $FA` (low in A and $FC, high in X and $FD).
         *
         * The game uses this as its table index: at bank $69 $413F and $4144 it
         * calls `jsr $6FD8` twice, and $6FD8 is `sta $F8; lda #$05; sta $FA;
         * jsr $E0BD`, then builds a pointer as `$74D1 + $FC/$FD`.  With $F8=6
         * that is $74D1 + 30 = **$74EF**, which is exactly the live pointer the
         * console's own work RAM holds at f1837.  Our stub used to return
         * A=0/X=0/C=0 and leave $FC/$FD untouched, so the pointer came out as
         * $71C5 -- the unexplained divergence recorded in STATUS for five rounds.
         *
         * Flags are left as the ROM leaves them: the last instruction that
         * touches them is `bne` in the loop (or the `sta $FC`, which sets none),
         * so C is whatever the last carry test left.  Reproduce the loop rather
         * than the product, so nothing downstream can tell the difference. */
        {
            unsigned a, x, mult;
            int i, cy = 0, cout = 0;
            /* The ROM reads both operands from the zero page (`lda $F8`,
             * `ldx $FA`), not from the caller's registers, so do the same. */
            mult = xl_rd(c, 0x20F8);
            x = xl_rd(c, 0x20FA);
            a = 0;
            for (i = 0; i < 8; i++) {
                cy = (a >> 7) & 1;                 /* asl a */
                a = (a << 1) & 0xFF;
                {                                  /* sax; rol a; sax */
                    unsigned nx = ((x << 1) & 0xFFu) | (unsigned)cy;
                    cy = (x >> 7) & 1;
                    x = nx;
                }
                cout = cy;
                if (cy) {                          /* clc; adc $FC */
                    unsigned sum = a + mult;
                    a = sum & 0xFF;
                    cout = (sum > 0xFF);
                    if (cout)
                        x = (x + 1) & 0xFF;        /* inx */
                }
            }
            xl_wr(c, 0x20FC, (uint8_t)a);
            xl_wr(c, 0x20FD, (uint8_t)x);
            c->a = (uint8_t)a;
            c->x = (uint8_t)x;
            /* C is whatever the last iteration's `bcc` tested (see above). */
            c->p = (uint8_t)((c->p & (uint8_t)~XL_C) | (cout ? XL_C : 0));
            modelled = 1;
            c->modelled_reads++;
            if (c->log)
                xl_logf(c, "bios: $E0BD multiply $F8($%02X) x $FA($%02X) = "
                           "$%02X%02X", mult, xl_rd(c, 0x20FA), (unsigned)x,
                        (unsigned)a);
            goto done;
        }
    }
    if (entry == 0xE01B) {
        /* ROM $F347: `cmp #$00; bne ...; lda $1800; and #$80; rts` -- with A = 0
         * this is "is the drive busy", returning bit 7 of $1800.  An idle drive
         * answers 0, which is what the caller polls for. */
        c->a = (uint8_t)(xl_rd(c, 0x1800) & 0x80);
        c->x = 0;
        c->p &= (uint8_t)~XL_C;
        modelled = 1;
        c->modelled_reads++;
        goto done;
    }
    if (entry == 0xE042) {
        /* ROM $F6C1: clear the drive's interrupt-enable bits and the two control
         * bits, then drop the four ADPCM pointers:
         *   $1802 &= ~$0C ; $180D &= ~$60 ; $22A5 = 0 ; $229D..$22A0 = 0. */
        xl_wr(c, 0x1802, (uint8_t)(xl_rd(c, 0x1802) & (uint8_t)~0x0C));
        xl_wr(c, 0x180D, (uint8_t)(xl_rd(c, 0x180D) & (uint8_t)~0x60));
        xl_wr(c, 0x22A5, 0x00);
        xl_wr(c, 0x229D, 0x00);
        xl_wr(c, 0x229E, 0x00);
        xl_wr(c, 0x229F, 0x00);
        xl_wr(c, 0x22A0, 0x00);
        c->a = 0x60;
        c->p &= (uint8_t)~XL_C;
        modelled = 1;
        c->modelled_reads++;
        goto done;
    }
    if (entry == 0xE045) {
        /* ROM $F6DB, instruction for instruction:
         *   A = ($180C & 1) ? 1 : ($1803 & 4)
         *   X = ($180D & $20) ? (same value) : ($180C & 8)
         * i.e. "report the drive's state bits" with two different sources for the
         * same answer depending on which control bits are set. */
        uint8_t a = 0, x;
        if (xl_rd(c, 0x180C) & 0x01)
            a = 0x01;
        else
            a = (uint8_t)(xl_rd(c, 0x1803) & 0x04);
        if (xl_rd(c, 0x180D) & 0x20)
            x = a;
        else
            x = (uint8_t)(xl_rd(c, 0x180C) & 0x08);
        c->a = a;
        c->x = x;
        c->p = (uint8_t)((c->p & (uint8_t)~XL_C) | (a ? 0 : XL_C));
        modelled = 1;
        c->modelled_reads++;
        goto done;
    }
    if (entry == 0xE069) {
        /* ROM $E267: set the VCE's screen-mode register from A:
         *   A = ($2242 & $8F) | ((A & 7) << 4) ; $2242 = A ; $F7 = 9 ;
         *   select $09 ; write A.  (The game calls it with A = 3 at bank $6B
         *   $8060, right after the intro's first state runs.) */
        uint8_t v = (uint8_t)((xl_rd(c, 0x2242) & 0x8F) | ((c->a & 0x07) << 4));
        xl_wr(c, 0x2242, v);
        xl_wr(c, 0x20F7, 0x09);
        vdc_write(&c->vdc, 0x0000, 0x09);
        vdc_write(&c->vdc, 0x0002, v);
        c->a = v;
        c->p &= (uint8_t)~XL_C;
        modelled = 1;
        c->modelled_reads++;
        goto done;
    }
    if (entry == 0xE05D) {
        /* BIOS $F034: install an interrupt hook.  A = hook index, X = address low,
         * Y = address high; the routine does `asl a; tax` and then
         * `sta $2200,x` twice, so index N writes to $2200+2N (low) and $2201+2N
         * (high).  Index 1 therefore lands at $2202, which is the address the
         * BIOS's own IRQ path jumps through (`$E86D: jmp ($2202)`); index 2 lands
         * at $2204, the second entry.  A = $06 is a special case: the BIOS stashes
         * the address at $2284/$2285 instead.
         *
         * This is why every interrupt was being discarded: the game installs its
         * handler through this call ($4152-$4161: `lda #$01; ldx #$77; ldy #$41;
         * jsr $E05D`), and without the model nothing was ever written to $2202.
         */
        if (c->a == 0x06) {
            xl_wr(c, 0x2284, c->x);
            xl_wr(c, 0x2285, c->y);
        } else {
            uint16_t slot = (uint16_t)(0x2200u + (unsigned)c->a * 2u);
            xl_wr(c, slot, c->x);
            xl_wr(c, (uint16_t)(slot + 1u), c->y);
            if (c->log)
                xl_logf(c, "bios: install hook %u -> $%02X%02X (stored at $%04X)",
                        (unsigned)c->a, (unsigned)c->y, (unsigned)c->x,
                        (unsigned)slot);
        }
        modelled = 1;
        c->modelled_reads++;
        goto done;
    }
    if (entry == 0xE063) {
        /* BIOS $E49A: read five controllers and keep three tables per pad:
         *   $2228+y current state, $2232+y previous state, $222D+y newly pressed
         * Bits are active high: bit0 I, bit1 II, bit2 SELECT, bit3 RUN, bit4 UP,
         * bit5 RIGHT, bit6 DOWN, bit7 LEFT.  `newly` is the edge, so a held button
         * reports once -- which is exactly what the boot program waits for.
         */
        unsigned y;
        /* The ROM routine ($E49A) resets the multitap counter before it starts:
         * `lda #$01; sta $1000; lda #$03; sta $1000` -- the $03 write raises CLR
         * with SEL high, which is what puts read_index back to controller 0
         * (input.cpp, MultiTapWrite).  Without it the five iterations read slots
         * 1..5 instead of 1..5 of the tap starting at pad 1, i.e. every table is
         * shifted one controller along.  The model issues the same write, so the
         * tables it keeps mean what the ROM's tables mean. */
        xl_wr(c, 0x1000, 0x01);
        xl_wr(c, 0x1000, 0x03);
        for (y = 0; y < 5; y++) {
            unsigned cur, prev, newly;
            xl_wr(c, 0x1000, 0x01);                     /* select directions */
            cur = (unsigned)((xl_rd(c, 0x1000) & 0x0F) << 4);
            xl_wr(c, 0x1000, 0x00);                     /* select buttons */
            cur |= (unsigned)(xl_rd(c, 0x1000) & 0x0F);
            cur = (~cur) & 0xFF;                        /* port is active low */
            prev = xl_rd(c, (uint16_t)(0x2228 + y));
            newly = (cur ^ prev) & cur;
            xl_wr(c, (uint16_t)(0x2232 + y), (uint8_t)prev);
            xl_wr(c, (uint16_t)(0x2228 + y), (uint8_t)cur);
            xl_wr(c, (uint16_t)(0x222D + y), (uint8_t)newly);
        }
        modelled = 1;
        c->modelled_reads++;
        goto done;
    }
    if (entry == 0xE0AE) {
        /* BIOS $E446: select VDC register 0 and set it from A (low) and X (high).
         * That is the VRAM address register -- a program writing tile data uses
         * this to say where the data goes. */
        vdc_write(&c->vdc, 0x0000, 0x00);
        vdc_write(&c->vdc, 0x0002, c->a);
        vdc_write(&c->vdc, 0x0003, c->x);
        /* The BIOS leaves register 2 (the data port) selected, so the caller's
         * next write goes into VRAM.  Getting this wrong sends every subsequent
         * VRAM write into the address register instead. */
        vdc_write(&c->vdc, 0x0000, 0x02);
        modelled = 1;
        c->modelled_reads++;
        goto done;
    }
    if (entry == 0xE09F) {
        /* BIOS $E40B: select VDC register $13 and write the BIOS's shadow pair
         * ($2214/$2215) into it.  Register $13 is the sprite attribute table
         * address, and writing it starts the sprite-table DMA. */
        vdc_write(&c->vdc, 0x0000, 0x13);
        vdc_write(&c->vdc, 0x0002, xl_rd(c, 0x2214));
        vdc_write(&c->vdc, 0x0003, xl_rd(c, 0x2215));
        modelled = 1;
        c->modelled_reads++;
        goto done;
    }
    if (entry == 0xE096) {
        /* BIOS $E3EE: set bits 6-7 of the control shadow ($F3), i.e. enable the
         * background and sprite layers.  Applied to the chip at call time rather
         * than deferred to the next vertical blank (see README). */
        uint8_t f3 = (uint8_t)(xl_rd(c, 0x20F3) | 0xC0);
        xl_wr(c, 0x20F3, f3);
        c->vdc.cr |= 0x00C0;
        modelled = 1;
        c->modelled_reads++;
        goto done;
    }
    if (entry == 0xE099) {
        /* BIOS $E3F3: clear bits 6-7 of the control shadow -- blank the display. */
        uint8_t f3 = (uint8_t)(xl_rd(c, 0x20F3) & (uint8_t)~0xC0);
        xl_wr(c, 0x20F3, f3);
        c->vdc.cr &= (uint16_t)~0x00C0;
        modelled = 1;
        c->modelled_reads++;
        goto done;
    }
    if (entry == 0xE072) {
        /* BIOS $E382: set the VRAM access width in the control shadow from A.
         * `and #$18` after shifting left three puts A's low two bits into CR bits
         * 11-12, which is the memory-increment select. */
        unsigned inc = (unsigned)((c->a << 3) & 0x18);
        c->vdc.cr = (uint16_t)((c->vdc.cr & (uint16_t)~(0x18u << 8)) | (inc << 8));
        modelled = 1;
        c->modelled_reads++;
        goto done;
    }
    if (entry == 0xE0A2) {
        /* BIOS $E5DA: set the VRAM address from the shadow pair ($2214/$2215), then
         * clear 256 words from there. */
        unsigned addr = (unsigned)(xl_rd(c, 0x2214) | (xl_rd(c, 0x2215) << 8));
        unsigned i;
        vdc_write(&c->vdc, 0x0000, 0x00);
        vdc_write(&c->vdc, 0x0002, xl_rd(c, 0x2214));
        vdc_write(&c->vdc, 0x0003, xl_rd(c, 0x2215));
        vdc_write(&c->vdc, 0x0000, 0x02);
        for (i = 0; i < 256; i++) {
            vdc_write(&c->vdc, 0x0002, 0x00);
            vdc_write(&c->vdc, 0x0003, 0x00);
        }
        if (c->log)
            xl_logf(c, "vram clear: 256 words from $%04X", addr);
        modelled = 1;
        c->modelled_reads++;
        goto done;
    }
    if (entry == 0xE0E4) {
        /* BIOS $E509: upload a colour table.  The descriptor lives at $2220:
         *   $2220/$2221 = source address (16-bit)
         *   $2222       = length in 64-byte blocks (each block = 32 palette entries)
         * The BIOS builds a `TIA src,$0404,len` instruction and runs it, so the
         * bytes alternate between the VCE's low-byte and high-byte ports and the
         * colour table address auto-increments once per entry.  $221F bit 0 selects
         * the alternate path, which the boot program leaves at 2 (bit 0 clear). */
        unsigned src = (unsigned)(xl_rd(c, 0x2220) | (xl_rd(c, 0x2221) << 8));
        unsigned blocks = xl_rd(c, 0x2222);
        unsigned pal_bytes = blocks * 64u;
        unsigned i;
        vdc_write_vce(&c->vce, 0x0002, 0x00);        /* colour table address = 0 */
        vdc_write_vce(&c->vce, 0x0003, 0x00);
        for (i = 0; i + 1 < pal_bytes; i += 2) {
            vdc_write_vce(&c->vce, 0x0004, xl_rd(c, (uint16_t)(src + i)));
            vdc_write_vce(&c->vce, 0x0005, (uint8_t)(xl_rd(c, (uint16_t)(src + i + 1)) & 1));
        }
        if (c->log)
            xl_logf(c, "palette upload: %u blocks (%u bytes) from $%04X", blocks,
                    pal_bytes, src);
        modelled = 1;
        c->modelled_reads++;
        goto done;
    }
    if (entry == 0xE07B) {
        /* BIOS $E3B5: wait for the next vertical blank.  On the console this spins
         * on the BIOS frame counter at $2241; here the model advances the machine
         * clock to the next vblank instead of spinning the CPU through it. */
        int f = c->vdc.frame;
        unsigned guard = 0;
        while (c->vdc.frame == f && guard < 400000u) {
            xl_tick(c, 256);
            guard += 256u;
        }
        modelled = 1;
        c->modelled_reads++;
        c->vblank_waits++;
        goto done;
    }
    if (entry == 0xE009) {
        /* CD_READ.  The parameter block, from Hudson's own System Card BIOS manual
         * (four columns, one per destination type) and confirmed against this ROM's
         * dispatcher at $EC05 and its sector-count
         * helper at $ED9F (see README "CD read"):
         *
         *   $F8/$F9  AL/AH  record count (mode bit 0 = 1) or byte count (bit 0 = 0)
         *   $FA/$FB  BL/BH  destination address -- a CPU address; the destination
         *                   BANK is whichever one the MPRs map over it when the
         *                   transfer's `sta [_bl],Y` runs (ROM $EA8B), not a parameter
         *   $FC      CL     record number bits 16-23   (most significant)
         *   $FD      CH     record number bits  8-15
         *   $FE      DL     record number bits  0-7
         *   $FF      DH     destination class in bits 7-1, count unit in bit 0:
         *                     0,1   -> LOCAL (CPU RAM through the MPRs)
         *                     $FE   -> VRAM, $F8/$F9 is a BYTE count
         *                     $FF   -> VRAM, $F8 is a RECORD count
         *                     $02-$06 -> MPR window, bank number in $F8 (BL)
         *
         * The loader's read (mode 1, record $23, 20 sectors, $4000) is the check:
         * it produces the boot program byte for byte.
         */
        unsigned mode = sector_mode;
        unsigned sectors = 0;
        /* The three parameter shapes, from the ROM itself ($EC05 onwards):
         *
         *   mode 0     -> $EC43, `jsr $ED9F`: a BYTE count in $F9/$F8
         *   mode 1     -> $EC31: `lda $F8; sta $2280; asl;asl;asl; sta $F9;
         *                 stz $F8` -- $F8 is a SECTOR count, converted to bytes
         *   mode $FE   -> $ECBB, `jsr $ED9F`: a BYTE count in $F9/$F8
         *   mode $FF   -> $EC79: `lda $F8` is a SECTOR count ($F9 is written by
         *                 the BIOS, never read from the caller)
         *
         * Mode 1 must read $F8 ALONE.  This model used to take the whole
         * $F9:$F8 pair for every mode below 2, which is right for mode 0 and
         * wrong for mode 1: the game's own loader at bank $6A $70EB writes $F8
         * only and never touches $F9, so a stale $F9 from the boot program
         * ($37, left by the $4498 descriptor) turned a 4-sector read into
         * 14,088 sectors -- a call the console never makes and the host could
         * not survive until the bank guard below was added.  Measured on the
         * current boot: `$F8=$04, $F9=$37` at the first game-side read. */
        if (mode == 0) {
            sectors = (count + 2047u) / 2048u;          /* byte count */
        } else if (mode == 1) {
            sectors = count & 0xFF;                    /* $F8 alone, 8-bit */
        } else if (mode == 0xFE) {
            unsigned byte_count = (count & 0xFFFF);     /* $F8 | $F9<<8 */
            sectors = (byte_count + 2047u) / 2048u;
        } else if (mode == 0xFF) {
            sectors = count & 0xFF;
        } else {
            snprintf(err, sizeof err, "transfer mode $%02X not modelled", mode);
        }
        /* The BIOS record number *is* the record number the disc map uses; the
         * payload file simply starts 225 sectors earlier (the pregap).  It is the
         * full 24-bit value $FC/$FD/$FE -- see the parameter breakdown at the top of
         * this function.  Taking only $FE dropped bits 8-23 and asked the drive for
         * a different half of the disc. */
        record = (int)record_num;
        {
            /* Every CD read with its parameters, one line each (XLATE_CDREAD=1).
             * The log above is rate-limited per BIOS entry, which hides a stream
             * of read requests behind "first 3 + every 20000th". */
            static int cdr = -1;
            if (cdr < 0) {
                const char *e = getenv("XLATE_CDREAD");
                cdr = (e && e[0] && e[0] != '0') ? 1 : 0;
            }
            if (cdr && c->log)
                xl_logf(c, "readreq f=%u record=$%02X/%02X/%02X -> $%06X (%u) "
                           "sectors=%u dest=$%04X mode=$%02X -> payload off $%05lX "
                           "target bank at dest $%02X",
                        c->vdc.frame, record_h, record_m, sector, record_num,
                        record_num, sectors, dest_addr, mode,
                        (unsigned long)(((size_t)record_num + 225u) * 2048u),
                        c->mpr[dest_addr >> 13]);
        }
        if (!err[0]) {
            if (!c->disc_attached) {
                snprintf(err, sizeof err, "no disc attached");
            } else {
                size_t off = ((size_t)record + 225u) * 2048u;
                size_t want = (size_t)sectors * 2048u;
                if (off >= c->disc_len) {
                    snprintf(err, sizeof err, "record $%02X past end of payload", record);
                } else {
                    size_t avail = c->disc_len - off;
                    size_t n = want < avail ? want : avail;
                    /* WHERE THE BYTES GO: THE BIOS'S OWN STORE INSTRUCTION SAYS SO.
                     *
                     * The transfer loop in the firmware is at $EA8B-$EAB5:
                     *
                     *   lea8b: lda <_al / ora <_ah / beq done      ; bytes remaining
                     *          cly / ldx #$08
                     *   lea99: lda $1808                           ; data port
                     *          sta [_bl], Y                        ; <- store through $FA/$FB
                     *          ... count down ...
                     *          iny
                     *          bne lea99
                     *          inc <_bh                            ; pointer += 256
                     *          dex / bne lea99
                     *
                     * `sta [_bl],Y` is an ordinary CPU store to address $FA/$FB and
                     * upward.  There is no destination bank parameter and no fixed
                     * CD RAM base in the routine: the bank is whichever one the MPRs
                     * map over that address *at the moment of the store*.  A 12-sector
                     * read from $8000 therefore fills the banks on pages 4, 5 and 6,
                     * whatever the caller left there.
                     *
                     * This is not an inference from two measured destination sets --
                     * it reproduces both.  The game's own read caller (bank $6A
                     * $700B-$701B) pages the target banks in at pages 4/5/6
                     * immediately before each `jsr $E009`, and the console's saved CD
                     * RAM at f1845 and f2400 matches that page sequence bank for bank:
                     * record 201 -> pages $73,$74,$72 and the console's $73/$74/$72
                     * hold records 201/205/209.
                     *
                     * The old model added `$80 + ($FC & $1F)` -- a base the routine
                     * does not have -- so every read landed on bank $80 and overwrote
                     * the previous one.
                     *
                     * WHICH PATH.  The firmware does not have one copy loop; the mode
                     * picks it:
                     *
                     *   mode 0, 1   -> $EC46 calls $EA79 (`lea79_00`): the RAM copy
                     *                  above, through the MPRs.  Handled here.
                     *   mode 2-$FD  -> $ED03, a scheduled variant that also ends in
                     *                  that same $EA79 copy ($ED6F `jsr lea79_00`).
                     *                  Same handling.
                     *   mode $FF    -> $EC79 calls $EABE: two bytes per word to the
                     *                  VDC data port (`video_data_l/h` = $0002/$0003).
                     *   mode $FE    -> $ECBB calls $EB16: one byte per write, the port
                     *                  address toggled between $0002 and $0003.
                     *
                     * The two VRAM modes are NOT modelled, and that is deliberate:
                     *
                     *   - The only mode-$FE caller in our boot is $408D, reading 24
                     *     sectors of record 3.  The console never issues that read:
                     *     its command trace has no such command, and its saved VRAM
                     *     at f1800 and f1845 contains none of record 3's blocks.  So
                     *     that call is evidence of an upstream divergence in our boot
                     *     chain, not a behaviour to reproduce.
                     *   - Modelling it faithfully (measured) blanks the frame: our
                     *     chain reaches the call with the VDC write address at 0, so
                     *     48 KB of record 3 lands on the background map -- 57,344 of
                     *     57,344 pixels differing, one colour.
                     *
                     * So they keep the old (wrong) CD RAM placement, logged once, and
                     * are named as the next defect.  Changing them in the same round
                     * as the destination fix would also hide which change did what. */
                    if (mode == 0xFE || mode == 0xFF) {
                        static int warned = 0;
                        if (c->log && !warned++) {
                            xl_logf(c, "cd: mode $%02X is a VRAM upload in the ROM "
                                       "($EB16/$EABE); this port still places it in "
                                       "CD RAM -- NOT modelled, see runtime.c",
                                    mode);
                        }
                        /* The pre-existing behaviour, kept byte for byte so that this
                         * round changes one thing: bank $80 from the low 13 bits of the
                         * caller's address.  Wrong, and labelled as such.
                         *
                         * MEASURED 2026-10-28, AND DELIBERATELY HELD BACK.  Two things
                         * this comment used to say are not true:
                         *
                         *  - "the console's own boot never makes this call" -- it does.
                         *    The boot program at $406D-$4091 (dis/bootprog_4000.asm) sets
                         *    $FC/$FD/$FE = 0:0:3, $FF = $FE, $FA/$FB = $00/$10 and
                         *    $F8/$F9 = $00/$C0, then `jsr $E009` in a retry loop: record
                         *    3, VRAM word address $1000, 49,152 bytes -- exactly Hudson's
                         *    VRAM-destination form.  The game's own reads all use modes
                         *    0/1 (to CPU RAM), so $FE is the boot program's path only.
                         *  - "modelling it faithfully blanks the frame" -- that was
                         *    measured with the VDC write address at 0.  With MAWR := BH:BL
                         *    the 48 KB lands at $1000-$6FFF and the frame is fine.
                         *
                         * What happens when it IS modelled (VRAM upload through the data
                         * port, byte-by-byte, MAWR set by the model):
                         *  - VRAM $1000-$6FFF becomes byte-for-byte record 3 at the frame
                         *    after the call (0 of 24,576 words differ);
                         *  - our VRAM at the booted logo moves from 5 words away from the
                         *    console's own saved VRAM to 3;
                         *  - the early boot screen stops being tile noise and shows the
                         *    boot program's own STAGE X animation;
                         *  - but the booted logo's 48-pixel residual MOVES (rows 49/97 ->
                         *    51/99), two test stages that assert this CD-RAM placement go
                         *    red, and the version of the boot program WE run then stops
                         *    matching the console's cadence -- because the console's boot
                         *    program is reached from different records than ours (see
                         *    STATUS item 3: record 35 vs 67/77/89).  The CD-RAM
                         *    misplacement has been standing in for that divergence: the
                         *    corruption is what put the right bytes where our boot
                         *    program looks for them.
                         *
                         * So it cannot land before the boot chain is right.  The next
                         * round should fix the loader's record table first, then turn this
                         * on and expect the 48-pixel gate to keep its rows. */
                        {
                            size_t boff = dest_addr & 0x1FFF;
                            if (boff + n > XL_BANK_SIZE * 32u) {
                                snprintf(err, sizeof err,
                                         "mode $%02X transfer does not fit CD RAM", mode);
                                goto done;
                            }
                            xl_cd_move(c, off, n, (uint16_t)dest_addr, XL_CD_TO_CDRAM);
                        }
                    } else {
                        /* A 16-bit pointer cannot cross the top of the address space;
                         * the ROM's `inc <_bh` would wrap it into page 0 (the I/O
                         * page).  A block that would wrap is a malformed block, so
                         * refuse it as a failed call (A=$FF, carry set) instead of
                         * letting it run into the hardware registers.  Same SAFETY
                         * rule as before: a bad parameter block is a failed BIOS
                         * call, never a host crash. */
                        if (n > 0x10000u || (size_t)dest_addr + n > 0x10000u) {
                            snprintf(err, sizeof err,
                                     "read of %lu bytes to $%04X wraps past $FFFF",
                                     (unsigned long)n, dest_addr);
                            goto done;
                        }
                        /* Tell the stale-bank barrier that the stores about to happen
                         * are a disc transfer, so it can name the record instead of
                         * blaming this stub's own address ($E009). */
                        xl_cd_ctx.on   = 1;
                        xl_cd_ctx.rec  = (unsigned)record;
                        xl_cd_ctx.sec  = sectors;
                        xl_cd_ctx.dest = dest_addr;
                        xl_cd_move(c, off, n, (uint16_t)dest_addr, XL_CD_TO_RAM);
                        xl_cd_ctx.on   = 0;
                    }
                    bytes = (unsigned long)n;
                    modelled = 1;
                    c->modelled_reads++;
                    c->read_bytes += n;
                    /* The same transfer also feeds the raw data port, so a program
                     * that streams sectors byte-by-byte through $180A (the kernel's
                     * own driver does: bank $69 $5C68-$5CBC) sees real bytes. */
                    xl_cd_prime(c, (unsigned)record, sectors);
                    /* THE READ TAKES ITS TIME, AND THE TIME IS SPENT ONE SECTOR PER
                     * FRAME (see xl_cd_move).  The frame count is unchanged from the
                     * old `sectors + 2` tick burst -- only the moment each sector's
                     * bytes appear inside that window moved, so no timeline moves. */
                    {
                        unsigned frames = (sectors > 255u ? 255u : sectors) + 2u;
                        if (xl_cd_paced() && c->log)
                            xl_logf(c, "cd read took %u frames (%u sectors + 2 overhead)",
                                    frames, sectors);
                    }
                    /* The console does not return from a CD read immediately: the BIOS
                     * polls the drive for the sector before it hands back.  Measured
                     * from the trace: ~89 frames of BIOS polling between the disc code
                     * starting and the upload burst.  Our stub is synchronous, so all
                     * work after a read happens that much too early unless the elapsed
                     * time is modelled.  Opt-in, because it changes every timeline. */
                    {
                        static int delay = -2;
                        if (delay == -2) {
                            const char *e = getenv("XLATE_CD_DELAY");
                            delay = e ? atoi(e) : 0;
                        }
                        if (delay > 0) {
                            unsigned k;
                            for (k = 0; k < (unsigned)delay; k++)
                                xl_tick(c, 120000u);
                        }
                    }
                }
            }
        }
    }

done:
    if (!modelled) {
        /* An unmodelled BIOS entry must not report success.  Ten entries the game
         * calls were returning A=0, i.e. "done, nothing to do", which is a lie that
         * hides itself: the caller takes its success path and the fault shows up
         * thousands of frames later as "nothing happened".  Return the failure the
         * entry's own contract would produce instead. */
        c->unmodelled_calls++;
        c->a = 0xFF;
        c->x = 0xFF;
        c->p = (uint8_t)((c->p | XL_C) & (uint8_t)~XL_N);
        if (c->log)
            xl_logf(c, "bios: UNMODELLED $%04X returning A=$FF carry set "
                       "(was: fake success)", entry);
    }

    if (c->log && xl_should_log(entry)) {
        char rec[8];
        if (record < 0)
            snprintf(rec, sizeof rec, "-");
        else
            snprintf(rec, sizeof rec, "$%02X", (unsigned)record);
        /* The caller matters as much as the parameters: it is the only way to tell
         * which call site wanted what back, and the return address is on the stack. */
        {
            uint8_t lo = xl_rd(c, (uint16_t)(XL_STACK_BASE | ((c->s + 1) & 0xFF)));
            uint8_t hi = xl_rd(c, (uint16_t)(XL_STACK_BASE | ((c->s + 2) & 0xFF)));
            uint16_t ret = (uint16_t)((lo | (hi << 8)) + 1);
            xl_logf(c, "CD BIOS call at $%04X from $%04X (bank $%02X) params count=%u "
                       "dest=$%04X record=$%02X/%02X/%02X mode=$%02X record=%s %s",
                    entry, ret, c->mpr[ret >> 13], count, dest_addr,
                    record_h, record_m, sector, sector_mode, rec,
                    modelled ? "MODELLED" : "unmodelled (returns A=0,X=0,C=0)");
        }
        if (modelled) {
            if (stored) {
                xl_logf(c, "  -> %s", stored);
            } else {
            /* The destination is an address, not a bank: report the banks the MPRs
             * actually map over it, which is what the transfer touched. */
            unsigned nb = (unsigned)((bytes + 0x1FFFu) / 0x2000u), k;
            char dst[96];
            size_t w = 0;
            dst[0] = '\0';
            for (k = 0; k < nb && k < 12u; k++) {
                unsigned a = (unsigned)((dest_addr & 0xE000u) + k * 0x2000u);
                if (a > 0xFFFFu)
                    break;
                w += (size_t)snprintf(dst + w, sizeof dst - w, "%s$%02X",
                                      w ? "," : "", c->mpr[(a & 0xFFFFu) >> 13]);
            }
            xl_logf(c, "  -> copied %lu bytes from record $%02X to $%04X, banks %s",
                    bytes, (unsigned)record, dest_addr, dst);
            }
        }
        else if (err[0])
            xl_logf(c, "  -> not modelled: %s", err);
    }

    /* Machine-readable trace consumed by the tests (one line per call).  The
     * `bank=` field is a leftover from the misreading that caused the record bug:
     * it carries the record's two upper bytes ($FC/$FD), not a bank, and is kept
     * only because tests/harness.py parses this exact shape.  Only the CD read
     * path uses it; for every other entry those bytes are unread parameters. */
    printf("CALL pc=%04X count=%u dest=%04X bank=%u sector=%02X mode=%02X "
           "record=%d modelled=%d bytes=%lu\n",
           entry, count, dest_addr, (unsigned)(record_num >> 8), sector, sector_mode,
           record, modelled, bytes);
    fflush(stdout);

    c->a = 0;
    c->x = 0;
    c->p &= (uint8_t)~XL_C;
    c->pc = (uint16_t)(xl_pop16(c) + 1);
}

/* Which banks actually execute, and when they first do.  That is the evidence for
 * "which module ran", which is otherwise invisible in a long run. */
static uint8_t xl_bank_seen[256];
static uint32_t xl_bank_frame[256];

void xl_note_pc(Cpu *c)
{
    uint8_t b = c->mpr[c->pc >> 13];
    /* Stale-bank instrument: arm on the first executed instruction (the image is
     * final by now) and compare a dirty bank the moment the dispatcher selects it. */
    xl_stale_arm(c);
    xl_stale_enter(c, b);
    if (xl_bank_seen[b])
        return;
    xl_bank_seen[b] = 1;
    xl_bank_frame[b] = (uint32_t)c->vdc.frame;
    if (c->log)
        xl_logf(c, "bank $%02X first executes at frame %u, pc $%04X", b,
                xl_bank_frame[b], c->pc);
    printf("BANK first=%02X frame=%u pc=%04X\n", b, xl_bank_frame[b], c->pc);
    fflush(stdout);
}

void xl_banks_summary(FILE *out)
{
    int b;
    fprintf(out, "BANKS_EXECUTED");
    for (b = 0; b < 256; b++)
        if (xl_bank_seen[b])
            fprintf(out, " %02X@%u", b, xl_bank_frame[b]);
    fprintf(out, "\n");
    fflush(out);
}

/* Banks that were reached but have no translated code.  Logged once per bank (plus
 * every thousandth hit), counted per bank, and summarised at exit. */
static uint32_t xl_unt_hits[256];

/* Optional ring of the last PCs executed, so a stop has a *route* and not just a
 * destination.  Off by default (one branch per instruction); XLATE_TRACE=1 turns it
 * on and the ring is printed when execution stops on an untranslated address. */
#define XL_TRACE_N 128
static uint16_t xl_trace_pc[XL_TRACE_N];
static uint8_t  xl_trace_bank[XL_TRACE_N];
static unsigned xl_trace_pos;
static int xl_trace_on = -1;

static void xl_trace_dump(Cpu *c)
{
    unsigned n, k;
    if (!c->log)
        return;
    n = xl_trace_pos < XL_TRACE_N ? xl_trace_pos : XL_TRACE_N;
    xl_logf(c, "trace: last %u instruction addresses (oldest first)", n);
    for (k = 0; k < n; k++) {
        unsigned i = (xl_trace_pos - n + k) % XL_TRACE_N;
        xl_logf(c, "  trace: $%04X  bank $%02X", xl_trace_pc[i], xl_trace_bank[i]);
    }
}

/* BRK IS AN INSTRUCTION, NOT AN UNDEFINED BYTE.
 *
 * The reference core (huc6280_ops.inc, `case 0x00`) does what the hardware does:
 * skip the byte after the opcode, push PC, push P with the B flag set, set I,
 * clear D and T, and take the vector from **$FFF6** -- the HuC6280 routes BRK
 * through the IRQ2 slot, not through $FFF8, which is the one place it departs
 * from the 6502's layout.  Seven cycles.
 *
 * This exists because the alternative was a crash.  BRK and every undefined byte
 * are untranslatable, so execution always arrives at them through
 * xl_untranslated() -> xl_interp_step() -> no_instruction(); with BRK treated as
 * "undefined" that chain called itself at the same pc until the stack ran out
 * (measured: SIGSEGV at pc $1CCA in syscard3.pce, frame 35,721, inside the
 * second console reset).  A machine model must not crash: an instruction it can
 * execute it executes, and one it cannot is reported. */
int xl_brk(Cpu *c)
{
    uint16_t ret = (uint16_t)(c->pc + 1);
    uint16_t target;
    c->p &= (uint8_t)~XL_T;
    xl_push(c, (uint8_t)(ret >> 8));
    xl_push(c, (uint8_t)(ret & 0xFF));
    xl_push(c, (uint8_t)(c->p | XL_BF));
    c->p |= XL_I;
    c->p &= (uint8_t)~XL_D;
    target = xl_rd16(c, 0xFFF6);
    if (c->log)
        xl_logf(c, "brk: BRK at frame %u pc=$%04X bank $%02X -- pushed $%04X, "
                   "vector $FFF6 -> $%04X",
                c->vdc.frame, c->pc, c->mpr[c->pc >> 13], ret, target);
    c->pc = target;
    xl_tick(c, 7);
    return 1;
}

int xl_untranslated(Cpu *c, uint8_t bank, uint16_t pc)
{
    /* UNDER THE RESET-VECTOR ENTRY, "NO TRANSLATION" MEANS "INTERPRET IT".
     *
     * The card loads its own IPL off the disc and builds copy routines in work RAM;
     * none of that code exists at build time, so there is nothing to translate and
     * no compiled image for the guard to compare against (runtime.c's guard note,
     * condition 1).  The bytes in memory are real program bytes, so the honest
     * thing is to run them -- the same rule the guard applies to an unknown *bank*,
     * applied to an unknown *address*.  This is deliberately limited to the
     * reset-entry route: on the default path an unknown address still stops the run,
     * because there it means the boot chain went somewhere it should not have. */
    if (c->bios_reset) {
        int was_rom = xl_pc_in_rom(c);
        int ok;
        /* THE FALLBACK IS A RECURSION, SO IT NEEDS A DEPTH.
         *
         * "No translation means interpret it" is a rule about *code we could not
         * translate*, and it is answered by xl_interp_step().  If the byte there is
         * one the interpreter also cannot execute -- an undefined opcode -- then
         * no_instruction() calls straight back here, at the same pc, and the chain
         * only ends when the C stack does.  Measured: SIGSEGV with `error 6` and sp
         * at the faulting address, hundreds of identical xl_untranslated frames,
         * pc $1CCA (a $00 byte) in syscard3.pce at frame 35,721.
         *
         * A machine model reports an execution point it cannot run; it does not die
         * on it.  Eight is far more nesting than any real instruction needs (one
         * level per call, and the interpreted instruction either advances pc or
         * fails), so reaching it means the same pc refused twice, which is a stop,
         * not a wait. */
        static unsigned depth;
        if (depth >= 8u) {
            if (c->log)
                xl_logf(c, "RECURSION at frame %u: untranslated pc=$%04X bank $%02X "
                           "(mpr=%02X %02X %02X %02X %02X %02X %02X %02X) -- the byte "
                           "there is not an instruction this machine can execute; "
                           "stopping",
                        (unsigned)c->vdc.frame, c->pc, c->mpr[c->pc >> 13],
                        c->mpr[0], c->mpr[1], c->mpr[2], c->mpr[3],
                        c->mpr[4], c->mpr[5], c->mpr[6], c->mpr[7]);
            printf("RECURSION frame=%u pc=%04X bank=%02X\n", (unsigned)c->vdc.frame,
                   c->pc, c->mpr[c->pc >> 13]);
            fflush(stdout);
            c->untranslated_stop = 1;
            return 0;
        }
        xl_unt_interp[bank]++;
        depth++;
        ok = xl_interp_step(c);
        depth--;
        if (ok) {
            c->interp_steps++;
            c->interp_untranslated_steps++;
            if (was_rom)
                c->bios_interp_steps++;
        }
        return ok;
    }
    xl_unt_hits[bank]++;
    if (c->log && (xl_unt_hits[bank] == 1 || (xl_unt_hits[bank] % 1000) == 0))
        xl_logf(c, "untranslated: bank $%02X has no code for $%04X "
                   "(mpr=%02X %02X %02X %02X %02X %02X %02X %02X) -- stopping",
                bank, pc, c->mpr[0], c->mpr[1], c->mpr[2], c->mpr[3],
                c->mpr[4], c->mpr[5], c->mpr[6], c->mpr[7]);
    if (xl_trace_on)
        xl_trace_dump(c);
    printf("UNTRANSLATED bank=%02X pc=%04X\n", bank, pc);
    fflush(stdout);
    c->untranslated_stop = 1;
    return 0;
}

void xl_untranslated_summary(FILE *out)
{
    int b, any = 0;
    fprintf(out, "UNTRANSLATED_BANKS");
    for (b = 0; b < 256; b++) {
        if (xl_unt_hits[b]) {
            fprintf(out, " %02X", b);
            any = 1;
        }
    }
    if (!any)
        fprintf(out, " none");
    fprintf(out, "\n");
    for (b = 0; b < 256; b++) {
        if (xl_unt_hits[b])
            fprintf(out, "UNTRANSLATED_BANK bank=%02X hits=%u\n", b,
                    (unsigned)xl_unt_hits[b]);
    }
    fflush(out);
}

int xl_trap(Cpu *c, uint16_t pc)
{
    if (c->log)
        xl_logf(c, "TRAP: no translated code at $%04X (mpr=%02X %02X %02X %02X "
                   "%02X %02X %02X %02X)", pc,
                c->mpr[0], c->mpr[1], c->mpr[2], c->mpr[3],
                c->mpr[4], c->mpr[5], c->mpr[6], c->mpr[7]);
    printf("TRAP pc=%04X\n", pc);
    fflush(stdout);
    return 0;
}

/* ------------------------------------------------- timing and interrupts -- */

/* Report-only whole-RAM snapshot hook, defined with the other ramdump code further
 * down.  Declared here because xl_tick() is the only place that sees EVERY frame
 * boundary: a CD read advances frames inside its own xl_tick() burst, so a hook in
 * the run loop would skip the frames a multi-frame wait consumes. */
static void xl_ramdump_maybe(Cpu *c, uint32_t frame);
static void xl_ramstream_maybe(Cpu *c, uint32_t frame);
static void xl_wrseries_maybe(Cpu *c, uint32_t frame);
/* The port's side of the state-hash envelope (portstate.c, job 6). */
void xl_portstate_maybe(Cpu *c);
static void xl_framepc_maybe(Cpu *c, uint32_t frame);
/* The reset schedule (host --resets/--resetfile).  Fired here for the same reason
 * the instruments above live here: this is the only place that sees every frame
 * boundary, and the reset has to land before the new frame's first access. */
static void xl_reset_tick(Cpu *c);

/* The HuC6280 timer counts the CPU clock / 1024 and raises its own vector when it
 * wraps.  `cycles` is the cost of the instruction that just ran, so a program that
 * polls or spins sees a plausible amount of time pass. */
void xl_tick(Cpu *c, unsigned cycles)
{
    /* ONE CHARGED CYCLE IS NOT ALWAYS THREE MASTER CLOCKS.
     *
     * Every cost in this machine -- `cycles.py`, the generated `xl_tick(c, N)`
     * literals, the interpreter's table, the VDC's port steal -- is in the unit the
     * reference core calls a CPU cycle, the `ADDCYC(n)` argument.  The conversion
     * from that unit to master clocks is the ONE place the speed mode acts, and it
     * is the reference's own expression:
     *
     *     int master = (x * 3) << speed_shift_cache;      // huc6280.h:227
     *     speed_shift_cache = (speed ^ 1) << 1;           // huc6280.h:209
     *
     * normal (speed = 1): master = x * 3    -- 7.15909 MHz = 21.47727 / 3
     * slow   (speed = 0): master = x * 12   -- 1.78977  MHz = 21.47727 / 12
     *
     * so the ratio is exactly four, and *nothing else in the machine changes*: an
     * instruction still costs the same number of charged cycles, but each of them
     * buys four times as much real time.  That is why the tables and the published
     * per-opcode cycle counts are untouched by this and stay in ADDCYC units.
     *
     * Because `master` is a multiple of three, dividing it back by three gives the
     * normal-speed CPU-cycle count, and `cycles << speed_shift` is that number.  So
     * the whole conversion is one shift, and `c->cycles` stays what every consumer
     * already assumes it is (trace.c: `tr_now = c->cycles * 3`, the CD model's
     * `XL_CD_PERIOD_MASTER / 3`, the timer's own /1024): a master-clock timeline
     * divided by three, i.e. "how many normal-speed CPU cycles of real time have
     * passed".  In slow mode four such units pass per charged cycle.
     *
     * The timer is on the same clock (the reference subtracts master clocks from
     * `timer_div` and reloads with `(load ? load : 1024) * 3`), and vdc_tick() steps
     * one normal-speed cycle of chip time per unit, which is `VD_WS_MASTER_PER_CYCLE`
     * = 3 master clocks -- so both take the scaled number. */
    unsigned scaled = cycles << c->speed_shift;

    c->cycles_charged += cycles;
    c->cycles += scaled;

    c->timer.div += scaled;
    while (c->timer.div >= 1024) {
        c->timer.div -= 1024;
        if (c->timer.status & 1) {
            if (c->timer.value == 0) {
                c->timer.value = c->timer.load;
                c->irq_status |= XL_IQ_TIMER;    /* timer forwarded to $1403 */
            } else {
                c->timer.value--;
            }
        }
    }

    {
        int before = c->vdc.frame;
        int was_vblank = c->vdc.vblank;
        vdc_tick(&c->vdc, &c->vce, scaled);
        /* The BIOS IRQ1 (vblank) path, as its own ROM does it.
         *
         * WHERE: at the START of vertical blank, not at the end of the frame.
         * The console's IRQ1 handler runs when the raster enters line 240 (the
         * vblank line), and this block is that handler's effects.  Applying them
         * at the frame wrap instead put them at the END of vblank, i.e. after the
         * game's own vblank work had already run.
         *
         * WHETHER: only while the BIOS still owns the video vector.  ROM $E870 is
         * the IRQ1 entry and its first instruction is `BBS1 $F5, +$FA`: when bit 1
         * of $F5 (the BIOS's vector-enable mask, `irq_m`) is SET, the whole
         * housekeeping block $E879-$E8DF is skipped.  The game sets $F5 = $06 when
         * it installs its own handler, so the BIOS stops touching the scroll
         * registers from that frame on.
         *
         * That is measured, not assumed.  In the console's access trace the
         * BIOS writes $E892/$E899 (BXR from $220C) and $E89B/$E8A2 (BYR from
         * $2210) on frames 1-556 and never again, and every BIOS IRQ1 port access
         * ($E879, $E880, $E885, $E88A, $E892, $E899, $E89B, $E8A2, $E8DF) has the
         * same last frame, 556 -- while IRQ1 itself keeps firing 1/frame to 2899.
         * The scroll registers then belong to the game: from 557 on only game pcs
         * ($4304/$4315, and the whole per-strip set from the stage code) write
         * them.
         *
         * Our port reaches the same point by the same route: its work RAM holds
         * $F5 = $00 through frame 1700 and $F5 = $06 from the frame the game runs
         * its install sequence.  The absolute frame differs from the trace's
         * because the two runs boot by different paths; the condition does not.
         *
         * Continuing to apply the copy for ever -- as this model did -- overwrote
         * BXR/BYR with the stale zeros at $220C/$2210 at every frame end, which is
         * exactly the "our port reports both at zero, never non-zero" symptom.
         */
        if (!c->bios_exec && !was_vblank && c->vdc.vblank && (xl_rd(c, 0x20F5) & 0x02) == 0) {
            /*   $E87E-$E887  CR   <- $F3 | $F4<<8   (the control shadow)
             *   $E88A        inc $2241              (frame counter)
             *   $E890-$E89B  BXR  <- $220C/$220D, BYR <- $2210/$2211
             *   $E8A5        inc $2249              (a second frame counter)
             *   $E8B1        pad read
             * Implemented here because we do not execute ROM: these are the effects
             * the game's own code relies on every frame.
             *
             * RETIRED UNDER XLATE_BIOS_EXEC.  That block is not the chip, it is the
             * body of ROM $E879-$E8DF, and when the System Card is executed the CPU
             * takes IRQ1 through the card's own vector ($FFF8 -> $E870) and that code
             * runs for real, from the user's file.  Doing both would apply the
             * housekeeping twice per frame and would overwrite BXR/BYR with the
             * shadow once more after the game had set them. */
            uint16_t cr = (uint16_t)(xl_rd(c, 0x20F3) | (xl_rd(c, 0x20F4) << 8));
            uint16_t bxr = (uint16_t)(xl_rd(c, 0x220C) | (xl_rd(c, 0x220D) << 8));
            uint16_t byr = (uint16_t)(xl_rd(c, 0x2210) | (xl_rd(c, 0x2211) << 8));
            uint16_t a = 0x2241;
            xl_wr(c, a, (uint8_t)(xl_rd(c, a) + 1));
            a = 0x2249;
            xl_wr(c, a, (uint8_t)(xl_rd(c, a) + 1));
            {
                static int vf = -2, vt = -1;
                if (vf == -2) {
                    const char *env_from = getenv("XLATE_VBL_FROM");
                    const char *env_to = getenv("XLATE_VBL_TO");
                    vf = env_from ? atoi(env_from) : -1;
                    vt = env_to ? atoi(env_to) : -1;
                }
                if (vf >= 0 && c->vdc.frame >= vf && (vt < 0 || c->vdc.frame <= vt) &&
                    c->log)
                    xl_logf(c, "vblank %d: status=$%04X dcr=$%04X sat_pending=%d "
                               "3A=$%02X cr=$%04X",
                            c->vdc.frame, c->vdc.status, c->vdc.dcr,
                            c->vdc.sat_pending, xl_rd_phys(c, XL_WORKRAM_BANK, 0x3A), c->vdc.cr);
            }
            /* CR is NOT restored from RAM here.
             *
             * This model used to do `if (cr) vdc.cr = cr & 0x3FFF` on every
             * vblank, taking $00F3/$00F4 as a shadow of the control register.
             * The console never behaves that way: in the trace, every CR write
             * in the game window comes from game code once per frame --
             * pc $41FD writes $C8, pc $8005 writes the high byte -- and in
             * frames 2390-2410 there are 42 CR writes and none of them from a
             * BIOS pc ($E000+).  The same holds for BXR ($4304/$430A) and BYR
             * ($4315/$431B), all writing $0000.
             *
             * $00F3 does hold $0088 in the console's own states, but that is a
             * leftover of the boot, not a shadow the BIOS reapplies.  Restoring
             * from it forced CR back to $0088 every frame and cleared the
             * sprite-enable bit (bit 6), so the sprite layer was never on: that
             * is the missing 94x12 text band at frame 3000, and it is the same
             * reason sprite content has never matched in gameplay captures.
             */
            (void)cr;
            vdc_write(&c->vdc, 0x0000, 0x07);         /* BXR */
            vdc_write(&c->vdc, 0x0002, (uint8_t)(bxr & 0xFF));
            vdc_write(&c->vdc, 0x0003, (uint8_t)(bxr >> 8));
            vdc_write(&c->vdc, 0x0000, 0x08);         /* BYR */
            vdc_write(&c->vdc, 0x0002, (uint8_t)(byr & 0xFF));
            vdc_write(&c->vdc, 0x0003, (uint8_t)(byr >> 8));
            vdc_write(&c->vdc, 0x0000, 0x02);         /* leave the data port up */
        }
        /* A checkpoint that lands at a chosen raster line (XLATE_PORTSTATE_LINE) is
         * taken here, at the instruction boundary right after the clock advance, so
         * it samples the same instant in the frame the oracle does. */
        if (xl_portstate_on())
            xl_portstate_raster(c);
        if (c->vdc.frame != before) {
            /* Report-only snapshot at every frame boundary, whichever code path
             * advanced the frame (main loop or a CD read's own tick burst). */
            xl_trace_frame(c);          /* the trace's FRAME marker, before any access */
            xl_reset_tick(c);           /* scheduled console resets: top of the frame */
            xl_ramdump_maybe(c, (uint32_t)c->vdc.frame);
            xl_ramstream_maybe(c, (uint32_t)c->vdc.frame);
            xl_wrseries_maybe(c, (uint32_t)c->vdc.frame);
            xl_framepc_maybe(c, (uint32_t)c->vdc.frame);
        }
    }
    if (c->vdc.irq_line)
        c->irq_status |= XL_IQ_IRQ1;             /* VDC line asserted: $1403 bit 1 */
    /* The CD unit's line is IRQ2, $1403 bit 0 (research §5.4: Mednafen wires the
     * VDC's hook to IQIRQ1 and the CD's to IQIRQ2).  It is a LEVEL, not a latch:
     * while the drive has an enabled source active the line stays asserted, and when
     * the source goes away it drops.  Latching it (which this did, "the model never
     * raises one") means an executed System Card that takes IRQ2 re-enters its
     * handler for ever after the first request, because nothing on the card clears a
     * CPU-side flag that the drive no longer asserts. */
    if (c->cd.irq_status & c->cd.irq_mask)
        c->irq_status |= XL_IQ_IRQ2;
    else
        c->irq_status &= (uint8_t)~XL_IQ_IRQ2;
}

/* The name of an interrupt source, for log lines.  Named for the source, not the
 * vector bit (see XL_HOOK_* in runtime.h for the slot each one dispatches through). */
static const char *xl_irq_name(int kind)
{
    switch (kind) {
    case XL_IRQ_CD:    return "cd-irq2";
    case XL_IRQ_VDC:   return "vdc-irq1";
    case XL_IRQ_TIMER: return "timer";
    }
    return "?";
}

/* Acknowledge a source the way a real handler's first read of the status register
 * does, so a discarded interrupt is not raised again on the next step.  The CD arm
 * clears only the CPU's pending bit: the drive's own status belongs to the drive
 * ($1801), and this port's CD model never raises the line (see xl_tick). */
static void xl_irq_ack(Cpu *c, int kind)
{
    switch (kind) {
    case XL_IRQ_TIMER:
        c->irq_status &= (uint8_t)~XL_IQ_TIMER;
        break;
    case XL_IRQ_VDC:
        c->irq_status &= (uint8_t)~XL_IQ_IRQ1;
        c->vdc.irq_line = 0;
        c->vdc.status &= (uint16_t)~0x3F;
        break;
    default:
        c->irq_status &= (uint8_t)~XL_IQ_IRQ2;
        break;
    }
}

/* Interrupt entry, as the CPU does it: push PC, push P with B clear, set I, clear
 * T and D, then run the handler.
 *
 * Here is the one place this port deviates from the console, and it is deliberate:
 * on real hardware the CPU vectors into the CD BIOS, whose handler does its own
 * housekeeping and then calls the game's hook.  We do not execute BIOS code, so
 * the model performs the *dispatch half* of that job itself: the six hook slots at
 * $2200-$220A hold a 16-bit handler address each (XL_HOOK_CD / _VDC / _TIMER / _NMI
 * / _SYNC / _RCR in runtime.h), and an interrupt enters the address in its own
 * slot.  If the slot is empty the interrupt is logged and discarded, which is what
 * a BIOS with a null hook does.
 *
 * A slot is TWO bytes.  There is no bank byte: the handler runs in whatever bank
 * the current mapping puts at its address, exactly as `jmp (slot)` would.  If that
 * bank has no translated code at that address, the step after entry says
 * `untranslated` and names the bank -- which is the honest report, and the reason
 * no bank comparison belongs here.
 *
 * This is the contract the README calls the "interrupt contract"; the game's real
 * hook installation is not mapped yet, which is why the game's own interrupts are
 * still a known gap rather than a working feature.
 */
void xl_service_irq(Cpu *c)
{
    uint16_t hook_addr = 0;
    int kind = -1;

    {
        static int why = -1;
        if (why < 0) {
            const char *e = getenv("XLATE_IRQ_WHY");
            why = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (why && c->log) {
            static uint32_t n;
            if (n < 6 || (n % 100) == 0)
                xl_logf(c, "irqwhy f=%u p=$%02X I=%d in_irq=%d status=$%02X mask=$%02X "
                           "vdc.stat=$%04X cr=$%04X line=%d pending=%d",
                        c->vdc.frame, c->p, (c->p & XL_I) ? 1 : 0, c->in_irq,
                        c->irq_status, c->irq_mask, c->vdc.status, c->vdc.cr,
                        c->vdc.irq_line,
                        (c->irq_status & XL_IQ_IRQ1) ? 1 : 0);
            n++;
        }
    }
    if (c->p & XL_I)                             /* interrupts masked */
        return;
    /* Handlers may nest: the game's vblank handler does `cli` at $4197 and later
     * waits for a flag the *next* vblank sets, so on hardware a second IRQ2 arrives
     * while the first handler is still running.  The CPU has no "already in an
     * interrupt" state -- the I flag is the only gate -- so the same is true here.
     * A depth cap only guards against a handler that never acknowledges the source. */
    if (c->in_irq >= 8)
        return;

    /* Priority when several are pending: reset > NMI > BRK > timer > IRQ1 >
     * IRQ2 (Hudson, hardware manual p.H8-12; research §5.4).  Each source is
     * gated by its own bit of $1402, where 1 = disabled.  A masked source is
     * still *reported* in $1403 -- that is the difference between "the game polls
     * the VDC" and "the game takes the interrupt" -- it just does not reach the
     * CPU. */
    if ((c->irq_status & XL_IQ_TIMER) && !(c->irq_mask & XL_IQ_TIMER)) {
        kind = XL_IRQ_TIMER;
        hook_addr = XL_HOOK_TIMER;               /* $2204, not $2206: $2206 is NMI */
    } else if ((c->irq_status & XL_IQ_IRQ1) && !(c->irq_mask & XL_IQ_IRQ1)) {
        kind = XL_IRQ_VDC;
        /* The video slot is $2202.  BIOS $E86D is `jmp ($2202)`; the game stores its
         * handler there directly (the oracle's work RAM holds $4177 at $2202/$2203).
         *
         * There is deliberately NO gate on $F5 here.  One was tried and removed:
         * the ROM's IRQ1 vector ($FFF8) enters at $E870, whose first byte pair
         * `9F F5 FA` looks like `bbs1 $F5` but branches into the CD poll loop at
         * $E96D, not to a hook check -- reading it as "the hook is called only
         * when $F5 bit 1 is set" was a misdecode.  Before the game installs its
         * own handler the slots hold the BIOS default $E8E2 (a bare `rti`), so an
         * early vblank is harmless without any extra condition. */
        hook_addr = XL_HOOK_VDC;
    } else if ((c->irq_status & XL_IQ_IRQ2) && !(c->irq_mask & XL_IQ_IRQ2)) {
        /* The CD unit's line, lowest priority of the three.  This port's CD model
         * never raises a pending flag yet (see xl_tick), so this arm is unreachable
         * in every run measured so far -- it is here so the slot table is complete
         * and the next reader does not look for the CD hook somewhere else. */
        kind = XL_IRQ_CD;
        hook_addr = XL_HOOK_CD;
    }
    if (kind < 0)
        return;
    c->irq_kind[kind]++;

    {
        static int irq_trace = -1;
        if (irq_trace < 0) {
            const char *e = getenv("XLATE_IRQ_TRACE");
            irq_trace = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (irq_trace && c->log)
            xl_logf(c, "irq %lu: %s at frame %u, pc=$%04X, vdc status=$%04X cr=$%04X "
                       "3A=$%02X",
                    (unsigned long)c->irq_taken, xl_irq_name(kind),
                    c->vdc.frame, c->pc, c->vdc.status, c->vdc.cr,
                    xl_rd_phys(c, XL_WORKRAM_BANK, 0x3A));
    }

    /* ---- EXECUTED SYSTEM CARD: the interrupt is taken through the CPU's own vector
     * table, and the ROM's handler does everything else.
     *
     * The table lives at the top of the address space, $FFF6-$FFFF, five 16-bit
     * entries in the hardware manual's order: IRQ2 ($FFF6), IRQ1 ($FFF8), timer
     * ($FFFA), NMI ($FFFC), reset ($FFFE).  It is read THROUGH THE MPRs, so it is
     * whatever the card's byte at that moment is -- with bank $00 at $E000 that is the
     * last 10 bytes of the user's file.  Nothing here interprets the entry; the CPU
     * simply jumps to it, and ROM $E870 (the IRQ1 entry) then does the frame
     * housekeeping and dispatches through $2202 itself.
     *
     * The modelled $E8E2 short-circuit below must NOT run in this mode: entering the
     * card's default hook is exactly what the card does with a bare `rti`, and the
     * interpreter's `rti` already unwinds the frame the CPU pushed here. */
    if (c->bios_exec && c->rom_present) {
        static const uint16_t vec[3] = { 0xFFF6u, 0xFFF8u, 0xFFFAu };  /* CD, VDC, timer */
        uint16_t target = xl_rd16(c, vec[kind]);
        extern uint16_t xl_irq_entry_status;
        extern uint8_t xl_irq_entry_3a;
        extern uint16_t xl_irq_entry_pc;
        xl_trace_irq(c, kind, vec[kind]);   /* pc is still the interrupted one here */
        xl_push(c, (uint8_t)(c->pc >> 8));
        xl_push(c, (uint8_t)(c->pc & 0xFF));
        xl_push(c, (uint8_t)((c->p & (uint8_t)~(XL_BF | XL_T))));
        c->p |= XL_I;
        c->p &= (uint8_t)~(XL_T | XL_D);
        c->irq_taken++;
        xl_irq_entry_status = c->vdc.status;
        /* $3A is a work-RAM variable, so it is read through the work-RAM bank and not
         * through the CPU's mapping: during the card's init MPR1 is $FF (the I/O page
         * over $2000-$3FFF), and the mapped read returned a hardware byte instead of
         * the variable -- and showed up in the bus-access trace (XLATE_ACC_FROM/TO) as
         * an access the machine never made.  Same for $3F31 below. */
        xl_irq_entry_3a = xl_rd_phys(c, XL_WORKRAM_BANK, 0x3A);
        xl_irq_entry_pc = c->pc;
        c->pc = target;
        c->in_irq++;
        return;
    }

    /* $E8E2 is the BIOS's own default hook: the single byte `40` = RTI at ROM
     * offset $8E2.  All six slots hold it after a reset, and the console's work
     * RAM at f1748/f1800/f1837 shows all six still holding it while the boot
     * program runs.
     *
     * Entering a bare `rti` and returning are the same thing, so this case is
     * handled BEFORE the CPU frame is pushed -- "entering rti" must leave the
     * stack and the I flag exactly as it found them.  Handling it after the
     * push (the first attempt) left I set and a frame on the stack with nothing
     * to pop it, and the machine took exactly ONE interrupt for the rest of the
     * run: measured irq=1 at frame 5849.
     *
     * Without the rule at all, the first vblank after the hand-off -- which
     * arrives before the game installs its handler at $2202 -- ran $E8E2 through
     * the BIOS-stub dispatcher, whose call/return convention is not an interrupt
     * return, and the machine ended up executing in the I/O bank (measured:
     * pc $0081, and pc $0001 in a later run). */
    if (hook_addr) {
        uint8_t d0 = xl_rd(c, hook_addr);
        uint8_t d1 = xl_rd(c, (uint16_t)(hook_addr + 1));
        if ((d0 | d1) && (uint16_t)(d0 | (d1 << 8)) == 0xE8E2) {
            xl_irq_ack(c, kind);
            return;
        }
    }

    xl_push(c, (uint8_t)(c->pc >> 8));
    xl_push(c, (uint8_t)(c->pc & 0xFF));
    xl_push(c, (uint8_t)((c->p & (uint8_t)~(XL_BF | XL_T))));
    c->p |= XL_I;
    c->p &= (uint8_t)~(XL_T | XL_D);
    c->irq_taken++;
    {
        extern uint16_t xl_irq_entry_status;
        extern uint8_t xl_irq_entry_3a;
        extern uint16_t xl_irq_entry_pc;
        xl_irq_entry_status = c->vdc.status;
        xl_irq_entry_3a = xl_rd_phys(c, XL_WORKRAM_BANK, 0x3A);
        xl_irq_entry_pc = c->pc;
    }

    if (hook_addr) {
        /* Two bytes: the handler address, and nothing else.  No bank byte exists in
         * this layout (see XL_HOOK_* in runtime.h), so there is nothing to compare a
         * bank against.  The handler runs in whatever bank the mapping currently has
         * at that address, exactly as the CPU's `jmp (slot)` would; if that bank has
         * no translated code there, the next step reports `untranslated` with the
         * bank and address, which is the truth and not a mismatch.
         * ($E8E2 was already handled above, before the stack frame.) */
        uint8_t h0 = xl_rd(c, hook_addr);
        uint8_t h1 = xl_rd(c, (uint16_t)(hook_addr + 1));
        if (h0 | h1) {
            uint16_t target = (uint16_t)(h0 | (h1 << 8));
            xl_trace_irq(c, kind, 0);
            c->pc = target;
            c->in_irq++;
            return;
        }
    }

    if (c->log)
        xl_logf(c, "irq: %s raised at $%04X with no handler installed -- discarded",
                xl_irq_name(kind), c->pc);
    printf("IRQ kind=%s pc=%04X unhandled=1\n", xl_irq_name(kind), c->pc);
    fflush(stdout);
    /* Behave like a handler that returns after acknowledging the source: a real
     * BIOS handler reads the status register, which clears the latch. */
    xl_irq_ack(c, kind);
    c->p = xl_pop(c);
    c->pc = (uint16_t)(xl_pop16(c));
}

/* ------------------------------------------------------- native boot service --

 * What the CD BIOS does between the boot program and the game, as far as this port
 * needs it, with where each fact comes from:
 *
 *   - CD_BOOT is the BIOS entry at $E000 (ROM: jmp $E0F3).  It switches ROM bank $01
 *     into $C000 ($E0F3-$E104) and calls $CB0F, which clears $2000-$3FFF
 *     (tii $2000,$2001,$00FF and tii $2200,$2201,$1DFF at $CB11/$CB1B) and
 *     initialises the drive; the "PUSH RUN BUTTON" screen is drawn by the stream
 *     routine at $C950, fed from the table at $C97B ($C860-$C887).
 *   - the IPL then reads record 2 (1 sector) to $3000 and jumps there; that loader
 *     reads record $23 (20 sectors) to $4000 in CD RAM and jumps there; the boot
 *     program reads record 3 (24 sectors) to $1000 in CD RAM.  All three steps are
 *     executed by *translated* code in this port, and the bytes are verified.
 *   - the game module is record 67 ($43), 12,288 bytes, and the oracle's own
 *     savestate at the moment game code runs shows MPR2..MPR3 = $69,$6A with
 *     PC = $8062 in bank $72 - i.e. the game lives in CD RAM banks $69+.
 *
 * So the step modelled here is exactly one: read record $43 (6 sectors) into CD RAM
 * banks $69/$6A, map them at $4000/$6000, set the bank-base byte at $20DB (the
 * kernel's entry does `adc $DB` to page its own overlays), and jump to $4000 - the
 * kernel's own entry point, which the kernel image confirms is `jmp $40F9`.
 */
void xl_boot_game(Cpu *c)
{
    /* Which CD RAM bank gets which disc block.  This mapping is *measured*, not read
     * out of a table: for every bank the oracle had mapped at the logo capture, the
     * 8 KB in memory matches an 8 KB-aligned disc block exactly (8192/8192), and the
     * same bytes are present at the intro capture, so the banks are loaded rather than
     * generated.  The loader's own table at $312F covers only the first load; the
     * table that produces *this* arrangement has not been located, and the entries are
     * not in the order they are read.  Reported as such in the README.
     */
    static const struct { uint8_t bank; uint16_t record; } blocks[] = {
        { 0x69, 67 }, { 0x6A, 71 }, { 0x6B, 193 }, { 0x6C, 197 },
        { 0x6D, 225 }, { 0x6E, 229 }, { 0x6F, 221 }, { 0x70, 213 },
        { 0x71, 217 }, { 0x72, 209 }, { 0x73, 201 }, { 0x74, 205 },
        { 0x75, 77 }, { 0x76, 81 }, { 0x77, 85 }, { 0x78, 97 },
        { 0x79, 101 }, { 0x7A, 105 }, { 0x7B, 109 }, { 0x7C, 113 },
        { 0x7D, 117 }, { 0x7E, 121 }, { 0x7F, 125 },
    };
    unsigned i;
    /* Read from the oracle's own work RAM (BaseRAM, offset $0DB) at the logo capture:
     * the byte is $68.  With it the kernel's `lda #$0D; adc $DB` maps MPR4..6 =
     * $75/$76/$77, and its `jsr $B233` lands in bank $76 on `phx; pha; clx; ...` -
     * real code.  ($64 and $65 were guesses; both put that call on data.) */
    const unsigned overlay_base = 0x68;  /* oracle $20DB: +$0D lands on MPR4=$72 */

    if (!c->disc_attached) {
        if (c->log)
            xl_logf(c, "boot: no disc attached, cannot load the game");
        return;
    }
    for (i = 0; i < sizeof blocks / sizeof blocks[0]; i++) {
        size_t off = ((size_t)blocks[i].record + 225u) * 2048u;
        if (off + 8192u > c->disc_len) {
            if (c->log)
                xl_logf(c, "boot: record %u past end of payload", blocks[i].record);
            continue;
        }
        xl_stale_note(c, blocks[i].bank, 0, XL_SW_BOOTLOAD);
        memcpy(&c->mem[(size_t)blocks[i].bank * XL_BANK_SIZE], c->disc + off, 8192u);
    }
    c->mpr[2] = 0x69;
    c->mpr[3] = 0x6A;
    xl_wr(c, 0x20DB, (uint8_t)overlay_base);
    /* The BIOS's own initialisation, which we do natively because it is ROM we do
     * not execute.  Two parts matter to the game:
     *
     *  - $F5 = $06, the flags the IRQ2 path branches on (ROM $E870 `bbs1 $F5,$E86D`,
     *    $E88D `bbs5 $F5,$E8B4`, $E8B4 `bbr4 $F5`, $E8BC `bbs7`, $E8D5 `bbr6`).
     *    Bit 1 = "a game hook is installed, dispatch through $2202"; the oracle holds
     *    exactly $06 at the logo, and its $2202 does hold a game address.  A program
     *    polling "has the BIOS finished setting up" reads this.
     *  - the interrupt mask at $0C02: the oracle's mask register reads $07, which is
     *    all-masked, so the BIOS must clear it while it sets the drive up; without
     *    that our VDC interrupt raises and is never taken (vblank_irq=1, irq=0).
     */
    /* $F3/$F4 are the BIOS's control-register shadow, and the vblank housekeeping
     * above restores CR from them every frame.  The BIOS leaves $F3 = $88
     * (background on + vertical-blank interrupt enabled) - the oracle's work RAM
     * holds exactly that.  Without the interrupt bit the game never runs again: the
     * kernel's entry ($40F9-$4174) finishes its setup and parks in `jmp $4174`,
     * a deliberate self-jump, because *all* of its work happens in the vblank hook
     * it installed at $2202 (entry $4177, right beside the halt). */
    /* The BIOS leaves its default IRQ trampoline ($E8E2, a bare `rti`) in all six
     * hook slots.  Measured in the console's own work RAM at f1748, f1800 AND
     * f1837: $2200-$220B is `E2 E8 E2 E8 E2 E8 E2 E8 E2 E8 E2 E8`, i.e. six
     * complete two-byte slots ($E8E2, address $E8E2 with no bank byte) -- not just
     * the high bytes.  This code
     * used to write low bytes for slots 0/3/4/5 and *only* the high byte for
     * slots 1 and 2, which left those slots holding a half-written address; the
     * first vblank after the hand-off then dispatched through that half-address
     * (measured: pc $0081 with MPR0=$FF, and in another run $0001). */
    {
        unsigned s;
        for (s = 0; s < 6; s++) {
            xl_wr(c, (uint16_t)(0x2200u + s * 2u), 0xE2);
            xl_wr(c, (uint16_t)(0x2201u + s * 2u), 0xE8);
        }
    }
    xl_wr(c, 0x20F3, 0x88);
    xl_wr(c, 0x20F4, 0x00);
    /* $F5 is ZERO here, not $06.  Measured in the console's work RAM at f1748,
     * f1800 and f1837: $00 ($DB = $68 in the same dump).  The boot program clears
     * it itself (`stz $F5`, $402B) and the GAME sets $06 at $4164, after it has
     * installed its own handler at $2202 with `jsr $E05D`.  Writing $06 here
     * claimed a state the boot had not reached yet. */
    xl_wr(c, 0x20F5, 0x00);
    /* The interrupt mask, at the register it actually lives in.
     *
     * Evidence, not inference: the console's own hardware-access trace writes the
     * interrupt controller exactly twice in its first 3,700 frames, both at frame 0
     * from BIOS code, and never writes $0C02 at all --
     *   f=0 pc=$CB75 (52085) W $1403 = $00
     *   f=0 pc=$CB7B (52091) W $1402 = $00
     * $1402 is inverted, so $00 = every source enabled -- that includes IRQ1, which
     * is the video interrupt, so the console really does hand the game over with
     * interrupts enabled.  It does NOT enable anything else by accident: $1403's
     * pending flags are all clear at that point, and the six hook slots hold the
     * BIOS's bare-`rti` default.
     *
     * This used to be `xl_wr(c, 0x0C02, 0x00)`.  $0C02 is the timer's reload
     * register (research §5.3), so that write did nothing to the mask; the mask this
     * port ran on was set by reading $0C02 as the mask and by a reset default of $07.
     * The value written has not changed (0 then, 0 now, still "all enabled"), so if
     * this round moves the boot gate at all it is because the game's *later* writes
     * to $1402 and $1403 are now obeyed, not because the hand-off state changed. */
    xl_wr(c, 0x1403, 0x00);          /* acknowledge anything pending: as $CB75 does */
    xl_wr(c, 0x1402, 0x00);          /* mask $00 = IRQ2, IRQ1 and TIMER all enabled */
    c->pc = 0x4000;
    if (c->log)
        xl_logf(c, "boot: loaded %u disc blocks into CD RAM banks $69-$7F, "
                   "MPR2=$69 MPR3=$6A, $20DB=$%02X; BIOS init $F5=$00 (no game "
                   "hook yet; the game sets it), $1403=$00 $1402=$00 (mask 0 = all "
                   "three sources enabled, as the console's own $CB75/$CB7B writes); "
                   "jumping to $4000",
                (unsigned)(sizeof blocks / sizeof blocks[0]), overlay_base);
    printf("BOOT blocks=%u banks=69..7F base=%02X entry=4000\n",
           (unsigned)(sizeof blocks / sizeof blocks[0]), overlay_base);
    fflush(stdout);
}

/* ------------------------------------------------------ reset-vector entry --

 * THE OTHER WAY INTO THE MACHINE (XLATE_BIOS_RESET=1, under XLATE_BIOS_EXEC).
 *
 * The default entry hands the translated loader a machine at $3000 whose mapping,
 * video chip and interrupt mask this port assembled by hand.  That is not how the
 * console boots.  It boots like this: the CPU comes out of RESET with every mapping
 * register zero, fetches the reset vector at $FFFE (which through that mapping is
 * the card's own first page) and lands in the card's init at $E0F3.  The card then
 * programs the hardware it needs -- the interrupt controller at $1402/$1403, the
 * display-control shadow at $F3/$F4, the timer, the VDC, the colour chip -- probes
 * the drive, draws PRESS RUN, and finally reads record 2 of the disc to $3000 and
 * jumps there.  The loader and everything after it is the same code we execute on
 * the default path; what changes is who set the machine up first.
 *
 * Everything this function asserts is measured, not assumed:
 *   - the power-on mapping: the console's own trace records its first frame as
 *     `pc=0000 mpr=0000000000000000` (trace-intro-3700.log.gz, first FRAME line),
 *     so every window maps card page 0 and the CPU's first fetch is at $0000,
 *     which in this ROM is `jmp $E0F3`;
 *   - the reset vector: $FFFE through that mapping, i.e. the last two bytes of the
 *     card's first 8 KB page -- where a HuC6280 fetches after RESET.  In this ROM
 *     both routes arrive at $E0F3, so the two descriptions agree.
 *
 * The hardware is left at its power-on state on purpose (video chip reset, palette
 * cleared, timer clear, no interrupt pending).  Seeding it with the state the BIOS
 * *would* leave would be assembling by hand the very state the switch exists to
 * measure; if the card's init cannot get from power-on to the console's state, that
 * is a defect in the hardware model and it has to show up as one. */
int xl_boot_reset(Cpu *c)
{
    uint16_t vector;
    unsigned i;

    if (!c->bios_reset)
        return 0;
    if (!c->bios_exec || !c->rom_present) {
        printf("BOOT_RESET off=1 reason=%s\n",
               !c->bios_exec ? "XLATE_BIOS_EXEC is off" : "no System Card attached");
        if (c->log)
            xl_logf(c, "boot: XLATE_BIOS_RESET ignored (%s); entering the loader at "
                       "$3000 as before", !c->bios_exec ? "XLATE_BIOS_EXEC off"
                                                        : "no System Card");
        c->bios_reset = 0;
        return 0;
    }
    /* Power-on: every window maps card page 0 (measured, see above). */
    for (i = 0; i < 8; i++)
        c->mpr[i] = 0;
    /* A HuC6280 after RESET fetches $FFFE through MPR7; with card page 0 there that
     * is ROM byte $1FFE, the last entry of the vector table xl_report_vectors()
     * prints. */
    vector = xl_rd16(c, 0xFFFE);
    c->pc = vector;
    /* Held until the next step: see `reset_resume` in runtime.h.  A reset that
     * lands mid-instruction must not be undone by that instruction's own pc. */
    c->reset_resume = vector;
    /* The chips come up cold, not programmed: whatever the screen shows from here is
     * what the card puts there.  The VCE is not zero, though: VCE::Reset gives it
     * the checkerboard colour table (entry 0 white), which is why the console's
     * first frames are pure white rather than black. */
    vdc_reset(&c->vdc);
    vce_reset(&c->vce);
    vdc_render_top_line(&c->vdc, &c->vce);
    memset(&c->timer, 0, sizeof c->timer);
    c->irq_status = 0;
    c->irq_mask = XL_IQ_ALL;          /* power-on: nothing enabled, nothing pending */
    /* PCE_Power() calls PCEINPUT_Power(), which puts the multitap counter back to
     * controller 0 with SEL and CLR low (input.cpp).  The controllers' own button
     * state is NOT reset -- a reset does not let go of a held button -- so only the
     * selector state moves here. */
    c->joypad_sel = 0;
    c->joypad_clr = 0;
    c->joypad_index = 0;
    c->s = 0xFF;
    c->p = (uint8_t)(XL_I | XL_BF);   /* interrupts masked, as RESET leaves them */
    /* RESET LEAVES THE CPU IN SLOW MODE.  huc6280.cpp:567, the `IQRESET` arm:
     *     speed = 0;
     *     REDOSPEEDCACHE();
     * and the reset vector this function then enters ($E0F3) is `sei; csh`, so the
     * card climbs back to 7.16 MHz on its second instruction.  This entry is the
     * card-executing one (it returns 0 without XLATE_BIOS_RESET), so the two slow
     * instructions are executed for real and nothing has to be assumed about them. */
    xl_speed_set(c, 0);
    printf("BOOT_RESET on=1 pc=%04X vector=%04X mpr=0000000000000000 "
           "entry=card-init\n", c->pc, vector);
    if (c->log)
        xl_logf(c, "boot: reset-vector entry -- MPRs all zero (measured: the console's "
                   "frame-0 trace), reset vector $%04X fetched from $FFFE, VDC/VCE/"
                   "timer at power-on, $1402=$%02X.  The card's own init runs from "
                   "here; the port writes no boot state.", vector, c->irq_mask);
    fflush(stdout);
    return 1;
}

/* ------------------------------------------------------ reset schedule --
 *
 * A CONSOLE RESET AS AN INPUT EVENT, not as a hardware rule.
 *
 * A console RESET is not a pad input, so an input schedule cannot carry one: a
 * schedule that resets the machine has to name the frames at which the power is
 * cut and restored, and the machine comes back up inside the same run.  The
 * scheduler here is deliberately the same shape as the press file: a list of
 * frames, applied at the top of the named frame, in the same counter the press
 * windows are compared against (c->vdc.frame).  The event is scheduled here; what
 * a reset DOES to the machine is xl_boot_reset() + xl_console_reset() below, and
 * that is the hardware rule, written once.
 *
 * What a reset leaves behind is measurable rather than assumed: across a reset,
 * work RAM comes back holding only what power-on holds (a few dozen non-zero
 * bytes of 8,192), $2047 returns to 0, and the first access afterwards is the
 * System Card's own init rather than game code.
 */
static uint32_t xl_reset_frames[64];
static int      xl_reset_n;
static int      xl_reset_next;          /* first entry not yet performed */

int xl_reset_at(uint32_t frame)
{
    int i;
    if (xl_reset_n >= (int)(sizeof xl_reset_frames / sizeof xl_reset_frames[0])) {
        fprintf(stderr, "runtime: reset schedule full at %d frames; %u dropped\n",
                xl_reset_n, frame);
        return 0;
    }
    /* Kept sorted so the per-frame test is one compare: a schedule written in
     * movie order is already sorted, and one that is not still fires in order. */
    for (i = xl_reset_n; i > 0 && xl_reset_frames[i - 1] > frame; i--)
        xl_reset_frames[i] = xl_reset_frames[i - 1];
    xl_reset_frames[i] = frame;
    xl_reset_n++;
    return 1;
}

int xl_reset_count(void)
{
    return xl_reset_n;
}

uint32_t xl_reset_frame(int i)
{
    return (i >= 0 && i < xl_reset_n) ? xl_reset_frames[i] : 0;
}

/* Perform a console RESET on a machine that is already running.
 *
 * THE DIFFERENCE FROM xl_boot_reset() IS THAT THE MACHINE HAS HISTORY.  What a
 * reset must and must not destroy is measured against the reference core's
 * `PCE_Power()` (mednafen/pce/pce.cpp) and against the console corpus that
 * core produced:
 *
 *   - WORK RAM GOES TO ZERO.  `PCE_Power()` does `memset(BaseRAM, 0, ...)` before
 *     anything runs, and the corpus's work RAM at console frame 38,137 is 0
 *     non-zero bytes of 8,192 -- the card's init then rebuilds it (28 bytes at
 *     38,138, 43 at 38,139).  A reset that kept work RAM would leave the card's
 *     init running on the dead game's variables.
 *   - THE BACKUP RAM SURVIVES, both its contents and its write protection being
 *     re-armed (the reference clears `bBRAMEnabled`; the device is battery-backed,
 *     see XL_BRAM_BANK).  That is why a post-reset machine is not a cold boot:
 *     the card reads save data the run wrote before the reset, and the work RAM it
 *     builds from it differs from the cold boot's by 733 bytes by frame 9.
 *   - THE DRIVE IS NOT DISCARDED.  The reset lands mid-run with a disc in the
 *     drive, and the console's own trace says the unit keeps its place: the first
 *     post-reset CD accesses are the card's init (`$1804`), not a fresh spin-up,
 *     so `c->cd` is left exactly as it stands.  Discarding it would be a guess in
 *     the one direction the trace can already refute.
 *   - THE RASTER CLOCK IS NOT A CHIP.  `vdc_reset()` zeroes the line counter and
 *     the frame counter because it is written for power-on.  A reset that lands
 *     between two frames must not put the raster back at the top of the display
 *     (that would make the next frame 20 lines short) and must not renumber the
 *     frames (the schedule, the press file and every dump are keyed to them).
 *     The port's frame buffer is likewise an instrument, not chip memory: the
 *     frame that has just finished is still the frame that has just finished.
 *
 * Returns 1 when the machine was reset, 0 when the reset-vector entry is not
 * available (no card executed, or XLATE_BIOS_RESET off) -- in which case nothing
 * is touched at all. */
int xl_console_reset(Cpu *c)
{
    static uint8_t pixels[VD_W * VD_H * 3];
    int frame = c->vdc.frame, line = c->vdc.line, dot = c->vdc.dot;
    uint8_t vblank = c->vdc.vblank;

    memcpy(pixels, c->vdc.frame_pixels, sizeof pixels);
    if (!xl_boot_reset(c))
        return 0;
    c->vdc.frame = frame;
    /* THE RASTER STARTS A FRAME, IT DOES NOT RESUME ONE.  The boundary this reset
     * is scheduled on is crossed at line 240 of 263 (the VCE's frame end), so
     * putting the raster back where it was leaves the reset frame 23 lines long --
     * measured: 143,250 master clocks instead of 358,995.  The console's own reset
     * frame is a FULL one (trace-cyc-full.log: 38,137 -> 38,138 is 355,137 clocks),
     * because its PCE_Power() runs between frames and the machine then starts the
     * next frame at the top.  So `line`/`dot` are left as `vdc_reset()` put them
     * (0/0) and only the frame *number* and the finished picture are carried over.
     */
    c->vdc.vblank = vblank;
    memcpy(c->vdc.frame_pixels, pixels, sizeof pixels);

    memset(&c->mem[(size_t)XL_WORKRAM_BANK * XL_BANK_SIZE], 0, XL_BANK_SIZE);
    c->bram_wprot = 1;                  /* the card unlocks it again ($1807 = $80) */

    printf("CONSOLE_RESET frame=%d pc=%04X line=%d dot=%d\n", frame, c->pc,
           (int)line, (int)dot);
    fflush(stdout);
    if (c->log)
        xl_logf(c, "reset: console RESET at the top of frame %d -- pc=$%04X from "
                   "$FFFE, MPRs all zero, VDC/VCE/timer at power-on, work RAM "
                   "cleared, backup RAM kept (%u bytes written so far), drive left "
                   "as it stands (%u sectors queued)",
                frame, c->pc, c->bram_writes, c->cd.sectors_queued);
    return 1;
}

/* Called at every frame boundary, from xl_tick() -- the one place that sees them
 * all (a CD read advances frames inside its own tick burst).  It runs BEFORE the
 * frame-boundary instruments, because the console's own trace puts the reset
 * before the first access of the new frame: our RAM stream block for that frame
 * must be the post-reset array, exactly as the corpus's is. */
static void xl_reset_tick(Cpu *c)
{
    while (xl_reset_next < xl_reset_n &&
           xl_reset_frames[xl_reset_next] <= (uint32_t)c->vdc.frame) {
        xl_reset_next++;
        if (!xl_console_reset(c)) {
            /* Not silently skipped: a scheduled reset that could not happen is
             * the difference between "the port reset here" and "the port ignored
             * it", and only one of those can be compared with the corpus. */
            printf("CONSOLE_RESET frame=%d refused=1 (reset-vector entry not "
                   "available)\n", c->vdc.frame);
            fflush(stdout);
            if (c->log)
                xl_logf(c, "reset: frame %d REFUSED -- XLATE_BIOS_RESET/XLATE_BIOS_EXEC "
                           "off or no card attached", c->vdc.frame);
        }
    }
}

/* What the card's init left behind, read out of work RAM and the interrupt
 * controller where a program would read it.  Two calls bracket the boot: one when
 * the drive has been probed and one when the kernel is entered, so "the state the
 * kernel receives" is a printed line and can be compared with the console's own
 * trace and with a savestate's work RAM. */
void xl_report_boot_state(Cpu *c, const char *when)
{
    unsigned s;
    printf("BOOT_STATE %s frame=%u pc=%04X 1402=%02X 1403=%02X F3=%02X F4=%02X "
           "F5=%02X DB=%02X hooks=",
           when, (unsigned)c->vdc.frame, c->pc, c->irq_mask, c->irq_status,
           xl_rd(c, 0x20F3), xl_rd(c, 0x20F4), xl_rd(c, 0x20F5), xl_rd(c, 0x20DB));
    for (s = 0; s < 6; s++)
        printf("%02X%02X%s", xl_rd(c, (uint16_t)(0x2201u + s * 2u)),
               xl_rd(c, (uint16_t)(0x2200u + s * 2u)), s == 5 ? "\n" : ",");
    fflush(stdout);
    if (c->log)
        xl_logf(c, "boot_state %s f=%u pc=$%04X 1402=$%02X 1403=$%02X F3=$%02X "
                   "F4=$%02X F5=$%02X DB=$%02X CR=$%04X MWR=$%04X HDR=$%04X "
                   "VDR=%u VSR=$%04X", when, (unsigned)c->vdc.frame, c->pc,
                c->irq_mask, c->irq_status, xl_rd(c, 0x20F3), xl_rd(c, 0x20F4),
                xl_rd(c, 0x20F5), xl_rd(c, 0x20DB), c->vdc.cr, c->vdc.mwr,
                c->vdc.hdr, c->vdc.vdr & 0x1FF, c->vdc.vsr);
}

/* ---------------------------------------------------------------- driver -- */

/* WHERE EXECUTION ENTERS THE SYSTEM CARD, and from where (XLATE_ROM_ENTRY=1).
 *
 * A run under XLATE_BIOS_EXEC that stops inside the card has to be able to say which
 * caller put it there -- "the boot stalls somewhere in the BIOS" is not a finding.
 * One line per distinct (caller, entry) pair, with the frame it first happened. */
static struct { uint16_t to, from; uint32_t frame; } xl_rom_entry[24];
static int xl_rom_entry_n;

/* XLATE_ROMTRACE=from:to -- every entry into the card over a frame window, in order,
 * one line each, with the caller's frame, bank and pushed return address.  The
 * deduplicated note below answers "who got here first"; a count of calls in a frame
 * needs every one of them, which is what this prints. */
static int xl_romtrace_from = -2, xl_romtrace_to = -1;

static int xl_romtrace_on(Cpu *c)
{
    if (xl_romtrace_from == -2) {
        const char *e = getenv("XLATE_ROMTRACE");
        xl_romtrace_from = -1;
        if (e && e[0]) {
            unsigned a = 0, b = 0;
            if (sscanf(e, "%u:%u", &a, &b) == 2) {
                xl_romtrace_from = (int)a;
                xl_romtrace_to = (int)b;
            }
        }
    }
    return xl_romtrace_from >= 0 && (int)c->vdc.frame >= xl_romtrace_from &&
           (int)c->vdc.frame <= xl_romtrace_to && c->log;
}

static void xl_rom_return_note(Cpu *c)
{
    /* The value the card handed back: the game branches on A (0 = accepted,
     * anything else = this routine's own error code), so a card call that returns
     * a different A on our machine than on the console explains a different route
     * without a single pixel being consulted. */
    if (xl_romtrace_on(c))
        xl_logf(c, "romret f=%d to=%04X a=%02X x=%02X y=%02X p=%02X",
                c->vdc.frame, c->pc, c->a, c->x, c->y, c->p);
}

static void xl_rom_entry_note(Cpu *c, uint16_t from)
{
    int i;
    if (xl_romtrace_on(c)) {
        uint8_t lo = xl_rd(c, (uint16_t)(XL_STACK_BASE | ((c->s + 1) & 0xFF)));
        uint8_t hi = xl_rd(c, (uint16_t)(XL_STACK_BASE | ((c->s + 2) & 0xFF)));
        xl_logf(c, "romcall f=%d to=%04X from=%04X ret=%04X spr=%02X",
                c->vdc.frame, c->pc, from,
                (unsigned)((uint16_t)(lo | (hi << 8)) + 1u), c->s);
    }
    for (i = 0; i < xl_rom_entry_n && i < 24; i++) {
        if (xl_rom_entry[i].to == c->pc && xl_rom_entry[i].from == from)
            return;
    }
    if (xl_rom_entry_n < 24) {
        xl_rom_entry[xl_rom_entry_n].to = c->pc;
        xl_rom_entry[xl_rom_entry_n].from = from;
        xl_rom_entry[xl_rom_entry_n].frame = (uint32_t)c->vdc.frame;
        xl_rom_entry_n++;
        {
            /* The caller, when the entry is a jsr: the pushed return address is at
             * the top of the stack, the same two bytes xl_bios_call reads. */
            uint8_t lo = xl_rd(c, (uint16_t)(XL_STACK_BASE | ((c->s + 1) & 0xFF)));
            uint8_t hi = xl_rd(c, (uint16_t)(XL_STACK_BASE | ((c->s + 2) & 0xFF)));
            printf("ROM_ENTRY to=%04X from=%04X frame=%u ret=%04X\n",
                   c->pc, from, (unsigned)c->vdc.frame,
                   (unsigned)((uint16_t)(lo | (hi << 8)) + 1u));
        }
        fflush(stdout);
    }
}

/* One CPU step.  The translated dispatcher is the machine; the interpreter is the
 * same machine reading the bytes that are in memory now.  XLATE_INTERP=1 forces
 * every step through the interpreter (that is how the identity tests run both paths
 * over identical inputs).  Without it the guard decides, per bank: a bank whose live
 * bytes no longer match the image its translation was built from is interpreted
 * (see the rule above xl_guard_refresh). */
/* STEP TRACE state (see xl_cpu_step): -1 = not read from the environment yet. */
static int xl_step_on = -1;
static int xl_step_from, xl_step_to;
static unsigned xl_step_lo, xl_step_hi;

int xl_cpu_step(Cpu *c)
{
    static int forced = -1;
    static int entry_on = -1;
    static uint16_t prev_pc;
    static int prev_in_rom;
    int rom;
    if (forced < 0) {
        const char *e = getenv("XLATE_INTERP");
        forced = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (entry_on < 0) {
        const char *e = getenv("XLATE_ROM_ENTRY");
        entry_on = (e && e[0] && e[0] != '0') ? 1 : 0;
        if (getenv("XLATE_ROMTRACE") && getenv("XLATE_ROMTRACE")[0])
            entry_on = 1;      /* the per-call trace needs the previous pc */
    }
    /* A SYSTEM CARD PAGE IS EXECUTED, NEVER TRANSLATED.  The card is not ours to
     * translate and its bytes are not in this repository, so there is no translation
     * of them to run and no guard question to ask: the interpreter decodes what the
     * user's file holds at that address, which is what the hardware does.  This is
     * also why it must be checked BEFORE the dispatcher: the dispatcher's $E000 rule
     * is a model, and under this switch the model is what we are removing.
     *
     * The counter is separate from interp_steps so that "BIOS instructions actually
     * interpreted" is a number the report can quote on its own. */
    rom = xl_pc_in_rom(c);
    /* STEP TRACE -- "what did the machine actually execute here?"
     *
     * A bus-access trace (trace.c) shows the hardware registers a program touched;
     * it cannot show a branch that was not taken or a call that was not made,
     * because neither touches the bus.  When the question is "the console took the
     * branch at $XXXX and this machine did not", the instrument has to be execution
     * itself.  One line per step (a translated unit runs to its own end, so a unit
     * is one line; under the interpreter one instruction is one line), with the
     * return address sitting on the stack, so the call chain is in the trace and not
     * inferred from it.
     *
     *   XLATE_STEP_FROM=<frame>   first frame to record
     *   XLATE_STEP_TO=<frame>     last frame to record (default: the first frame)
     *   XLATE_STEP_PC=<lo>:<hi>   only pcs in that range (hex, inclusive; default all)
     *
     * Off unless XLATE_STEP_FROM is set: one compare per step, nothing else. */
    if (xl_step_on < 0) {
        const char *e = getenv("XLATE_STEP_FROM");
        xl_step_on = 0;
        if (e && e[0]) {
            const char *f = getenv("XLATE_STEP_TO");
            const char *p = getenv("XLATE_STEP_PC");
            xl_step_lo = 0;
            xl_step_hi = 0xFFFF;
            xl_step_from = atoi(e);
            xl_step_to = f ? atoi(f) : xl_step_from;
            if (p && p[0]) {
                unsigned a = 0, b = 0;
                if (sscanf(p, "%x:%x", &a, &b) == 2) {
                    xl_step_lo = (unsigned)(a & 0xFFFFu);
                    xl_step_hi = (unsigned)(b & 0xFFFFu);
                }
            }
            xl_step_on = 1;
        }
    }
    if (xl_step_on && c->log && (int)c->vdc.frame >= xl_step_from &&
        (int)c->vdc.frame <= xl_step_to &&
        (unsigned)c->pc >= xl_step_lo && (unsigned)c->pc <= xl_step_hi) {
        uint8_t lo = xl_rd(c, (uint16_t)(XL_STACK_BASE | ((c->s + 1) & 0xFF)));
        uint8_t hi = xl_rd(c, (uint16_t)(XL_STACK_BASE | ((c->s + 2) & 0xFF)));
        xl_logf(c, "step f=%u pc=%04X b=%02X a=%02X x=%02X y=%02X p=%02X s=%02X "
                   "ret=%04X irq=%d",
                (unsigned)c->vdc.frame, c->pc, c->mpr[c->pc >> 13], c->a, c->x,
                c->y, c->p, c->s,
                (unsigned)((uint16_t)((uint16_t)lo | ((uint16_t)hi << 8)) + 1u),
                c->in_irq);
    }
    if (c->bios_reset && c->pc == 0x4174) {
        /* The kernel's idle loop: the exact pc the default entry used to stop at and
         * never leave.  Print the state the kernel actually arrived with, once, so
         * "what the card's init left" is a measurement at the moment that matters. */
        static int told;
        if (!told) {
            told = 1;
            xl_report_boot_state(c, "kernel-idle");
        }
    }
    if (entry_on) {
        /* Where did control come FROM?  "Under the switch, where did it go instead?"
         * is a question that must be answered with a name and a frame.
         *
         * The previous pc's OWN page is remembered when it is stepped, not re-derived
         * from the MPRs here: by the time control arrives the game has already
         * remapped the window, so asking the MPRs where the old pc lived answers with
         * the new mapping and the transition is invisible.  (That is what happened to
         * the deduplicated note below: it never fired for a `jmp $E04E`, because page
         * 7 held bank $00 in both the caller's and the card's mapping.) */
        if (rom && !prev_in_rom)
            xl_rom_entry_note(c, prev_pc);
        else if (!rom && prev_in_rom)
            xl_rom_return_note(c);
        prev_pc = c->pc;
        prev_in_rom = rom;
    }
    if (forced || rom) {
        int ok = xl_interp_step(c);
        if (ok) {
            c->interp_steps++;      /* instructions the interpreter actually ran */
            if (rom)
                c->bios_interp_steps++;
        }
        return ok;
    }
    {
        uint8_t b = c->mpr[c->pc >> 13];
        if (xl_guard_can[b] && xl_guard_stale(c, b)) {
            int ok = xl_interp_step(c);
            if (ok) {
                c->interp_steps++;
                xl_guard_note_interp(c, b);
            }
            return ok;
        }
    }
    return xlate_step(c);
}

int xl_run(Cpu *c, unsigned long max_steps, int use_stop_pc, uint16_t stop_pc)
{
    return xl_run_budget(c, max_steps, use_stop_pc, stop_pc, 0);
}

uint16_t xl_irq_entry_status;
uint8_t xl_irq_entry_3a;
uint16_t xl_irq_entry_pc;

uint32_t xl_hist[65536];
uint32_t xl_cd_pending_at;
static int hist_on = -1;

/* ------------------------------------------------------------------ ramdump --
 * REPORT-ONLY instrument: whole-RAM snapshots at chosen frame boundaries, in the
 * same byte layout the reference emulator writes its whole-RAM dumps in, so the two
 * machines can be diffed with no conversion:
 *
 *   0x00000 .. 0x3FFFF  Super CD RAM, physical banks $68..$87, $68 first (32 x 8 KB)
 *   0x40000 .. 0x41FFF  base work RAM, physical bank $F8 (8 KB)
 *
 * Enabled only by XLATE_RAMDUMP (path prefix) plus XLATE_RAMDUMP_FRAMES (comma or
 * space separated frame numbers).  When unset, this costs one getenv() on the
 * first frame boundary and nothing else.  It writes memory and a log line; it
 * does not touch the CPU, the CD model, dispatch or data placement.
 */
static char     xl_ramdump_prefix[512];
static uint32_t xl_ramdump_frames[32];
static int      xl_ramdump_nframes = -1;   /* -1 = not yet read from the env */

static void xl_ramdump_init(void)
{
    const char *p = getenv("XLATE_RAMDUMP");
    const char *s = getenv("XLATE_RAMDUMP_FRAMES");
    xl_ramdump_nframes = 0;
    if (!p || !p[0] || !s || !s[0])
        return;
    snprintf(xl_ramdump_prefix, sizeof xl_ramdump_prefix, "%s", p);
    while (*s && xl_ramdump_nframes < 32) {
        char *end = NULL;
        unsigned long v = strtoul(s, &end, 10);
        if (end == s)
            break;
        xl_ramdump_frames[xl_ramdump_nframes++] = (uint32_t)v;
        s = end;
        while (*s == ',' || *s == ' ')
            s++;
    }
}

static void xl_ramdump_write(Cpu *c, uint32_t frame)
{
    char path[640];
    FILE *f;

    snprintf(path, sizeof path, "%s-f%05u.bin", xl_ramdump_prefix, frame);
    f = fopen(path, "wb");
    if (!f) {
        if (c->log)
            xl_logf(c, "ramdump: cannot write %s", path);
        return;
    }
    fwrite(&c->mem[(size_t)0x68 * XL_BANK_SIZE], 1, (size_t)32 * XL_BANK_SIZE, f);
    fwrite(&c->mem[(size_t)XL_WORKRAM_BANK * XL_BANK_SIZE], 1, XL_BANK_SIZE, f);
    fclose(f);
    if (c->log)
        xl_logf(c, "ramdump: frame %u -> %s (banks $68-$87 + $F8, %d bytes)",
                frame, path, 32 * XL_BANK_SIZE + XL_BANK_SIZE);
}

static void xl_ramdump_maybe(Cpu *c, uint32_t frame)
{
    int i;
    if (xl_ramdump_nframes < 0)
        xl_ramdump_init();
    for (i = 0; i < xl_ramdump_nframes; i++) {
        if (xl_ramdump_frames[i] == frame) {
            xl_ramdump_write(c, frame);
            xl_ramdump_frames[i] = 0xFFFFFFFFu;   /* once per frame */
            return;
        }
    }
}

/* ------------------------------------------------------------------ framelog --
 * XLATE_FRAMELOG=PATH: one line per frame boundary, at the same instant the picture
 * series is written -- "<frame> <$31> <$222D>".  It exists so a comparison tool can
 * pair a port frame with a reference frame without guessing: the file's Nth line is
 * the machine's state at the start of frame <frame>, which is the convention the
 * console oracle's ram.bin uses (block N = start-of-frame-N), and the picture written
 * at the same instant holds frame <frame>-1's rows.
 *
 * One line per frame instead of 8 KB per frame: a whole-route structural comparison
 * costs 20 bytes a frame rather than 8 KB, and the frame number is in the line, so a
 * run resumed from a checkpoint needs no index arithmetic to be read.
 *
 * FRAME 0 IS A LINE LIKE ANY OTHER, written by xl_framelog_begin() from the power-on
 * RAM, because the reference's block 0 is the machine before its first instruction --
 * the same reason xl_ramstream_begin() writes block 0.  Without it the first frame of
 * every run has a picture and a reference block but no port structure, and a comparison
 * has to choose between calling frame 0 "clean" (which it never measured) and declaring
 * the whole run's clearance unestablished.  It did the latter; this is why. */
static FILE *xl_framelog_open(Cpu *c)
{
    static FILE *lf = NULL;
    static int tried = 0;
    if (!tried) {
        const char *p = getenv("XLATE_FRAMELOG");
        tried = 1;
        if (p && p[0]) {
            lf = fopen(p, "w");
            if (!lf && c->log)
                xl_logf(c, "framelog: cannot write %s", p);
        }
    }
    return lf;
}

static void xl_framelog_line(Cpu *c)
{
    FILE *lf = xl_framelog_open(c);
    if (!lf)
        return;
    fprintf(lf, "%u %02X %02X\n", (unsigned)c->vdc.frame,
            xl_rd(c, 0x2031), xl_rd(c, 0x222D));
    fflush(lf);                       /* readable while the run is going */
}

void xl_framelog_begin(Cpu *c)
{
    xl_framelog_line(c);              /* frame 0 = the power-on state */
}

static void xl_framelog_maybe(Cpu *c)
{
    xl_framelog_line(c);
}

/* ----------------------------------------------------------------- satstream --
 * XLATE_SATSTREAM=PATH: the VDC's 512-byte sprite attribute table, once per frame, at
 * the frame boundary -- block N = the SAT in force at the start of frame N, which is
 * the layout the console corpus' sat.bin used.  The reference this project measures
 * against (BizHawk/TurboNyma) exposes main RAM only, so this channel is named
 * "unavailable" until such a dump exists; the port side is here so the comparison is
 * wired and cannot be quietly forgotten. */
static void xl_satstream_maybe(Cpu *c)
{
    static FILE *sf = NULL;
    static int tried = 0;
    if (!tried) {
        const char *p = getenv("XLATE_SATSTREAM");
        tried = 1;
        if (p && p[0]) {
            sf = fopen(p, "wb");
            if (!sf && c->log)
                xl_logf(c, "satstream: cannot write %s", p);
        }
    }
    if (!sf)
        return;
    {
        uint8_t b[VD_SAT_WORDS * 2];
        int i;
        for (i = 0; i < VD_SAT_WORDS; i++) {
            b[2 * i] = (uint8_t)(c->vdc.sat[i] & 0xFF);
            b[2 * i + 1] = (uint8_t)(c->vdc.sat[i] >> 8);
        }
        fwrite(b, 1, sizeof b, sf);
        fflush(sf);
    }
}

/* XLATE_RAMSTREAM=PATH: append the 8 KB work RAM (the bank mapped at $2000-$3FFF,
 * 8192 bytes) once per frame, the same layout and the same sampling point as the
 * console oracle's ram.bin, so the two streams diff with no conversion:
 * byte f*8192 .. f*8192+8191 is the machine's state at the START of frame f.
 * Flushed per frame, so the file is readable while the run is still going. */
static FILE *xl_ramstream_file(Cpu *c)
{
    static FILE *sf = NULL;
    static int tried = 0;
    if (!tried) {
        const char *p = getenv("XLATE_RAMSTREAM");
        tried = 1;
        if (p && p[0]) {
            sf = fopen(p, "wb");
            if (!sf && c->log)
                xl_logf(c, "ramstream: cannot write %s", p);
        }
    }
    return sf;
}

static void xl_ramstream_write(Cpu *c, uint32_t frame)
{
    FILE *sf = xl_ramstream_file(c);
    if (!sf)
        return;
    fwrite(&c->mem[(size_t)XL_WORKRAM_BANK * XL_BANK_SIZE], 1, XL_BANK_SIZE, sf);
    fflush(sf);
    if (c->log && (frame % 1000) == 0)
        xl_logf(c, "ramstream: frame %u written", frame);
}

/* BLOCK 0 IS THE POWER-ON STATE, or every comparison against the console corpus is
 * one frame out.  The console's ram.bin block 0 is the machine before any
 * instruction has run (it is all zeroes); this stream used to begin at the first
 * frame boundary (the state at the start of frame 1), so ours[i] was the console's
 * [i+1] and every lag this file ever reported read one frame low.  Called from
 * xl_reset(), where the work RAM really is the just-powered-on array. */
void xl_ramstream_begin(Cpu *c)
{
    FILE *sf = xl_ramstream_file(c);
    if (!sf)
        return;
    fwrite(&c->mem[(size_t)XL_WORKRAM_BANK * XL_BANK_SIZE], 1, XL_BANK_SIZE, sf);
    fflush(sf);
}

static void xl_ramstream_maybe(Cpu *c, uint32_t frame)
{
    static int on = -1;
    if (on < 0) {
        const char *p = getenv("XLATE_RAMSTREAM");
        on = (p && p[0]) ? 1 : 0;
    }
    if (on)
        xl_ramstream_write(c, frame);
}

/* ------------------------------------------------------------------ wrseries --
 * REPORT-ONLY instrument: the 8 KB work-RAM bank at every Nth frame boundary, one
 * file per sampled frame, in the console's own byte order (bank $F8 is what the
 * oracle saves as `BaseRAM`).  It exists so "which of our frames is the console's
 * state" can be answered by a distance curve instead of by eye: the console's five
 * captures of the held intro are identical in every record it saves, so a frame
 * search needs our variable values per frame, not just a handful.
 *
 * Enabled only by XLATE_WR_EVERY (pitch in frames) with XLATE_WR_DIR (directory);
 * unset, this is one getenv() on the first frame boundary.  It writes 8 KB and
 * touches nothing else -- no CPU, CD, dispatch or placement effect.
 */
static int  xl_wrseries_every = -2;          /* -2 = not read from the env yet */
static char xl_wrseries_dir[512];

static void xl_wrseries_maybe(Cpu *c, uint32_t frame)
{
    char path[640];
    FILE *f;

    if (xl_wrseries_every == -2) {
        const char *e = getenv("XLATE_WR_EVERY");
        const char *d = getenv("XLATE_WR_DIR");
        xl_wrseries_every = e ? atoi(e) : 0;
        snprintf(xl_wrseries_dir, sizeof xl_wrseries_dir, "%s", d ? d : ".");
    }
    if (xl_wrseries_every <= 0 || (frame % (uint32_t)xl_wrseries_every) != 0)
        return;
    snprintf(path, sizeof path, "%s/w%05u.bin", xl_wrseries_dir, (unsigned)frame);
    f = fopen(path, "wb");
    if (!f)
        return;
    fwrite(&c->mem[(size_t)XL_WORKRAM_BANK * XL_BANK_SIZE], 1, XL_BANK_SIZE, f);
    fclose(f);
}

/* Per-frame CPU position, in the shape a console trace records it
 * (`f= pc=$ bank=$ mpr=`).  Report-only, driven by
 * XLATE_FRAMEPC; it exists so "the two machines are in different places in the
 * program" can be measured as a frame number instead of guessed from RAM. */
static int xl_framepc_on = -1;
static void xl_framepc_maybe(Cpu *c, uint32_t frame)
{
    unsigned b = (unsigned)(c->pc >> 13);
    if (xl_framepc_on < 0) {
        const char *e = getenv("XLATE_FRAMEPC");
        xl_framepc_on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (!xl_framepc_on || !c->log)
        return;
    xl_logf(c, "framepc f=%u pc=$%04X page=%u exec_bank=$%02X mpr=%02X%02X%02X%02X%02X%02X%02X%02X",
            frame, c->pc, b, c->mpr[b], c->mpr[0], c->mpr[1], c->mpr[2], c->mpr[3],
            c->mpr[4], c->mpr[5], c->mpr[6], c->mpr[7]);
}

int xl_run_budget(Cpu *c, unsigned long max_steps, int use_stop_pc,
                  uint16_t stop_pc, uint64_t max_cycles)
{
    unsigned long i;
    if (hist_on < 0) {
        const char *e = getenv("XLATE_HIST");
        hist_on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (xl_trace_on < 0) {
        const char *e = getenv("XLATE_TRACE");
        xl_trace_on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    for (i = 0; i < max_steps; i++) {
        /* The reset vector, re-asserted at an instruction boundary: see
         * `reset_resume` in runtime.h.  One compare when no reset is pending. */
        if (c->reset_resume) {
            c->pc = c->reset_resume;
            c->reset_resume = 0;
        }
        /* Boot gate: on the console the BIOS owns the machine until RUN is pressed,
         * and the disc boot starts from there (no CD command exists before frame
         * 1700 in the trace).  Holding the CPU at the loader's entry until the
         * press puts the loader, the boot program and its CD load at the same
         * point in the game's life as they are on the console, instead of 1,700
         * frames earlier in a phase we do not render.
         *
         * The release condition is the PAD EVENT, not a frame number: the gate
         * opens in xl_pad_tick when RUN arrives.  XLATE_BOOT_GATE=0 disables it
         * (for experiments); any other value enables it. */
        {
            static int gate = -2;
            if (gate == -2) {
                const char *e = getenv("XLATE_BOOT_GATE");
                /* Under the reset-vector entry there is nothing to hold: the card's
                 * own PRESS RUN loop is the machine's wait, and the disc boot starts
                 * from the card, not from this port's gate. */
                gate = e ? (atoi(e) != 0)
                         : ((c->boot_game_on_run && !c->bios_reset) ? 1 : 0);
            }
            if (gate && !c->boot_gate_open) {
                /* The pad has to be ticked while the CPU is held, or the press
                 * that opens the gate would never be seen (the first version of
                 * this loop did not, and the machine span here until the step
                 * budget ran out). */
                xl_pad_tick(c);
                xl_tick(c, 120000u);
                c->frames = (uint32_t)c->vdc.frame;
                continue;
            }
        }
        if (use_stop_pc && c->pc == stop_pc)
            return XL_STOPPED_AT_PC;
        if (max_cycles && c->cycles >= max_cycles)
            return XL_OUT_OF_STEPS;
        xl_pad_tick(c);             /* timed input, at frame boundaries */
        xl_note_pc(c);              /* first execution per bank */
        /* XLATE_PCTRACE_FROM/TO: every executed pc in a frame window, one line
         * each.  Diagnostic for "what did the machine actually run here"; off
         * unless the two variables are set. */
        {
            static FILE *pf = NULL;
            static int pf_from = -2, pf_to = -2;
            if (pf_from == -2) {
                const char *a = getenv("XLATE_PCTRACE_FROM");
                const char *b = getenv("XLATE_PCTRACE_TO");
                const char *p = getenv("XLATE_PCTRACE");
                pf_from = a ? atoi(a) : -1;
                pf_to = b ? atoi(b) : -1;
                if (p && p[0] && pf_from >= 0)
                    pf = fopen(p, "w");
            }
            if (pf && (int)c->vdc.frame >= pf_from && (pf_to < 0 || (int)c->vdc.frame <= pf_to))
                fprintf(pf, "%u %04X %02X %02X%02X%02X%02X%02X%02X%02X%02X\n",
                        (unsigned)c->vdc.frame, c->pc, c->mpr[c->pc >> 13],
                        c->mpr[0], c->mpr[1], c->mpr[2], c->mpr[3],
                        c->mpr[4], c->mpr[5], c->mpr[6], c->mpr[7]);
        }
        /* Route recorder (XLATE_TRACE=1).  It used to be declared but never
         * written to, so a stop printed "last 0 instruction addresses" -- an
         * instrument that looks installed and reports nothing.  Filled here. */
        if (xl_trace_on) {
            unsigned slot = xl_trace_pos % XL_TRACE_N;
            xl_trace_pc[slot] = c->pc;
            xl_trace_bank[slot] = c->mpr[c->pc >> 13];
            xl_trace_pos++;
        }
        xl_service_irq(c);          /* only between instructions, as the CPU does */
        /* First-execution stamp per address (XLATE_FIRST=[BANK:]ADDR[,...]).
         * The oracle trace names a routine by the first frame it executes it;
         * this is the same measurement on our machine, so the two tables are
         * comparable line by line.  A bare address matches in any bank; a
         * BANK:ADDR pair matches only when that bank is mapped at the address's
         * page -- `$800A` exists in several banks, so the table needs the pair.
         * Prints "FIRST pc=$xxxx bank=$xx frame=N mpr=...". */
        {
            static int first_init;
            static int first_n;
            static unsigned first_pc[16];
            static unsigned first_bank[16];   /* $100 = any bank */
            static char first_seen[16];
            if (!first_init) {
                const char *e = getenv("XLATE_FIRST");
                first_init = 1;
                while (e && *e && first_n < 16) {
                    char *end;
                    unsigned bank = 0x100u;
                    unsigned long v = strtoul(e, &end, 16);
                    if (end == e)
                        break;
                    if (*end == ':') {
                        bank = (unsigned)v;
                        v = strtoul(end + 1, &end, 16);
                    }
                    first_pc[first_n] = (unsigned)v;
                    first_bank[first_n] = bank;
                    first_n++;
                    e = (*end == ',') ? end + 1 : NULL;
                }
            }
            if (first_n) {
                int k;
                for (k = 0; k < first_n; k++)
                    if (!first_seen[k] && first_pc[k] == c->pc &&
                        (first_bank[k] == 0x100u || first_bank[k] == c->mpr[c->pc >> 13])) {
                        first_seen[k] = 1;
                        printf("FIRST pc=%04X bank=%02X frame=%u "
                               "mpr=%02X%02X%02X%02X%02X%02X%02X%02X\n",
                               c->pc, c->mpr[c->pc >> 13], c->vdc.frame,
                               c->mpr[0], c->mpr[1], c->mpr[2],
                               c->mpr[3], c->mpr[4], c->mpr[5], c->mpr[6], c->mpr[7]);
                        fflush(stdout);
                    }
            }
        }
        if (hist_on) {
            static int hf = -2, ht = -1;
            if (hf == -2) {
                const char *a = getenv("XLATE_HIST_FROM");
                const char *b = getenv("XLATE_HIST_TO");
                hf = a ? atoi(a) : -1;
                ht = b ? atoi(b) : -1;
            }
            if (hf < 0 || ((int)c->vdc.frame >= hf && (ht < 0 || (int)c->vdc.frame <= ht)))
                xl_hist[c->pc]++;
        }
        /* A handler that never returns is invisible in a state dump: the PC just
         * looks busy.  Keep a coarse history (every 64th instruction, 128 entries,
         * so ~8000 instructions of route) and dump it the first time a handler has
         * run far longer than a frame without returning. */
        {
            static uint16_t coarse_pc[128];
            static uint8_t coarse_bank[128];
            static unsigned coarse_pos;
            static unsigned long long in_irq_steps;
            static int reported;
            if ((c->steps & 63) == 0) {
                unsigned slot = coarse_pos % 128;
                coarse_pc[slot] = c->pc;
                coarse_bank[slot] = c->mpr[c->pc >> 13];
                coarse_pos++;
            }
            if (c->in_irq)
                in_irq_steps++;
            else
                in_irq_steps = 0;
            if (in_irq_steps == 8000000ull && !reported) {
                unsigned n = coarse_pos < 128 ? coarse_pos : 128, k;
                reported = 1;
                if (c->log) {
                    xl_logf(c, "irq: handler at frame %u has run %llu instructions "
                               "without returning; pc=$%04X bank $%02X status=$%04X "
                               "3A=$%02X 3F31=$%02X",
                            c->vdc.frame, in_irq_steps, c->pc,
                            c->mpr[c->pc >> 13], c->vdc.status,
                            xl_rd_phys(c, XL_WORKRAM_BANK, 0x3A),
                            xl_rd_phys(c, XL_WORKRAM_BANK, 0x3F31));
                    xl_logf(c, "irq: entry had status=$%04X 3A=$%02X from pc=$%04X",
                            xl_irq_entry_status, xl_irq_entry_3a, xl_irq_entry_pc);
                    xl_logf(c, "route into the stuck handler (every 64th instruction):");
                    for (k = 0; k < n; k++) {
                        unsigned slot = (coarse_pos - n + k) % 128;
                        xl_logf(c, "  route: $%04X  bank $%02X",
                                coarse_pc[slot], coarse_bank[slot]);
                    }
                }
            }
        }
        {
            /* $6FD8 entry and the $6FFB read, with the pointer they see. */
            static int on6 = -1;
            if (on6 < 0) {
                const char *e = getenv("XLATE_6FD8");
                on6 = (e && e[0] && e[0] != '0') ? 1 : 0;
            }
            if (on6 && (c->pc == 0x6FD8 || c->pc == 0x6FFB) && c->log)
                xl_logf(c, "%s f=%u $00/$01=%02X%02X $FD=$%02X A=$%02X",
                        c->pc == 0x6FD8 ? "enter6FD8" : "read6FFB",
                        c->vdc.frame,
                        xl_rd(c, (uint16_t)(XL_ZP_BASE | 0x01)),
                        xl_rd(c, (uint16_t)(XL_ZP_BASE | 0x00)),
                        xl_rd(c, (uint16_t)(XL_ZP_BASE | 0xFD)), c->a);
        }
        {
            static int onf = -1;
            if (onf < 0) {
                const char *e = getenv("XLATE_ADC_FD");
                onf = (e && e[0] && e[0] != '0') ? 1 : 0;
            }
            if (onf && c->pc == 0x6FEC && c->log) {
                static int n;
                if (n < 8) {
                    xl_logf(c, "adcfd f=%u $FD(read)=$%02X P=$%02X carry=%d",
                            c->vdc.frame, xl_rd(c, (uint16_t)(XL_ZP_BASE | 0xFD)),
                            c->p, (c->p & XL_C) ? 1 : 0);
                    n++;
                }
            }
        }
        {
            static int on = -1;
            if (on < 0) {
                const char *e = getenv("XLATE_PC442C");
                on = (e && e[0] && e[0] != '0') ? 1 : 0;
            }
            if (on && c->pc == 0x442C && c->log) {
                static uint32_t n;
                if (n < 6) {
                    uint8_t lo = xl_rd(c, (uint16_t)(XL_STACK_BASE | ((c->s + 1) & 0xFF)));
                    uint8_t hi = xl_rd(c, (uint16_t)(XL_STACK_BASE | ((c->s + 2) & 0xFF)));
                    uint16_t ret = (uint16_t)((lo | (hi << 8)) + 1);
                    xl_logf(c, "pc442c f=%u mpr4=$%02X ret=$%04X (bank $%02X) "
                               "A=$%02X X=$%02X Y=$%02X S=$%02X P=$%02X",
                            c->vdc.frame, c->mpr[4], ret, c->mpr[ret >> 13],
                            c->a, c->x, c->y, c->s, c->p);
                }
                n++;
            }
        }
        {
            static int on = -1;
            if (on < 0) {
                const char *e = getenv("XLATE_CLRLOOP");
                on = (e && e[0] && e[0] != '0') ? 1 : 0;
            }
            /* bank $72 $8050: the `beq` after `ldx $17` -- the clear/fill loop's count. */
            if (on && c->pc == 0x8050 && c->mpr[4] == 0x72 && c->log) {
                static uint32_t n;
                if (n < 60 || (n % 2000) == 0)
                    xl_logf(c, "clr f=%u X=%u ($17) mpr0=$%02X mpr1=$%02X "
                               "rd17=$%02X hw17=$%02X wr17=$%02X",
                            c->vdc.frame, c->x, c->mpr[0], c->mpr[1],
                            xl_rd(c, 0x0017), c->hw_regs[0x17],
                            c->mem[(size_t)XL_WORKRAM_BANK * XL_BANK_SIZE + 0x17]);
                n++;
                if (n == 60)
                    xl_logf(c, "clr (further lines sampled every 2000)");
            }
        }
        {
            /* Display-list trace: bank $6C's interpreter fetches an inner opcode at
             * $81CE ('cly; lda ($12),y') and reads the outer job entries at $8169.
             * Logging both, with the pointer and the frame, shows exactly what each
             * job's list contains and how it is decoded. */
            static int dl_from = -2, dl_to = -1;
            if (dl_from == -2) {
                const char *a = getenv("XLATE_DLIST_FROM");
                const char *b = getenv("XLATE_DLIST_TO");
                dl_from = a ? atoi(a) : -1;
                dl_to = b ? atoi(b) : -1;
            }
            if (dl_from >= 0 && c->log && (int)c->vdc.frame >= dl_from &&
                (dl_to < 0 || (int)c->vdc.frame <= dl_to)) {
                if (c->pc == 0x81CE) {
                    uint16_t lp = (uint16_t)(xl_rd(c, 0x2012) | (xl_rd(c, 0x2013) << 8));
                    uint8_t op = xl_rd(c, (uint16_t)(lp + c->y));
                    const char *h = (op == 0x00) ? "$822B fill(low from stream)"
                                  : (op < 0x80)  ? "$820E fill(fixed word)"
                                  : (op == 0xFD) ? "$8266"
                                  : (op == 0xFE) ? "$8247 set VRAM address"
                                  : (op == 0xFF) ? "end" : "$81ED literal run";
                    xl_logf(c, "dlist f=%u op=$%02X at $%04X list=$%04X -> %s",
                            c->vdc.frame, op, (unsigned)(lp + c->y), lp, h);
                } else if (c->pc == 0x8151) {
                    /* The job-parameter lookup: A < $80 selects a 16-bit pointer from
                     * the table at ($9802), indexed by A*2.  Logging A, the mapping
                     * and the table contents shows whether the pointer is resolved
                     * against the right bank. */
                    uint16_t tp = (uint16_t)(xl_rd(c, 0x9802) | (xl_rd(c, 0x9803) << 8));
                    unsigned idx = (unsigned)(c->a << 1);
                    uint16_t got = (uint16_t)(xl_rd(c, (uint16_t)(tp + idx)) |
                                              (xl_rd(c, (uint16_t)(tp + idx + 1)) << 8));
                    xl_logf(c, "lkp  f=%u A=$%02X mpr4=$%02X mpr5=$%02X table=$%04X "
                               "idx=$%02X -> $%04X",
                            c->vdc.frame, c->a, c->mpr[4], c->mpr[5], tp, idx, got);
                } else if ((c->vdc.select & 0x1F) == 0x00) {
                    /* ignored: not a data commit */
                } else if (c->pc == 0x81C7) {
                    /* One block-copy op: `jsr $827F` copies X*32 bytes from the
                     * paged source ($12/$13, banked by the TAM pair the job's cmd
                     * byte selected) to VRAM at MAWR ($16/$17).  One line per op,
                     * so the whole upload reads as a list of (banks, source, dest,
                     * length) instead of one line per 32-byte block. */
                    xl_logf(c, "upl  f=%u cmd_banks=$%02X/$%02X src=$%04X len=%u B "
                               "vram=$%04X",
                            c->vdc.frame, c->mpr[5], c->mpr[6],
                            (unsigned)(xl_rd(c, 0x2012) | (xl_rd(c, 0x2013) << 8)),
                            (unsigned)((c->x ? c->x : 256) * 32u),
                            (unsigned)(xl_rd(c, 0x2016) | (xl_rd(c, 0x2017) << 8)));
                } else if (c->pc == 0x827F) {
                    xl_logf(c, "blk  f=%u count X=%u at vram $%04X (stream $%04X)",
                            c->vdc.frame, c->x, c->vdc.mawr & 0x7FFF,
                            (unsigned)(xl_rd(c, 0x2012) | (xl_rd(c, 0x2013) << 8)));
                } else if (c->pc == 0x8169) {
                    uint16_t lp = (uint16_t)(xl_rd(c, 0x2014) | (xl_rd(c, 0x2015) << 8));
                    uint8_t op = xl_rd(c, (uint16_t)(lp + c->y));
                    xl_logf(c, "job  f=%u entry=$%02X at $%04X list=$%04X "
                               "bank=$%02X vram=$%04X inner=$%04X",
                            c->vdc.frame, op, (unsigned)(lp + c->y), lp,
                            xl_rd(c, 0x2011), (unsigned)(xl_rd(c, 0x2016) |
                            (xl_rd(c, 0x2017) << 8)),
                            (unsigned)(xl_rd(c, 0x2012) | (xl_rd(c, 0x2013) << 8)));
                }
            }
        }
        {
            static int ct_a = -1, ct_b = -1;
            if (ct_a == -2) { ct_a = -1; }
            /* Caller trace for a chosen pair of entry addresses (env XLATE_CALL_TRACE=
             * "8151,8169"): logs the return address sitting on the stack, i.e. exactly
             * which call site started the routine. */
            if (ct_a == -1) {
                const char *e = getenv("XLATE_CALL_TRACE");
                if (e && e[0]) {
                    unsigned v1 = 0, v2 = 0;
                    if (sscanf(e, "%x,%x", &v1, &v2) == 2) { ct_a = (int)v1; ct_b = (int)v2; }
                }
                if (ct_a == -1) ct_a = -2;
            }
            if (c->pc == (uint16_t)ct_a || c->pc == (uint16_t)ct_b) {
                static uint32_t logged;
                if (logged < 40) {
                    uint8_t lo = xl_rd(c, (uint16_t)(XL_STACK_BASE | ((c->s + 1) & 0xFF)));
                    uint8_t hi = xl_rd(c, (uint16_t)(XL_STACK_BASE | ((c->s + 2) & 0xFF)));
                    uint16_t ret = (uint16_t)((lo | (hi << 8)) + 1);
                    logged++;
                    if (c->log)
                        xl_logf(c, "call: $%04X entered at frame %u from $%04X "
                                   "(bank $%02X) with A=$%02X X=$%02X Y=$%02X",
                                c->pc, c->vdc.frame, ret, c->mpr[ret >> 13],
                                c->a, c->x, c->y);
                }
            }
        }
        if (!xl_cpu_step(c))
            return c->untranslated_stop ? XL_UNTRANSLATED : XL_TRAPPED;
        c->steps++;
        if ((uint32_t)c->vdc.frame != c->frames) {
            c->frames = (uint32_t)c->vdc.frame;
            c->vblank_seen = 1;
            {
                static int ph = -1;
                if (ph < 0) {
                    const char *e = getenv("XLATE_PHASE");
                    ph = (e && e[0] && e[0] != '0') ? 1 : 0;
                }
                if (ph && c->log && c->frames >= 1740 && c->frames <= 1910) {
                    unsigned nz = 0, zp = 0, k;
                    const uint8_t *w = &c->mem[(size_t)XL_WORKRAM_BANK * XL_BANK_SIZE];
                    for (k = 0; k < 8192; k++) if (w[k]) nz++;
                    for (k = 0; k < 256; k++) if (w[k]) zp++;
                    xl_logf(c, "phase f=%u nonzero=%u zp=%u", c->frames, nz, zp);
                }
            }
            /* THE PER-FRAME STRUCTURAL LINE, and then a CHECKPOINT if one is due.
             *
             * Both happen here, at the first instruction boundary after the frame
             * counter changed, which is the only instant that is both "start of frame
             * N" (what every reference block is sampled at) and safe to snapshot (no
             * instruction half-executed: that is what makes a checkpoint resumable
             * exactly -- see xl_ckpt_save). */
            xl_framelog_maybe(c);
            xl_satstream_maybe(c);
            /* XLATE_PORTSTATE_*: a full state capture at anchored checkpoints. */
            xl_portstate_maybe(c);
            {
                static long every = -1;
                static uint32_t next = 0;
                static char ckdir[512];
                static int init = 0;
                if (!init) {
                    const char *e = getenv("XLATE_CKPT_EVERY");
                    const char *d = getenv("XLATE_CKPT_DIR");
                    init = 1;
                    every = (e && e[0]) ? atol(e) : 0;
                    if (d) snprintf(ckdir, sizeof ckdir, "%s", d);
                    if (every > 0)
                        next = ((c->frames / (uint32_t)every) + 1u) * (uint32_t)every;
                }
                if (every > 0 && c->frames >= next) {
                    char path[1200];
                    int e2;
                    snprintf(path, sizeof path, "%s/ckpt-f%06u.ckpt",
                             ckdir[0] ? ckdir : ".", (unsigned)c->frames);
                    e2 = xl_ckpt_save(c, path);
                    if (c->log) {
                        if (e2 == 0)
                            xl_logf(c, "ckpt: frame %u -> %s", c->frames, path);
                        else
                            xl_logf(c, "ckpt: frame %u FAILED (%d) -> %s",
                                    c->frames, e2, path);
                    }
                    printf("CKPT frame=%u status=%d path=%s\n",
                           (unsigned)c->frames, e2, path);
                    fflush(stdout);
                    next += (uint32_t)every;
                }
            }
            {
                /* HYPOTHESIS TEST, not a fix: issue the console's own read schedule
                 * (sector, count, frame) with the payload DISCARDED -- no CD RAM write.
                 * The destination is still unknown; this tests only whether a completed
                 * read is the signal the game's job loop waits for.
                 * Active only in the booted configuration (XLATE_BOOT_GATE set), so
                 * injected runs are untouched by construction. */
                static const struct { unsigned short frame, sector, count; } sched[] = {
                    {1701,4082,2},{1707,4084,1},{1711,3957,10},{1723,3967,12},
                    {1738,3979,8},{1748,4083,8},{1759,4091,12},{1773,4103,12},
                    {1787,4115,8},{1798,4051,12},{1812,4063,12},{1826,4075,8},
                    {1846,3987,12},{1860,3999,12},{1874,4011,12},{1889,4023,12},
                    {1903,4035,8},{1913,4043,8},{1924,4127,32},
                };
                static int sched_on = -1;
                if (sched_on < 0) {
                    const char *e = getenv("XLATE_CDSCHED");
                    sched_on = (e && e[0] && e[0] != '0') ? 1 : 0;
                }
                if (sched_on) {
                    unsigned k;
                    for (k = 0; k < sizeof sched / sizeof sched[0]; k++) {
                        if (sched[k].frame != c->frames)
                            continue;
                        /* Discard the payload deliberately: touch the disc image so the
                         * read is real in timing and addressing, but write nothing. */
                        {
                            size_t off = (size_t)sched[k].sector * 2048u;
                            if (c->disc && off + 2048u <= c->disc_len)
                                (void)c->disc[off];
                        }
                        /* Present a completion the way a real transfer would: the port
                         * reports data ready, and the interrupt latch is raised. */
                        /* No interrupt: raising one here derailed the boot (see log).
                         * The port state records the completion instead. */
                        c->cd.sectors_queued = sched[k].count;
                        if (c->log)
                            xl_logf(c, "cdsched: read sector %u x%u at frame %u "
                                       "(payload discarded, destination unknown)",
                                    sched[k].sector, sched[k].count, c->frames);
                    }
                }
            }
            /* THE HAND-OFF IS AN EVENT, NOT A FRAME.  After RUN the boot program
             * runs its own start-up ($413E-$4223: clear loop, palette upload, VDC
             * init) and then enters its steady loop at $41E9, which is where it waits
             * for the BIOS to transfer control to the kernel.  That is the moment our
             * synthetic hand-off stands in for, so it is the moment to fire: pc $41E9
             * in bank $80, reached after the boot program's own work.  Any change that
             * alters how long the boot takes -- disc read time, for one -- now moves
             * the game's start, which is the falsifiable property the frame counter
             * did not have. */
            if (!c->bios_reset && c->boot_pressed && c->pc == 0x41E9 && c->mpr[2] == 0x80) {
                c->boot_pressed = 0;
                if (c->log)
                    xl_logf(c, "boot: boot program reached $41E9 at frame %u "
                               "-> handing over", (uint32_t)c->vdc.frame);
                xl_boot_game(c);
            }
            /* Optional frame series with its scene state, so a comparison can pick
             * the frame at the same moment in the game instead of the same number.
             * XLATE_FRAME_EVERY=50 XLATE_FRAME_DIR=/tmp/frames */
            {
                static int every = -2;
                static long win_from = -1, win_to = -1;
                if (every == -2) {
                    const char *e = getenv("XLATE_FRAME_EVERY");
                    const char *a = getenv("XLATE_FRAME_FROM");
                    const char *b = getenv("XLATE_FRAME_TO");
                    every = e ? atoi(e) : 0;
                    /* XLATE_FRAME_FROM/TO: dump only the frames a comparison window
                     * needs.  A 2,000-frame segment measured from a cold boot would
                     * otherwise write every frame of the route before it -- 200 kB a
                     * frame of pictures nobody reads. */
                    win_from = a ? atol(a) : -1;
                    win_to = b ? atol(b) : -1;
                }
                if (every > 0 && (c->vdc.frame % every) == 0 &&
                    (win_from < 0 || (long)c->vdc.frame >= win_from) &&
                    (win_to < 0 || (long)c->vdc.frame < win_to)) {
                    const char *dir = getenv("XLATE_FRAME_DIR");
                    char path[512];
                    snprintf(path, sizeof path, "%s/f%05u.ppm",
                             dir ? dir : ".", (unsigned)c->vdc.frame);
                    {
                        FILE *fp = fopen(path, "wb");
                        if (fp) { xl_write_ppm(c, fp); fclose(fp); }
                    }
                    if (c->log)
                        xl_logf(c, "series: frame %u 31=%02X 37=%02X 45=%02X "
                                   "3F83=%02X 3F84=%02X cr=%04X mwr=%04X "
                                   "tasks=%02X%02X%02X%02X%02X%02X%02X%02X "
                                   "params=%02X%02X%02X%02X%02X%02X%02X%02X",
                                c->vdc.frame, xl_rd(c, 0x2031), xl_rd(c, 0x2037),
                                xl_rd(c, 0x2045), xl_rd(c, 0x0F83), xl_rd(c, 0x0F84),
                                c->vdc.cr, c->vdc.mwr,
                                xl_rd(c, 0x3C87), xl_rd(c, 0x3C86), xl_rd(c, 0x3C85),
                                xl_rd(c, 0x3C84), xl_rd(c, 0x3C83), xl_rd(c, 0x3C82),
                                xl_rd(c, 0x3C81), xl_rd(c, 0x3C80),
                                xl_rd(c, 0x3C8F), xl_rd(c, 0x3C8E), xl_rd(c, 0x3C8D),
                                xl_rd(c, 0x3C8C), xl_rd(c, 0x3C8B), xl_rd(c, 0x3C8A),
                                xl_rd(c, 0x3C89), xl_rd(c, 0x3C88));
                }
            }
            {
                static int stop_frame = -2;
                if (stop_frame == -2) {
                    const char *e = getenv("XLATE_STOP_FRAME");
                    stop_frame = e ? atoi(e) : -1;
                }
                if (stop_frame >= 0 && (int)c->vdc.frame >= stop_frame)
                    return XL_STOPPED_AT_PC;
            }
            /* XLATE_PORTSTATE_STOP: the last checkpoint has been written; do not
             * spend another minute of emulation nobody will read. */
            if (xl_portstate_done())
                return XL_STOPPED_AT_PC;
            /* Scene-state alignment for comparison against a savestate: stop the
             * machine the moment the work-RAM scene variables match a wanted triple,
             * so the frame we capture is the same *moment in the game* rather than
             * the same frame number.  XLATE_STOP_STATE=31:37:45 in hex. */
            {
                static int st_a = -2, st_b = 0, st_c = 0;
                if (st_a == -2) {
                    const char *e = getenv("XLATE_STOP_STATE");
                    st_a = -1;
                    if (e && e[0]) {
                        unsigned v1 = 0, v2 = 0, v3 = 0;
                        if (sscanf(e, "%x:%x:%x", &v1, &v2, &v3) == 3) {
                            st_a = (int)(v1 & 0xFF);
                            st_b = (int)(v2 & 0xFF);
                            st_c = (int)(v3 & 0xFF);
                        }
                    }
                }
                if (st_a >= 0 && c->frames > 1700 &&
                    xl_rd(c, 0x2031) == st_a &&
                    xl_rd(c, 0x2037) == st_b &&
                    xl_rd(c, 0x2045) == st_c) {
                    if (c->log)
                        xl_logf(c, "stopped at the scene state %02X:%02X:%02X at frame %u",
                                st_a, st_b, st_c, c->vdc.frame);
                    return XL_STOPPED_AT_PC;
                }
            }
        }
    }
    return XL_OUT_OF_STEPS;
}

/* ---------------------------------------------------------------- checkpoints --
 * xl_ckpt_save / xl_ckpt_load: the machine's ENTIRE state as one opaque image.
 *
 * Why not the XLST1 layout the console savestates use: XLST1 carries registers, the
 * VDC register file, the palette, VRAM, SAT, CD RAM and work RAM -- and nothing else.
 * It is a *console* savestate, and `xl_load_state_file` has to rebuild the parts a
 * console savestate does not carry (the raster position, the chip's display cache) by
 * refusing to move the machine at all: it puts the raster back at the frame boundary
 * and lets frame 0 render from injected memory.
 *
 * A checkpoint taken from this port mid-run cannot be resumed that way.  The instant
 * it is written is an instruction boundary somewhere inside a frame, and the machine
 * at that instant owns: the CPU's cycle total, the VDC's raster position and its
 * pending-write/read queues and latched shadows, the palette's display cache, the
 * drive's sector countdown and in-flight SCSI phase, the timer, the interrupt latch,
 * the pad window table, the backup RAM, the boot-gate flags and the frame counter.
 * Reconstructing that list is a second, silently-diverging state format; dumping the
 * struct is one image that cannot drift from the machine it came from.
 *
 * It is therefore a private format, tagged with sizeof(Cpu) and an ABI number: an
 * image from a build whose struct differs is REJECTED, never half-read.  It is written
 * to `<path>.tmp` and renamed, so a checkpoint interrupted by a crash is absent rather
 * than truncated -- an image that loads and is subtly wrong is the one failure this
 * whole exercise exists to prevent.
 *
 * Pointers (log, disc) are not state and are re-attached from the live machine on load. */
#define XL_CKPT_ABI 1u

int xl_ckpt_save(Cpu *c, const char *path)
{
    static const char magic[8] = { 'X','C','K','P','T','1',0,0 };
    uint8_t hdr[24];
    Cpu *tmp;
    FILE *f;
    char tmpath[1200];
    uint32_t sz = (uint32_t)sizeof(Cpu), abi = XL_CKPT_ABI;
    uint32_t fr = (uint32_t)c->vdc.frame, zero = 0;
    if (strlen(path) + 5 >= sizeof tmpath)
        return -1;
    memcpy(hdr, magic, 8);
    memcpy(hdr + 8, &sz, 4);
    memcpy(hdr + 12, &abi, 4);
    memcpy(hdr + 16, &fr, 4);
    memcpy(hdr + 20, &zero, 4);
    tmp = malloc(sizeof *tmp);              /* the struct is over a megabyte */
    if (!tmp)
        return -2;
    *tmp = *c;
    tmp->log = NULL;
    tmp->disc = NULL;
    tmp->disc_len = 0;
    tmp->disc_attached = 0;
    snprintf(tmpath, sizeof tmpath, "%s.tmp", path);
    f = fopen(tmpath, "wb");
    if (!f) {
        free(tmp);
        return -3;
    }
    if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr ||
        fwrite(tmp, sizeof *tmp, 1, f) != 1) {
        fclose(f);
        remove(tmpath);
        free(tmp);
        return -4;
    }
    if (fclose(f) != 0) {
        remove(tmpath);
        free(tmp);
        return -4;
    }
    free(tmp);
    if (rename(tmpath, path) != 0) {
        remove(tmpath);
        return -5;
    }
    return 0;
}

int xl_ckpt_load(Cpu *c, const char *path)
{
    static const char magic[8] = { 'X','C','K','P','T','1',0,0 };
    uint8_t hdr[24];
    Cpu *tmp;
    FILE *f = fopen(path, "rb");
    uint32_t sz = 0, abi = 0;
    if (!f)
        return -1;
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || memcmp(hdr, magic, 8) != 0) {
        fclose(f);
        return -2;
    }
    memcpy(&sz, hdr + 8, 4);
    memcpy(&abi, hdr + 12, 4);
    if (abi != XL_CKPT_ABI || sz != (uint32_t)sizeof(Cpu)) {
        fclose(f);
        return -3;                     /* a different build's layout: refuse it */
    }
    tmp = malloc(sizeof *tmp);
    if (!tmp) {
        fclose(f);
        return -4;
    }
    if (fread(tmp, sizeof *tmp, 1, f) != 1) {
        fclose(f);
        free(tmp);
        return -5;
    }
    fclose(f);
    tmp->log = c->log;
    tmp->disc = c->disc;
    tmp->disc_len = c->disc_len;
    tmp->disc_attached = c->disc_attached;
    *c = *tmp;
    free(tmp);
    return 0;
}

/* Load a state created by tools/mkstate.py: registers, VDC register file, palette,
 * VRAM, SAT, the 24 CD RAM banks and the 8 KB work RAM.  Returns 0 on success. */
int xl_load_state_file(Cpu *c, const char *path)
{
    static const char magic[8] = { 'X','L','S','T','1',0,0,0 };
    uint8_t hdr[8];
    uint16_t regs[12];
    int i;
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fread(hdr, 1, 8, f) != 8 || memcmp(hdr, magic, 8) != 0) {
        fclose(f);
        return -2;
    }
    if (fread(&c->pc, 2, 1, f) != 1) { fclose(f); return -3; }
    c->reset_resume = 0;      /* the state's pc is the machine's, not a pending reset */
    if (fread(&c->a, 1, 1, f) != 1) { fclose(f); return -3; }
    if (fread(&c->x, 1, 1, f) != 1) { fclose(f); return -3; }
    if (fread(&c->y, 1, 1, f) != 1) { fclose(f); return -3; }
    if (fread(&c->s, 1, 1, f) != 1) { fclose(f); return -3; }
    if (fread(&c->p, 1, 1, f) != 1) { fclose(f); return -3; }
    if (fread(c->mpr, 1, 8, f) != 8) { fclose(f); return -3; }
    if (fread(regs, 2, 12, f) != 12) { fclose(f); return -3; }
    c->vdc.mwr = regs[0];  c->vdc.cr  = regs[1];  c->vdc.bxr = regs[2];
    c->vdc.byr = regs[3];  c->vdc.hsr = regs[4];  c->vdc.hdr = regs[5];
    c->vdc.vsr = regs[6];  c->vdc.vdr = regs[7];  c->vdc.vcr = regs[8];
    c->vdc.dcr = regs[9];  c->vdc.satb = regs[10]; c->vdc.rcr = regs[11];
    if (fread(c->vce.pal, 2, 512, f) != 512) { fclose(f); return -4; }
    /* The image carries the palette TABLE, not the chip's display cache; rebuild
     * the cache the way the chip would (VCE::FixPCache over all 512 entries). */
    vce_fix_all(&c->vce);
    for (i = 0; i < VD_VRAM_WORDS; i++) {
        uint8_t b[2];
        if (fread(b, 1, 2, f) != 2) { fclose(f); return -5; }
        c->vdc.vram[i] = (uint16_t)(b[0] | (b[1] << 8));
    }
    for (i = 0; i < VD_SAT_WORDS; i++) {
        uint8_t b[2];
        if (fread(b, 1, 2, f) != 2) { fclose(f); return -6; }
        c->vdc.sat[i] = (uint16_t)(b[0] | (b[1] << 8));
    }
    /* The oracle's SysCardRAM is the 24 CD RAM banks starting at PHYSICAL bank $68.
     * XL_CDRAM_BASE is $80 -- the base the BIOS's CD-read destination uses -- so it must
     * not be used here: doing so wrote the whole 192 KB 24 banks too high and left
     * $68-$7F zero. */
    if (fread(&c->mem[(size_t)0x68 * XL_BANK_SIZE], 1,
              (size_t)24 * XL_BANK_SIZE, f) != (size_t)24 * XL_BANK_SIZE) {
        fclose(f);
        return -7;
    }
    if (fread(&c->mem[(size_t)XL_WORKRAM_BANK * XL_BANK_SIZE], 1,
              XL_BANK_SIZE, f) != XL_BANK_SIZE) {
        fclose(f);
        return -8;
    }
    /* The console's second 64 KB of CD memory, physical banks $80-$87.  It is a
     * separate device from the 192 KB at $68-$7F (oracle huc.cpp: `CDRAM` vs
     * `SysCardRAM`, both in the save state, neither a mirror of the other).  The
     * stage code keeps sprite patterns here, so an image without it renders the
     * background correctly and no characters at all.
     *
     * Older images end before this field; they load with those banks as they were
     * (zero), which is exactly what the previous loader did. */
    {
        uint8_t *cdram = &c->mem[(size_t)XL_CDRAM_BASE * XL_BANK_SIZE];
        size_t got = fread(cdram, 1, (size_t)8 * XL_BANK_SIZE, f);
        if (got != 0 && got != (size_t)8 * XL_BANK_SIZE) {
            fclose(f);
            return -9;
        }
    }
    /* TRAILING BLOCKS.  Everything after the fixed layout is a sequence of
     * magic-tagged blocks, each one optional and self-describing, so the layout can
     * grow without a version number and without a reader having to know the newest
     * tag: an image that ends here (every image written before 2026-10-29) loads
     * exactly as it did, and an image whose next block this reader does not know
     * stops there instead of misreading it as another block.
     *
     *   XDEV1  the drive's in-flight state (mkstate.py, 2026-10-29)
     *   XBRM1  the machine's 2 KB backup RAM, the save card (mkstate.py, 2026-10-30)
     *
     * WITHOUT XDEV1 A MID-READ STATE CANNOT BE CONTINUED: the card's own reader at
     * ROM $E95E polls $1800 for $D0/$98/$88/$80 and a drive that answers "bus free"
     * leaves it spinning for ever -- measured, 2,000 frames at pc $E95E-$E974, on
     * the console's frame 28,200 state.
     *
     * WITHOUT XBRM1 THE SAVE CARD IS THE POWER-ON ONE (formatted, no files), so the
     * card's `bm_read` of the game's save file answers "no such file" and the game's
     * `$3F83` never sets -- the RUN press on the title screen is then dropped.  That
     * was the wall at console 39,017: not a CD reply and not $F8E3's arithmetic, but
     * an input the image did not carry. */
    for (;;) {
        static const uint8_t dmagic[8] = { 'X','D','E','V','1',0,0,0 };
        static const uint8_t bmagic[8] = { 'X','B','R','M','1',0,0,0 };
        uint8_t tag[8];
        if (fread(tag, 1, 8, f) != 8)
            break;                          /* end of file: nothing more */
        if (memcmp(tag, dmagic, 8) == 0) {
            uint8_t dev[32];
            Cd *cd = &c->cd;
            uint32_t lba, cnt, pos;
            uint64_t delta;
            if (fread(dev, 1, sizeof dev, f) != sizeof dev) {
                fclose(f);
                return -9;
            }
            memcpy(&lba, dev + 12, 4);
            memcpy(&cnt, dev + 16, 4);
            memcpy(&pos, dev + 20, 4);
            memcpy(&delta, dev + 24, 8);
            cd->phase = dev[0];
            cd->data_kind = dev[1];
            cd->data_have = dev[2];
            cd->req = dev[3];
            cd->port3 = dev[4];
            cd->irq_mask = dev[5];
            cd->adpcm_lastcmd = dev[6];
            cd->adpcm_half = dev[7];
            cd->adpcm_end = dev[8];
            cd->cdda_playing = dev[9];
            cd->status_byte = dev[10];
            cd->read_lba = lba;
            cd->read_count = cnt;
            cd->read_served = 0;
            cd->data_pos = pos;
            /* The countdown is in MASTER clocks (3 per CPU cycle) and the image is
             * taken at the injection instant, so `cycles` is 0 here.  It is stored
             * as a delta, not an absolute, for that reason. */
            cd->next_fill = XL_CD_M3(c->cycles) + delta;
            cd->read_start = XL_CD_M3(c->cycles);
            cd->read_frame = (uint32_t)c->vdc.frame;
            /* The image carries the transfer, not the head's absolute position.
             * A development image is resumed mid-dialogue far more often than
             * mid-load, and the seek model only reads `head_lba` when a Read(6) is
             * accepted: taking the head to be at the transfer's first sector costs
             * at most one wrong seek on the first read after the injection, and
             * never invents data.  Recorded so it is not mistaken for a model. */
            cd->head_lba = cd->read_lba;
            if (cd->data_kind == CD_DATA_SECTORS && pos < 2048u) {
                /* The sector on the bus is the disc's own; reload it rather than
                 * carrying 2 KB of copy in the image.  A miss leaves the buffer as
                 * it is and the byte read is simply wrong -- it never silently
                 * invents a different sector. */
                if (c->disc_attached && cd->read_lba >= XL_DISC_LBA0) {
                    size_t off = (size_t)(cd->read_lba - XL_DISC_LBA0) * 2048u;
                    if (off + 2048u <= c->disc_len)
                        memcpy(cd->buf, c->disc + off, 2048u);
                }
                cd->db = cd->buf[cd->data_pos];
            }
            if (c->log)
                xl_logf(c, "state: drive block applied -- phase %u kind %u lba %u "
                           "count %u pos %u next +%llu master clocks",
                        cd->phase, cd->data_kind, cd->read_lba, cd->read_count,
                        cd->data_pos, (unsigned long long)delta);
            continue;
        }
        if (memcmp(tag, bmagic, 8) == 0) {
            uint8_t card[XL_BRAM_SIZE];
            if (fread(card, 1, sizeof card, f) != sizeof card) {
                fclose(f);
                return -10;
            }
            /* The device, not a byte array: the card comes up write-protected on a
             * real machine and the card's own `bm_open` ($FC49) is what unlocks it,
             * so loading the battery contents must not also unlock it. */
            xl_load_sram(c, card, sizeof card);
            c->bram_wprot = 1;
            if (c->log) {
                /* Say what is on the card in the card's own terms -- the BM
                 * directory at $0010 is `len(2), name(12)`, next entry at
                 * off+len -- so "the save file is there" is a fact in the log and
                 * not an inference from a later branch. */
                unsigned off = 0x10, n = 0;
                while (off + 16u <= XL_BRAM_SIZE && n < 4) {
                    unsigned len = (unsigned)c->bram[off] |
                                   ((unsigned)c->bram[off + 1] << 8);
                    if (!len)
                        break;
                    xl_logf(c, "state: card record %u at $%04X len %u name "
                               "'%.12s'", n, off, len, (const char *)&c->bram[off + 4]);
                    if (len < 4u || off + len > XL_BRAM_SIZE)
                        break;
                    off += len;
                    n++;
                }
                if (!n)
                    xl_logf(c, "state: card is formatted but holds no records");
            }
            continue;
        }
        break;                              /* unknown block: stop here */
    }
    fclose(f);
    /* The interrupt mask ($1402) is a REGISTER, not work RAM, so the state file does
     * not carry it and this loader left it at xl_reset()'s default of $07 -- every
     * source masked.  The console at these captures demonstrably takes one vblank
     * interrupt per frame, and its own trace shows $1402 = $00 (inverted: nothing
     * masked) from frame 0 through frame 3,700, so restore that and record that it is
     * inferred from observed behaviour rather than read out of the file. */
    c->irq_mask = 0x00;
    c->irq_status = 0x00;
    c->in_irq = 0;
    /* An injected console savestate is a RUNNING machine, so its drive is at speed:
     * the spin-up contract at cd_dispatch() is a power-on fact and would otherwise
     * answer NOT READY to the first command of a state taken hours into the run.
     * (The image's own drive phase, when it carries one, is restored above.) */
    c->cd.ready_at = 0;
    return 0;
}

void xl_write_ppm(Cpu *c, FILE *f)
{
    fprintf(f, "P6\n%d %d\n255\n", VD_W, VD_H);
    fwrite(c->vdc.frame_pixels, 1, (size_t)VD_W * VD_H * 3, f);
}

/* Dump the video device exactly like tools/state_dump.py dumps a savestate, so the
 * same renderer and the same comparisons apply to a frame our code produced. */
void xl_dump_vdc_state(Cpu *c, FILE *f)
{
    static const char magic[8] = { 'X', 'L', 'V', 'D', 'C', '1', 0, 0 };
    uint16_t regs[12];
    int i;
    regs[0] = c->vdc.mwr; regs[1] = c->vdc.cr; regs[2] = c->vdc.bxr;
    regs[3] = c->vdc.byr; regs[4] = c->vdc.hsr; regs[5] = c->vdc.hdr;
    regs[6] = c->vdc.vsr; regs[7] = c->vdc.vdr; regs[8] = c->vdc.vcr;
    regs[9] = c->vdc.dcr; regs[10] = c->vdc.satb; regs[11] = c->vdc.rcr;
    fwrite(magic, 1, 8, f);
    for (i = 0; i < 12; i++) {
        uint8_t b[2];
        b[0] = (uint8_t)(regs[i] & 0xFF);
        b[1] = (uint8_t)(regs[i] >> 8);
        fwrite(b, 1, 2, f);
    }
    for (i = 0; i < 512; i++) {
        uint8_t b[2];
        b[0] = (uint8_t)(c->vce.pal[i] & 0xFF);
        b[1] = (uint8_t)(c->vce.pal[i] >> 8);
        fwrite(b, 1, 2, f);
    }
    for (i = 0; i < VD_VRAM_WORDS; i++) {
        uint8_t b[2];
        b[0] = (uint8_t)(c->vdc.vram[i] & 0xFF);
        b[1] = (uint8_t)(c->vdc.vram[i] >> 8);
        fwrite(b, 1, 2, f);
    }
    for (i = 0; i < VD_SAT_WORDS; i++) {
        uint8_t b[2];
        b[0] = (uint8_t)(c->vdc.sat[i] & 0xFF);
        b[1] = (uint8_t)(c->vdc.sat[i] >> 8);
        fwrite(b, 1, 2, f);
    }
    fflush(f);
}

void xl_dump_state(Cpu *c, FILE *out)
{
    fprintf(out, "STATE a=%02X x=%02X y=%02X s=%02X p=%02X pc=%04X "
                 "mpr=%02X%02X%02X%02X%02X%02X%02X%02X steps=%lu "
                 "crc=%08X bios=%lu cycles=%llu frames=%d irq=%lu vblank=%lu\n",
            c->a, c->x, c->y, c->s, c->p, c->pc,
            c->mpr[0], c->mpr[1], c->mpr[2], c->mpr[3],
            c->mpr[4], c->mpr[5], c->mpr[6], c->mpr[7],
            c->steps, xl_crc_phys(c), c->bios_calls,
            (unsigned long long)c->cycles, c->vdc.frame, (unsigned long)c->irq_taken,
            (unsigned long)c->vdc.vblank_irqs);
    fflush(out);
}
