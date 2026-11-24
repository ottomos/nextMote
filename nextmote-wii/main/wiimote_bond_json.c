#include "wiimote_bond_json.h"

#include <limits.h>
#include <stdio.h>

int wiimote_bond_json(const wiimote_peer_address_t *peers, size_t count,
                      char *output, size_t capacity)
{
    if (!output || !capacity || (count && !peers) || count > INT_MAX) return -1;
    int length = snprintf(output, capacity, "{\"bonded_count\":%zu,\"peers\":[", count);
    if (length < 0 || (size_t)length >= capacity) return -1;
    size_t used = (size_t)length;
    for (size_t i = 0; i < count; ++i) {
        length = snprintf(output + used, capacity - used,
                          "%s\"%02x:%02x:%02x:%02x:%02x:%02x\"",
                          i ? "," : "", (unsigned)peers[i][0], (unsigned)peers[i][1],
                          (unsigned)peers[i][2], (unsigned)peers[i][3],
                          (unsigned)peers[i][4], (unsigned)peers[i][5]);
        if (length < 0 || (size_t)length >= capacity - used) return -1;
        used += (size_t)length;
        if (used > INT_MAX) return -1;
    }
    if (capacity - used < 3 || used > INT_MAX - 2U) return -1;
    output[used++] = ']';
    output[used++] = '}';
    output[used] = '\0';
    return (int)used;
}
