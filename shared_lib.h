#ifndef SHARED_LIB_H
#define SHARED_LIB_H

#include <stddef.h>
#include <stdint.h>

extern uint64_t access_array(size_t count, const size_t *indices);
extern const void* get_array();

#endif
