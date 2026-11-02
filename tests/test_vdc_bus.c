/* test_vdc_bus.c -- the VDC data port as the chip actually drives it.
 *
 * Three rules are checked here, each one a rule about the chip and not about any
 * particular screen:
 *
 *   1. READING A WORD TAKES ONE INCREMENT.  The chip prefetches a whole word into
 *      its read buffer; the low-byte read returns that word's low byte and the
 *      high-byte read returns its high byte, and only the high-byte read starts
 *      the next prefetch.  A model that increments on the low byte hands back one
 *      half of this word and one half of the next.
 *
 *   2. A WRITE ABOVE 32K IS DISCARDED.  Only 32,768 words are connected, so a
 *      write at $8000+ goes nowhere.  It must NOT be wrapped down to the bottom of
 *      VRAM: word 0 is the background map, and a masked write silently corrupts it.
 *      The write pointer still moves.
 *
 *   3. AN UNDRIVEN READ RETURNS THE RETAINED BUS WORD.  A read at $8000+ leaves
 *      VRAM alone and returns the last word the bus held; it does not wrap either.
 *
 * Built and run by tests/test_video.py; exits non-zero if any check fails.
 * Implemented by the reference core in hw_video/huc6270/vdc.cpp (case 0x01,
 * case 0x02, case 0x3).
 */
#include "video.h"

#include <stdio.h>

static int failures;

static void want(int cond, const char *what, unsigned got, unsigned expect)
{
    if (cond)
        return;
    printf("FAIL  %s: got $%04X, want $%04X\n", what, got, expect);
    failures++;
}

/* Write a VDC register through the data ports, the way a program does. */
static void set_reg(Vdc *v, unsigned sel, unsigned val)
{
    vdc_write(v, 0x0000, (uint8_t)sel);
    vdc_write(v, 0x0002, (uint8_t)(val & 0xFF));
    vdc_write(v, 0x0003, (uint8_t)(val >> 8));
}

/* Read the 16-bit VRAM read register: low byte then high byte. */
static unsigned read_vrr(Vdc *v)
{
    unsigned lo, hi;
    vdc_write(v, 0x0000, 0x02);            /* select the VRAM data register */
    lo = vdc_read(v, 0x0002);
    hi = vdc_read(v, 0x0003);
    return (hi << 8) | lo;
}

int main(void)
{
    Vdc v;
    unsigned lo, hi;

    /* ---- 1. one word, two byte reads, one increment --------------------- */
    vdc_reset(&v);
    set_reg(&v, 0x00, 0x0100);             /* MAWR = $0100 */
    set_reg(&v, 0x02, 0x1234);             /* VRAM[$0100] = $1234 */
    set_reg(&v, 0x00, 0x0101);
    set_reg(&v, 0x02, 0x5678);             /* VRAM[$0101] = $5678 */
    set_reg(&v, 0x01, 0x0100);             /* MARR = $0100: starts a prefetch */
    want(v.marr == 0x0101, "MARR advances one word on the prefetch",
         v.marr, 0x0101);

    vdc_write(&v, 0x0000, 0x02);
    lo = vdc_read(&v, 0x0002);
    want(lo == 0x34, "low byte of the prefetched word", lo, 0x34);
    want(v.marr == 0x0101, "the low-byte read does NOT advance MARR",
         v.marr, 0x0101);
    hi = vdc_read(&v, 0x0003);
    want(hi == 0x12, "high byte of the SAME word", hi, 0x12);
    want(v.marr == 0x0102, "the high-byte read advances MARR by one word",
         v.marr, 0x0102);

    /* The next word reads back as itself, not as half of its neighbour. */
    vdc_write(&v, 0x0000, 0x02);
    (void)vdc_read(&v, 0x0002);
    hi = vdc_read(&v, 0x0003);
    want(hi == 0x56, "the NEXT word, whole, as its own bytes", hi, 0x56);
    want(v.marr == 0x0103, "and the pointer has reached $0103", v.marr, 0x0103);

    /* With the 32-word increment, two reads still move one step, not two. */
    vdc_reset(&v);
    set_reg(&v, 0x05, 0x0800);             /* CR bits 11-12 = 01 -> increment 32 */
    set_reg(&v, 0x01, 0x0040);             /* prefetch; MARR -> $0060 */
    want(v.marr == 0x0060, "increment 32 applies once per word", v.marr, 0x0060);
    vdc_write(&v, 0x0000, 0x02);
    (void)vdc_read(&v, 0x0002);
    want(v.marr == 0x0060, "still one step after the low byte", v.marr, 0x0060);
    (void)vdc_read(&v, 0x0003);
    want(v.marr == 0x0080, "two steps after the word", v.marr, 0x0080);

    /* ---- 2. a write above 32K is discarded, not wrapped ------------------ */
    vdc_reset(&v);
    v.vram[0] = 0xBEEF;                    /* the background map's first word */
    set_reg(&v, 0x00, 0x8000);             /* MAWR = $8000 */
    set_reg(&v, 0x02, 0xFFFF);             /* write a word the chip cannot reach */
    want(v.vram[0] == 0xBEEF, "a write at $8000 must not land on VRAM word 0",
         v.vram[0], 0xBEEF);
    want(v.mawr == 0x8001, "the write pointer still advances", v.mawr, 0x8001);

    /* ---- 3. an undriven read returns the retained bus word --------------- */
    vdc_reset(&v);
    v.vram[0x0005] = 0x1234;
    v.vram[0x0006] = 0x4321;               /* the prefetch that follows the read */
    v.vram[0x0001] = 0x0000;               /* a wrap $8005 -> $0005 would show $1234 */
    set_reg(&v, 0x01, 0x0005);             /* prefetch VRAM[$0005] */
    lo = read_vrr(&v);                     /* reading the word also fetches $0006 */
    want(lo == 0x1234, "a driven read returns its word", lo, 0x1234);
    set_reg(&v, 0x01, 0x8005);             /* prefetch an address nothing drives */
    lo = read_vrr(&v);
    want(lo == 0x4321, "an undriven read retains the last driven word",
         lo, 0x4321);
    want(v.marr == 0x8007, "the pointer still wraps and advances there too",
         v.marr, 0x8007);

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("vdc bus: prefetch, one increment per word, upper half ignored\n");
    return 0;
}
