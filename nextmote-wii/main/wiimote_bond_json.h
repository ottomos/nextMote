#pragma once

#include <stddef.h>
#include <stdint.h>

typedef uint8_t wiimote_peer_address_t[6];

int wiimote_bond_json(const wiimote_peer_address_t *peers, size_t count,
                      char *output, size_t capacity);
