#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex4(const char *p, uint32_t *out)
{
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        int h = hex_value(p[i]);
        if (h < 0) return false;
        value = (value << 4) | (uint32_t)h;
    }
    *out = value;
    return true;
}

static bool append_bytes(char *out, size_t out_len, size_t *used,
                         const char *src, size_t count)
{
    if (*used + count >= out_len) return false;
    memcpy(out + *used, src, count);
    *used += count;
    return true;
}

static bool append_codepoint(char *out, size_t out_len, size_t *used, uint32_t cp)
{
    char encoded[4];
    size_t n;
    if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = '?';

    if (cp <= 0x7f) {
        encoded[0] = (char)cp;
        n = 1;
    } else if (cp <= 0x7ff) {
        encoded[0] = (char)(0xc0 | (cp >> 6));
        encoded[1] = (char)(0x80 | (cp & 0x3f));
        n = 2;
    } else if (cp <= 0xffff) {
        encoded[0] = (char)(0xe0 | (cp >> 12));
        encoded[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        encoded[2] = (char)(0x80 | (cp & 0x3f));
        n = 3;
    } else {
        encoded[0] = (char)(0xf0 | (cp >> 18));
        encoded[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        encoded[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
        encoded[3] = (char)(0x80 | (cp & 0x3f));
        n = 4;
    }
    return append_bytes(out, out_len, used, encoded, n);
}

static size_t raw_utf8_length(const unsigned char *p)
{
    size_t n = (p[0] & 0xe0) == 0xc0 ? 2 :
               (p[0] & 0xf0) == 0xe0 ? 3 :
               (p[0] & 0xf8) == 0xf0 ? 4 : 0;
    if (!n) return 0;
    for (size_t i = 1; i < n; i++) {
        if ((p[i] & 0xc0) != 0x80) return 0;
    }
    return n;
}

/* Shared JSON string decoder for both P4 backends. The previous local parsers
 * handled common escapes but turned JSON Unicode (for example \u00d6) into the
 * literal text "u00d6". Preserve valid UTF-8 and decode surrogate pairs so the
 * compiled fallback fonts receive the real codepoint. */
bool p4_json_copy_string(const char *p, char *out, size_t out_len)
{
    if (!p || *p != '"' || !out || out_len == 0) return false;
    p++;
    size_t used = 0;
    bool truncated = false;

    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            char esc = *p++;
            char decoded = 0;
            switch (esc) {
            case '"': decoded = '"'; break;
            case '\\': decoded = '\\'; break;
            case '/': decoded = '/'; break;
            case 'b': decoded = '\b'; break;
            case 'f': decoded = '\f'; break;
            case 'n': decoded = '\n'; break;
            case 'r': decoded = '\r'; break;
            case 't': decoded = '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!parse_hex4(p, &cp)) {
                    decoded = '?';
                    break;
                }
                p += 4;
                if (cp >= 0xd800 && cp <= 0xdbff && p[0] == '\\' && p[1] == 'u') {
                    uint32_t low;
                    if (parse_hex4(p + 2, &low) && low >= 0xdc00 && low <= 0xdfff) {
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                        p += 6;
                    }
                }
                truncated |= !append_codepoint(out, out_len, &used, cp);
                continue;
            }
            default:
                decoded = esc;
                break;
            }
            truncated |= !append_bytes(out, out_len, &used, &decoded, 1);
            continue;
        }

        const unsigned char *raw = (const unsigned char *)p;
        if (*raw >= 0x80) {
            size_t n = raw_utf8_length(raw);
            if (!n) {
                truncated |= !append_codepoint(out, out_len, &used, '?');
                p++;
            } else {
                truncated |= !append_bytes(out, out_len, &used, p, n);
                p += n;
            }
        } else {
            char c = (*raw < 0x20) ? '?' : *p;
            truncated |= !append_bytes(out, out_len, &used, &c, 1);
            p++;
        }
    }

    out[used] = '\0';
    return *p == '"' && !truncated;
}
