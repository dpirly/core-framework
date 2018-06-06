#include <ossie/shm/Heap.h>
#include "Superblock.h"
#include "Block.h"
#include "ThreadState.h"

#include <stdexcept>
#include <cstring>
#include <iostream>
#include <cstdlib>
#include <vector>

#include <unistd.h>

#include <boost/thread.hpp>

using namespace redhawk::shm;

#define PAGE_ROUND_DOWN(x,p) ((x/p)*p)
#define PAGE_ROUND_UP(x,p) (((x+p-1)/p)*p)

class Heap::PrivateHeap {
public:
    PrivateHeap(int id, Heap* heap) :
        _id(id),
        _heap(heap)
    {
    }

    void* allocate(size_t bytes)
    {
        // NB: Thread-specific state may not be needed with per-CPU private
        //     heaps; it is maintained here to avoid modifying the superblock
        //     API (for now)
        ThreadState* state = _heap->_getThreadState();
        boost::mutex::scoped_lock lock(_mutex);
        for (SuperblockList::iterator superblock = _superblocks.begin(); superblock != _superblocks.end(); ++superblock) {
            void* ptr = (*superblock)->allocate(state, bytes);
            if (ptr) {
                // Move the successful superblock to the front of the list,
                // under the assumption that it is more likely to satisfy a
                // future request
                std::iter_swap(superblock, _superblocks.begin());
                return ptr;
            }
        }

        Superblock* superblock = _heap->_createSuperblock(bytes);
        _superblocks.insert(_superblocks.begin(), superblock);
        return superblock->allocate(state, bytes);
    }

private:
    int _id;
    Heap* _heap;

    boost::mutex _mutex;
    typedef std::vector<Superblock*> SuperblockList;
    SuperblockList _superblocks;
};

Heap::Heap(const std::string& name) :
    _file(name)
{
    _file.create();
    int nprocs = sysconf(_SC_NPROCESSORS_CONF);
    for (int id = 0; id < nprocs; ++id) {
        _allocs.push_back(new PrivateHeap(id, this));
    }
}

Heap::~Heap()
{
#ifdef HEAP_DEBUG
    std::cout << _superblocks.size() << " superblocks" << std::endl;
    std::cout << _file.size() << " total bytes" << std::endl;
#endif
}

void* Heap::allocate(size_t bytes)
{
    PrivateHeap* heap = _getPrivateHeap();
    return heap->allocate(bytes);
}

void Heap::deallocate(void* ptr)
{
    Superblock::deallocate(ptr);
}

MemoryRef Heap::getRef(const void* ptr)
{
    Block* block = Block::from_pointer(const_cast<void*>(ptr));
    const Superblock* superblock = block->getSuperblock();
    MemoryRef ref;
    ref.heap = superblock->heap();
    ref.superblock = superblock->offset();
    ref.offset = block->offset();
    return ref;
}

const std::string& Heap::name() const
{
    return _file.name();
}

Heap::PrivateHeap* Heap::_getPrivateHeap()
{
    size_t cpuid = sched_getcpu();
    assert(cpuid < _allocs.size());
    return _allocs[cpuid];
}

ThreadState* Heap::_getThreadState()
{
    ThreadState* state = _threadState.get();
    if (!state) {
        state = new ThreadState();
        _threadState.reset(state);
    }
    return state;
}

Superblock* Heap::_createSuperblock(size_t minSize)
{
    boost::mutex::scoped_lock lock(_mutex);
    size_t superblock_size = DEFAULT_SUPERBLOCK_SIZE;
    const char* superblock_size_env = getenv("SUPERBLOCK_SIZE");
    if (superblock_size_env) {
        char* end;
        superblock_size = strtoll(superblock_size_env, &end, 10);
        if ((superblock_size == 0) || (*end != '\0')) {
            std::cerr << "Invalid superblock size, using default" << std::endl;
            superblock_size = DEFAULT_SUPERBLOCK_SIZE;
        } else {
            // Shared memory should be allocated along page boundaries
            superblock_size = PAGE_ROUND_UP(superblock_size, MappedFile::PAGE_SIZE);
            std::cout << "Using superblock size " << superblock_size << std::endl;
        }
    }

    // Ensure that the superblock is large enough for the request, accounting
    // for the overhead of the block metadata (roughly)
    // TODO: Should extra large requests be handled differently? In glibc,
    //       above a certain size it starts using mmap/munmap. As a quick "fix"
    //       use a minimum of 2 blocks plus overhead.
    minSize = (minSize + 64) * 2;
    if (minSize > superblock_size) {
        superblock_size = PAGE_ROUND_UP(minSize, MappedFile::PAGE_SIZE);
    }
    return _file.createSuperblock(superblock_size);
}