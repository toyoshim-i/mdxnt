#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

extern "C" {
uint8_t *mdxnt_wasm_get_data_buf(void);
float *mdxnt_wasm_get_pcm_buf(void);
void mdxnt_wasm_init(uint32_t sample_rate);
uint32_t mdxnt_wasm_load(uint32_t len);
uint32_t mdxnt_wasm_render(uint32_t frames);
void mdxnt_wasm_set_speed(uint32_t speed_percent);
uint32_t mdxnt_wasm_seek_ticks(uint32_t target_ticks);
uint32_t mdxnt_wasm_get_tick(void);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <song.mdxt>\n", argv[0]);
        return 2;
    }
    std::ifstream ifs(argv[1], std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (data.empty()) {
        std::fprintf(stderr, "Failed to read %s\n", argv[1]);
        return 1;
    }

    mdxnt_wasm_init(48000);
    std::memcpy(mdxnt_wasm_get_data_buf(), data.data(), data.size());
    if (!mdxnt_wasm_load(static_cast<uint32_t>(data.size()))) {
        std::fprintf(stderr, "mdxnt_wasm_load failed\n");
        return 1;
    }

    double sum_sq = 0.0;
    float peak = 0.0f;
    uint32_t total = 0;
    for (int block = 0; block < 200; ++block) {
        uint32_t n = mdxnt_wasm_render(256);
        float *pcm = mdxnt_wasm_get_pcm_buf();
        for (uint32_t i = 0; i < n; ++i) {
            float s = pcm[i * 2];
            sum_sq += static_cast<double>(s) * static_cast<double>(s);
            if (std::fabs(s) > peak) peak = std::fabs(s);
        }
        total += n;
    }
    double rms = std::sqrt(sum_sq / static_cast<double>(total));
    std::printf("Rendered %u frames (ticks=%u): RMS=%.5f, peak=%.5f\n", total, mdxnt_wasm_get_tick(), rms, peak);
    if (rms < 0.001) {
        std::fprintf(stderr, "FAIL: audio output is silent!\n");
        return 1;
    }

    // Test fast-forward 4x
    uint32_t t_before = mdxnt_wasm_get_tick();
    mdxnt_wasm_set_speed(400);
    for (int block = 0; block < 50; ++block) {
        mdxnt_wasm_render(256);
    }
    uint32_t t_after = mdxnt_wasm_get_tick();
    std::printf("Fast-forward 4x ticks advanced: %u\n", t_after - t_before);
    if (t_after <= t_before) {
        std::fprintf(stderr, "FAIL: fast-forward did not advance ticks\n");
        return 1;
    }

    // Test seek
    uint32_t t_seek = mdxnt_wasm_seek_ticks(192);
    std::printf("Seeked to tick: %u\n", t_seek);
    if (t_seek != 192) {
        std::fprintf(stderr, "FAIL: seek returned %u\n", t_seek);
        return 1;
    }

    std::printf("HOST YMFM PLAYER TEST PASSED!\n");
    return 0;
}
