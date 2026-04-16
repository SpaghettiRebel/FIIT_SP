#include "../include/allocator_global_heap.h"

[[nodiscard]] void *allocator_global_heap::do_allocate_sm(size_t size) { return ::operator new(size); }

void allocator_global_heap::do_deallocate_sm(void *at) {
    if (at == nullptr) return;

    ::operator delete(at);
}

bool allocator_global_heap::do_is_equal(const std::pmr::memory_resource &other) const noexcept {
    auto const *other_ptr = dynamic_cast<allocator_global_heap const *>(&other);

    return other_ptr != nullptr;
}

allocator_global_heap::allocator_global_heap() = default;

allocator_global_heap::~allocator_global_heap() = default;

allocator_global_heap::allocator_global_heap(const allocator_global_heap &other) {}

allocator_global_heap::allocator_global_heap(allocator_global_heap &&other) noexcept {}

allocator_global_heap &allocator_global_heap::operator=(const allocator_global_heap &other) { return *this; }

allocator_global_heap &allocator_global_heap::operator=(allocator_global_heap &&other) noexcept { return *this; }
