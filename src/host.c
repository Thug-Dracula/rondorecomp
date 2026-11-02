/* host.c -- headless driver for statically recompiled HuC6280 programs.
 *
 * No window, no SDL, no input devices: this reads files, runs translated code,
 * and prints machine-readable lines that the Python tests compare against.
 *
 * Modes:
 *   host run [options]
 *       run the program from xlate_init_state()
 *   host cases            (only if built with -DXLATE_WITH_CASES)
 *       run every generated random-test case once and print its final state
 *
 * Options for `run`:
 *   --disc PATH     attach the data track (default ../inputs/data_payload.bin)
 *   --bios PATH     attach the CD BIOS ROM (for $FFF5 etc.)
 *   --steps N       step budget (default 200000)
 *   --stop-pc ADDR  stop when PC reaches ADDR (before executing it)
 *   --press FROM:TO:MASK / --pressfile PATH  timed pad windows
 *   --resets F1,F2,... / --resetfile PATH  a console RESET at the top of each frame
 *   --trace         print each PC as it is executed
 *   --hw            print a hardware-register write summary
 */
#include "runtime.h"

#include <stdlib.h>
#include <string.h>

static Cpu cpu;

#ifndef XLATE_WITH_CASES
static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (uint8_t *)malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void print_hw_summary(void)
{
    int i, shown = 0;
    printf("HW summary:");
    for (i = 0; i < XL_BANK_SIZE && shown < 16; i++) {
        if (cpu.hw_writes[i]) {
            printf(" [$%04X]=%ux", i, (unsigned)cpu.hw_writes[i]);
            shown++;
        }
    }
    if (!shown)
        printf(" (no register writes)");
    printf("\n");
}

#endif /* !XLATE_WITH_CASES */

/* Load a dumped video state (see tools/state_dump.py) into our device model and
 * write one frame as a binary PPM.  This is how the renderer is compared against
 * the emulator's own screenshot for the same frame. */
#define VDC_BLOB_MAGIC "XLVDC1"
static int render_state_mode(int argc, char **argv)
{
    const char *state = NULL, *out = NULL;
    int zero_vds = 0, show = 0;
    int i;
    FILE *f;
    uint8_t *blob;
    long n;
    static Vdc vdc;
    static Vce vce;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--state") == 0 && i + 1 < argc) state = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out = argv[++i];
        else if (strcmp(argv[i], "--zero-vds") == 0) zero_vds = 1;

        else if (strcmp(argv[i], "--info") == 0) show = 1;
        else { fprintf(stderr, "render: unknown option %s\n", argv[i]); return 64; }
    }
    if (!state || !out) {
        fprintf(stderr, "usage: host render --state BLOB --out FRAME.ppm "
                        "[--zero-vds] [--info]\n");
        return 64;
    }
    f = fopen(state, "rb");
    if (!f) { fprintf(stderr, "render: cannot open %s\n", state); return 64; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    blob = (uint8_t *)malloc((size_t)n);
    if (!blob || fread(blob, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "render: short read\n");
        fclose(f);
        return 64;
    }
    fclose(f);
    if (n < 67104 || memcmp(blob, VDC_BLOB_MAGIC, 6) != 0) {
        fprintf(stderr, "render: %s is not a VDC state blob (%ld bytes)\n", state, n);
        free(blob);
        return 64;
    }

    vdc_reset(&vdc);
    memset(&vce, 0, sizeof vce);
    {
        static const int off[12] = { 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30 };
        uint16_t reg[12];
        for (i = 0; i < 12; i++) {
            reg[i] = (uint16_t)(blob[off[i]] | (blob[off[i] + 1] << 8));
        }
        vdc.mwr = reg[0]; vdc.cr = reg[1]; vdc.bxr = reg[2]; vdc.byr = reg[3];
        vdc.hsr = reg[4]; vdc.hdr = reg[5]; vdc.vsr = reg[6]; vdc.vdr = reg[7];
        vdc.vcr = reg[8]; vdc.dcr = reg[9]; vdc.satb = reg[10]; vdc.dvssr = reg[10];
        vdc.rcr = reg[11];
    }
    if (zero_vds)                                    /* hypothesis test only */
        vdc.vsr = (uint16_t)(vdc.vsr & 0x00FF);
    for (i = 0; i < 512; i++) {
        vce.pal[i] = (uint16_t)(blob[32 + i * 2] | (blob[33 + i * 2] << 8));
    }
    /* The blob carries the palette TABLE.  What the chip displays is its cache
     * (VCE::FixPCache), so build the cache the way the chip would -- the table's
     * entry 0 is not what an index ending in 0 shows. */
    vce_fix_all(&vce);
    memcpy(vdc.vram, blob + 1056, 65536);
    memcpy(vdc.sat, blob + 1056 + 65536, 512);

    if (show) {
        printf("state: %dx%d BAT %dx%d BG=%d SPR=%d BXR=%d BYR=%d SATB=%04X "
               "VDS=%d VDW=%d\n",
               ((vdc.hdr & 0x7F) + 1) * 8, vdc.vdr & 0x1FF,
               (32 + 32 * (((vdc.mwr >> 4) & 3) != 0)) * (((vdc.mwr >> 4) & 3) != 0 ? 2 : 1),
               (vdc.mwr & 0x40) ? 64 : 32,
               (vdc.cr & 0x80) ? 1 : 0, (vdc.cr & 0x40) ? 1 : 0,
               vdc.bxr, vdc.byr, vdc.satb, (vdc.vsr >> 8) & 0xFF, vdc.vdr & 0x1FF);
    }

    vdc_render_frame(&vdc, &vce);

    f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "render: cannot write %s\n", out); free(blob); return 64; }
    fprintf(f, "P6\n%d %d\n255\n", VD_W, VD_H);
    fwrite(vdc.frame_pixels, 1, (size_t)VD_W * VD_H * 3, f);
    fclose(f);
    free(blob);
    printf("wrote %s (%dx%d)\n", out, VD_W, VD_H);
    return 0;
}

#ifdef XLATE_WITH_CASES
static void run_cases(void)
{
    unsigned count = 0, i;
    const XlateCase *cases = xlate_cases(&count);
    printf("CASES %u\n", count);
    for (i = 0; i < count; i++) {
        unsigned long s;
        memset(&cpu, 0, sizeof cpu);
        cases[i].init(&cpu);
        for (s = 0; s < cases[i].steps; s++) {
            if (!xl_cpu_step(&cpu))
                break;
            cpu.steps++;
        }
        {
            const char *want = getenv("XLATE_DUMP");
            if (want && atoi(want) == (int)cases[i].id) {
                FILE *df = fopen(getenv("XLATE_DUMP_PATH"), "wb");
                if (df) { fwrite(cpu.mem, 1, XL_PHYS_SIZE, df); fclose(df); }
            }
        }
        printf("CASE id=%u a=%02X x=%02X y=%02X s=%02X p=%02X pc=%04X "
               "mpr=%02X%02X%02X%02X%02X%02X%02X%02X crc=%08X cyc=%llu\n",
               cases[i].id, cpu.a, cpu.x, cpu.y, cpu.s, cpu.p, cpu.pc,
               cpu.mpr[0], cpu.mpr[1], cpu.mpr[2], cpu.mpr[3],
               cpu.mpr[4], cpu.mpr[5], cpu.mpr[6], cpu.mpr[7],
               xl_crc_phys(&cpu), (unsigned long long)cpu.cycles);
    }
    fflush(stdout);
}
#endif

#ifndef XLATE_WITH_CASES
/* host cdprobe -- a unit test of the drive's register interface, with no CPU code
 * and no disc image involved.
 *
 * It drives $1800-$180F the way the card's own driver does (ROM $E905-$E9C4): DB on
 * $1801, a select on $1800, then one byte per REQ/ACK handshake, then the status and
 * message bytes of the result.  What comes back is printed, so the two things a
 * command decoder cannot be allowed to get wrong are asserted where they can be seen:
 * an unknown opcode is REFUSED (CHECK CONDITION, named, counted) and the commands the
 * boot route uses answer with the values the console's own drive gave.
 *
 * Exit status is the number of failed expectations. */
static int probe_fail;

static void probe_expect(const char *what, unsigned got, unsigned want)
{
    printf("CDPROBE %-28s got=%02X want=%02X %s\n", what, got & 0xFF, want & 0xFF,
           (got & 0xFF) == (want & 0xFF) ? "ok" : "FAIL");
    if ((got & 0xFF) != (want & 0xFF))
        probe_fail++;
}

/* Wait, with a bound, for the drive to request a byte. */
static int probe_wait_req(void)
{
    unsigned i;
    for (i = 0; i < 1000u; i++)
        if (xl_rd(&cpu, 0x1800) & 0x40)
            return 1;
    return 0;
}

/* One REQ/ACK byte in whatever phase has the request up. */
static void probe_byte(uint8_t v)
{
    if (!probe_wait_req())
        return;
    xl_wr(&cpu, 0x1801, v);
    xl_wr(&cpu, 0x1802, 0x80);          /* ACK on  -> the drive latches/consumes */
    xl_wr(&cpu, 0x1802, 0x00);          /* ACK off -> next byte or next phase    */
}

/* Start a command and push `n` bytes of `op`: the drive decides how many it wants,
 * and stops asking when the command is complete. */
static void probe_command(const uint8_t *cdb, unsigned n)
{
    unsigned i;
    xl_wr(&cpu, 0x1801, 0x81);          /* the $81 start pulse, as the driver does */
    xl_wr(&cpu, 0x1800, 0x81);          /* select                                   */
    for (i = 0; i < n; i++) {
        if (!(xl_rd(&cpu, 0x1800) & 0x40))
            break;                      /* the drive stopped asking: it has enough */
        probe_byte(cdb[i]);
    }
}

/* Read the result: DATA IN bytes if the command has any, then STATUS, then MESSAGE,
 * then the bus goes free.  Returns the status byte. */
static uint8_t probe_result(uint8_t *data, unsigned *nbytes)
{
    unsigned guard = 0, n = 0;
    uint8_t status = 0xFF;

    *nbytes = 0;
    for (;;) {
        uint8_t st = xl_rd(&cpu, 0x1800) & 0xF8u;
        if (++guard > 100000u)
            break;
        if (st == 0xC8 || st == 0x88) {
            /* data in: take offered bytes through $1808 (which acknowledges) */
            if (st == 0xC8) {
                uint8_t v = xl_rd(&cpu, 0x1808);
                if (data && n < 32u)
                    data[n] = v;
                n++;
                *nbytes = n;
            }
            continue;
        }
        if (st == 0xD8) {                       /* status */
            status = xl_rd(&cpu, 0x1801);
            probe_byte(0);                      /* ACK it (the byte is already read) */
            continue;
        }
        if (st == 0xF8) {                       /* message in */
            (void)xl_rd(&cpu, 0x1801);
            probe_byte(0);
            continue;
        }
        if (st == 0x00)                         /* bus free: done */
            break;
    }
    return status;
}

static int cd_probe_mode(void)
{
    static const uint8_t tur[6]   = { 0x00, 0, 0, 0, 0, 0 };
    static const uint8_t ff[10]   = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                      0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    static const uint8_t q42[10]  = { 0x42, 0x11, 0x22, 0x33, 0x44,
                                      0x55, 0x66, 0x77, 0x88, 0x99 };
    static const uint8_t de0[10]  = { 0xDE, 0x00, 0xCA, 0, 0, 0, 0, 0, 0, 0 };
    static const uint8_t de1[10]  = { 0xDE, 0x01, 0xCA, 0, 0, 0, 0, 0, 0, 0 };
    static const uint8_t de2[10]  = { 0xDE, 0x02, 0x02, 0, 0, 0, 0, 0, 0, 0 };
    static const uint8_t de22[10] = { 0xDE, 0x02, 0x22, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t data[32];
    unsigned n;
    uint8_t status;
    unsigned i;

    memset(&cpu, 0, sizeof cpu);
    xlate_init_state(&cpu);
    cpu.bios_exec = 1;                  /* the decoder is the executed-card drive */

    printf("CDPROBE idle phase=%02X\n", xl_rd(&cpu, 0x1800));

    /* 1. an unknown opcode must not be silently accepted */
    probe_command(q42, 10);
    status = probe_result(data, &n);
    printf("CDPROBE unknown 42 bytes=%u\n", n);
    probe_expect("unknown 0x42 -> status", status, 0x02);       /* CHECK CONDITION */
    probe_expect("unknown 0x42 -> no data", n, 0);
    probe_expect("unknown 0x42 counted", (unsigned)cpu.cd.cmds_bad, 1);
    probe_expect("idle again", xl_rd(&cpu, 0x1800), 0x00);

    /* 2. the $FF probe the BIOS sends before it trusts the drive: also refused */
    probe_command(ff, 10);
    status = probe_result(data, &n);
    probe_expect("unknown 0xFF -> status", status, 0x02);
    probe_expect("unknown 0xFF counted", (unsigned)cpu.cd.cmds_bad, 2);

    /* 3. Test Unit Ready: GOOD, no data */
    probe_command(tur, 6);
    status = probe_result(data, &n);
    probe_expect("$00 Test Unit Ready status", status, 0x00);
    probe_expect("$00 Test Unit Ready data", n, 0);

    /* 4. Get Dir Info, all three modes, against the console's own answers */
    probe_command(de0, 10);
    status = probe_result(data, &n);
    probe_expect("$DE mode 0 status", status, 0x00);
    probe_expect("$DE mode 0 length", n, 2);
    for (i = 0; i < n && i < 32u; i++)
        printf("CDPROBE   de0[%u]=%02X\n", i, data[i]);
    probe_expect("$DE mode 0 first track", n > 0 ? data[0] : 0, 0x01);
    probe_expect("$DE mode 0 last track", n > 1 ? data[1] : 0, 0x22);

    probe_command(de1, 10);
    status = probe_result(data, &n);
    probe_expect("$DE mode 1 status", status, 0x00);
    probe_expect("$DE mode 1 length", n, 3);
    probe_expect("$DE mode 1 minutes", n > 0 ? data[0] : 0, 0x49);
    probe_expect("$DE mode 1 seconds", n > 1 ? data[1] : 0, 0x12);
    probe_expect("$DE mode 1 frames", n > 2 ? data[2] : 0, 0x12);

    probe_command(de2, 10);
    status = probe_result(data, &n);
    probe_expect("$DE mode 2 track 2 status", status, 0x00);
    probe_expect("$DE mode 2 track 2 length", n, 4);
    for (i = 0; i < n && i < 32u; i++)
        printf("CDPROBE   de2[%u]=%02X\n", i, data[i]);
    probe_expect("$DE track 2 minute", n > 0 ? data[0] : 0, 0x00);
    probe_expect("$DE track 2 second", n > 1 ? data[1] : 0, 0x53);
    probe_expect("$DE track 2 frame", n > 2 ? data[2] : 0, 0x65);
    probe_expect("$DE track 2 type(data)", n > 3 ? data[3] : 0, 0x04);

    probe_command(de22, 10);
    status = probe_result(data, &n);
    probe_expect("$DE mode 2 track 22 status", status, 0x00);
    probe_expect("$DE mode 2 track 22 length", n, 4);
    for (i = 0; i < n && i < 32u; i++)
        printf("CDPROBE   de22[%u]=%02X\n", i, data[i]);
    probe_expect("$DE track 22 minute", n > 0 ? data[0] : 0, 0x46);
    probe_expect("$DE track 22 second", n > 1 ? data[1] : 0, 0x58);
    probe_expect("$DE track 22 frame", n > 2 ? data[2] : 0, 0x62);

    /* 5. a request sense after the refusals reports the refusal, not GOOD */
    {
        static const uint8_t sense[6] = { 0x03, 0, 0, 0, 0x12, 0 };
        probe_command(sense, 6);
        status = probe_result(data, &n);
        probe_expect("$03 Request Sense status", status, 0x00);
        probe_expect("$03 Request Sense length", n, 18);
        probe_expect("$03 sense key", n > 2 ? data[2] : 0, 0x05);   /* illegal req */
        probe_expect("$03 ASC", n > 12 ? data[12] : 0, 0x20);
    }

    printf("CDPROBE commands=%lu accepted=%lu rejected=%lu\n",
           cpu.cd.cmds, cpu.cd.cmds_ok, cpu.cd.cmds_bad);
    printf("CDPROBE %s (%d failed expectations)\n", probe_fail ? "FAIL" : "PASS",
           probe_fail);
    return probe_fail;
}
#endif

#ifndef XLATE_WITH_CASES
static int run_program(int argc, char **argv)
{
    const char *disc_path = "../inputs/data_payload.bin";
    const char *bios_path = NULL;
    const char *sram_path = NULL;
    const char *log_path = NULL;
    const char *dump_path = NULL;
    const char *vram_path = NULL;
    unsigned pad0 = 0;
    int boot_game = 0;
    /* Input windows are static rather than allocated: a long run can carry one
     * window per input (thousands of windows) and this array is 2.4 MB of BSS,
     * which costs nothing at runtime. */
    static struct { uint32_t from, to; unsigned mask; } presses[200000];
    int npresses = 0;
    int nresets = 0;
    uint16_t poke_addr[8];
    uint8_t poke_val[8];
    int npoke = 0;
    unsigned long max_steps = 200000;
    uint64_t max_cycles = 0;
    const char *frame_out = NULL;
    const char *from_state = NULL;
    const char *from_ckpt = NULL;
    const char *padstream_path = NULL;
    uint32_t padstream_from = 0;
    int32_t padstream_lag = 0;
    const uint8_t *padstream = NULL;
    size_t padstream_bytes = 0;
    int use_stop = 0, trace = 0, show_hw = 0;
    uint16_t stop_pc = 0;
    int i, rc;
    FILE *logf = NULL;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--disc") == 0 && i + 1 < argc) {
            disc_path = argv[++i];
        } else if (strcmp(argv[i], "--bios") == 0 && i + 1 < argc) {
            bios_path = argv[++i];
        } else if (strcmp(argv[i], "--sram") == 0 && i + 1 < argc) {
            /* The console's backup RAM (2 KB, bank $F7).  An input like the disc:
             * the machine's save data. */
            sram_path = argv[++i];
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_path = argv[++i];
        } else if (strcmp(argv[i], "--steps") == 0 && i + 1 < argc) {
            max_steps = strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--stop-pc") == 0 && i + 1 < argc) {
            stop_pc = (uint16_t)strtoul(argv[++i], NULL, 0);
            use_stop = 1;
        } else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            dump_path = argv[++i];
        } else if (strcmp(argv[i], "--vram-out") == 0 && i + 1 < argc) {
            vram_path = argv[++i];
        } else if (strcmp(argv[i], "--press") == 0 && i + 1 < argc) {
            /* FROM:TO:MASK -- press MASK from frame FROM up to (not including) TO.
             * Derive the list from an emulator replay with tools/replay_presses.py. */
            unsigned f = 0, t = 0, m = 0;
            char *spec = argv[++i], *c1, *c2;
            c1 = strchr(spec, ':');
            c2 = c1 ? strchr(c1 + 1, ':') : NULL;
            if (!c1 || !c2 || npresses >= (int)(sizeof presses / sizeof presses[0])) {
                fprintf(stderr, "host: bad --press %s (want FROM:TO:MASK)\n", spec);
                return 64;
            }
            f = (unsigned)strtoul(spec, NULL, 0);
            t = (unsigned)strtoul(c1 + 1, NULL, 0);
            m = (unsigned)strtoul(c2 + 1, NULL, 0);
            presses[npresses].from = f; presses[npresses].to = t;
            presses[npresses].mask = m; npresses++;
        } else if (strcmp(argv[i], "--pressfile") == 0 && i + 1 < argc) {
            /* One FROM:TO:MASK per line, '#' to end of line is a comment.
             * This is how a recorded run is replayed: tools/replay_presses.py
             * prints exactly this from an emulator movie, and the console's own
             * schedule (behaviour-engine's tas6733.schedule.tsv) is one run per
             * line in the same shape. */
            const char *path = argv[++i];
            FILE *pf = fopen(path, "r");
            char line[256];
            if (!pf) { fprintf(stderr, "host: no press file at %s\n", path); return 64; }
            while (fgets(line, sizeof line, pf)) {
                char *hash = strchr(line, '#');
                unsigned f = 0, t = 0, m = 0;
                if (hash) *hash = '\0';
                {
                    /* Parse with base 0: the masks in a press file are written
                     * 0x08, and %u reads that as 0 and silently presses nothing. */
                    char *q1 = strchr(line, ':'), *q2 = q1 ? strchr(q1 + 1, ':') : NULL;
                    if (q1 && q2) {
                        f = (unsigned)strtoul(line, NULL, 0);
                        t = (unsigned)strtoul(q1 + 1, NULL, 0);
                        m = (unsigned)strtoul(q2 + 1, NULL, 0);
                    } else {
                        continue;
                    }
                }
                if (m || f || t) {
                    if (npresses >= (int)(sizeof presses / sizeof presses[0])) {
                        fprintf(stderr, "host: press file too long at %s\n", line);
                        fclose(pf);
                        return 64;
                    }
                    presses[npresses].from = f; presses[npresses].to = t;
                    presses[npresses].mask = m; npresses++;
                }
            }
            fclose(pf);
        } else if (strcmp(argv[i], "--resets") == 0 && i + 1 < argc) {
            /* F1,F2,... -- a console RESET at the top of each of these frames.
             * A reset is not a pad input, so it cannot be in the press file: a
             * schedule that resets the machine names its frames here instead.
             * The frame numbers are the same counter the press windows use. */
            char *list = argv[++i];
            char *p = list;
            while (*p) {
                char *end;
                unsigned long f = strtoul(p, &end, 0);
                if (end == p) {
                    fprintf(stderr, "host: bad --resets %s (want F1,F2,...)\n", list);
                    return 64;
                }
                if (!xl_reset_at((uint32_t)f)) {
                    fprintf(stderr, "host: reset schedule too long at %s\n", list);
                    return 64;
                }
                nresets++;
                p = (*end == ',') ? end + 1 : end;
            }
        } else if (strcmp(argv[i], "--resetfile") == 0 && i + 1 < argc) {
            /* One frame number per line, '#' to end of line is a comment: the press
             * file's conventions, because this is the same kind of thing -- a
             * recorded event at a frame, not a hardware rule. */
            const char *path = argv[++i];
            FILE *rf = fopen(path, "r");
            char line[256];
            if (!rf) { fprintf(stderr, "host: no reset file at %s\n", path); return 64; }
            while (fgets(line, sizeof line, rf)) {
                char *hash = strchr(line, '#');
                char *end;
                unsigned long f;
                if (hash) *hash = '\0';
                f = strtoul(line, &end, 0);
                if (end == line)
                    continue;                    /* blank or not a number */
                if (!xl_reset_at((uint32_t)f)) {
                    fprintf(stderr, "host: reset schedule too long at %s\n", line);
                    fclose(rf);
                    return 64;
                }
                nresets++;
            }
            fclose(rf);
        } else if (strcmp(argv[i], "--padstream") == 0 && i + 1 < argc) {
            /* The reference machine's own held controller state, five bytes per
             * frame, as produced by tools/lockstep/pad_from_ref.py from its
             * per-frame RAM.  This replaces the press windows: the experiment
             * becomes "given the buttons the reference physically held, does this
             * machine behave the same", with no question about whether a fixture
             * pressed at the right moment. */
            padstream_path = argv[++i];
        } else if (strcmp(argv[i], "--padstream-from") == 0 && i + 1 < argc) {
            /* The frame number of the stream's first entry.  0 by default, which
             * is what pad_from_ref.py writes for a whole-run stream. */
            padstream_from = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--padstream-lag") == 0 && i + 1 < argc) {
            /* How many frames EARLIER than the port's frame number the stream's
             * entries belong: with `lag` set, port frame N receives the console's
             * held state for frame N-lag.  This is the alignment parameter of the
             * experiment, and it is measured, not chosen -- tools/lockstep/
             * clock_offset.py reports the constant offset between the two
             * machines' clocks from the whole 8 KB of work RAM.  Zero (the
             * default) is the honest wall-frame comparison and is what a claim
             * about the two machines in the same frame must use. */
            padstream_lag = (int32_t)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--boot-game") == 0) {
            boot_game = 1;
        } else if (strcmp(argv[i], "--pad") == 0 && i + 1 < argc) {
            /* Active-high pad state: bit0 I, 1 II, 2 SELECT, 3 RUN, 4 UP, 5 RIGHT,
             * 6 DOWN, 7 LEFT.  A constant value gives one "newly pressed" edge. */
            pad0 = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--poke") == 0 && i + 1 < argc) {
            /* ADDR=VALUE: set one byte before the run starts.  Diagnostic tool for
             * experiments; tests state their own expectations. */
            char *arg = argv[++i];
            char *eq = strchr(arg, '=');
            if (!eq || npoke >= 8) {
                fprintf(stderr, "host: bad --poke %s\n", arg);
                return 64;
            }
            *eq = 0;
            poke_addr[npoke] = (uint16_t)strtoul(arg, NULL, 0);
            poke_val[npoke] = (uint8_t)strtoul(eq + 1, NULL, 0);
            npoke++;
        } else if (strcmp(argv[i], "--cycles") == 0 && i + 1 < argc) {
            max_cycles = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--from-state") == 0 && i + 1 < argc) {
            from_state = argv[++i];
        } else if (strcmp(argv[i], "--from-ckpt") == 0 && i + 1 < argc) {
            /* A state image this port wrote itself (XLATE_CKPT_DIR): the whole machine,
             * taken at an instruction boundary, so it resumes where it was rather than
             * being placed at a frame boundary the way a console savestate is. */
            from_ckpt = argv[++i];
        } else if (strcmp(argv[i], "--frame") == 0 && i + 1 < argc) {
            frame_out = argv[++i];
        } else if (strcmp(argv[i], "--trace") == 0) {
            trace = 1;
        } else if (strcmp(argv[i], "--hw") == 0) {
            show_hw = 1;
        } else {
            fprintf(stderr, "host: unknown option %s\n", argv[i]);
            return 64;
        }
    }

    if (log_path) {
        logf = fopen(log_path, "a");
        if (!logf) {
            fprintf(stderr, "host: cannot open log %s\n", log_path);
            return 64;
        }
        setvbuf(logf, NULL, _IOLBF, 0);
        printf("log: %s\n", log_path);   /* never a mystery where the trail is */
    }

    memset(&cpu, 0, sizeof cpu);
    /* xlate_init_state() starts with xl_reset(), which zeroes the whole struct --
     * so everything that has to survive (log, disc, ROM) is attached after it. */
    xlate_init_state(&cpu);
    xl_set_log(&cpu, logf);

    if (disc_path) {
        size_t len = 0;
        uint8_t *disc = read_file(disc_path, &len);
        if (disc)
            xl_attach_disc(&cpu, disc, len);
        else
            fprintf(stderr, "host: no disc at %s (CD reads will not be modelled)\n",
                    disc_path);
    }
    if (bios_path) {
        size_t len = 0;
        uint8_t *rom = read_file(bios_path, &len);
        if (rom)
            xl_load_rom(&cpu, rom, len);
        else
            fprintf(stderr, "host: no BIOS at %s\n", bios_path);
    }
    if (sram_path) {
        size_t len = 0;
        uint8_t *sram = read_file(sram_path, &len);
        if (sram)
            xl_load_sram(&cpu, sram, len);
        else
            fprintf(stderr, "host: no save RAM at %s (the card is blank)\n", sram_path);
    }
    if (cpu.bios_exec) {
        /* Announced, not silent: a run with the switch on is a different machine from
         * one with it off, and no number from the two may be mixed up.  It goes to the
         * log as well as to stdout, because the log is where a run is read from after
         * the fact. */
        printf("BIOS_EXEC on=1 rom=%d (%s) -- code in a ROM page is interpreted; the "
               "modelled BIOS entries are off\n",
               cpu.rom_present, bios_path ? bios_path : "none");
        xl_report_vectors(&cpu, stdout);
        if (cpu.log) {
            fprintf(cpu.log, "BIOS_EXEC on=1 rom=%d: code in a ROM page is interpreted; "
                             "the modelled BIOS entries are off\n", cpu.rom_present);
            xl_report_vectors(&cpu, cpu.log);
            fflush(cpu.log);
        }
    }
    /* XLATE_BIOS_RESET: the machine starts where the console starts it, at the
     * card's reset vector, with the power-on mapping and cold chips.  The card then
     * initialises the hardware and loads the game itself.  Off by default; the
     * translated loader at $3000 stays the default entry. */
    xl_boot_reset(&cpu);

    /* A scheduled reset is the reset-vector entry performed again, so a schedule
     * without that entry cannot happen -- and a run that quietly ignored its
     * resets would look like a content divergence, not like a missing switch.
     * Refused here, before any frame is emulated, rather than at frame 9,937. */
    if (nresets && !cpu.bios_reset) {
        fprintf(stderr, "host: %d reset(s) scheduled but the reset-vector entry is "
                        "off (%s); a console reset needs XLATE_BIOS_EXEC=1 and "
                        "XLATE_BIOS_RESET=1 with the System Card attached\n",
                nresets, !cpu.bios_exec ? "XLATE_BIOS_EXEC is off"
                                        : "no System Card / XLATE_BIOS_RESET off");
        return 64;
    }
    if (nresets) {
        int k;
        printf("RESETS n=%d frames=", nresets);
        for (k = 0; k < xl_reset_count(); k++)
            printf("%s%u", k ? "," : "", xl_reset_frame(k));
        printf("\n");
        if (cpu.log) {
            fprintf(cpu.log, "resets: %d scheduled, at the top of frames", nresets);
            for (k = 0; k < xl_reset_count(); k++)
                fprintf(cpu.log, " %u", xl_reset_frame(k));
            fprintf(cpu.log, "\n");
            fflush(cpu.log);
        }
    }

    xl_set_pad(&cpu, 0, (uint8_t)pad0);
    cpu.boot_game_on_run = (uint8_t)boot_game;
    if (padstream_path) {
        padstream = read_file(padstream_path, &padstream_bytes);
        if (!padstream || padstream_bytes < 5) {
            fprintf(stderr, "host: no pad stream at %s (or it is shorter than one "
                            "frame; build it with tools/lockstep/pad_from_ref.py)\n",
                    padstream_path);
            return 64;
        }
        /* Announced in full: a run on console input is a different experiment from
         * a run on a press file, and no number from the two may be mixed up. */
        printf("PADSTREAM %s from=%u frames=%zu lag=%d held=console (5 B/frame, $2228)\n",
               padstream_path, padstream_from, padstream_bytes / 5, padstream_lag);
        if (cpu.log) {
            fprintf(cpu.log, "padstream: %s from=%u frames=%zu lag=%d; the held state "
                             "the console's own pad routine latched, fed at the frame "
                             "boundary\n", padstream_path, padstream_from,
                    padstream_bytes / 5, padstream_lag);
            fflush(cpu.log);
        }
        if (npresses)
            fprintf(stderr, "host: --padstream given, so the %d press window(s) are "
                            "NOT used (the stream is the input)\n", npresses);
        xl_pad_stream(&cpu, padstream, (uint32_t)(padstream_bytes / 5), padstream_from,
                      padstream_lag);
    }
    for (i = 0; i < npresses; i++)
        xl_pad_press(&cpu, presses[i].from, presses[i].to, (uint8_t)presses[i].mask);
    for (i = 0; i < npoke; i++)
        xl_wr(&cpu, poke_addr[i], poke_val[i]);

    if (from_ckpt) {
        int e = xl_ckpt_load(&cpu, from_ckpt);
        if (e) {
            fprintf(stderr, "host: checkpoint load failed (%d) for %s\n", e, from_ckpt);
            return 2;
        }
        /* The image carries its own raster position, cycle total, drive phase and
         * frame counter, so nothing is repositioned here: that is the difference
         * between a checkpoint and a console savestate. */
        /* The image is the whole struct, and `pad_hold` is a POINTER into the pad
         * stream this process read.  A checkpoint therefore carries a pointer that
         * belonged to the run that wrote it: resuming it without re-attaching the
         * input dereferences another process's address and dies on the first pad
         * tick (measured: SIGSEGV immediately after START).  Input is not state. */
        if (padstream)
            xl_pad_stream(&cpu, padstream, (uint32_t)(padstream_bytes / 5),
                          padstream_from, padstream_lag);
        printf("CKPT_INJECTED pc=%04X frame=%u cycles=%llu mpr=%02X%02X%02X%02X%02X%02X%02X%02X\n",
               cpu.pc, (unsigned)cpu.vdc.frame, (unsigned long long)cpu.cycles,
               cpu.mpr[0], cpu.mpr[1], cpu.mpr[2], cpu.mpr[3],
               cpu.mpr[4], cpu.mpr[5], cpu.mpr[6], cpu.mpr[7]);
    }
    if (from_state) {
        int e = xl_load_state_file(&cpu, from_state);
        if (e) {
            fprintf(stderr, "host: state load failed (%d) for %s\n", e, from_state);
            return 2;
        }
        /* THE STATE IS TAKEN AT A FRAME BOUNDARY, SO THE RASTER BELONGS THERE.
         *
         * A savestate is written at the start of a console frame, which on this
         * chip is the vertical sync -- VSW+1 + VDS+2 lines before the top of the
         * display, i.e. this model's line `VD_LINES - (vsw + vds)` (see
         * vdc_tick's frame-boundary note).  The image carries no raster, so the
         * model's counter stayed at 0 -- the top of the display, twenty lines
         * *later* in the frame than the console's instant -- and the first frame
         * was therefore short by those lines while the drive's sector countdown
         * (which is measured from the state's own instant) ran on unaffected.
         *
         * This used to be papered over by advancing a whole invented frame of
         * clock with nothing executing (`xl_tick(120000)`), which rendered "frame
         * 0" from the injected video memory.  That cost a frame of drive time with
         * no CPU work in it: the sector the console drains during frame 0 stayed
         * on the FIFO, the next fill found it full, and the drive deferred a whole
         * period -- one frame lost, every time, before the machine had run an
         * instruction.  Measured on the 28,200 prologue state: the fill blocks at
         * frame 1 with 209 of 2048 bytes taken. */
        {
            int vsw = (cpu.vdc.vsr & 0x1F) + 1;
            int vds = ((cpu.vdc.vsr >> 8) & 0xFF) + 2;
            int frame_line = VD_LINES - (vsw + vds);
            if (frame_line <= 0 || frame_line >= VD_LINES)
                frame_line = VD_LINES - 20;
            cpu.vdc.line = frame_line;
            cpu.vdc.dot = 0;
            cpu.vdc.vblank = 0;
        }
        printf("STATE_INJECTED pc=%04X mpr=%02X%02X%02X%02X%02X%02X%02X%02X "
               "cr=%04X mwr=%04X bxr=%04X byr=%04X line=%d\n",
               cpu.pc, cpu.mpr[0], cpu.mpr[1], cpu.mpr[2], cpu.mpr[3], cpu.mpr[4],
               cpu.mpr[5], cpu.mpr[6], cpu.mpr[7],
               cpu.vdc.cr, cpu.vdc.mwr, cpu.vdc.bxr, cpu.vdc.byr, cpu.vdc.line);
    }

    printf("START pc=%04X steps=%lu presses=%d npads=%d schedule=%u:%u:%02X\n",
           cpu.pc, cpu.steps, npresses, cpu.npads,
           cpu.npads ? cpu.pads[0].from : 0, cpu.npads ? cpu.pads[0].to : 0,
           cpu.npads ? cpu.pads[0].mask : 0);

    if (trace) {
        unsigned long n = 0;
        while (n < max_steps) {
            if (use_stop && cpu.pc == stop_pc)
                break;
            printf("TRACE pc=%04X bank=%02X op=%02X a=%02X x=%02X y=%02X s=%02X p=%02X\n",
                   cpu.pc, cpu.mpr[cpu.pc >> 13], xl_rd(&cpu, cpu.pc),
                   cpu.a, cpu.x, cpu.y, cpu.s, cpu.p);
            if (!xlate_step(&cpu))
                break;
            cpu.steps++;
            n++;
        }
        rc = XL_OUT_OF_STEPS;
    } else {
        rc = xl_run_budget(&cpu, max_steps, use_stop, stop_pc, max_cycles);
    }

    switch (rc) {
    case XL_STOPPED_AT_PC: printf("STOP reason=stop-pc pc=%04X\n", cpu.pc); break;
    case XL_TRAPPED:       printf("STOP reason=trap pc=%04X\n", cpu.pc); break;
    case XL_UNTRANSLATED:  printf("STOP reason=untranslated pc=%04X\n", cpu.pc); break;
    case XL_OUT_OF_STEPS:  printf("STOP reason=steps pc=%04X\n", cpu.pc); break;
    default:               printf("STOP reason=%d pc=%04X\n", rc, cpu.pc); break;
    }
    /* How much of the run the interpreter executed.  Zero on a plain run; on a
     * forced run (XLATE_INTERP=1) it is the instruction count that replaced the
     * translation -- the number gate O-5 makes mandatory, so a run leaning on the
     * interpreter can never be mistaken for a translated one. */
    printf("INTERP steps=%lu\n", cpu.interp_steps);

    xl_dump_state(&cpu, stdout);
    /* Interrupts by source.  The card's vblank housekeeping is one IRQ1 per frame;
     * anything else here is the drive's line or the timer, and knowing which is
     * which is the difference between "the machine takes its interrupts" and "the
     * machine takes the one it always took". */
    printf("IRQSUM irq2=%u irq1=%u timer=%u taken=%u\n",
           cpu.irq_kind[0], cpu.irq_kind[1], cpu.irq_kind[2], cpu.irq_taken);
    {
        extern unsigned long xl_e9_window_count;
        extern int xl_e9_window_from, xl_e9_window_to;
        if (xl_e9_window_from >= 0)
            printf("CDWIN $E009 calls in frames %d-%d: %lu\n",
                   xl_e9_window_from, xl_e9_window_to, xl_e9_window_count);
    }
    xl_vramhi_summary(stdout);
    xl_cd_summary(&cpu, stdout);
    if (getenv("XLATE_SRAM_DUMP") && getenv("XLATE_SRAM_DUMP")[0]) {
        /* The card's save data as the run leaves it, so it can be compared byte for
         * byte with a save dump taken from a reference run. */
        FILE *sf = fopen(getenv("XLATE_SRAM_DUMP"), "wb");
        if (sf) {
            fwrite(cpu.bram, 1, XL_BRAM_SIZE, sf);
            fclose(sf);
            printf("SRAM %s writes=%u blocked=%u wprot=%u\n",
                   getenv("XLATE_SRAM_DUMP"), cpu.bram_writes, cpu.bram_blocked,
                   cpu.bram_wprot);
        }
    }
    xl_untranslated_summary(stdout);
    xl_banks_summary(stdout);
    xl_guard_summary(&cpu, stdout);
    xl_trace_summary(stdout);
    printf("SUMMARY steps=%lu bios_calls=%lu modelled_reads=%lu unmodelled=%lu "
           "read_bytes=%llu rom_writes_ignored=%u bios_interp=%lu "
           "interp_untranslated=%lu\n",
           cpu.steps, cpu.bios_calls, cpu.modelled_reads, cpu.unmodelled_calls,
           (unsigned long long)cpu.read_bytes, cpu.rom_writes_ignored,
           cpu.bios_interp_steps, cpu.interp_untranslated_steps);
    printf("VIDEO vdc_writes=%u vram_writes=%u vdc_reads=%u dma=%u sat_dma=%u "
           "vblank_irq=%u rcr_irq=%u cr=%04X mwr=%04X vdw=%d hdr=%04X "
           "bg=%d spr=%d\n",
           cpu.vdc.writes, cpu.hw_writes[0x0002] + cpu.hw_writes[0x0003],
           cpu.vdc.reads, cpu.vdc.dma_runs, cpu.vdc.sat_dma_runs,
           cpu.vdc.vblank_irqs, cpu.vdc.rcr_irqs, cpu.vdc.cr, cpu.vdc.mwr,
           cpu.vdc.vdr & 0x1FF, cpu.vdc.hdr,
           (cpu.vdc.cr & 0x80) ? 1 : 0, (cpu.vdc.cr & 0x40) ? 1 : 0);
    if (getenv("XLATE_HW") && getenv("XLATE_HW")[0] != '0') {
        int hi, k;
        printf("HW_READS");
        for (k = 0; k < 12; k++) {
            unsigned best = 0;
            int bo = -1;
            for (hi = 0; hi < 0x2000; hi++) {
                if (cpu.hw_reads[hi] > best) { best = cpu.hw_reads[hi]; bo = hi; }
            }
            if (bo < 0 || !best) break;
            printf(" %04X=%u", bo, best);
            cpu.hw_reads[bo] = 0;
        }
        printf("\n");
    }
    if (getenv("XLATE_BIOS_COUNTS") && getenv("XLATE_BIOS_COUNTS")[0] != '0') {
        extern uint32_t xl_bios_entry_count[256];
        int e;
        printf("BIOS_COUNTS");
        for (e = 0; e < 256; e++)
            if (xl_bios_entry_count[e])
                printf(" %04X=%u", 0xE000 + e, xl_bios_entry_count[e]);
        printf("\n");
    }
    if (getenv("XLATE_VDC_VALS") && getenv("XLATE_VDC_VALS")[0] != '0') {
        /* REPORT-ONLY: the vertical timing registers as the game left them.  The
         * vblank line the chip uses is the end of the display window (VDR), and
         * VPR/VCR say how the rest of the frame is split; without them "line 240"
         * is a number nobody measured. */
        printf("VDC_VALS cr=%04X vpr=%04X vdw=%04X vcr=%04X mwr=%04X\n",
               cpu.vdc.cr, cpu.vdc.vsr, cpu.vdc.vdr, cpu.vdc.vcr, cpu.vdc.mwr);
    }
    if (getenv("XLATE_VDC_REGS") && getenv("XLATE_VDC_REGS")[0] != '0') {
        int r;
        printf("VDC_REGS");
        for (r = 0; r < 32; r++)
            if (cpu.vdc.reg_writes[r])
                printf(" %02X=%u", r, cpu.vdc.reg_writes[r]);
        printf("\n");
    }
    {
        const char *e = getenv("XLATE_PC");
        if (e && e[0]) {
            extern uint32_t xl_hist[65536];
            const char *q = e;
            printf("PC_COUNTS");
            while (*q) {
                char *end;
                unsigned long addr = strtoul(q, &end, 16);
                if (end == q) break;
                printf(" %04lX=%u", addr, xl_hist[addr & 0xFFFF]);
                q = (*end == ',') ? end + 1 : end;
            }
            printf("\n");
        }
    }
    if (getenv("XLATE_HIST") && getenv("XLATE_HIST")[0] != '0') {
        /* Top hot spots: tells you where the program loops, which is usually the
         * answer to "why is nothing happening". */
        extern uint32_t xl_hist[65536];
        int k, shown = 0;
        unsigned long long total = 0;
        int t;
        for (t = 0; t < 65536; t++) total += xl_hist[t];
        printf("HOT_TOTAL %llu\n", total);
        for (k = 0; k < 15; k++) {
            uint32_t best = 0;
            int at = -1, j;
            for (j = 0; j < 65536; j++)
                if (xl_hist[j] > best) { best = xl_hist[j]; at = j; }
            if (at < 0 || !best)
                break;
            printf("HOT pc=%04X count=%u pct=%.1f\n", at, best,
                   total ? 100.0 * (double)best / (double)total : 0.0);
            xl_hist[at] = 0;
            shown++;
        }
        (void)shown;
    }
    if (show_hw)
        print_hw_summary();
    if (vram_path) {
        FILE *vf = fopen(vram_path, "wb");
        if (!vf) {
            fprintf(stderr, "host: cannot write %s\n", vram_path);
        } else {
            xl_dump_vdc_state(&cpu, vf);
            fclose(vf);
            printf("VRAM_STATE %s\n", vram_path);
        }
    }
    if (frame_out) {
        FILE *ff = fopen(frame_out, "wb");
        if (!ff) {
            fprintf(stderr, "host: cannot write %s\n", frame_out);
        } else {
            xl_write_ppm(&cpu, ff);
            fclose(ff);
            printf("FRAME %s %dx%d\n", frame_out, VD_W, VD_H);
        }
    }
    if (dump_path) {
        FILE *df = fopen(dump_path, "wb");
        if (!df) {
            fprintf(stderr, "host: cannot write %s\n", dump_path);
        } else {
            fwrite(cpu.mem, 1, XL_PHYS_SIZE, df);
            fclose(df);
            printf("DUMP %s size=%d\n", dump_path, XL_PHYS_SIZE);
        }
    }
    /* XLATE_ADPCM_DUMP=PATH: the ADPCM chip's own 64 KB sample RAM, and the registers
     * that say where the pointers are.  It is not part of the CPU's memory map (the
     * chip owns it, `geargrafx/src/src/adpcm.h:95`), so it cannot come out of
     * `--dump`; without this file there is no way to compare our sample RAM with the
     * reference's `cd.adpcm_ram` blob at all. */
    {
        const char *ap = getenv("XLATE_ADPCM_DUMP");
        if (ap && ap[0]) {
            FILE *af = fopen(ap, "wb");
            if (!af) {
                fprintf(stderr, "host: cannot write %s\n", ap);
            } else {
                fwrite(cpu.cd.adpcm_ram, 1, sizeof cpu.cd.adpcm_ram, af);
                fclose(af);
                printf("ADPCM_DUMP %s size=%u read=%04X write=%04X addr=%04X len=%u "
                       "ctl=%02X dma=%02X ring=%d/%d freq=%02X\n",
                       ap, (unsigned)sizeof cpu.cd.adpcm_ram,
                       (unsigned)cpu.cd.adpcm_readaddr, (unsigned)cpu.cd.adpcm_writeaddr,
                       (unsigned)cpu.cd.adpcm_addr, (unsigned)cpu.cd.adpcm_len,
                       (unsigned)cpu.cd.adpcm_lastcmd, (unsigned)cpu.cd.status_b,
                       (int)cpu.cd.adpcm_read_cycles, (int)cpu.cd.adpcm_write_cycles,
                       (unsigned)cpu.cd.adpcm_freq);
            }
        }
    }
    if (cpu.log) {
        fprintf(cpu.log, "host: stop reason=%d pc=%04X steps=%lu bios=%lu\n",
                rc, cpu.pc, cpu.steps, cpu.bios_calls);
        fflush(cpu.log);
        fclose(cpu.log);
    }
    fflush(stdout);
    if (rc == XL_TRAPPED)
        return 2;
    if (rc == XL_UNTRANSLATED)
        return 3;
    return 0;
}
#endif /* !XLATE_WITH_CASES */

int main(int argc, char **argv)
{
    /* Both build shapes can render a dumped video state: that is how the
     * renderer is checked against the emulator's own screenshots. */
    if (argc >= 2 && strcmp(argv[1], "render") == 0)
        return render_state_mode(argc, argv);
#ifndef XLATE_WITH_CASES
    if (argc >= 2 && strcmp(argv[1], "cdprobe") == 0)
        return cd_probe_mode();
#endif
#ifdef XLATE_WITH_CASES
    /* Random-test builds contain only the case table: no boot program. */
    if (argc >= 2 && strcmp(argv[1], "cases") == 0) {
        memset(&cpu, 0, sizeof cpu);
        run_cases();
        return 0;
    }
    fprintf(stderr, "host: this build only supports `cases`\n");
    return 64;
#else
    return run_program(argc, argv);
#endif
}
