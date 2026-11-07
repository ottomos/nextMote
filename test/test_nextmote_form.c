#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "nextmote_form.h"

static void test_form_field(void)
{
    char out[16];
    assert(nextmote_form_field("ssid=My+Net%21&password=a%2Bb", "ssid", out, sizeof(out)));
    assert(strcmp(out, "My Net!") == 0);
    assert(nextmote_form_field("ssid=My+Net%21&password=a%2Bb", "password", out, sizeof(out)));
    assert(strcmp(out, "a+b") == 0);
    assert(nextmote_form_field("xssid=1&ssid=2", "ssid", out, sizeof(out)));
    assert(strcmp(out, "2") == 0);
    assert(nextmote_form_field("password=", "password", out, sizeof(out)));
    assert(out[0] == '\0');
    assert(!nextmote_form_field("password=x", "ssid", out, sizeof(out)));
    assert(!nextmote_form_field("ssid=%4", "ssid", out, sizeof(out)));
    assert(!nextmote_form_field("ssid=%zz", "ssid", out, sizeof(out)));
    assert(!nextmote_form_field("ssid=0123456789abcdef", "ssid", out, sizeof(out)));
    assert(nextmote_form_field("ssid=0123456789abcde", "ssid", out, sizeof(out)));
}

static void test_json_escape(void)
{
    char out[32];
    assert(nextmote_json_escape("a\"b\\c\n", out, sizeof(out)));
    assert(strcmp(out, "a\\\"b\\\\c\\u000a") == 0 || strcmp(out, "a\\\"b\\\\c\\n") == 0);
    char small[4];
    assert(!nextmote_json_escape("\"\"\"", small, sizeof(small)));
    assert(small[0] == '\0');
}

int main(void)
{
    test_form_field();
    test_json_escape();
    puts("nextmote_form tests passed");
    return 0;
}
