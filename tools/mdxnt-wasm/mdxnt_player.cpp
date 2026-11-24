#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "opn_chip.h"

extern "C" {
#include "mdxnt.h"
}

#ifdef __wasm__
#define WASM_EXPORT(name) __attribute__((export_name(name)))
#else
#define WASM_EXPORT(name)
#endif

namespace {

constexpr uint32_t OPN_MASTER_CLOCK = 3993600u; // PC-8801mkIISR YM2203
constexpr size_t MAX_MDXT_SIZE = 65536u;
constexpr size_t MAX_RENDER_FRAMES = 4096u;

OpnChip *g_chip = nullptr;
uint8_t g_mdxt_buf[MAX_MDXT_SIZE];
size_t g_mdxt_len = 0;
float g_pcm_buf[MAX_RENDER_FRAMES * 2]; // Stereo interleaved L,R

uint32_t g_sample_rate = 48000u;
uint32_t g_native_rate = 166400u;
double g_resample_phase = 0.0;
float g_prev_sample = 0.0f;
float g_cur_sample = 0.0f;

uint8_t g_timer_b = 220u;
uint8_t g_irq_enabled = 0u;
uint8_t g_playing = 0u;
uint32_t g_tick_count = 0u;
double g_tick_frame_accum = 0.0;
double g_speed_mult = 1.0;

// Per-channel mute mask: bit 0..2 = FM ch 1..3, bit 3..5 = SSG ch A..C
uint8_t g_mute_mask = 0u;
uint8_t g_ssg_vol_shadow[3] = {0, 0, 0};
bool g_silent_seek = false;

double frames_per_tick() {
    // YM2203 Timer-B period = (256 - TB) * 1152 / 3993600 seconds
    const double period_sec = static_cast<double>(256u - g_timer_b) * 1152.0 / static_cast<double>(OPN_MASTER_CLOCK);
    return period_sec * static_cast<double>(g_sample_rate);
}

void write_chip_filtered(uint8_t reg, uint8_t val) {
    if (!g_chip) return;
    if (reg == 0x26u) {
        g_timer_b = val;
    }
    // Track SSG volumes for instant mute/unmute
    if (reg >= 0x08u && reg <= 0x0Au) {
        const uint8_t ch = reg - 0x08u;
        g_ssg_vol_shadow[ch] = val;
        if (g_mute_mask & (1u << (3u + ch))) {
            g_chip->write(reg, 0u);
            return;
        }
    }
    // Suppress FM key-on for muted FM channels
    if (reg == 0x28u) {
        const uint8_t ch = val & 0x03u;
        if (ch < 3u && (g_mute_mask & (1u << ch))) {
            g_chip->write(reg, ch); // force key-off
            return;
        }
    }
    g_chip->write(reg, val);
}

void reset_audio_state() {
    g_resample_phase = 0.0;
    g_prev_sample = 0.0f;
    g_cur_sample = 0.0f;
    g_tick_count = 0u;
    g_tick_frame_accum = 0.0;
    g_ssg_vol_shadow[0] = 0;
    g_ssg_vol_shadow[1] = 0;
    g_ssg_vol_shadow[2] = 0;
}

float render_one_output_sample() {
    if (!g_chip) return 0.0f;
    const double step = static_cast<double>(g_native_rate) / static_cast<double>(g_sample_rate);
    g_resample_phase += step;
    while (g_resample_phase >= 1.0) {
        g_prev_sample = g_cur_sample;
        g_cur_sample = g_chip->next_native_sample();
        g_resample_phase -= 1.0;
    }
    const float frac = static_cast<float>(g_resample_phase);
    return g_prev_sample + (g_cur_sample - g_prev_sample) * frac;
}

} // namespace

/* -------------------------------------------------------------------------- */
/* mdxnt HAL hooks (called by src/mdxnt_drv.c)                                */
/* -------------------------------------------------------------------------- */
extern "C" {

void mdxnt_opn_write(uint8_t reg, uint8_t val) {
    write_chip_filtered(reg, val);
}

void mdxnt_hw_enable_irq(uint8_t init_tb) {
    g_timer_b = init_tb;
    g_irq_enabled = 1u;
}

void mdxnt_hw_disable_irq(void) {
    g_irq_enabled = 0u;
}

void mdxnt_hw_ack_timer_b(void) {
}

void mdxnt_enter_crit(void) {
}

void mdxnt_leave_crit(void) {
}

/* -------------------------------------------------------------------------- */
/* Wasm Player Public C ABI                                                   */
/* -------------------------------------------------------------------------- */

WASM_EXPORT("mdxnt_wasm_get_data_buf")
uint8_t *mdxnt_wasm_get_data_buf(void) {
    return g_mdxt_buf;
}

WASM_EXPORT("mdxnt_wasm_get_data_capacity")
uint32_t mdxnt_wasm_get_data_capacity(void) {
    return static_cast<uint32_t>(MAX_MDXT_SIZE);
}

WASM_EXPORT("mdxnt_wasm_get_pcm_buf")
float *mdxnt_wasm_get_pcm_buf(void) {
    return g_pcm_buf;
}

WASM_EXPORT("mdxnt_wasm_init")
void mdxnt_wasm_init(uint32_t sample_rate) {
    g_sample_rate = (sample_rate >= 8000u && sample_rate <= 192000u) ? sample_rate : 48000u;
    g_chip = create_ymfm_opn_chip(OPN_MASTER_CLOCK);
    g_native_rate = g_chip->native_sample_rate();
    g_chip->reset();
    reset_audio_state();
    mdxnt_init();
}

WASM_EXPORT("mdxnt_wasm_load")
uint32_t mdxnt_wasm_load(uint32_t len) {
    if (!g_chip) {
        mdxnt_wasm_init(g_sample_rate);
    }
    if (len < 20u || len > MAX_MDXT_SIZE) {
        return 0u;
    }
    if (g_mdxt_buf[0] != 'M' || g_mdxt_buf[1] != 'D' || g_mdxt_buf[2] != 'X' || g_mdxt_buf[3] != 'T') {
        return 0u;
    }
    g_mdxt_len = len;
    g_chip->reset();
    reset_audio_state();
    mdxnt_init();
    mdxnt_play_bgm(g_mdxt_buf);
    g_playing = 1u;
    return 1u;
}

WASM_EXPORT("mdxnt_wasm_stop")
void mdxnt_wasm_stop(void) {
    g_playing = 0u;
    mdxnt_stop_bgm();
    if (g_chip) {
        g_chip->reset();
    }
    reset_audio_state();
}

WASM_EXPORT("mdxnt_wasm_set_speed")
void mdxnt_wasm_set_speed(uint32_t speed_percent) {
    if (speed_percent < 25u) speed_percent = 25u;
    if (speed_percent > 1600u) speed_percent = 1600u;
    g_speed_mult = static_cast<double>(speed_percent) / 100.0;
}

WASM_EXPORT("mdxnt_wasm_set_mute_mask")
void mdxnt_wasm_set_mute_mask(uint32_t mask) {
    const uint8_t prev = g_mute_mask;
    g_mute_mask = static_cast<uint8_t>(mask & 0x3Fu);
    if (!g_chip) return;
    // Immediately silence newly muted FM channels
    for (uint8_t ch = 0u; ch < 3u; ++ch) {
        if ((g_mute_mask & (1u << ch)) && !(prev & (1u << ch))) {
            g_chip->write(0x28u, ch);
        }
    }
    // Immediately apply SSG mute/unmute
    for (uint8_t ch = 0u; ch < 3u; ++ch) {
        const bool muted = (g_mute_mask & (1u << (3u + ch))) != 0;
        g_chip->write(static_cast<uint8_t>(0x08u + ch), muted ? 0u : g_ssg_vol_shadow[ch]);
    }
}

WASM_EXPORT("mdxnt_wasm_seek_ticks")
uint32_t mdxnt_wasm_seek_ticks(uint32_t target_ticks) {
    if (g_mdxt_len < 20u || !g_chip) return 0u;
    g_silent_seek = true;
    g_chip->reset();
    reset_audio_state();
    mdxnt_init();
    mdxnt_play_bgm(g_mdxt_buf);
    g_playing = 1u;
    // Advance driver silently to target_ticks
    for (uint32_t t = 0u; t < target_ticks; ++t) {
        mdxnt_tick_isr();
        g_tick_count++;
    }
    // Silence FM key-ons left over during seek so notes don't pop awkwardly
    for (uint8_t ch = 0u; ch < 3u; ++ch) {
        g_chip->write(0x28u, ch);
    }
    g_silent_seek = false;
    return g_tick_count;
}

WASM_EXPORT("mdxnt_wasm_render")
uint32_t mdxnt_wasm_render(uint32_t frames) {
    if (frames > MAX_RENDER_FRAMES) {
        frames = MAX_RENDER_FRAMES;
    }
    if (!g_playing || !g_chip) {
        memset(g_pcm_buf, 0, frames * 2u * sizeof(float));
        return frames;
    }

    for (uint32_t i = 0u; i < frames; ++i) {
        if (g_irq_enabled) {
            g_tick_frame_accum += g_speed_mult;
            const double fpt = frames_per_tick();
            while (g_tick_frame_accum >= fpt && fpt > 0.0) {
                g_tick_frame_accum -= fpt;
                mdxnt_tick_isr();
                g_tick_count++;
            }
        }
        const float s = render_one_output_sample();
        g_pcm_buf[i * 2u] = s;
        g_pcm_buf[i * 2u + 1u] = s;
    }
    return frames;
}

WASM_EXPORT("mdxnt_wasm_get_tick")
uint32_t mdxnt_wasm_get_tick(void) {
    return g_tick_count;
}

WASM_EXPORT("mdxnt_wasm_get_tb")
uint32_t mdxnt_wasm_get_tb(void) {
    return g_timer_b;
}

WASM_EXPORT("mdxnt_wasm_is_playing")
uint32_t mdxnt_wasm_is_playing(void) {
    return g_playing;
}

} // extern "C"
