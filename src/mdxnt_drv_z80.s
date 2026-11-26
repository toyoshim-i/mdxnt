; =============================================================================
; MDX-OPN (YM2203: FM 3ch + SSG 3ch) Pure Z80 Sound Driver (SDCC --sdcccall 1)
; Bit-exact Z80 port of src/mdxnt_drv.c (YM2203 OPN, SDCC --sdcccall 1).
; =============================================================================
        .module mdxnt_drv
        .globl  _mdxnt_opn_write
        .globl  _mdxnt_hw_enable_irq
        .globl  _mdxnt_hw_disable_irq
        .globl  _mdxnt_hw_ack_timer_b
        .globl  _mdxnt_enter_crit
        .globl  _mdxnt_leave_crit

; Channel struct layout (51 bytes)
CH_ACTIVE       = 0
CH_PTR          = 1
CH_BASE         = 3
CH_VOICE_TBL    = 5
CH_WAIT         = 7
CH_GATE         = 9
CH_Q            = 11
CH_VOL          = 12
CH_CUR_VOL      = 13
CH_VOICE        = 14
CH_ENV_PHASE    = 15
CH_ENV_CNT      = 16
CH_ENV_LVL      = 17
CH_TIE          = 18
CH_KEY_ON       = 19
CH_SSG_MODE     = 20
CH_SSG_NFREQ    = 21
CH_DETUNE       = 22
CH_SWEEP        = 24
CH_CUR_FREQ     = 26
CH_CUR_BLK      = 28
CH_FM_CON       = 29
CH_FM_TL        = 30
CH_REP_SP       = 34
CH_REP_CNT      = 35
CH_LFO_ON       = 39
CH_LFO_DELAY    = 40
CH_LFO_SPEED    = 41
CH_LFO_COUNT    = 42
CH_LFO_DEPTH    = 43
CH_LFO_D_CNT    = 45
CH_LFO_S_CNT    = 46
CH_LFO_C_CNT    = 47
CH_LFO_DIR      = 48
CH_LFO_CUR      = 49
CH_SIZE         = 51

ENV_OFF         = 0
ENV_ATK         = 1
ENV_DEC         = 2
ENV_SUS         = 3
ENV_REL         = 4

BLIP_PENDING    = 0
BLIP_TICKS      = 1
BLIP_TP         = 2
BLIP_VOL        = 4
BLIP_SIZE       = 5

        .area   _DATA
_s_ch:
        .ds     8 * CH_SIZE     ; 408 B
_s_blip:
        .ds     3 * BLIP_SIZE   ; 15 B
_s_inited:
        .ds     1
_s_se_active:
        .ds     1
_sc_slot:
        .ds     1
_sc_flags:
        .ds     1               ; bit0=is_fm, bit1=is_se, bit2=can_out, bit3=freq_dirty, bit4=was_tie
_sc_hw:
        .ds     1
_sc_guard:
        .ds     1
_sc_ticks:
        .ds     2
_sc_oct:
        .ds     1
_sc_semi:
        .ds     1

        .area   _INITIALIZED
_s_ssg_mixer:
        .ds     1

        .area   _INITIALIZER
        .db     0x38

        .area   _CODE
; -----------------------------------------------------------------------------
; Pitch, Volume & Software Envelope Tables
; -----------------------------------------------------------------------------
_FM_FNUM:
        .dw     618, 655, 694, 735, 779, 825, 874, 926, 981, 1040, 1102, 1167

_SSG_TP_O1:
        .dw     3816, 3602, 3400, 3209, 3029, 2859
        .dw     2699, 2547, 2404, 2269, 2142, 2022

_FM_CARRIER_MASK:
        .db     0x08, 0x08, 0x08, 0x08, 0x0C, 0x0E, 0x0E, 0x0F

_FM_VOL_TL:
        .db     127, 52, 44, 38, 32, 27, 23, 19
        .db      16, 13, 10,  8,  6,  4,  2,  0

; SsgEnvPreset: al, ar, dr, sl, sr, rr (6 bytes each)
_SSG_ENVS:
        .db     15, 0, 0, 15,  0, 1
        .db     12, 2, 4, 12, 24, 2
        .db     15, 0, 2,  8, 16, 1
        .db     15, 0, 3, 11, 20, 2
        .db      9, 1, 6, 12, 32, 3
        .db     15, 0, 1,  0,  0, 1

; -----------------------------------------------------------------------------
; Helpers
; -----------------------------------------------------------------------------
; Signed negate HL
_neg_hl:
        xor     a
        sub     l
        ld      l, a
        sbc     a, a
        sub     h
        ld      h, a
        ret

; Portable symmetric (int16_t)HL / 16
_div16_sym:
        bit     7, h
        jr      nz, _d16_neg
        ld      b, #4
_d16_pos_lp:
        srl     h
        rr      l
        djnz    _d16_pos_lp
        ret
_d16_neg:
        call    _neg_hl
        ld      b, #4
_d16_neg_lp:
        srl     h
        rr      l
        djnz    _d16_neg_lp
        jr      _neg_hl

; Clamp signed HL to [256, 2047]
_clamp_fm:
        bit     7, h
        jr      nz, _cfm_lo
        ld      a, h
        or      a
        jr      z, _cfm_lo
        cp      #0x08
        ret     c
        ld      hl, #2047
        ret
_cfm_lo:
        ld      hl, #256
        ret

; Clamp signed HL to [16, 4095]
_clamp_ssg16:
        bit     7, h
        jr      nz, _csg_lo
        ld      a, h
        or      a
        jr      nz, _csg_hi
        ld      a, l
        cp      #16
        ret     nc
_csg_lo:
        ld      hl, #16
        ret
_csg_hi:
        cp      #0x10
        ret     c
        ld      hl, #4095
        ret

; Return DE = &table[A] (preserved C, IX)
_lut_u8:
        add     a, l
        ld      l, a
        ret     nc
        inc     h
        ret

; Load HL = table[A * 2] (uint16_t)
_lut_u16:
        add     a, a
        add     a, l
        ld      l, a
        jr      nc, _lu16_ok
        inc     h
_lu16_ok:
        ld      a, (hl)
        inc     hl
        ld      h, (hl)
        ld      l, a
        ret

; Compute A = ssg_scaled_vol(vol=A, env_lvl=E) -> ((p + (p<<4) + 136) >> 8)
_ssg_scaled_vol:
        and     #0x0F
        ret     z
        ld      d, a
        ld      a, e
        and     #0x0F
        ret     z
        ld      e, a
        ; p = D * E (both 0..15 -> max 225 fits in 8 bits)
        xor     a
        ld      b, #4
_sv_mul:
        add     a, a
        rl      d
        bit     4, d
        jr      z, _sv_skip
        add     a, e
_sv_skip:
        djnz    _sv_mul
        ; HL = p + (p << 4) + 136
        ld      e, a
        ld      d, #0
        ld      l, a
        ld      h, #0
        add     hl, hl
        add     hl, hl
        add     hl, hl
        add     hl, hl
        add     hl, de
        ld      de, #136
        add     hl, de
        ld      a, h
        ret

; Return IY = &SSG_ENVS[min(voice, 5)] (clobbers A, DE)
_get_env_preset:
        ld      a, CH_VOICE(ix)
        cp      #6
        jr      c, _gep_ok
        xor     a
_gep_ok:
        ld      e, a
        add     a, a            ; *2
        add     a, a            ; *4
        add     a, e            ; *5
        add     a, e            ; *6
        ld      e, a
        ld      d, #0
        ld      iy, #_SSG_ENVS
        add     iy, de
        ret

; Compute &s_ch[A] into HL (clobbers A, DE)
_ch_ptr_a:
        ld      l, a
        ld      h, #0
        ld      d, h
        ld      e, l            ; 1x
        add     hl, hl          ; 2x
        add     hl, de          ; 3x
        add     hl, hl          ; 6x
        add     hl, hl          ; 12x
        add     hl, hl          ; 24x
        add     hl, de          ; 25x
        add     hl, hl          ; 50x
        add     hl, de          ; 51x
        ld      de, #_s_ch
        add     hl, de
        ret

; Safe opn write preserving C and IX/IY (A = reg, L = val)
_opn_wr:
        push    bc
        push    iy
        call    _mdxnt_opn_write
        pop     iy
        pop     bc
        ret

; FM key off/on (A = hw_ch)
_fm_key_off:
        ld      l, a
        ld      a, #0x28
        jr      _opn_wr

_fm_key_on:
        or      #0xF0
        ld      l, a
        ld      a, #0x28
        jr      _opn_wr

; ssg_write_vol(C = ssg_ch, A = vol) (preserves C)
_ssg_write_vol:
        and     #0x0F
        ld      l, a
        ld      a, c
        add     a, #8
        jr      _opn_wr

; ssg_write_tone(C = ssg_ch, HL = tp) (preserves C)
_ssg_write_tone:
        ld      a, h
        cp      #0x10
        jr      c, _swt_chk0
        ld      hl, #4095
        jr      _swt_out
_swt_chk0:
        or      l
        jr      nz, _swt_out
        ld      hl, #1
_swt_out:
        push    hl
        ld      a, c
        add     a, a
        push    af
        call    _opn_wr         ; reg = c*2, val = l (lo)
        pop     af
        pop     hl
        inc     a
        ld      l, h
        jr      _opn_wr         ; reg = c*2+1, val = h (hi)

; ssg_apply_mixer(C = ssg_ch, D = mode, E = nfreq) (preserves C)
_ssg_apply_mixer:
        push    de
        ld      b, c
        inc     b
        ld      a, #0x80
_sam_sh:
        rlca
        djnz    _sam_sh
        ld      h, a            ; H = tbit = 1 << ssg_ch
        add     a, a
        add     a, a
        add     a, a
        ld      l, a            ; L = nbit = 8 << ssg_ch
        ld      a, d
        or      a
        jr      z, _sam_m0
        dec     a
        jr      z, _sam_m1
        jr      _sam_m2
_sam_m0:
        ; mode == 0: (mixer & ~tbit) | nbit
        ld      a, (_s_ssg_mixer)
        cpl
        or      h
        cpl
        or      l
        ld      (_s_ssg_mixer), a
        jr      _sam_wr7
_sam_m1:
        ; mode == 1: (mixer | tbit) & ~nbit
        ld      a, (_s_ssg_mixer)
        or      h
        cpl
        or      l
        cpl
        jr      _sam_wrn
_sam_m2:
        ; mode >= 2: mixer & ~(tbit | nbit)
        ld      a, (_s_ssg_mixer)
        ld      e, a
        ld      a, h
        or      l
        cpl
        and     e
_sam_wrn:
        ld      (_s_ssg_mixer), a
        pop     de
        push    de
        ld      a, e
        and     #0x1F
        ld      l, a
        ld      a, #6
        call    _opn_wr
_sam_wr7:
        pop     de
        ld      a, (_s_ssg_mixer)
        ld      l, a
        ld      a, #7
        jp      _opn_wr

; ssg_eff_tp(IX = c) -> returns uint16_t in HL
_ssg_eff_tp:
        ld      l, CH_CUR_FREQ(ix)
        ld      h, CH_CUR_FREQ+1(ix)
        ld      a, CH_LFO_ON(ix)
        or      a
        jr      z, _set_clamp
        push    hl
        ld      l, CH_LFO_CUR(ix)
        ld      h, CH_LFO_CUR+1(ix)
        call    _div16_sym
        ex      de, hl
        pop     hl
        or      a
        sbc     hl, de
_set_clamp:
        jp      _clamp_ssg16

; ssg_ch_owned_by_bgm(A = ssg_ch) -> Z if NOT owned, NZ (A=1) if owned (preserves C)
_ssg_ch_owned_by_bgm:
        ld      b, a
        ld      e, a
        add     a, a
        add     a, a
        add     a, e
        ld      l, a
        ld      h, #0
        ld      de, #_s_blip
        add     hl, de
        ld      a, (hl)         ; pending
        inc     hl
        or      (hl)            ; | ticks
        jr      nz, _scob_no
        ld      a, b
        cp      #1
        jr      nz, _scob_c2
        ld      a, (_s_ch + 6 * CH_SIZE + CH_ACTIVE)
        or      a
        jr      nz, _scob_no
        jr      _scob_yes
_scob_c2:
        cp      #2
        jr      nz, _scob_yes
        ld      a, (_s_ch + 7 * CH_SIZE + CH_ACTIVE)
        or      a
        jr      nz, _scob_no
_scob_yes:
        ld      a, #1
        or      a
        ret
_scob_no:
        xor     a
        ret

; ssg_ch_owned_by_se(A = ssg_ch) -> A=0/1 (preserves C)
_ssg_ch_owned_by_se:
        ld      e, a
        add     a, a
        add     a, a
        add     a, e
        ld      l, a
        ld      h, #0
        ld      de, #_s_blip
        add     hl, de
        ld      a, (hl)         ; pending
        inc     hl
        or      (hl)            ; | ticks
        jr      nz, _scob_no
        ld      a, #1
        ret

; uint8_t mdxnt_bgm_active(void) / bgm_any_active() -> A=1 if ch0..5 active, else 0
_mdxnt_bgm_active::
_bgm_any_active:
        ld      hl, #_s_ch
        ld      de, #CH_SIZE
        ld      b, #6
_baa_lp:
        ld      a, (hl)
        or      a
        jr      nz, _scob_yes
        add     hl, de
        djnz    _baa_lp
        xor     a
        ret

; ssg_restore_channel(A = hw_ch) (preserves IX)
_ssg_restore_channel:
        push    ix
        ld      c, a            ; C = hw_ch
        call    _ssg_ch_owned_by_se
        or      a
        jr      z, _src_done    ; blip still holds channel
        ld      a, c
        cp      #1
        jr      nz, _src_chk2
        ld      ix, #_s_ch + 6 * CH_SIZE
        ld      a, CH_ACTIVE(ix)
        or      a
        jr      nz, _src_owner
        jr      _src_bgm
_src_chk2:
        cp      #2
        jr      nz, _src_bgm
        ld      ix, #_s_ch + 7 * CH_SIZE
        ld      a, CH_ACTIVE(ix)
        or      a
        jr      nz, _src_owner
_src_bgm:
        ld      a, c
        add     a, #3
        call    _ch_ptr_a
        push    hl
        pop     ix
        ld      a, CH_ACTIVE(ix)
        or      a
        jr      z, _src_silence
_src_owner:
        ld      d, CH_SSG_MODE(ix)
        ld      e, CH_SSG_NFREQ(ix)
        call    _ssg_apply_mixer
        call    _ssg_eff_tp
        call    _ssg_write_tone
        ld      a, CH_CUR_VOL(ix)
        call    _ssg_write_vol
        jr      _src_done
_src_silence:
        ld      de, #0x000C     ; D=mode 0, E=nfreq 12
        call    _ssg_apply_mixer
        xor     a
        call    _ssg_write_vol
_src_done:
        pop     ix
        ret

; fm_apply_tl(A = fm_ch, IX = c)
_fm_apply_tl_clean:
        ; Entry: A = fm_ch, IX = c
        push    af              ; [SP] high byte = fm_ch
        ld      a, CH_FM_CON(ix)
        and     #7
        ld      hl, #_FM_CARRIER_MASK
        call    _lut_u8
        ld      d, (hl)         ; D = cmask
        ld      a, CH_VOL(ix)
        and     #15
        ld      hl, #_FM_VOL_TL
        call    _lut_u8
        ld      e, (hl)         ; E = atten
        pop     af              ; A = fm_ch
        add     a, #0x40
        ld      c, a            ; C = reg 0x40 + fm_ch
        push    ix
        pop     hl
        push    de
        ld      de, #CH_FM_TL
        add     hl, de          ; HL = &fm_tl[0]
        pop     de              ; D = cmask, E = atten
        ld      b, #4
_fat_lp:
        ld      a, (hl)
        inc     hl
        rrc     d
        jr      nc, _fat_wr
        add     a, e
        jr      c, _fat_max
        cp      #128
        jr      c, _fat_wr
_fat_max:
        ld      a, #127
_fat_wr:
        push    hl
        push    de
        ld      l, a
        ld      a, c
        push    bc
        call    _mdxnt_opn_write
        pop     bc
        pop     de
        pop     hl
        ld      a, c
        add     a, #4
        ld      c, a
        djnz    _fat_lp
        ret

; fm_load_voice(A = fm_ch, IX = c, L = vnum)
_fm_load_voice:
        ld      e, CH_VOICE_TBL(ix)
        ld      d, CH_VOICE_TBL+1(ix)
        ld      a, d
        or      e
        ret     z
        push    de              ; [SP+2] = voice_tbl
        ; Offset = L * 26
        ld      e, l
        ld      d, #0
        ld      h, d
        ld      l, e
        add     hl, hl          ; 2
        push    hl              ; [SP] = 2*vnum
        add     hl, hl          ; 4
        add     hl, hl          ; 8
        ld      b, h
        ld      c, l            ; BC = 8
        add     hl, hl          ; 16
        add     hl, bc          ; 24
        pop     bc              ; BC = 2
        add     hl, bc          ; HL = 26*vnum
        pop     de
        add     hl, de          ; HL = vp
        push    iy
        push    hl
        pop     iy              ; IY = vp
        ld      a, (_sc_hw)
        ld      c, a            ; C = fm_ch
        ld      b, #4
_flv_lp:
        push    bc
        ; DT1/MUL: 0x30 + off, vp[0]
        ld      a, c
        add     a, #0x30
        ld      l, 0(iy)
        call    _opn_wr
        ; c->fm_tl[op] = vp[4]
        ld      a, 4(iy)
        push    ix
        pop     hl
        ld      e, #CH_FM_TL
        ld      d, #0
        add     hl, de
        ; Wait: we need current op index 0..3 -> easiest if we advance HL or compute op = 4 - B!
        ld      a, #4
        sub     b
        ld      e, a
        add     hl, de
        ld      a, 4(iy)
        ld      (hl), a
        ; KS/AR: 0x50 + off, vp[8]
        ld      a, c
        add     a, #0x50
        ld      l, 8(iy)
        call    _opn_wr
        ; DR: 0x60 + off, vp[12]
        ld      a, c
        add     a, #0x60
        ld      l, 12(iy)
        call    _opn_wr
        ; SR: 0x70 + off, vp[16]
        ld      a, c
        add     a, #0x70
        ld      l, 16(iy)
        call    _opn_wr
        ; SL/RR: 0x80 + off, vp[20]
        ld      a, c
        add     a, #0x80
        ld      l, 20(iy)
        call    _opn_wr
        pop     bc
        inc     iy              ; vp++
        ld      a, c
        add     a, #4
        ld      c, a
        djnz    _flv_lp

        ; IY is now vp + 4, so 20(iy) == orig vp[24] (FB/CON)
        ld      a, 20(iy)
        ld      l, a
        and     #7
        ld      CH_FM_CON(ix), a
        ld      a, (_sc_hw)
        add     a, #0xB0
        call    _opn_wr
        pop     iy
        ld      a, (_sc_hw)
        jp      _fm_apply_tl_clean

; fm_write_freq(C = hw_ch, B = blk, HL = fnum) (preserves C)
_fm_write_freq:
        push    hl
        ld      a, b
        and     #7
        add     a, a
        add     a, a
        add     a, a
        ld      d, a
        ld      a, h
        and     #7
        or      d
        ld      l, a
        ld      a, c
        add     a, #0xA4
        call    _opn_wr
        pop     hl
        ld      a, c
        add     a, #0xA0
        jp      _opn_wr

; next_is_tie(HL = p) -> A=1 if tie, 0 otherwise
_next_is_tie:
        ld      b, #16
_nit_lp:
        ld      a, (hl)
        cp      #0xF7
        jr      z, _nit_yes
        ld      de, #1
        cp      #0xFA
        jr      z, _nit_adv
        cp      #0xF9
        jr      z, _nit_adv
        inc     de              ; 2
        cp      #0xFF
        jr      z, _nit_adv
        cp      #0xFD
        jr      z, _nit_adv
        cp      #0xFB
        jr      z, _nit_adv
        cp      #0xF8
        jr      z, _nit_adv
        inc     de              ; 3
        cp      #0xF3
        jr      z, _nit_adv
        cp      #0xF2
        jr      z, _nit_adv
        cp      #0xED
        jr      z, _nit_adv
        cp      #0xEE
        jr      nz, _nit_no
        ld      e, #6
_nit_adv:
        add     hl, de
        djnz    _nit_lp
_nit_no:
        xor     a
        ret
_nit_yes:
        ld      a, #1
        ret

; ch_reset(IX = c, HL = base, DE = vtbl, BC = off)
_ch_reset:
        push    hl
        push    de
        push    bc
        push    ix
        pop     hl
        ld      d, h
        ld      e, l
        inc     de
        ld      bc, #CH_SIZE - 1
        ld      (hl), #0
        ldir
        pop     bc              ; BC = off
        pop     de              ; DE = vtbl
        pop     hl              ; HL = base
        ld      a, b
        or      c
        jr      z, _cr_act0
        ld      a, #1
_cr_act0:
        ld      CH_ACTIVE(ix), a
        ld      CH_BASE(ix), l
        ld      CH_BASE+1(ix), h
        add     hl, bc
        ld      CH_PTR(ix), l
        ld      CH_PTR+1(ix), h
        ld      CH_VOICE_TBL(ix), e
        ld      CH_VOICE_TBL+1(ix), d
        ld      CH_WAIT(ix), #1
        ld      CH_Q(ix), #7
        ld      CH_VOL(ix), #12
        ld      CH_SSG_NFREQ(ix), #12
        ld      CH_CUR_FREQ+1(ix), #1   ; 256
        ld      CH_CUR_BLK(ix), #4
        ld      CH_FM_CON(ix), #4
        ld      CH_LFO_DIR(ix), #1
        ret

; step_ssg_env(IX = c, C = hw_ch, flags bit2 = can_out)
_step_ssg_env:
        ld      a, CH_ENV_PHASE(ix)
        or      a
        ret     z
        push    iy
        ld      d, CH_ENV_LVL(ix)       ; D = prev_lvl
        push    de
        call    _get_env_preset         ; IY = ep (al, ar, dr, sl, sr, rr)
        pop     de                      ; D = prev_lvl
        ld      a, CH_ENV_PHASE(ix)
        cp      #ENV_ATK
        jr      nz, _sse_chk_dec
        ld      a, CH_ENV_LVL(ix)
        add     a, 1(iy)                ; + ep->ar
        cp      #15
        jr      c, _sse_atk_nc
        ld      CH_ENV_LVL(ix), #15
        ld      a, 2(iy)                ; dr
        or      a
        jr      z, _sse_set_sus
        ld      a, 3(iy)                ; sl
        cp      #15
        jr      nc, _sse_set_sus
        ld      a, #ENV_DEC
        jr      _sse_set_ph
_sse_set_sus:
        ld      a, #ENV_SUS
_sse_set_ph:
        ld      CH_ENV_PHASE(ix), a
        ld      CH_ENV_CNT(ix), #0
        jp      _sse_done
_sse_atk_nc:
        ld      CH_ENV_LVL(ix), a
        jp      _sse_done
_sse_chk_dec:
        cp      #ENV_DEC
        jr      nz, _sse_chk_sus
        ld      a, CH_ENV_CNT(ix)
        inc     a
        ld      CH_ENV_CNT(ix), a
        cp      2(iy)                   ; dr
        jr      c, _sse_done
        ld      CH_ENV_CNT(ix), #0
        ld      a, CH_ENV_LVL(ix)
        ld      e, 3(iy)                ; sl
        cp      e
        jr      z, _sse_dec_le
        jr      c, _sse_dec_le
        dec     a
        ld      CH_ENV_LVL(ix), a
        cp      e
        jr      z, _sse_dec_le
        jr      nc, _sse_done
_sse_dec_le:
        ld      CH_ENV_LVL(ix), e
        ld      CH_ENV_PHASE(ix), #ENV_SUS
        jr      _sse_done
_sse_chk_sus:
        cp      #ENV_SUS
        jr      nz, _sse_chk_rel
        ld      a, 4(iy)                ; sr
        or      a
        jr      z, _sse_done
        ld      a, CH_ENV_LVL(ix)
        or      a
        jr      z, _sse_done
        ld      a, CH_ENV_CNT(ix)
        inc     a
        ld      CH_ENV_CNT(ix), a
        cp      4(iy)                   ; sr
        jr      c, _sse_done
        ld      CH_ENV_CNT(ix), #0
        dec     CH_ENV_LVL(ix)
        jr      _sse_done
_sse_chk_rel:
        cp      #ENV_REL
        jr      nz, _sse_done
        ld      a, 5(iy)                ; rr
        or      a
        jr      nz, _sse_rel_cnt
        ld      CH_ENV_LVL(ix), #0
        ld      CH_ENV_PHASE(ix), #ENV_OFF
        jr      _sse_done
_sse_rel_cnt:
        ld      a, CH_ENV_CNT(ix)
        inc     a
        ld      CH_ENV_CNT(ix), a
        cp      5(iy)                   ; rr
        jr      c, _sse_done
        ld      CH_ENV_CNT(ix), #0
        ld      a, CH_ENV_LVL(ix)
        or      a
        jr      z, _sse_rel_zero
        dec     a
        ld      CH_ENV_LVL(ix), a
_sse_rel_zero:
        or      a
        jr      nz, _sse_done
        ld      CH_ENV_PHASE(ix), #ENV_OFF
_sse_done:
        pop     iy
        ld      a, CH_ENV_LVL(ix)
        cp      d                       ; == prev_lvl?
        ret     z
_ssg_sync_vol:
        ld      e, CH_ENV_LVL(ix)
        ld      a, CH_VOL(ix)
        push    bc
        call    _ssg_scaled_vol
        pop     bc
        cp      CH_CUR_VOL(ix)
        ret     z
        ld      CH_CUR_VOL(ix), a
        ld      hl, #_sc_flags
        bit     2, (hl)                 ; can_out?
        ret     z
        jp      _ssg_write_vol

; -----------------------------------------------------------------------------
; step_channel(A = slot)
; -----------------------------------------------------------------------------
_step_channel:
        ld      (_sc_slot), a
        call    _ch_ptr_a
        push    hl
        pop     ix
        ld      a, CH_ACTIVE(ix)
        or      a
        ret     z

        ; Compute _sc_flags (bit0=is_fm, bit1=is_se, bit2=can_out) and _sc_hw
        ld      a, (_sc_slot)
        cp      #3
        jr      nc, _sc_not_fm
        ld      (_sc_hw), a
        ld      c, a
        ld      a, #0x05                ; is_fm=1, can_out=1
        ld      (_sc_flags), a
        jr      _sc_chk_wait
_sc_not_fm:
        cp      #6
        jr      c, _sc_ssg_bgm
        sub     #5
        ld      (_sc_hw), a
        ld      c, a
        call    _ssg_ch_owned_by_se
        ld      e, #0x02                ; is_se=1
        jr      _sc_set_fl
_sc_ssg_bgm:
        sub     #3
        ld      (_sc_hw), a
        ld      c, a
        call    _ssg_ch_owned_by_bgm
        ld      e, #0x00
_sc_set_fl:
        or      a
        jr      z, _sc_fl_nco
        set     2, e                    ; can_out=1
_sc_fl_nco:
        ld      a, e
        ld      (_sc_flags), a

_sc_chk_wait:
        ; Check if wait > 1
        ld      l, CH_WAIT(ix)
        ld      h, CH_WAIT+1(ix)
        ld      a, h
        or      a
        jr      nz, _sc_ongoing
        ld      a, l
        cp      #2
        jp      c, _sc_fetch_start

_sc_ongoing:
        dec     hl
        ld      CH_WAIT(ix), l
        ld      CH_WAIT+1(ix), h
        ld      a, CH_KEY_ON(ix)
        or      a
        jp      z, _sc_on_env

        ld      hl, #_sc_flags
        res     3, (hl)                 ; freq_dirty = 0
        ; sweep
        ld      e, CH_SWEEP(ix)
        ld      d, CH_SWEEP+1(ix)
        ld      a, d
        or      e
        jr      z, _sc_on_lfo
        ld      l, CH_CUR_FREQ(ix)
        ld      h, CH_CUR_FREQ+1(ix)
        add     hl, de
        ld      a, (_sc_flags)
        bit     0, a
        jr      z, _sc_sw_ssg
        call    _clamp_fm
        jr      _sc_sw_st
_sc_sw_ssg:
        call    _clamp_ssg16
_sc_sw_st:
        ld      CH_CUR_FREQ(ix), l
        ld      CH_CUR_FREQ+1(ix), h
        ld      hl, #_sc_flags
        set     3, (hl)
_sc_on_lfo:
        ld      a, CH_LFO_ON(ix)
        or      a
        jr      z, _sc_on_fdirty
        ld      a, CH_LFO_D_CNT(ix)
        or      a
        jr      z, _sc_lfo_step
        dec     a
        ld      CH_LFO_D_CNT(ix), a
        jr      _sc_on_fdirty
_sc_lfo_step:
        ld      a, CH_LFO_S_CNT(ix)
        or      a
        jr      z, _sc_lfo_s0
        dec     a
        ld      CH_LFO_S_CNT(ix), a
        jr      nz, _sc_on_fdirty
_sc_lfo_s0:
        ld      a, CH_LFO_SPEED(ix)
        ld      CH_LFO_S_CNT(ix), a
        ld      l, CH_LFO_CUR(ix)
        ld      h, CH_LFO_CUR+1(ix)
        ld      e, CH_LFO_DEPTH(ix)
        ld      d, CH_LFO_DEPTH+1(ix)
        bit     7, CH_LFO_DIR(ix)
        jr      nz, _sc_lfo_sub
        add     hl, de
        jr      _sc_lfo_cur_st
_sc_lfo_sub:
        or      a
        sbc     hl, de
_sc_lfo_cur_st:
        ld      CH_LFO_CUR(ix), l
        ld      CH_LFO_CUR+1(ix), h
        ld      hl, #_sc_flags
        set     3, (hl)
        ld      a, CH_LFO_C_CNT(ix)
        or      a
        jr      z, _sc_lfo_c0
        dec     a
        ld      CH_LFO_C_CNT(ix), a
        jr      nz, _sc_on_fdirty
_sc_lfo_c0:
        xor     a
        sub     CH_LFO_DIR(ix)
        ld      CH_LFO_DIR(ix), a
        ld      a, CH_LFO_COUNT(ix)
        ld      CH_LFO_C_CNT(ix), a
_sc_on_fdirty:
        ld      a, (_sc_flags)
        bit     3, a
        jr      z, _sc_on_gate
        bit     0, a
        jr      z, _sc_fdirty_ssg
        ld      l, CH_CUR_FREQ(ix)
        ld      h, CH_CUR_FREQ+1(ix)
        ld      e, CH_LFO_CUR(ix)
        ld      d, CH_LFO_CUR+1(ix)
        add     hl, de
        call    _clamp_fm
        ld      b, CH_CUR_BLK(ix)
        ld      a, (_sc_hw)
        ld      c, a
        call    _fm_write_freq
        jr      _sc_on_gate
_sc_fdirty_ssg:
        bit     2, a
        jr      z, _sc_on_gate
        call    _ssg_eff_tp
        ld      a, (_sc_hw)
        ld      c, a
        call    _ssg_write_tone
_sc_on_gate:
        ld      l, CH_GATE(ix)
        ld      h, CH_GATE+1(ix)
        ld      a, h
        or      l
        jr      z, _sc_on_env
        dec     hl
        ld      CH_GATE(ix), l
        ld      CH_GATE+1(ix), h
        ld      a, h
        or      l
        jr      nz, _sc_on_env
        ld      a, CH_TIE(ix)
        or      a
        jr      nz, _sc_on_env
        ld      CH_KEY_ON(ix), #0
        ld      a, (_sc_flags)
        bit     0, a
        jr      z, _sc_gate_rel
        ld      a, (_sc_hw)
        call    _fm_key_off
        jr      _sc_on_env
_sc_gate_rel:
        ld      CH_ENV_PHASE(ix), #ENV_REL
        ld      CH_ENV_CNT(ix), #0
_sc_on_env:
        ld      a, (_sc_flags)
        bit     0, a
        ret     nz
        ld      a, (_sc_hw)
        ld      c, a
        jp      _step_ssg_env

; 2. Command fetch loop (guard = 64)
_sc_fetch_start:
        ld      a, #64
        ld      (_sc_guard), a
_sc_fetch_lp:
        ld      a, (_sc_guard)
        or      a
        jp      z, _sc_stop_safe
        dec     a
        ld      (_sc_guard), a

        ; cmd = *c->ptr++
        ld      l, CH_PTR(ix)
        ld      h, CH_PTR+1(ix)
        ld      a, (hl)
        inc     hl
        ld      CH_PTR(ix), l
        ld      CH_PTR+1(ix), h

        bit     7, a
        jr      nz, _sc_not_rest

        ; Rest: wait = cmd + 1
        inc     a
        ld      CH_WAIT(ix), a
        ld      CH_WAIT+1(ix), #0
        ld      CH_GATE(ix), #0
        ld      CH_GATE+1(ix), #0
        ld      CH_TIE(ix), #0
        ld      a, CH_KEY_ON(ix)
        or      a
        jr      z, _sc_rest_env
        ld      CH_KEY_ON(ix), #0
        ld      a, (_sc_flags)
        bit     0, a
        jr      z, _sc_rest_rel
        ld      a, (_sc_hw)
        call    _fm_key_off
        ret
_sc_rest_rel:
        ld      CH_ENV_PHASE(ix), #ENV_REL
        ld      CH_ENV_CNT(ix), #0
_sc_rest_env:
        ld      a, (_sc_flags)
        bit     0, a
        ret     nz
        ld      a, (_sc_hw)
        ld      c, a
        jp      _step_ssg_env

_sc_not_rest:
        cp      #0xE0
        jp      nc, _sc_ext_cmd

        ; Note: note = A - 0x80
        sub     #0x80
        ld      b, a
        ; ticks = (*c->ptr++) + 1 (1..256)
        ld      l, CH_PTR(ix)
        ld      h, CH_PTR+1(ix)
        ld      e, (hl)
        inc     hl
        ld      CH_PTR(ix), l
        ld      CH_PTR+1(ix), h
        ld      d, #0
        inc     de
        ld      (_sc_ticks), de
        ld      CH_WAIT(ix), e
        ld      CH_WAIT+1(ix), d

        ; oct & semi
        ld      a, b
        ld      c, #0
_sc_oct_lp:
        cp      #12
        jr      c, _sc_oct_ok
        sub     #12
        inc     c
        jr      _sc_oct_lp
_sc_oct_ok:
        ld      (_sc_semi), a
        ld      a, c
        ld      (_sc_oct), a

        ; was_tie = (tie && key_on) ? 1 : 0 -> store in _sc_flags bit4
        ld      hl, #_sc_flags
        res     4, (hl)
        ld      a, CH_TIE(ix)
        and     CH_KEY_ON(ix)
        or      a
        jr      z, _sc_no_wt
        set     4, (hl)
_sc_no_wt:
        ld      CH_TIE(ix), #0
        ; Reset LFO on fresh key-on
        bit     4, (hl)
        jr      nz, _sc_gate_calc
        ld      a, CH_LFO_ON(ix)
        or      a
        jr      z, _sc_gate_calc
        ld      a, CH_LFO_DELAY(ix)
        ld      CH_LFO_D_CNT(ix), a
        or      a
        jr      z, _sc_lfo_nod
        ld      a, #1
        jr      _sc_lfo_st_s
_sc_lfo_nod:
        ld      a, CH_LFO_SPEED(ix)
_sc_lfo_st_s:
        ld      CH_LFO_S_CNT(ix), a
        ld      a, CH_LFO_COUNT(ix)
        cp      #2
        jr      c, _sc_lfo_c1
        srl     a
        jr      _sc_lfo_st_c
_sc_lfo_c1:
        ld      a, #1
_sc_lfo_st_c:
        ld      CH_LFO_C_CNT(ix), a
        ld      CH_LFO_DIR(ix), #1
        ld      CH_LFO_CUR(ix), #0
        ld      CH_LFO_CUR+1(ix), #0

_sc_gate_calc:
        ld      l, CH_PTR(ix)
        ld      h, CH_PTR+1(ix)
        call    _next_is_tie
        or      a
        jr      nz, _sc_gate_zero
        ld      a, CH_Q(ix)
        bit     7, a
        jr      z, _sc_gate_ratio
        and     #0x7F
        jr      z, _sc_gate_zero
        ld      e, a
        ld      d, #0
        ld      hl, (_sc_ticks)
        or      a
        sbc     hl, de
        jr      c, _sc_gate_one
        jr      z, _sc_gate_one
        jr      _sc_gate_set
_sc_gate_ratio:
        cp      #8
        jr      nc, _sc_gate_zero
        ld      e, a            ; E = q (0..7)
        ld      hl, #0
        ld      bc, (_sc_ticks)
        bit     0, e
        jr      z, _sc_gr1
        add     hl, bc
_sc_gr1:
        sla     c
        rl      b               ; 2*ticks
        bit     1, e
        jr      z, _sc_gr2
        add     hl, bc
_sc_gr2:
        sla     c
        rl      b               ; 4*ticks
        bit     2, e
        jr      z, _sc_gr4
        add     hl, bc
_sc_gr4:
        ; >> 3
        srl     h
        rr      l
        srl     h
        rr      l
        srl     h
        rr      l
        ld      a, h
        or      l
        jr      nz, _sc_gate_set
_sc_gate_one:
        ld      hl, #1
        jr      _sc_gate_set
_sc_gate_zero:
        ld      hl, #0
_sc_gate_set:
        ld      CH_GATE(ix), l
        ld      CH_GATE+1(ix), h

        ; Pitch & hardware output
        ld      a, (_sc_flags)
        bit     0, a
        jr      z, _sc_note_ssg

        ; FM note
        ld      a, (_sc_oct)
        ld      CH_CUR_BLK(ix), a
        ld      a, (_sc_semi)
        ld      hl, #_FM_FNUM
        call    _lut_u16
        ld      e, CH_DETUNE(ix)
        ld      d, CH_DETUNE+1(ix)
        add     hl, de
        call    _clamp_fm
        ld      CH_CUR_FREQ(ix), l
        ld      CH_CUR_FREQ+1(ix), h
        ld      a, (_sc_flags)
        bit     4, a                    ; was_tie?
        jr      nz, _sc_fm_wt_chk
        push    hl
        ld      a, (_sc_hw)
        call    _fm_key_off
        pop     hl
        jr      _sc_fm_out
_sc_fm_wt_chk:
        ld      a, CH_LFO_ON(ix)
        or      a
        jr      z, _sc_fm_out
        ld      e, CH_LFO_CUR(ix)
        ld      d, CH_LFO_CUR+1(ix)
        add     hl, de
        call    _clamp_fm
_sc_fm_out:
        ld      b, CH_CUR_BLK(ix)
        ld      a, (_sc_hw)
        ld      c, a
        call    _fm_write_freq
        ld      a, (_sc_flags)
        bit     4, a
        jp      nz, _sc_note_done
        ld      a, (_sc_hw)
        call    _fm_key_on
        jp      _sc_note_done

_sc_note_ssg:
        ld      a, (_sc_semi)
        ld      hl, #_SSG_TP_O1
        call    _lut_u16
        ld      a, (_sc_oct)
        cp      #2
        jr      c, _sc_ssg_oct_done
        dec     a
        dec     a
        jr      z, _sc_ssg_oct_last
        ld      b, a
_sc_ssg_oct_lp:
        srl     h
        rr      l
        djnz    _sc_ssg_oct_lp
_sc_ssg_oct_last:
        srl     h
        rr      l
        jr      nc, _sc_ssg_oct_done
        inc     hl
_sc_ssg_oct_done:
        push    hl
        ld      l, CH_DETUNE(ix)
        ld      h, CH_DETUNE+1(ix)
        call    _div16_sym
        ex      de, hl
        pop     hl
        or      a
        sbc     hl, de
        call    _clamp_ssg16
        ld      CH_CUR_FREQ(ix), l
        ld      CH_CUR_FREQ+1(ix), h
        ld      a, (_sc_flags)
        bit     4, a                    ; was_tie?
        jr      nz, _sc_ssg_out
        push    iy
        call    _get_env_preset         ; IY = ep (al=0, ar=1, dr=2, sl=3)
        ld      a, 0(iy)                ; al
        ld      CH_ENV_LVL(ix), a
        ld      CH_ENV_CNT(ix), #0
        ld      a, 1(iy)                ; ar
        or      a
        jr      z, _sc_se_chk_d
        ld      a, 0(iy)
        cp      #15
        jr      nc, _sc_se_chk_d
        ld      a, #ENV_ATK
        jr      _sc_se_st
_sc_se_chk_d:
        ld      a, 2(iy)                ; dr
        or      a
        jr      z, _sc_se_sus
        ld      a, 3(iy)                ; sl
        cp      #15
        jr      nc, _sc_se_sus
        ld      a, #ENV_DEC
        jr      _sc_se_st
_sc_se_sus:
        ld      a, #ENV_SUS
_sc_se_st:
        ld      CH_ENV_PHASE(ix), a
        pop     iy
        ld      e, CH_ENV_LVL(ix)
        ld      a, CH_VOL(ix)
        call    _ssg_scaled_vol
        ld      CH_CUR_VOL(ix), a
_sc_ssg_out:
        ld      a, (_sc_flags)
        bit     2, a                    ; can_out?
        jr      z, _sc_note_done
        ld      a, (_sc_hw)
        ld      c, a
        ld      d, CH_SSG_MODE(ix)
        ld      e, CH_SSG_NFREQ(ix)
        call    _ssg_apply_mixer
        ld      a, (_sc_flags)
        bit     4, a
        jr      z, _sc_ssg_nof
        ld      a, CH_LFO_ON(ix)
        or      a
        jr      z, _sc_ssg_nof
        call    _ssg_eff_tp
        jr      _sc_ssg_wt
_sc_ssg_nof:
        ld      l, CH_CUR_FREQ(ix)
        ld      h, CH_CUR_FREQ+1(ix)
_sc_ssg_wt:
        ld      a, (_sc_hw)
        ld      c, a
        call    _ssg_write_tone
        ld      a, (_sc_flags)
        bit     4, a
        jr      nz, _sc_note_done
        ld      a, CH_CUR_VOL(ix)
        call    _ssg_write_vol
_sc_note_done:
        ld      CH_KEY_ON(ix), #1
        ret

; Read 1 byte from *c->ptr++ into A
_rd_p8:
        ld      l, CH_PTR(ix)
        ld      h, CH_PTR+1(ix)
        ld      a, (hl)
        inc     hl
        ld      CH_PTR(ix), l
        ld      CH_PTR+1(ix), h
        ret

; Read 2 bytes from *c->ptr into DE and advance ptr += 2
_rd_p16:
        ld      l, CH_PTR(ix)
        ld      h, CH_PTR+1(ix)
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        inc     hl
        ld      CH_PTR(ix), l
        ld      CH_PTR+1(ix), h
        ret

; Extended MDX commands (0xE0..0xFF)
_sc_ext_cmd:
        cp      #0xFF
        jr      nz, _ext_fd
        call    _rd_p8
        ld      l, a
        ld      a, (_sc_flags)
        bit     1, a                    ; is_se?
        jr      z, _ext_ff_wr
        push    hl
        call    _bgm_any_active
        pop     hl
        or      a
        jp      nz, _sc_fetch_lp
_ext_ff_wr:
        ld      a, #0x26
        call    _opn_wr
        jp      _sc_fetch_lp

_ext_fd:
        cp      #0xFD
        jr      nz, _ext_fb
        call    _rd_p8
        ld      CH_VOICE(ix), a
        ld      e, a
        ld      a, (_sc_flags)
        bit     0, a
        jp      z, _sc_fetch_lp
        ld      a, CH_KEY_ON(ix)
        or      a
        jr      z, _ext_fd_ld
        ld      a, CH_TIE(ix)
        or      a
        jr      nz, _ext_fd_ld
        push    de
        ld      a, (_sc_hw)
        call    _fm_key_off
        pop     de
        ld      CH_KEY_ON(ix), #0
_ext_fd_ld:
        ld      l, e
        ld      a, (_sc_hw)
        call    _fm_load_voice
        jp      _sc_fetch_lp

_ext_fb:
        cp      #0xFB
        jr      nz, _ext_fa
        call    _rd_p8
        and     #15
_ext_vset:
        ld      CH_VOL(ix), a
        ld      a, (_sc_flags)
        bit     0, a
        jr      z, _ext_vssg
        ld      a, (_sc_hw)
        call    _fm_apply_tl_clean
        jp      _sc_fetch_lp
_ext_vssg:
        ld      a, (_sc_hw)
        ld      c, a
        call    _ssg_sync_vol
        jp      _sc_fetch_lp

_ext_fa:
        cp      #0xFA
        jr      nz, _ext_f9
        ld      a, CH_VOL(ix)
        or      a
        jr      z, _ext_vset
        dec     a
        jr      _ext_vset

_ext_f9:
        cp      #0xF9
        jr      nz, _ext_f8
        ld      a, CH_VOL(ix)
        cp      #15
        jr      nc, _ext_vset
        inc     a
        jr      _ext_vset

_ext_f8:
        cp      #0xF8
        jr      nz, _ext_f7
        call    _rd_p8
        ld      CH_Q(ix), a
        jp      _sc_fetch_lp

_ext_f7:
        cp      #0xF7
        jr      nz, _ext_f6
        ld      CH_TIE(ix), #1
        jp      _sc_fetch_lp

_ext_f6:
        cp      #0xF6
        jr      nz, _ext_f5
        call    _rd_p16                 ; E = cnt, D = reserved
        ld      a, CH_REP_SP(ix)
        cp      #4
        jp      nc, _sc_fetch_lp
        push    ix
        pop     hl
        add     a, #CH_REP_CNT
        ld      c, a
        ld      b, #0
        add     hl, bc
        ld      (hl), e
        inc     CH_REP_SP(ix)
        jp      _sc_fetch_lp

_ext_f5:
        cp      #0xF5
        jr      nz, _ext_f4
        call    _rd_p16                 ; DE = rel
        ld      a, CH_REP_SP(ix)
        or      a
        jp      z, _sc_fetch_lp
        dec     a
        push    ix
        pop     hl
        add     a, #CH_REP_CNT
        ld      c, a
        ld      b, #0
        add     hl, bc
        ld      a, (hl)
        cp      #2
        jr      c, _ext_f5_pop
        dec     (hl)
_ext_jmp_rel:
        ld      l, CH_PTR(ix)
        ld      h, CH_PTR+1(ix)
        add     hl, de
        ld      CH_PTR(ix), l
        ld      CH_PTR+1(ix), h
        jp      _sc_fetch_lp
_ext_f5_pop:
        dec     CH_REP_SP(ix)
        jp      _sc_fetch_lp

_ext_f4:
        cp      #0xF4
        jr      nz, _ext_f3
        call    _rd_p16                 ; DE = rel
        ld      a, CH_REP_SP(ix)
        or      a
        jp      z, _sc_fetch_lp
        dec     a
        push    ix
        pop     hl
        add     a, #CH_REP_CNT
        ld      c, a
        ld      b, #0
        add     hl, bc
        ld      a, (hl)
        cp      #1
        jp      nz, _sc_fetch_lp
        dec     CH_REP_SP(ix)
        jr      _ext_jmp_rel

_ext_f3:
        cp      #0xF3
        jr      nz, _ext_f2
        call    _rd_p16
        ld      CH_DETUNE(ix), e
        ld      CH_DETUNE+1(ix), d
        jp      _sc_fetch_lp

_ext_f2:
        cp      #0xF2
        jr      nz, _ext_ee
        call    _rd_p16
        ld      CH_SWEEP(ix), e
        ld      CH_SWEEP+1(ix), d
        jp      _sc_fetch_lp

_ext_ee:
        cp      #0xEE
        jr      nz, _ext_ed
        call    _rd_p16                 ; E = delay, D = speed
        ld      CH_LFO_DELAY(ix), e
        ld      CH_LFO_SPEED(ix), d
        call    _rd_p16                 ; DE = depth
        ld      CH_LFO_DEPTH(ix), e
        ld      CH_LFO_DEPTH+1(ix), d
        call    _rd_p8                  ; A = lcnt
        cp      #2
        jr      nc, _ee_not_lt2
        ld      a, #2
        jr      _ee_cnt_ok
_ee_not_lt2:
        cp      #255
        jr      nz, _ee_even
        ld      a, #254
        jr      _ee_cnt_ok
_ee_even:
        bit     0, a
        jr      z, _ee_cnt_ok
        inc     a
_ee_cnt_ok:
        ld      CH_LFO_COUNT(ix), a
        ld      b, a                    ; save lfo_count
        xor     a
        ld      c, a
        ld      a, CH_LFO_SPEED(ix)
        or      a
        jr      z, _ee_on_ok
        ld      a, CH_LFO_DEPTH(ix)
        or      CH_LFO_DEPTH+1(ix)
        jr      z, _ee_on_ok
        ld      c, #1
_ee_on_ok:
        ld      CH_LFO_ON(ix), c
        ld      a, CH_LFO_DELAY(ix)
        ld      CH_LFO_D_CNT(ix), a
        or      a
        jr      z, _ee_nod
        ld      a, #1
        jr      _ee_sc_st
_ee_nod:
        ld      a, CH_LFO_SPEED(ix)
_ee_sc_st:
        ld      CH_LFO_S_CNT(ix), a
        ld      a, b
        cp      #2
        jr      c, _ee_c1
        srl     a
        jr      _ee_cc_st
_ee_c1:
        ld      a, #1
_ee_cc_st:
        ld      CH_LFO_C_CNT(ix), a
        ld      CH_LFO_DIR(ix), #1
        ld      CH_LFO_CUR(ix), #0
        ld      CH_LFO_CUR+1(ix), #0
        jp      _sc_fetch_lp

_ext_ed:
        cp      #0xED
        jr      nz, _ext_f1
        call    _rd_p16                 ; E = mode, D = nfreq
        ld      CH_SSG_MODE(ix), e
        ld      CH_SSG_NFREQ(ix), d
        jp      _sc_fetch_lp

_ext_f1:
        cp      #0xF1
        jr      nz, _sc_stop_safe
        call    _rd_p16                 ; DE = loop_off
        ld      a, d
        or      e
        jr      z, _sc_stop_safe
        ld      CH_REP_SP(ix), #0
        ld      l, CH_BASE(ix)
        ld      h, CH_BASE+1(ix)
        add     hl, de
        ld      CH_PTR(ix), l
        ld      CH_PTR+1(ix), h
        jp      _sc_fetch_lp

_sc_stop_safe:
        ld      CH_ACTIVE(ix), #0
        ld      CH_KEY_ON(ix), #0
        ld      CH_CUR_VOL(ix), #0
        ld      a, (_sc_flags)
        bit     0, a
        jr      z, _scs_ssg
        ld      a, (_sc_hw)
        jp      _fm_key_off
_scs_ssg:
        ld      a, (_sc_hw)
        jp      _ssg_restore_channel

; -----------------------------------------------------------------------------
; Public API (SDCC --sdcccall 1)
; -----------------------------------------------------------------------------
_mdxnt_tick_isr::
        push    ix
        push    iy
        call    _mdxnt_hw_ack_timer_b
        ; 1. Service queued one-shot SSG voice blips (0..2)
        ld      iy, #_s_blip
        ld      c, #0
_ti_blp:
        ld      a, BLIP_PENDING(iy)
        or      a
        jr      z, _ti_chk_t
        ld      BLIP_PENDING(iy), #0
        ld      de, #0x000C
        call    _ssg_apply_mixer
        ld      l, BLIP_TP(iy)
        ld      h, BLIP_TP+1(iy)
        call    _ssg_write_tone
        ld      a, BLIP_VOL(iy)
        call    _ssg_write_vol
        jr      _ti_bnxt
_ti_chk_t:
        ld      a, BLIP_TICKS(iy)
        or      a
        jr      z, _ti_bnxt
        dec     a
        ld      BLIP_TICKS(iy), a
        cp      #1
        jr      nz, _ti_chk0
        ld      a, BLIP_VOL(iy)
        srl     a
        call    _ssg_write_vol
        jr      _ti_bnxt
_ti_chk0:
        or      a
        jr      nz, _ti_bnxt
        ld      a, c
        call    _ssg_restore_channel
_ti_bnxt:
        ld      de, #BLIP_SIZE
        add     iy, de
        inc     c
        ld      a, c
        cp      #3
        jr      c, _ti_blp

        ; 2. Step BGM (0..5) and SE (6..7)
        xor     a
_ti_clp:
        push    af
        call    _step_channel
        pop     af
        inc     a
        cp      #8
        jr      c, _ti_clp

        ld      a, (_s_ch + 6 * CH_SIZE + CH_ACTIVE)
        ld      hl, #_s_ch + 7 * CH_SIZE + CH_ACTIVE
        or      (hl)
        jr      z, _ti_sa0
        ld      a, #1
_ti_sa0:
        ld      (_s_se_active), a
        pop     iy
        pop     ix
        ret

_mdxnt_init::
        push    ix
        call    _mdxnt_enter_crit
        ld      hl, #_s_ch
        ld      de, #_s_ch + 1
        ld      bc, #8 * CH_SIZE + 3 * BLIP_SIZE - 1
        ld      (hl), #0
        ldir
        xor     a
        ld      (_s_se_active), a
        ld      a, #0x38
        ld      (_s_ssg_mixer), a
        ; YM2203 prescaler 1/6 FM + 1/4 SSG
        ld      a, #0x2D
        ld      l, #0
        call    _opn_wr
        ; Silence FM 0..2 + SSG 0..2
        xor     a
_in_fm:
        push    af
        call    _fm_key_off
        pop     af
        inc     a
        cp      #3
        jr      c, _in_fm
        ld      a, #7
        ld      l, #0x38
        call    _opn_wr
        ld      a, #8
        ld      l, #0
        call    _opn_wr
        ld      a, #9
        ld      l, #0
        call    _opn_wr
        ld      a, #10
        ld      l, #0
        call    _opn_wr
        ld      a, #1
        ld      (_s_inited), a
        ld      a, #220
        call    _mdxnt_hw_enable_irq
        pop     ix
        ret

_mdxnt_shutdown::
        ld      a, (_s_inited)
        or      a
        ret     z
        push    ix
        call    _mdxnt_hw_disable_irq
        ld      hl, #_s_ch
        ld      de, #CH_SIZE
        ld      b, #8
_sd_clp:
        ld      (hl), #0
        add     hl, de
        djnz    _sd_clp
        xor     a
_sd_fm:
        push    af
        call    _fm_key_off
        pop     af
        inc     a
        cp      #3
        jr      c, _sd_fm
        ld      a, #8
        ld      l, #0
        call    _opn_wr
        ld      a, #9
        ld      l, #0
        call    _opn_wr
        ld      a, #10
        ld      l, #0
        call    _opn_wr
        xor     a
        ld      (_s_se_active), a
        ld      (_s_inited), a
        pop     ix
        ret

; Verify 'MDXT' magic at HL -> Z=ok, NZ=bad
_chk_mdxt:
        ld      a, h
        or      l
        jr      z, _cm_bad
        ld      a, (hl)
        cp      #'M'
        ret     nz
        inc     hl
        ld      a, (hl)
        cp      #'D'
        ret     nz
        inc     hl
        ld      a, (hl)
        cp      #'X'
        ret     nz
        inc     hl
        ld      a, (hl)
        cp      #'T'
        ret
_cm_bad:
        inc     a                       ; A != 0 -> NZ
        ret

_mdxnt_play_bgm::
        push    hl
        call    _chk_mdxt
        pop     hl
        ret     nz
        push    ix
        push    hl                      ; [SP] = mdxt_data
        call    _mdxnt_enter_crit
        xor     a
_pb2_fm:
        push    af
        call    _fm_key_off
        pop     af
        inc     a
        cp      #3
        jr      c, _pb2_fm
        ld      c, #0
_pb2_ssg:
        ld      a, c
        call    _ssg_ch_owned_by_bgm
        or      a
        jr      z, _pb2_sn
        xor     a
        call    _ssg_write_vol
_pb2_sn:
        inc     c
        ld      a, c
        cp      #3
        jr      c, _pb2_ssg

        pop     hl                      ; HL = mdxt_data
        push    hl
        ld      de, #5
        add     hl, de
        ld      a, (hl)                 ; mdxt_data[5]
        push    hl
        ld      l, a
        ld      a, #0x26
        call    _opn_wr
        pop     hl                      ; HL = mdxt_data + 5
        inc     hl
        ld      e, (hl)
        inc     hl
        ld      d, (hl)                 ; DE = vtbl_off
        pop     bc                      ; BC = mdxt_data
        push    bc
        ld      a, d
        or      e
        jr      z, _pb2_vt0
        ld      l, c
        ld      h, b
        add     hl, de
        ex      de, hl                  ; DE = vtbl
        jr      _pb2_vt_ok
_pb2_vt0:
        ld      de, #0
_pb2_vt_ok:
        pop     hl                      ; HL = mdxt_data
        xor     a
_pb2_lp:
        ld      (_sc_slot), a
        push    hl                      ; [SP+2] = mdxt_data
        push    de                      ; [SP]   = vtbl
        add     a, a
        add     a, #8
        ld      c, a
        ld      b, #0
        add     hl, bc
        ld      c, (hl)
        inc     hl
        ld      b, (hl)                 ; BC = coff
        push    bc                      ; [SP] = coff
        ld      a, (_sc_slot)
        call    _ch_ptr_a
        push    hl
        pop     ix
        pop     bc                      ; BC = coff
        pop     de                      ; DE = vtbl
        pop     hl                      ; HL = mdxt_data
        push    hl
        push    de
        call    _ch_reset
        pop     de
        pop     hl
        ld      a, (_sc_slot)
        inc     a
        cp      #6
        jr      c, _pb2_lp
        call    _mdxnt_leave_crit
        pop     ix
        ret

_mdxnt_stop_bgm::
        push    ix
        call    _mdxnt_enter_crit
        ld      hl, #_s_ch
        ld      de, #CH_SIZE
        ld      b, #6
_sb_clp:
        ld      (hl), #0
        add     hl, de
        djnz    _sb_clp
        xor     a
_sb_fm:
        push    af
        call    _fm_key_off
        pop     af
        inc     a
        cp      #3
        jr      c, _sb_fm
        ld      c, #0
_sb_ssg:
        ld      a, c
        call    _ssg_ch_owned_by_bgm
        or      a
        jr      z, _sb_sn
        xor     a
        call    _ssg_write_vol
_sb_sn:
        inc     c
        ld      a, c
        cp      #3
        jr      c, _sb_ssg
        call    _mdxnt_leave_crit
        pop     ix
        ret

_mdxnt_play_se::
        push    hl
        call    _chk_mdxt
        pop     hl
        ret     nz
        push    ix
        push    hl                      ; [SP] = mdxt_data
        call    _mdxnt_enter_crit
        call    _bgm_any_active
        pop     hl
        push    hl
        or      a
        jr      nz, _ps_no_tb
        ld      de, #5
        add     hl, de
        ld      l, (hl)
        ld      a, #0x26
        call    _opn_wr
_ps_no_tb:
        pop     hl                      ; HL = mdxt_data
        push    hl
        ld      de, #16
        add     hl, de
        ld      c, (hl)
        inc     hl
        ld      b, (hl)                 ; BC = off_d
        inc     hl
        ld      e, (hl)
        inc     hl
        ld      d, (hl)                 ; DE = off_e
        pop     hl                      ; HL = mdxt_data
        push    hl
        push    de
        ld      ix, #_s_ch + 6 * CH_SIZE
        ld      de, #0
        call    _ch_reset
        pop     bc                      ; BC = off_e
        pop     hl                      ; HL = mdxt_data
        ld      ix, #_s_ch + 7 * CH_SIZE
        ld      de, #0
        call    _ch_reset

        ld      a, (_s_ch + 6 * CH_SIZE + CH_ACTIVE)
        or      a
        jr      nz, _ps_ch6_act
        ld      a, #1
        call    _ssg_restore_channel
        jr      _ps_chk7
_ps_ch6_act:
        ld      c, #1
        ld      a, c
        call    _ssg_ch_owned_by_se
        or      a
        jr      z, _ps_chk7
        xor     a
        call    _ssg_write_vol
_ps_chk7:
        ld      a, (_s_ch + 7 * CH_SIZE + CH_ACTIVE)
        or      a
        jr      nz, _ps_ch7_act
        ld      a, #2
        call    _ssg_restore_channel
        jr      _ps_upd
_ps_ch7_act:
        ld      c, #2
        ld      a, c
        call    _ssg_ch_owned_by_se
        or      a
        jr      z, _ps_upd
        xor     a
        call    _ssg_write_vol
_ps_upd:
        ld      a, (_s_ch + 6 * CH_SIZE + CH_ACTIVE)
        ld      hl, #_s_ch + 7 * CH_SIZE + CH_ACTIVE
        or      (hl)
        jr      z, _ps_sa0
        ld      a, #1
_ps_sa0:
        ld      (_s_se_active), a
        call    _mdxnt_leave_crit
        pop     ix
        ret

_mdxnt_stop_se::
        push    ix
        call    _mdxnt_enter_crit
        xor     a
        ld      (_s_ch + 6 * CH_SIZE + CH_ACTIVE), a
        ld      (_s_ch + 7 * CH_SIZE + CH_ACTIVE), a
        ld      (_s_se_active), a
        ld      a, #1
        call    _ssg_restore_channel
        ld      a, #2
        call    _ssg_restore_channel
        call    _mdxnt_leave_crit
        pop     ix
        ret

_mdxnt_se_busy::
        ld      a, (_s_se_active)
        ret

; void mdxnt_ssg_blip(uint8_t ssg_ch, uint16_t tp, uint8_t vol, uint8_t ticks)
;   A = ssg_ch, DE = tp, [SP+2] = vol, [SP+3] = ticks (callee cleans 2 bytes!)
_mdxnt_ssg_blip::
        push    ix
        ld      ix, #0
        add     ix, sp                  ; 4(ix)=vol, 5(ix)=ticks
        cp      #3
        jr      nc, _blp_ret
        ld      c, a                    ; C = ssg_ch
        ld      a, 5(ix)
        or      a
        jr      nz, _blp_tok
        ld      5(ix), #1
_blp_tok:
        push    bc
        push    de
        call    _mdxnt_enter_crit
        pop     de
        pop     bc
        ld      a, c
        add     a, a
        add     a, a
        add     a, c                    ; *5
        ld      c, a
        ld      b, #0
        ld      hl, #_s_blip
        add     hl, bc
        ld      (hl), #1                ; pending
        inc     hl
        ld      a, 5(ix)
        ld      (hl), a                 ; ticks
        inc     hl
        ld      (hl), e                 ; tp lo
        inc     hl
        ld      (hl), d                 ; tp hi
        inc     hl
        ld      a, 4(ix)
        and     #0x0F
        ld      (hl), a                 ; vol
        call    _mdxnt_leave_crit
_blp_ret:
        pop     ix
        pop     hl
        pop     af                      ; callee cleanup 2 stack args
        jp      (hl)
