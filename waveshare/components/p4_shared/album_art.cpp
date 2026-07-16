/*
 * JPEGDEC wrapper -- decodes a JPEG into an RGB565 buffer.
 *
 * Uses the C-style JPEGDEC API (JPEG_openRAM/JPEG_decode/...) because the
 * bitbank2/jpegdec IDF component only ships JPEGDEC.c, not the .cpp that
 * defines the C++ class methods.
 *
 * JPEGDEC streams pixels to us in MCU-sized chunks via a draw callback;
 * we just memcpy each chunk into the right slot of the destination buffer.
 * The callback context carries the buffer pointer and the post-scale
 * destination dimensions.
 *
 * Scale selection rule: pick the largest scale (1, /2, /4, /8) that keeps
 * the decoded image inside out_max_pixels and reasonably close to 320x320
 * (the now-playing art size on the 800x480 panel). A 640x640 Spotify source
 * decodes /2 -> 320x320; smaller sources get full size.
 *
 * JPEGIMAGE is a large struct (internal MCU/Huffman buffers), so we
 * heap-allocate it rather than putting it on the FreeRTOS task stack.
 */

#include "album_art.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "JPEGDEC.h"

static const char *TAG = "album_art";

// JPEGDEC.h hides the C API behind #ifndef __cplusplus, but JPEGDEC.c
// (the only translation unit shipped by the IDF component) compiles them
// with C linkage. Forward-declare what we use so a C++ TU can call them.
extern "C" {
    int  JPEG_openRAM(JPEGIMAGE *pJPEG, uint8_t *pData, int iDataSize,
                      JPEG_DRAW_CALLBACK *pfnDraw);
    int  JPEG_openFile(JPEGIMAGE *pJPEG, const char *szFilename,
                       JPEG_DRAW_CALLBACK *pfnDraw);
    int  JPEG_getWidth(JPEGIMAGE *pJPEG);
    int  JPEG_getHeight(JPEGIMAGE *pJPEG);
    int  JPEG_decode(JPEGIMAGE *pJPEG, int x, int y, int iOptions);
    void JPEG_close(JPEGIMAGE *pJPEG);
    void JPEG_setPixelType(JPEGIMAGE *pJPEG, int iType);
    int  JPEG_getLastError(JPEGIMAGE *pJPEG);
}

namespace {

/* The ~19 KB JPEGIMAGE working struct must live in INTERNAL SRAM, not PSRAM.
 * JPEGDEC hammers it with small, random reads/writes (sMCUs, Huffman tables, the
 * bit buffer) during decode; in PSRAM that caused an intermittent store fault in
 * JPEGDecodeMCU (wild pMCU at a PSRAM address) -- a cache/coherency artifact, not
 * bad JPEG data. Internal SRAM removes that and is faster.
 *
 * It is reserved ONCE at boot by album_art_init(), BEFORE the FLAC decoder, mDNS
 * and the websocket server fragment the internal heap. During music playback the
 * largest free internal block drops below 19 KB, so a per-decode calloc fails and
 * album art vanishes -- reusing one pre-reserved buffer (19 KB internal, held for
 * the program's life) is the price of art that survives playback. */
static JPEGIMAGE *s_jpeg_persistent = nullptr;

void *alloc_jpeg_image(void)
{
    if (s_jpeg_persistent) {
        memset(s_jpeg_persistent, 0, sizeof(JPEGIMAGE));
        return s_jpeg_persistent;
    }
    /* Fallback if init was never called or the reservation failed: a fresh
     * internal calloc. Fine before the heap fragments; may fail during playback. */
    return heap_caps_calloc(1, sizeof(JPEGIMAGE),
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void free_jpeg_image(void *p)
{
    if (p && p != s_jpeg_persistent) heap_caps_free(p);
}

struct DecodeCtx {
    uint16_t *out;
    int       dest_w;
    int       dest_h;
};

int draw_callback(JPEGDRAW *pDraw)
{
    DecodeCtx *ctx = static_cast<DecodeCtx *>(pDraw->pUser);
    if (!ctx || !ctx->out) return 1;

    int x = pDraw->x;
    int y = pDraw->y;
    int w = pDraw->iWidth;
    int h = pDraw->iHeight;

    if (x >= ctx->dest_w || y >= ctx->dest_h) return 1;
    if (x + w > ctx->dest_w) w = ctx->dest_w - x;
    if (y + h > ctx->dest_h) h = ctx->dest_h - y;
    if (w <= 0 || h <= 0) return 1;

    const uint16_t *src = reinterpret_cast<const uint16_t *>(pDraw->pPixels);
    for (int row = 0; row < h; row++) {
        memcpy(&ctx->out[(y + row) * ctx->dest_w + x],
               &src[row * pDraw->iWidth],
               static_cast<size_t>(w) * sizeof(uint16_t));
    }
    return 1;
}

/* Shared post-open path. Caller must have already populated `pJPEG`
 * via JPEG_openRAM / JPEG_openFile so getWidth/getHeight return real
 * values. Always closes `pJPEG` before returning. */
bool decode_opened(JPEGIMAGE *pJPEG,
                   uint16_t *out_rgb, size_t out_max_pixels,
                   uint16_t *out_w, uint16_t *out_h)
{
    const int src_w = JPEG_getWidth(pJPEG);
    const int src_h = JPEG_getHeight(pJPEG);
    /* Pick the smallest (best-quality) divisor whose decoded size actually
     * fits out_max_pixels, instead of fixed absolute-size thresholds. Fixed
     * thresholds left gaps: e.g. a 350px source (>360? no -> divisor 1 ->
     * 122500px) or a 660px source (>680? no -> divisor 1 -> 435600px) could
     * exceed out_max_pixels=102400 and fail the check below even though /2
     * would easily fit. Trying divisors in order and taking the first that
     * fits covers every source size up to 8x sqrt(out_max_pixels) per side. */
    static const int k_divisor[4]     = { 1, 2, 4, 8 };
    static const int k_scale_flag[4]  = { 0, JPEG_SCALE_HALF, JPEG_SCALE_QUARTER, JPEG_SCALE_EIGHTH };
    int scale_flag = k_scale_flag[3];
    int divisor    = k_divisor[3];
    for (int i = 0; i < 4; i++) {
        int dw = src_w / k_divisor[i];
        int dh = src_h / k_divisor[i];
        if (dw > 0 && dh > 0 &&
            static_cast<size_t>(dw) * static_cast<size_t>(dh) <= out_max_pixels) {
            scale_flag = k_scale_flag[i];
            divisor    = k_divisor[i];
            break;
        }
    }
    const int dest_w = src_w / divisor;
    const int dest_h = src_h / divisor;
    ESP_LOGI(TAG, "src=%dx%d divisor=%d dest=%dx%d max_pixels=%u",
             src_w, src_h, divisor, dest_w, dest_h, (unsigned)out_max_pixels);

    if (dest_w <= 0 || dest_h <= 0 ||
        static_cast<size_t>(dest_w) * static_cast<size_t>(dest_h) > out_max_pixels) {
        ESP_LOGE(TAG, "size check failed");
        JPEG_close(pJPEG);
        return false;
    }

    DecodeCtx ctx = { out_rgb, dest_w, dest_h };
    JPEG_setPixelType(pJPEG, RGB565_LITTLE_ENDIAN);
    pJPEG->pUser = &ctx;

    const int rc       = JPEG_decode(pJPEG, 0, 0, scale_flag);
    const int last_err = JPEG_getLastError(pJPEG);
    JPEG_close(pJPEG);
    if (rc != 1) {
        ESP_LOGE(TAG, "JPEG_decode rc=%d lastError=%d", rc, last_err);
        return false;
    }

    *out_w = static_cast<uint16_t>(dest_w);
    *out_h = static_cast<uint16_t>(dest_h);
    return true;
}

}  // namespace

extern "C" void album_art_init(void)
{
    if (s_jpeg_persistent) return;
    s_jpeg_persistent = static_cast<JPEGIMAGE *>(
        heap_caps_malloc(sizeof(JPEGIMAGE), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (s_jpeg_persistent)
        ESP_LOGI(TAG, "reserved %u B internal JPEGIMAGE buffer",
                 (unsigned)sizeof(JPEGIMAGE));
    else
        ESP_LOGW(TAG, "JPEGIMAGE reservation failed; art uses per-decode alloc");
}

extern "C" bool album_art_decode(const uint8_t *jpeg, size_t jpeg_len,
                                 uint16_t *out_rgb, size_t out_max_pixels,
                                 uint16_t *out_w, uint16_t *out_h)
{
    if (!jpeg || jpeg_len == 0 || !out_rgb || !out_w || !out_h) {
        ESP_LOGE(TAG, "bad args");
        return false;
    }

    ESP_LOGI(TAG, "decode start: %u bytes, free heap=%u largest=%u",
             (unsigned)jpeg_len,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    // EOI check: a valid JPEG ends in FFD9. Spotify might be truncating.
    if (jpeg_len >= 2) {
        ESP_LOGI(TAG, "last2 = %02x %02x  (EOI is FFD9)",
                 jpeg[jpeg_len - 2], jpeg[jpeg_len - 1]);
    }
    // Walk segments until we hit a SOFn marker (FFC0..FFCF) and identify mode.
    {
        const uint8_t *p   = jpeg;
        const uint8_t *end = jpeg + jpeg_len;
        if (p[0] == 0xFF && p[1] == 0xD8) p += 2; // skip SOI
        while (p + 4 <= end && p[0] == 0xFF) {
            uint8_t  m   = p[1];
            uint16_t len = (uint16_t)((p[2] << 8) | p[3]);
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                const char *mode =
                    (m == 0xC0) ? "baseline"     :
                    (m == 0xC2) ? "progressive"  :
                    (m == 0xC1) ? "extended-seq" :
                    (m == 0xC3) ? "lossless"     : "other";
                ESP_LOGI(TAG, "SOF marker FF%02X (%s) at offset %u",
                         m, mode, (unsigned)(p - jpeg));
                break;
            }
            p += 2 + len;
        }
    }

    JPEGIMAGE *pJPEG = static_cast<JPEGIMAGE *>(alloc_jpeg_image());
    if (!pJPEG) {
        ESP_LOGE(TAG, "calloc(%u) failed", (unsigned)sizeof(JPEGIMAGE));
        return false;
    }

    if (!JPEG_openRAM(pJPEG, const_cast<uint8_t *>(jpeg),
                      static_cast<int>(jpeg_len), draw_callback)) {
        ESP_LOGE(TAG, "JPEG_openRAM failed, lastError=%d", JPEG_getLastError(pJPEG));
        free_jpeg_image(pJPEG);
        return false;
    }

    bool ok = decode_opened(pJPEG, out_rgb, out_max_pixels, out_w, out_h);
    free_jpeg_image(pJPEG);
    return ok;
}

extern "C" bool album_art_make_thumb_file(const char *path, uint16_t *out_rgb,
                                          int out_w, int out_h)
{
    if (!path || !out_rgb || out_w <= 0 || out_h <= 0) return false;

    /* Decode into a PSRAM scratch at up to 320x320 (a 640px Spotify cover
     * decodes /2 -> 320; a 300px one full size), then nearest-neighbour resize
     * down to the thumbnail size. */
    const size_t scratch_max = 320 * 320;
    uint16_t *scratch = static_cast<uint16_t *>(
        heap_caps_malloc(scratch_max * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
    if (!scratch) {
        ESP_LOGE(TAG, "thumb scratch alloc failed");
        return false;
    }

    uint16_t sw = 0, sh = 0;
    bool ok = album_art_decode_file(path, scratch, scratch_max, &sw, &sh);
    if (ok && sw > 0 && sh > 0) {
        for (int dy = 0; dy < out_h; dy++) {
            int sy = (dy * sh) / out_h;
            const uint16_t *srow = scratch + static_cast<size_t>(sy) * sw;
            uint16_t *drow = out_rgb + static_cast<size_t>(dy) * out_w;
            for (int dx = 0; dx < out_w; dx++)
                drow[dx] = srow[(dx * sw) / out_w];
        }
    } else {
        ok = false;
    }
    heap_caps_free(scratch);
    return ok;
}

extern "C" bool album_art_decode_file(const char *path,
                                      uint16_t *out_rgb, size_t out_max_pixels,
                                      uint16_t *out_w, uint16_t *out_h)
{
    if (!path || !out_rgb || !out_w || !out_h) {
        ESP_LOGE(TAG, "bad args");
        return false;
    }

    ESP_LOGI(TAG, "decode-file start: %s, free heap=%u largest=%u",
             path,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

    JPEGIMAGE *pJPEG = static_cast<JPEGIMAGE *>(alloc_jpeg_image());
    if (!pJPEG) {
        ESP_LOGE(TAG, "calloc(%u) failed", (unsigned)sizeof(JPEGIMAGE));
        return false;
    }

    if (!JPEG_openFile(pJPEG, path, draw_callback)) {
        ESP_LOGE(TAG, "JPEG_openFile failed, lastError=%d", JPEG_getLastError(pJPEG));
        free_jpeg_image(pJPEG);
        return false;
    }

    bool ok = decode_opened(pJPEG, out_rgb, out_max_pixels, out_w, out_h);
    free_jpeg_image(pJPEG);
    return ok;
}
