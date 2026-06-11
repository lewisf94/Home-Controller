/*
 * UI sound effects -- synthesised, played through the onboard ES8311 speaker.
 * See audio.h. Kept deliberately small: a scratch PSRAM buffer is filled with a
 * few tone notes (with attack/decay envelopes so there are no clicks) and
 * written to the codec on a dedicated task.
 *
 * Sound is organised as a table of named "sound sets" (k_sets) -- each set is a
 * waveform plus a note sequence per SFX. The user picks one in Settings, or
 * leaves it on AUTO and the active visual MODE chooses (modern/chiptune/ambient).
 * User volume (0..100) is applied as a perceptual square-law gain so the quiet
 * end of the slider has fine control.
 */
#include "audio.h"

#include "bsp/esp32_p4_wifi6_touch_lcd_4_3.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <math.h>
#include <string.h>

static const char *TAG = "audio";

#define AUDIO_SR          22050          /* sample rate (Hz), matches BSP default  */
#define AUDIO_MAX_SAMP    (AUDIO_SR)     /* longest SFX = 1.0 s of 16-bit mono      */
#define AUDIO_LEAD_MS     4              /* leading silence so the amp settles      */
#define AUDIO_CODEC_VOL   90             /* fixed codec out level; user vol is gain */
#define AUDIO_VOL_DEFAULT 70             /* default user volume (0..100)            */

typedef enum { WAVE_SINE = 0, WAVE_SQUARE, WAVE_TRI } waveform_t;

/* One tone segment. */
typedef struct { float freq; uint16_t ms; float amp; } note_t;
/* One SFX within a set: a short note sequence. */
typedef struct { const note_t *n; uint8_t count; } sfx_def_t;
/* A complete sound design: a waveform + a note sequence per SFX. */
typedef struct { const char *name; waveform_t wave; sfx_def_t sfx[AUDIO_SFX_COUNT]; } sound_set_t;

/* ---- Note tables (one per set per SFX; order = TICK, SELECT, BACK, CONNECT) -- */
static const note_t SINE_TICK[] = { { 1200, 12, 0.45f } };
static const note_t SINE_SEL [] = { { 440, 70, 0.50f }, { 660, 90, 0.50f } };
static const note_t SINE_BACK[] = { { 360, 60, 0.40f } };
static const note_t SINE_CON [] = { { 392, 90, 0.50f }, { 494, 90, 0.50f }, { 587, 150, 0.50f } };

static const note_t CHIP_TICK[] = { { 880, 10, 0.30f } };
static const note_t CHIP_SEL [] = { { 523, 55, 0.30f }, { 784, 80, 0.30f } };
static const note_t CHIP_BACK[] = { { 392, 60, 0.28f } };
static const note_t CHIP_CON [] = { { 523, 70, 0.30f }, { 659, 70, 0.30f }, { 784, 70, 0.30f }, { 1047, 150, 0.30f } };

static const note_t AMB_TICK[]  = { { 520, 28, 0.32f } };
static const note_t AMB_SEL []  = { { 330, 120, 0.42f }, { 440, 170, 0.42f } };
static const note_t AMB_BACK[]  = { { 294, 110, 0.36f } };
static const note_t AMB_CON []  = { { 262, 150, 0.42f }, { 330, 150, 0.42f }, { 392, 260, 0.42f } };

static const note_t MAR_TICK[]  = { { 1046, 14, 0.42f } };
static const note_t MAR_SEL []  = { { 523, 60, 0.50f }, { 784, 80, 0.50f } };
static const note_t MAR_BACK[]  = { { 392, 60, 0.42f } };
static const note_t MAR_CON []  = { { 523, 80, 0.50f }, { 659, 80, 0.50f }, { 784, 140, 0.50f } };

static const note_t ARC_TICK[]  = { { 1320, 10, 0.26f } };
static const note_t ARC_SEL []  = { { 659, 45, 0.28f }, { 988, 45, 0.28f }, { 1319, 70, 0.28f } };
static const note_t ARC_BACK[]  = { { 494, 50, 0.26f } };
static const note_t ARC_CON []  = { { 784, 60, 0.28f }, { 988, 60, 0.28f }, { 1319, 60, 0.28f }, { 1568, 150, 0.28f } };

static const note_t BEL_TICK[]  = { { 1568, 18, 0.35f } };
static const note_t BEL_SEL []  = { { 784, 90, 0.40f }, { 1175, 150, 0.40f } };
static const note_t BEL_BACK[]  = { { 659, 90, 0.35f } };
static const note_t BEL_CON []  = { { 784, 120, 0.40f }, { 1047, 120, 0.40f }, { 1319, 260, 0.40f } };

/* TELEX: dry typewriter mechanics for the PAPER theme. Square-wave notes so
 * short (5-12 ms) that the attack/decay envelope is most of the sound -- they
 * read as key strikes, not tones. CONNECT is three rapid strikes and the
 * carriage bell. */
static const note_t TLX_TICK[]  = { { 1700, 6, 0.50f } };
static const note_t TLX_SEL []  = { { 1250, 8, 0.50f }, { 740, 16, 0.45f } };
static const note_t TLX_BACK[]  = { { 520, 12, 0.45f } };
static const note_t TLX_CON []  = { { 980, 7, 0.45f }, { 980, 7, 0.45f }, { 980, 7, 0.45f }, { 1320, 70, 0.40f } };

#define SFX4(t, s, b, c) { { t, sizeof(t)/sizeof((t)[0]) }, { s, sizeof(s)/sizeof((s)[0]) }, \
                           { b, sizeof(b)/sizeof((b)[0]) }, { c, sizeof(c)/sizeof((c)[0]) } }

/* The selectable sound designs. Index here maps to the UI list AFTER the AUTO
 * entry (UI index 0 = AUTO, UI index i+1 = k_sets[i]). */
static const sound_set_t k_sets[] = {
    { "SINE",    WAVE_SINE,   SFX4(SINE_TICK, SINE_SEL, SINE_BACK, SINE_CON) },
    { "CHIP",    WAVE_SQUARE, SFX4(CHIP_TICK, CHIP_SEL, CHIP_BACK, CHIP_CON) },
    { "AMBIENT", WAVE_SINE,   SFX4(AMB_TICK,  AMB_SEL,  AMB_BACK,  AMB_CON)  },
    { "MARIMBA", WAVE_TRI,    SFX4(MAR_TICK,  MAR_SEL,  MAR_BACK,  MAR_CON)  },
    { "ARCADE",  WAVE_SQUARE, SFX4(ARC_TICK,  ARC_SEL,  ARC_BACK,  ARC_CON)  },
    { "BELL",    WAVE_SINE,   SFX4(BEL_TICK,  BEL_SEL,  BEL_BACK,  BEL_CON)  },
    { "TELEX",   WAVE_SQUARE, SFX4(TLX_TICK,  TLX_SEL,  TLX_BACK,  TLX_CON)  },
};
#define SET_COUNT ((int)(sizeof k_sets / sizeof k_sets[0]))

/* AUTO mapping: MODE-derived theme -> index into k_sets. */
static const int k_theme_set[AUDIO_THEME_COUNT] = {
    [AUDIO_THEME_MODERN]  = 0,   /* SINE    */
    [AUDIO_THEME_PIXEL]   = 1,   /* CHIP    */
    [AUDIO_THEME_AMBIENT] = 2,   /* AMBIENT */
    [AUDIO_THEME_TELEX]   = 6,   /* TELEX   */
};

static esp_codec_dev_handle_t s_spk      = NULL;
static QueueHandle_t          s_queue    = NULL;
static int16_t               *s_buf      = NULL;   /* synth scratch (PSRAM)        */
static volatile bool          s_enabled  = true;
static volatile int           s_volume   = AUDIO_VOL_DEFAULT;   /* 0..100          */
static volatile int           s_user_set = -1;     /* -1 = AUTO, else index k_sets */
static volatile int           s_theme_set = 0;     /* AUTO target (from MODE)      */

static inline float osc(waveform_t w, float phase /* [0,1) */)
{
    switch (w) {
    case WAVE_SQUARE: return (phase < 0.5f) ? 1.0f : -1.0f;
    case WAVE_TRI:    return 4.0f * fabsf(phase - 0.5f) - 1.0f;
    default:          return sinf(2.0f * (float)M_PI * phase);
    }
}

/* Fill s_buf with leading silence then the notes (waveform + 3 ms attack /
 * linear decay-to-zero per note), scaled by a perceptual (square-law) volume
 * gain. Returns the sample count written. */
static size_t synth(const note_t *notes, int count, waveform_t wave)
{
    float g    = (float)s_volume / 100.0f;
    float gain = g * g;                           /* square-law audio taper */
    size_t idx = 0;
    for (size_t i = 0; i < (size_t)(AUDIO_SR * AUDIO_LEAD_MS / 1000) && idx < AUDIO_MAX_SAMP; i++)
        s_buf[idx++] = 0;

    for (int k = 0; k < count; k++) {
        int ns  = AUDIO_SR * notes[k].ms / 1000;
        float att = 0.003f * AUDIO_SR;            /* 3 ms attack */
        for (int i = 0; i < ns && idx < AUDIO_MAX_SAMP; i++, idx++) {
            float env = (i < att) ? (i / att)
                                  : (1.0f - (float)(i - att) / (float)(ns - att));
            if (env < 0.0f) env = 0.0f;
            float phase = fmodf(notes[k].freq * (float)i / AUDIO_SR, 1.0f);
            float s = osc(wave, phase);
            s_buf[idx] = (int16_t)(s * env * notes[k].amp * gain * 32767.0f);
        }
    }
    return idx;
}

static size_t synth_sfx(audio_sfx_t sfx)
{
    int set = (s_user_set >= 0) ? s_user_set : s_theme_set;
    if (set < 0 || set >= SET_COUNT) set = 0;
    const sfx_def_t *d = &k_sets[set].sfx[sfx];
    if (!d->n || d->count == 0) return 0;
    return synth(d->n, d->count, k_sets[set].wave);
}

static void audio_task(void *arg)
{
    (void)arg;
    audio_sfx_t sfx;
    for (;;) {
        if (xQueueReceive(s_queue, &sfx, portMAX_DELAY) != pdTRUE) continue;
        if (!s_spk) continue;
        size_t ns = synth_sfx(sfx);
        if (ns == 0) continue;
        esp_codec_dev_write(s_spk, s_buf, (int)(ns * sizeof(int16_t)));
    }
}

void audio_init(void)
{
    s_buf = heap_caps_malloc(AUDIO_MAX_SAMP * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_buf) { ESP_LOGE(TAG, "scratch alloc failed"); return; }

    if (bsp_audio_init(NULL) != ESP_OK) { ESP_LOGE(TAG, "I2S init failed"); return; }
    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk) { ESP_LOGE(TAG, "speaker codec init failed"); return; }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 1,
        .sample_rate     = AUDIO_SR,
    };
    if (esp_codec_dev_open(s_spk, &fs) != ESP_OK) {
        ESP_LOGE(TAG, "codec open failed");
        s_spk = NULL;
        return;
    }
    esp_codec_dev_set_out_vol(s_spk, AUDIO_CODEC_VOL);

    s_queue = xQueueCreate(6, sizeof(audio_sfx_t));
    if (!s_queue) { ESP_LOGE(TAG, "queue alloc failed"); return; }
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 4, NULL, tskNO_AFFINITY);
    ESP_LOGI(TAG, "audio ready (ES8311 speaker, %d Hz, vol %d, %d sets)",
             AUDIO_SR, s_volume, SET_COUNT);
}

void audio_play(audio_sfx_t sfx)
{
    if (!s_enabled || !s_queue || sfx >= AUDIO_SFX_COUNT) return;
    xQueueSend(s_queue, &sfx, 0);   /* non-blocking; drop if full */
}

void audio_set_enabled(bool enabled) { s_enabled = enabled; }
bool audio_is_enabled(void)          { return s_enabled; }

void audio_set_volume(int vol)
{
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    s_volume = vol;
}
int audio_get_volume(void) { return s_volume; }

void audio_set_theme(audio_theme_t theme)
{
    if (theme < AUDIO_THEME_COUNT) s_theme_set = k_theme_set[theme];
}

void audio_set_set(int set)
{
    s_user_set = (set >= 0 && set < SET_COUNT) ? set : -1;
}
int audio_get_set(void) { return s_user_set; }
int audio_set_count(void) { return SET_COUNT; }

const char *audio_set_name(int i)
{
    return (i >= 0 && i < SET_COUNT) ? k_sets[i].name : "";
}
