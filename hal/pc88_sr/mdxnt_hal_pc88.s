; =============================================================================
; PC-8801mkIISR YM2203 OPN Hardware Layer for MDX-OPN Driver (SDCC --sdcccall 1)
; Hooks IM 2 INT 4 (SOUND / OPN Timer-B) via crt0.s _g_sound_isr_ptr.
; =============================================================================
        .module mdxnt_hal
        .globl  _g_irq_level
        .globl  _g_p32_base
        .globl  _g_sound_isr_ptr
        .globl  _mdxnt_tick_isr

        .area   _CODE

; -----------------------------------------------------------------------------
; void mdxnt_opn_write(uint8_t reg, uint8_t val)
;   A = reg, L = val  (--sdcccall 1)
; -----------------------------------------------------------------------------
_mdxnt_opn_write::
        ld      d, a            ; D = reg
        ld      e, l            ; E = val
        ld      b, #16
_mo_w1:
        in      a, (0x44)
        rlca
        jr      nc, _mo_a1
        djnz    _mo_w1
_mo_a1:
        ld      a, d
        out     (0x44), a
        ld      b, #16
_mo_w2:
        in      a, (0x44)
        rlca
        jr      nc, _mo_a2
        djnz    _mo_w2
_mo_a2:
        ld      a, e
        out     (0x45), a
        ret

; -----------------------------------------------------------------------------
; void mdxnt_hw_ack_timer_b(void)
;   Clears Timer-B overflow flag and keeps Timer-B running + IRQ armed.
; -----------------------------------------------------------------------------
_mdxnt_hw_ack_timer_b::
        ld      a, #0x27
        ld      l, #0x3A        ; Reset Timer-B flag (bit5) + Enable (bit3) + Load (bit1)
        jr      _mdxnt_opn_write

; -----------------------------------------------------------------------------
; void mdxnt_hw_enable_irq(uint8_t init_tb)
;   A = init_tb (Timer-B period register 0x26)
; -----------------------------------------------------------------------------
_mdxnt_hw_enable_irq::
        di
        push    af
        ; Register ISR callback
        ld      hl, #_mdxnt_tick_isr
        ld      (_g_sound_isr_ptr), hl
        ; Unmask onboard OPN IRQ (Port 0x32 bit 7 = 0), preserving bits 0..6
        ld      a, (_g_p32_base)
        and     #0x7F
        ld      (_g_p32_base), a
        out     (0x32), a
        ; Set Timer-B period
        pop     af
        ld      l, a
        ld      a, #0x26
        call    _mdxnt_opn_write
        ; Start Timer-B with IRQ enabled (0x3A)
        ld      a, #0x27
        ld      l, #0x3A
        call    _mdxnt_opn_write
        ; Raise PC-88 IRQ priority level to 5 (enables INT 1 VSYNC + INT 4 SOUND)
        ld      a, #0x05
        ld      (_g_irq_level), a
        out     (0xE4), a
        ei
        ret

; -----------------------------------------------------------------------------
; void mdxnt_hw_disable_irq(void)
; -----------------------------------------------------------------------------
_mdxnt_hw_disable_irq::
        di
        ; Stop Timer-B
        ld      a, #0x27
        ld      l, #0x30
        call    _mdxnt_opn_write
        ; Mask OPN IRQ (Port 0x32 bit 7 = 1) preserving bits 0..6 & restore VSYNC-only level
        ld      hl, #0x0000
        ld      (_g_sound_isr_ptr), hl
        ld      a, (_g_p32_base)
        or      #0x80
        ld      (_g_p32_base), a
        out     (0x32), a
        ld      a, #0x02
        ld      (_g_irq_level), a
        out     (0xE4), a
        ei
        ret

_mdxnt_enter_crit::
        di
        ret

_mdxnt_leave_crit::
        ei
        ret
