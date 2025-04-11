#include <err.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/mman.h>
#include <stdatomic.h>

#define LLC_SIZE 1024*1024*33
#define SHM_SIZE 1024*512
#define CACHE_LINE_SIZE 64
#define TEST_REPEAT 300

#define SEC_TO_NS(sec) ((sec)*1000000000)

uint64_t dummy_value;
volatile uint8_t *shared_mem;

void flush_cache() {
    volatile uint8_t* data = malloc(LLC_SIZE);
    for (int i = 0; i < LLC_SIZE; ++i)
        data[i] = i;
    free((void*)data);
}

void fill_cache(volatile uint8_t *buff) {
    for (int line = 0; line < LLC_SIZE / CACHE_LINE_SIZE; ++line)
        buff[line*64] = (uint8_t)dummy_value;
}

uint64_t nanosec(struct timespec ts) {
    return SEC_TO_NS((uint64_t)ts.tv_sec) + (uint64_t)ts.tv_nsec;
}

/**
 * Returns ts2 - ts2 in nanoseconds
 */
uint64_t nano_diff(struct timespec ts1, struct timespec ts2) {
    uint64_t time1 = nanosec(ts1);
    uint64_t time2 = nanosec(ts2);
    // probably needs better way to deal with case where variable wraps around
    return time2 > time1 ? time2 - time1 : 0;
}

uint64_t time_access(volatile uint8_t *addr) {
    struct timespec ts1;
    struct timespec ts2;
    int8_t tmp;
    atomic_thread_fence(memory_order_acquire);
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts1);
    tmp = *addr;
    atomic_thread_fence(memory_order_release);
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts2);
    dummy_value += tmp;
    return nano_diff(ts1, ts2);
}

int cmp_uint64(const void* a, const void* b)
{
    uint64_t arg1 = *(const uint64_t*)a;
    uint64_t arg2 = *(const uint64_t*)b;
    return (arg1 > arg2) - (arg1 < arg2);
}

void time_cache_access(volatile uint8_t *buffer) {
    uint64_t cache_hit[TEST_REPEAT] = { 0 };
    uint64_t cache_miss[TEST_REPEAT] = { 0 };
    for (int i = 0; i < TEST_REPEAT; ++i) {
        buffer = malloc(LLC_SIZE);
        dummy_value += *buffer;
        cache_hit[i] += time_access(buffer);
        free((void*)buffer);
    }

    for (int i = 0; i < TEST_REPEAT; ++i) {
        buffer = malloc(LLC_SIZE);
        flush_cache();
        cache_miss[i] = time_access(buffer);
        free((void*)buffer);
    }

    qsort(cache_hit, sizeof(cache_hit)/sizeof(*cache_hit), sizeof(*cache_hit), cmp_uint64);
    qsort(cache_miss, sizeof(cache_miss)/sizeof(*cache_miss), sizeof(*cache_miss), cmp_uint64);

    printf("Mean cache hit time: %lu\n", cache_hit[TEST_REPEAT / 2]);
    printf("Mean cache miss time: %lu\n", cache_miss[TEST_REPEAT / 2]);
}

/* Should be cache miss, otherwise shared buffer is accessed (probably copied) */
void time_nop_tee_command() {
    uint64_t access_time[TEST_REPEAT] = { 0 };
    for (int i = 0; i < TEST_REPEAT; ++i) {
        flush_cache();
        atomic_thread_fence(memory_order_acquire);
        system("./no_access_shared_mem");
        atomic_thread_fence(memory_order_release);
        access_time[i] = time_access(shared_mem);
    }

    qsort(access_time, sizeof(access_time)/sizeof(*access_time), sizeof(*access_time), cmp_uint64);
    printf("Mean shared memory access time: %lu\n", access_time[TEST_REPEAT / 2]);
}

void flush_and_reload() {
    uint64_t access_time_accessed[TEST_REPEAT] = { 0 };
    uint64_t access_time_not_accessed[TEST_REPEAT] = { 0 };
    for (int i = 0; i < TEST_REPEAT; ++i) {
        flush_cache();
        atomic_thread_fence(memory_order_acquire);
        system("./access_shared_mem");
        atomic_thread_fence(memory_order_release);
        access_time_accessed[i] = time_access(shared_mem);
        // access_time_not_accessed[i] = time_access(shared_mem + SHM_SIZE - 1);
    }

    qsort(access_time_accessed, sizeof(access_time_accessed)/sizeof(*access_time_accessed),
        sizeof(*access_time_accessed), cmp_uint64);
    qsort(access_time_not_accessed, sizeof(access_time_not_accessed)/sizeof(*access_time_not_accessed),
        sizeof(*access_time_not_accessed), cmp_uint64);
    printf("Flush+Reload: Mean shared memory access time: %lu\n", access_time_accessed[TEST_REPEAT / 2]);
    printf("Flush+Reload: Mean evicted shared memory access time (should be cache miss): %lu\n", access_time_not_accessed[TEST_REPEAT / 2]);
}

void evict_and_time() {
    struct timespec ts1, ts2;
    uint64_t time_no_evict[TEST_REPEAT] = { 0 };
    uint64_t time_evict[TEST_REPEAT] = { 0 };
    uint64_t mean_time_evict, mean_time_no_evict;
    /**
     * Calculate how long it takes process to finish when shared memory is
     * in cache.
     */
    for (int i = 0; i < TEST_REPEAT; ++i) {
        flush_cache();
        dummy_value += *(uint64_t*)shared_mem;
        atomic_thread_fence(memory_order_acquire);
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts1);
        system("./access_shared_mem");
        atomic_thread_fence(memory_order_release);
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts2);
        time_no_evict[i] = nano_diff(ts1, ts2);
    }
    qsort(time_no_evict, sizeof(time_no_evict)/sizeof(*time_no_evict), sizeof(*time_no_evict), cmp_uint64);
    mean_time_no_evict = time_no_evict[TEST_REPEAT / 2];
    printf("Evict+Time: Mean program time without evicting cache: %lu\n", mean_time_no_evict);
    fflush(NULL);

    for (int i = 0; i < TEST_REPEAT; ++i) {
        flush_cache();
        atomic_thread_fence(memory_order_acquire);
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts1);
        system("./access_shared_mem");
        atomic_thread_fence(memory_order_release);
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts2);
        time_evict[i] = nano_diff(ts1, ts2);
    }

    qsort(time_evict, sizeof(time_evict)/sizeof(*time_evict), sizeof(*time_evict), cmp_uint64);
    mean_time_evict = time_evict[TEST_REPEAT / 2];
    printf("Evict+Time: Mean program time with evicted cache finished ");
    if (mean_time_evict > mean_time_no_evict) {
        printf("%lu ns later\n", mean_time_evict - mean_time_no_evict);
    } else {
        printf("%lu ns quicker\n", mean_time_no_evict - mean_time_evict);
    }
}

int main(void)
{
    printf("Prepare program\n");
    fflush(NULL);

    char path[2048];
    if (readlink("/proc/self/exe", path, 2048) == -1) {
        perror("readlink");
        return 1;
    }

    chdir(dirname(path));

    int fd = shm_open("test",  O_RDWR|O_CREAT, 0666);
    if (ftruncate(fd, SHM_SIZE) == -1) {
        perror("ftruncate");
        return 1;
    }
    shared_mem = mmap(0, SHM_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);

    /**
     * Some base statistics, how long average cache hit/miss takes and
     * whether passing shared memory buffer results in it being
     * copied/accessed.
     */
    printf("Time average time of cache hit and cache miss when accessing shared memory\n");
    time_cache_access(shared_mem);
    fflush(NULL);
    printf("\nCheck whether passing shared buffer results in it being copied\n");
    time_nop_tee_command();
    fflush(NULL);

    /* Test cases */
    printf("\nFlush+Reload:\n");
    flush_and_reload();
    fflush(NULL);

    printf("\nEvict+Time:\n");
    evict_and_time();
    fflush(NULL);

    /* Too slow (mostly flushing) */
    // printf("\nPrime+Probe:\n");
    // prime_and_probe(&tee);

    /* End test cases */

    // Print dummy value to make sure compiler doesn't optimize out
    // instructions without side effects
    printf("Dummy value: %lu\n", dummy_value);

    close(fd);
    return 0;
}
