// Sound hardware: YM2612 (via ymfm), SN76489 PSG, and resampling to the host rate.
// The machine calls sound_advance() as emulated time passes; samples are made
// at the YM2612's native rate and resampled into sound_out.
#include "ymfm_opn.h"
#include <cmath>
#include <cstring>
#include <vector>

extern "C" {
#include "genesis.h"
}

namespace {

// The chips run from the console's clock, which a PAL console has slightly slower
// (sound_reset() picks them for the cartridge loaded).
constexpr uint32_t YM_DIVIDER = 144;        // clocks per output sample
double YM_RATE = 7670453.0 / YM_DIVIDER;    // NTSC 68000 clock
double PSG_CLOCK = 3579545.0 / 16.0;

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

    // As step(), but each channel's level is its average over the `ticks` clocks instead of
    // its value at the end: a square wave whose edges fall between samples comes out
    // band-limited enough not to alias into the audible range. The state ends up the same.
    int32_t step_average(double ticks) {
        double mix = 0;
        for (int ch = 0; ch < 3; ch++) {
            int p = period[ch] ? period[ch] : 1;
            double left = ticks, high = 0;
            while (counter[ch] <= left) {   // the time spent at each level before it flips
                if (counter[ch] > 0) { if (out[ch]) high += counter[ch]; left -= counter[ch]; }
                counter[ch] = p;
                out[ch] ^= 1;
            }
            if (out[ch]) high += left;
            counter[ch] -= left;
            if (period[ch] > 1) mix += (2 * high / ticks - 1) * volume[atten[ch]];
        }
        // The noise counter reloads with $10, $20, $40 or tone 2's period and flips its output
        // each time, and the shift register moves on every other flip: once per twice that.
        int np =(period[3] & 3) == 3 ? (period[2] ? period[2] : 1) * 2 : (0x20 << (period[3] & 3));
        double left = ticks, high = 0;
        while (counter[3] <= left) {
            if (counter[3] > 0) { if (out[3]) high += counter[3]; left -= counter[3]; }
            counter[3] = np;
            int fb = (period[3] & 4) ? ((lfsr & 1) ^ ((lfsr >> 3) & 1)) : (lfsr & 1);
            lfsr = uint16_t((lfsr >> 1) | (fb << 15));
            out[3] = lfsr & 1;
        }
        if (out[3]) high += left;
        counter[3] -= left;
        mix += (2 * high / ticks - 1) * volume[atten[3]];
        return int32_t(mix);
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
        int np = (period[3] & 3) == 3 ? (period[2] ? period[2] : 1) * 2 : (0x20 << (period[3] & 3));
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

// ---- Band-limited resampling (sound_quality >= 1) -------------------------------------
// Each output sample is a windowed-sinc sum over the 16 YM-rate samples around it, cut
// off a little below the output's Nyquist frequency, so nothing above it folds back.
// It is 8 YM samples (0.15 ms) behind the linear path.
constexpr int TAPS = 16, PHASES = 256;
constexpr float HEADROOM = 0.5f;            // the mix runs hot: room for its peaks until the limiter
float kernel[PHASES + 1][TAPS];
int kernel_rate;                            // output rate the kernel was made for
float hist_l[TAPS * 2], hist_r[TAPS * 2];   // the latest YM samples, twice over for easy reading
int hist_at;

void make_kernel() {
    double fc = 0.9 * (sound_out_rate / 2.0) / YM_RATE;    // cutoff, in cycles per YM sample
    if (fc > 0.45) fc = 0.45;
    for (int p = 0; p <= PHASES; p++) {
        double frac = double(p) / PHASES, sum = 0;
        for (int k = 0; k < TAPS; k++) {
            double x = k - (TAPS / 2 - 1) - frac;           // distance from the output point
            double s = x == 0 ? 2 * fc : std::sin(2 * M_PI * fc * x) / (M_PI * x);
            double w = 0.42 + 0.5 * std::cos(M_PI * x / (TAPS / 2)) + 0.08 * std::cos(2 * M_PI * x / (TAPS / 2));  // Blackman
            kernel[p][k] = float(s * (std::fabs(x) < TAPS / 2 ? w : 0));
            sum += kernel[p][k];
        }
        for (int k = 0; k < TAPS; k++) kernel[p][k] = float(kernel[p][k] / sum);
    }
    kernel_rate = sound_out_rate;
}

// ---- Output effects (sound_end_frame) --------------------------------------------------
// A DC blocker (clean and rich), and for rich a gentle stereo widening and a small room:
// four comb filters and two all-pass filters per side (after Freeverb), mixed in quietly.
struct Comb {
    std::vector<float> buf; size_t at = 0; float store = 0;
    float run(float in, float feedback, float damp) {
        float out = buf[at];
        store = out * (1 - damp) + store * damp;
        buf[at] = in + store * feedback;
        if (++at == buf.size()) at = 0;
        return out;
    }
};
struct Allpass {
    std::vector<float> buf; size_t at = 0;
    float run(float in) {
        float b = buf[at], out = b - in;
        buf[at] = in + b * 0.5f;
        if (++at == buf.size()) at = 0;
        return out;
    }
};
struct Room {
    Comb comb[2][4];
    Allpass ap[2][2];
    int rate = 0;
    void make(int r) {
        static const int comb_ms10[4] = { 253, 269, 290, 307 };    // tenths of a millisecond
        static const int ap_ms10[2] = { 50, 126 };
        for (int s = 0; s < 2; s++) {
            int spread = s ? 23 : 0;                                // left and right differ a little
            for (int k = 0; k < 4; k++) comb[s][k].buf.assign(size_t((comb_ms10[k] * r) / 10000 + spread), 0.f);
            for (int k = 0; k < 2; k++) ap[s][k].buf.assign(size_t((ap_ms10[k] * r) / 10000 + spread), 0.f);
        }
        rate = r;
    }
} room;
float dc_in[2], dc_out[2];

} // namespace

extern "C" {

int16_t sound_out[SOUND_OUT_MAX * 2];
int     sound_out_count;
int     sound_out_rate = 48000;
int     sound_quality;
bool    sound_pal;
bool    sound_fm_enabled = true, sound_psg_enabled = true;

void sound_reset(void) {
    YM_RATE = (sound_pal ? 7600489.0 : 7670453.0) / YM_DIVIDER;
    PSG_CLOCK = (sound_pal ? 3546893.0 : 3579545.0) / 16.0;
    kernel_rate = 0;                        // the resampler's filter is for the old rate
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
        int32_t p = sound_quality ? psg.step_average(PSG_CLOCK / YM_RATE) : psg.step(PSG_CLOCK / YM_RATE);
        int32_t l = (sound_fm_enabled ? o.data[0] * 6 : 0) + (sound_psg_enabled ? p : 0);
        int32_t r = (sound_fm_enabled ? o.data[1] * 6 : 0) + (sound_psg_enabled ? p : 0);
        if (sound_quality) {
            if (kernel_rate != sound_out_rate) make_kernel();
            hist_l[hist_at] = hist_l[hist_at + TAPS] = float(l);
            hist_r[hist_at] = hist_r[hist_at + TAPS] = float(r);
            hist_at = (hist_at + 1) % TAPS;   // hist_*[hist_at .. +TAPS) is now oldest to newest
            while (resample_pos < 1.0) {
                if (sound_out_count < SOUND_OUT_MAX) {
                    const float *k = kernel[int(resample_pos * PHASES)];
                    const float *hl = hist_l + hist_at, *hr = hist_r + hist_at;
                    float sl = 0, sr = 0;
                    for (int t = 0; t < TAPS; t++) { sl += hl[t] * k[t]; sr += hr[t] * k[t]; }
                    sl *= HEADROOM; sr *= HEADROOM;     // sound_end_frame() brings the level back
                    sound_out[sound_out_count * 2] = int16_t(sl < -32768 ? -32768 : sl > 32767 ? 32767 : sl);
                    sound_out[sound_out_count * 2 + 1] = int16_t(sr < -32768 ? -32768 : sr > 32767 ? 32767 : sr);
                    sound_out_count++;
                }
                resample_pos += out_step;
            }
            resample_pos -= 1.0;
            prev_l = l;
            prev_r = r;
            continue;
        }
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

void sound_end_frame(void) {
    if (!sound_quality) return;
    if (sound_quality >= 2 && room.rate != sound_out_rate) room.make(sound_out_rate);
    for (int i = 0; i < sound_out_count; i++) {
        float v[2];
        for (int c = 0; c < 2; c++) {
            float x = sound_out[i * 2 + c];
            dc_out[c] = x - dc_in[c] + 0.9985f * dc_out[c];      // DC blocker, corner about 11 Hz
            dc_in[c] = x;
            v[c] = dc_out[c];
        }
        if (sound_quality >= 2) {
            float mid = (v[0] + v[1]) * 0.5f, side = (v[0] - v[1]) * 0.5f * 1.3f;
            v[0] = mid + side; v[1] = mid - side;
            float in = (v[0] + v[1]) * 0.015f;
            for (int c = 0; c < 2; c++) {
                float wet = 0;
                for (int k = 0; k < 4; k++) wet += room.comb[c][k].run(in, 0.78f, 0.35f);
                for (int k = 0; k < 2; k++) wet = room.ap[c][k].run(wet);
                v[c] += wet;
            }
        }
        for (int c = 0; c < 2; c++) {
            // Back to full level, with a soft knee above 3/4 of full scale instead of hard clipping.
            float x = v[c] / HEADROOM, a = std::fabs(x), knee = 24576;
            if (a > knee) a = knee + (32767 - knee) * std::tanh((a - knee) / (32767 - knee));
            sound_out[i * 2 + c] = int16_t(x < 0 ? -a : a);
        }
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
