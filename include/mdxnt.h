#ifndef MDXNT_H
#define MDXNT_H

#include <stdint.h>

/*
 * mdxnt — Clean-Room MDX-Compatible YM2203 OPN (FM 3ch + SSG 3ch) Sound Driver
 *
 * Portable C99 sequencer core with a thin platform HAL (e.g. PC-8801mkIISR
 * YM2203 Timer-B IM 2 ISR in hal/pc88_sr/mdxnt_hal_pc88.s).
 */

void mdxnt_init(void);
void mdxnt_shutdown(void);

/* Play / stop 6-channel BGM (FM ch 0..2, SSG ch 3..5) */
void mdxnt_play_bgm(const uint8_t *mdxt_data);
void mdxnt_stop_bgm(void);

/* Play / stop priority SE (uses SSG ch.B/C, auto-restores BGM when done) */
void mdxnt_play_se(const uint8_t *mdxt_data);
void mdxnt_stop_se(void);
uint8_t mdxnt_se_busy(void);

/* One-shot SSG voice blip queued safely for ISR output (zero port contention) */
void mdxnt_ssg_blip(uint8_t ssg_ch, uint16_t tp, uint8_t vol, uint8_t ticks);

/*
 * Platform HAL hooks:
 *   - Default (target build): implemented in platform HAL (e.g. mdxnt_hal_pc88.s)
 *   - MDXNT_MOCK_HAL: extern hooks provided by host unit tests / emulator harness
 *   - HOST_TEST (without MDXNT_MOCK_HAL): no-op stubs for headless game sim builds
 */
#if defined(HOST_TEST) && !defined(MDXNT_MOCK_HAL)
static inline void mdxnt_opn_write(uint8_t reg, uint8_t val) { (void)reg; (void)val; }
static inline void mdxnt_hw_enable_irq(uint8_t init_tb) { (void)init_tb; }
static inline void mdxnt_hw_disable_irq(void) {}
static inline void mdxnt_hw_ack_timer_b(void) {}
static inline void mdxnt_enter_crit(void) {}
static inline void mdxnt_leave_crit(void) {}
#else
void mdxnt_opn_write(uint8_t reg, uint8_t val);
void mdxnt_hw_enable_irq(uint8_t init_tb);
void mdxnt_hw_disable_irq(void);
void mdxnt_hw_ack_timer_b(void);
void mdxnt_enter_crit(void);
void mdxnt_leave_crit(void);
#endif

/* Timer-B ISR tick entry called from platform HAL */
void mdxnt_tick_isr(void);

#endif /* MDXNT_H */
