// Sound hardware: YM2612 (via ymfm), SN76489 PSG, and resampling to the host rate.
// The machine calls sound_advance() as emulated time passes; samples are made
// at the YM2612's native rate and resampled into sound_out.
#include "ymfm_opn.h"
#include <cstring>
#include <vector>

extern "C" {
#include "genesis.h"
}

namespace {

constexpr uint32_t YM_CLOCK = 7670453;      // NTSC 68000 clock
constexpr uint32_t YM_DIVIDER = 144;        // clocks per output sample
constexpr double   YM_RATE = double(YM_CLOCK) / YM_DIVIDER;
constexpr double   PSG_CLOCK = 3579545.0 / 16.0;

class Ym : public ymfm::ymfm_interface {
public:
    Ym() : chip(*this) {}
    ymfm::ym2612 chip;
    int32_t timer_left[2] = { -1, -1 };     // clocks until each timer fires, -1 = stopped

    void ymfm_set_timer(uint32_t tnum, int32_t clocks) override { timer_left[tnum] = clocks; }

    void tick_timers(int32_t clocks) {
        for (uint32_t t = 0; t < 2; t++) {
            if (timer_left[t] < 0) continue;
            timer_left[t] -= clocks;
            if (timer_left[t] <= 0) {
                timer_left[t] = -1;
                m_engine->engine_timer_expired(t); // may re-arm via ymfm_set_timer
            }
        }
    }
};

Ym ym;

// SN76489 as wired in the Genesis: 16-bit noise LFSR tapped at bits 0 and 3.
struct Psg {
    uint16_t period[4] = { 0, 0, 0, 0 };
    uint8_t  atten[4] = { 15, 15, 15, 15 };
    double   counter[4] = { 0, 0, 0, 0 };
    int      out[4] = { 1, 1, 1, 1 };
    uint16_t lfsr = 0x8000;
    int      latch = 0;
    int16_t  volume[16];

    Psg() {
        double v = 4000.0;                  // per channel; four channels stay below FM peaks
        for (int i = 0; i < 15; i++) { volume[i] = int16_t(v); v *= 0.794328; } // -2 dB steps
        volume[15] = 0;
    }

    void write(uint8_t d) {
        if (d & 0x80) {
            latch = (d >> 4) & 7;
            int ch = latch >> 1;
            if (latch & 1) atten[ch] = d & 0x0F;
            else if (ch == 3) { period[3] = d & 7; lfsr = 0x8000; }
            else period[ch] = uint16_t((period[ch] & 0x3F0) | (d & 0x0F));
        } else {
            int ch = latch >> 1;
            if (latch & 1) atten[ch] = d & 0x0F;
            else if (ch == 3) { period[3] = d & 7; lfsr = 0x8000; }
            else period[ch] = uint16_t((period[ch] & 0x00F) | ((d & 0x3F) << 4));
        }
    }

    // Advances by `ticks` PSG clocks and returns the mixed level.
    int32_t step(double ticks) {
        int32_t mix = 0;
        for (int ch = 0; ch < 3; ch++) {
            int p = period[ch] ? period[ch] : 1;
            counter[ch] -= ticks;
            while (counter[ch] <= 0) { counter[ch] += p; out[ch] ^= 1; }
            if (period[ch] > 1) mix += out[ch] ? volume[atten[ch]] : -volume[atten[ch]];
        }
        int np = (period[3] & 3) == 3 ? (period[2] ? period[2] : 1) * 2 : (0x10 << (period[3] & 3));
        counter[3] -= ticks;
        while (counter[3] <= 0) {
            counter[3] += np;
            int fb = (period[3] & 4) ? ((lfsr & 1) ^ ((lfsr >> 3) & 1)) : (lfsr & 1);
            lfsr = uint16_t((lfsr >> 1) | (fb << 15));
            out[3] = lfsr & 1;
        }
        mix += out[3] ? volume[atten[3]] : -volume[atten[3]];
        return mix;
    }
};

Psg psg;
double ym_pending = 0;                      // fractional YM samples owed
double resample_pos = 0;                    // position between the last two YM samples
int32_t prev_l = 0, prev_r = 0;

} // namespace

extern "C" {

int16_t sound_out[SOUND_OUT_MAX * 2];
int     sound_out_count;
int     sound_out_rate = 48000;
bool    sound_fm_enabled = true, sound_psg_enabled = true;

void sound_reset(void) {
    ym.chip.reset();
    ym.timer_left[0] = ym.timer_left[1] = -1;
    psg = Psg();
    ym_pending = resample_pos = 0;
    sound_out_count = 0;
}

void    ym_write(int port, uint8_t v) { ym.chip.write(uint32_t(port & 3), v); }
uint8_t ym_read(void) { return ym.chip.read_status(); }
void    psg_write(uint8_t v) { psg.write(v); }

// Called with the number of 68000 clocks that just elapsed.
void sound_advance(int m68k_cycles) {
    ym_pending += m68k_cycles / double(YM_DIVIDER);
    const double out_step = YM_RATE / sound_out_rate;   // YM samples per output sample
    while (ym_pending >= 1.0) {
        ym_pending -= 1.0;
        ymfm::ym2612::output_data o;
        ym.chip.generate(&o);
        ym.tick_timers(YM_DIVIDER);
        int32_t p = psg.step(PSG_CLOCK / YM_RATE);
        int32_t l = (sound_fm_enabled ? o.data[0] * 6 : 0) + (sound_psg_enabled ? p : 0);
        int32_t r = (sound_fm_enabled ? o.data[1] * 6 : 0) + (sound_psg_enabled ? p : 0);
        // Linear interpolation from YM rate to the output rate.
        while (resample_pos < 1.0) {
            if (sound_out_count < SOUND_OUT_MAX) {
                int32_t sl = int32_t(prev_l + (l - prev_l) * resample_pos);
                int32_t sr = int32_t(prev_r + (r - prev_r) * resample_pos);
                sound_out[sound_out_count * 2] = int16_t(sl < -32768 ? -32768 : sl > 32767 ? 32767 : sl);
                sound_out[sound_out_count * 2 + 1] = int16_t(sr < -32768 ? -32768 : sr > 32767 ? 32767 : sr);
                sound_out_count++;
            }
            resample_pos += out_step;
        }
        resample_pos -= 1.0;
        prev_l = l;
        prev_r = r;
    }
}

void sound_state(StateIO *s) {
    // ymfm serializes itself into a byte vector; store its length first.
    std::vector<uint8_t> chip;
    uint32_t n = 0;
    if (s->saving) {
        ymfm::ymfm_saved_state st(chip, true);
        ym.chip.save_restore(st);
        n = uint32_t(chip.size());
    }
    state_io(s, &n, sizeof n);
    if (!s->saving) {
        if (n > 4096) { s->ok = false; return; }
        chip.resize(n);
    }
    state_io(s, chip.data(), n);
    if (!s->saving && s->ok) {
        ymfm::ymfm_saved_state st(chip, false);
        ym.chip.save_restore(st);
    }
    state_io(s, ym.timer_left, sizeof ym.timer_left);
    state_io(s, &psg, sizeof psg);
    state_io(s, &ym_pending, sizeof ym_pending);
    state_io(s, &resample_pos, sizeof resample_pos);
    state_io(s, &prev_l, sizeof prev_l);
    state_io(s, &prev_r, sizeof prev_r);
}

} // extern "C"
