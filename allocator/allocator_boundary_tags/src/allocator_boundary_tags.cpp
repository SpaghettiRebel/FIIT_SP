#include <new>
#include <stdexcept>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>
#include "../include/allocator_boundary_tags.h"


struct BlockHeader {
    size_t size;               
    void* owner;               
    BlockHeader* prev_occupied; 
    BlockHeader* next_occupied;
};

struct PoolHeader {
    std::pmr::memory_resource* parent;
    allocator_with_fit_mode::fit_mode mode;
    size_t total_size;
    BlockHeader* first_occupied;
    alignas(std::mutex) std::mutex mutex; 
};

inline PoolHeader* getPool(void* p) noexcept { 
    return static_cast<PoolHeader*>(p); 
}

inline std::byte* getPoolDataStart(PoolHeader* ph) noexcept { 
    return reinterpret_cast<std::byte*>(ph) + sizeof(PoolHeader); 
}

inline std::byte* getPoolEnd(PoolHeader* ph) noexcept { 
    return reinterpret_cast<std::byte*>(ph) + ph->total_size; 
}

inline void* getBlockData(BlockHeader* h) noexcept { 
    return static_cast<void*>(reinterpret_cast<std::byte*>(h) + sizeof(BlockHeader)); 
}

inline BlockHeader* getBlockHeader(void* data) noexcept { 
    return reinterpret_cast<BlockHeader*>(static_cast<std::byte*>(data) - sizeof(BlockHeader)); 
}

inline std::byte* nextPhysicalBlock(BlockHeader* h) noexcept { 
    return reinterpret_cast<std::byte*>(h) + sizeof(BlockHeader) + h->size; 
}

inline BlockHeader* getLastOccupied(PoolHeader* ph) noexcept {
    BlockHeader* curr = ph->first_occupied;
    if (!curr) 
        return nullptr;

    while (curr->next_occupied) 
        curr = curr->next_occupied;

    return curr;
}

inline BlockHeader* getNextOccupiedFromGap(PoolHeader* ph, void* gap_start) noexcept {
    BlockHeader* curr = ph->first_occupied;

    while (curr) {
        if (reinterpret_cast<std::byte*>(curr) >= reinterpret_cast<std::byte*>(gap_start)) {
            return curr;
        }
        curr = curr->next_occupied;
    }

    return nullptr;
}

// конструктор/деструктор

allocator_boundary_tags::allocator_boundary_tags(size_t space_size, std::pmr::memory_resource *parent, allocator_with_fit_mode::fit_mode mode) {
    if (!parent) 
        parent = std::pmr::get_default_resource();
    
    size_t total = sizeof(PoolHeader) + space_size; 
    _trusted_memory = parent->allocate(total);
    
    PoolHeader* ph = getPool(_trusted_memory);
    ph->parent = parent;
    ph->mode = mode;
    ph->total_size = total;
    ph->first_occupied = nullptr;
    new (&ph->mutex) std::mutex();
}

allocator_boundary_tags::~allocator_boundary_tags() {
    if (!_trusted_memory) 
        return;

    PoolHeader* ph = getPool(_trusted_memory);
    std::pmr::memory_resource* parent = ph->parent;
    size_t total = ph->total_size;
    ph->mutex.~mutex();
    parent->deallocate(_trusted_memory, total);
}

// основные
[[nodiscard]] void *allocator_boundary_tags::do_allocate_sm(size_t size) {
    PoolHeader* ph = getPool(_trusted_memory);
    std::lock_guard<std::mutex> lock(ph->mutex);
    
    BlockHeader* best_prev_end = nullptr;
    BlockHeader* best_right_block = nullptr;
    size_t best_gap = (ph->mode == fit_mode::the_best_fit) ? std::numeric_limits<size_t>::max() : 0;
    bool found_any = false;

    auto update_best = [&](std::byte* gap_start, BlockHeader* right_block, size_t g) {
        if (g < sizeof(BlockHeader) + (size > 0 ? size : 1)) 
            return false;
        
        bool update = !found_any;
        if (found_any) {
            if (ph->mode == fit_mode::the_best_fit && g < best_gap) 
                update = true;
            else if (ph->mode == fit_mode::the_worst_fit && g > best_gap) 
                update = true;
        }
        
        if (update) {
            found_any = true;
            best_prev_end = reinterpret_cast<BlockHeader*>(gap_start);
            best_right_block = right_block;
            best_gap = g;

            return (ph->mode == fit_mode::first_fit); 
        }
        return false;
    };

    BlockHeader* curr = ph->first_occupied;
    std::byte* last_end = getPoolDataStart(ph);

    while (curr) {
        size_t gap = reinterpret_cast<std::byte*>(curr) - last_end;
        if (update_best(last_end, curr, gap)) break;
        last_end = nextPhysicalBlock(curr);
        curr = curr->next_occupied;
    }
    
    if (!found_any || ph->mode != fit_mode::first_fit) {
        size_t gap = getPoolEnd(ph) - last_end;
        update_best(last_end, nullptr, gap);
    }

    if (!found_any) 
        throw std::bad_alloc();

    BlockHeader* newBlock = best_prev_end;
    
    size_t usefulSize = size;
    if (best_gap < sizeof(BlockHeader) + size + sizeof(BlockHeader)) {
        usefulSize = best_gap - sizeof(BlockHeader);
    }

    newBlock->size = usefulSize;
    newBlock->owner = _trusted_memory;

    newBlock->next_occupied = best_right_block;
    
    BlockHeader* prev_occ = nullptr;
    if (best_right_block) {
        prev_occ = best_right_block->prev_occupied;
        best_right_block->prev_occupied = newBlock;
    } else {
        prev_occ = getLastOccupied(ph);
    }

    newBlock->prev_occupied = prev_occ;

    if (prev_occ) {
        prev_occ->next_occupied = newBlock;
    } else {
        ph->first_occupied = newBlock;
    }

    return getBlockData(newBlock);
}

void allocator_boundary_tags::do_deallocate_sm(void *at) {
    if (!at) 
        return;
    PoolHeader* ph = getPool(_trusted_memory);
    std::lock_guard<std::mutex> lock(ph->mutex);
    
    BlockHeader* block = getBlockHeader(at);
    
    if (reinterpret_cast<std::byte*>(block) < getPoolDataStart(ph) || reinterpret_cast<std::byte*>(block) >= getPoolEnd(ph) || block->owner != _trusted_memory) {
        throw std::logic_error("Неверный указатель для деаллокации");
    }

    if (block->prev_occupied) 
        block->prev_occupied->next_occupied = block->next_occupied;
    else 
        ph->first_occupied = block->next_occupied;

    if (block->next_occupied) 
        block->next_occupied->prev_occupied = block->prev_occupied;
}

// копирование/перемещение
allocator_boundary_tags::allocator_boundary_tags(const allocator_boundary_tags &other) : _trusted_memory(nullptr) {
    if (!other._trusted_memory) 
        return;
    PoolHeader* other_ph = getPool(other._trusted_memory);
    
    std::lock_guard<std::mutex> lock(other_ph->mutex);
    size_t total = other_ph->total_size;
    
    _trusted_memory = other_ph->parent->allocate(total);
    std::memcpy(_trusted_memory, other._trusted_memory, total);
    
    PoolHeader* ph = getPool(_trusted_memory);
    new (&ph->mutex) std::mutex();
    
    std::ptrdiff_t delta = reinterpret_cast<std::byte*>(_trusted_memory) - reinterpret_cast<std::byte*>(other._trusted_memory);
    
    if (ph->first_occupied) {
        ph->first_occupied = reinterpret_cast<BlockHeader*>(reinterpret_cast<std::byte*>(ph->first_occupied) + delta);
        BlockHeader* curr = ph->first_occupied;
        while (curr) {
            curr->owner = _trusted_memory;
            if (curr->prev_occupied) {
                curr->prev_occupied = reinterpret_cast<BlockHeader*>(reinterpret_cast<std::byte*>(curr->prev_occupied) + delta);
            }
            if (curr->next_occupied) {
                curr->next_occupied = reinterpret_cast<BlockHeader*>(reinterpret_cast<std::byte*>(curr->next_occupied) + delta);
            }
            curr = curr->next_occupied;
        }
    }
}

allocator_boundary_tags &allocator_boundary_tags::operator=(const allocator_boundary_tags &other) {
    if (this != &other) { 
        allocator_boundary_tags tmp(other); 
        *this = std::move(tmp); 
    }
    return *this;
}

allocator_boundary_tags::allocator_boundary_tags(allocator_boundary_tags &&other) noexcept : _trusted_memory(other._trusted_memory) {
    other._trusted_memory = nullptr;
}

allocator_boundary_tags &allocator_boundary_tags::operator=(allocator_boundary_tags &&other) noexcept {
    if (this != &other) { 
        this->~allocator_boundary_tags();
        _trusted_memory = other._trusted_memory; 
        other._trusted_memory = nullptr; 
    }
    return *this;
}

// остальное

void allocator_boundary_tags::set_fit_mode(allocator_with_fit_mode::fit_mode mode) {
    PoolHeader* ph = getPool(_trusted_memory);
    std::lock_guard<std::mutex> lock(ph->mutex);
    ph->mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info() const {
    PoolHeader* ph = getPool(_trusted_memory);
    
    std::lock_guard<std::mutex> lock(ph->mutex);
    std::vector<allocator_test_utils::block_info> info;

    for (auto it = begin(); it != end(); ++it) { 
        info.push_back({ it.size(), it.occupied() }); 
    }
    return info;
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info_inner() const {
    return get_blocks_info();
}

bool allocator_boundary_tags::do_is_equal(const std::pmr::memory_resource &other) const noexcept { 
    return this == &other; 
}

// итератор
allocator_boundary_tags::boundary_iterator allocator_boundary_tags::begin() const noexcept {
    if (!_trusted_memory) 
        return boundary_iterator();

    PoolHeader* ph = getPool(_trusted_memory);
    BlockHeader* first_occ = ph->first_occupied;
    std::byte* pool_start = getPoolDataStart(ph);
    
    if (!first_occ || reinterpret_cast<std::byte*>(first_occ) > pool_start) {
        return boundary_iterator(_trusted_memory, pool_start, false);
    }
    return boundary_iterator(_trusted_memory, first_occ, true);
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::end() const noexcept {
    if (!_trusted_memory) 
        return boundary_iterator();

    return boundary_iterator(_trusted_memory, getPoolEnd(getPool(_trusted_memory)), false);
}

allocator_boundary_tags::boundary_iterator::boundary_iterator() 
    : _occupied_ptr(nullptr), _occupied(false), _trusted_memory(nullptr) {}

bool allocator_boundary_tags::boundary_iterator::operator==(const boundary_iterator &other) const noexcept {
    return _occupied_ptr == other._occupied_ptr && _occupied == other._occupied;
}

bool allocator_boundary_tags::boundary_iterator::operator!=(const boundary_iterator &other) const noexcept {
    return !(*this == other);
}

allocator_boundary_tags::boundary_iterator &allocator_boundary_tags::boundary_iterator::operator++() & noexcept {
    if (!_trusted_memory || _occupied_ptr == getPoolEnd(getPool(_trusted_memory))) return *this;
    PoolHeader* ph = getPool(_trusted_memory);

    if (_occupied) {
        BlockHeader* block = reinterpret_cast<BlockHeader*>(_occupied_ptr);
        std::byte* gap_start = nextPhysicalBlock(block);
        BlockHeader* next_occ = block->next_occupied;
        std::byte* right_limit = next_occ ? reinterpret_cast<std::byte*>(next_occ) : getPoolEnd(ph);

        if (gap_start < right_limit) {
            _occupied_ptr = gap_start;
            _occupied = false; 
        } else {
            _occupied_ptr = right_limit;
            _occupied = (_occupied_ptr != getPoolEnd(ph));
        }
    } else {
        BlockHeader* next_occ = getNextOccupiedFromGap(ph, _occupied_ptr);
        if (next_occ) {
            _occupied_ptr = next_occ;
            _occupied = true;
        } else {
            _occupied_ptr = getPoolEnd(ph);
            _occupied = false;
        }
    }
    return *this;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator++(int) {
    auto tmp = *this; 
    ++(*this); 
    return tmp;
}

allocator_boundary_tags::boundary_iterator &allocator_boundary_tags::boundary_iterator::operator--() & noexcept {
    if (!_trusted_memory) return *this;
    PoolHeader* ph = getPool(_trusted_memory);

    if (!_occupied) {
        if (_occupied_ptr == getPoolDataStart(ph)) return *this;

        if (_occupied_ptr != getPoolEnd(ph)) {
            _occupied_ptr = getLastOccupied(ph); 
            
            BlockHeader* curr = ph->first_occupied;
            while (curr && nextPhysicalBlock(curr) < reinterpret_cast<std::byte*>(_occupied_ptr)) {
                curr = curr->next_occupied;
            }
            _occupied_ptr = curr;
            _occupied = true;
        } else {
            BlockHeader* last = getLastOccupied(ph);
            if (!last) {
                _occupied_ptr = getPoolDataStart(ph);
                _occupied = false; 
            } else {
                if (nextPhysicalBlock(last) < getPoolEnd(ph)) {
                    _occupied_ptr = nextPhysicalBlock(last);
                    _occupied = false;
                } else {
                    _occupied_ptr = last;
                    _occupied = true;
                }
            }
        }
    } else {
        BlockHeader* block = reinterpret_cast<BlockHeader*>(_occupied_ptr);
        BlockHeader* prev_occ = block->prev_occupied;
        std::byte* left_limit = prev_occ ? nextPhysicalBlock(prev_occ) : getPoolDataStart(ph);

        if (left_limit < reinterpret_cast<std::byte*>(block)) {
            _occupied_ptr = left_limit;
            _occupied = false;
        } else {
            _occupied_ptr = prev_occ;
            _occupied = true;
        }
    }
    return *this;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator--(int) {
    auto tmp = *this; 
    --(*this); 
    return tmp;
}

size_t allocator_boundary_tags::boundary_iterator::size() const noexcept {
    if (!_trusted_memory || _occupied_ptr == getPoolEnd(getPool(_trusted_memory))) return 0;
    
    if (_occupied) {
        return sizeof(BlockHeader) + reinterpret_cast<BlockHeader*>(_occupied_ptr)->size;
    }

    PoolHeader* ph = getPool(_trusted_memory);
    BlockHeader* next_occ = getNextOccupiedFromGap(ph, _occupied_ptr);
    std::byte* end_of_gap = next_occ ? reinterpret_cast<std::byte*>(next_occ) : getPoolEnd(ph);
    
    return end_of_gap - reinterpret_cast<std::byte*>(_occupied_ptr);
}

bool allocator_boundary_tags::boundary_iterator::occupied() const noexcept { return _occupied; }

void* allocator_boundary_tags::boundary_iterator::operator*() const noexcept {
    return get_ptr();
}

void* allocator_boundary_tags::boundary_iterator::get_ptr() const noexcept { 
    if (_trusted_memory == nullptr) return nullptr;
    if (_occupied_ptr == accessEnd(_trusted_memory)) return nullptr;
    
    return _occupied_ptr; 
}