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
#include <libflush/libflush.h>
#include <calibrate.h>

#define LLC_SIZE 1024*1024*33
#define SHM_SIZE 1024*512
#define CACHE_LINE_SIZE 64
#define TEST_REPEAT 300

#define SEC_TO_NS(sec) ((sec)*1000000000)

uint64_t dummy_value;
uint64_t threshold;
volatile uint8_t *shared_mem;
libflush_session_t* libflush_session;

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
	libflush_access_memory((void*)buffer);
	for (int i = 0; i < TEST_REPEAT; ++i) {
		cache_hit[i] += libflush_reload_address(libflush_session, (void*)buffer);
	}

	libflush_flush(libflush_session, (void*)buffer);
	for (int i = 0; i < TEST_REPEAT; ++i) {
		cache_miss[i] = libflush_reload_address_and_flush(libflush_session, (void*)buffer);
	}

	qsort(cache_hit, sizeof(cache_hit)/sizeof(*cache_hit), sizeof(*cache_hit), cmp_uint64);
	qsort(cache_miss, sizeof(cache_miss)/sizeof(*cache_miss), sizeof(*cache_miss), cmp_uint64);

	printf("Mean cache hit time: %lu\n", cache_hit[TEST_REPEAT / 2]);
	printf("Mean cache miss time: %lu\n", cache_miss[TEST_REPEAT / 2]);
}

/* Should be cache miss, otherwise shared buffer is accessed (probably copied) */
void time_nop_tee_command() {
	uint64_t access_time[TEST_REPEAT] = { 0 };
	libflush_flush(libflush_session, (void*)shared_mem);
	for (int i = 0; i < TEST_REPEAT; ++i) {
		system("./no_access_shared_mem");
		access_time[i] = libflush_reload_address_and_flush(libflush_session, (void*)shared_mem);
	}

	qsort(access_time, sizeof(access_time)/sizeof(*access_time), sizeof(*access_time), cmp_uint64);
	printf("Mean shared memory access time: %lu\n", access_time[TEST_REPEAT / 2]);
	if (access_time[TEST_REPEAT / 2] < threshold)
		printf("CACHE HIT: passing shared memory results in it being accessed\n");
	else
		printf("CACHE MISS: passing shared memory doesn't result in it being accessed\n");
}

void flush_and_reload() {
	uint64_t access_time_accessed[TEST_REPEAT] = { 0 };
	uint64_t access_time_not_accessed[TEST_REPEAT] = { 0 };
	libflush_flush(libflush_session, (void*)shared_mem);
	for (int i = 0; i < TEST_REPEAT; ++i) {
		system("./access_shared_mem");
		access_time_accessed[i] = libflush_reload_address_and_flush(libflush_session, (void*)shared_mem);
	}

	libflush_flush(libflush_session, (void*)shared_mem + SHM_SIZE + 1);
	for (int i = 0; i < TEST_REPEAT; ++i) {
		system("./access_shared_mem");
		access_time_not_accessed[i] = libflush_reload_address_and_flush(libflush_session, (void*)shared_mem + SHM_SIZE - 1);
	}

	qsort(access_time_accessed, sizeof(access_time_accessed)/sizeof(*access_time_accessed),
	sizeof(*access_time_accessed), cmp_uint64);
	qsort(access_time_not_accessed, sizeof(access_time_not_accessed)/sizeof(*access_time_not_accessed),
	sizeof(*access_time_not_accessed), cmp_uint64);
	printf("Flush+Reload: Mean shared memory access time: %lu\n", access_time_accessed[TEST_REPEAT / 2]);
	printf("Flush+Reload: Mean evicted shared memory access time (should be cache miss): %lu\n", access_time_not_accessed[TEST_REPEAT / 2]);

	if (access_time_accessed[TEST_REPEAT / 2] < threshold)
		printf("CACHE HIT: ./access_shared_mem access to memory was detected\n");
	else
		printf("CACHE MISS: ./access_shared_mem access to memory wasn't detected\n");

	if (access_time_not_accessed[TEST_REPEAT / 2] < threshold)
		printf("CACHE HIT: ./access_shared_mem accessed last byte of shared memory (it shouldn't)\n");
}

void evict_and_time() {
	uint64_t ts1, ts2;
	uint64_t time_no_evict[TEST_REPEAT] = { 0 };
	uint64_t time_evict[TEST_REPEAT] = { 0 };
	uint64_t mean_time_evict, mean_time_no_evict;
	/**
	* Calculate how long it takes process to finish when shared memory is
	* in cache.
	*/
	system("./access_shared_mem");
	for (int i = 0; i < TEST_REPEAT; ++i) {
		ts1 = libflush_get_timing(libflush_session);
		system("./access_shared_mem");
		ts2 = libflush_get_timing(libflush_session);
		time_no_evict[i] = ts2 - ts1;
	}
	qsort(time_no_evict, sizeof(time_no_evict)/sizeof(*time_no_evict), sizeof(*time_no_evict), cmp_uint64);
	mean_time_no_evict = time_no_evict[TEST_REPEAT / 2];
	printf("Evict+Time: Mean program time without evicting cache: %lu\n", mean_time_no_evict);
	fflush(NULL);

	for (int i = 0; i < TEST_REPEAT; ++i) {
		libflush_flush(libflush_session, (void*)shared_mem);
		ts1 = libflush_get_timing(libflush_session);
		system("./access_shared_mem");
		ts2 = libflush_get_timing(libflush_session);
		time_no_evict[i] = ts2 - ts1;
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
	if (libflush_init(&libflush_session, NULL) == false) {
		return -1;
	}

	char path[2048];
    if (readlink("/proc/self/exe", path, 2048) == -1) {
        perror("readlink");
        return 1;
    }

    chdir(dirname(path));

	threshold = calibrate(libflush_session);
	printf("Calibration threshold: %lu\n", threshold);

	int fd = shm_open("test",  O_RDWR|O_CREAT, 0666);
	if (ftruncate(fd, SHM_SIZE) == -1) {
	    perror("ftruncate");
	    return 1;
	}
	shared_mem = mmap(0, SHM_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	printf("Shared memory: virtual to physical address: %p -> %p\n",
		   shared_mem, (void*)libflush_get_physical_address(libflush_session, (uintptr_t)shared_mem));
	void *tmp = malloc(10);
	printf("Private memory: virtual to physical address: %p -> %p\n",
		tmp, (void*)libflush_get_physical_address(libflush_session, (uintptr_t)tmp));
	free(tmp);

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

	// Terminate libflush
	if (libflush_terminate(libflush_session) == false) {
		return -1;
	}
	return 0;
}
