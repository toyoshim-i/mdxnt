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
constexpr uint32_t MAX_SEEK_TICKS = 2000000u;

OpnChip *g_chip = nullptr;
uint8_t g_mdxt_buf[MAX_MDXT_SIZE];
size_t g_mdxt_len = 0;
float g_pcm_buf[MAX_RENDER_FRAMES * 2]; // Stereo interleaved L,R

uint32_t g_sample_rate = 48000u;
uint32_t g_native_rate = 166400u;
double g_resample_rem = 0.0;
float g_resample_carry = 0.0f;

uint8_t g_timer_b = 220u;
uint8_t g_irq_enabled = 0u;
uint8_t g_playing = 0u;
uint32_t g_tick_count = 0u;
double g_tick_frame_accum = 0.0;
double g_speed_mult = 1.0;

// Per-channel mute mask: bit 0..2 = FM ch 1..3, bit 3..5 = SSG ch A..C
uint8_t g_mute_mask = 0u;
uint8_t g_ssg_vol_shadow[3] = {0, 0, 0};
uint8_t g_fm_tl_shadow[3][4] = {
    {127, 127, 127, 127},
    {127, 127, 127, 127},
    {127, 127, 127, 127},
};
bool g_silent_seek = false;

uint16_t read_u16_le(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

bool validate_mdxt(const uint8_t *buf, size_t len) {
    if (len < 20u || len > MAX_MDXT_SIZE) return false;
    if (buf[0] != 'M' || buf[1] != 'D' || buf[2] != 'X' || buf[3] != 'T') return false;
    const uint16_t vtbl_off = read_u16_le(buf + 6u);
    if (vtbl_off != 0u && (vtbl_off < 20u || vtbl_off >= len)) return false;
    for (size_t i = 0u; i < 6u; ++i) {
        const uint16_t coff = read_u16_le(buf + 8u + i * 2u);
        if (coff != 0u && (coff < 20u || coff >= len)) return false;
    }
    return true;
}

double frames_per_tick() {
    // YM2203 Timer-B period = (256 - TB) * 1152 / 3993600 seconds
    const double period_sec =
        static_cast<double>(256u - g_timer_b) * 1152.0 / static_cast<double>(OPN_MASTER_CLOCK);
    return period_sec * static_cast<double>(g_sample_rate);
}

void write_chip_filtered(uint8_t reg, uint8_t val) {
    if (!g_chip) return;
    if (reg == 0x26u) {
        g_timer_b = val;
    }
    // Track FM operator TL (0x40..0x4F) for instant mute even on slow/zero-RR voices
    if (reg >= 0x40u && reg <= 0x4Fu) {
        const uint8_t ch = reg & 0x03u;
        const uint8_t op = (reg >> 2) & 0x03u;
        if (ch < 3u) {
            g_fm_tl_shadow[ch][op] = val;
            if (g_mute_mask & (1u << ch)) {
                g_chip->write(reg, 127u);
                return;
            }
        }
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
    // Suppress FM key-on for muted FM channels or during silent seek
    if (reg == 0x28u) {
        const uint8_t ch = val & 0x03u;
        if (g_silent_seek || (ch < 3u && (g_mute_mask & (1u << ch)))) {
            g_chip->write(reg, ch); // force key-off
            return;
        }
    }
    g_chip->write(reg, val);
}

void reset_audio_state() {
    g_resample_rem = 0.0;
    g_resample_carry = 0.0f;
    g_tick_count = 0u;
    g_tick_frame_accum = 0.0;
    g_ssg_vol_shadow[0] = 0;
    g_ssg_vol_shadow[1] = 0;
    g_ssg_vol_shadow[2] = 0;
    for (uint8_t ch = 0u; ch < 3u; ++ch) {
        for (uint8_t op = 0u; op < 4u; ++op) {
            g_fm_tl_shadow[ch][op] = 127u;
        }
    }
}

// Boxcar integrating resampler from g_native_rate (166.4 kHz) to g_sample_rate (e.g. 48 kHz)
// to suppress FM/SSG high-frequency aliasing.
float render_one_output_sample() {
    if (!g_chip) return 0.0f;
    const double ratio = static_cast<double>(g_native_rate) / static_cast<double>(g_sample_rate);
    double needed = ratio;
    double sum = 0.0;

    if (g_resample_rem > 0.0) {
        const double take = (g_resample_rem < needed) ? g_resample_rem : needed;
        sum += static_cast<double>(g_resample_carry) * take;
        g_resample_rem -= take;
        needed -= take;
    }

    while (needed >= 1.0) {
        sum += static_cast<double>(g_chip->next_native_sample());
        needed -= 1.0;
    }

    if (needed > 0.0) {
        const float s = g_chip->next_native_sample();
        sum += static_cast<double>(s) * needed;
        g_resample_carry = s;
        g_resample_rem = 1.0 - needed;
    }

    return static_cast<float>(sum / ratio);
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
    g_playing = 0u;
    g_mdxt_len = 0u;
    reset_audio_state();
    mdxnt_init();
}

WASM_EXPORT("mdxnt_wasm_load")
uint32_t mdxnt_wasm_load(uint32_t len) {
    if (!g_chip) {
        mdxnt_wasm_init(g_sample_rate);
    }
    if (!validate_mdxt(g_mdxt_buf, len)) {
        g_playing = 0u;
        g_mdxt_len = 0u;
        mdxnt_stop_bgm();
        return 0u;
    }
    if (len < MAX_MDXT_SIZE) {
        memset(g_mdxt_buf + len, 0, MAX_MDXT_SIZE - len);
    }
    g_mdxt_len = len;
    g_chip->reset();
    reset_audio_state();
    mdxnt_init();
    mdxnt_play_bgm(g_mdxt_buf);
    g_playing = mdxnt_bgm_active() ? 1u : 0u;
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
    // Immediately apply FM mute (key-off + TL=127) and unmute (restore shadowed TL)
    for (uint8_t ch = 0u; ch < 3u; ++ch) {
        const bool muted = (g_mute_mask & (1u << ch)) != 0;
        const bool was_muted = (prev & (1u << ch)) != 0;
        if (muted && !was_muted) {
            g_chip->write(0x28u, ch);
            for (uint8_t op = 0u; op < 4u; ++op) {
                g_chip->write(static_cast<uint8_t>(0x40u + (op << 2) + ch), 127u);
            }
        } else if (!muted && was_muted) {
            for (uint8_t op = 0u; op < 4u; ++op) {
                g_chip->write(static_cast<uint8_t>(0x40u + (op << 2) + ch), g_fm_tl_shadow[ch][op]);
            }
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
    if (target_ticks > MAX_SEEK_TICKS) target_ticks = MAX_SEEK_TICKS;
    const uint8_t was_playing = g_playing;
    g_silent_seek = true;
    g_chip->reset();
    reset_audio_state();
    mdxnt_init();
    mdxnt_play_bgm(g_mdxt_buf);
    for (uint32_t t = 0u; t < target_ticks; ++t) {
        mdxnt_tick_isr();
        g_tick_count++;
        if (!mdxnt_bgm_active()) {
            break;
        }
    }
    g_silent_seek = false;
    g_playing = (was_playing && mdxnt_bgm_active()) ? 1u : 0u;
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
        if (g_irq_enabled && g_playing) {
            g_tick_frame_accum += g_speed_mult;
            while (g_playing) {
                const double fpt = frames_per_tick();
                if (fpt <= 0.0 || g_tick_frame_accum < fpt) break;
                g_tick_frame_accum -= fpt;
                mdxnt_tick_isr();
                g_tick_count++;
                if (!mdxnt_bgm_active()) {
                    g_playing = 0u;
                }
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
