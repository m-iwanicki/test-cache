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

#define TEST_REPEAT 1000
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
	void *start;
	void *end;
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

void flush_range(struct Range range) {
	void* addr = range.start;
	while (addr < range.end) {
		libflush_flush(libflush_session, addr);
		addr += LINE_LENGTH;
	}
}

void flush_and_reload(struct List ranges) {
	struct Node* node = ranges.first;

	while (node != NULL) {
		for (void* addr = node->range.start; addr < node->range.end; addr += LINE_LENGTH)
		{
			libflush_flush(libflush_session, addr);
			libflush_memory_barrier();
			usleep(RELOAD_WAIT_US);
			libflush_memory_barrier();
			uint64_t time = libflush_reload_address(libflush_session, addr);
			if (time <= threshold) {
				node->range.cache_line_hit[(addr - node->range.start) / LINE_LENGTH].count += 1;
			}
		}
		node = node->next;
	}
}

void evict_and_time(struct List ranges) {
	size_t indices[] = {0};
	uint64_t time_avg_1 = 0;
	uint64_t time_avg_2 = 0;
	access_array(sizeof(indices) / sizeof(*indices), indices);
	for (int i = 0; i < TEST_REPEAT; ++i) {
		libflush_memory_barrier();
		uint64_t time = libflush_get_timing(libflush_session);
		libflush_memory_barrier();
		access_array(sizeof(indices) / sizeof(*indices), indices);
		libflush_memory_barrier();
		time_avg_1 += libflush_get_timing(libflush_session) - time;
	}
	time_avg_1 /= TEST_REPEAT;

	for (int i = 0; i < TEST_REPEAT; ++i) {
		for (struct Node* range = ranges.first; range != NULL; range = range->next) {
			flush_range(range->range);
		}
		libflush_memory_barrier();
		uint64_t time = libflush_get_timing(libflush_session);
		libflush_memory_barrier();
		access_array(sizeof(indices) / sizeof(*indices), indices);
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
	stop = true;
}

int main(void)
{
	printf("Prepare program\n");
	fflush(NULL);
	if (libflush_init(&libflush_session, NULL) == false) {
		return -1;
	}
	threshold = calibrate(libflush_session);
	printf("Calibration threshold: %lu\n", threshold);

	pid_t pid = getpid();
	// get shared_lib.so address ranges with r--p permissions command
	const char map_cmd_fmt[] = "/bin/bash -c \"grep libshared_lib.so /proc/%d/maps | grep 'r--p' | awk '{print $1}' | tr '-' ' '\"";
	const size_t map_cmd_size = sizeof(map_cmd_fmt) + 10;
	char map_cmd[map_cmd_size];
	int ret = snprintf(map_cmd, map_cmd_size, map_cmd_fmt, pid);
	if (ret < 0) {
		printf("snprintf error: %d\n", ret);
		return -1;
	} else if (ret >= map_cmd_size) {
		printf("snprintf buffer too small\n");
		return -1;
	}
	// run previously defined command
	FILE *file = popen(map_cmd, "r");
	if (file == NULL) {
		printf("popen error: %s\n", map_cmd);
		return -1;
	}
	char range_start_str[2048], range_end_str[2048];
	char* line = NULL;
	size_t len;
	struct List ranges = { 0 };
	struct Node **range = &ranges.first;

	// parse each line (address range of shared lib)
	while(getline(&line, &len, file) != -1) {
		*range = malloc(sizeof(struct Node));
		// split 'range_start and range_end' string
		strcpy(range_start_str, strtok(line, " "));
		strcpy(range_end_str, strtok(NULL, " "));
		// convert to pointers
		sscanf(range_start_str, "%p", &(*range)->range.start);
		sscanf(range_end_str, "%p", &(*range)->range.end);
		// calculate how many cache lines address range has and allocate array
		int cache_lines = ((*range)->range.end - (*range)->range.start) / LINE_LENGTH;
		(*range)->range.cache_line_hit = malloc(cache_lines * sizeof(struct Line));
		memset((*range)->range.cache_line_hit, 0, cache_lines * sizeof(struct Line));
		// set index (offset from start) for each cache line so we will know
		// which offset it is when we sort it by number of cache hits
		for (int i = 0; i < cache_lines; ++i)
			(*range)->range.cache_line_hit[i].index = i;

		range = &(*range)->next;
		*range = NULL;
	}
	pclose(file);

	if (ranges.first) {
		printf("Average time of cache hit and cache miss when accessing shared memory\n");
		time_cache_access(ranges.first->range.start);
		fflush(NULL);
	}

	signal(SIGINT, int_handler);
	printf("\nFlush+Reload:\n");
	evict_and_time(ranges);
	fflush(NULL);
	// CTRL+C to stop and display statistics
	while(!stop) {
		flush_and_reload(ranges);
	}

	printf("Addr\t\tCache hits\tCache line offset\tByte offset\tuint64_t offset\n");
	struct Node* node = ranges.first;
	while (node != NULL) {
		int cache_lines = (node->range.end - node->range.start) / LINE_LENGTH;
		printf("range %p-%p:\n", node->range.start, node->range.end);
		struct Line *hits = node->range.cache_line_hit;
		qsort(hits, cache_lines, sizeof(*hits), cmp_line);
		for (int i = 0; i < cache_lines; ++i) {
			uint32_t index = hits[i].index;
			if (hits[i].count > 0) {
				printf("%p\t%u\t\t+%d\t\t\t+%u\t\t+<%u-%u>\n",
					node->range.start + index * LINE_LENGTH,
					hits[i].count,
					index,
					index*LINE_LENGTH,
					index*LINE_LENGTH/8,
					index*LINE_LENGTH/8 + LINE_LENGTH/8
				);
			}
		}
		node = node->next;
	}
	fflush(NULL);
	free_list(&ranges);

	printf("Dummy value: %lu\n", dummy_value);

	// Terminate libflush
	if (libflush_terminate(libflush_session) == false) {
		return -1;
	}

	return 0;
}
