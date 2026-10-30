#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

bool p4_json_copy_string(const char *p, char *out, size_t out_len);
void p4_json_escape(char *out, size_t out_len, const char *src);
bool p4_url_encode(char *out, size_t out_len, const char *src);

int main(void)
{
    char out[64];

    assert(p4_json_copy_string("\"plain\"", out, sizeof(out)));
    assert(strcmp(out, "plain") == 0);

    assert(p4_json_copy_string("\"quote: \\\" slash: \\\\ line\\n tab\\t\"", out, sizeof(out)));
    assert(strcmp(out, "quote: \" slash: \\ line\n tab\t") == 0);

    assert(p4_json_copy_string("\"\\u00d6resund\"", out, sizeof(out)));
    assert(strcmp(out, "\xC3\x96resund") == 0);

    assert(p4_json_copy_string("\"\\ud83c\\udfb5\"", out, sizeof(out)));
    assert(memcmp(out, "\xF0\x9F\x8E\xB5", 5) == 0);

    assert(p4_json_copy_string("\"raw \xC3\xA9\"", out, sizeof(out)));
    assert(strcmp(out, "raw \xC3\xA9") == 0);

    assert(p4_json_copy_string("\"\\ud800\"", out, sizeof(out)));
    assert(strcmp(out, "?") == 0);

    assert(!p4_json_copy_string("not a string", out, sizeof(out)));
    assert(!p4_json_copy_string("\"unterminated", out, sizeof(out)));
    assert(!p4_json_copy_string(NULL, out, sizeof(out)));
    assert(!p4_json_copy_string("\"x\"", NULL, sizeof(out)));
    assert(!p4_json_copy_string("\"x\"", out, 0));

    char tiny[4];
    assert(!p4_json_copy_string("\"abcdef\"", tiny, sizeof(tiny)));
    assert(strcmp(tiny, "abc") == 0);

    p4_json_escape(out, sizeof(out), "say \"hi\" \\ now\n");
    assert(strcmp(out, "say \\\"hi\\\" \\\\ now ") == 0);

    /* The escape for '"' needs two bytes. With one byte left, stop before it
     * so the JSON command never gets a bare quote. */
    char three[3];
    p4_json_escape(three, sizeof(three), "a\"b");
    assert(strcmp(three, "a") == 0);
    char four[4];
    p4_json_escape(four, sizeof(four), "a\\b");
    assert(strcmp(four, "a\\\\") == 0);
    p4_json_escape(four, sizeof(four), NULL);
    assert(strcmp(four, "") == 0);

    assert(p4_url_encode(out, sizeof(out), "AC/DC Back in Black~"));
    assert(strcmp(out, "AC%2FDC%20Back%20in%20Black~") == 0);
    assert(p4_url_encode(out, sizeof(out), "\xC3\x96"));
    assert(strcmp(out, "%C3%96") == 0);
    assert(p4_url_encode(four, sizeof(four), " "));
    assert(strcmp(four, "%20") == 0);
    assert(!p4_url_encode(four, sizeof(four), "  "));
    assert(p4_url_encode(four, sizeof(four), "abc"));
    assert(!p4_url_encode(four, sizeof(four), "abcd"));

    return 0;
}
