#ifndef MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_BUDDIES_SYSTEM_H
#define MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_BUDDIES_SYSTEM_H

#include <pp_allocator.h>
#include <allocator_test_utils.h>
#include <allocator_with_fit_mode.h>
#include <mutex>
#include <cmath>

namespace __detail
{
    constexpr size_t nearest_greater_k_of_2(size_t size) noexcept
    {
        int ones_counter = 0, index = -1;

        constexpr const size_t o = 1;

        for (int i = sizeof(size_t) * 8 - 1; i >= 0; --i)
        {
            if (size & (o << i))
            {
                if (ones_counter == 0)
                    index = i;
                ++ones_counter;
            }
        }

        return ones_counter <= 1 ? index : index + 1;
    }

    constexpr size_t align_up(size_t size, size_t alignment) noexcept
    {
        return (size + alignment - 1) & ~(alignment - 1);
    }
}

class allocator_buddies_system final:
    public smart_mem_resource,
    public allocator_test_utils,
    public allocator_with_fit_mode
{

private:

    void *get_buddy(void *block) const noexcept;

    struct block_metadata
    {
        bool occupied : 1;
        unsigned char size : 7;
    };

    void *_trusted_memory;

    static constexpr const size_t alignment = alignof(std::max_align_t);
    
    static constexpr const size_t allocator_metadata_size = __detail::align_up(
        sizeof(std::pmr::memory_resource*) +
        sizeof(allocator_dbg_helper*) + 
        sizeof(fit_mode) + 
        sizeof(unsigned char) + 
        sizeof(std::mutex), 
        alignment);

    static constexpr const size_t occupied_block_metadata_size = __detail::align_up(
        sizeof(block_metadata) + sizeof(void*), 
        alignment);

    static constexpr const size_t free_block_metadata_size = __detail::align_up(
        sizeof(block_metadata), 
        alignment);

    static constexpr const size_t min_k = __detail::nearest_greater_k_of_2(occupied_block_metadata_size);
    static const size_t max_k = 64;

public:

    explicit allocator_buddies_system(
            size_t space_size_power_of_two,
            std::pmr::memory_resource *parent_allocator = nullptr,
            allocator_with_fit_mode::fit_mode allocate_fit_mode = allocator_with_fit_mode::fit_mode::first_fit);

    allocator_buddies_system(
        allocator_buddies_system const &other);
    
    allocator_buddies_system &operator=(
        allocator_buddies_system const &other);
    
    allocator_buddies_system(
        allocator_buddies_system &&other) noexcept;
    
    allocator_buddies_system &operator=(
        allocator_buddies_system &&other) noexcept;

    ~allocator_buddies_system() override;

private:
    
    [[nodiscard]] void *do_allocate_sm(size_t size) override;
    void do_deallocate_sm(void *at) override;
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;
    inline void set_fit_mode(allocator_with_fit_mode::fit_mode mode) override;
    std::vector<allocator_test_utils::block_info> get_blocks_info() const noexcept override;

private:

    std::vector<allocator_test_utils::block_info> get_blocks_info_inner() const override;
    inline std::pmr::memory_resource *&get_parent_allocator() const noexcept;
    inline allocator_dbg_helper *&get_dbg_helper() const noexcept;
    inline fit_mode &get_current_fit_mode() const noexcept;
    inline unsigned char &get_total_size_k() const noexcept;
    inline std::mutex &get_mutex() const noexcept;
    static inline void** get_trusted_ptr_address(void* block_start) noexcept;

    class buddy_iterator
    {
        void* _block;

    public:

        using iterator_category = std::forward_iterator_tag;
        using value_type = void*;
        using reference = void*&;
        using pointer = void**;
        using difference_type = ptrdiff_t;

        bool operator==(const buddy_iterator&) const noexcept;

        bool operator!=(const buddy_iterator&) const noexcept;

        buddy_iterator& operator++() & noexcept;

        buddy_iterator operator++(int n);

        size_t size() const noexcept;

        bool occupied() const noexcept;

        void* operator*() const noexcept;

        buddy_iterator();

        buddy_iterator(void* start);
    };

    friend class buddy_iterator;

    buddy_iterator begin() const noexcept;

    buddy_iterator end() const noexcept;
    
};

#endif //MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_BUDDIES_SYSTEM_H
