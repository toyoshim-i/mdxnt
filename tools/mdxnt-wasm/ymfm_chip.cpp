#include <new>
#include "opn_chip.h"
#include "ymfm_opn.h"

namespace {

class YmfmOpnInterface : public ymfm::ymfm_interface {
public:
    void ymfm_sync_mode_write(uint8_t data) override {
        ymfm::ymfm_interface::ymfm_sync_mode_write(data);
    }
    void ymfm_sync_check_interrupts() override {
        ymfm::ymfm_interface::ymfm_sync_check_interrupts();
    }
};

class YmfmOpnChip final : public OpnChip {
public:
    explicit YmfmOpnChip(uint32_t master_clock)
        : m_clock(master_clock), m_intf(), m_chip(m_intf) {
        m_chip.set_fidelity(ymfm::OPN_FIDELITY_MIN);
        m_chip.reset();
    }

    void reset() override {
        m_chip.reset();
    }

    void write(uint8_t reg, uint8_t val) override {
        m_chip.write_address(reg);
        m_chip.write_data(val);
    }

    float next_native_sample() override {
        ymfm::ym2203::output_data out;
        m_chip.generate(&out, 1);
        // out.data[0] = FM (3ch mixed), out.data[1..3] = SSG ch A..C
        // Mix FM and SSG at balanced PC-8801mkIISR levels.
        const float fm = static_cast<float>(out.data[0]) / 32768.0f;
        const float ssg = static_cast<float>(out.data[1] + out.data[2] + out.data[3]) / (32768.0f * 2.5f);
        float mixed = (fm * 0.85f + ssg * 0.65f);
        if (mixed > 1.0f) mixed = 1.0f;
        if (mixed < -1.0f) mixed = -1.0f;
        return mixed;
    }

    uint32_t native_sample_rate() const override {
        return m_chip.sample_rate(m_clock);
    }

private:
    uint32_t m_clock;
    YmfmOpnInterface m_intf;
    ymfm::ym2203 m_chip;
};

} // namespace

OpnChip *create_ymfm_opn_chip(uint32_t master_clock) {
    alignas(YmfmOpnChip) static unsigned char storage[sizeof(YmfmOpnChip)];
    static bool constructed = false;
    if (constructed) {
        reinterpret_cast<YmfmOpnChip *>(storage)->reset();
        return reinterpret_cast<YmfmOpnChip *>(storage);
    }
    constructed = true;
    return new (storage) YmfmOpnChip(master_clock);
}
