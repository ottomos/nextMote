#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wiimote_inquiry_policy.h"

int main(void)
{
    const uint8_t captured_capacity[] = {0x0e, 0x05, 0x05, 0x38, 0x0c, 0x00, 0x01};
    uint8_t capacity = 99;
    assert(wiimote_inquiry_parse_capacity(captured_capacity, sizeof(captured_capacity),
                                         &capacity));
    assert(capacity == 1);
    for (size_t length = 0; length < sizeof(captured_capacity); ++length) {
        capacity = 99;
        assert(!wiimote_inquiry_parse_capacity(captured_capacity, length, &capacity));
        assert(capacity == 99);
    }
    uint8_t invalid_capacity[sizeof(captured_capacity)];
    memcpy(invalid_capacity, captured_capacity, sizeof(invalid_capacity));
    invalid_capacity[5] = 0x12;
    assert(!wiimote_inquiry_parse_capacity(invalid_capacity, sizeof(invalid_capacity), &capacity));
    invalid_capacity[5] = 0;
    invalid_capacity[6] = 0;
    assert(!wiimote_inquiry_parse_capacity(invalid_capacity, sizeof(invalid_capacity), &capacity));
    invalid_capacity[6] = 0x41;
    assert(!wiimote_inquiry_parse_capacity(invalid_capacity, sizeof(invalid_capacity), &capacity));
    assert(!wiimote_inquiry_parse_capacity(NULL, 0, &capacity));
    assert(!wiimote_inquiry_parse_capacity(captured_capacity, sizeof(captured_capacity), NULL));

    const wiimote_iac_t pair[] = {{0x9e, 0x8b, 0x00}, {0x9e, 0x8b, 0x33}};
    const wiimote_iac_t reversed[] = {{0x9e, 0x8b, 0x33}, {0x9e, 0x8b, 0x00}};
    const wiimote_iac_t unrelated[] = {{0x9e, 0x8b, 0x01}, {0x9e, 0x8b, 0x33}};
    wiimote_iac_t selected[2];
    uint8_t count = 99;
    assert(wiimote_inquiry_select_codes(1, 2, pair, selected, &count));
    assert(count == 1 && memcmp(selected[0], pair[1], sizeof(wiimote_iac_t)) == 0);
    assert(wiimote_inquiry_select_codes(1, 2, reversed, selected, &count));
    assert(count == 1 && memcmp(selected[0], pair[1], sizeof(wiimote_iac_t)) == 0);
    assert(wiimote_inquiry_select_codes(2, 2, pair, selected, &count));
    assert(count == 2 && memcmp(selected, pair, sizeof(pair)) == 0);
    assert(wiimote_inquiry_select_codes(0x40, 2, reversed, selected, &count));
    assert(count == 2 && memcmp(selected, reversed, sizeof(reversed)) == 0);
    assert(wiimote_inquiry_select_codes(1, 1, reversed, selected, &count));
    assert(count == 1 && memcmp(selected[0], reversed[0], sizeof(wiimote_iac_t)) == 0);
    assert(!wiimote_inquiry_select_codes(0, 2, pair, selected, &count));
    assert(!wiimote_inquiry_select_codes(1, 0, pair, selected, &count));
    assert(!wiimote_inquiry_select_codes(1, 3, pair, selected, &count));
    assert(!wiimote_inquiry_select_codes(1, 2, unrelated, selected, &count));
    assert(!wiimote_inquiry_select_codes(1, 2, NULL, selected, &count));
    assert(!wiimote_inquiry_select_codes(1, 2, pair, NULL, &count));
    assert(!wiimote_inquiry_select_codes(1, 2, pair, selected, NULL));

    wiimote_iac_t code;
    wiimote_inquiry_get_code(false, code);
    assert(memcmp(code, pair[0], sizeof(code)) == 0);
    wiimote_inquiry_get_code(true, code);
    assert(memcmp(code, pair[1], sizeof(code)) == 0);
    puts("Controller capacity decoding, single-IAC selection, dual-IAC preservation, and rotation codes: PASS");
    return 0;
}
