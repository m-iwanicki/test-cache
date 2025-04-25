#include LIBFLUSH_CONFIGURATION
#include "shared_lib.h"
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
#include <signal.h>

#define WAYS 8
#define TEST_REPEAT 100
#define RELOAD_WAIT_US 5
#define msleep(x) usleep(x*1000);

#define SEC_TO_NS(sec) ((sec)*1000000000)

uint64_t dummy_value;
uint64_t threshold;
volatile uint8_t *shared_mem;
libflush_session_t* libflush_session;
volatile atomic_bool stop = false;

// quick patch for sorting
struct Line {
	uint32_t count;
	uint32_t index;
};

struct Range {
	const void *start;
	const void *end;
	struct Line *cache_line_hit;
};

struct Node {
	struct Range range;
	struct Node *next;
};

struct List {
	struct Node *first;
};

void free_list(struct List *list) {
	struct Node *node = list ? list->first : NULL;
	while (node != NULL) {
		struct Node *next = node->next;
		free(node->range.cache_line_hit);
		free(node);
		node = next;
	}
}

int cmp_uint64(const void* a, const void* b)
{
	uint64_t arg1 = *(const uint64_t*)a;
	uint64_t arg2 = *(const uint64_t*)b;
	return (arg1 > arg2) - (arg1 < arg2);
}

int cmp_line(const void* a, const void* b)
{
	struct Line arg1 = *(const struct Line*)a;
	struct Line arg2 = *(const struct Line*)b;
	return (arg1.count > arg2.count) - (arg1.count < arg2.count);
}

void flush() {
	const size_t malloc_size = LLC_SIZE*WAYS*8;
	uint8_t* big_array = malloc(malloc_size);
	uint8_t* iter = big_array;
	if (!big_array) {
		fprintf(stderr, "malloc error\n");
		exit(1);
	}
	uint8_t* end = big_array + malloc_size;

	while (iter < end) {
		libflush_access_memory(iter);
		iter += LINE_LENGTH;
	}
	free(big_array);
}

void evict_and_time(size_t count, size_t *indices) {
	uint64_t time_avg_1 = 0;
	uint64_t time_avg_2 = 0;
	access_array(count, indices);
	for (int i = 0; i < TEST_REPEAT; ++i) {
		libflush_memory_barrier();
		uint64_t time = libflush_get_timing(libflush_session);
		libflush_memory_barrier();
		access_array(count, indices);
		libflush_memory_barrier();
		time_avg_1 += libflush_get_timing(libflush_session) - time;
	}
	time_avg_1 /= TEST_REPEAT;

	printf("count = %lu\n", count);
	printf("Indices: ");
	for (int i = 0; i < count; ++i) {
		printf("%lu ", indices[i]);
	}
	printf("\n");

	for (int i = 0; i < TEST_REPEAT; ++i) {
		flush();
		libflush_memory_barrier();
		uint64_t time = libflush_get_timing(libflush_session);
		libflush_memory_barrier();
		access_array(count, indices);
		libflush_memory_barrier();
		time_avg_2 += libflush_get_timing(libflush_session) - time;
	}
	time_avg_2 /= TEST_REPEAT;

	if (time_avg_2 > time_avg_1)
		printf("Evicted cache: %lu slower\n", time_avg_2 - time_avg_1);
	else
		printf("Evicted cache: %lu quicker\n", time_avg_1 - time_avg_2);
}

void int_handler(int unused) {
	if (stop) {
		exit(1);
	}
	stop = true;
}

int main(int argc, char **argv)
{
	printf("Prepare program\n");
	fflush(NULL);
	if (libflush_init(&libflush_session, NULL) == false) {
		return -1;
	}

	size_t *indices;
    indices = malloc((argc - 1) * sizeof(size_t));
    for (int i = 0; i < (argc - 1); ++i) {
        indices[i] = strtoul(argv[i + 1], NULL, 10);
    }

	signal(SIGINT, int_handler);
	printf("shared array = %p\n", get_array());
	printf("Evict+Time:\n");
	evict_and_time(argc - 1, indices);
	printf("\nFlush+Reload:\n");
	fflush(NULL);

	printf("Dummy value: %lu\n", dummy_value);

	// Terminate libflush
	if (libflush_terminate(libflush_session) == false) {
		return -1;
	}

	return 0;
}
