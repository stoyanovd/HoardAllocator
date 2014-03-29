#include "internals.h"
#include "tracing.h"
#include <sys/mman.h>
#include <stack>
#include <mutex>
#include <thread>
#include <cstring>
#include <unistd.h>
#include <algorithm>

namespace malloc_intercept
{
	struct SuperBlock;
	struct Heap;
	struct SingleBlock;
	struct Lockable;

	struct SingleBlock
	{
		size_t sizeOfData;
		size_t alignedSize;
		void* magicBytes;
		SingleBlock* linkToThis;

		SingleBlock(size_t _sizeOfData, size_t _alignedSize);
	};

	struct Block
	{
		SuperBlock* superBlock;

		size_t sizeOfData;
		size_t alignedSize;

		void* magicBytes;
		Block* linkToThis;

		Block(SuperBlock* _superBlock, size_t _sizeOfData, size_t _alignedSize);
		void freeBlock();
	};

	struct Lockable
	{
	private:
		std::mutex mutex;
		bool locked;

	public:
		void lock()
		{
			mutex.lock();
			locked = 1;
		}

		bool tryLock()
		{
			return mutex.try_lock();
		}

		void unlock()
		{
			locked = 0;
			mutex.unlock();
		}
	};

	struct SuperBlock : Lockable
	{
	private:
		const size_t sizeClass;
		const size_t maxSizeOfDataPerBlock;
		const size_t maxCountOfBlocks;

		const size_t offsetOfFirstBlock;
		size_t nextOffsetIndex;

		size_t countOfFreeBlocksInStack;

		size_t calcMaxCountOfBlocks();

	public:
		Heap* heap;

		size_t usedMemory;

		SuperBlock* previous;
		SuperBlock* next;

		void* magicBytes;

		SuperBlock(size_t sizePerBlock, size_t sizeClazz);
		Block* allocateBlock(size_t size, size_t sizeOfData);
		void addFreeBlock(Block* block);
		void deattachFromHeap();
		void attachToHeap(Heap* heap);
	};

	struct Heap : Lockable
	{
		SuperBlock** firstSBbySizeclass;
		SuperBlock** lastSBbySizeclass;

		size_t usedMemory;
		size_t allocatedMemory;

		Heap();
		Block* allocateBlock(size_t size, size_t sizeOfData);
	};

	void abortIfFalse(bool condition)
	{
		if (!condition)
			std::abort();
	}

	const int MAX_THREAD_COUNT = 200;
	const double K = 1.2;
	const double BASE = 4;
	const double FRACTION = 1.0/4;

	size_t SIZE_OF_SUPERBLOCK;
	int COUNT_OF_CLASSSIZE;
	size_t HEAPS_COUNT;

	Heap* commonHeap;
	Heap* heaps;

	const size_t PRECALC_MAX_SIZE = 256;
	size_t* roundedUpSizes;
	size_t* classesForSizes;

	bool initialized;
	Lockable initializationLock;

	size_t roundUpSize(size_t size)
	{
		if (initialized && size < PRECALC_MAX_SIZE)
			return roundedUpSizes[size];

		double curSize = BASE;
		size_t classSize = 0;
		while (curSize < size)
		{
			curSize *= BASE;
			classSize++;
		}
		return (size_t)curSize;
	}

	size_t getClassSize(size_t size)
	{
		if (initialized && size < PRECALC_MAX_SIZE)
			return classesForSizes[size];

		double curSize = BASE;
		size_t classSize = 0;
		while (curSize < size)
		{
			curSize *= BASE;
			classSize++;
		}
		return classSize;
	}

	void * getMap(size_t size)
	{
		return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	}

	void init()
	{
		if (!initialized)
		{
			initializationLock.lock();
			if (!initialized)
			{
				SIZE_OF_SUPERBLOCK = 4 * sysconf(_SC_PAGE_SIZE);
				COUNT_OF_CLASSSIZE = getClassSize(SIZE_OF_SUPERBLOCK) + 1;
				HEAPS_COUNT = 2 * MAX_THREAD_COUNT;

				heaps = (Heap*)getMap(sizeof(Heap)* (HEAPS_COUNT + 1));
				commonHeap = heaps + HEAPS_COUNT;
				for (Heap* heap = heaps; heap < heaps + HEAPS_COUNT + 1; heap++)
					new(heap)Heap();

				roundedUpSizes = (size_t*)getMap(sizeof(size_t)* PRECALC_MAX_SIZE * 2);
				classesForSizes = roundedUpSizes + PRECALC_MAX_SIZE;

				for (size_t size = 0; size < PRECALC_MAX_SIZE; size++)
				{
					roundedUpSizes[size] = roundUpSize(size);
					classesForSizes[size] = getClassSize(size);
				}
				initialized = true;
			}
			initializationLock.unlock();
		}
	}

	size_t getThreadIdHash()
	{
		size_t tidHash = std::hash<std::thread::id>()(std::this_thread::get_id());
		return tidHash;
	}

	bool isValidAlignment(size_t alignment)
	{
		size_t tmp = alignment;
		while (tmp > sizeof(void*))
		{
			if (tmp % 2 != 0)
				return false;
			tmp /= 2;
		}
		return tmp % sizeof(void*) == 0;
	}

	void* roundUpToAlignment(void* pointer, size_t alignment)
	{
		size_t mod = (size_t)pointer % alignment;
		if (mod == 0)
			return pointer;
		else
			return (void*)((char*)pointer + alignment - mod);
	}

	void setLinkTo(void* from, void* to)
	{
		*(void**)((char*)from - sizeof(void*)) = to;
	}

	SingleBlock::SingleBlock(size_t _sizeOfData, size_t _alignedSize)
		: sizeOfData(_sizeOfData),
		alignedSize(_alignedSize),
		magicBytes(MAGIC_BYTES_FOR_SINGLEBLOCK),
		linkToThis(this)
	{
	}

	Block::Block(SuperBlock* _superBlock, size_t _sizeOfData, size_t _alignedSize)
		: superBlock(_superBlock),
		sizeOfData(_sizeOfData),
		alignedSize(_alignedSize),
		magicBytes(MAGIC_BYTES_FOR_BLOCK),
		linkToThis(this)
	{
	}

	void Block::freeBlock()
	{
		abortIfFalse(magicBytes == MAGIC_BYTES_FOR_BLOCK);
		abortIfFalse(superBlock->magicBytes == MAGIC_BYTES_FOR_SUPERBLOCK);
		magicBytes = MAGIC_BYTES_FOR_FREE_BLOCK;
		superBlock->addFreeBlock(this);
	}

	size_t SuperBlock::calcMaxCountOfBlocks()
	{
		return (SIZE_OF_SUPERBLOCK - sizeof(SuperBlock)) /
			(sizeof(void*)+sizeof(Block)+this->maxSizeOfDataPerBlock);
	}

	SuperBlock::SuperBlock(size_t sizePerBlock, size_t sizeClazz)
		: heap(NULL),
		usedMemory(0),
		previous(NULL),
		next(NULL),
		sizeClass(sizeClazz),
		maxSizeOfDataPerBlock(sizePerBlock),
		maxCountOfBlocks(calcMaxCountOfBlocks()),
		offsetOfFirstBlock((size_t)(void*)((char*) this + sizeof(SuperBlock)+sizeof(Block*)* maxCountOfBlocks)),
		nextOffsetIndex(0),
		countOfFreeBlocksInStack(0),
		magicBytes(MAGIC_BYTES_FOR_SUPERBLOCK)
	{
	}

	Block* SuperBlock::allocateBlock(size_t size, size_t sizeOfData)
	{
		abortIfFalse(size <= maxSizeOfDataPerBlock);

		Block* result = NULL;
		if (nextOffsetIndex != maxCountOfBlocks)
		{
			result = (Block*)((char*)offsetOfFirstBlock + nextOffsetIndex * (sizeof(Block)+maxSizeOfDataPerBlock));
			nextOffsetIndex++;
		}
		else if (countOfFreeBlocksInStack != 0)
		{
			countOfFreeBlocksInStack--;
			result = *((Block**)((char*) this + sizeof(SuperBlock)+countOfFreeBlocksInStack * sizeof(Block*)));
		}
		if (result != NULL)
		{
			new (result)Block(this, sizeOfData, size);
			usedMemory += sizeOfData;
			heap->usedMemory += sizeOfData;
		}
		return result;
	}

	void SuperBlock::deattachFromHeap()
	{
		heap->usedMemory -= usedMemory;
		heap->allocatedMemory -= SIZE_OF_SUPERBLOCK;

		if (previous == NULL)
			heap->firstSBbySizeclass[sizeClass] = next;
		else
			previous->next = next;

		if (next == NULL)
			heap->lastSBbySizeclass[sizeClass] = previous;
		else
			next->previous = previous;

		previous = NULL;
		next = NULL;
		heap = NULL;
	}

	void SuperBlock::attachToHeap(Heap* heap)
	{
		this->heap = heap;
		heap->usedMemory += usedMemory;
		heap->allocatedMemory += SIZE_OF_SUPERBLOCK;
		SuperBlock* insertAfter = heap->lastSBbySizeclass[sizeClass];
		while (insertAfter != NULL && insertAfter->usedMemory < usedMemory)
			insertAfter = insertAfter->previous;
		if (insertAfter == NULL)
		{
			previous = NULL;
			next = heap->firstSBbySizeclass[sizeClass];
			if (heap->firstSBbySizeclass[sizeClass] != NULL)
				heap->firstSBbySizeclass[sizeClass]->previous = this;

			heap->firstSBbySizeclass[sizeClass] = this;
			if (heap->lastSBbySizeclass[sizeClass] == NULL)
				heap->lastSBbySizeclass[sizeClass] = this;
		}
		else
		{
			next = insertAfter->next;
			previous = insertAfter;
			insertAfter->next = this;
			if (next == NULL)
				heap->lastSBbySizeclass[sizeClass] = this;
			else
				next->previous = this;
		}
	}

	void SuperBlock::addFreeBlock(Block* block)
	{
		Block** offsetForNewFreeBlock = (Block**)((char*) this + sizeof(SuperBlock)+countOfFreeBlocksInStack * sizeof(Block*));
		*offsetForNewFreeBlock = block;
		countOfFreeBlocksInStack++;
		usedMemory -= block->sizeOfData;
		heap->usedMemory -= block->sizeOfData;
	}

	Heap::Heap()
	{
		firstSBbySizeclass = (SuperBlock**)getMap(sizeof(SuperBlock*) * COUNT_OF_CLASSSIZE);
		lastSBbySizeclass = (SuperBlock**)getMap(sizeof(SuperBlock*) * COUNT_OF_CLASSSIZE);
		usedMemory = 0;
		allocatedMemory = sizeof(Heap) + 2 * sizeof(SuperBlock*) * COUNT_OF_CLASSSIZE;
		for (int i = 0; i < COUNT_OF_CLASSSIZE; i++)
		{
			firstSBbySizeclass[i] = NULL;
			lastSBbySizeclass[i] = NULL;
		}
	}

	Block* Heap::allocateBlock(size_t size, size_t sizeOfData)
	{
		size_t classSize = getClassSize(size);
		SuperBlock* cur = firstSBbySizeclass[classSize];
		while (cur != NULL)
		{
			Block* result = cur->allocateBlock(size, sizeOfData);
			if (result != NULL)
				return result;
			cur = cur->next;
		}
		return NULL;
	}

	void* allocateBigBlock(size_t size, size_t alignment)
	{
		void* result;
		size_t alignedSize = roundUpSize(size + alignment - 1);
		SingleBlock* singleBlock = (SingleBlock*)getMap(sizeof(SingleBlock) + alignedSize);
		if (singleBlock != NULL && singleBlock != MAP_FAILED)
		{
			new (singleBlock)SingleBlock(size, alignedSize);
			result = roundUpToAlignment((char*)singleBlock + sizeof(SingleBlock), alignment);
			setLinkTo(result, singleBlock);
			return result;
		}
		return NULL;
	}


	void lockHeap(Heap * heap)
	{
		heap->lock();
	}

	void lockSBAndHeap(SuperBlock * superBlock)
	{
		while(true)
		{
			superBlock->lock();
			Heap* heap = superBlock->heap;
			if(heap->tryLock())
				return;
			superBlock->unlock();
			std::this_thread::yield();
		}
	}

	void* hoardMalloc(size_t size, size_t alignment)
	{
		init();
		if (size == 0)
			return NULL;
		size_t alignedSize = roundUpSize(size + alignment - 1);
		if (alignedSize > SIZE_OF_SUPERBLOCK / 2)
			return allocateBigBlock(size, alignment);
		void* result;

		size_t heapId = getThreadIdHash() % HEAPS_COUNT;
		Heap* heap = &heaps[heapId];
		heap->lock();
		Block* allocatedBlock = heap->allocateBlock(alignedSize, size);
		heap->unlock();
		if (allocatedBlock == NULL)
		{
			commonHeap->lock();
			allocatedBlock = commonHeap->allocateBlock(alignedSize, size);
			commonHeap->unlock();
			if (allocatedBlock == NULL)
			{
				SuperBlock* superBlock = (SuperBlock*)getMap(SIZE_OF_SUPERBLOCK);
				if (superBlock != NULL && superBlock != MAP_FAILED)
				{
					new (superBlock)SuperBlock(roundUpSize(alignedSize), getClassSize(alignedSize));
					heap->lock();
					superBlock->attachToHeap(heap);
					allocatedBlock = superBlock->allocateBlock(alignedSize, size);
					heap->unlock();
				}
			}
			else
			{
				SuperBlock* superBlock = allocatedBlock->superBlock;
				superBlock->lock();
				Heap* oldHeap = superBlock->heap;
				if (oldHeap == commonHeap)
				{
					lockHeap(heap);
					lockHeap(commonHeap);
					superBlock->deattachFromHeap();
					superBlock->attachToHeap(heap);
					commonHeap->unlock();
					heap->unlock();
				}
				superBlock->unlock();
			}
		}
		if (allocatedBlock != NULL)
		{
			result = roundUpToAlignment((char*)allocatedBlock + sizeof(Block), alignment);
			setLinkTo(result, allocatedBlock);
		}
		return result;
	}

	void* hoardCalloc(size_t n, size_t size)
	{
		init();
		if (size == 0)
			return NULL;
		void* result;
		result = hoardMalloc(n * size);
		if (result != NULL)
			memset(result, 0, n * size);
		return result;
	}

	void freeBigBlock(void* pointer)
	{
		SingleBlock* singleBlock = (SingleBlock*)((char*)pointer - sizeof(SingleBlock));
		singleBlock = singleBlock->linkToThis;
		abortIfFalse(singleBlock->magicBytes == MAGIC_BYTES_FOR_SINGLEBLOCK
			&& singleBlock->alignedSize > SIZE_OF_SUPERBLOCK / 2);
		singleBlock->magicBytes = MAGIC_BYTES_FOR_FREE_BLOCK;
		munmap(singleBlock, sizeof(SingleBlock)+singleBlock->alignedSize);
	}

	void hoardFree(void* pointer)
	{
		init();
		if (pointer == NULL)
			return;
		Block* block = (Block*)((char*)pointer - sizeof(Block));
		block = block->linkToThis;
		if (block->magicBytes != MAGIC_BYTES_FOR_BLOCK)
		{
			freeBigBlock(pointer);
			return;
		}
		SuperBlock* superBlock = block->superBlock;
		lockSBAndHeap(superBlock);
		Heap* heap = superBlock->heap;

		block->freeBlock();
		if (heap != commonHeap)
		{
			if (heap->usedMemory + K * SIZE_OF_SUPERBLOCK < heap->allocatedMemory
				&& heap->usedMemory < (1 - FRACTION) * heap->allocatedMemory /*true*/)
			{
				commonHeap->lock();
				SuperBlock* minUsedBlock = NULL;
				for (int i = 0; i < COUNT_OF_CLASSSIZE; i++)
				if (heap->lastSBbySizeclass[i] != NULL)
				{
					if (minUsedBlock == NULL || minUsedBlock->usedMemory > heap->lastSBbySizeclass[i]->usedMemory)
						minUsedBlock = heap->lastSBbySizeclass[i];
				}
				if (minUsedBlock != superBlock)
					minUsedBlock->lock();

				minUsedBlock->deattachFromHeap();
				minUsedBlock->attachToHeap(commonHeap);

				commonHeap->unlock();
				heap->unlock();
				if (minUsedBlock != superBlock)
					minUsedBlock->unlock();

			}
			else
				heap->unlock();
		}
		else
			heap->unlock();
		superBlock->unlock();
	}

	void* hoardRealloc(void* pointer, size_t size)
	{
		init();
		void* result = hoardMalloc(size);
		if (pointer != NULL && result != NULL)
		{
			Block* block = (Block*)((char*)pointer - sizeof(Block));
			block = block->linkToThis;
			if (block->magicBytes == MAGIC_BYTES_FOR_BLOCK)
				memcpy(result, pointer, std::min(size, block->sizeOfData));
			else
			{
				SingleBlock* singleBlock = (SingleBlock*)((char*)pointer - sizeof(SingleBlock));
				singleBlock = singleBlock->linkToThis;
				abortIfFalse(singleBlock->magicBytes == MAGIC_BYTES_FOR_SINGLEBLOCK
					&& singleBlock->alignedSize > SIZE_OF_SUPERBLOCK / 2);
				memcpy(result, pointer, std::min(size, singleBlock->sizeOfData));
			}
			hoardFree(pointer);
		}
		return result;
	}

	int hoardPosixMemalign(void** memoryPointer, size_t alignment, size_t size)
	{
		init();
		void* result = NULL;
		*memoryPointer = NULL;
		if (!isValidAlignment(alignment))
			return EINVAL;

		result = hoardMalloc(size, alignment);
		if (result == NULL)
			return ENOMEM;

		*memoryPointer = result;
		return 0;
	}

}
