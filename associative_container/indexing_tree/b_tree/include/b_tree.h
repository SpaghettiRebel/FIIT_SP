#ifndef SYS_PROG_B_TREE_H
#define SYS_PROG_B_TREE_H

#include <associative_container.h>
#include <pp_allocator.h>

#include <algorithm>
#include <boost/container/static_vector.hpp>
#include <initializer_list>
#include <iterator>
#include <stack>
#include <stdexcept>
#include <utility>

template <typename tkey, typename tvalue, comparator<tkey> compare = std::less<tkey>, std::size_t t = 5>
class B_tree final : private compare  // EBCO
{
public:
    using tree_data_type = std::pair<tkey, tvalue>;
    using tree_data_type_const = std::pair<const tkey, tvalue>;
    using value_type = tree_data_type_const;

    class exception : public std::exception {
    private:
        std::string _message;

    public:
        explicit exception(std::string message) : _message(std::move(message)) {}
        [[nodiscard]] const char* what() const noexcept override { return _message.c_str(); }
    };

    class key_not_found : public exception {
    public:
        key_not_found() : exception("Key not found") {}
    };

    class duplicate_key : public exception {
    public:
        duplicate_key() : exception("Duplicate key") {}
    };

private:
    static constexpr const size_t minimum_keys_in_node = t - 1;
    static constexpr const size_t maximum_keys_in_node = 2 * t - 1;

    // region comparators declaration

    inline bool compare_keys(const tkey& lhs, const tkey& rhs) const;
    inline bool compare_pairs(const tree_data_type& lhs, const tree_data_type& rhs) const;

    // endregion comparators declaration

    struct btree_node {
        boost::container::static_vector<tree_data_type, maximum_keys_in_node + 1> _keys;
        boost::container::static_vector<btree_node*, maximum_keys_in_node + 2> _pointers;
        btree_node() noexcept;
        bool is_leaf() const { return _pointers.empty(); }
    };

    pp_allocator<value_type> _allocator;
    btree_node* _root;
    size_t _size;

    pp_allocator<value_type> get_allocator() const noexcept;

    // Private helpers
    pp_allocator<btree_node> get_node_allocator() const noexcept {
        return pp_allocator<btree_node>(_allocator.resource());
    }

    btree_node* create_node() {
        auto alloc = get_node_allocator();
        
        btree_node* node = alloc.allocate(1);
        if (!node)
            throw std::bad_alloc();
        new (node) btree_node();

        return node;
    }

    void destroy_node(btree_node* node) {
        if (!node)
            return;
        node->~btree_node();
        get_node_allocator().deallocate(node, 1);
    }

    void destroy_recursive(btree_node* node) {
        if (!node)
            return;
        for (auto child : node->_pointers) {
            destroy_recursive(child);
        }
        destroy_node(node);
    }

    btree_node* copy_recursive(btree_node* other_node) {
        if (!other_node) return nullptr;
        btree_node* node = create_node();
        try {
            node->_keys = other_node->_keys;
            for (auto child : other_node->_pointers) {
                node->_pointers.push_back(copy_recursive(child));
            }
        } catch (...) {
            destroy_recursive(node);
            throw;
        }
        return node;
    }

    size_t find_key_index(const btree_node* node, const tkey& key) const {
        auto it = std::lower_bound(node->_keys.begin(), node->_keys.end(), key,
            [this](const tree_data_type& pair, const tkey& k) { return compare_keys(pair.first, k); });
        return std::distance(node->_keys.begin(), it);
    }

    bool insert_recursive(
        btree_node* node, const tkey& key, const tvalue& value, std::vector<btree_node*>& path_nodes) {
        size_t idx = find_key_index(node, key);
        if (idx < node->_keys.size() && !compare_keys(key, node->_keys[idx].first) &&
            !compare_keys(node->_keys[idx].first, key))
            return false;
        if (node->is_leaf()) {
            node->_keys.insert(node->_keys.begin() + idx, {key, value});
        } else {
            path_nodes.push_back(node);
            if (!insert_recursive(node->_pointers[idx], key, value, path_nodes)) {
                path_nodes.pop_back();
                return false;
            }
            path_nodes.pop_back();
        }
        if (node->_keys.size() > maximum_keys_in_node) {
            split_node(node, path_nodes.empty() ? nullptr : path_nodes.back());
        }
        return true;
    }

    void split_node(btree_node* node, btree_node* parent) {
        size_t mid = node->_keys.size() / 2;
        btree_node* right = create_node();

        for (size_t i = mid + 1; i < node->_keys.size(); ++i) 
            right->_keys.push_back(std::move(node->_keys[i]));

        if (!node->is_leaf()) {
            for (size_t i = mid + 1; i < node->_pointers.size(); ++i) right->_pointers.push_back(node->_pointers[i]);
        }

        tree_data_type mid_kv = std::move(node->_keys[mid]);
        node->_keys.erase(node->_keys.begin() + mid, node->_keys.end());

        if (!node->is_leaf())
            node->_pointers.erase(node->_pointers.begin() + mid + 1, node->_pointers.end());

        if (!parent) {
            btree_node* new_root = create_node();
            new_root->_keys.push_back(std::move(mid_kv));
            new_root->_pointers.push_back(node);
            new_root->_pointers.push_back(right);
            _root = new_root;
        } else {
            size_t idx = find_key_index(parent, mid_kv.first);
            parent->_keys.insert(parent->_keys.begin() + idx, std::move(mid_kv));
            parent->_pointers.insert(parent->_pointers.begin() + idx + 1, right);
        }
    }

    bool erase_recursive(btree_node* node, const tkey& key, btree_node* parent, size_t idx_in_parent) {
        size_t idx = find_key_index(node, key);
        bool found = (idx < node->_keys.size() && !compare_keys(key, node->_keys[idx].first) &&
                      !compare_keys(node->_keys[idx].first, key));

        if (found) {
            if (node->is_leaf()) {
                node->_keys.erase(node->_keys.begin() + idx);
            } else {
                btree_node* pred = node->_pointers[idx];

                while (!pred->is_leaf())
                    pred = pred->_pointers.back();

                node->_keys[idx] = pred->_keys.back();
                
                erase_recursive(node->_pointers[idx], node->_keys[idx].first, node, idx);
            }
        } else {
            if (node->is_leaf())
                return false;
            if (!erase_recursive(node->_pointers[idx], key, node, idx))
                return false;
        }

        if (node != _root && node->_keys.size() < minimum_keys_in_node)
            fix_underflow(node, parent, idx_in_parent);
        
            return true;
    }

    void fix_underflow(btree_node* node, btree_node* parent, size_t idx) {
        if (idx > 0 && parent->_pointers[idx - 1]->_keys.size() > minimum_keys_in_node) {
            btree_node* left = parent->_pointers[idx - 1];
            node->_keys.insert(node->_keys.begin(), std::move(parent->_keys[idx - 1]));

            parent->_keys[idx - 1] = std::move(left->_keys.back());
            left->_keys.pop_back();
            
            if (!left->is_leaf()) {
                node->_pointers.insert(node->_pointers.begin(), left->_pointers.back());
                left->_pointers.pop_back();
            }
        } else if (idx < parent->_keys.size() && parent->_pointers[idx + 1]->_keys.size() > minimum_keys_in_node) {
            btree_node* right = parent->_pointers[idx + 1];
            node->_keys.push_back(std::move(parent->_keys[idx]));
            parent->_keys[idx] = std::move(right->_keys.front());
            right->_keys.erase(right->_keys.begin());

            if (!right->is_leaf()) {
                node->_pointers.push_back(right->_pointers.front());
                right->_pointers.erase(right->_pointers.begin());
            }
        } else {
            if (idx > 0)
                merge_nodes(parent, idx - 1);
            else
                merge_nodes(parent, idx);
        }
    }

    void merge_nodes(btree_node* parent, size_t idx) {
        btree_node* left = parent->_pointers[idx];
        btree_node* right = parent->_pointers[idx + 1];
        left->_keys.push_back(std::move(parent->_keys[idx]));
        
        for (auto& k : right->_keys)
            left->_keys.push_back(std::move(k));
        
        for (auto p : right->_pointers)
            left->_pointers.push_back(p);
        
        parent->_keys.erase(parent->_keys.begin() + idx);
        parent->_pointers.erase(parent->_pointers.begin() + idx + 1);
        right->_pointers.clear();
        destroy_node(right);
    }

public:
    // region constructors declaration

    explicit B_tree(const compare& cmp = compare(), pp_allocator<value_type> = pp_allocator<value_type>());

    explicit B_tree(pp_allocator<value_type> alloc, const compare& comp = compare());

    template <input_iterator_for_pair<tkey, tvalue> iterator>
    explicit B_tree(iterator begin, iterator end, const compare& cmp = compare(),
        pp_allocator<value_type> = pp_allocator<value_type>());

    B_tree(std::initializer_list<std::pair<tkey, tvalue>> data, const compare& cmp = compare(),
        pp_allocator<value_type> = pp_allocator<value_type>());

    // endregion constructors declaration

    // region five declaration

    B_tree(const B_tree& other);

    B_tree(B_tree&& other) noexcept;

    B_tree& operator=(const B_tree& other);

    B_tree& operator=(B_tree&& other) noexcept;

    ~B_tree() noexcept;

    // endregion five declaration

    // region iterators declaration

    class btree_iterator;
    class btree_reverse_iterator;
    class btree_const_iterator;
    class btree_const_reverse_iterator;

    class btree_iterator final {
        std::stack<std::pair<btree_node**, size_t>> _path;
        size_t _index;

    public:
        using value_type = tree_data_type_const;
        using reference = value_type&;
        using pointer = value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = btree_iterator;

        friend class B_tree;
        friend class btree_reverse_iterator;
        friend class btree_const_iterator;
        friend class btree_const_reverse_iterator;

        reference operator*() const noexcept;
        pointer operator->() const noexcept;

        self& operator++();
        self operator++(int);

        self& operator--();
        self operator--(int);

        bool operator==(const self& other) const noexcept;
        bool operator!=(const self& other) const noexcept;

        size_t depth() const noexcept;
        size_t current_node_keys_count() const noexcept;
        bool is_terminate_node() const noexcept;
        size_t index() const noexcept;

        explicit btree_iterator(
            const std::stack<std::pair<btree_node**, size_t>>& path = std::stack<std::pair<btree_node**, size_t>>(),
            size_t index = 0);
    };

    class btree_const_iterator final {
        std::stack<std::pair<btree_node* const*, size_t>> _path;
        size_t _index;

    public:
        using value_type = tree_data_type_const;
        using reference = const value_type&;
        using pointer = const value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = btree_const_iterator;

        friend class B_tree;
        friend class btree_reverse_iterator;
        friend class btree_iterator;
        friend class btree_const_reverse_iterator;

        btree_const_iterator(const btree_iterator& it) noexcept;

        reference operator*() const noexcept;
        pointer operator->() const noexcept;

        self& operator++();
        self operator++(int);

        self& operator--();
        self operator--(int);

        bool operator==(const self& other) const noexcept;
        bool operator!=(const self& other) const noexcept;

        size_t depth() const noexcept;
        size_t current_node_keys_count() const noexcept;
        bool is_terminate_node() const noexcept;
        size_t index() const noexcept;

        explicit btree_const_iterator(const std::stack<std::pair<btree_node* const*, size_t>>& path =
                                          std::stack<std::pair<btree_node* const*, size_t>>(),
            size_t index = 0);
    };

    class btree_reverse_iterator final {
        std::stack<std::pair<btree_node**, size_t>> _path;
        size_t _index;

    public:
        using value_type = tree_data_type_const;
        using reference = value_type&;
        using pointer = value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = btree_reverse_iterator;

        friend class B_tree;
        friend class btree_iterator;
        friend class btree_const_iterator;
        friend class btree_const_reverse_iterator;

        btree_reverse_iterator(const btree_iterator& it) noexcept;
        operator btree_iterator() const noexcept;

        reference operator*() const noexcept;
        pointer operator->() const noexcept;

        self& operator++();
        self operator++(int);

        self& operator--();
        self operator--(int);

        bool operator==(const self& other) const noexcept;
        bool operator!=(const self& other) const noexcept;

        size_t depth() const noexcept;
        size_t current_node_keys_count() const noexcept;
        bool is_terminate_node() const noexcept;
        size_t index() const noexcept;

        explicit btree_reverse_iterator(
            const std::stack<std::pair<btree_node**, size_t>>& path = std::stack<std::pair<btree_node**, size_t>>(),
            size_t index = 0);
    };

    class btree_const_reverse_iterator final {
        std::stack<std::pair<btree_node* const*, size_t>> _path;
        size_t _index;

    public:
        using value_type = tree_data_type_const;
        using reference = const value_type&;
        using pointer = const value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = btree_const_reverse_iterator;

        friend class B_tree;
        friend class btree_reverse_iterator;
        friend class btree_const_iterator;
        friend class btree_iterator;

        btree_const_reverse_iterator(const btree_reverse_iterator& it) noexcept;
        operator btree_const_iterator() const noexcept;

        reference operator*() const noexcept;
        pointer operator->() const noexcept;

        self& operator++();
        self operator++(int);

        self& operator--();
        self operator--(int);

        bool operator==(const self& other) const noexcept;
        bool operator!=(const self& other) const noexcept;

        size_t depth() const noexcept;
        size_t current_node_keys_count() const noexcept;
        bool is_terminate_node() const noexcept;
        size_t index() const noexcept;

        explicit btree_const_reverse_iterator(const std::stack<std::pair<btree_node* const*, size_t>>& path =
                                                  std::stack<std::pair<btree_node* const*, size_t>>(),
            size_t index = 0);
    };

    friend class btree_iterator;
    friend class btree_const_iterator;
    friend class btree_reverse_iterator;
    friend class btree_const_reverse_iterator;

    // endregion iterators declaration

    // region element access declaration

    /*
     * Returns a reference to the mapped value of the element with specified key. If no such element exists, an
     * exception of type std::out_of_range is thrown.
     */
    tvalue& at(const tkey&);
    const tvalue& at(const tkey&) const;

    /*
     * If key not exists, makes default initialization of value
     */
    tvalue& operator[](const tkey& key);
    tvalue& operator[](tkey&& key);

    // endregion element access declaration
    // region iterator begins declaration

    btree_iterator begin();
    btree_iterator end();

    btree_const_iterator begin() const;
    btree_const_iterator end() const;

    btree_const_iterator cbegin() const;
    btree_const_iterator cend() const;

    btree_reverse_iterator rbegin();
    btree_reverse_iterator rend();

    btree_const_reverse_iterator rbegin() const;
    btree_const_reverse_iterator rend() const;

    btree_const_reverse_iterator crbegin() const;
    btree_const_reverse_iterator crend() const;

    // endregion iterator begins declaration

    // region lookup declaration

    size_t size() const noexcept;
    bool empty() const noexcept;

    /*
     * Returns end() if not exist
     */

    btree_iterator find(const tkey& key);
    btree_const_iterator find(const tkey& key) const;

    btree_iterator lower_bound(const tkey& key);
    btree_const_iterator lower_bound(const tkey& key) const;

    btree_iterator upper_bound(const tkey& key);
    btree_const_iterator upper_bound(const tkey& key) const;

    bool contains(const tkey& key) const;

    // endregion lookup declaration

    // region modifiers declaration

    void clear() noexcept;

    /*
     * Does nothing if key exists, delegates to emplace.
     * Second return value is true, when inserted
     */
    std::pair<btree_iterator, bool> insert(const tree_data_type& data);
    std::pair<btree_iterator, bool> insert(tree_data_type&& data);

    template <typename... Args>
    std::pair<btree_iterator, bool> emplace(Args&&... args);

    /*
     * Updates value if key exists, delegates to emplace.
     */
    btree_iterator insert_or_assign(const tree_data_type& data);
    btree_iterator insert_or_assign(tree_data_type&& data);

    template <typename... Args>
    btree_iterator emplace_or_assign(Args&&... args);

    /*
     * Return iterator to node next ro removed or end() if key not exists
     */
    btree_iterator erase(btree_iterator pos);
    btree_iterator erase(btree_const_iterator pos);

    btree_iterator erase(btree_iterator beg, btree_iterator en);
    btree_iterator erase(btree_const_iterator beg, btree_const_iterator en);

    btree_iterator erase(const tkey& key);

    // endregion modifiers declaration
};

template <std::input_iterator iterator,
    comparator<typename std::iterator_traits<iterator>::value_type::first_type> compare =
        std::less<typename std::iterator_traits<iterator>::value_type::first_type>,
    std::size_t t = 5, typename U>
B_tree(iterator begin, iterator end, const compare& cmp = compare(), pp_allocator<U> = pp_allocator<U>())
    -> B_tree<typename std::iterator_traits<iterator>::value_type::first_type,
        typename std::iterator_traits<iterator>::value_type::second_type, compare, t>;

template <typename tkey, typename tvalue, comparator<tkey> compare = std::less<tkey>, std::size_t t = 5, typename U>
B_tree(std::initializer_list<std::pair<tkey, tvalue>> data, const compare& cmp = compare(),
    pp_allocator<U> = pp_allocator<U>()) -> B_tree<tkey, tvalue, compare, t>;

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::compare_pairs(
    const B_tree::tree_data_type& lhs, const B_tree::tree_data_type& rhs) const {
    return compare_keys(lhs.first, rhs.first);
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::compare_keys(const tkey& lhs, const tkey& rhs) const {
    return compare::operator()(lhs, rhs);
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_node::btree_node() noexcept {
    // No-op, vectors are inline.
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
pp_allocator<typename B_tree<tkey, tvalue, compare, t>::value_type> B_tree<tkey, tvalue, compare, t>::get_allocator()
    const noexcept {
    return _allocator;
}

// region constructors implementation

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::B_tree(const compare& cmp, pp_allocator<value_type> alloc)
    : compare(cmp), _allocator(alloc), _root(nullptr), _size(0) {}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::B_tree(pp_allocator<value_type> alloc, const compare& comp)
    : compare(comp), _allocator(alloc), _root(nullptr), _size(0) {}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
template <input_iterator_for_pair<tkey, tvalue> iterator>
B_tree<tkey, tvalue, compare, t>::B_tree(
    iterator begin, iterator end, const compare& cmp, pp_allocator<value_type> alloc)
    : compare(cmp), _allocator(alloc), _root(nullptr), _size(0) {
    for (auto it = begin; it != end; ++it) insert(*it);
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::B_tree(
    std::initializer_list<std::pair<tkey, tvalue>> data, const compare& cmp, pp_allocator<value_type> alloc)
    : compare(cmp), _allocator(alloc), _root(nullptr), _size(0) {
    for (const auto& item : data) insert(item);
}

// endregion constructors implementation

// region five implementation

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::~B_tree() noexcept {
    clear();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::B_tree(const B_tree& other)
    : compare(static_cast<const compare&>(other)), _allocator(other._allocator), _size(other._size) {
    _root = copy_recursive(other._root);
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>& B_tree<tkey, tvalue, compare, t>::operator=(const B_tree& other) {
    if (this != &other) {
        clear();
        compare::operator=(static_cast<const compare&>(other));
        _allocator = other._allocator;
        _root = copy_recursive(other._root);
        _size = other._size;
    }
    return *this;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::B_tree(B_tree&& other) noexcept
    : compare(std::move(static_cast<compare&>(other))),
      _allocator(std::move(other._allocator)),
      _root(other._root),
      _size(other._size) {
    other._root = nullptr;
    other._size = 0;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>& B_tree<tkey, tvalue, compare, t>::operator=(B_tree&& other) noexcept {
    if (this != &other) {
        clear();
        compare::operator=(std::move(static_cast<compare&>(other)));
        _allocator = std::move(other._allocator);
        _root = other._root;
        _size = other._size;
        other._root = nullptr;
        other._size = 0;
    }
    return *this;
}

// endregion five implementation

// region iterators implementation

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_iterator::btree_iterator(
    const std::stack<std::pair<btree_node**, size_t>>& path, size_t index)
    : _path(path), _index(index) {}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator::reference
B_tree<tkey, tvalue, compare, t>::btree_iterator::operator*() const noexcept {
    return reinterpret_cast<reference>((*_path.top().first)->_keys[_index]);
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator::pointer
B_tree<tkey, tvalue, compare, t>::btree_iterator::operator->() const noexcept {
    return &operator*();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator&
B_tree<tkey, tvalue, compare, t>::btree_iterator::operator++() {
    if (_path.empty()) return *this;

    auto* node = *_path.top().first;

    if (!node->is_leaf()) {
        const size_t next_index = _index + 1;
        _path.top().second = next_index;

        btree_node** curr_ptr = &node->_pointers[next_index];
        while (!(*curr_ptr)->is_leaf()) {
            _path.push({curr_ptr, 0});
            curr_ptr = &(*curr_ptr)->_pointers[0];
        }

        _path.push({curr_ptr, 0});
        _index = 0;
        return *this;
    }

    ++_index;

    while (!_path.empty()) {
        auto* current_node = *_path.top().first;
        if (_index < current_node->_keys.size()) {
            return *this;
        }

        _path.pop();
        if (_path.empty()) {
            _index = 0;
            return *this;
        }

        _index = _path.top().second;
    }

    return *this;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::btree_iterator::operator++(
    int) {
    btree_iterator tmp = *this;
    ++(*this);
    return tmp;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator&
B_tree<tkey, tvalue, compare, t>::btree_iterator::operator--() {
    if (_path.empty()) return *this;
    btree_node* node = *_path.top().first;
    if (!node->is_leaf()) {
        btree_node** curr_ptr = &node->_pointers[_index];
        _path.push({curr_ptr, _index});
        btree_node* curr = *curr_ptr;
        while (!curr->is_leaf()) {
            curr_ptr = &curr->_pointers[curr->_pointers.size() - 1];
            _path.push({curr_ptr, curr->_pointers.size() - 1});
            curr = *curr_ptr;
        }
        _index = curr->_keys.size() - 1;
    } else {
        if (_index > 0) {
            _index--;
        } else {
            while (!_path.empty() && _path.top().second == 0) {
                _path.pop();
            }
            if (!_path.empty()) {
                _index = _path.top().second - 1;
                _path.pop();
            } else {
                _index = 0;  // Became begin()
            }
        }
    }
    return *this;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::btree_iterator::operator--(int) {
    btree_iterator tmp = *this;
    --(*this);
    return tmp;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_iterator::operator==(const self& other) const noexcept {
    if (_path.empty() && other._path.empty()) return true;

    if (_path.empty() || other._path.empty()) return false;

    return (*_path.top().first == *other._path.top().first) && _index == other._index;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_iterator::operator!=(const self& other) const noexcept {
    return !(*this == other);
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_iterator::depth() const noexcept {
    return _path.empty() ? 0 : _path.size() - 1;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_iterator::current_node_keys_count() const noexcept {
    return _path.empty() ? 0 : (*_path.top().first)->_keys.size();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_iterator::is_terminate_node() const noexcept {
    return _path.empty() ? false : (*_path.top().first)->is_leaf();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_iterator::index() const noexcept {
    return _index;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::btree_const_iterator(
    const std::stack<std::pair<btree_node* const*, size_t>>& path, size_t index)
    : _path(path), _index(index) {}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::btree_const_iterator(const btree_iterator& it) noexcept
    : _index(it._index) {
    auto path_copy = it._path;
    std::vector<std::pair<btree_node**, size_t>> vec;
    while (!path_copy.empty()) {
        vec.push_back(path_copy.top());
        path_copy.pop();
    }
    for (auto vit = vec.rbegin(); vit != vec.rend(); ++vit) {
        _path.push({reinterpret_cast<btree_node* const*>(vit->first), vit->second});
    }
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator::reference
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator*() const noexcept {
    return reinterpret_cast<reference>((*_path.top().first)->_keys[_index]);
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator::pointer
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator->() const noexcept {
    return &operator*();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator&
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator++() {
    if (_path.empty()) return *this;

    const auto* node = *_path.top().first;

    if (!node->is_leaf()) {
        const size_t next_index = _index + 1;
        _path.top().second = next_index;

        btree_node* const* curr_ptr = &node->_pointers[next_index];
        while (!(*curr_ptr)->is_leaf()) {
            _path.push({curr_ptr, 0});
            curr_ptr = &(*curr_ptr)->_pointers[0];
        }

        _path.push({curr_ptr, 0});
        _index = 0;
        return *this;
    }

    ++_index;

    while (!_path.empty()) {
        const auto* current_node = *_path.top().first;
        if (_index < current_node->_keys.size()) {
            return *this;
        }

        _path.pop();
        if (_path.empty()) {
            _index = 0;
            return *this;
        }

        _index = _path.top().second;
    }

    return *this;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator++(int) {
    btree_const_iterator tmp = *this;
    ++(*this);
    return tmp;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator&
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator--() {
    if (_path.empty()) return *this;
    const btree_node* node = *_path.top().first;
    if (!node->is_leaf()) {
        btree_node* const* curr_ptr = &node->_pointers[_index];
        _path.push({curr_ptr, _index});
        const btree_node* curr = *curr_ptr;
        while (!curr->is_leaf()) {
            curr_ptr = &curr->_pointers[curr->_pointers.size() - 1];
            _path.push({curr_ptr, curr->_pointers.size() - 1});
            curr = *curr_ptr;
        }
        _index = curr->_keys.size() - 1;
    } else {
        if (_index > 0) {
            _index--;
        } else {
            while (!_path.empty() && _path.top().second == 0) {
                _path.pop();
            }
            if (!_path.empty()) {
                _index = _path.top().second - 1;
                _path.pop();
            } else {
                _index = 0;
            }
        }
    }
    return *this;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator--(int) {
    btree_const_iterator tmp = *this;
    --(*this);
    return tmp;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator==(const self& other) const noexcept {
    if (_path.empty() && other._path.empty()) return true;
    if (_path.empty() || other._path.empty()) return false;

    return (*_path.top().first == *other._path.top().first) && _index == other._index;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator!=(const self& other) const noexcept {
    return !(*this == other);
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_const_iterator::depth() const noexcept {
    return _path.empty() ? 0 : _path.size() - 1;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_const_iterator::current_node_keys_count() const noexcept {
    return _path.empty() ? 0 : (*_path.top().first)->_keys.size();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_const_iterator::is_terminate_node() const noexcept {
    return _path.empty() ? false : (*_path.top().first)->is_leaf();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_const_iterator::index() const noexcept {
    return _index;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::btree_reverse_iterator(
    const std::stack<std::pair<btree_node**, size_t>>& path, size_t index)
    : _path(path), _index(index) {}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::btree_reverse_iterator(const btree_iterator& it) noexcept
    : _path(it._path), _index(it._index) {}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator B_tree<tkey, tvalue, compare, t>::btree_iterator()
    const noexcept {
    return btree_iterator(_path, _index);
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::reference
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator*() const noexcept {
    btree_iterator tmp(_path, _index);
    return *(--tmp);
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::pointer
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator->() const noexcept {
    return &operator*();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator&
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator++() {
    btree_iterator tmp(_path, _index);
    --tmp;
    _path = tmp._path;
    _index = tmp._index;
    return *this;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator++(int) {
    btree_reverse_iterator tmp = *this;
    ++(*this);
    return tmp;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator&
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator--() {
    btree_iterator tmp(_path, _index);
    ++tmp;
    _path = tmp._path;
    _index = tmp._index;
    return *this;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator--(int) {
    btree_reverse_iterator tmp = *this;
    --(*this);
    return tmp;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator==(const self& other) const noexcept {
    if (_path.empty() && other._path.empty()) return true;
    // Если один пуст, а другой нет — не равны
    if (_path.empty() || other._path.empty()) return false;

    // СРАВНИВАЕМ САМИ УЗЛЫ (btree_node*), разыменовывая btree_node**
    return (*_path.top().first == *other._path.top().first) && _index == other._index;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator!=(const self& other) const noexcept {
    return !(*this == other);
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::depth() const noexcept {
    btree_iterator tmp(_path, _index);
    return (--tmp).depth();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::current_node_keys_count() const noexcept {
    btree_iterator tmp(_path, _index);
    return (--tmp).current_node_keys_count();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::is_terminate_node() const noexcept {
    btree_iterator tmp(_path, _index);
    return (--tmp).is_terminate_node();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::index() const noexcept {
    btree_iterator tmp(_path, _index);
    return (--tmp).index();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::btree_const_reverse_iterator(
    const std::stack<std::pair<btree_node* const*, size_t>>& path, size_t index)
    : _path(path), _index(index) {}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::btree_const_reverse_iterator(
    const btree_reverse_iterator& it) noexcept
    : _index(it._index) {
    auto path_copy = it._path;
    std::vector<std::pair<btree_node**, size_t>> vec;
    while (!path_copy.empty()) {
        vec.push_back(path_copy.top());
        path_copy.pop();
    }
    for (auto vit = vec.rbegin(); vit != vec.rend(); ++vit) {
        _path.push({reinterpret_cast<btree_node* const*>(vit->first), vit->second});
    }
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator B_tree<tkey, tvalue, compare,
    t>::btree_const_iterator() const noexcept {
    return btree_const_iterator(_path, _index);
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::reference
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator*() const noexcept {
    btree_const_iterator tmp(_path, _index);
    return *(--tmp);
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::pointer
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator->() const noexcept {
    return &operator*();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator&
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator++() {
    btree_const_iterator tmp(_path, _index);
    --tmp;
    _path = tmp._path;
    _index = tmp._index;
    return *this;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator++(int) {
    btree_const_reverse_iterator tmp = *this;
    ++(*this);
    return tmp;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator&
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator--() {
    btree_const_iterator tmp(_path, _index);
    ++tmp;
    _path = tmp._path;
    _index = tmp._index;
    return *this;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator--(int) {
    btree_const_reverse_iterator tmp = *this;
    --(*this);
    return tmp;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator==(const self& other) const noexcept {
    if (_path.empty() && other._path.empty()) return true;
    // Если один пуст, а другой нет — не равны
    if (_path.empty() || other._path.empty()) return false;

    // СРАВНИВАЕМ САМИ УЗЛЫ (btree_node*), разыменовывая btree_node**
    return (*_path.top().first == *other._path.top().first) && _index == other._index;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator!=(const self& other) const noexcept {
    return !(*this == other);
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::depth() const noexcept {
    btree_const_iterator tmp(_path, _index);
    return (--tmp).depth();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::current_node_keys_count() const noexcept {
    btree_const_iterator tmp(_path, _index);
    return (--tmp).current_node_keys_count();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::is_terminate_node() const noexcept {
    btree_const_iterator tmp(_path, _index);
    return (--tmp).is_terminate_node();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::index() const noexcept {
    btree_const_iterator tmp(_path, _index);
    return (--tmp).index();
}

// endregion iterators implementation

// region element access implementation

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
tvalue& B_tree<tkey, tvalue, compare, t>::at(const tkey& key) {
    btree_node* curr = _root;
    while (curr) {
        size_t idx = find_key_index(curr, key);
        if (idx < curr->_keys.size() && !compare_keys(key, curr->_keys[idx].first) &&
            !compare_keys(curr->_keys[idx].first, key))
            return curr->_keys[idx].second;
        if (curr->is_leaf()) break;
        curr = curr->_pointers[idx];
    }
    throw key_not_found();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
const tvalue& B_tree<tkey, tvalue, compare, t>::at(const tkey& key) const {
    const btree_node* curr = _root;
    while (curr) {
        size_t idx = find_key_index(curr, key);
        if (idx < curr->_keys.size() && !compare_keys(key, curr->_keys[idx].first) &&
            !compare_keys(curr->_keys[idx].first, key))
            return curr->_keys[idx].second;
        if (curr->is_leaf()) break;
        curr = curr->_pointers[idx];
    }
    throw key_not_found();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
tvalue& B_tree<tkey, tvalue, compare, t>::operator[](const tkey& key) {
    try {
        return at(key);
    } catch (const key_not_found&) {
        auto res = insert({key, tvalue()});
        return res.first->second;
    }
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
tvalue& B_tree<tkey, tvalue, compare, t>::operator[](tkey&& key) {
    tkey key_copy = key;
    try {
        return at(key_copy);
    } catch (const key_not_found&) {
        auto res = insert({std::move(key), tvalue()});
        return res.first->second;
    }
}

// endregion element access implementation

// region iterator begins implementation

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::begin() {
    if (!_root) return end();
    std::stack<std::pair<btree_node**, size_t>> path;
    btree_node** curr_ptr = &_root;
    while (!(*curr_ptr)->is_leaf()) {
        path.push({curr_ptr, 0});
        curr_ptr = &(*curr_ptr)->_pointers[0];
    }
    path.push({curr_ptr, 0});
    return btree_iterator(path, 0);
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::end() {
    return btree_iterator();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator B_tree<tkey, tvalue, compare, t>::begin() const {
    return cbegin();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator B_tree<tkey, tvalue, compare, t>::end() const {
    return cend();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator B_tree<tkey, tvalue, compare, t>::cbegin() const {
    if (!_root) return cend();
    std::stack<std::pair<btree_node* const*, size_t>> path;
    btree_node* const* curr_ptr = &_root;
    while (!(*curr_ptr)->is_leaf()) {
        path.push({curr_ptr, 0});
        curr_ptr = &(*curr_ptr)->_pointers[0];
    }
    path.push({curr_ptr, 0});
    return btree_const_iterator(path, 0);
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator B_tree<tkey, tvalue, compare, t>::cend() const {
    return btree_const_iterator();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator B_tree<tkey, tvalue, compare, t>::rbegin() {
    return btree_reverse_iterator(end());
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator B_tree<tkey, tvalue, compare, t>::rend() {
    return btree_reverse_iterator(begin());
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator B_tree<tkey, tvalue, compare, t>::rbegin()
    const {
    return crbegin();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator B_tree<tkey, tvalue, compare, t>::rend() const {
    return crend();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator B_tree<tkey, tvalue, compare, t>::crbegin()
    const {
    return btree_const_reverse_iterator(btree_reverse_iterator(const_cast<B_tree*>(this)->end()));
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator B_tree<tkey, tvalue, compare, t>::crend()
    const {
    return btree_const_reverse_iterator(btree_reverse_iterator(const_cast<B_tree*>(this)->begin()));
}

// endregion iterator begins implementation

// region lookup implementation

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::size() const noexcept {
    return _size;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::empty() const noexcept {
    return _size == 0;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::find(const tkey& key) {
    std::stack<std::pair<btree_node**, size_t>> path;
    btree_node** curr_ptr = &_root;

    while (*curr_ptr) {
        size_t idx = find_key_index(*curr_ptr, key);
        path.push({curr_ptr, idx});

        if (idx < (*curr_ptr)->_keys.size() && 
            !compare_keys(key, (*curr_ptr)->_keys[idx].first) && 
            !compare_keys((*curr_ptr)->_keys[idx].first, key))
            return btree_iterator(path, idx);

        if ((*curr_ptr)->is_leaf())
            break;

        curr_ptr = &(*curr_ptr)->_pointers[idx];
    }
    return end();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator B_tree<tkey, tvalue, compare, t>::find(
    const tkey& key) const {
    std::stack<std::pair<btree_node* const*, size_t>> path;
    btree_node* const* curr_ptr = &_root;
    while (*curr_ptr) {
        size_t idx = find_key_index(*curr_ptr, key);
        path.push({curr_ptr, idx});
        if (idx < (*curr_ptr)->_keys.size() && !compare_keys(key, (*curr_ptr)->_keys[idx].first) &&
            !compare_keys((*curr_ptr)->_keys[idx].first, key))
            return btree_const_iterator(path, idx);
        if ((*curr_ptr)->is_leaf()) break;
        curr_ptr = &(*curr_ptr)->_pointers[idx];
    }
    return cend();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::lower_bound(const tkey& key) {
    btree_node** curr_ptr = &_root;
    std::stack<std::pair<btree_node**, size_t>> path;
    
    std::stack<std::pair<btree_node**, size_t>> best_path;
    size_t best_index = 0;
    bool found = false;

    while (*curr_ptr != nullptr) {
        btree_node* node = *curr_ptr;
        size_t i = 0;
        while (i < node->_keys.size() && compare_keys(node->_keys[i].first, key)) i++;

        if (i < node->_keys.size()) {
            // Запоминаем этот узел как потенциальный ответ
            best_path = path;
            best_path.push({curr_ptr, i});
            best_index = i;
            found = true;
            
            if (!compare_keys(key, node->_keys[i].first)) break; // Нашли точное совпадение
            
            if (node->is_leaf()) break;
            path.push({curr_ptr, i});
            curr_ptr = &node->_pointers[i]; // Идем в левое поддерево ключа i
        } else {
            if (node->is_leaf()) break;
            path.push({curr_ptr, i}); // i == size
            curr_ptr = &node->_pointers[i]; // Идем в самое правое поддерево
        }
    }
    return found ? btree_iterator(best_path, best_index) : end();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator B_tree<tkey, tvalue, compare, t>::lower_bound(
    const tkey& key) const {
    btree_node * const *curr_ptr = &_root;
    std::stack<std::pair<btree_node * const *, size_t>> path;
    std::stack<std::pair<btree_node * const *, size_t>> best_path;
    size_t best_index = 0;
    bool found = false;

    while (*curr_ptr != nullptr) {
        btree_node *node = *curr_ptr;
        size_t i = 0;
        while (i < node->_keys.size() && compare_keys(node->_keys[i].first, key)) i++;

        if (i < node->_keys.size()) {
            best_path = path;
            best_path.push({curr_ptr, i});
            best_index = i;
            found = true;
            if (!compare_keys(key, node->_keys[i].first)) break;
            if (node->is_leaf()) break;
            path.push({curr_ptr, i});
            curr_ptr = &node->_pointers[i];
        } else {
            if (node->is_leaf()) break;
            path.push({curr_ptr, i});
            curr_ptr = &node->_pointers[i];
        }
    }
    return found ? btree_const_iterator(best_path, best_index) : cend();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::upper_bound(const tkey& key) {
    btree_node** curr_ptr = &_root;
    std::stack<std::pair<btree_node**, size_t>> path;
    std::stack<std::pair<btree_node**, size_t>> best_path;

    size_t best_index = 0;
    bool found = false;

    while (*curr_ptr != nullptr) {
        btree_node* node = *curr_ptr;
        size_t i = 0;

        while (i < node->_keys.size() && !compare_keys(key, node->_keys[i].first)) 
            i++;

        if (i < node->_keys.size()) {
            best_path = path;
            best_path.push({curr_ptr, i});

            best_index = i;
            found = true;

            if (node->is_leaf())
                break;

            path.push({curr_ptr, i});
            curr_ptr = &node->_pointers[i];
        } else {
            if (node->is_leaf())
                break;

            path.push({curr_ptr, i});
            curr_ptr = &node->_pointers[i];
        }
    }
    return found ? btree_iterator(best_path, best_index) : end();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator B_tree<tkey, tvalue, compare, t>::upper_bound(const tkey& key) const {
    btree_node * const *curr_ptr = &_root;
    std::stack<std::pair<btree_node * const *, size_t>> path;
    std::stack<std::pair<btree_node * const *, size_t>> best_path;

    size_t best_index = 0;
    bool found = false;

    while (*curr_ptr != nullptr) {
        btree_node *node = *curr_ptr;
        size_t i = 0;

        while (i < node->_keys.size() && !compare_keys(key, node->_keys[i].first)) 
            i++;

        if (i < node->_keys.size()) {
            best_path = path;
            best_path.push({curr_ptr, i});

            best_index = i;
            found = true;

            if (node->is_leaf())
                break;

            path.push({curr_ptr, i});
            curr_ptr = &node->_pointers[i];
        } else {
            if (node->is_leaf())
                break;

            path.push({curr_ptr, i});
            curr_ptr = &node->_pointers[i];
        }
    }
    return found ? btree_const_iterator(best_path, best_index) : cend();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::contains(const tkey& key) const {
    return find(key) != cend();
}

// endregion lookup implementation

// region modifiers implementation

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
void B_tree<tkey, tvalue, compare, t>::clear() noexcept {
    destroy_recursive(_root);
    _root = nullptr;
    _size = 0;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
std::pair<typename B_tree<tkey, tvalue, compare, t>::btree_iterator, bool> B_tree<tkey, tvalue, compare, t>::insert(
    const tree_data_type& data) {
    if (!_root) {
        _root = create_node();
        _root->_keys.push_back(data);
        _size = 1;

        return {find(data.first), true};
    }

    std::vector<btree_node*> path_nodes;
    bool inserted = insert_recursive(_root, data.first, data.second, path_nodes);

    if (inserted)
        _size++;

    return {find(data.first), inserted};
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
std::pair<typename B_tree<tkey, tvalue, compare, t>::btree_iterator, bool> B_tree<tkey, tvalue, compare, t>::insert(
    tree_data_type&& data) {
    tkey k = data.first;

    if (!_root) {
        _root = create_node();
        _root->_keys.push_back(std::move(data));
        _size = 1;
        return {find(k), true};
    }

    std::vector<btree_node*> path_nodes;
    bool inserted = insert_recursive(_root, data.first, data.second, path_nodes);
    if (inserted)
        _size++;

    return {find(k), inserted};
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
template <typename... Args>
std::pair<typename B_tree<tkey, tvalue, compare, t>::btree_iterator, bool> B_tree<tkey, tvalue, compare, t>::emplace(
    Args&&... args) {
    return insert(tree_data_type(std::forward<Args>(args)...));
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::insert_or_assign(
    const tree_data_type& data) {
    auto res = insert(data);

    if (!res.second) res.first->second = data.second;

    return res.first;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::insert_or_assign(
    tree_data_type&& data) {
    tvalue v = data.second;

    auto res = insert(std::move(data));
    if (!res.second) res.first->second = std::move(v);

    return res.first;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
template <typename... Args>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::emplace_or_assign(
    Args&&... args) {
    return insert_or_assign(tree_data_type(std::forward<Args>(args)...));
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::erase(btree_iterator pos) {
    if (pos == end())
        return end();
    
    tkey key = pos->first;
    btree_iterator next_it = pos;
    ++next_it;
    tkey next_key;

    bool has_next = (next_it != end());
    if (has_next)
        next_key = next_it->first;

    if (erase_recursive(_root, key, nullptr, 0)) {
        _size--;
        if (_root && _root->_keys.empty() && !_root->is_leaf()) {
            btree_node* old_root = _root;
            _root = _root->_pointers[0];
            old_root->_pointers.clear();
            destroy_node(old_root);
        }
        if (_size == 0) {
            clear();
        }
        return has_next ? find(next_key) : end();
    }
    return end();
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::erase(
    btree_const_iterator pos) {
    if (pos == cend()) return end();

    return erase(find(pos->first));
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::erase(
    btree_iterator beg, btree_iterator en) {
    while (beg != en) beg = erase(beg);
    
    return en;
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::erase(
    btree_const_iterator beg, btree_const_iterator en)
{
    while (beg != en) {
        beg = erase(beg);
    }

    return (en == cend()) ? end() : find(en->first);
}
template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::erase(const tkey& key) {
    return erase(find(key));
}

// endregion modifiers implementation

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool compare_pairs(const typename B_tree<tkey, tvalue, compare, t>::tree_data_type& lhs,
    const typename B_tree<tkey, tvalue, compare, t>::tree_data_type& rhs) {
    return compare()(lhs.first, rhs.first);
}

template <typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool compare_keys(const tkey& lhs, const tkey& rhs) {
    return compare()(lhs, rhs);
}

#endif
