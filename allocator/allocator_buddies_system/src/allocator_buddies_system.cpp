#include "../include/allocator_buddies_system.h"
#include <new>
#include <cstring>
#include <stdexcept>
#include <algorithm>


inline std::pmr::memory_resource *&allocator_buddies_system::get_parent_allocator() const noexcept
{
    return *reinterpret_cast<std::pmr::memory_resource**>(_trusted_memory);
}

inline allocator_dbg_helper *&allocator_buddies_system::get_dbg_helper() const noexcept
{
    size_t offset = __detail::align_up(sizeof(std::pmr::memory_resource*), alignof(allocator_dbg_helper*));
    return *reinterpret_cast<allocator_dbg_helper**>(reinterpret_cast<std::byte*>(_trusted_memory) + offset);
}

inline allocator_with_fit_mode::fit_mode &allocator_buddies_system::get_current_fit_mode() const noexcept
{
    size_t offset = __detail::align_up(sizeof(std::pmr::memory_resource*) + sizeof(allocator_dbg_helper*), alignof(fit_mode));
    return *reinterpret_cast<fit_mode*>(reinterpret_cast<std::byte*>(_trusted_memory) + offset);
}

inline unsigned char &allocator_buddies_system::get_total_size_k() const noexcept
{
    size_t offset = __detail::align_up(sizeof(std::pmr::memory_resource*) + sizeof(allocator_dbg_helper*) + sizeof(fit_mode), alignof(unsigned char));
    return *reinterpret_cast<unsigned char*>(reinterpret_cast<std::byte*>(_trusted_memory) + offset);
}

inline std::mutex &allocator_buddies_system::get_mutex() const noexcept
{
    size_t offset = __detail::align_up(sizeof(std::pmr::memory_resource*) + sizeof(allocator_dbg_helper*) + sizeof(fit_mode) + sizeof(unsigned char), alignof(std::mutex));
    return *reinterpret_cast<std::mutex*>(reinterpret_cast<std::byte*>(_trusted_memory) + offset);
}

inline void** allocator_buddies_system::get_trusted_ptr_address(void* block_start) noexcept
{
    size_t ptr_offset = __detail::align_up(sizeof(block_metadata), alignof(void*));
    return reinterpret_cast<void**>(reinterpret_cast<std::byte*>(block_start) + ptr_offset);
}

void *allocator_buddies_system::get_buddy(void *block) const noexcept
{
    size_t offset = reinterpret_cast<std::byte*>(block) - 
                    (reinterpret_cast<std::byte*>(_trusted_memory) + allocator_metadata_size);
    
    unsigned char k = reinterpret_cast<block_metadata*>(block)->size;
    offset ^= (1ULL << k);

    return reinterpret_cast<std::byte*>((_trusted_memory)) + allocator_metadata_size + offset;
}


allocator_buddies_system::~allocator_buddies_system()
{
    if (!_trusted_memory) return;

    size_t total_size = (1ULL << get_total_size_k()) + allocator_metadata_size;
    std::pmr::memory_resource* parent = get_parent_allocator();
    
    get_mutex().~mutex();
    parent->deallocate(_trusted_memory, total_size, alignment);
}

allocator_buddies_system::allocator_buddies_system(
        size_t space_size, // количество байт
        std::pmr::memory_resource *parent_allocator,
        allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    unsigned char k = static_cast<unsigned char>(__detail::nearest_greater_k_of_2(space_size));

    if (k < min_k || k > max_k)
        throw std::logic_error("Запрашиваемый размер памяти некорректен");

    size_t total_memory_size = (1ULL << k) + allocator_metadata_size;
    
    std::pmr::memory_resource* actual_parent = parent_allocator ? parent_allocator : std::pmr::get_default_resource();
    _trusted_memory = actual_parent->allocate(total_memory_size, alignment);

    get_parent_allocator() = actual_parent;
    get_dbg_helper() = nullptr;
    get_current_fit_mode() = allocate_fit_mode;
    get_total_size_k() = k;
    
    new (&get_mutex()) std::mutex();

    void* first_block = reinterpret_cast<std::byte*>(_trusted_memory) + allocator_metadata_size;
    auto* meta = reinterpret_cast<block_metadata*>(first_block);
    meta->occupied = false;
    meta->size = k;
}

allocator_buddies_system::allocator_buddies_system(allocator_buddies_system &&other) noexcept 
    : _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_buddies_system &allocator_buddies_system::operator=(allocator_buddies_system &&other) noexcept {
    if (this != &other)
    {
        if (_trusted_memory) this->~allocator_buddies_system();
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    return *this;
}

[[nodiscard]] void *allocator_buddies_system::do_allocate_sm(size_t size)
{
    std::lock_guard lock(get_mutex());

    size_t required_size = size + occupied_block_metadata_size;
    unsigned char target_k = static_cast<unsigned char>(std::max(min_k, (size_t)__detail::nearest_greater_k_of_2(required_size)));
    
    void* found_block = nullptr;
    auto mode = get_current_fit_mode();

    if (mode == fit_mode::first_fit) {
        for (auto it = begin(); it != end(); ++it) {
            if (!it.occupied() && it.size() >= (1ULL << target_k)) { 
                found_block = *it; 
                break; 
            }
        }
    } else {
        size_t extreme_size = (mode == fit_mode::the_best_fit) ? size_t(-1) : 0;
        for (auto it = begin(); it != end(); ++it) {
            if (!it.occupied() && it.size() >= (1ULL << target_k)) {
                if ((mode == fit_mode::the_best_fit && it.size() < extreme_size) || 
                    (mode == fit_mode::the_worst_fit && it.size() > extreme_size)) {
                    extreme_size = it.size();
                    found_block = *it;
                }
            }
        }
    }

    if (!found_block) throw std::bad_alloc();
  
    while (reinterpret_cast<block_metadata*>(found_block)->size > target_k)
    {
        auto* meta = reinterpret_cast<block_metadata*>(found_block);
        meta->size -= 1;
        
        void* buddy = reinterpret_cast<std::byte*>(found_block) + (1ULL << meta->size);
        auto* buddy_meta = reinterpret_cast<block_metadata*>(buddy);
        buddy_meta->occupied = false;
        buddy_meta->size = meta->size;
    }

    auto* final_meta = reinterpret_cast<block_metadata*>(found_block);
    final_meta->occupied = true;
    
    *get_trusted_ptr_address(found_block) = _trusted_memory;

    return reinterpret_cast<std::byte*>(found_block) + occupied_block_metadata_size;
}

void allocator_buddies_system::do_deallocate_sm(void *at)
{
    if (!at) return;
    std::lock_guard lock(get_mutex());

    void* block_ptr = reinterpret_cast<std::byte*>(at) - occupied_block_metadata_size;
    auto* meta = reinterpret_cast<block_metadata*>(block_ptr);

    if (*get_trusted_ptr_address(block_ptr) != _trusted_memory)
        throw std::logic_error("Деаллокация памяти, не принадлежащей этому аллокатору");

    meta->occupied = false;

    void* current = block_ptr;
    size_t base_addr = reinterpret_cast<size_t>(reinterpret_cast<std::byte*>(_trusted_memory) + allocator_metadata_size);
    
    while (meta->size < get_total_size_k()) {
        void* buddy = get_buddy(current);
        auto* buddy_meta = reinterpret_cast<block_metadata*>(buddy);

        // занят или разбит
        if (buddy_meta->occupied || buddy_meta->size != meta->size)
            break;

        if (buddy < current) {
            current = buddy;
        }
        
        meta = reinterpret_cast<block_metadata*>(current);
        meta->size += 1;
    }
}

allocator_buddies_system::allocator_buddies_system(const allocator_buddies_system &other)
{
    std::lock_guard lock(other.get_mutex());

    size_t power = other.get_total_size_k();
    size_t total_size = (1ULL << power) + allocator_metadata_size;
    
    std::pmr::memory_resource* parent = other.get_parent_allocator();
    
    _trusted_memory = parent->allocate(total_size, alignment);
    
    std::memcpy(_trusted_memory, other._trusted_memory, total_size);
    
    new (&get_mutex()) std::mutex();
    
    for (auto it = begin(); it != end(); ++it)
    {
        if (it.occupied())
        {
            void** trusted_ptr = get_trusted_ptr_address(*it);
            *trusted_ptr = _trusted_memory;
        }
    }
}

allocator_buddies_system &allocator_buddies_system::operator=(const allocator_buddies_system &other)
{
    if (this == &other)
        return *this;

    allocator_buddies_system tmp(other);
    
    *this = std::move(tmp);

    return *this;
}

bool allocator_buddies_system::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return this == &other;
}

void allocator_buddies_system::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard lock(get_mutex());
    get_current_fit_mode() = mode;
}

std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info() const noexcept
{
    std::lock_guard lock(get_mutex());
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> info;
    for (auto it = begin(); it != end(); ++it)
        info.push_back({it.size(), it.occupied()});
    return info;
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::begin() const noexcept
{
    return buddy_iterator(reinterpret_cast<std::byte*>(_trusted_memory) + allocator_metadata_size);
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::end() const noexcept
{
    return buddy_iterator(reinterpret_cast<std::byte*>(_trusted_memory) + allocator_metadata_size + (1ULL << get_total_size_k()));
}

bool allocator_buddies_system::buddy_iterator::operator==(const buddy_iterator& other) const noexcept { return _block == other._block; }
bool allocator_buddies_system::buddy_iterator::operator!=(const buddy_iterator& other) const noexcept { return !(*this == other); }

allocator_buddies_system::buddy_iterator& allocator_buddies_system::buddy_iterator::operator++() & noexcept 
{
    _block = reinterpret_cast<std::byte*>(_block) + size();
    return *this;
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::buddy_iterator::operator++(int)
{
    buddy_iterator tmp = *this;
    ++(*this);
    return tmp;
}

size_t allocator_buddies_system::buddy_iterator::size() const noexcept 
{ 
    return 1ULL << reinterpret_cast<block_metadata*>(_block)->size; 
}

bool allocator_buddies_system::buddy_iterator::occupied() const noexcept 
{ 
    return reinterpret_cast<block_metadata*>(_block)->occupied; 
}

void* allocator_buddies_system::buddy_iterator::operator*() const noexcept { return _block; }

allocator_buddies_system::buddy_iterator::buddy_iterator(void* start) : _block(start) {}
allocator_buddies_system::buddy_iterator::buddy_iterator() : _block(nullptr) {}