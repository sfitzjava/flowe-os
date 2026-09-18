#pragma once
#include <cstdlib>
#include <cstddef>
extern "C" void* fbp_test_malloc(size_t);
extern "C" void fbp_test_free(void*);
#define malloc fbp_test_malloc
#define free fbp_test_free
