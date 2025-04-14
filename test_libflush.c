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

#define CACHE_LINE_SIZE 64
#define TEST_REPEAT 300
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
		addr += CACHE_LINE_SIZE;
	}
}

void flush_and_reload(struct List ranges) {
	struct Node* node = ranges.first;
	while (node != NULL) {
		void* addr = node->range.start;
		while (addr < node->range.end) {
			libflush_flush(libflush_session, addr);
			libflush_memory_barrier();
			usleep(RELOAD_WAIT_US);
			libflush_memory_barrier();
			uint64_t time = libflush_reload_address(libflush_session, addr);
			if (time <= threshold) {
				node->range.cache_line_hit[(addr - node->range.start) / CACHE_LINE_SIZE].count += 1;
			}
			addr += CACHE_LINE_SIZE;
		}
		node = node->next;
	}
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

	while(getline(&line, &len, file) != -1) {
		*range = malloc(sizeof(struct Node));
		strcpy(range_start_str, strtok(line, " "));
		strcpy(range_end_str, strtok(NULL, " "));
		sscanf(range_start_str, "%p", &(*range)->range.start);
		sscanf(range_end_str, "%p", &(*range)->range.end);
		int cache_lines = ((*range)->range.end - (*range)->range.start) / CACHE_LINE_SIZE;
		(*range)->range.cache_line_hit = malloc(cache_lines * sizeof(struct Line));
		memset((*range)->range.cache_line_hit, 0, cache_lines * sizeof(struct Line));
		for (int i = 0; i < cache_lines; ++i)
			(*range)->range.cache_line_hit[i].index = i;
		printf("range: %p-%p\n", (*range)->range.start, (*range)->range.end);
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
	while(!stop) {
		flush_and_reload(ranges);
		fflush(NULL);
	}

	printf("Addr\t\tCache hits\tCache line offset\tByte offset\tuint64_t offset\n");
	struct Node* node = ranges.first;
	while (node != NULL) {
		int cache_lines = (node->range.end - node->range.start) / CACHE_LINE_SIZE;
		printf("range %p-%p:\n", node->range.start, node->range.end);
		struct Line *hits = node->range.cache_line_hit;
		qsort(hits, cache_lines, sizeof(*hits), cmp_line);
		for (int i = 0; i < cache_lines; ++i) {
			uint32_t index = hits[i].index;
			if (hits[i].count > 0) {
				printf("%p\t%u\t\t+%d\t\t\t+%u\t\t+<%u-%u>\n",
					node->range.start + index * CACHE_LINE_SIZE,
					hits[i].count,
					index,
					index*CACHE_LINE_SIZE,
					index*CACHE_LINE_SIZE/8,
					index*CACHE_LINE_SIZE/8 + CACHE_LINE_SIZE/8
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
