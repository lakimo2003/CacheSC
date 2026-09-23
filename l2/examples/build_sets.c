#include "l2_evset.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    const uint32_t ids[] = {30, 60, 99};
    l2_evsets attacker = {0};

    if (l2_evsets_create(&attacker, ids, 3, L2_ASSOCIATIVITY, 4096) == -1) {
        int error = errno;
        perror("l2_evsets_create");
        if (error == EACCES || error == EPERM)
            fputs("Usable physical frame numbers are required from pagemap.\n",
                  stderr);
        return EXIT_FAILURE;
    }

    printf("Retained pool: %zu bytes; translated %zu pages\n",
           attacker.pool_size, attacker.pages_scanned);
    for (size_t i = 0; i < attacker.set_count; ++i) {
        printf("Set %u: %zu lines, head %p\n", attacker.sets[i].id,
               attacker.sets[i].count, (void *)attacker.sets[i].head);
    }

    l2_evsets_destroy(&attacker);
    return EXIT_SUCCESS;
}
