#include "../include/allocator_red_black_tree.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <cstddef>


std::pmr::memory_resource*&
allocator_red_black_tree::get_parent_resource() const noexcept {
    return *reinterpret_cast<std::pmr::memory_resource**>(
        static_cast<std::byte*>(_trusted_memory) + allocator_parent_offset);
}

allocator_red_black_tree::fit_mode&
allocator_red_black_tree::get_stored_fit_mode() const noexcept {
    return *reinterpret_cast<fit_mode*>(
        static_cast<std::byte*>(_trusted_memory) + allocator_fit_offset);
}

size_t& allocator_red_black_tree::get_stored_space_size() const noexcept {
    return *reinterpret_cast<size_t*>(
        static_cast<std::byte*>(_trusted_memory) + allocator_space_offset);
}

std::mutex& allocator_red_black_tree::get_stored_mutex() const noexcept {
    return *reinterpret_cast<std::mutex*>(
        static_cast<std::byte*>(_trusted_memory) + allocator_mutex_offset);
}

void*& allocator_red_black_tree::get_root_block() const noexcept {
    return *reinterpret_cast<void**>(
        static_cast<std::byte*>(_trusted_memory) + allocator_root_offset);
}

allocator_red_black_tree::block_data&
allocator_red_black_tree::get_block_data(void* block) const noexcept {
    return *reinterpret_cast<block_data*>(static_cast<std::byte*>(block));
}

void*& allocator_red_black_tree::get_block_parent(void* block) const noexcept {
    return *reinterpret_cast<void**>(static_cast<std::byte*>(block) + PARENT_OFFSET);
}

void*& allocator_red_black_tree::get_block_next(void* block) const noexcept {
    return *reinterpret_cast<void**>(static_cast<std::byte*>(block) + NEXT_OFFSET);
}

void*& allocator_red_black_tree::get_block_prev(void* block) const noexcept {
    return *reinterpret_cast<void**>(static_cast<std::byte*>(block) + PREV_OFFSET);
}

void*& allocator_red_black_tree::get_block_left(void* block) const noexcept {
    return *reinterpret_cast<void**>(static_cast<std::byte*>(block) + LEFT_OFFSET);
}

void*& allocator_red_black_tree::get_block_right(void* block) const noexcept {
    return *reinterpret_cast<void**>(static_cast<std::byte*>(block) + RIGHT_OFFSET);
}

// --- FIXED SEARCHES + SIZE LOGIC ---

size_t allocator_red_black_tree::calculate_block_size(void* block) const noexcept {
    const auto* start = static_cast<const std::byte*>(block);
    const auto* end =
        static_cast<const std::byte*>(_trusted_memory) +
        allocator_metadata_size + get_stored_space_size();

    const auto* next = get_block_next(block)
        ? static_cast<const std::byte*>(get_block_next(block))
        : end;

    // ✅ ВСЕГДА occupied metadata
    return static_cast<size_t>(next - start - occupied_block_metadata_size);
}

size_t allocator_red_black_tree::get_allocatable_capacity(void* block) const noexcept {
    if (!block) return 0;

    size_t raw = calculate_block_size(block);

    if (get_block_data(block).occupied) {
        return raw;
    }

    const size_t overhead = free_block_metadata_size - occupied_block_metadata_size;
    return (raw > overhead) ? raw - overhead : 0;
}

bool allocator_red_black_tree::is_left_child(void* child, void* parent) const noexcept {
    return child == get_block_left(parent);
}

allocator_red_black_tree::~allocator_red_black_tree() {
    if (!_trusted_memory) return;

    get_stored_mutex().~mutex();

    const size_t total = allocator_metadata_size + get_stored_space_size();
    if (auto* parent = get_parent_resource()) {
        parent->deallocate(_trusted_memory, total, allocator_alignment);
    } else {
        ::operator delete(_trusted_memory, std::align_val_t(allocator_alignment));
    }
}

allocator_red_black_tree::allocator_red_black_tree(allocator_red_black_tree&& other) noexcept
    : _trusted_memory(std::exchange(other._trusted_memory, nullptr)) {}

allocator_red_black_tree&
allocator_red_black_tree::operator=(allocator_red_black_tree&& other) noexcept {
    if (this != &other) {
        this->~allocator_red_black_tree();
        _trusted_memory = std::exchange(other._trusted_memory, nullptr);
    }
    return *this;
}

allocator_red_black_tree::allocator_red_black_tree(const allocator_red_black_tree& other) {
    if (!other._trusted_memory) {
        _trusted_memory = nullptr;
        return;
    }
    throw std::logic_error("copy construction not supported");
}

allocator_red_black_tree&
allocator_red_black_tree::operator=(const allocator_red_black_tree& other) {
    if (this != &other) {
        this->~allocator_red_black_tree();
        new (this) allocator_red_black_tree(other);
    }
    return *this;
}

allocator_red_black_tree::allocator_red_black_tree(
    size_t space_size,
    std::pmr::memory_resource* parent,
    fit_mode mode)
{
    if (space_size < free_block_metadata_size)
        throw std::logic_error("space too small");

    const size_t total = allocator_metadata_size + space_size;

    _trusted_memory = parent
        ? parent->allocate(total, allocator_alignment)
        : ::operator new(total, std::align_val_t(allocator_alignment));

    get_parent_resource() = parent;
    get_stored_fit_mode() = mode;
    get_stored_space_size() = space_size;
    new (&get_stored_mutex()) std::mutex();

    void* first = static_cast<std::byte*>(_trusted_memory) + allocator_metadata_size;
    get_root_block() = first;

    auto& d = get_block_data(first);
    d.occupied = 0;
    d.color = block_color::BLACK;

    get_block_parent(first) = nullptr;
    get_block_next(first) = nullptr;
    get_block_prev(first) = nullptr;
    get_block_left(first) = nullptr;
    get_block_right(first) = nullptr;
}

[[nodiscard]] void* allocator_red_black_tree::do_allocate_sm(size_t size) {
    std::lock_guard lock{get_stored_mutex()};
    if (size == 0) size = 1;

    const size_t used_size = align_up(size, alignof(void*));

    void* block = nullptr;
    switch (get_stored_fit_mode()) {
        case fit_mode::first_fit:
            block = search_first_fit(used_size);
            break;
        case fit_mode::the_best_fit:
            block = search_best_fit(used_size);
            break;
        case fit_mode::the_worst_fit:
            block = search_worst_fit(used_size);
            break;
    }

    if (!block) {
        throw std::bad_alloc();
    }

    rb_tree_remove(block);

    const size_t logical_free_size = calculate_block_size(block);
    if (logical_free_size >= used_size + free_block_metadata_size) {
        void* new_free = static_cast<std::byte*>(block) + occupied_block_metadata_size + used_size;

        auto& nd = get_block_data(new_free);
        nd.occupied = 0;
        nd.color = block_color::RED;

        get_block_next(new_free) = get_block_next(block);
        get_block_prev(new_free) = block;

        if (get_block_next(block)) {
            get_block_prev(get_block_next(block)) = new_free;
        }

        get_block_next(block) = new_free;

        get_block_parent(new_free) = nullptr;
        get_block_left(new_free) = nullptr;
        get_block_right(new_free) = nullptr;

        rb_tree_insert(new_free);
    }

    auto& d = get_block_data(block);
    d.occupied = 1;
    d.color = block_color::BLACK;
    get_block_parent(block) = _trusted_memory;

    return static_cast<std::byte*>(block) + occupied_block_metadata_size;
}

void allocator_red_black_tree::do_deallocate_sm(void* at) {
    std::lock_guard lock{get_stored_mutex()};

    if (!at) {
        throw std::logic_error("allocator_red_black_tree: null deallocation");
    }

    void* block = static_cast<std::byte*>(at) - occupied_block_metadata_size;

    auto* trusted_start = static_cast<std::byte*>(_trusted_memory) + allocator_metadata_size;
    auto* trusted_end = trusted_start + get_stored_space_size();
    auto* block_ptr = static_cast<std::byte*>(block);

    if (block_ptr < trusted_start || block_ptr >= trusted_end) {
        throw std::logic_error("allocator_red_black_tree: block out of bounds");
    }

    auto& d = get_block_data(block);
    if (!d.occupied || get_block_parent(block) != _trusted_memory) {
        throw std::logic_error("allocator_red_black_tree: invalid deallocation");
    }

    d.occupied = 0;
    get_block_parent(block) = nullptr;

    void* prev = get_block_prev(block);
    if (prev && !get_block_data(prev).occupied) {
        rb_tree_remove(prev);

        get_block_next(prev) = get_block_next(block);
        if (get_block_next(block)) {
            get_block_prev(get_block_next(block)) = prev;
        }

        block = prev;
    }

    void* next = get_block_next(block);
    if (next && !get_block_data(next).occupied) {
        rb_tree_remove(next);

        get_block_next(block) = get_block_next(next);
        if (get_block_next(block)) {
            get_block_prev(get_block_next(block)) = block;
        }
    }

    rb_tree_insert(block);
}

bool allocator_red_black_tree::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

void allocator_red_black_tree::set_fit_mode(fit_mode mode) {
    std::lock_guard lock{get_stored_mutex()};
    get_stored_fit_mode() = mode;
}

void* allocator_red_black_tree::search_first_fit(size_t size) const noexcept {
    void* cur = static_cast<std::byte*>(_trusted_memory) + allocator_metadata_size;
    while (cur) {
        if (!get_block_data(cur).occupied &&
            get_allocatable_capacity(cur) >= size)
            return cur;
        cur = get_block_next(cur);
    }
    return nullptr;
}

void* allocator_red_black_tree::search_best_fit(size_t size) const noexcept {
    void* cur = get_root_block();
    void* best = nullptr;
    size_t best_cap = std::numeric_limits<size_t>::max();

    while (cur) {
        size_t cap = get_allocatable_capacity(cur);

        if (cap >= size) {
            if (cap < best_cap) {
                best = cur;
                best_cap = cap;
            }
            cur = get_block_left(cur);   // ищем меньше
        } else {
            cur = get_block_right(cur);
        }
    }

    return best;
}

void* allocator_red_black_tree::search_worst_fit(size_t size) const noexcept {
    void* cur = get_root_block();
    void* worst = nullptr;
    size_t worst_cap = 0;

    while (cur) {
        size_t cap = get_allocatable_capacity(cur);

        if (cap >= size) {
            if (cap > worst_cap) {
                worst = cur;
                worst_cap = cap;
            }
            cur = get_block_right(cur); // ищем больше
        } else {
            cur = get_block_right(cur);
        }
    }

    return worst;
}


void allocator_red_black_tree::rb_tree_insert(void* z) noexcept {
    void* y = nullptr;
    void* x = get_root_block();

    while (x) {
        y = x;
        x = (get_allocatable_capacity(z) < get_allocatable_capacity(x))
            ? get_block_left(x)
            : get_block_right(x);
    }

    get_block_parent(z) = y;
    get_block_left(z) = nullptr;
    get_block_right(z) = nullptr;

    get_block_data(z).color = block_color::RED;

    if (!y) get_root_block() = z;
    else if (get_allocatable_capacity(z) < get_allocatable_capacity(y))
        get_block_left(y) = z;
    else
        get_block_right(y) = z;

    rb_insert_fixup(z);
}

void allocator_red_black_tree::rb_insert_fixup(void* z) noexcept {
    while (z != get_root_block() &&
           color_of(get_block_data(get_block_parent(z))) == block_color::RED) {

        void* p = get_block_parent(z);
        void* g = get_block_parent(p);

        if (p == get_block_left(g)) {
            void* u = get_block_right(g);

            if (u && color_of(get_block_data(u)) == block_color::RED) {
                get_block_data(p).color = block_color::BLACK;
                get_block_data(u).color = block_color::BLACK;
                get_block_data(g).color = block_color::RED;
                z = g;
            } else {
                if (z == get_block_right(p)) {
                    z = p;
                    rotate_left(z);
                }
                get_block_data(get_block_parent(z)).color = block_color::BLACK;
                get_block_data(get_block_parent(get_block_parent(z))).color = block_color::RED;
                rotate_right(get_block_parent(get_block_parent(z)));
            }
        } else {
            void* u = get_block_left(g);

            if (u && color_of(get_block_data(u)) == block_color::RED) {
                get_block_data(p).color = block_color::BLACK;
                get_block_data(u).color = block_color::BLACK;
                get_block_data(g).color = block_color::RED;
                z = g;
            } else {
                if (z == get_block_left(p)) {
                    z = p;
                    rotate_right(z);
                }
                get_block_data(get_block_parent(z)).color = block_color::BLACK;
                get_block_data(get_block_parent(get_block_parent(z))).color = block_color::RED;
                rotate_left(get_block_parent(get_block_parent(z)));
            }
        }
    }

    get_block_data(get_root_block()).color = block_color::BLACK;
}

void allocator_red_black_tree::rb_tree_remove(void* z) noexcept {
    auto color_of = [this](void* node) noexcept -> block_color {
        return node ? static_cast<block_color>(get_block_data(node).color) : block_color::BLACK;
    };

    auto set_color = [this](void* node, block_color color) noexcept {
        if (node) {
            get_block_data(node).color = color;
        }
    };

    void* y = z;
    void* x = nullptr;
    void* x_parent = nullptr;
    const block_color y_original_color = color_of(y);

    if (!get_block_left(z)) {
        x = get_block_right(z);
        x_parent = get_block_parent(z);
        rb_transplant(z, get_block_right(z));
        if (x) get_block_parent(x) = x_parent;
    } else if (!get_block_right(z)) {
        x = get_block_left(z);
        x_parent = get_block_parent(z);
        rb_transplant(z, get_block_left(z));
        if (x) get_block_parent(x) = x_parent;
    } else {
        y = get_block_right(z);
        while (get_block_left(y)) {
            y = get_block_left(y);
        }

        const block_color y_color = color_of(y);
        x = get_block_right(y);

        if (get_block_parent(y) == z) {
            x_parent = y;
            if (x) get_block_parent(x) = y;
        } else {
            x_parent = get_block_parent(y);
            rb_transplant(y, get_block_right(y));
            get_block_right(y) = get_block_right(z);
            if (get_block_right(y)) {
                get_block_parent(get_block_right(y)) = y;
            }
            if (x) get_block_parent(x) = x_parent;
        }

        rb_transplant(z, y);
        get_block_left(y) = get_block_left(z);
        if (get_block_left(y)) {
            get_block_parent(get_block_left(y)) = y;
        }
        get_block_data(y).color = get_block_data(z).color;

        if (y_color == block_color::BLACK) {
            rb_delete_fixup(x, x_parent);
        }
        return;
    }

    if (y_original_color == block_color::BLACK) {
        rb_delete_fixup(x, x_parent);
    }
}

void allocator_red_black_tree::rb_transplant(void* u, void* v) noexcept {
    if (!get_block_parent(u)) {
        get_root_block() = v;
    } else if (u == get_block_left(get_block_parent(u))) {
        get_block_left(get_block_parent(u)) = v;
    } else {
        get_block_right(get_block_parent(u)) = v;
    }
    if (v) {
        get_block_parent(v) = get_block_parent(u);
    }
}

void allocator_red_black_tree::rb_delete_fixup(void* x, void* x_parent) noexcept {
    auto color_of = [this](void* node) noexcept -> block_color {
        return node ? static_cast<block_color>(get_block_data(node).color) : block_color::BLACK;
    };

    auto set_color = [this](void* node, block_color color) noexcept {
        if (node) {
            get_block_data(node).color = color;
        }
    };

    while (x != get_root_block() && color_of(x) == block_color::BLACK) {
        if (!x_parent) break;

        if (x == get_block_left(x_parent)) {
            void* w = get_block_right(x_parent);

            if (color_of(w) == block_color::RED) {
                set_color(w, block_color::BLACK);
                set_color(x_parent, block_color::RED);
                rotate_left(x_parent);
                w = get_block_right(x_parent);
            }

            if (color_of(get_block_left(w)) == block_color::BLACK &&
                color_of(get_block_right(w)) == block_color::BLACK) {
                set_color(w, block_color::RED);
                x = x_parent;
                x_parent = get_block_parent(x_parent);
            } else {
                if (color_of(get_block_right(w)) == block_color::BLACK) {
                    set_color(get_block_left(w), block_color::BLACK);
                    set_color(w, block_color::RED);
                    rotate_right(w);
                    w = get_block_right(x_parent);
                }

                set_color(w, color_of(x_parent));
                set_color(x_parent, block_color::BLACK);
                set_color(get_block_right(w), block_color::BLACK);
                rotate_left(x_parent);
                x = get_root_block();
                break;
            }
        } else {
            void* w = get_block_left(x_parent);

            if (color_of(w) == block_color::RED) {
                set_color(w, block_color::BLACK);
                set_color(x_parent, block_color::RED);
                rotate_right(x_parent);
                w = get_block_left(x_parent);
            }

            if (color_of(get_block_right(w)) == block_color::BLACK &&
                color_of(get_block_left(w)) == block_color::BLACK) {
                set_color(w, block_color::RED);
                x = x_parent;
                x_parent = get_block_parent(x_parent);
            } else {
                if (color_of(get_block_left(w)) == block_color::BLACK) {
                    set_color(get_block_right(w), block_color::BLACK);
                    set_color(w, block_color::RED);
                    rotate_left(w);
                    w = get_block_left(x_parent);
                }

                set_color(w, color_of(x_parent));
                set_color(x_parent, block_color::BLACK);
                set_color(get_block_left(w), block_color::BLACK);
                rotate_right(x_parent);
                x = get_root_block();
                break;
            }
        }
    }

    if (x) {
        set_color(x, block_color::BLACK);
    }
    if (get_root_block()) {
        set_color(get_root_block(), block_color::BLACK);
    }
}

void allocator_red_black_tree::rotate_left(void* x) noexcept {
    if (!x) return;
    void* y = get_block_right(x);
    if (!y) return;

    get_block_right(x) = get_block_left(y);
    if (get_block_left(y)) {
        get_block_parent(get_block_left(y)) = x;
    }

    get_block_parent(y) = get_block_parent(x);

    if (!get_block_parent(x)) {
        get_root_block() = y;
    } else if (x == get_block_left(get_block_parent(x))) {
        get_block_left(get_block_parent(x)) = y;
    } else {
        get_block_right(get_block_parent(x)) = y;
    }

    get_block_left(y) = x;
    get_block_parent(x) = y;
}

void allocator_red_black_tree::rotate_right(void* x) noexcept {
    if (!x) return;
    void* y = get_block_left(x);
    if (!y) return;

    get_block_left(x) = get_block_right(y);
    if (get_block_right(y)) {
        get_block_parent(get_block_right(y)) = x;
    }

    get_block_parent(y) = get_block_parent(x);

    if (!get_block_parent(x)) {
        get_root_block() = y;
    } else if (x == get_block_right(get_block_parent(x))) {
        get_block_right(get_block_parent(x)) = y;
    } else {
        get_block_left(get_block_parent(x)) = y;
    }

    get_block_right(y) = x;
    get_block_parent(x) = y;
}

std::vector<allocator_test_utils::block_info>
allocator_red_black_tree::get_blocks_info() const {
    std::lock_guard lock{get_stored_mutex()};
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info>
allocator_red_black_tree::get_blocks_info_inner() const {
    std::vector<allocator_test_utils::block_info> result;
    result.reserve(64);
    for (auto it = begin(), end_it = end(); it != end_it; ++it) {
        result.push_back({it.size(), it.occupied()});
    }
    return result;
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::begin() const noexcept {
    return rb_iterator{_trusted_memory};
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::end() const noexcept {
    return rb_iterator{};
}

allocator_red_black_tree::rb_iterator::rb_iterator()
    : _block_ptr(nullptr), _trusted(nullptr) {}

allocator_red_black_tree::rb_iterator::rb_iterator(void* trusted)
    : _trusted(trusted) {
    _block_ptr = trusted
        ? static_cast<std::byte*>(trusted) + allocator_red_black_tree::allocator_metadata_size
        : nullptr;
}

bool allocator_red_black_tree::rb_iterator::operator==(const rb_iterator& other) const noexcept {
    return _block_ptr == other._block_ptr;
}

bool allocator_red_black_tree::rb_iterator::operator!=(const rb_iterator& other) const noexcept {
    return !(*this == other);
}

allocator_red_black_tree::rb_iterator&
allocator_red_black_tree::rb_iterator::operator++() & noexcept {
    if (_block_ptr && _trusted) {
        auto* self = static_cast<const allocator_red_black_tree*>(_trusted);
        _block_ptr = self->get_block_next(_block_ptr);
    }
    return *this;
}

allocator_red_black_tree::rb_iterator
allocator_red_black_tree::rb_iterator::operator++(int) {
    rb_iterator tmp = *this;
    ++(*this);
    return tmp;
}

size_t allocator_red_black_tree::rb_iterator::size() const noexcept {
    if (!_block_ptr || !_trusted) return 0;
    auto* self = static_cast<const allocator_red_black_tree*>(_trusted);
    return self->calculate_block_size(_block_ptr);
}

void* allocator_red_black_tree::rb_iterator::operator*() const noexcept {
    return _block_ptr;
}

bool allocator_red_black_tree::rb_iterator::occupied() const noexcept {
    if (!_block_ptr || !_trusted) return false;
    auto* self = static_cast<const allocator_red_black_tree*>(_trusted);
    return self->get_block_data(_block_ptr).occupied != 0;
}