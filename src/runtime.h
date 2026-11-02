/* runtime.h -- CPU state and typed helpers for statically recompiled HuC6280 code.
 *
 * The generated C never does flag arithmetic inline.  It is a flat sequence of
 * one helper call per instruction, e.g.
 *
 *     static void i_3008(Cpu *c) {   // lda #$10
 *         xl_lda(c, 0x10);
 *         c->p &= ~XL_T;
 *         c->pc = 0x300A;
 *     }
 *
 * so that the semantics live in one readable place instead of being smeared
 * across a megabyte of generated code.
 *
 * Correctness over speed in this pass: every memory access goes through a
 * function pointer-free helper with bounds-safe bank arithmetic, hardware is
 * detected by bank number, and nothing is cached.
 */
#ifndef XLATE_RUNTIME_H
#define XLATE_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "video.h"

#define XL_BANK_SIZE    8192
#define XL_BANK_COUNT   256
#define XL_PHYS_SIZE    (XL_BANK_SIZE * XL_BANK_COUNT)

#define XL_IO_BANK      0xFF
#define XL_CDRAM_BASE   0x80
#define XL_ROM_BANKS    32
#define XL_WORKRAM_BANK 0xF8
#define XL_ZP_BASE      0x2000
#define XL_STACK_BASE   0x2100

/* The CD unit's backup RAM: 2 KB of battery-backed SRAM, the machine's save store.
 * It is a physical bank of its own ($F7), mapped by the card's `bm_open` (ROM
 * $FC49: save the MPRs, `lda #$f7 / tam #$04`, `lda #$80 / sta bram_unlock`) and
 * unmapped again by `bm_close` ($FC7F).  Two hardware rules make it a device and
 * not a byte array:
 *
 *   - it is WRITE-PROTECTED from power-on; only a store of $80 to $1807 clears the
 *     protection (the tail of `bm_open`, $FC49-$FC7E: `lda #$80 / sta bram_unlock`),
 *     and any read of $1803 sets it again -- which is why the card's close path
 *     `bm_close` ($FC7F) is `lda bram_lock`, i.e. a READ of $1803, and not a store
 *     (syscard/bank0.asm labels: `bram_lock = $1803`, `bram_unlock = $1807`);
 *   - it SURVIVES a reset: that is what "battery-backed" means, and it is why the
 *     console corpus had to replay the run's five resets on one machine (its save
 *     file is created before the first reset and read after it). */
#define XL_BRAM_BANK    0xF7
#define XL_BRAM_SIZE    2048
#define XL_BRAM_LOCK    0x80            /* the value $1807 takes to unlock */

/* P register bits. N/V/T/B align with the value's bit 7/6 - that is why BIT and
 * TST can copy them with a single AND. */
enum {
    XL_C  = 0x01,
    XL_Z  = 0x02,
    XL_I  = 0x04,
    XL_D  = 0x08,
    XL_BF = 0x10,
    XL_T  = 0x20,
    XL_V  = 0x40,
    XL_N  = 0x80
};

/* Block-transfer kinds for xl_bmt. */
enum { XL_TII = 0, XL_TDD = 1, XL_TIN = 2, XL_TIA = 3, XL_TAI = 4 };

/* Why xl_run / xlate_step stopped. */
enum { XL_OK = 0, XL_STOPPED_AT_PC = 1, XL_UNTRANSLATED = 2, XL_TRAPPED = 3,
       XL_OUT_OF_STEPS = 4 };

/* HuC6280 timer at $0C00-$0C01: 7-bit reload, counts down from the CPU clock
 * divided by 1024, raises the dedicated timer interrupt when it wraps. */
typedef struct Timer {
    uint8_t load, value, status;
    uint32_t div;
} Timer;

/* CD drive interface registers, $1800-$180F in the I/O page.  The contracts are
 * read off the kernel's own driver rather than assumed:
 *   $5CBD  `lda $180C; bmi` then `lda $180A`      -> bit 7 clear = a byte is ready
 *   $5D2B  sets/clsd $180D bit 3 around a read    -> data strobe
 *   $5CE1  $180E = mode, $1808/$1809 = dest,      -> transfer setup
 *          $1802 bit 3 and $180D = $60 start it
 *   $F347  `lda $1800; and #$80`                  -> bit 7 = drive busy
 *   $F393  waits for $1803 bit 5 and $180C bit 2  -> command completion
 *
 * $1803 IS A REGISTER, NOT A SUMMARY OF "IS THE DRIVE IDLE".  It holds the
 * drive's interrupt/source bits, and the bits are events that latch and clear:
 *
 *   bit 1  toggles on every read (the left/right select the drive hands back;
 *          the reference core's `PCECD_Read` case 0x3 does `_Port[3] ^= 2`)
 *   bit 2  ADPCM half reached      (latched by the ADPCM engine, $180C bit 0's
 *                                   sibling -- see adpcm_half in Cd)
 *   bit 3  ADPCM end reached
 *   bit 4  a subchannel byte is available (set when the drive stuffs one, cleared
 *          by a read of $1807 when it has none left)
 *   bit 5  SCSI data transfer DONE  -- set when the drive has finished handing
 *          over data and is offering status, cleared when the bus goes free or
 *          when the host selects ($1800 write) or resets ($1804 bit 1)
 *   bit 6  SCSI data transfer READY -- set while a data byte is on the bus,
 *          cleared when the FIFO empties
 *
 * and the IRQ2 line is exactly `(($1802 & $1803) & $7C) != 0` -- the reference
 * core's `update_irq_state()`.  Deriving bit 5 from "no command is open" (what
 * this port used to do) makes it permanently true, which is the save menu's
 * first divergence: the card's `ad_stat` ($F6DB) reads it and answers X=0 where
 * the console answers X=4.
 *
 *   $180C is the ADPCM engine's own status:
 *   bit 0 end reached, bit 2 a byte write is pending, bit 3 playing,
 *   bit 7 a byte read is pending.
 *
 *   $180D is not a write-through latch: a read returns `adpcm_lastcmd`, the last
 *   control byte written (0 after a D7 reset), which is why the card's `ad_stat`
 *   reads $60 while a sample plays and $00 after the BIOS clears it at $E853.
 */
/* The drive's phases, as the byte in $1800 encodes them.  Bits 7..3 are
 * BSY/REQ/MSG/CD/IO, exactly as the SCSI bus defines them and as the card's own
 * driver tests them (`and #$f8` then compare with $D0/$D8/$C8/$88):
 *
 *   $00  bus free (BSY clear)              $90  command, no byte wanted
 *   $D0  command, byte wanted (REQ)        $88  data in, no byte ready
 *   $C8  data in, byte ready (REQ)         $98  status, no byte wanted
 *   $D8  status, byte wanted               $B8  message in, no byte wanted
 *   $F8  message in, byte wanted
 *
 * Measured, not assumed: the console's access trace records every one of
 * those values at the pc that read it (f=1701: $D0 while the six command bytes go
 * in, then $90 for three frames of seek, then $C8 while a sector streams, then
 * $D8/$98/$F8/$B8/$00 as the result is taken). */
enum {
    CD_PHASE_BUS_FREE = 0,
    CD_PHASE_COMMAND  = 1,
    CD_PHASE_DATA_IN  = 2,
    CD_PHASE_STATUS   = 3,
    CD_PHASE_MESSAGE  = 4,
    /* The drive holds the bus with BSY alone while it works on something that
     * takes longer than a command handshake -- an audio seek, or playback running
     * to its stop position.  Geargrafx's SCSI_PHASE_BUSY; $1800 reads $80
     * (`GetStatus()` = `signals & 0xF8`, and `SetPhase(BUSY)` sets BSY only). */
    CD_PHASE_BUSY     = 5
};

/* Why the drive is holding the bus, which decides what releases it. */
enum {
    CD_BUSY_NONE = 0,
    CD_BUSY_ON_SEEK = 1,               /* elapsed clock: an audio seek            */
    CD_BUSY_ON_AUDIO_END = 2           /* the head reached the programmed end LBA */
};

/* What the data-in phase is carrying. */
enum {
    CD_DATA_NONE    = 0,
    CD_DATA_RESP    = 1,               /* a few bytes: Get Dir Info, sense */
    CD_DATA_SECTORS = 2                /* a Read(6): one 2 KB sector per frame */
};

typedef struct Cd {
    uint8_t  irq_mask;                 /* $1802 bits 0..6: which sources raise IRQ2 */
    uint8_t  irq_status;               /* the drive's active IRQ sources */
    uint8_t  control;                  /* $180D */
    uint8_t  mode;                     /* $180E */
    uint8_t  last_cmd;                 /* $1800 (write) */
    uint8_t  status_b;                 /* $180B */
    uint16_t dest;                     /* $1808/$1809 */
    uint8_t  buf[2048];                /* the sector presented on $180A / $1808 */
    unsigned len, pos;                 /* bytes held / next one to hand out */
    uint32_t bytes_out;
    uint32_t sectors_queued;
    unsigned next_sector;              /* disc sector the port is streaming */
    unsigned sectors_left;

    /* ---- the command decoder: the drive's own side of $1800-$180F ------- *
     * The card is a real driver: it selects the drive, writes a command byte by
     * byte against REQ/ACK, and then reads the answer out of the same registers.
     * These fields are that
     * conversation.  They are only exercised when the card is executed
     * (XLATE_BIOS_EXEC): with the models in charge nothing reads $1800-$180F,
     * because every such access in the console trace comes from card code. */
    uint8_t  phase;                    /* CD_PHASE_* */
    uint8_t  db;                       /* the byte on the drive's data bus */
    uint8_t  host_db;                  /* the byte the host put on $1801 */
    uint8_t  bus_reset;                /* $1804: last byte written, read back */
    uint8_t  ack;                      /* $1802 bit 7, as written */
    uint8_t  req;                      /* a byte is wanted/offered */
    uint8_t  consumed;                 /* ACK rose on a requested byte */
    uint8_t  cmd[10];                  /* the command, as it arrives */
    unsigned cmd_len, cmd_need;
    uint8_t  resp[18];                 /* short response bytes */
    unsigned resp_len, resp_pos;
    uint8_t  status_byte;              /* what STATUS phase hands over */
    uint8_t  sense_key, sense_asc;     /* last error, for REQUEST SENSE */
    /* the Read(6) in flight */
    unsigned read_lba, read_count, read_served;
    uint8_t  data_kind;                /* CD_DATA_* */
    uint8_t  data_have;                /* the current sector/response byte is ready */
    unsigned data_pos;                 /* bytes handed out of `buf` */
    uint64_t read_start;               /* the accept instant, in MASTER-CLOCK units
                                        * (3 x c->cycles) -- see XL_CD_PERIOD_MASTER */
    uint32_t read_frame;               /* the frame of that same moment, for the log */
    uint32_t blocked_fills;            /* fills deferred because the FIFO was full */
    /* WHERE THE HEAD IS, in LBA units: the sector the drive last served (0 = the
     * lead-in, i.e. cold).  The next command's seek time is measured from here --
     * see `xl_gg_seek_ms` in runtime.c.  This is a property of the mechanism,
     * not of any one emulator: a drive cannot reach a sector it is not over. */
    uint32_t head_lba;
    /* WHEN THE SPINDLE IS AT SPEED, in master clocks since power-on (0 in a machine
     * that has already spun up).  Before this instant the drive answers every
     * command except REQUEST SENSE with CHECK CONDITION / sense 2 ($04, NOT READY),
     * which is what the System Card's boot retries on every 70 frames.  The contract
     * and the evidence are at the top of cd_dispatch() in runtime.c. */
    uint64_t ready_at;
    /* The drive's countdown to the next sector, in MASTER-CLOCK units: the core's
     * CDReadTimer.  See XL_CD_PERIOD_MASTER in runtime.c. */
    uint64_t next_fill;
    /* When the sector the drive took for itself has finished LEAVING the FIFO, in
     * master clocks (0 = nothing in flight).  A drive-side (ADPCM) sector is not
     * finished when it lands: its 2,048 bytes still have to be clocked out one at a
     * time, and "transfer done" ($1803 bit 5) is raised by the last byte, not by the
     * fill.  See XL_CD_DRAIN_MASTER in runtime.c. */
    uint64_t drain_end;
    /* counters, reported once by xl_cd_summary() */
    unsigned long cmds, cmds_ok, cmds_bad;
    unsigned long op_count[256];
    unsigned long op_bad[256];

    /* ---- $1803: the drive's status register (see the contract above) ------ *
     * The bits are kept here, not derived on the fly, because they are events:
     * the SCSI transfer bits latch until the bus goes free, and the ADPCM bits
     * are the sample engine's own flags.  bit 1 is toggled by the read itself. */
    uint8_t  port3;
    uint8_t  cdda_playing;             /* CD-DA running: the drive stuffs subchannel
                                        * bytes, which is what sets $1803 bit 4.
                                        * A $D8 with a nonzero second byte starts it;
                                        * it is not stopped by the pause at $7590 on
                                        * the console either -- the FIFO keeps the
                                        * flag set until something reads $1807. */
    uint8_t  subq_byte;                /* last subchannel byte handed out */

    /* ---- CD-DA: the drive's audio head (see the CD-DA contract in runtime.c) --- *
     * The port has no mixer and produces no sound.  What it models is everything the
     * card and the game can READ BACK: the play/pause/stop state, the position the
     * head has reached, and the events the drive raises from it.  That is the same
     * division as the ADPCM engine above: no audio, every visible bit. */
    uint8_t  cdda_status;              /* 0 stopped, 1 playing, 2 paused            */
    uint8_t  cdda_mode;                /* 0 silent, 1 loop, 2 interrupt, 3 normal   */
    uint32_t cdda_start;               /* play start LBA (SAPSP)                    */
    uint32_t cdda_end;                 /* play end LBA   (SAPEP)                    */
    uint32_t cdda_pos;                 /* sector the head is reading now            */
    uint64_t cdda_next;                /* master clock of the next 1/75 s boundary  */
    uint16_t cdda_vol_l, cdda_vol_r;   /* $1805/$1806: raw PCM level of the last read */
    uint8_t  subq[10];                 /* the Q channel the drive reports           */

    /* THE BUSY PHASE -- Geargrafx's SCSI_PHASE_BUSY, which this drive had no way
     * to enter.  The console takes it on `$D8` while the audio seek runs and on
     * `$D9` modes 1/2 until playback reaches the stop position; the game waits on
     * it (measured: 36 frames of the vblank handler with `$2032` frozen), so
     * without it the port runs the game 36 game ticks ahead.  See the `$D8` case
     * in cd_dispatch(). */
    uint8_t  busy_release;             /* CD_BUSY_*                                 */
    uint64_t busy_until;               /* CD_BUSY_ON_SEEK: the master clock it ends */

    /* ---- the ADPCM sample engine: GEARGRAFX'S MODEL (see the contract in
     * runtime.c).  Field names follow `adpcm.h` so a reader can check each one
     * against the source without translation. */
    uint8_t  adpcm_ram[0x10000];       /* m_adpcm_ram: the chip's 64 KB sample RAM */
    uint8_t  adpcm_read_value;         /* m_read_value: what $180A hands back     */
    uint8_t  adpcm_write_value;        /* m_write_value: the byte heading to RAM  */
    int32_t  adpcm_read_cycles;        /* m_read_cycles, master clocks            */
    int32_t  adpcm_write_cycles;       /* m_write_cycles, master clocks           */
    int32_t  adpcm_dma_cycles;         /* m_dma_cycles: the drive's pull, 36       */
    uint16_t adpcm_readaddr;           /* m_read_address                          */
    uint16_t adpcm_writeaddr;          /* m_write_address                         */
    uint16_t adpcm_addr;               /* m_address ($1808/$1809)                 */
    uint8_t  adpcm_lastcmd;            /* m_control  ($180D's last write)         */
    uint8_t  adpcm_playing;            /* m_playing                               */
    uint8_t  adpcm_play_pending;       /* m_play_pending                          */
    uint8_t  adpcm_playnibble;         /* m_nibble_toggle                         */
    uint8_t  adpcm_half;               /* m_half_irq  -> $1803 bit 2              */
    uint8_t  adpcm_end;                /* m_end_irq   -> $1803 bit 3, $180C bit 0 */
    uint8_t  adpcm_freq;               /* m_sample_rate ($180E, raw)              */
    uint32_t adpcm_len;                /* m_length: 17-bit (adpcm_inline.h:183)   */
    uint32_t adpcm_cycles_per_sample;  /* m_cycles_per_sample, 16.16 fixed point  */
    int64_t  adpcm_sample_acc;         /* m_adpcm_cycle_counter                   */
    uint64_t adpcm_last;               /* the port's own clock bookkeeping        */
} Cd;

/* Interrupt sources this port dispatches, named for the source rather than the
 * vector bit: IRQ2 is the CD unit's line, IRQ1 is the VDC's. */
enum { XL_IRQ_CD = 0, XL_IRQ_VDC = 1, XL_IRQ_TIMER = 2 };

/* The six interrupt hook slots.  $2200-$220B, **two bytes each**: a 16-bit handler
 * address and nothing else.  The order is the documented one and is the order of the
 * CPU's vectors, low to high:
 *
 *   $2200  CD             IRQ2          vector $FFF6   (the drive's line)
 *   $2202  video          IRQ1          vector $FFF8   (VDC: vblank, raster, collision)
 *   $2204  timer                        vector $FFFA   (the HuC6280's on-chip timer)
 *   $2206  NMI                          vector $FFFC
 *   $2208  raster sync    SYNC          (BIOS-dispatched, not a CPU vector)
 *   $220A  raster compare RCR           (BIOS-dispatched, not a CPU vector)
 *
 * How the layout is known, rather than assumed:
 *   - the BIOS's own installer at $F034 (called as entry $E05D) does `asl a; tax`
 *     and then `sta $2200,x` *twice*, so index N writes $2200+2N and $2201+2N: two
 *     bytes per slot, no bank byte;
 *   - the CPU vectors at $FFF6-$FFFC hold `jmp ($2200)`, `jmp ($2202)`, ... in this
 *     order (Hudson's manual, and the ROM image itself);
 *   - the console's own work RAM at frames 1748/1800/1837 holds six complete two-byte
 *     $E8E2 defaults at $2200-$220B, i.e. six slots of two bytes;
 *   - at the frame-9835 stop the slots read $E2E8 $4177 $4297 $E8E2 $E8E2 $E8E2, and
 *     $4177/$4297 are the game's video and timer handlers, both in bank $69.
 * Sources: Hudson's System Card BIOS manual (hook slots), and the console's own
 * work RAM.
 *
 * THIS PORT USED TO READ THREE BYTES PER SLOT, taking byte +2 as a bank number.  It
 * was never a bank: it is the *next* slot's address low byte.  With the game's timer
 * handler at $2204 the video slot looked like "bank $97", which never matched the
 * mapped bank and produced 8,298 bogus warnings in the 2G-cycle run.  Do not
 * reintroduce a bank comparison here. */
#define XL_HOOK_CD     0x2200u
#define XL_HOOK_VDC    0x2202u
#define XL_HOOK_TIMER  0x2204u
#define XL_HOOK_NMI    0x2206u
#define XL_HOOK_SYNC   0x2208u
#define XL_HOOK_RCR    0x220Au

/* The interrupt controller's two registers, as bits of $1402 (mask, inverted:
 * 1 = disabled) and $1403 (status, 1 = pending).  The bit position is the same in
 * both registers and names the source:
 *
 *   bit 0  IRQ2  the CD unit's line (ADPCM / data transfer)
 *   bit 1  IRQ1  the VDC's line (vertical blank, raster, sprite collision/overflow)
 *   bit 2  TIMER the HuC6280's on-chip timer
 *
 * vec:  Hudson, HuC6280 hardware manual p.H8-12 (vector table, per-source bits);
 *       Charles MacDonald, pcetech.txt (measured register layout, the inversion);
 *       Mednafen pce.cpp IRQStatusRead/IRQStatusWrite (same, bit for bit).
 *
 * This is NOT $0C02/$0C03: those are the timer's reload and control registers,
 * mirrored across $0C00-$0FFF.  Reading the mask from $0C02 leaves the game's own
 * mask writes with no effect. */
#define XL_IQ_IRQ2  0x01u
#define XL_IQ_IRQ1  0x02u
#define XL_IQ_TIMER 0x04u
#define XL_IQ_ALL   0x07u

typedef struct Cpu {
    uint8_t  a, x, y, s, p;
    uint16_t pc;
    uint8_t  mpr[8];

    uint8_t  mem[XL_PHYS_SIZE];

    /* Video hardware.  VRAM and the palette live here so translated code reaches
     * them exactly the way it reached the chips: through the $FF bank ports. */
    Vdc      vdc;
    Vce      vce;
    Timer    timer;

    /* The CPUs interrupt controller: $1402 (mask) and $1403 (status) in the I/O
     * page -- not $0C02/$0C03, which belong to the timer.  See XL_IQ_* above. */
    uint8_t  irq_status;         /* $1403 read: XL_IQ_IRQ2 / XL_IQ_IRQ1 / XL_IQ_TIMER */
    uint8_t  irq_mask;           /* $1402: 1 = disabled, as written by the program */
    /* Synthetic controllers.  One byte per pad, active high: bit0 I, bit1 II,
     * bit2 SELECT, bit3 RUN, bit4 UP, bit5 RIGHT, bit6 DOWN, bit7 LEFT.  The
     * hardware port is active low and returns one nibble at a time; the BIOS pad
     * routine ($E49A, called through $E063) converts and keeps the tables at
     * $2228 (current), $222D (newly pressed), $2232 (previous). */
    uint8_t  pad[5];
    uint8_t  joypad_sel;         /* SEL: bit0 of the last value written to $1000 */
    /* THE MULTITAP COUNTER.  The machine the corpus came from has a five-player
     * multitap, and the reference core models it exactly (input.cpp, INPUT_Read /
     * INPUT_Write with MultiTapEnabled):
     *
     *   read  $1000-$13FF  ->  pad[read_index], or $0F when read_index > 4
     *   write $1000-$13FF  ->  read_index = 0   if SEL goes high while CLR goes 0->1
     *                          read_index++     if SEL goes 0->1 while CLR is low
     *                          (SEL = V bit 0, CLR = V bit 1)
     *
     * That counter, not the port address, is what selects a controller: the BIOS
     * pad routine ($E49A) writes $01 and reads twice per pad and never touches
     * $1001-$1004, so without it every one of its five iterations answers with
     * pad 1's state.  Measured at frame 10,818 against the corpus: work RAM
     * $2228-$222C held $08 $08 $08 $08 $08 where the console holds $08 $00 $00
     * $00 $00 -- a RUN press reported on five controllers instead of one. */
    uint8_t  joypad_clr;         /* CLR: bit1 of the last value written to $1000 */
    uint8_t  joypad_index;       /* read_index, 0..5 (5 = no controller) */
    /* Timed input: press windows expressed in emulated frames.  The pad state is
     * applied at frame boundaries, so "newly pressed" sees a clean edge. */
    /* Input windows, one per press.  Sized for a long run -- every recorded input
     * of an hour-long input schedule fits, so a table that silently drops the
     * tail cannot be mistaken for a machine that stopped listening. */
    struct { uint32_t from, to; uint8_t mask; } pads[8192];
    int      npads;
    /* Input source 2: the console's own held state, one entry per frame.  Five
     * bytes per frame (one per controller, active high, same bit order as
     * `pad`), produced by tools/lockstep/pad_from_ref.py from the reference
     * capture's own pad latch at $2228-$222C.  When set it replaces the window
     * table above, and the experiment becomes "given the buttons the console
     * physically held, does this machine do the same thing" instead of "was the
     * fixture's press frame the same as the console's".  Held, not edges: a
     * one-frame press is held across a whole frame's worth of polls, which is
     * the only shape that survives a poll sliding over a frame boundary. */
    const uint8_t *pad_hold;     /* pad_hold_n * 5 bytes */
    uint32_t pad_hold_n;         /* frames covered by pad_hold */
    uint32_t pad_hold_from;      /* the frame number of pad_hold[0] */
    int32_t  pad_hold_lag;       /* see xl_pad_stream: entries are this many
                                  * frames earlier than the port's frame number */
    uint64_t cycles;             /* CPU cycles executed, at the normal-speed rate:
                                  * master clocks / 3.  See xl_tick(). */
    /* The same running total in the OTHER unit: straight ADDCYC cycles, unscaled by
     * the speed mode.  Two counters, because they answer two different questions and
     * both are asked:
     *
     *   `cycles`          how much TIME has passed -- the master clock divided by
     *                     three.  The trace stamps accesses with `cycles * 3`, the CD
     *                     deadlines are master clocks, and a slow-mode instruction
     *                     buys four times as much of it per charged cycle.
     *   `cycles_charged`  how many cycles the program was CHARGED -- the published
     *                     per-opcode counts (SingleStepTests `num_cycles`) and the
     *                     differential test's cost table are in this unit, and they
     *                     are speed-independent by definition.
     *
     * They differ only across a slow-mode window: `CSL` charges 2 cycles at the old
     * rate and 1 at the new, so it adds 3 here and 6 to `cycles`.  Nothing reads
     * `cycles_charged` except the vector harnesses (tools/vec_host.c,
     * tools/case_host.c), which are comparing against the published counts. */
    uint64_t cycles_charged;
    /* ---- THE CPU'S SPEED MODE ($54 CSL / $D4 CSH) ---------------------------- *
     *
     * The HuC6280 runs its bus at two clock rates: 7.15909 MHz (master 21.47727 / 3)
     * normally, and 1.78977 MHz (master / 12) in "slow" mode -- a factor of four.
     * `CSL` selects slow, `CSH` selects normal.  These two bytes are the reference
     * core's own state, name for name (mednafen pce/huc6280.h:301-302,
     * `uint8 speed; uint8 speed_shift_cache;`), and `speed_shift` holds exactly what
     * `REDOSPEEDCACHE()` puts there:
     *
     *     speed_shift_cache = (speed ^ 1) << 1;   // 0 normal, 2 slow
     *
     * and every cycle an instruction charges is converted to master clocks by
     *
     *     int master = (x * 3) << speed_shift_cache;   // huc6280.h:227 ADDCYC()
     *
     * so one charged cycle is three master clocks normally and twelve in slow mode.
     * `xl_tick()` applies that conversion to the whole instruction at once; see the
     * derivation there.  Nothing else in this machine may assume 3. */
    uint8_t  speed;              /* 1 = 7.16 MHz (normal), 0 = 1.79 MHz (slow) */
    uint8_t  speed_shift;        /* 0 or 2: the log2 of the slow-mode multiplier */
    uint32_t irq_taken;
    uint32_t irq_kind[3];        /* taken, by source: [XL_IRQ_CD], [VDC], [TIMER] */
    uint32_t vblank_seen;
    uint32_t frames;             /* last frame counter seen by the run loop */
    /* Interrupt handlers live in work RAM, one 16-bit address per slot, six slots
     * at $2200-$220A (XL_HOOK_*): CD/IRQ2, video/IRQ1, timer, NMI, SYNC, RCR.  There
     * is no bank byte.  See README "interrupt contract" -- this is where the port
     * stands in for the CD BIOS, which is not translated. */
    uint8_t  in_irq;
    uint8_t  untranslated_stop;   /* the run ended on a bank with no code */
    uint8_t  boot_game_on_run;    /* load the game when RUN is newly pressed */
    uint8_t  boot_pressed;        /* RUN was accepted; wait for pc $41E9 */
    uint8_t  boot_gate_open;      /* the press has released the disc boot */

    /* A RESET IS NOT AN INSTRUCTION, and this is the one byte that says so.
     *
     * The frame boundary is crossed *inside* an instruction: xl_tick() is called
     * from the instruction's own body (translated units and the interpreter both
     * end with `xl_tick(...)`), so a console RESET scheduled at that boundary
     * happens with an instruction in flight.  A translated unit has already
     * written its next pc before it ticks and therefore survives; the
     * interpreter writes `c->pc = nxt` *after* its tick, so it overwrote the
     * reset vector and the machine carried on with the pre-reset pc under the
     * post-reset (all-zero) mapping -- measured: after the reset at frame 9,937
     * the run executed $5B6B with mpr=0000000000000000 instead of the card's
     * $E0F3, and work RAM stayed zero for the next 270 frames because every
     * zero-page store was landing in ROM.  The hardware does not have this
     * ambiguity: RESET aborts the instruction in flight and the CPU fetches the
     * vector.  So the vector is held here and re-asserted at the top of the next
     * step, which is exactly "the in-flight instruction did not complete". */
    uint16_t reset_resume;

    /* CD drive interface ($1800-$180F in the I/O page).  The contracts are read
     * off the kernel's own driver: bank $69 $5CBD is "wait for a byte"
     * (`lda $180C; bmi` -> bit 7 clear means a byte is ready, then `lda $180A`),
     * bank $69 $5D2B pulses $180D bit 3 around a data read, and bank $69
     * $5CE1-$5D0C loads $180E (mode), $1808/$1809 (destination), then $1802 bit 3
     * and $180D = $60 to start a transfer. */
    Cd       cd;

    /* The backup-RAM device (bank $F7): see XL_BRAM_BANK.  `bram_wprot` is 1 when
     * writes are blocked, which is the state the machine powers on in. */
    uint8_t  bram[XL_BRAM_SIZE];
    uint8_t  bram_wprot;
    uint32_t bram_blocked;        /* stores dropped by the write protect, counted */
    uint32_t bram_writes;         /* stores that landed */

    /* Hardware register page (bank $FF). Stored, counted, never acted on. */
    uint8_t  hw_regs[XL_BANK_SIZE];
    uint32_t hw_writes[XL_BANK_SIZE];
    uint32_t hw_reads[XL_BANK_SIZE];

    /* CD BIOS stub bookkeeping. */
    FILE    *log;
    const uint8_t *disc;
    size_t   disc_len;
    int      disc_attached;
    int      rom_present;
    uint32_t rom_writes_ignored;
    /* XLATE_BIOS_EXEC=1: the supplied System Card is EXECUTED instead of being
     * modelled.  Resolved once, in xl_reset().  With it
     * off -- the default -- every modelled BIOS behaviour in this file is reached
     * exactly as before; with it on, code in a ROM page runs on the interpreter and
     * the models are skipped. */
    uint8_t  bios_exec;
    /* XLATE_BIOS_RESET=1 (only under XLATE_BIOS_EXEC and with a card attached):
     * the machine is entered the way the console enters it -- at the card's reset
     * vector, with the power-on mapping -- so the card initialises the hardware and
     * loads the game itself instead of the port assembling that state by hand.
     * Off by default: the translated loader at $3000 stays the default entry.
     * See xl_boot_reset(). */
    uint8_t  bios_reset;
    /* Writes aimed at a ROM page are counted (rom_writes_ignored): the card is
     * read-only, so a store there changes nothing -- see xl_wr_phys. */

    /* Observation counters. */
    unsigned long steps;
    unsigned long interp_steps;      /* of those, how many the interpreter ran */
    unsigned long bios_interp_steps; /* of those, how many were System Card code */
    unsigned long bios_calls;
    unsigned long modelled_reads;
    unsigned long vblank_waits;
    unsigned long unmodelled_calls;
    unsigned long interp_untranslated_steps; /* of interp_steps: no translation existed */
    uint64_t     read_bytes;

    uint8_t  abackup;               /* T-flag pre/postfix scratch */
} Cpu;

/* ---- lifecycle ---------------------------------------------------------- */
void xl_reset(Cpu *c);
void xl_attach_disc(Cpu *c, const uint8_t *data, size_t len);
void xl_load_rom(Cpu *c, const uint8_t *rom, size_t len);
/* Load the battery-backed backup RAM (2 KB).  This is the machine's save data, an
 * input like the disc: a comparison that does not hand the machine the same card
 * is comparing two different consoles. */
void xl_load_sram(Cpu *c, const uint8_t *data, size_t len);
void xl_load_phys(Cpu *c, uint8_t bank, uint16_t off, const uint8_t *data, size_t len);
void xl_set_log(Cpu *c, FILE *f);
/* XLATE_RAMSTREAM: emit the power-on work-RAM block (block 0) at reset. */
void xl_ramstream_begin(Cpu *c);

/* ---- memory ------------------------------------------------------------- */
uint8_t  xl_rd(Cpu *c, uint16_t addr);
void     xl_wr(Cpu *c, uint16_t addr, uint8_t v);
uint8_t  xl_rd_phys(Cpu *c, uint8_t bank, uint16_t off);
void     xl_wr_phys(Cpu *c, uint8_t bank, uint16_t off, uint8_t v);
uint16_t xl_rd16(Cpu *c, uint16_t addr);

/* ---- harness-only bus recorder (tools/vec_host.c) ----------------------- *
 * Every CPU data/opcode access xl_rd()/xl_wr() makes, in order, as a physical
 * (bank << 13 | offset) address plus the byte and the direction.  It exists so a
 * single-step vector can be compared against the machine's own bus activity
 * instead of against the final state alone.
 *
 * Compiled in ONLY when -DXLATE_BUS_REC is defined; absent from every normal
 * build, and it never changes a value the machine returns or stores. */
#ifdef XLATE_BUS_REC
void     xl_bus_begin(void);
unsigned xl_bus_count(void);
void     xl_bus_get(unsigned i, uint32_t *phys, uint8_t *v, int *rw);
void     xl_bus_note(Cpu *c, uint16_t addr, uint8_t v, int wr);
#endif

/* Is the byte the PC points at part of the supplied System Card?  True only under
 * XLATE_BIOS_EXEC, only when a ROM is attached, and only while the MPRs actually map
 * a ROM page (physical bank $00-$1F) over the PC's window -- so a game bank mapped at
 * $E000 is game code, not BIOS, and is dispatched as before.
 *
 * The dispatcher asks this before it hands a pc to the model; xl_cpu_step() asks it
 * before it hands one to the dispatcher.  Cheapest possible form: one byte load and a
 * branch on the default path. */
static inline int xl_pc_in_rom(Cpu *c)
{
    return c->bios_exec && c->rom_present && c->mpr[c->pc >> 13] < XL_ROM_BANKS;
}

/* zero page is $2000|z on this CPU; index arithmetic wraps inside the page */
static inline uint16_t xl_zp(uint8_t z)            { return (uint16_t)(XL_ZP_BASE | z); }
static inline uint16_t xl_zp_x(Cpu *c, uint8_t z)  { return (uint16_t)(XL_ZP_BASE | (uint8_t)(z + c->x)); }
static inline uint16_t xl_zp_y(Cpu *c, uint8_t z)  { return (uint16_t)(XL_ZP_BASE | (uint8_t)(z + c->y)); }
/* The speed register is a cache, and this is the ONLY writer of it: `speed` and
 * `speed_shift` must never disagree (the reference core folds the same two into
 * REDOSPEEDCACHE() for the same reason).  Called by CSL/CSH -- once from the
 * interpreter, once from generated code -- and by the two reset entries. */
static inline void xl_speed_set(Cpu *c, int fast)
{
    c->speed = (uint8_t)(fast & 1);
    c->speed_shift = (uint8_t)((c->speed ^ 1u) << 1);
}
uint16_t xl_rd16_zp(Cpu *c, uint8_t z);

/* ---- stack -------------------------------------------------------------- */
void     xl_push(Cpu *c, uint8_t v);
uint8_t  xl_pop(Cpu *c);
void     xl_push16(Cpu *c, uint16_t v);
uint16_t xl_pop16(Cpu *c);

/* ---- trace -------------------------------------------------------------- */
/* The machine's own rce-trace v1 emitter: one record per I/O-page access, in the
 * same format a reference emulator's tracer writes (including the `c=` master-clock
 * field).  Off unless XLATE_TRACE_OUT names a file; every call below costs one
 * comparison when it is off.  The point of it is that a comparison against a
 * reference run becomes a diff instead of bespoke analysis:
 *
 *     trace_cmp.py ours.log reference.log
 *
 * What each call records:
 *   xl_trace_frame   the FRAME marker of the frame the CPU has just entered
 *   xl_trace_bus     one I/O-page access, exactly as it happened (the byte the
 *                    device returned / the byte written)
 *   xl_trace_cdcmd   the write to $1800, after its own bus line
 *   xl_trace_irq     an interrupt being taken, with its vector
 *   xl_trace_irqe    the RTI that returned from it
 */
void xl_trace_frame(Cpu *c);
void xl_trace_bus(Cpu *c, int wr, uint16_t la, uint8_t v);
void xl_trace_cdcmd(Cpu *c, uint8_t cmd);
void xl_trace_irq(Cpu *c, int kind, uint16_t vec);
void xl_trace_irqe(Cpu *c);
void xl_trace_summary(FILE *out);
/* Opcode width, from the generated table the interpreter and translator share
 * (interp.c).  Used by the trace for the console's `pc=` convention. */
unsigned xl_op_size(uint8_t op);
/* RTI.  This lives here, not inline in the translated code, so the trace sees the
 * return in the one place it can happen (generated code, the interpreter and this
 * file's own unwinding all call it) and so the three pops exist once. */
void xl_rti(Cpu *c);

/* ---- ALU / transfers (each sets exactly the flags the hardware sets) ----- */
void xl_lda(Cpu *c, uint8_t v);
void xl_ldx(Cpu *c, uint8_t v);
void xl_ldy(Cpu *c, uint8_t v);
void xl_adc(Cpu *c, uint8_t v);
void xl_sbc(Cpu *c, uint8_t v);
void xl_cmp(Cpu *c, uint8_t v);
void xl_cpx(Cpu *c, uint8_t v);
void xl_cpy(Cpu *c, uint8_t v);
void xl_and(Cpu *c, uint8_t v);
void xl_ora(Cpu *c, uint8_t v);
void xl_eor(Cpu *c, uint8_t v);
void xl_bit(Cpu *c, uint8_t v);
void xl_tst(Cpu *c, uint8_t memv, uint8_t imm);
uint8_t xl_asl(Cpu *c, uint8_t v);
uint8_t xl_lsr(Cpu *c, uint8_t v);
uint8_t xl_rol(Cpu *c, uint8_t v);
uint8_t xl_ror(Cpu *c, uint8_t v);
uint8_t xl_inc(Cpu *c, uint8_t v);
uint8_t xl_dec(Cpu *c, uint8_t v);
void xl_nz(Cpu *c, uint8_t v);

/* ---- mapper / hardware -------------------------------------------------- */
void xl_tam(Cpu *c, uint8_t mask);
void xl_tma(Cpu *c, uint8_t mask);
void xl_st0(Cpu *c, uint8_t v);
void xl_st1(Cpu *c, uint8_t v);
void xl_st2(Cpu *c, uint8_t v);
int xl_ram_trampoline(Cpu *c);
void xl_cd_prime(Cpu *c, unsigned record, unsigned sectors);
void xl_cd_summary(Cpu *c, FILE *out);       /* the drive's command census */
void xl_vramhi_note(Cpu *c, uint16_t pc, unsigned addr);
void xl_vramhi_summary(FILE *out);
void xl_bmt(Cpu *c, int kind, uint16_t src, uint16_t dst, uint16_t len);

/* ---- video, timing and interrupts --------------------------------------- */
void xl_tick(Cpu *c, unsigned cycles);       /* advance devices by CPU cycles */
void xl_service_irq(Cpu *c);                 /* take a pending interrupt if allowed */
void xl_set_pad(Cpu *c, unsigned port, uint8_t state);   /* see Cpu.pad */
void xl_pad_press(Cpu *c, uint32_t from_frame, uint32_t to_frame, uint8_t mask);
void xl_pad_tick(Cpu *c);                                /* apply the schedule */
/* Console-driven held input: `held` holds 5 bytes per frame, frame
 * `from_frame` first.  Copies nothing -- the buffer must outlive the run. */
void xl_pad_stream(Cpu *c, const uint8_t *held, uint32_t frames, uint32_t from_frame,
                   int32_t lag);
/* Native stand-in for the CD BIOS boot step: load the game module and enter it. */
void xl_boot_game(Cpu *c);
void xl_video_init(Cpu *c);
uint8_t *xl_frame_rgb(Cpu *c);               /* VD_W*VD_H*3 bytes, last frame */
int xl_load_state_file(Cpu *c, const char *path);
/* The port's own state image: the whole machine struct, taken at an instruction
 * boundary, resumable exactly.  Private format (see runtime.c); written atomically. */
int xl_ckpt_save(Cpu *c, const char *path);
int xl_ckpt_load(Cpu *c, const char *path);
/* XLATE_FRAMELOG: the start-of-frame-0 structural line, written from the power-on RAM
 * so frame 0 is measured like every other frame (the reference has a block 0 too). */
void xl_framelog_begin(Cpu *c);
/* XLATE_PORTSTATE_*: the port's side of the state-hash envelope (portstate.c).
 * Writes one JSON capture per anchored checkpoint, in the oracle's field layout, so
 * the two machines' states can be compared by one comparator.  Hashes are NOT
 * computed here: pce_state.py owns the format and hashes the fields, once. */
void xl_portstate_maybe(Cpu *c);
/* The same capture taken at a chosen raster line instead of the frame wrap: the
 * provider samples at line 256, this port wraps at line 0, and comparing two
 * different instants of a frame reports the phase difference as a machine difference.
 * Called from xl_tick; one branch when off (xl_portstate_on()). */
void xl_portstate_raster(Cpu *c);
int  xl_portstate_on(void);
int  xl_portstate_done(void);
void xl_portstate_manifest(FILE *out);
void xl_write_ppm(Cpu *c, FILE *f);
void xl_dump_vdc_state(Cpu *c, FILE *f);   /* same blob format as tools/state_dump.py */

/* ---- stubs and driver --------------------------------------------------- */
void xl_bios_call(Cpu *c);          /* entry at c->pc; returns via RTS */
int  xl_trap(Cpu *c, uint16_t pc);  /* logs and returns 0 */
/* No translation unit for this bank at this address: record it and stop cleanly
 * rather than executing whatever bytes happen to be there. */
int  xl_untranslated(Cpu *c, uint8_t bank, uint16_t pc);
int  xl_brk(Cpu *c);
void xl_untranslated_summary(FILE *out);
void xl_note_pc(Cpu *c);          /* record which bank is executing, first time */
void xl_banks_summary(FILE *out);
/* Stale-bank instrument (report only; see runtime.c).  xl_stale_image() is called by
 * the generated init to register the bytes each bank's translation was built from;
 * the write barrier and the bank-entry comparison are internal to runtime.c. */
void xl_stale_image(Cpu *c, uint8_t bank, uint16_t off, const uint8_t *data, size_t len);
void xl_stale_note(Cpu *c, uint8_t bank, uint16_t off, int src);
int  xl_run(Cpu *c, unsigned long max_steps, int use_stop_pc, uint16_t stop_pc);
int  xl_run_budget(Cpu *c, unsigned long max_steps, int use_stop_pc,
                   uint16_t stop_pc, uint64_t max_cycles);
void xl_dump_state(Cpu *c, FILE *out);
void xl_dump_call_log(Cpu *c, FILE *out);
/* The CPU's vector table as the machine reads it: $FFF6-$FFFF through the current
 * mapping, five 16-bit entries (IRQ2, IRQ1, timer, NMI, reset).  Printed when the
 * System Card is executed, so "the vectors come from the ROM, not from a model" is a
 * measured line rather than a claim. */
void xl_report_vectors(Cpu *c, FILE *out);
/* Enter at the card's reset vector (XLATE_BIOS_RESET=1).  Sets the power-on mapping
 * the console's own trace records (all MPRs zero) and the pc from $FFFE, and leaves
 * the video/timer/interrupt state at power-on so the card's own init has work to do.
 * Returns 1 when it moved the machine, 0 when the switch is off or unusable. */
int  xl_boot_reset(Cpu *c);
/* A console RESET on a machine that is already running: the reset-vector entry
 * (xl_boot_reset) plus what the machine has history for -- work RAM cleared as
 * PCE_Power() clears it, backup RAM and the drive kept, the raster clock and the
 * frame counter carried across so the schedule keeps its coordinate.  Returns 1
 * when the machine was reset, 0 when the reset-vector entry is unavailable (and
 * nothing was touched).  See "reset schedule" in runtime.c. */
int  xl_console_reset(Cpu *c);
/* The reset schedule, the same shape as the press file: frames of c->vdc.frame at
 * whose top the console is reset.  Added sorted; each fires once. */
int  xl_reset_at(uint32_t frame);
int  xl_reset_count(void);
uint32_t xl_reset_frame(int i);
/* The state the card's init left, as work RAM holds it: prints $1402, $1403, the
 * display-control shadow $F3/$F4 and the six hook slots.  Report only. */
void xl_report_boot_state(Cpu *c, const char *when);
uint32_t xl_crc_phys(Cpu *c);
uint32_t xl_crc32(const uint8_t *data, size_t len);

/* ---- interpreter -------------------------------------------------------- */
/* A real HuC6280 interpreter executing the bytes in memory (interp.c).  It shares
 * this struct, the memory map and the cycle table.  Two things route to it:
 * XLATE_INTERP=1 forces every step (that is how its identity with the translation
 * is tested), and otherwise the overlay guard routes one bank at a time while that
 * bank's live bytes disagree with its compiled image (runtime.c). */
int  xl_interp_step(Cpu *c);
/* One CPU step: the translated dispatcher, or the interpreter when forced or when
 * the guard has routed the executing bank. */
int  xl_cpu_step(Cpu *c);
/* The translated/interpreted split and the per-bank numbers (ADR O-5).  Printed on
 * every run: a run that leaned on the interpreter must never be quotable as pure. */
void xl_guard_summary(Cpu *c, FILE *out);

/* Provided by the generated translation unit. */
int  xlate_step(Cpu *c);
void xlate_init_state(Cpu *c);

/* Provided by translation units generated with --cases (differential tests). */
typedef struct XlateCase {
    unsigned id;
    void (*init)(Cpu *c);
    unsigned long steps;
} XlateCase;
const XlateCase *xlate_cases(unsigned *count);

#endif /* XLATE_RUNTIME_H */
