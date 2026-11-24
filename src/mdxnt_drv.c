/*
 * MDX-OPN (YM2203: FM 3ch + SSG 3ch) Clean-Room Sound Driver
 *
 * Portable C implementation driven by PC-8801mkIISR OPN Timer-B ISR (~96Hz+).
 * Supports MDX-compatible compact bytecode (.mdxt), 4-op FM voices, SSG
 * software envelopes, noise mixer, pitch sweep, nested repeats, priority SE
 * channel hijacking (SSG ch.B/C), and contention-free one-shot SSG blips.
 */

#include "mdxnt.h"

/* -------------------------------------------------------------------------- */
/* YM2203 Pitch Tables (3.9936 MHz master clock)                              */
/* -------------------------------------------------------------------------- */
/* FM F-number for C..B (YM2203 f = F_num * 3993600 / (144 * 2^(20 - Block)), A4=440Hz at Block=4) */
static const uint16_t FM_FNUM[12] = {
    618u, 655u, 694u, 735u, 779u, 825u, 874u, 926u, 981u, 1040u, 1102u, 1167u
};

/* SSG 12-bit Tone Period for o1 C..B (TP = 124800 / Hz); shifted >> (oct - 1) */
static const uint16_t SSG_TP_O1[12] = {
    3816u, 3602u, 3400u, 3209u, 3029u, 2859u,
    2699u, 2547u, 2404u, 2269u, 2142u, 2022u
};

/* Carrier slot bitmask (bit0=OP1, bit1=OP3, bit2=OP2, bit3=OP4) per CON 0..7 */
static const uint8_t FM_CARRIER_MASK[8] = {
    0x08u, 0x08u, 0x08u, 0x08u, 0x0Cu, 0x0Eu, 0x0Eu, 0x0Fu
};

/* Logarithmic musical FM attenuation table for v0..v15 (YM2203 TL = 0.75dB/step) */
static const uint8_t FM_VOL_TL[16] = {
   127u, 52u, 44u, 38u, 32u, 27u, 23u, 19u,
    16u, 13u, 10u,  8u,  6u,  4u,  2u,  0u
};

/* ADSR Software Envelope presets for SSG (@0..@5), scaled in 0..15 volume steps */
typedef struct {
    uint8_t al; /* initial attack level (0..15) */
    uint8_t ar; /* attack step per tick (0 = instant peak) */
    uint8_t dr; /* decay ticks per -1 vol down to sl (0 = none) */
    uint8_t sl; /* sustain level (0..15) */
    uint8_t sr; /* sustain slow-decay ticks per -1 vol (0 = hold forever) */
    uint8_t rr; /* release ticks per -1 vol after key-off (1 = fast smooth, 0 = instant) */
} SsgEnvPreset;

static const SsgEnvPreset SSG_ENVS[6] = {
    /* @0: flat organ / SE sustain with gentle 1-tick release */
    { 15u, 0u, 0u, 15u,  0u, 1u },
    /* @1: bright synth lead / strings (slight attack bloom, smooth sustain & release) */
    { 12u, 2u, 4u, 12u, 24u, 2u },
    /* @2: punchy bass / pluck (fast decay to mid-sustain, crisp release) */
    { 15u, 0u, 2u,  8u, 16u, 1u },
    /* @3: expressive melody / bell-lead (lush decay to high sustain, lingering release) */
    { 15u, 0u, 3u, 11u, 20u, 2u },
    /* @4: soft pad / chord (gentle swell, warm sustain, slow release) */
    {  9u, 1u, 6u, 12u, 32u, 3u },
    /* @5: short percussive hi-hat / snare noise */
    { 15u, 0u, 1u,  0u,  0u, 1u },
};

#define ENV_PHASE_OFF 0u
#define ENV_PHASE_ATK 1u
#define ENV_PHASE_DEC 2u
#define ENV_PHASE_SUS 3u
#define ENV_PHASE_REL 4u

/* -------------------------------------------------------------------------- */
/* Channel State                                                              */
/* -------------------------------------------------------------------------- */
#define MDXNT_NUM_BGM_CH 6u
#define MDXNT_NUM_SE_CH  2u
#define MDXNT_TOTAL_CH   8u /* 0..2: FM BGM, 3..5: SSG BGM, 6..7: SSG SE (ch1,ch2) */
#define MDXNT_REP_DEPTH  4u

typedef struct {
    uint8_t        active;
    const uint8_t *ptr;
    const uint8_t *base;
    const uint8_t *voice_tbl;
    uint16_t       wait;
    uint16_t       gate;
    uint8_t        q;
    uint8_t        vol;
    uint8_t        cur_vol;
    uint8_t        voice;
    uint8_t        env_phase; /* ENV_PHASE_* */
    uint8_t        env_cnt;
    uint8_t        env_lvl;   /* 0..15 envelope multiplier level */
    uint8_t        tie;
    uint8_t        key_on;
    uint8_t        ssg_mode;  /* 0=Tone, 1=Noise, 2=Both */
    uint8_t        ssg_nfreq; /* 0..31 */
    int16_t        detune;
    int16_t        sweep;
    uint16_t       cur_freq;
    uint8_t        cur_blk;
    uint8_t        fm_con;
    uint8_t        fm_tl[4];  /* base TL from current voice */
    uint8_t        rep_sp;
    uint8_t        rep_cnt[MDXNT_REP_DEPTH];
    /* Software Pitch LFO (0xEE: delay, speed, depth, half-cycle count) */
    uint8_t        lfo_on;
    uint8_t        lfo_delay;
    uint8_t        lfo_speed;
    uint8_t        lfo_count;
    int16_t        lfo_depth;
    uint8_t        lfo_d_cnt;
    uint8_t        lfo_s_cnt;
    uint8_t        lfo_c_cnt;
    int8_t         lfo_dir;
    int16_t        lfo_cur;
} MdxntCh;

typedef struct {
    uint8_t  pending;
    uint8_t  ticks;
    uint16_t tp;
    uint8_t  vol;
} SsgBlipSlot;

static MdxntCh      s_ch[MDXNT_TOTAL_CH];
static SsgBlipSlot s_blip[3];
static uint8_t     s_ssg_mixer = 0x38u; /* Tone A/B/C ON, Noise OFF */
static uint8_t     s_inited = 0u;
static uint8_t     s_se_active = 0u;

/* -------------------------------------------------------------------------- */
/* Low-Level OPN FM / SSG Output Helpers (called ONLY inside DI / ISR)        */
/* -------------------------------------------------------------------------- */
static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int16_t read_s16(const uint8_t *p)
{
    return (int16_t)read_u16(p);
}

static void ssg_write_tone(uint8_t ssg_ch, uint16_t tp)
{
    uint8_t r = (uint8_t)(ssg_ch << 1);
    if (tp > 4095u) tp = 4095u;
    if (tp < 1u) tp = 1u;
    mdxnt_opn_write(r, (uint8_t)(tp & 0xFFu));
    mdxnt_opn_write((uint8_t)(r + 1u), (uint8_t)((tp >> 8) & 0x0Fu));
}

static void ssg_write_vol(uint8_t ssg_ch, uint8_t vol)
{
    mdxnt_opn_write((uint8_t)(8u + ssg_ch), (uint8_t)(vol & 0x0Fu));
}

static void ssg_apply_mixer(uint8_t ssg_ch, uint8_t mode, uint8_t nfreq)
{
    uint8_t tbit = (uint8_t)(1u << ssg_ch);
    uint8_t nbit = (uint8_t)(8u << ssg_ch);
    /* Active-low mixer bits: 0 = enabled, 1 = disabled */
    if (mode == 0u) {
        s_ssg_mixer = (uint8_t)((s_ssg_mixer & (uint8_t)~tbit) | nbit);
    } else if (mode == 1u) {
        s_ssg_mixer = (uint8_t)((s_ssg_mixer | tbit) & (uint8_t)~nbit);
        mdxnt_opn_write(6u, (uint8_t)(nfreq & 0x1Fu));
    } else {
        s_ssg_mixer = (uint8_t)(s_ssg_mixer & (uint8_t)~(tbit | nbit));
        mdxnt_opn_write(6u, (uint8_t)(nfreq & 0x1Fu));
    }
    mdxnt_opn_write(7u, s_ssg_mixer);
}

static void fm_apply_tl(uint8_t fm_ch, const MdxntCh *c)
{
    uint8_t cmask = FM_CARRIER_MASK[c->fm_con & 7u];
    uint8_t atten = FM_VOL_TL[c->vol & 15u];
    uint8_t op;
    for (op = 0u; op < 4u; ++op) {
        uint8_t tl = c->fm_tl[op];
        if (cmask & (uint8_t)(1u << op)) {
            uint16_t v = (uint16_t)tl + atten;
            if (v > 127u) v = 127u;
            tl = (uint8_t)v;
        }
        mdxnt_opn_write((uint8_t)(0x40u + (op << 2) + fm_ch), tl);
    }
}

static void fm_load_voice(uint8_t fm_ch, MdxntCh *c, uint8_t vnum)
{
    const uint8_t *vp;
    uint8_t op;
    if (!c->voice_tbl) return;
    vp = c->voice_tbl + (uint16_t)vnum * 26u;
    for (op = 0u; op < 4u; ++op) {
        uint8_t off = (uint8_t)((op << 2) + fm_ch);
        mdxnt_opn_write((uint8_t)(0x30u + off), vp[op]);          /* DT1/MUL */
        c->fm_tl[op] = vp[4u + op];
        mdxnt_opn_write((uint8_t)(0x50u + off), vp[8u + op]);     /* KS/AR */
        mdxnt_opn_write((uint8_t)(0x60u + off), vp[12u + op]);    /* DR */
        mdxnt_opn_write((uint8_t)(0x70u + off), vp[16u + op]);    /* SR */
        mdxnt_opn_write((uint8_t)(0x80u + off), vp[20u + op]);    /* SL/RR */
    }
    c->fm_con = (uint8_t)(vp[24] & 7u);
    mdxnt_opn_write((uint8_t)(0xB0u + fm_ch), vp[24]);            /* FB/CON */
    fm_apply_tl(fm_ch, c);
}

static void fm_write_freq(uint8_t fm_ch, uint8_t blk, uint16_t fnum)
{
    mdxnt_opn_write((uint8_t)(0xA4u + fm_ch),
                   (uint8_t)(((blk & 7u) << 3) | ((fnum >> 8) & 7u)));
    mdxnt_opn_write((uint8_t)(0xA0u + fm_ch), (uint8_t)(fnum & 0xFFu));
}

static void fm_key_off(uint8_t fm_ch)
{
    mdxnt_opn_write(0x28u, fm_ch);
}

static void fm_key_on(uint8_t fm_ch)
{
    mdxnt_opn_write(0x28u, (uint8_t)(0xF0u | fm_ch));
}

/* Portable symmetric / 16 (avoids implementation-defined signed >>, INT16_MIN UB, and DC vibrato bias) */
static int16_t div16_sym(int16_t x)
{
    if (x >= 0) return (int16_t)((uint16_t)x >> 4);
    return -(int16_t)((uint16_t)(0u - (uint16_t)x) >> 4);
}

/* Compute effective SSG tone period including current LFO offset */
static uint16_t ssg_eff_tp(const MdxntCh *c)
{
    int16_t out_f = (int16_t)c->cur_freq;
    if (c->lfo_on) out_f = (int16_t)(out_f - div16_sym(c->lfo_cur));
    if (out_f < 16) out_f = 16;
    if (out_f > 4095) out_f = 4095;
    return (uint16_t)out_f;
}

/* Check if a BGM SSG channel (0..2) is currently owned by BGM (no SE/blip override) */
static uint8_t ssg_ch_owned_by_bgm(uint8_t ssg_ch)
{
    if (s_blip[ssg_ch].ticks > 0u || s_blip[ssg_ch].pending) return 0u;
    if (ssg_ch == 1u && s_ch[6].active) return 0u;
    if (ssg_ch == 2u && s_ch[7].active) return 0u;
    return 1u;
}

/* Check if an SE SSG channel (1..2) is currently owned by SE (no blip override) */
static uint8_t ssg_ch_owned_by_se(uint8_t ssg_ch)
{
    if (s_blip[ssg_ch].ticks > 0u || s_blip[ssg_ch].pending) return 0u;
    return 1u;
}

static uint8_t bgm_any_active(void)
{
    uint8_t i;
    for (i = 0u; i < MDXNT_NUM_BGM_CH; ++i) {
        if (s_ch[i].active) return 1u;
    }
    return 0u;
}

/* Restore highest-priority active owner on hardware SSG channel (0..2), or silence */
static void ssg_restore_channel(uint8_t hw_ch)
{
    const MdxntCh *owner = (const MdxntCh *)0;
    if (s_blip[hw_ch].ticks > 0u || s_blip[hw_ch].pending) return;
    if (hw_ch == 1u && s_ch[6].active) owner = &s_ch[6];
    else if (hw_ch == 2u && s_ch[7].active) owner = &s_ch[7];
    else if (s_ch[3u + hw_ch].active) owner = &s_ch[3u + hw_ch];

    if (owner) {
        ssg_apply_mixer(hw_ch, owner->ssg_mode, owner->ssg_nfreq);
        ssg_write_tone(hw_ch, ssg_eff_tp(owner));
        ssg_write_vol(hw_ch, owner->cur_vol);
    } else {
        ssg_apply_mixer(hw_ch, 0u, 12u);
        ssg_write_vol(hw_ch, 0u);
    }
}

/* Look ahead past non-note state commands to detect tie (0xF7) */
static uint8_t next_is_tie(const uint8_t *p)
{
    uint8_t guard = 16u;
    while (guard--) {
        uint8_t b = *p;
        if (b == 0xF7u) return 1u;
        if (b == 0xFAu || b == 0xF9u) {
            p += 1;
        } else if (b == 0xFFu || b == 0xFDu || b == 0xFBu || b == 0xF8u) {
            p += 2;
        } else if (b == 0xF3u || b == 0xF2u || b == 0xEDu) {
            p += 3;
        } else if (b == 0xEEu) {
            p += 6;
        } else {
            return 0u;
        }
    }
    return 0u;
}

static void ch_reset(MdxntCh *c, const uint8_t *base, const uint8_t *vtbl, uint16_t off)
{
    uint8_t i;
    c->active = (off != 0u) ? 1u : 0u;
    c->base = base;
    c->ptr = base + off;
    c->voice_tbl = vtbl;
    c->wait = 1u;
    c->gate = 0u;
    c->q = 7u;
    c->vol = 12u;
    c->cur_vol = 0u;
    c->voice = 0u;
    c->env_phase = ENV_PHASE_OFF;
    c->env_cnt = 0u;
    c->env_lvl = 0u;
    c->tie = 0u;
    c->key_on = 0u;
    c->ssg_mode = 0u;
    c->ssg_nfreq = 12u;
    c->detune = 0;
    c->sweep = 0;
    c->cur_freq = 256u;
    c->cur_blk = 4u;
    c->fm_con = 4u;
    for (i = 0u; i < 4u; ++i) c->fm_tl[i] = 0u;
    c->rep_sp = 0u;
    for (i = 0u; i < MDXNT_REP_DEPTH; ++i) c->rep_cnt[i] = 0u;
    c->lfo_on = 0u;
    c->lfo_delay = 0u;
    c->lfo_speed = 0u;
    c->lfo_count = 0u;
    c->lfo_depth = 0;
    c->lfo_d_cnt = 0u;
    c->lfo_s_cnt = 0u;
    c->lfo_c_cnt = 0u;
    c->lfo_dir = 1;
    c->lfo_cur = 0;
}

static uint8_t ssg_scaled_vol(uint8_t vol, uint8_t env_lvl)
{
    uint16_t p;
    if (vol == 0u || env_lvl == 0u) return 0u;
    /* Bit-exact division-free equivalent of ((vol * env_lvl) + 7) / 15 for 0..15 */
    p = (uint16_t)((uint8_t)vol * (uint8_t)env_lvl);
    return (uint8_t)((p + (p << 4) + 136u) >> 8);
}

static void step_ssg_env(MdxntCh *c, uint8_t hw_ch, uint8_t can_out)
{
    const SsgEnvPreset *ep;
    uint8_t prev_lvl;
    uint8_t nv;
    if (c->env_phase == ENV_PHASE_OFF) return;
    prev_lvl = c->env_lvl;
    ep = &SSG_ENVS[(c->voice < 6u) ? c->voice : 0u];

    if (c->env_phase == ENV_PHASE_ATK) {
        if ((uint8_t)(c->env_lvl + ep->ar) >= 15u) {
            c->env_lvl = 15u;
            c->env_phase = (ep->dr > 0u && ep->sl < 15u) ? ENV_PHASE_DEC : ENV_PHASE_SUS;
            c->env_cnt = 0u;
        } else {
            c->env_lvl = (uint8_t)(c->env_lvl + ep->ar);
        }
    } else if (c->env_phase == ENV_PHASE_DEC) {
        ++c->env_cnt;
        if (c->env_cnt >= ep->dr) {
            c->env_cnt = 0u;
            if (c->env_lvl > ep->sl) {
                --c->env_lvl;
            }
            if (c->env_lvl <= ep->sl) {
                c->env_lvl = ep->sl;
                c->env_phase = ENV_PHASE_SUS;
            }
        }
    } else if (c->env_phase == ENV_PHASE_SUS) {
        if (ep->sr > 0u && c->env_lvl > 0u) {
            ++c->env_cnt;
            if (c->env_cnt >= ep->sr) {
                c->env_cnt = 0u;
                --c->env_lvl;
            }
        }
    } else if (c->env_phase == ENV_PHASE_REL) {
        if (ep->rr == 0u) {
            c->env_lvl = 0u;
            c->env_phase = ENV_PHASE_OFF;
        } else {
            ++c->env_cnt;
            if (c->env_cnt >= ep->rr) {
                c->env_cnt = 0u;
                if (c->env_lvl > 0u) --c->env_lvl;
                if (c->env_lvl == 0u) c->env_phase = ENV_PHASE_OFF;
            }
        }
    }

    if (c->env_lvl == prev_lvl) return;
    nv = ssg_scaled_vol(c->vol, c->env_lvl);
    if (nv != c->cur_vol) {
        c->cur_vol = nv;
        if (can_out) ssg_write_vol(hw_ch, nv);
    }
}

static void ssg_sync_vol(MdxntCh *c, uint8_t hw_ch, uint8_t can_out)
{
    uint8_t nv = ssg_scaled_vol(c->vol, c->env_lvl);
    if (nv != c->cur_vol) {
        c->cur_vol = nv;
        if (can_out) ssg_write_vol(hw_ch, nv);
    }
}

/* -------------------------------------------------------------------------- */
/* Channel Sequencer Step (1 Timer-B tick)                                    */
/* -------------------------------------------------------------------------- */
static void step_channel(uint8_t slot)
{
    MdxntCh *c = &s_ch[slot];
    uint8_t is_fm = (slot < 3u) ? 1u : 0u;
    uint8_t is_se = (slot >= 6u) ? 1u : 0u;
    uint8_t hw_ch = is_fm ? slot : (is_se ? (uint8_t)(slot - 5u) : (uint8_t)(slot - 3u));
    uint8_t can_out = is_fm ? 1u : (is_se ? ssg_ch_owned_by_se(hw_ch) : ssg_ch_owned_by_bgm(hw_ch));
    uint8_t cmd_guard = 64u;

    if (!c->active) return;

    /* 1. Ongoing note: pitch sweep, SSG soft envelope, gate-off */
    if (c->wait > 1u) {
        --c->wait;
        if (c->key_on) {
            uint8_t freq_dirty = 0u;
            if (c->sweep != 0) {
                int16_t nf = (int16_t)c->cur_freq + c->sweep;
                if (is_fm) {
                    if (nf < 256) nf = 256;
                    if (nf > 2047) nf = 2047;
                } else {
                    if (nf < 16) nf = 16;
                    if (nf > 4095) nf = 4095;
                }
                c->cur_freq = (uint16_t)nf;
                freq_dirty = 1u;
            }
            if (c->lfo_on) {
                if (c->lfo_d_cnt > 0u) {
                    --c->lfo_d_cnt;
                } else {
                    if (c->lfo_s_cnt > 0u) --c->lfo_s_cnt;
                    if (c->lfo_s_cnt == 0u) {
                        c->lfo_s_cnt = c->lfo_speed;
                        c->lfo_cur += (c->lfo_dir > 0) ? c->lfo_depth : (int16_t)(-c->lfo_depth);
                        freq_dirty = 1u;
                        if (c->lfo_c_cnt > 0u) --c->lfo_c_cnt;
                        if (c->lfo_c_cnt == 0u) {
                            c->lfo_dir = (int8_t)(-c->lfo_dir);
                            c->lfo_c_cnt = c->lfo_count;
                        }
                    }
                }
            }
            if (freq_dirty) {
                if (is_fm) {
                    int16_t out_f = (int16_t)c->cur_freq + c->lfo_cur;
                    if (out_f < 256) out_f = 256;
                    if (out_f > 2047) out_f = 2047;
                    fm_write_freq(hw_ch, c->cur_blk, (uint16_t)out_f);
                } else {
                    /* SSG LFO uses 1/16 TP fixed-point resolution (symmetric, no DC bias) */
                    if (can_out) ssg_write_tone(hw_ch, ssg_eff_tp(c));
                }
            }
            if (c->gate > 0u) {
                --c->gate;
                if (c->gate == 0u && !c->tie) {
                    c->key_on = 0u;
                    if (is_fm) {
                        fm_key_off(hw_ch);
                    } else {
                        c->env_phase = ENV_PHASE_REL;
                        c->env_cnt = 0u;
                    }
                }
            }
        }
        /* Always advance SSG ADSR envelope (including smooth Release after key-off!) */
        if (!is_fm) {
            step_ssg_env(c, hw_ch, can_out);
        }
        return;
    }

    /* 2. Fetch next MDX-compatible command(s) until a note/rest or end */
    while (cmd_guard--) {
        uint8_t cmd = *c->ptr++;

        if (cmd < 0x80u) {
            /* Rest: ticks = cmd + 1 */
            c->wait = (uint16_t)(cmd + 1u);
            c->gate = 0u;
            c->tie = 0u;
            if (c->key_on) {
                c->key_on = 0u;
                if (is_fm) {
                    fm_key_off(hw_ch);
                } else {
                    c->env_phase = ENV_PHASE_REL;
                    c->env_cnt = 0u;
                }
            }
            if (!is_fm) {
                step_ssg_env(c, hw_ch, can_out);
            }
            return;
        }

        if (cmd < 0xE0u) {
            /* Note: note = cmd - 0x80, next byte = ticks - 1 (1..256 ticks!) */
            uint8_t note = (uint8_t)(cmd - 0x80u);
            uint16_t ticks = (uint16_t)(*c->ptr++) + 1u;
            uint8_t oct = 0u;
            uint8_t semi = note;
            uint8_t was_tie = (c->tie && c->key_on) ? 1u : 0u;
            while (semi >= 12u) {
                semi = (uint8_t)(semi - 12u);
                ++oct;
            }
            c->tie = 0u;
            c->wait = ticks;

            /* Reset software LFO phase on fresh key-on */
            if (!was_tie && c->lfo_on) {
                c->lfo_d_cnt = c->lfo_delay;
                c->lfo_s_cnt = (c->lfo_delay > 0u) ? 1u : c->lfo_speed;
                c->lfo_c_cnt = (c->lfo_count > 1u) ? (uint8_t)(c->lfo_count >> 1) : 1u;
                c->lfo_dir = 1;
                c->lfo_cur = 0;
            }

            /* Gate calculation:
               - Next effective command is tie (0xF7) -> full sustain
               - c->q has bit 7 set (@qN) -> key-off N ticks before end (0 = full sustain)
               - c->q in 1..8 (MDX ratio) -> ticks * q / 8 (8 = full sustain) */
            if (next_is_tie(c->ptr)) {
                c->gate = 0u;
            } else if (c->q & 0x80u) {
                uint8_t off_t = (uint8_t)(c->q & 0x7Fu);
                if (off_t == 0u) c->gate = 0u;
                else c->gate = (ticks > off_t) ? (uint16_t)(ticks - off_t) : 1u;
            } else if (c->q >= 8u) {
                c->gate = 0u;
            } else {
                uint16_t g = 0u;
                if (c->q & 1u) g = (uint16_t)(g + ticks);
                if (c->q & 2u) g = (uint16_t)(g + (ticks << 1));
                if (c->q & 4u) g = (uint16_t)(g + (ticks << 2));
                g >>= 3;
                c->gate = (g < 1u) ? 1u : g;
            }

            if (is_fm) {
                uint8_t blk = oct;
                int16_t fnum = (int16_t)FM_FNUM[semi] + c->detune;
                int16_t out_f;
                if (fnum < 256) fnum = 256;
                if (fnum > 2047) fnum = 2047;
                c->cur_blk = blk;
                c->cur_freq = (uint16_t)fnum;
                out_f = (was_tie && c->lfo_on) ? (int16_t)(fnum + c->lfo_cur) : fnum;
                if (out_f < 256) out_f = 256;
                if (out_f > 2047) out_f = 2047;
                if (!was_tie) fm_key_off(hw_ch);
                fm_write_freq(hw_ch, blk, (uint16_t)out_f);
                if (!was_tie) fm_key_on(hw_ch);
                c->key_on = 1u;
            } else {
                uint16_t tp = SSG_TP_O1[semi];
                int16_t adj;
                if (oct > 1u) {
                    tp = (uint16_t)((tp + (uint16_t)(1u << (oct - 2u))) >> (oct - 1u));
                }
                /* SSG detune uses 1/16 TP fixed-point resolution (symmetric) */
                adj = (int16_t)tp - div16_sym(c->detune);
                if (adj < 16) adj = 16;
                if (adj > 4095) adj = 4095;
                c->cur_freq = (uint16_t)adj;
                if (!was_tie) {
                    const SsgEnvPreset *ep = &SSG_ENVS[(c->voice < 6u) ? c->voice : 0u];
                    c->env_lvl = ep->al;
                    c->env_cnt = 0u;
                    if (ep->ar > 0u && ep->al < 15u) c->env_phase = ENV_PHASE_ATK;
                    else if (ep->dr > 0u && ep->sl < 15u) c->env_phase = ENV_PHASE_DEC;
                    else c->env_phase = ENV_PHASE_SUS;
                    c->cur_vol = ssg_scaled_vol(c->vol, c->env_lvl);
                }
                if (can_out) {
                    ssg_apply_mixer(hw_ch, c->ssg_mode, c->ssg_nfreq);
                    ssg_write_tone(hw_ch, (was_tie && c->lfo_on) ? ssg_eff_tp(c) : c->cur_freq);
                    if (!was_tie) ssg_write_vol(hw_ch, c->cur_vol);
                }
                c->key_on = 1u;
            }
            return;
        }

        /* Extended MDX commands 0xE0..0xFF */
        if (cmd == 0xFFu) {
            /* Set OPN Timer-B tempo (BGM only, SE shouldn't hijack BGM tempo unless no BGM) */
            uint8_t tb = *c->ptr++;
            if (!is_se || !bgm_any_active()) {
                mdxnt_opn_write(0x26u, tb);
            }
        } else if (cmd == 0xFDu) {
            uint8_t v = *c->ptr++;
            c->voice = v;
            if (is_fm) {
                if (c->key_on && !c->tie) {
                    fm_key_off(hw_ch);
                    c->key_on = 0u;
                }
                fm_load_voice(hw_ch, c, v);
            }
        } else if (cmd == 0xFBu) {
            c->vol = (uint8_t)(*c->ptr++ & 15u);
            if (is_fm) fm_apply_tl(hw_ch, c);
            else ssg_sync_vol(c, hw_ch, can_out);
        } else if (cmd == 0xFAu) {
            if (c->vol > 0u) --c->vol;
            if (is_fm) fm_apply_tl(hw_ch, c);
            else ssg_sync_vol(c, hw_ch, can_out);
        } else if (cmd == 0xF9u) {
            if (c->vol < 15u) ++c->vol;
            if (is_fm) fm_apply_tl(hw_ch, c);
            else ssg_sync_vol(c, hw_ch, can_out);
        } else if (cmd == 0xF8u) {
            c->q = *c->ptr++;
        } else if (cmd == 0xF7u) {
            c->tie = 1u;
        } else if (cmd == 0xF6u) {
            uint8_t cnt = *c->ptr++;
            (void)*c->ptr++; /* reserved 0 */
            if (c->rep_sp < MDXNT_REP_DEPTH) {
                c->rep_cnt[c->rep_sp++] = cnt;
            }
        } else if (cmd == 0xF5u) {
            int16_t rel = read_s16(c->ptr);
            c->ptr += 2;
            if (c->rep_sp > 0u) {
                uint8_t top = (uint8_t)(c->rep_sp - 1u);
                if (c->rep_cnt[top] > 1u) {
                    --c->rep_cnt[top];
                    c->ptr += rel;
                } else {
                    --c->rep_sp;
                }
            }
        } else if (cmd == 0xF4u) {
            int16_t rel = read_s16(c->ptr);
            c->ptr += 2;
            if (c->rep_sp > 0u && c->rep_cnt[c->rep_sp - 1u] == 1u) {
                --c->rep_sp;
                c->ptr += rel;
            }
        } else if (cmd == 0xF3u) {
            c->detune = read_s16(c->ptr);
            c->ptr += 2;
        } else if (cmd == 0xF2u) {
            c->sweep = read_s16(c->ptr);
            c->ptr += 2;
        } else if (cmd == 0xEEu) {
            uint8_t lcnt;
            c->lfo_delay = *c->ptr++;
            c->lfo_speed = *c->ptr++;
            c->lfo_depth = read_s16(c->ptr);
            c->ptr += 2;
            lcnt = *c->ptr++;
            /* Keep lfo_count even in [2, 254] so half-period oscillation is drift-free */
            if (lcnt < 2u) lcnt = 2u;
            else if (lcnt == 255u) lcnt = 254u;
            else if (lcnt & 1u) ++lcnt;
            c->lfo_count = lcnt;
            c->lfo_on = (c->lfo_speed != 0u && c->lfo_depth != 0) ? 1u : 0u;
            c->lfo_d_cnt = c->lfo_delay;
            c->lfo_s_cnt = (c->lfo_delay > 0u) ? 1u : c->lfo_speed;
            c->lfo_c_cnt = (c->lfo_count > 1u) ? (uint8_t)(c->lfo_count >> 1) : 1u;
            c->lfo_dir = 1;
            c->lfo_cur = 0;
        } else if (cmd == 0xEDu) {
            c->ssg_mode = *c->ptr++;
            c->ssg_nfreq = *c->ptr++;
        } else if (cmd == 0xF1u) {
            uint16_t loop_off = read_u16(c->ptr);
            c->ptr += 2;
            if (loop_off != 0u) {
                c->rep_sp = 0u;
                c->ptr = c->base + loop_off;
            } else {
                c->active = 0u;
                c->key_on = 0u;
                c->cur_vol = 0u;
                if (is_fm) {
                    fm_key_off(hw_ch);
                } else {
                    ssg_restore_channel(hw_ch);
                }
                return;
            }
        } else {
            /* Unknown opcode: stop channel and silence/restore safely */
            c->active = 0u;
            c->key_on = 0u;
            c->cur_vol = 0u;
            if (is_fm) fm_key_off(hw_ch);
            else ssg_restore_channel(hw_ch);
            return;
        }
    }
    /* ISR safety net: stop channel if a malformed stream loops without consuming ticks */
    c->active = 0u;
    c->key_on = 0u;
    c->cur_vol = 0u;
    if (is_fm) fm_key_off(hw_ch);
    else ssg_restore_channel(hw_ch);
}

/* -------------------------------------------------------------------------- */
/* OPN Timer-B ISR Entry                                                      */
/* -------------------------------------------------------------------------- */
void mdxnt_tick_isr(void)
{
    uint8_t i;
    /* Re-arm YM2203 Timer-B immediately */
    mdxnt_hw_ack_timer_b();

    /* 1. Service queued one-shot SSG voice blips (contention-free!) */
    for (i = 0u; i < 3u; ++i) {
        if (s_blip[i].pending) {
            s_blip[i].pending = 0u;
            ssg_apply_mixer(i, 0u, 12u);
            ssg_write_tone(i, s_blip[i].tp);
            ssg_write_vol(i, s_blip[i].vol);
        } else if (s_blip[i].ticks > 0u) {
            --s_blip[i].ticks;
            if (s_blip[i].ticks == 1u) {
                ssg_write_vol(i, (uint8_t)(s_blip[i].vol >> 1));
            } else if (s_blip[i].ticks == 0u) {
                ssg_restore_channel(i);
            }
        }
    }

    /* 2. Step BGM channels (0..5) and SE channels (6..7) */
    for (i = 0u; i < MDXNT_TOTAL_CH; ++i) {
        if (s_ch[i].active) {
            step_channel(i);
        }
    }
    s_se_active = (s_ch[6].active || s_ch[7].active) ? 1u : 0u;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */
void mdxnt_init(void)
{
    uint8_t i;
    mdxnt_enter_crit();
    for (i = 0u; i < MDXNT_TOTAL_CH; ++i) s_ch[i].active = 0u;
    for (i = 0u; i < 3u; ++i) {
        s_blip[i].pending = 0u;
        s_blip[i].ticks = 0u;
    }
    s_se_active = 0u;
    s_ssg_mixer = 0x38u;

    /* Guarantee YM2203 prescaler 1/6 FM + 1/4 SSG on warm boot */
    mdxnt_opn_write(0x2Du, 0u);

    /* Silence FM 3ch + SSG 3ch */
    for (i = 0u; i < 3u; ++i) fm_key_off(i);
    mdxnt_opn_write(7u, 0x38u);
    mdxnt_opn_write(8u, 0u);
    mdxnt_opn_write(9u, 0u);
    mdxnt_opn_write(10u, 0u);

    s_inited = 1u;
    /* Enable OPN Timer-B IRQ (~120 BPM = TB 220 -> ~96.3 Hz tick rate on YM2203 1152 divider) */
    mdxnt_hw_enable_irq(220u);
}

void mdxnt_shutdown(void)
{
    uint8_t i;
    if (!s_inited) return;
    mdxnt_hw_disable_irq();
    for (i = 0u; i < MDXNT_TOTAL_CH; ++i) s_ch[i].active = 0u;
    for (i = 0u; i < 3u; ++i) fm_key_off(i);
    mdxnt_opn_write(8u, 0u);
    mdxnt_opn_write(9u, 0u);
    mdxnt_opn_write(10u, 0u);
    s_se_active = 0u;
    s_inited = 0u;
}

void mdxnt_play_bgm(const uint8_t *mdxt_data)
{
    uint8_t i;
    uint16_t vtbl_off;
    const uint8_t *vtbl;
    if (!mdxt_data || mdxt_data[0] != 'M' || mdxt_data[1] != 'D' ||
        mdxt_data[2] != 'X' || mdxt_data[3] != 'T') {
        return;
    }
    mdxnt_enter_crit();
    for (i = 0u; i < 3u; ++i) fm_key_off(i);
    for (i = 0u; i < 3u; ++i) {
        if (ssg_ch_owned_by_bgm(i)) ssg_write_vol(i, 0u);
    }
    vtbl_off = read_u16(mdxt_data + 6u);
    vtbl = (vtbl_off != 0u) ? (mdxt_data + vtbl_off) : (const uint8_t *)0;
    mdxnt_opn_write(0x26u, mdxt_data[5]);
    for (i = 0u; i < MDXNT_NUM_BGM_CH; ++i) {
        uint16_t coff = read_u16(mdxt_data + 8u + (uint8_t)(i << 1));
        ch_reset(&s_ch[i], mdxt_data, vtbl, coff);
    }
    mdxnt_leave_crit();
}

void mdxnt_stop_bgm(void)
{
    uint8_t i;
    mdxnt_enter_crit();
    for (i = 0u; i < MDXNT_NUM_BGM_CH; ++i) s_ch[i].active = 0u;
    for (i = 0u; i < 3u; ++i) fm_key_off(i);
    for (i = 0u; i < 3u; ++i) {
        if (ssg_ch_owned_by_bgm(i)) ssg_write_vol(i, 0u);
    }
    mdxnt_leave_crit();
}

uint8_t mdxnt_bgm_active(void)
{
    return bgm_any_active();
}

void mdxnt_play_se(const uint8_t *mdxt_data)
{
    uint16_t off_d, off_e;
    if (!mdxt_data || mdxt_data[0] != 'M' || mdxt_data[1] != 'D' ||
        mdxt_data[2] != 'X' || mdxt_data[3] != 'T') {
        return;
    }
    mdxnt_enter_crit();
    /* SE uses MML channels E (idx 4) & F (idx 5) -> hardware SSG ch.B (1) & ch.C (2) */
    off_d = read_u16(mdxt_data + 8u + 8u);  /* ch 4 (E) */
    off_e = read_u16(mdxt_data + 8u + 10u); /* ch 5 (F) */
    if (!bgm_any_active()) {
        mdxnt_opn_write(0x26u, mdxt_data[5]);
    }
    ch_reset(&s_ch[6], mdxt_data, (const uint8_t *)0, off_d);
    ch_reset(&s_ch[7], mdxt_data, (const uint8_t *)0, off_e);
    if (!s_ch[6].active) ssg_restore_channel(1u);
    else if (ssg_ch_owned_by_se(1u)) ssg_write_vol(1u, 0u);
    if (!s_ch[7].active) ssg_restore_channel(2u);
    else if (ssg_ch_owned_by_se(2u)) ssg_write_vol(2u, 0u);
    s_se_active = (s_ch[6].active || s_ch[7].active) ? 1u : 0u;
    mdxnt_leave_crit();
}

void mdxnt_stop_se(void)
{
    mdxnt_enter_crit();
    s_ch[6].active = 0u;
    s_ch[7].active = 0u;
    s_se_active = 0u;
    ssg_restore_channel(1u);
    ssg_restore_channel(2u);
    mdxnt_leave_crit();
}

uint8_t mdxnt_se_busy(void)
{
    return s_se_active;
}

void mdxnt_ssg_blip(uint8_t ssg_ch, uint16_t tp, uint8_t vol, uint8_t ticks)
{
    if (ssg_ch >= 3u) return;
    if (ticks == 0u) ticks = 1u;
    mdxnt_enter_crit();
    s_blip[ssg_ch].tp = tp;
    s_blip[ssg_ch].vol = (uint8_t)(vol & 15u);
    s_blip[ssg_ch].ticks = ticks;
    s_blip[ssg_ch].pending = 1u;
    mdxnt_leave_crit();
}
