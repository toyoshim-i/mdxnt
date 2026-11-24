#ifndef MDXNT_OPN_CHIP_H
#define MDXNT_OPN_CHIP_H

#include <stdint.h>

// Thin YM2203 (OPN) chip abstraction so ymfm can be swapped for Nuked later.
class OpnChip {
public:
    virtual ~OpnChip() = default;
    virtual void reset() = 0;
    virtual void write(uint8_t reg, uint8_t val) = 0;
    // Generate one mono sample at the chip's native output rate, normalized roughly to [-1.0, 1.0].
    virtual float next_native_sample() = 0;
    virtual uint32_t native_sample_rate() const = 0;
};

OpnChip *create_ymfm_opn_chip(uint32_t master_clock);

#endif /* MDXNT_OPN_CHIP_H */
