#ifndef MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_RED_BLACK_TREE_H
#define MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_RED_BLACK_TREE_H

#include <pp_allocator.h>
#include <allocator_test_utils.h>
#include <allocator_with_fit_mode.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <vector>

inline constexpr size_t align_up(size_t value, size_t alignment) noexcept {
    return (value + alignment - 1) / alignment * alignment;
}

inline constexpr size_t max_size_t(size_t a, size_t b) noexcept {
    return (a > b) ? a : b;
}

inline constexpr size_t allocator_alignment =
    max_size_t(
        max_size_t(alignof(std::max_align_t), alignof(std::mutex)),
        max_size_t(alignof(void*), alignof(allocator_with_fit_mode::fit_mode))
    );

class allocator_red_black_tree final :
    public smart_mem_resource,
    public allocator_test_utils,
    public allocator_with_fit_mode
{
private:
    enum class block_color : std::uint8_t { RED = 0, BLACK = 1 };

    struct block_data
    {
        bool occupied : 4;
        block_color color : 4;
    };

    static constexpr block_color color_of(const block_data& d) noexcept {
        return static_cast<block_color>(d.color);
    }

    // trusted memory layout:
    // [parent*][fit_mode][size_t][mutex][root*]
    static constexpr size_t allocator_parent_offset = 0;
    static constexpr size_t allocator_fit_offset =
        align_up(allocator_parent_offset + sizeof(std::pmr::memory_resource*), alignof(fit_mode));
    static constexpr size_t allocator_space_offset =
        align_up(allocator_fit_offset + sizeof(fit_mode), alignof(size_t));
    static constexpr size_t allocator_mutex_offset =
        align_up(allocator_space_offset + sizeof(size_t), alignof(std::mutex));
    static constexpr size_t allocator_root_offset =
        align_up(allocator_mutex_offset + sizeof(std::mutex), alignof(void*));

    static constexpr size_t allocator_metadata_size =
        align_up(allocator_root_offset + sizeof(void*), allocator_alignment);

    // block layout:
    // occupied: block_data + prev* + next* + parent*
    // free:     block_data + prev* + next* + parent* + left* + right*
    static constexpr size_t BLOCK_DATA_OFFSET = 0;
    static constexpr size_t PREV_OFFSET = align_up(sizeof(block_data), alignof(void*));
    static constexpr size_t NEXT_OFFSET = PREV_OFFSET + sizeof(void*);
    static constexpr size_t PARENT_OFFSET = NEXT_OFFSET + sizeof(void*);
    static constexpr size_t LEFT_OFFSET = PARENT_OFFSET + sizeof(void*);
    static constexpr size_t RIGHT_OFFSET = LEFT_OFFSET + sizeof(void*);

    static constexpr size_t occupied_block_metadata_size = PARENT_OFFSET + sizeof(void*);
    static constexpr size_t free_block_metadata_size = RIGHT_OFFSET + sizeof(void*);

    void* _trusted_memory{nullptr};

    std::pmr::memory_resource*& get_parent_resource() const noexcept;
    fit_mode& get_stored_fit_mode() const noexcept;
    size_t& get_stored_space_size() const noexcept;
    std::mutex& get_stored_mutex() const noexcept;
    void*& get_root_block() const noexcept;

    block_data& get_block_data(void* block) const noexcept;
    void*& get_block_parent(void* block) const noexcept;
    void*& get_block_next(void* block) const noexcept;
    void*& get_block_prev(void* block) const noexcept;
    void*& get_block_left(void* block) const noexcept;
    void*& get_block_right(void* block) const noexcept;

    size_t calculate_block_size(void* block) const noexcept;
    size_t get_allocatable_capacity(void* block) const noexcept;
    bool is_left_child(void* child, void* parent) const noexcept;

    void* search_first_fit(size_t size) const noexcept;
    void* search_best_fit(size_t size) const noexcept;
    void* search_worst_fit(size_t size) const noexcept;

    void rb_tree_insert(void* node) noexcept;
    void rb_insert_fixup(void* z) noexcept;
    void rb_tree_remove(void* z) noexcept;
    void rb_transplant(void* u, void* v) noexcept;
    void rb_delete_fixup(void* x, void* x_parent) noexcept;
    void rotate_left(void* x) noexcept;
    void rotate_right(void* x) noexcept;

public:
    ~allocator_red_black_tree() override;

    allocator_red_black_tree(const allocator_red_black_tree& other) = delete;
    allocator_red_black_tree& operator=(const allocator_red_black_tree& other) = delete;
    allocator_red_black_tree(allocator_red_black_tree&& other) noexcept; 
    allocator_red_black_tree& operator=(allocator_red_black_tree&& other) noexcept;

    explicit allocator_red_black_tree(
        size_t space_size,
        std::pmr::memory_resource* parent_allocator = nullptr,
        fit_mode allocate_fit_mode = fit_mode::first_fit);

private:
    [[nodiscard]] void* do_allocate_sm(size_t size) override;
    void do_deallocate_sm(void* at) override;
    bool do_is_equal(const std::pmr::memory_resource&) const noexcept override;
    std::vector<allocator_test_utils::block_info> get_blocks_info() const override;
    void set_fit_mode(fit_mode mode) override;
    std::vector<allocator_test_utils::block_info> get_blocks_info_inner() const override;

    class rb_iterator {
        void* _block_ptr;
        void* _trusted;
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = void*;
        using reference = void*&;
        using pointer = void**;
        using difference_type = ptrdiff_t;

        bool operator==(const rb_iterator&) const noexcept;
        bool operator!=(const rb_iterator&) const noexcept;
        rb_iterator& operator++() & noexcept;
        rb_iterator operator++(int);
        size_t size() const noexcept;
        void* operator*() const noexcept;
        bool occupied() const noexcept;
        rb_iterator();
        rb_iterator(void* trusted);
    };

    friend class rb_iterator;
    rb_iterator begin() const noexcept;
    rb_iterator end() const noexcept;
};

#endif