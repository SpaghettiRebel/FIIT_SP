#include "../include/allocator_sorted_list.h"

#include <not_implemented.h>
#include <cstring>
#include <cstddef>
#include <mutex>

// константы
constexpr size_t alignUp(size_t value, size_t alignment) noexcept {
    return (value + alignment - 1) / alignment * alignment;
}

static constexpr size_t alignment = alignof(std::max_align_t) > 8 ? 8 : alignof(std::max_align_t);

struct allocator_offsets {
    static constexpr size_t parent = 0;
    static constexpr size_t mode = alignUp(parent + sizeof(std::pmr::memory_resource*), alignof(allocator_with_fit_mode::fit_mode));
    static constexpr size_t size = alignUp(mode + sizeof(allocator_with_fit_mode::fit_mode), alignof(size_t));
    static constexpr size_t mtx = alignUp(size + sizeof(size_t), alignof(std::mutex));
    static constexpr size_t firstFree = alignUp(mtx + sizeof(std::mutex), alignof(void*));
    static constexpr size_t end = alignUp(firstFree + sizeof(void*), alignment);
};

struct block_offsets {
    static constexpr size_t nextOrOwner = 0;
    static constexpr size_t size = alignUp(nextOrOwner + sizeof(void*), alignof(size_t));    
    static constexpr size_t end = alignUp(size + sizeof(size_t), alignment);
};

constexpr size_t realAllocatorMetaSize = allocator_offsets::end;
constexpr size_t realBlockMetaSize = block_offsets::end;
constexpr size_t minimalFreeSize = sizeof(void*);

// вспомогательные функции
inline std::byte* ptrToBytes(void* ptr) { return reinterpret_cast<std::byte*>(ptr); }

template<typename T>
inline T& access(void* base, size_t offset) {
    return *reinterpret_cast<T*>(ptrToBytes(base) + offset);
}

inline std::pmr::memory_resource*& accessParent(void* m) { return access<std::pmr::memory_resource*>(m, allocator_offsets::parent); }
inline allocator_with_fit_mode::fit_mode& accessFitMode(void* m) { return access<allocator_with_fit_mode::fit_mode>(m, allocator_offsets::mode); }
inline size_t& accessTotalSize(void* m) { return access<size_t>(m, allocator_offsets::size); }
inline std::mutex& accessMutex(void* m) { return access<std::mutex>(m, allocator_offsets::mtx); }
inline void*& accessFirstFreeBlock(void* m) { return access<void*>(m, allocator_offsets::firstFree); }

inline void*& accessNextFreeOrOwner(void* block) { return access<void*>(block, block_offsets::nextOrOwner); }
inline size_t& accessBlockSize(void* block) { return access<size_t>(block, block_offsets::size); }
inline void* accessBlockSpace(void* block) { return ptrToBytes(block) + realBlockMetaSize; }
inline void* blockAddressFromSpace(void* space) { return ptrToBytes(space) - realBlockMetaSize; }
inline size_t fullBlockSize(void* block) { return realBlockMetaSize + accessBlockSize(block); }
inline void* nextPhysicalBlock(void* block) { return ptrToBytes(block) + fullBlockSize(block); }


// конструкторы + деструктор
allocator_sorted_list::~allocator_sorted_list() {
    if (_trusted_memory) {
        auto* parent = accessParent(_trusted_memory);
        size_t size = accessTotalSize(_trusted_memory);
        accessMutex(_trusted_memory).~mutex();
        
        if (parent) {
            parent->deallocate(_trusted_memory, size, alignment);
        } else
            ::operator delete(_trusted_memory);
    }
}

allocator_sorted_list::allocator_sorted_list(allocator_sorted_list &&other) noexcept : _trusted_memory(nullptr) {
    std::swap(_trusted_memory, other._trusted_memory);
}

allocator_sorted_list &allocator_sorted_list::operator=(allocator_sorted_list &&other) noexcept {
    if (this != &other) {
        this->~allocator_sorted_list();
        _trusted_memory = std::exchange(other._trusted_memory, nullptr);
    }
    return *this;
}

allocator_sorted_list::allocator_sorted_list(size_t space_size, std::pmr::memory_resource *parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode) {

    if (space_size < realAllocatorMetaSize + realBlockMetaSize + minimalFreeSize) throw std::bad_alloc();

    _trusted_memory = (parent_allocator) ? parent_allocator->allocate(space_size, alignment) : ::operator new(space_size);

    accessParent(_trusted_memory) = parent_allocator;
    accessFitMode(_trusted_memory) = allocate_fit_mode;
    accessTotalSize(_trusted_memory) = space_size;
    new (&accessMutex(_trusted_memory)) std::mutex();

    void* firstBlock = ptrToBytes(_trusted_memory) + realAllocatorMetaSize;
    accessFirstFreeBlock(_trusted_memory) = firstBlock;
    accessNextFreeOrOwner(firstBlock) = nullptr;
    accessBlockSize(firstBlock) = space_size - realAllocatorMetaSize - realBlockMetaSize;
}

allocator_sorted_list::allocator_sorted_list(const allocator_sorted_list &other) {
    if (!other._trusted_memory) {
        _trusted_memory = nullptr;
        return;
    }

    std::lock_guard lock(accessMutex(other._trusted_memory));
    size_t size = accessTotalSize(other._trusted_memory);
    auto* parent = accessParent(other._trusted_memory);

    _trusted_memory = (parent) ? parent->allocate(size, alignment) : ::operator new(size);
    std::memcpy(_trusted_memory, other._trusted_memory, size);

    new (&accessMutex(_trusted_memory)) std::mutex();

    std::ptrdiff_t delta = ptrToBytes(_trusted_memory) - ptrToBytes(other._trusted_memory);
    
    if (accessFirstFreeBlock(_trusted_memory)) {
        accessFirstFreeBlock(_trusted_memory) = ptrToBytes(accessFirstFreeBlock(_trusted_memory)) + delta;
    }

    void* curr = ptrToBytes(_trusted_memory) + realAllocatorMetaSize;
    void* endMem = ptrToBytes(_trusted_memory) + size;

    while (curr < endMem) {
        void*& field = accessNextFreeOrOwner(curr);
        if (field == other._trusted_memory) {
            field = _trusted_memory;
        } else if (field != nullptr) {
            field = ptrToBytes(field) + delta;
        }
        
        curr = nextPhysicalBlock(curr);
    }
}

allocator_sorted_list &allocator_sorted_list::operator=(const allocator_sorted_list &other) {
    if (this != &other) {
        allocator_sorted_list temp(other);
        std::swap(_trusted_memory, temp._trusted_memory);
    }
    return *this;
}

// основные функции
[[nodiscard]] void *allocator_sorted_list::do_allocate_sm(size_t size) {
    if (size == 0)
        return nullptr;
    
    std::lock_guard lock(accessMutex(_trusted_memory));
    const size_t targetSize = alignUp(size, alignment);
    const auto mode = accessFitMode(_trusted_memory);

    void *prev = nullptr, *curr = accessFirstFreeBlock(_trusted_memory);
    void *bestPrev = nullptr, *bestBlock = nullptr;

    while (curr) {
        size_t currSize = accessBlockSize(curr);
        if (currSize >= targetSize) {
            if (mode == fit_mode::first_fit) {
                bestBlock = curr; 
                bestPrev = prev;
                break;
            } else if (mode == fit_mode::the_best_fit) {
                if (!bestBlock || currSize < accessBlockSize(bestBlock)) {
                    bestBlock = curr; 
                    bestPrev = prev;
                }
            } else if (mode == fit_mode::the_worst_fit) {
                if (!bestBlock || currSize > accessBlockSize(bestBlock)) {
                    bestBlock = curr; 
                    bestPrev = prev;
                }
            }
        }
        prev = curr;
        curr = accessNextFreeOrOwner(curr);
    }

    if (!bestBlock) 
        throw std::bad_alloc();

    size_t oldSize = accessBlockSize(bestBlock);
    if (oldSize >= targetSize + realBlockMetaSize + minimalFreeSize) {
        void* newFree = ptrToBytes(bestBlock) + realBlockMetaSize + targetSize;
        accessBlockSize(newFree) = oldSize - targetSize - realBlockMetaSize;
        accessNextFreeOrOwner(newFree) = accessNextFreeOrOwner(bestBlock);
        
        if (!bestPrev) accessFirstFreeBlock(_trusted_memory) = newFree;
        else accessNextFreeOrOwner(bestPrev) = newFree;

        accessBlockSize(bestBlock) = targetSize;
    } else {
        if (!bestPrev) 
            accessFirstFreeBlock(_trusted_memory) = accessNextFreeOrOwner(bestBlock);
        else 
            accessNextFreeOrOwner(bestPrev) = accessNextFreeOrOwner(bestBlock);
    }

    accessNextFreeOrOwner(bestBlock) = _trusted_memory;
    return accessBlockSpace(bestBlock);
}

void allocator_sorted_list::do_deallocate_sm(void *at) {
    if (!at) 
        return;
    std::lock_guard lock(accessMutex(_trusted_memory));

    void* block = blockAddressFromSpace(at);
    if (accessNextFreeOrOwner(block) != _trusted_memory) {
        throw std::invalid_argument("Блок повреждён или не принадлежит этому аллокатору");
    }

    void *prevFree = nullptr;
    void *nextFree = accessFirstFreeBlock(_trusted_memory);
    while (nextFree && nextFree < block) {
        prevFree = nextFree;
        nextFree = accessNextFreeOrOwner(nextFree);
    }

    if (prevFree && nextPhysicalBlock(prevFree) == block) {
        accessBlockSize(prevFree) += fullBlockSize(block);
        block = prevFree; 
    } else {
        if (!prevFree)
            accessFirstFreeBlock(_trusted_memory) = block;
        else 
            accessNextFreeOrOwner(prevFree) = block;
        accessNextFreeOrOwner(block) = nextFree;
    }

    if (nextFree && nextPhysicalBlock(block) == nextFree) {
        accessBlockSize(block) += fullBlockSize(nextFree);
        accessNextFreeOrOwner(block) = accessNextFreeOrOwner(nextFree);
    } else {
        accessNextFreeOrOwner(block) = nextFree;
    }
}

bool allocator_sorted_list::do_is_equal(const std::pmr::memory_resource &other) const noexcept {
    return this == &other;
}


// прочие функции (информационные(?)
inline void allocator_sorted_list::set_fit_mode(allocator_with_fit_mode::fit_mode mode) {
    std::lock_guard lock(accessMutex(_trusted_memory));
    accessFitMode(_trusted_memory) = mode;
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info() const noexcept {
    std::lock_guard lock(accessMutex(_trusted_memory));
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info_inner() const {
    std::vector<allocator_test_utils::block_info> result;
    for (auto it = begin(); it != end(); ++it) {
        result.push_back({ it.size(), it.occupied() });
    }
    return result;
}

// итераторы
allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_begin() const noexcept {
    return sorted_free_iterator(_trusted_memory);
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_end() const noexcept {
    return sorted_free_iterator(nullptr);
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::begin() const noexcept {
    return sorted_iterator(_trusted_memory);
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::end() const noexcept {
    return sorted_iterator(nullptr);
}

bool allocator_sorted_list::sorted_free_iterator::operator==(const allocator_sorted_list::sorted_free_iterator &other) const noexcept {
    return _free_ptr == other._free_ptr;
}

bool allocator_sorted_list::sorted_free_iterator::operator!=(const allocator_sorted_list::sorted_free_iterator &other) const noexcept {
    return !(*this == other);
}

allocator_sorted_list::sorted_free_iterator &allocator_sorted_list::sorted_free_iterator::operator++() & noexcept {
    if (_free_ptr) _free_ptr = accessNextFreeOrOwner(_free_ptr);
    return *this;
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::sorted_free_iterator::operator++(int n) {
    auto a = *this;
    ++(*this);
    return a;
}

size_t allocator_sorted_list::sorted_free_iterator::size() const noexcept {
    return _free_ptr ? accessBlockSize(_free_ptr) : 0;
}

void *allocator_sorted_list::sorted_free_iterator::operator*() const noexcept { return _free_ptr; }

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator() : _free_ptr(nullptr) {}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator(void *trusted) 
    : _free_ptr(trusted ? accessFirstFreeBlock(trusted) : nullptr) {}

bool allocator_sorted_list::sorted_iterator::operator==(const allocator_sorted_list::sorted_iterator &other) const noexcept {
    return _current_ptr == other._current_ptr;
}

bool allocator_sorted_list::sorted_iterator::operator!=(const allocator_sorted_list::sorted_iterator &other) const noexcept {
    return !(*this == other);
}

allocator_sorted_list::sorted_iterator &allocator_sorted_list::sorted_iterator::operator++() & noexcept {
    if (_current_ptr) {
        void* next = nextPhysicalBlock(_current_ptr);
        void* endPtr = ptrToBytes(_trusted_memory) + accessTotalSize(_trusted_memory);
        _current_ptr = (next >= endPtr) ? nullptr : next;
    }
    return *this;
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::sorted_iterator::operator++(int n) {
    auto temp = *this;
    ++(*this);
    return temp;
}

size_t allocator_sorted_list::sorted_iterator::size() const noexcept {
    return _current_ptr ? accessBlockSize(_current_ptr) : 0;
}

void *allocator_sorted_list::sorted_iterator::operator*() const noexcept { return _current_ptr; }

allocator_sorted_list::sorted_iterator::sorted_iterator() 
    : _current_ptr(nullptr), _trusted_memory(nullptr), _free_ptr(nullptr) {}

allocator_sorted_list::sorted_iterator::sorted_iterator(void *trusted) : _trusted_memory(trusted) {
    _current_ptr = trusted ? (ptrToBytes(trusted) + realAllocatorMetaSize) : nullptr;
}

bool allocator_sorted_list::sorted_iterator::occupied() const noexcept {
    if (!_current_ptr || !_trusted_memory) 
        return false;

    return accessNextFreeOrOwner(_current_ptr) == _trusted_memory;
}
