#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "wiimote_bond_json.h"

int main(void)
{
    const wiimote_peer_address_t peers[] = {
        {0x00, 0x13, 0xa9, 0xc4, 0xaa, 0x44},
        {0x34, 0xaf, 0x2c, 0x70, 0xb5, 0xc8},
    };
    const char expected[] =
        "{\"bonded_count\":2,\"peers\":[\"00:13:a9:c4:aa:44\",\"34:af:2c:70:b5:c8\"]}";
    char output[128];
    int length = wiimote_bond_json(peers, 2, output, sizeof(output));
    assert(length == (int)strlen(expected));
    assert(strcmp(output, expected) == 0);
    assert(wiimote_bond_json(NULL, 0, output, sizeof(output)) > 0);
    assert(strcmp(output, "{\"bonded_count\":0,\"peers\":[]}") == 0);
    assert(wiimote_bond_json(peers, 1, output, sizeof(output)) > 0);
    assert(strstr(output, "\"bonded_count\":1") != NULL);
    assert(strstr(output, "34:af:2c") == NULL);
    for (size_t capacity = 0; capacity <= strlen(expected); ++capacity) {
        assert(wiimote_bond_json(peers, 2, output, capacity) == -1);
    }
    assert(wiimote_bond_json(peers, 2, output, strlen(expected) + 1) > 0);
    assert(wiimote_bond_json(NULL, 1, output, sizeof(output)) == -1);
    assert(wiimote_bond_json(peers, 2, NULL, sizeof(output)) == -1);
    puts("Bond metadata JSON shape, address encoding, and buffer guards: PASS");
    return 0;
}
