#include "ay8910.h"
#include <string.h>

/* The DAC is logarithmic: each step is about 1.5 dB, near enough to a factor of sqrt(2)
 * every two steps. These are MAME's measured levels for the 8910, scaled to 0..8191. */
static const uint16_t vol_table[16] = {
        0,   40,   59,   87,  128,  184,  269,  388,
      555,  813, 1152, 1671, 2360, 3406, 4870, 6982
};

static void update_periods(ay8910_t *ay);

void ay_init(ay8910_t *ay, uint32_t clock, ay_port_read_fn port_read, void *ctx)
{
    memset(ay, 0, sizeof(*ay));
    ay->clock = clock;
    ay->port_read = port_read;
    ay->port_ctx = ctx;
    ay_reset(ay);
}

void ay_reset(ay8910_t *ay)
{
    memset(ay->reg, 0, sizeof(ay->reg));
    ay->reg[7] = 0xff;                       /* everything disabled */
    memset(ay->tone_count, 0, sizeof(ay->tone_count));
    memset(ay->tone_out, 0, sizeof(ay->tone_out));
    ay->noise_count = 0; ay->noise_out = 0; ay->noise_rng = 1;
    ay->env_count = 0; ay->env_step = 0; ay->env_out = 0; ay->env_hold = 0; ay->env_attack = 0;
    ay->latch = 0; ay->acc = 0;
    update_periods(ay);
}

void ay_address_w(ay8910_t *ay, uint8_t reg) { ay->latch = reg & 0x0f; }

void ay_data_w(ay8910_t *ay, uint8_t data)
{
    int r = ay->latch;
    ay->reg[r] = data;
    if (r <= 6 || r == 11 || r == 12) update_periods(ay);
    if (r == 13) {                           /* writing the shape restarts the envelope */
        ay->env_step = 0; ay->env_hold = 0; ay->env_count = 0;
        ay->env_attack = (uint8_t)((data >> 2) & 1);
        ay->env_out = (uint8_t)(ay->env_attack ? 0 : 15);
    }
}

uint8_t ay_data_r(ay8910_t *ay)
{
    int r = ay->latch;
    if (r == 14) return ay->port_read ? ay->port_read(ay->port_ctx, 0) : 0xff;
    if (r == 15) return ay->port_read ? ay->port_read(ay->port_ctx, 1) : 0xff;
    return ay->reg[r];
}

/* the periods are recomputed on write rather than on every step, which is a hot loop */
static void update_periods(ay8910_t *ay)
{
    for (int ch = 0; ch < 3; ch++) {
        uint16_t p = (uint16_t)(ay->reg[ch * 2] | ((ay->reg[ch * 2 + 1] & 0x0f) << 8));
        ay->period[ch] = p ? p : 1;          /* a period of 0 behaves as 1 */
    }
    uint16_t np = (uint16_t)(ay->reg[6] & 0x1f);
    ay->noise_period = (uint16_t)((np ? np : 1) * 2);
    /* The datasheet gives the envelope a full cycle every 256*EP clocks, so one of its sixteen
     * steps is 16*EP clocks - two ticks of this clock/8 timebase, not one. */
    uint16_t ep = (uint16_t)(ay->reg[11] | (ay->reg[12] << 8));
    ay->env_period = (uint32_t)(ep ? ep : 1) * 2;
}

/* one step of the noise generator, on the clock/8 timebase */
static inline void ay_step_noise(ay8910_t *ay)
{
    if (++ay->noise_count >= ay->noise_period) {   /* the noise generator runs at half the tone rate */
        ay->noise_count = 0;
        /* 17-bit LFSR, taps at bits 0 and 3 */
        uint32_t bit = ((ay->noise_rng >> 0) ^ (ay->noise_rng >> 3)) & 1;
        ay->noise_rng = (ay->noise_rng >> 1) | (bit << 16);
        ay->noise_out = (uint8_t)(ay->noise_rng & 1);
    }
}

/* and one step of the envelope */
static inline void ay_step_env(ay8910_t *ay)
{
    if (++ay->env_count >= ay->env_period) {
        ay->env_count = 0;
        if (!ay->env_hold) {
            /*
             * The envelope is a sixteen-step ramp, and register 13's four bits say what happens
             * when it reaches the end: CONT decides whether there is a second cycle at all,
             * HOLD freezes it there, and ALT flips the direction each time round. Running a
             * thirty-two step counter and taking the low nibble - which is what this used to do
             * - ramps twice per cycle and never holds, which is audibly wrong on anything that
             * leans on the envelope for its shape.
             */
            uint8_t shape = (uint8_t)(ay->reg[13] & 0x0f);
            int cont = (shape >> 3) & 1, att = (shape >> 2) & 1;
            int alt = (shape >> 1) & 1, hold = shape & 1;
            if (++ay->env_step > 15) {
                if (!cont) {                       /* one ramp, then silence */
                    ay->env_hold = 1; ay->env_out = 0; ay->env_step = 15;
                } else if (hold) {                 /* one ramp, then frozen */
                    ay->env_hold = 1;
                    ay->env_out = (uint8_t)((att ^ alt) ? 15 : 0);
                    ay->env_step = 15;
                } else {
                    if (alt) ay->env_attack ^= 1;  /* turn round and go back */
                    ay->env_step = 0;
                }
            }
            if (!ay->env_hold)
                ay->env_out = (uint8_t)(ay->env_attack ? ay->env_step : 15 - ay->env_step);
        }
    }
}

void ay_render(ay8910_t *ay, int16_t *buf, int samples, int rate)
{
    /* the tone counters are clocked at clock/8 */
    uint32_t step_per_sample = (uint32_t)(((uint64_t)(ay->clock / 8) << 16) / (uint32_t)rate);

    /*
     * A board with five of these on it spends most of its time with most of them idle, and
     * stepping something that cannot make a sound is pure waste. A channel is silent when its
     * volume register is zero and it is not following the envelope; if all three are, so is the
     * whole chip, and all that has to happen is that its sample clock keeps moving.
     */
    uint8_t r7 = ay->reg[7];
    int active[3], any = 0, need_noise = 0, need_env = 0;
    for (int ch = 0; ch < 3; ch++) {
        uint8_t v = ay->reg[8 + ch];
        active[ch] = (v & 0x1f) != 0;
        if (!active[ch]) continue;
        any = 1;
        if (v & 0x10) need_env = 1;
        if (!((r7 >> (ch + 3)) & 1)) need_noise = 1;   /* this channel has the noise mixed in */
    }
    if (!any) {
        ay->acc += step_per_sample * (uint32_t)samples;
        ay->acc &= 0xffff;
        return;
    }

    for (int i = 0; i < samples; i++) {
        ay->acc += step_per_sample;
        uint32_t steps = ay->acc >> 16;
        ay->acc &= 0xffff;
        if (steps > 512) steps = 512;         /* never let a stall turn into a freeze */

        /*
         * The chip toggles its square waves far faster than we sample it - a dozen times
         * between one output sample and the next - so reading the level once per sample turns
         * every tone above a couple of kHz into whatever aliased mess happens to line up with
         * the sample clock. Counting how many of those steps the channel spent high and taking
         * the average is the cheap way to band-limit it.
         */
        uint32_t hi[3] = { 0, 0, 0 };
        uint32_t n = steps ? steps : 1;
#ifdef AY_SLOW_ONLY
        if (1) {
#else
        if (need_noise) {
#endif
            /* the slow path: the level is the tone ANDed with the noise, step by step */
            for (uint32_t s = 0; s < steps; s++) {
                for (int ch = 0; ch < 3; ch++) {
                    if (++ay->tone_count[ch] >= ay->period[ch]) {
                        ay->tone_count[ch] = 0;
                        ay->tone_out[ch] ^= 1;
                    }
                }
                if (need_noise) ay_step_noise(ay);
                if (need_env) ay_step_env(ay);
                for (int ch = 0; ch < 3; ch++) {
                    if (!active[ch]) continue;
                    uint8_t tone_dis  = (uint8_t)((r7 >> ch) & 1);
                    uint8_t noise_dis = (uint8_t)((r7 >> (ch + 3)) & 1);
                    hi[ch] += (uint32_t)((tone_dis | ay->tone_out[ch]) & (noise_dis | ay->noise_out));
                }
            }
        } else {
            /*
             * With no noise in the mix a channel is a plain square wave, and how many of the
             * next `steps` it spends high can be worked out a run at a time instead of a step
             * at a time. That turns a dozen iterations per channel per sample into one or two,
             * which is what makes five of these chips fit in the frame at all.
             */
            for (int ch = 0; ch < 3; ch++) {
                uint32_t rem = steps, c = ay->tone_count[ch], p = ay->period[ch];
                uint8_t o = ay->tone_out[ch];
                uint8_t tone_dis = (uint8_t)((r7 >> ch) & 1);
                while (rem) {
                    /*
                     * The counter is incremented first and the output flips when it reaches the
                     * period, so the step that causes the toggle already carries the new level -
                     * the run at the old one is one step shorter than the distance to it.
                     */
                    uint32_t to_toggle = (p > c) ? (p - c) : 1;
                    if (to_toggle > rem) {
                        if (active[ch] && (tone_dis | o)) hi[ch] += rem;
                        c += rem;
                        rem = 0;
                    } else {
                        if (to_toggle > 1 && active[ch] && (tone_dis | o)) hi[ch] += to_toggle - 1;
                        c = 0; o ^= 1;
                        if (active[ch] && (tone_dis | o)) hi[ch] += 1;
                        rem -= to_toggle;
                    }
                }
                ay->tone_count[ch] = (uint16_t)c;
                ay->tone_out[ch] = o;
            }
            if (need_env) for (uint32_t s = 0; s < steps; s++) ay_step_env(ay);
        }
        if (!steps) {
            for (int ch = 0; ch < 3; ch++) {
                if (!active[ch]) continue;
                uint8_t tone_dis  = (uint8_t)((r7 >> ch) & 1);
                uint8_t noise_dis = (uint8_t)((r7 >> (ch + 3)) & 1);
                hi[ch] = (uint32_t)((tone_dis | ay->tone_out[ch]) & (noise_dis | ay->noise_out));
            }
        }

        int32_t out = 0;
        for (int ch = 0; ch < 3; ch++) {
            if (!hi[ch]) continue;
            uint8_t v = ay->reg[8 + ch];
            out += (int32_t)((uint32_t)vol_table[(v & 0x10) ? ay->env_out : (v & 0x0f)] * hi[ch] / n);
        }
        /* the output only ever swings upward from zero, so it carries a fat DC term that the
         * cabinet's coupling capacitor removed; a one-pole high pass does the same job here */
        ay->dc += (out - ay->dc) >> 8;
        int32_t mixed = buf[i] + (out - ay->dc);
        buf[i] = (int16_t)(mixed > 32767 ? 32767 : (mixed < -32768 ? -32768 : mixed));
        r7 = ay->reg[7];
    }
}
