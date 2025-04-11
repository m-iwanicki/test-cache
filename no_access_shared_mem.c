#include <err.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdatomic.h>

#define SHM_SIZE 1024*512

int main(void)
{
    fflush(NULL);
    int fd = shm_open("test",  O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open");
        return -1;
    }
    volatile uint8_t *shared_mem = mmap(0, SHM_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    (void)&shared_mem;

    close(fd);
    return 0;
}
