/* case_host.c -- the translator half of the vector check.
 *
 * Runs the SAME single-step vector state twice, in one process:
 *
 *   INTERP      xl_interp_step() -- the interpreter, called directly
 *   TRANSLATED  xl_cpu_step()    -- the dispatcher, i.e. the recompiled C
 *
 * and prints the resulting registers, the cycle delta and the bus activity for each.
 * The initial state and the instruction bytes come from `out/cases/vec.c`, which
 * tools/vec_xlate.py generates from sampled published vectors; this program does not
 * know what an opcode is.
 *
 * Why both in one process: `XLATE_INTERP` is cached in a static inside xl_cpu_step(),
 * so a forced-interpreter run and a translated run cannot share a process through the
 * environment.  Calling xl_interp_step() directly is the same instruction path the
 * switch selects, without that static.
 *
 * INTERPSTEPS on the TRANSLATED line is the machine's own count of instructions that
 * fell back to the interpreter: non-zero means the dispatcher had no translated unit
 * for this address (an undefined opcode, or a unit the descent refused), and the case
 * proves nothing about the translator.  It is printed rather than inferred.
 */
#include "runtime.h"

#include <stdio.h>
#include <string.h>

const XlateCase *xlate_cases(unsigned *count);

static Cpu cpu;

static void dump(const char *tag, unsigned id, unsigned long long cyc0, int ok)
{
    unsigned i;
    printf("CASE %u %s A=%02X X=%02X Y=%02X S=%02X P=%02X PC=%04X "
           "MPR=%02X%02X%02X%02X%02X%02X%02X%02X CYC=%llu REFUSED=%d NBUS=%u "
           "INTERPSTEPS=%u CRCMEM=%08X BUS=",
           id, tag, cpu.a, cpu.x, cpu.y, cpu.s, cpu.p, cpu.pc,
           cpu.mpr[0], cpu.mpr[1], cpu.mpr[2], cpu.mpr[3],
           cpu.mpr[4], cpu.mpr[5], cpu.mpr[6], cpu.mpr[7],
           (unsigned long long)cpu.cycles_charged - cyc0, ok ? 0 : 1,
           xl_bus_count(), (unsigned)cpu.interp_steps, xl_crc_phys(&cpu));
    for (i = 0; i < xl_bus_count(); i++) {
        uint32_t a;
        uint8_t v;
        int wr;
        xl_bus_get(i, &a, &v, &wr);
        printf("%s%X:%02X:%c", i ? "," : "", a, v, wr ? 'w' : 'r');
    }
    putchar('\n');
    fflush(stdout);
}

int main(void)
{
    unsigned n = 0, i;
    const XlateCase *cs = xlate_cases(&n);

    printf("CASES %u\n", n);
    for (i = 0; i < n; i++) {
        unsigned long long cyc0;
        int ok;

        memset(&cpu, 0, sizeof cpu);
        cs[i].init(&cpu);
        xl_speed_set(&cpu, 1);   /* these cases are one instruction from a
                                  * normal-speed machine; the zeroed Cpu would
                                  * otherwise leave `speed` and `speed_shift` the
                                  * two sides of one register disagreeing */
        cpu.log = NULL;
        cyc0 = (unsigned long long)cpu.cycles_charged;
        xl_bus_begin();
        ok = xl_interp_step(&cpu);
        dump("INTERP", cs[i].id, cyc0, ok);

        memset(&cpu, 0, sizeof cpu);
        cs[i].init(&cpu);
        xl_speed_set(&cpu, 1);   /* these cases are one instruction from a
                                  * normal-speed machine; the zeroed Cpu would
                                  * otherwise leave `speed` and `speed_shift` the
                                  * two sides of one register disagreeing */
        cpu.log = NULL;
        cyc0 = (unsigned long long)cpu.cycles_charged;
        xl_bus_begin();
        ok = xl_cpu_step(&cpu);
        dump("TRANSLATED", cs[i].id, cyc0, ok);
    }
    return 0;
}
