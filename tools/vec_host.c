/* vec_host.c -- run ONE published HuC6280 single-step vector through this machine.
 *
 * WHY.  `interp.c` is the reference side of every verification in this project, and
 * until now it had only ever been compared with the translator -- two implementations
 * that share a misunderstanding agree forever.  This program is the other side of
 * that comparison: it takes the initial machine state a vector names, executes exactly
 * one instruction through `xl_interp_step()` / `xl_cpu_step()`, and prints the state
 * and the bus activity that resulted, so a Python front-end (tools/vec_suite.py) can
 * compare it with the published final state.
 *
 * WHAT IS SET, AND WHAT IS DELIBERATELY NOT.  The machine is built by `xl_reset()`,
 * then the eight MPRs, A/X/Y/S/P, PC and every RAM pair the vector lists are written.
 * Memory outside those pairs is zero, which is the vector set's own convention (its
 * decoder treats RAM as a flat 2 MB chunk).  Nothing else is touched: the VDC, VCE,
 * timer and CD unit keep their power-on state, so a step that reads a hardware
 * register reads the model's power-on value -- that is a real difference from the
 * vector set, is classified as such by the front-end, and is not papered over here.
 *
 * BUS RECORDING.  Built with -DXLATE_BUS_REC (see runtime.h), which compiles a hook
 * into xl_rd()/xl_wr() that stores (physical address, byte, direction) for every
 * access the CPU makes, in order.  The hook cannot change a value; it only observes.
 * Without -DXLATE_BUS_REC the program still runs, and prints NBUS=0 for every case --
 * so "the bus channel is unavailable" is a fact the output states instead of a silent
 * zero.
 *
 * INPUT (stdin), one case per line, '#' starts a comment:
 *
 *   A X Y S P PC MPRHEX16 NRAM [PHYSADDR:VALUE ...]        (all values hex)
 *
 * OUTPUT (argv[1], or stdout when omitted), one line per case:
 *
 *   CASE <n> A=.. X=.. Y=.. S=.. P=.. PC=.. MPR=.. CYC=.. REFUSED=0|1
 *            NBUS=.. BUS=phys:val:r,phys:val:w,...
 *
 * REFUSED=1 means the machine declined to execute the byte (an undefined opcode):
 * `xl_interp_step()` returned 0 and printed its own UNTRANSLATED line to stdout, which
 * the front-end ignores.  CYC is the change in `c->cycles_charged` across the step: the
 * cycles the machine CHARGED, in the ADDCYC unit the published `num_cycles` is written
 * in (it includes the VDC port steal and wait states, which are charged cycles too).
 * It is deliberately not `c->cycles`, which is the same quantity scaled by the CPU
 * speed mode -- `CSL` is the one instruction where the two differ, and the vectors are
 * written in the other unit.
 *
 * NOT part of the port's build: this file lives in the private vector work tree and
 * is compiled only by tools/vec_suite.py.  It adds nothing to `src/` except the
 * two `#ifdef XLATE_BUS_REC` hooks it needs, which are inert in every normal build.
 */
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Cpu cpu;
static Cpu base;   /* power-on machine, snapshotted once: copying the 2 MB RAM for
                    * every one of the ~640,000 vectors would be 1.3 TB of writes;
                    * the registers and devices are a few tens of KB. */

/* Pages to return to zero after a case: the vector's own RAM pairs plus every page
 * the instruction actually touched.  A full 2 MB memset per case would be ~1.3 TB of
 * writes over the whole set; zeroing the handful of pages a case uses is the same
 * machine state for a fraction of the time. */
#define MAXP 64
static uint8_t pages[MAXP];
static unsigned npages;

static void page_use(unsigned p)
{
    unsigned i;
    for (i = 0; i < npages; i++)
        if (pages[i] == p)
            return;
    if (npages < MAXP)
        pages[npages++] = (uint8_t)p;
}

static void pages_clear(void)
{
    unsigned i;
    for (i = 0; i < npages; i++)
        memset(&cpu.mem[(size_t)pages[i] * XL_BANK_SIZE], 0, XL_BANK_SIZE);
    npages = 0;
}

/* The dispatcher's translated entry.  This program executes through the interpreter
 * (XLATE_INTERP=1), so it is never reached; it exists so an interpreter-only link
 * resolves.  Weak, because the translated-side build of this same file links a real
 * generated `xlate_step()` from the generated cases C file and must win. */
__attribute__((weak)) int xlate_step(Cpu *c) { (void)c; return 0; }

int main(int argc, char **argv)
{
    char line[65536];
    unsigned long case_no = 0;
    FILE *out = stdout;

    xl_reset(&base);
    base.log = NULL;

    if (argc > 1) {
        out = fopen(argv[1], "w");
        if (!out) {
            fprintf(stderr, "vec_host: cannot write %s\n", argv[1]);
            return 64;
        }
    }

    while (fgets(line, sizeof line, stdin)) {
        char *p = line, *end;
        unsigned v[11];
        unsigned long long mpr;
        unsigned nram, i;
        unsigned long long cyc0;
        int ok;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\n' || *p == '\0')
            continue;

        for (i = 0; i < 6; i++) {
            v[i] = (unsigned)strtoul(p, &end, 16);
            if (end == p) {
                fprintf(stderr, "vec_host: bad field %u in case %lu\n", i, case_no);
                return 65;
            }
            p = end;
            while (*p == ' ' || *p == '\t')
                p++;
        }
        mpr = strtoull(p, &end, 16);        /* eight MPR bytes, one 64-bit word */
        if (end == p) {
            fprintf(stderr, "vec_host: missing MPR in case %lu\n", case_no);
            return 65;
        }
        p = end;
        while (*p == ' ' || *p == '\t')
            p++;
        nram = (unsigned)strtoul(p, &end, 10);
        if (end == p) {
            fprintf(stderr, "vec_host: missing RAM count in case %lu\n", case_no);
            return 65;
        }
        p = end;

        memcpy(&cpu, &base, offsetof(Cpu, mem));
        memcpy((char *)&cpu + offsetof(Cpu, mem) + XL_PHYS_SIZE,
               (const char *)&base + offsetof(Cpu, mem) + XL_PHYS_SIZE,
               sizeof(Cpu) - offsetof(Cpu, mem) - XL_PHYS_SIZE);
        cpu.a = (uint8_t)v[0];
        cpu.x = (uint8_t)v[1];
        cpu.y = (uint8_t)v[2];
        cpu.s = (uint8_t)v[3];
        cpu.p = (uint8_t)v[4];
        cpu.pc = (uint16_t)v[5];
        for (i = 0; i < 8; i++)
            cpu.mpr[i] = (uint8_t)((mpr >> (8 * (7 - i))) & 0xFF);

        for (i = 0; i < nram; i++) {
            unsigned a, val;
            while (*p == ' ' || *p == '\t' || *p == ',')
                p++;
            a = (unsigned)strtoul(p, &end, 16);
            if (end == p || *end != ':') {
                fprintf(stderr, "vec_host: bad RAM pair %u in case %lu\n", i, case_no);
                return 65;
            }
            p = end + 1;
            val = (unsigned)strtoul(p, &end, 16);
            if (end == p) {
                fprintf(stderr, "vec_host: bad RAM value %u in case %lu\n", i, case_no);
                return 65;
            }
            p = end;
            if (a >= XL_PHYS_SIZE) {
                fprintf(stderr, "vec_host: RAM address $%X past 2 MB\n", a);
                return 65;
            }
            page_use(a / XL_BANK_SIZE);
            cpu.mem[a] = (uint8_t)val;
        }

        cyc0 = (unsigned long long)cpu.cycles_charged;
#ifdef XLATE_BUS_REC
        xl_bus_begin();
#endif
        ok = xl_cpu_step(&cpu);

        fprintf(out, "CASE %lu A=%02X X=%02X Y=%02X S=%02X P=%02X PC=%04X "
                     "MPR=%02X%02X%02X%02X%02X%02X%02X%02X CYC=%llu REFUSED=%d NBUS=%u BUS=",
                case_no, cpu.a, cpu.x, cpu.y, cpu.s, cpu.p, cpu.pc,
                cpu.mpr[0], cpu.mpr[1], cpu.mpr[2], cpu.mpr[3],
                cpu.mpr[4], cpu.mpr[5], cpu.mpr[6], cpu.mpr[7],
                (unsigned long long)cpu.cycles_charged - cyc0, ok ? 0 : 1,
#ifdef XLATE_BUS_REC
                xl_bus_count()
#else
                0u
#endif
                );
#ifdef XLATE_BUS_REC
        for (i = 0; i < xl_bus_count(); i++) {
            uint32_t a;
            uint8_t val;
            int wr;
            xl_bus_get(i, &a, &val, &wr);
            page_use(a / XL_BANK_SIZE);
            fprintf(out, "%s%X:%02X:%c", i ? "," : "", a, val, wr ? 'w' : 'r');
        }
#endif
        fputc('\n', out);
        fflush(out);

        pages_clear();
        case_no++;
    }

    if (out != stdout)
        fclose(out);
    return 0;
}
