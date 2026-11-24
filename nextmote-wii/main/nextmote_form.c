#include "nextmote_form.h"

#include <stdio.h>
#include <string.h>

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool nextmote_form_field(const char *body, const char *key, char *out, size_t out_len)
{
    if (!body || !key || !out || out_len == 0) return false;
    out[0] = '\0';
    const size_t key_len = strlen(key);
    const char *p = body;
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        if ((size_t)(end - p) > key_len && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            size_t n = 0;
            for (const char *v = p + key_len + 1; v < end; ++v) {
                char c = *v;
                if (c == '+') {
                    c = ' ';
                } else if (c == '%') {
                    if (end - v < 3) return false;
                    const int hi = hex_value(v[1]);
                    const int lo = hex_value(v[2]);
                    if (hi < 0 || lo < 0) return false;
                    c = (char)((hi << 4) | lo);
                    v += 2;
                }
                if (n + 1 >= out_len) {
                    out[0] = '\0';
                    return false;
                }
                out[n++] = c;
            }
            out[n] = '\0';
            return true;
        }
        if ((size_t)(end - p) == key_len && strncmp(p, key, key_len) == 0) return true; /* "key" only */
        p = *end ? end + 1 : end;
    }
    return false;
}

bool nextmote_json_escape(const char *in, char *out, size_t out_len)
{
    if (!in || !out || out_len == 0) return false;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
        char buf[7];
        size_t len;
        if (*p == '"' || *p == '\\') {
            buf[0] = '\\';
            buf[1] = (char)*p;
            len = 2;
        } else if (*p < 0x20) {
            len = (size_t)snprintf(buf, sizeof(buf), "\\u%04x", *p);
        } else {
            buf[0] = (char)*p;
            len = 1;
        }
        if (n + len >= out_len) {
            out[0] = '\0';
            return false;
        }
        memcpy(out + n, buf, len);
        n += len;
    }
    out[n] = '\0';
    return true;
}
