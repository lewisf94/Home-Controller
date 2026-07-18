#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

bool p4_json_copy_string(const char *p, char *out, size_t out_len);

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

    return 0;
}
