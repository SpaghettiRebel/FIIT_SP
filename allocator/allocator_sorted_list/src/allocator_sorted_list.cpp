#include "../include/allocator_sorted_list.h"
#include <mutex>
#include <new>
#include <algorithm>
#include <stdexcept>
#include <cstdint>

namespace {

struct allocator_header
{
    std::pmr::memory_resource *parent_allocator;
    allocator_with_fit_mode::fit_mode mode;
    size_t total_size;
    std::mutex *mutex_ptr;
    void *first_block;
};

struct block_header
{
    void *next_block;
    size_t block_size; // Total size of this block including the header
};

// Alignment requirement for all allocations and headers
constexpr size_t ALIGNMENT = alignof(std::max_align_t);

inline size_t align_up(size_t size)
{
    return (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}

inline size_t get_allocator_header_total_size()
{
    size_t size = align_up(sizeof(allocator_header));
    size += align_up(sizeof(std::mutex));
    return size;
}

inline size_t get_block_header_size()
{
    return align_up(sizeof(block_header));
}

inline allocator_header* get_header(void* trusted_memory)
{
    return reinterpret_cast<allocator_header*>(trusted_memory);
}

inline std::mutex* get_mutex(void* trusted_memory)
{
    return reinterpret_cast<std::mutex*>(reinterpret_cast<char*>(trusted_memory) + align_up(sizeof(allocator_header)));
}

} // anonymous namespace

void allocator_sorted_list::init(size_t space_size, std::pmr::memory_resource *parent_allocator, allocator_with_fit_mode::fit_mode mode)
{
    size_t header_space = get_allocator_header_total_size();
    size_t block_hdr_size = get_block_header_size();

    if (space_size < block_hdr_size) space_size = block_hdr_size;
    space_size = align_up(space_size);

    size_t total_needed = header_space + space_size;

    if (parent_allocator)
    {
        _trusted_memory = parent_allocator->allocate(total_needed);
    }
    else
    {
        _trusted_memory = ::operator new(total_needed);
    }

    auto header = new (_trusted_memory) allocator_header;
    header->parent_allocator = parent_allocator;
    header->mode = mode;
    header->total_size = total_needed;
    header->mutex_ptr = new (get_mutex(_trusted_memory)) std::mutex;

    void* first_block_ptr = reinterpret_cast<char*>(_trusted_memory) + header_space;
    header->first_block = first_block_ptr;

    auto first_block = new (first_block_ptr) block_header;
    first_block->next_block = nullptr;
    first_block->block_size = space_size;
}

allocator_sorted_list::~allocator_sorted_list()
{
    if (_trusted_memory == nullptr) return;

    auto header = get_header(_trusted_memory);
    auto mutex = get_mutex(_trusted_memory);
    auto parent = header->parent_allocator;
    auto total_size = header->total_size;

    mutex->~mutex();

    if (parent)
    {
        parent->deallocate(_trusted_memory, total_size);
    }
    else
    {
        ::operator delete(_trusted_memory);
    }
    _trusted_memory = nullptr;
}

allocator_sorted_list::allocator_sorted_list(
        size_t space_size,
        std::pmr::memory_resource *parent_allocator,
        allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    init(space_size, parent_allocator, allocate_fit_mode);
}

allocator_sorted_list::allocator_sorted_list(
    allocator_sorted_list &&other) noexcept
{
    _trusted_memory = other._trusted_memory;
    other._trusted_memory = nullptr;
}

allocator_sorted_list &allocator_sorted_list::operator=(
    allocator_sorted_list &&other) noexcept
{
    if (this != &other)
    {
        this->~allocator_sorted_list();
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    return *this;
}

allocator_sorted_list::allocator_sorted_list(const allocator_sorted_list &other)
{
    if (other._trusted_memory == nullptr)
    {
        _trusted_memory = nullptr;
        return;
    }

    auto other_header = get_header(other._trusted_memory);
    size_t space_size = other_header->total_size - get_allocator_header_total_size();

    init(space_size, other_header->parent_allocator, other_header->mode);
}

allocator_sorted_list &allocator_sorted_list::operator=(const allocator_sorted_list &other)
{
    if (this != &other)
    {
        allocator_sorted_list temp(other);
        *this = std::move(temp);
    }
    return *this;
}

[[nodiscard]] void *allocator_sorted_list::do_allocate_sm(
    size_t size)
{
    if (_trusted_memory == nullptr) throw std::bad_alloc();

    auto header = get_header(_trusted_memory);
    auto mutex = get_mutex(_trusted_memory);
    std::lock_guard<std::mutex> lock(*mutex);

    size_t block_hdr_size = get_block_header_size();
    size_t needed_payload = align_up(size);
    size_t total_block_size = block_hdr_size + needed_payload;

    block_header* best_prev = nullptr;
    block_header* best_curr = nullptr;
    block_header* prev = nullptr;
    block_header* curr = reinterpret_cast<block_header*>(header->first_block);

    while (curr)
    {
        if (curr->block_size >= total_block_size)
        {
            if (header->mode == allocator_with_fit_mode::fit_mode::first_fit)
            {
                best_prev = prev;
                best_curr = curr;
                break;
            }
            else if (header->mode == allocator_with_fit_mode::fit_mode::the_best_fit)
            {
                if (!best_curr || curr->block_size < best_curr->block_size)
                {
                    best_prev = prev;
                    best_curr = curr;
                }
            }
            else if (header->mode == allocator_with_fit_mode::fit_mode::the_worst_fit)
            {
                if (!best_curr || curr->block_size > best_curr->block_size)
                {
                    best_prev = prev;
                    best_curr = curr;
                }
            }
        }
        prev = curr;
        curr = reinterpret_cast<block_header*>(curr->next_block);
    }

    if (!best_curr)
    {
        throw std::bad_alloc();
    }

    if (best_curr->block_size >= total_block_size + block_hdr_size + ALIGNMENT)
    {
        void* next_free_ptr = reinterpret_cast<char*>(best_curr) + total_block_size;
        auto next_free = new (next_free_ptr) block_header;
        next_free->block_size = best_curr->block_size - total_block_size;
        next_free->next_block = best_curr->next_block;

        if (best_prev)
        {
            best_prev->next_block = next_free;
        }
        else
        {
            header->first_block = next_free;
        }
        best_curr->block_size = total_block_size;
    }
    else
    {
        if (best_prev)
        {
            best_prev->next_block = best_curr->next_block;
        }
        else
        {
            header->first_block = best_curr->next_block;
        }
    }

    best_curr->next_block = nullptr;

    return reinterpret_cast<char*>(best_curr) + block_hdr_size;
}

void allocator_sorted_list::do_deallocate_sm(
    void *at)
{
    if (!at) return;

    auto header = get_header(_trusted_memory);
    auto mutex = get_mutex(_trusted_memory);
    std::lock_guard<std::mutex> lock(*mutex);

    size_t header_space = get_allocator_header_total_size();
    size_t block_hdr_size = get_block_header_size();

    char* ptr = reinterpret_cast<char*>(at);
    char* start = reinterpret_cast<char*>(_trusted_memory);
    char* end = start + header->total_size;

    if (ptr < start + header_space + block_hdr_size || ptr >= end)
    {
        throw std::runtime_error("Pointer does not belong to this allocator");
    }

    block_header* to_free = reinterpret_cast<block_header*>(ptr - block_hdr_size);

    block_header* prev = nullptr;
    block_header* curr = reinterpret_cast<block_header*>(header->first_block);

    while (curr && curr < to_free)
    {
        prev = curr;
        curr = reinterpret_cast<block_header*>(curr->next_block);
    }

    to_free->next_block = curr;
    if (prev)
    {
        prev->next_block = to_free;
    }
    else
    {
        header->first_block = to_free;
    }

    if (curr && reinterpret_cast<char*>(to_free) + to_free->block_size == reinterpret_cast<char*>(curr))
    {
        to_free->block_size += curr->block_size;
        to_free->next_block = curr->next_block;
    }

    if (prev && reinterpret_cast<char*>(prev) + prev->block_size == reinterpret_cast<char*>(to_free))
    {
        prev->block_size += to_free->block_size;
        prev->next_block = to_free->next_block;
    }
}

bool allocator_sorted_list::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    auto p = dynamic_cast<const allocator_sorted_list *>(&other);
    return p && p->_trusted_memory == _trusted_memory;
}

void allocator_sorted_list::set_fit_mode(
    allocator_with_fit_mode::fit_mode mode)
{
    if (_trusted_memory == nullptr) return;
    auto mutex = get_mutex(_trusted_memory);
    std::lock_guard<std::mutex> lock(*mutex);
    get_header(_trusted_memory)->mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info() const noexcept
{
    if (_trusted_memory == nullptr) return {};
    auto mutex = get_mutex(_trusted_memory);
    std::lock_guard<std::mutex> lock(*mutex);
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info_inner() const
{
    if (_trusted_memory == nullptr) return {};
    std::vector<allocator_test_utils::block_info> result;
    auto header = get_header(_trusted_memory);

    char* current_pos = reinterpret_cast<char*>(_trusted_memory) + get_allocator_header_total_size();
    char* end_pos = reinterpret_cast<char*>(_trusted_memory) + header->total_size;

    while (current_pos < end_pos)
    {
        block_header* block = reinterpret_cast<block_header*>(current_pos);

        bool is_free = false;
        block_header* free_curr = reinterpret_cast<block_header*>(header->first_block);
        while (free_curr)
        {
            if (free_curr == block)
            {
                is_free = true;
                break;
            }
            free_curr = reinterpret_cast<block_header*>(free_curr->next_block);
        }

        result.push_back({block->block_size, !is_free});
        current_pos += block->block_size;
    }

    return result;
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_begin() const noexcept
{
    return sorted_free_iterator(_trusted_memory);
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_end() const noexcept
{
    return sorted_free_iterator(nullptr);
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::begin() const noexcept
{
    return sorted_iterator(_trusted_memory);
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::end() const noexcept
{
    return sorted_iterator(nullptr);
}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator() : _free_ptr(nullptr) {}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator(void *trusted)
{
    if (trusted)
    {
        _free_ptr = get_header(trusted)->first_block;
    }
    else
    {
        _free_ptr = nullptr;
    }
}

bool allocator_sorted_list::sorted_free_iterator::operator==(const sorted_free_iterator &other) const noexcept
{
    return _free_ptr == other._free_ptr;
}

bool allocator_sorted_list::sorted_free_iterator::operator!=(const sorted_free_iterator &other) const noexcept
{
    return !(*this == other);
}

allocator_sorted_list::sorted_free_iterator &allocator_sorted_list::sorted_free_iterator::operator++() & noexcept
{
    if (_free_ptr)
    {
        _free_ptr = reinterpret_cast<block_header*>(_free_ptr)->next_block;
    }
    return *this;
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::sorted_free_iterator::operator++(int)
{
    sorted_free_iterator temp = *this;
    ++(*this);
    return temp;
}

size_t allocator_sorted_list::sorted_free_iterator::size() const noexcept
{
    return _free_ptr ? reinterpret_cast<block_header*>(_free_ptr)->block_size - get_block_header_size() : 0;
}

void *allocator_sorted_list::sorted_free_iterator::operator*() const noexcept
{
    return _free_ptr ? reinterpret_cast<char*>(_free_ptr) + get_block_header_size() : nullptr;
}

allocator_sorted_list::sorted_iterator::sorted_iterator() : _free_ptr(nullptr), _current_ptr(nullptr), _trusted_memory(nullptr) {}

allocator_sorted_list::sorted_iterator::sorted_iterator(void *trusted) : _trusted_memory(trusted)
{
    if (_trusted_memory)
    {
        _current_ptr = reinterpret_cast<char*>(_trusted_memory) + get_allocator_header_total_size();
        _free_ptr = get_header(_trusted_memory)->first_block;
    }
    else
    {
        _current_ptr = nullptr;
        _free_ptr = nullptr;
    }
}

bool allocator_sorted_list::sorted_iterator::operator==(const sorted_iterator &other) const noexcept
{
    return _current_ptr == other._current_ptr;
}

bool allocator_sorted_list::sorted_iterator::operator!=(const sorted_iterator &other) const noexcept
{
    return !(*this == other);
}

allocator_sorted_list::sorted_iterator &allocator_sorted_list::sorted_iterator::operator++() & noexcept
{
    if (_current_ptr)
    {
        block_header* block = reinterpret_cast<block_header*>(_current_ptr);

        if (_free_ptr == _current_ptr)
        {
            _free_ptr = block->next_block;
        }

        _current_ptr = reinterpret_cast<char*>(_current_ptr) + block->block_size;

        auto header = get_header(_trusted_memory);
        if (_current_ptr >= reinterpret_cast<char*>(_trusted_memory) + header->total_size)
        {
            _current_ptr = nullptr;
            _free_ptr = nullptr;
        }
    }
    return *this;
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::sorted_iterator::operator++(int)
{
    sorted_iterator temp = *this;
    ++(*this);
    return temp;
}

size_t allocator_sorted_list::sorted_iterator::size() const noexcept
{
    return _current_ptr ? reinterpret_cast<block_header*>(_current_ptr)->block_size - get_block_header_size() : 0;
}

void *allocator_sorted_list::sorted_iterator::operator*() const noexcept
{
    return _current_ptr ? reinterpret_cast<char*>(_current_ptr) + get_block_header_size() : nullptr;
}

bool allocator_sorted_list::sorted_iterator::occupied() const noexcept
{
    return _current_ptr && _current_ptr != _free_ptr;
}
