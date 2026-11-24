/*
 * Host unit tests for mdxnt_drv.c using a mock YM2203 register file.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mdxnt.h"

static uint8_t s_regs[256];
static uint8_t s_last_tb = 0u;
static int s_irq_enabled = 0;

void mdxnt_opn_write(uint8_t reg, uint8_t val)
{
    s_regs[reg] = val;
}

void mdxnt_hw_enable_irq(uint8_t init_tb)
{
    s_last_tb = init_tb;
    s_irq_enabled = 1;
}

void mdxnt_hw_disable_irq(void)
{
    s_irq_enabled = 0;
}

void mdxnt_hw_ack_timer_b(void) {}
void mdxnt_enter_crit(void) {}
void mdxnt_leave_crit(void) {}

static void reset_mock(void)
{
    memset(s_regs, 0, sizeof(s_regs));
    s_last_tb = 0u;
    s_irq_enabled = 0;
}

/* Verify YM2203 prescaler init, Timer-B 120 BPM (220), FM CON 4 carrier mask, and Block = oct */
static void test_fm_con4_and_pitch(void)
{
    /* Minimal MDXT song: 1 CON 4 voice (all base TL=0), ch A plays @0 v8 A4 (note 57 = o4 a) */
    static const uint8_t SONG[] = {
        'M', 'D', 'X', 'T', 6u, 220u,
        20u, 0u,             /* voice_tbl_off = 20 */
        46u, 0u,             /* ch A off = 46 */
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
        /* Voice @0 (26 bytes): DT/ML, TL=0, KS/AR, DR, SR, SL/RR, FB/CON=0x04 */
        1u, 1u, 1u, 1u,
        0u, 0u, 0u, 0u,
        31u, 31u, 31u, 31u,
        0u, 0u, 0u, 0u,
        0u, 0u, 0u, 0u,
        15u, 15u, 15u, 15u,
        0x04u, 0u,
        /* Ch A stream: @0, v8 (FM_VOL_TL[8] = 16), note 57 (o4 a) for 16 ticks, end */
        0xFDu, 0u,
        0xFBu, 8u,
        (uint8_t)(0x80u + 57u), 15u,
        0xF1u, 0u, 0u
    };

    reset_mock();
    mdxnt_init();
    assert(s_irq_enabled == 1);
    assert(s_last_tb == 220u);

    mdxnt_play_bgm(SONG);
    mdxnt_tick_isr();

    /* CON 4 carriers are OP2 (reg 0x48) and OP4 (reg 0x4C); modulators OP1 (0x40) & OP3 (0x44) stay 0 */
    assert(s_regs[0x40] == 0u);
    assert(s_regs[0x44] == 0u);
    assert(s_regs[0x48] == 16u);
    assert(s_regs[0x4C] == 16u);

    /* A4 = Block 4, F_num = 1040 (0x410): reg 0xA4 = (4 << 3) | 4 = 0x24, reg 0xA0 = 0x10 */
    assert(s_regs[0xA4] == 0x24u);
    assert(s_regs[0xA0] == 0x10u);

    mdxnt_shutdown();
    printf("[PASS] test_fm_con4_and_pitch\n");
}

/* Verify SSG high-octave rounding, tie lookahead across vN, and SE hijack + BGM restore */
static void test_ssg_tie_and_se_restore(void)
{
    /* BGM on SSG ch.B (slot 4 / E): @0 v12 o6 a (note 81) 8t -> v15 -> tie -> o6 a 8t */
    static const uint8_t BGM[] = {
        'M', 'D', 'X', 'T', 6u, 220u,
        0u, 0u,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
        20u, 0u,             /* ch E (SSG ch.B = hw_ch 1) */
        0u, 0u,
        /* Ch E: @0, v12, q4, note 81 (o6 a, TP=71) 8t, v15, &, note 81 8t, end */
        0xFDu, 0u,
        0xFBu, 12u,
        0xF8u, 4u,
        (uint8_t)(0x80u + 81u), 7u,
        0xFBu, 15u,
        0xF7u,
        (uint8_t)(0x80u + 81u), 7u,
        0xF1u, 0u, 0u
    };
    /* Short 2-tick SE on SSG ch.B (ch E): @0 v10 o4 c (note 48, TP=477) 2t, end */
    static const uint8_t SE[] = {
        'M', 'D', 'X', 'T', 6u, 220u,
        0u, 0u,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
        20u, 0u,
        0u, 0u,
        0xFDu, 0u,
        0xFBu, 10u,
        (uint8_t)(0x80u + 48u), 1u,
        0xF1u, 0u, 0u
    };

    reset_mock();
    mdxnt_init();
    mdxnt_play_bgm(BGM);

    /* Tick 1: BGM starts o6 a -> rounded TP = (2269 + 16) >> 5 = 71, vol = 12 */
    mdxnt_tick_isr();
    assert((uint16_t)(s_regs[2] | ((uint16_t)s_regs[3] << 8)) == 71u);
    assert(s_regs[9] == 12u);

    /* Tick 2..7: tie lookahead across v15 must keep note sustained (no gate-off at tick 4!) */
    mdxnt_tick_isr();
    mdxnt_tick_isr();
    mdxnt_tick_isr();
    mdxnt_tick_isr();
    assert(s_regs[9] == 12u);

    /* Trigger SE on SSG ch.B: SE hijacks hw_ch 1 with TP=477, vol=10 */
    mdxnt_play_se(SE);
    assert(mdxnt_se_busy() == 1u);
    mdxnt_tick_isr(); /* SE tick 1 */
    assert((uint16_t)(s_regs[2] | ((uint16_t)s_regs[3] << 8)) == 477u);
    assert(s_regs[9] == 10u);

    mdxnt_tick_isr(); /* Tick 7: SE tick 2 */
    mdxnt_tick_isr(); /* Tick 8: SE ends -> BGM restores (TP=71, vol=12) */
    assert(mdxnt_se_busy() == 0u);
    assert((uint16_t)(s_regs[2] | ((uint16_t)s_regs[3] << 8)) == 71u);
    assert(s_regs[9] == 12u);

    mdxnt_tick_isr(); /* Tick 9: BGM executes v15 & tied note -> vol updates immediately to 15 */
    assert(s_regs[9] == 15u);

    mdxnt_shutdown();
    printf("[PASS] test_ssg_tie_and_se_restore\n");
}

int main(void)
{
    test_fm_con4_and_pitch();
    test_ssg_tie_and_se_restore();
    printf("ALL DRIVER HOST TESTS PASSED!\n");
    return 0;
}
