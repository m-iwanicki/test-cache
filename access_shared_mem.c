#include "shared_lib.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#define SHM_SIZE 1024*512
#define msleep(x) usleep(x*1000);

int main(int argc, char** argv)
{
    uint64_t result = 0;
    size_t *indices;
    uint64_t repeat;
    if (argc < 3) {
        printf("%s repeat index [index]...\n", argv[0]);
        return 1;
    }
    repeat = strtoull(argv[1], NULL, 10);
    indices = malloc((argc - 2) * sizeof(size_t));
    for (int i = 0; i < (argc - 2); ++i) {
        indices[i] = strtoul(argv[i + 2], NULL, 10);
    }

    const struct timespec ts = {.tv_sec = 0, .tv_nsec = 500};

    for (uint64_t i = 0; i < repeat; ++i) {
        result ^= access_array(argc - 2, indices);
        nanosleep(&ts, NULL);
    }

    printf("result = %lu\n", result);
    return 0;
}
