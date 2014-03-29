#pragma once

#include <cstdlib>
#include <mutex>

namespace malloc_intercept
{

	void* hoardMalloc(size_t size, size_t alignment = 1);
	void* hoardCalloc(size_t n, size_t size);
	void hoardFree(void* pointer);
	void* hoardRealloc(void* pointer, size_t size);
	int hoardPosixMemalign(void** memoryPointer, size_t alignment, size_t size);

	bool isValidAlignment(size_t alignment);
	size_t const DEFAULT_ALIGNMENT = 8;

	void* const MAGIC_BYTES_FOR_SINGLEBLOCK = (void*)0x12345671;
	void* const MAGIC_BYTES_FOR_BLOCK = (void*)0x12345672;
	void* const MAGIC_BYTES_FOR_FREE_BLOCK = (void*)0x12345673;
	void* const MAGIC_BYTES_FOR_SUPERBLOCK = (void*)0x12345674;
}
