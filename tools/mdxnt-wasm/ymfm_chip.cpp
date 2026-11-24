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
        : m_clock(master_clock), m_intf(), m_chip(m_intf),
          m_ssg_dc_prev_x(0.0f), m_ssg_dc_prev_y(0.0f) {
        m_chip.set_fidelity(ymfm::OPN_FIDELITY_MIN);
        m_chip.reset();
    }

    void reset() override {
        m_chip.reset();
        m_ssg_dc_prev_x = 0.0f;
        m_ssg_dc_prev_y = 0.0f;
    }

    void write(uint8_t reg, uint8_t val) override {
        m_chip.write_address(reg);
        m_chip.write_data(val);
    }

    float next_native_sample() override {
        ymfm::ym2203::output_data out;
        m_chip.generate(&out, 1);
        // out.data[0] = signed FM (3ch mixed), out.data[1..3] = unipolar SSG ch A..C [0..16382]
        const float fm = static_cast<float>(out.data[0]) / 32768.0f;
        const float ssg_raw =
            static_cast<float>(out.data[1] + out.data[2] + out.data[3]) / (32768.0f * 2.5f);
        // 1-pole DC-blocking HPF (~13 Hz cutoff at 166.4 kHz) removes SSG unipolar DC offset.
        const float ssg = ssg_raw - m_ssg_dc_prev_x + 0.9995f * m_ssg_dc_prev_y;
        m_ssg_dc_prev_x = ssg_raw;
        m_ssg_dc_prev_y = ssg;

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
    float m_ssg_dc_prev_x;
    float m_ssg_dc_prev_y;
};

} // namespace

OpnChip *create_ymfm_opn_chip(uint32_t master_clock) {
    alignas(YmfmOpnChip) static unsigned char storage[sizeof(YmfmOpnChip)];
    static YmfmOpnChip *instance = nullptr;
    if (instance) {
        instance->reset();
        return instance;
    }
    instance = new (storage) YmfmOpnChip(master_clock);
    return instance;
}
