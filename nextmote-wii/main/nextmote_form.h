#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Finds `key` in an application/x-www-form-urlencoded body and URL-decodes its
 * value into `out` ('+' -> space, %XX -> byte). Returns false when the key is
 * missing, the encoding is malformed or the value does not fit (with NUL). */
bool nextmote_form_field(const char *body, const char *key, char *out, size_t out_len);

/* Writes `in` as JSON string content (without quotes) into `out`.
 * Returns false (with `out` empty) if it does not fit. */
bool nextmote_json_escape(const char *in, char *out, size_t out_len);
