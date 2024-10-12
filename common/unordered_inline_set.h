#pragma once
#include <assert.h>
#include <inttypes.h>
#include <initializer_list>
#include <memory>
#include <type_traits>
#include <limits>
#include <cmath>
#include <string.h>

// an array with possible missing values
template<typename T>
class SparseArray {
protected:
    // T[i] are elements; and ((bit*)T)[-i]
    // are bools for existance of T[i]
    T* _ptr = nullptr;
    size_t _capacity = 0, _size = 0;
    bool get_bit(size_t i) const {
        auto c = (char*)_ptr - i / 8 - 1;
        return *c & (1 << (i % 8));
    }
    void set_bit(size_t i) {
        auto c = (char*)_ptr - i / 8 - 1;
        *c |= (1 << (i % 8));
    }
    void erase_bit(size_t i) {
        auto c = (char*)_ptr - i / 8 - 1;
        *c &= ~(1 << (i % 8));
    }
    size_t get_s(size_t capacity) const {
        auto u = sizeof(T) * 8;
        return (capacity + u - 1) / u;
    }
    void* buf() {
        return _ptr - get_s(_capacity);
    }
    const T* _get(size_t i) const {
        assert(i < _capacity);
        return get_bit(i) ? (_ptr + i) : nullptr;
    }

public:
    SparseArray(size_t capacity) { reset(capacity); }
    void* reset(size_t new_capacity) {
        if (_size > 0)
            for (auto x: *this) ((T&)x).~T();
        if (new_capacity == _capacity)
            return buf();
        auto s = get_s(new_capacity) * sizeof(T);
        if (new_capacity > 0) {
            auto ptr = realloc(buf(), s + new_capacity * sizeof(T));
            memset(ptr, 0, s);
            _ptr = (T*)((char*)ptr + s);
            _capacity = new_capacity;
            _size = 0;
            return ptr;
        } else {
            free(buf());
            _capacity = _size = 0;
            return _ptr = nullptr;
        }
    }
    SparseArray(SparseArray&& rhs) { *this = std::move(rhs); }
    void operator=(SparseArray&& rhs) {
        reset(0);
        _ptr = rhs._ptr;
        _capacity = rhs._capacity;
        _size = rhs._size;
        rhs._ptr = nullptr;
        rhs._capacity = 0;
        rhs._size = 0;
    }
    template<typename X>
    void set(size_t i, X&& x) {
        assert(i < _capacity);
        if (get_bit(i)) {
            _ptr[i] = std::forward<X>(x);
        } else {
            new (_ptr + i) T(std::forward<X>(x));
            set_bit(i);
            _size++;
        }
    }
    template<typename...Xs>
    void emplace(size_t i, Xs&&...xs) {
        assert(i < _capacity);
        if (!get_bit(i)) {
            // _ptr[i].T(std::forward<Xs>(xs)...);      // these 2 forms should be almost equal, except
            new (_ptr + i) T(std::forward<Xs>(xs)...);  // this is compatible with basic types, like int, etc.
            set_bit(i);
            _size++;
        } else {
            _ptr[i].~T();
            new (_ptr + i) T(std::forward<Xs>(xs)...);
        }
    }
    const T* get(size_t i) const noexcept { return _get(i); }
    T* get(size_t i) noexcept { return (T*)_get(i); }
    void erase(size_t i) {
        assert(i < _capacity);
        if (get_bit(i)) {
            erase_bit(i);
            _ptr[i].~T();
        }
    }
    bool empty() const noexcept { return size() == 0; }
    size_t size() const noexcept { return _size; }
    size_t capacity() const noexcept { return _capacity; }
    ~SparseArray() { reset(0); }

    class iterator {
    protected:
        SparseArray* _a = nullptr;
        size_t _i = 0;
        void seek(size_t d) {
            _i = _a->seek_existance(_i + d, d);
        }
    public:
        iterator() = default;
        iterator(SparseArray* a, size_t i = 0) : _a(a), _i(i) { }
        iterator& operator++()      { seek(1);  return *this; }
        iterator& operator--()      { seek(-1); return *this; }
        iterator operator++(int)    { auto r = *this; seek(1);  return r; }
        iterator operator--(int)    { auto r = *this; seek(-1); return r; }
        void erase()                { _a->erase(_i); }
        const T& operator*() const  { return *_a->get(_i); }
        const T* operator->() const { return  _a->get(_i); }
        template<typename P>
        void set(P&& x)             { _a->set(_i, std::forward<P>(x)); }
        bool operator==(const iterator& rhs) const {
            return _a == rhs._a && _i == rhs._i;
        }
        bool operator!=(const iterator& rhs) const {
            return !(*this == rhs);
        }
        T extract() {
            assert(_a && _i < _a->size());
            assert(_a->get_bit(_i));
            auto v = _a->get(_i);
            if (!v) return T();
            auto r = std::move(*v);
            _a->erase_bit(_i);
            return r;
        }
        iterator get_next() const {
            return ++iterator(*this);
        }
        using difference_type = std::ptrdiff_t;
        using value_type = T;
        using pointer = T*;
        using reference = T&;
        using iterator_category = std::bidirectional_iterator_tag;
    };

    iterator begin() noexcept { return _begin(); }
    iterator end() noexcept   { return {this, _capacity}; }
    iterator begin() const noexcept { return ((SparseArray*)this)->_begin(); }
    iterator end() const   noexcept { return {(SparseArray*)this, _capacity}; }
    bool iterator_valid(iterator it) const noexcept {
        return it._a == this && it._i <= capacity();
    }

    const T& operator[](size_t i) const {
        return *get(i);
    }
    const T& operator[](iterator it) const {
        assert(iterator_valid(it));
        return *get(it._i);
    }

    template<class P = T>
    typename std::enable_if<std::is_trivially_copyable<
             std::remove_reference_t<P>>::value>::type
    operator = (const SparseArray& rhs) {
        auto b = reset(rhs._capacity);
        auto src = (char*)rhs.buf();
        memcpy(b, src, (char*)(rhs._ptr + rhs._capacity) - src);
        _size = rhs._size;
    }

    template<class P = T>
    typename std::enable_if<!std::is_trivially_copyable<
             std::remove_reference_t<P>>::value>::type
    operator = (const SparseArray& rhs) {
        auto b = reset(rhs._capacity);
        for (auto it = rhs.begin(); it != rhs.end(); ++it)
            (*this)[it] = *it;
    }

protected:
    size_t seek_existance(size_t i, size_t delta) {
        for (; i < _capacity; i += delta)
            if (get_bit(i))
                return i;
        return _capacity;
    }
    iterator _begin() noexcept {
        return {this, seek_existance(0, 1)};
    }
};


// A hash table with open addressing, i.e. collision
// is resolved by probing (searching) through
// alternative locations in the array. This design
// avoids allocation and deallocation during insertion
// and deletion, unless it needs to rehash.
template<class Key,
         class Hash = std::hash<Key>,
         class KeyEqual = std::equal_to<Key>,
         class Allocator = std::allocator<Key>>
class unordered_inline_set {
// it also features the ability to extract (move out) a
// stored element itself, without involving a "handle".
protected:
    constexpr static size_t MIN_CAPACITY = 16;
    SparseArray<Key> _data { MIN_CAPACITY };
    constexpr static float DEFAULT_LOAD_FACTOR_MIN = 0.1;
    constexpr static float DEFAULT_LOAD_FACTOR_MAX = 0.5;
    float _load_factor_max = DEFAULT_LOAD_FACTOR_MAX;
    float _load_factor_min = DEFAULT_LOAD_FACTOR_MIN;
    size_t _load_max = MIN_CAPACITY * DEFAULT_LOAD_FACTOR_MAX;
    size_t _load_min = MIN_CAPACITY * DEFAULT_LOAD_FACTOR_MIN;
    uint8_t _max_collision = 8;
    Hash _hasher;
    KeyEqual _key_equal;
    Allocator _alloc;

    void _set_load_min_max() {
        _load_max = _data.capacity() * _load_factor_max;
        _load_min = _data.capacity() * _load_factor_min;
    }

public:
    using key_type = Key;
    using value_type = Key;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using hasher = Hash;
    using key_equal = KeyEqual;
    using allocator_type = Allocator;
    using reference = value_type&;
    using const_reference = const value_type&;
    using pointer = typename std::allocator_traits<Allocator>::pointer;
    using const_pointer	= typename std::allocator_traits<Allocator>::const_pointer;

    unordered_inline_set() = default;

    explicit unordered_inline_set( size_type bucket_count,
            const Hash& hash = Hash(),
            const key_equal& equal = key_equal(),
            const Allocator& alloc = Allocator() )
    : _data(bucket_count), _hasher(hash), _key_equal(equal), _alloc(alloc) {}

    explicit unordered_inline_set( size_type bucket_count,
            const Allocator& alloc )
    : unordered_inline_set(bucket_count, Hash(), key_equal(), alloc) {}

    explicit unordered_inline_set( size_type bucket_count,
            const Hash& hash,
            const Allocator& alloc )
    : unordered_inline_set(bucket_count, hash, key_equal(), alloc) {}

    explicit unordered_inline_set( const Allocator& alloc )
    : _alloc(alloc) { }

    template< class InputIt >
    explicit unordered_inline_set( InputIt first, InputIt last,
               size_type bucket_count = MIN_CAPACITY,
               const Hash& hash = Hash(),
               const key_equal& equal = key_equal(),
               const Allocator& alloc = Allocator() )
    : unordered_inline_set(bucket_count, hash, equal, alloc) {
        _set_load_min_max();
        for (auto it = first; it != last; ++it)
            insert(*it);
    }

    template< class InputIt >
    unordered_inline_set( InputIt first, InputIt last,
               size_type bucket_count,
               const Allocator& alloc )
    : unordered_inline_set(first, last,
                bucket_count, Hash(), key_equal(), alloc) {}

    template< class InputIt >
    unordered_inline_set( InputIt first, InputIt last,
               size_type bucket_count,
               const Hash& hash,
               const Allocator& alloc )
    : unordered_inline_set(first, last,
                    bucket_count, hash, key_equal(), alloc) {}

    unordered_inline_set( const unordered_inline_set& other )
    : unordered_inline_set(other.begin(), other.end()) {}

    unordered_inline_set( const unordered_inline_set& other, const Allocator& alloc )
    : unordered_inline_set(other.begin(), other.end(), MIN_CAPACITY, alloc) {}

    unordered_inline_set( unordered_inline_set&& other )
    : unordered_inline_set( std::move(other), std::move(other._alloc) ) {}

    unordered_inline_set( unordered_inline_set&& other, const Allocator& alloc )
    : _data(std::move(other._data)), _hasher(std::move(_hasher)),
      _key_equal(std::move(other._key_equal)), _alloc(alloc) { }

    unordered_inline_set( std::initializer_list<value_type> init,
               size_type bucket_count = MIN_CAPACITY,
               const Hash& hash = Hash(),
               const key_equal& equal = key_equal(),
               const Allocator& alloc = Allocator() )
    : unordered_inline_set(init.begin(), init.end(),
                    bucket_count, hash, equal, alloc) {}

    unordered_inline_set( std::initializer_list<value_type> init,
               size_type bucket_count,
               const Allocator& alloc )
    : unordered_inline_set(init, bucket_count,
                    Hash(), key_equal(), alloc) {}

    unordered_inline_set( std::initializer_list<value_type> init,
               size_type bucket_count,
               const Hash& hash,
               const Allocator& alloc )
    : unordered_inline_set(init, bucket_count,
                    hash, key_equal(), alloc) {}

#if __cplusplus >= 202300L
    template< compatible_range_type R >
    unordered_inline_set( std::from_range_t, R&& rg,
               size_type bucket_count = MIN_CAPACITY,
               const Hash& hash = Hash(),
               const key_equal& equal = key_equal(),
               const Allocator& alloc = Allocator() );

    template< compatible_range_type R >
    unordered_inline_set( std::from_range_t, R&& rg,
                size_type bucket_count,
                const Allocator& alloc )
        : unordered_inline_set(std::from_range, std::forward<R>(rg),
                        bucket_count, Hash(), key_equal(), alloc) {}

    template< compatible_range_type R >
    unordered_inline_set( std::from_range_t, R&& rg,
                size_type bucket_count,
                const Hash& hash,
                const Alloc& alloc )
        : unordered_inline_set(std::from_range, std::forward<R>(rg),
                        bucket_count, hash, key_equal(), alloc) {}
#endif

    template<typename Iterator>
    void assign(Iterator begin, Iterator end, size_t capacity = 0) {
        _data.reset(capacity);
        _load_max = capacity * _load_factor_max;
        _load_min = capacity * _load_factor_min;
        _set_load_min_max();
        for (auto it = begin; it != end; ++it)
            insert(*it);
    }

    unordered_inline_set& operator=( const unordered_inline_set& other ) {
        assign(other.begin(), other.end(), other.size());
    }

    unordered_inline_set& operator=( unordered_inline_set&& other ) noexcept {
        _data.reset(0);
        _data = std::move(other._data);
        _load_min = other._load_min;
        _load_max = other._load_max;
        _load_factor_min = other._load_factor_min;
        _load_factor_max = other._load_factor_max;
        _hasher = std::move(other._hasher);
        _key_equal = std::move(other._key_equal);
        _alloc = std::move(other._alloc);
    }

    unordered_inline_set& operator=( std::initializer_list<value_type> ilist ) {
        assign(ilist.begin(), ilist.end(), ilist.size());
    }

    allocator_type get_allocator() const noexcept {
        return _alloc;
    }

    using const_iterator = const typename SparseArray<Key>::iterator;
    using iterator = const_iterator;

    // iterator begin() noexcept { return _data.begin(); }
    const_iterator begin() const noexcept { return _data.begin(); }
    const_iterator cbegin() const noexcept { return _data.begin(); }

    // iterator end() noexcept { return _data.end(); }
    const_iterator end() const noexcept { return _data.end(); }
    const_iterator cend() const noexcept { return _data.end(); }

    bool empty() const noexcept {
        return size() == 0;
    }

    size_type size() const noexcept {
        return _data.size();
    }

    size_type max_size() const noexcept {
        return std::numeric_limits<size_type>::max();
    }

    uint8_t max_collision() const noexcept {
        return _max_collision;
    }
    void max_collision(uint8_t n) {
        _max_collision = n;
    }

    void clear() noexcept {
        assign(nullptr, nullptr, 0);
    }

protected:
    using SAKI = typename SparseArray<Key>::iterator;
    template<typename K>
    auto _make_room( const K& value ) -> std::pair<SAKI, bool> {
        assert(_data.size() < _load_max );
        auto capacity = _data.capacity();
        assert(capacity >= _data.size());
        assert((capacity & (capacity - 1)) == 0); // capacity is 2^n
        if (_data.size() + 1 >= _load_max) {
do_rehash:
            rehash(capacity *= 2);
        }
        auto r = _find(value);
        if (r.first < capacity) // found available slot (true) or collision (false)
            return {{&_data, r.first}, r.second == capacity};
        goto do_rehash;
    }

    template<typename K>
    auto _find(const K& value) const -> std::pair<size_t, size_t> {
        auto capacity = _data.capacity();
        assert(capacity >= _data.size());
        assert((capacity & (capacity - 1)) == 0); // capacity is 2^n
        size_t mask = capacity - 1;
        auto begin = _hasher(value) % capacity;
        for (size_t i = begin; i < begin + _max_collision; ++i) {
            auto j = i & mask;
            auto item = _data.get(j);
            if (!item) return {j, capacity};
            if (_key_equal(*item, value)) return {j, j};
        }
        return {capacity, capacity};
    }
public:
    template< class InputIt >
    void insert( InputIt first, InputIt last ) {
        for (auto it = first; it != last; ++it)
            insert(*it);
    }
    void insert( std::initializer_list<value_type> ilist ) {
        insert(ilist.begin(), ilist.end());
    }
    std::pair<iterator,bool> insert( const value_type& value ) {
        auto r = _make_room(value);
        if (r.second) *r.first = value;
        return r;
    }
    std::pair<iterator,bool> insert( value_type&& value ) {
        auto r = _make_room(value);
        if (r.second) r.first.set(std::move(value));
        return r;
    }
    template< class K >
    std::pair<iterator, bool> insert( K&& value ) {
        auto r = _make_room(value);
        if (r.second) r.first.set(std::forward<K>(value));
        return r;
    }
    iterator insert( const_iterator hint, const value_type& value ) {
        return insert(value).first;
    }
    iterator insert( const_iterator hint, value_type&& value ) {
        return insert(std::move(value)).first;
    }
    template< class K >
    iterator insert( const_iterator hint, K&& value ) {
        return insert<K>(std::forward<K>(value)).first;
    }

#if __cplusplus >= 202300L
    template< compatible_range_type R >
    void insert_range( R&& rg );
#endif

    template< class... Args >
    std::pair<iterator, bool> emplace( Args&&... args ) {
        // we need to construct the obj of T before hashing it
        return insert( T(std::forward<Args>(args)...) );
    }
    template< class... Args >
    iterator emplace_hint( const_iterator hint, Args&&... args ) {
        return emplace( std::forward<Args>(args)... ).first;
    }

    size_type count( const Key& key ) const {
        return find(key) != end();
    }
    template< class K >
    size_type count( const K& x ) const {
        return find<K>(x) != end();
    }
    bool contains( const Key& key ) const {
        return find(key) != end();
    }
    template< class K >
    bool contains( const K& x ) const {
        return find(x) != end();
    }
    std::pair<iterator, iterator> equal_range( const Key& key ) {
        auto it = find(key);
        if (it == end()) return {it, it};
        return {it, it.get_next()};
    }
    std::pair<const_iterator, const_iterator> equal_range( const Key& key ) const {
        auto it = find(key);
        if (it == end()) return {it, it};
        return {it, it.get_next()};
    }
    template< class K >
    std::pair<iterator, iterator> equal_range( const K& x ) {
        auto it = find(x);
        if (it == end()) return {it, it};
        return {it, it.get_next()};
    }
    template< class K >
    std::pair<const_iterator, const_iterator> equal_range( const K& x ) const {
        auto it = find(x);
        if (it == end()) return {it, it};
        return {it, it.get_next()};
    }

public:
    iterator find( const Key& key ) {
        return {&_data, _find(key).second};
    }
    const_iterator find( const Key& key ) const {
        return {(SparseArray<Key>*)&_data, _find(key).second};
    }
    template< class K >
    iterator find( const K& x ) {
        return {&_data, _find(x).second};
    }
    template< class K >
    const_iterator find( const K& x ) const {
        return {&_data, _find(x).second};
    }

    iterator erase( iterator pos ) {
        if (pos._a != &_data ||
            pos._i >= _data.capacity())
                return pos;
        (pos++).erase();
        _check_down_rehash(pos);
        return pos;
    }
    // iterator erase( const_iterator pos );
    iterator erase( const_iterator first, const_iterator last ) {
        assert(first._a == _data);
        assert( last._a == _data);
        while (first != last)
            (first++).erase();
        return first;
    }
protected:
    void _rehash( size_type count, iterator& pos ) {
        assert(pos._a == &_data && pos._i <= _data.capacity());
        auto at_least = std::ceil(_data.size() / max_load_factor());
        if (count < at_least) count = at_least;
        if (size() == count) return;
        auto data = std::move(_data);
        _data.reset(count);
        _set_load_min_max();
        for (auto it = data.begin(); it != data.end(); ++it)
            insert(it.extract());
    }
    void _check_down_rehash(iterator pos) {
        if (_data.size() >= _load_min ||
            _data.capacity() <= MIN_CAPACITY) return;

        rehash(_data.capacity() / 2);
        _load_min /= 2;
        _load_max /= 2;

    }
    void _check_down_rehash() {
        if (_data.size() <= _load_max ) return;
        rehash(_data.capacity() * 2);
        _load_min *= 2;
        _load_max *= 2;
    }
    size_type _erase_it(iterator it) {
        if (it != end()) {
            erase(it);
            return 1;
        } else {
            return 0;
        }
    }
public:
    size_type erase( const Key& key ) {
        return _erase_it(find(key));
    }
    template< class K >
    size_type erase( K&& k ) {
        return _erase_it(find(std::forward<K>(k)));
    }

    void swap( unordered_inline_set& other ) noexcept;

    template< class K >
    value_type&& extract( K&& x ) {
        return extract(find(std::forward<K>(x)));
    }
    value_type&& extract( iterator position ) {
        assert(_data.iterator_valid(position));
        return position.extract();
    }
    value_type&& extract( const Key& key ) {
        return extract(find(key));
    }

    // bucket interface
    using local_iterator = const_iterator;
    using const_local_iterator = const_iterator;
protected:
    local_iterator _bucket_it(size_type i) {
        if (!_data.get(i)) i = _data.capacity();
        return {_data, i};
    }
public:
    local_iterator begin( size_type n ) {
        return _bucket_it(n);
    }
    const_local_iterator begin( size_type n ) const {
        return _bucket_it(n);
    }
    const_local_iterator cbegin( size_type n ) const {
        return _bucket_it(n);
    }
    local_iterator end( size_type n ) {
        return _bucket_it(n);
    }
    const_local_iterator end( size_type n ) const {
        return _bucket_it(n);
    }
    const_local_iterator cend( size_type n ) const {
        return _bucket_it(n);
    }
    size_type bucket_count() const {
        return _data.capacity();
    }
    size_type max_bucket_count() const {
        return max_size();
    }
    size_type bucket_size( size_type n ) const {
        return 1;
    }
    size_type bucket( const Key& key ) const {
        return _hasher(key) % bucket_count();
    }
    template< typename K >
    size_type bucket( const K& k ) const {
        return _hasher(k) % bucket_count();
    }

    float load_factor() const {
        return float(_data.size()) / _data.capacity();
    }
    void load_factor(float min, float max) {
        if (0 < min && min < max && max < 1) {
            _load_factor_min = min;
            _load_factor_max = max;
            _set_load_min_max();
        }
    }

    float max_load_factor() const {
        return _load_factor_max;
    }
    void max_load_factor( float mlf ) {
        assert(mlf < 1);
        if (_load_factor_min < mlf && mlf < 1) {
            _load_factor_max = mlf;
            _load_max = _data.capacity() * mlf;
        }
    }

    void rehash( size_type count ) {
        auto at_least = std::ceil(_data.size() / max_load_factor());
        if (count < at_least) count = at_least;
        if (size() == count) return;
        auto data = std::move(_data);
        _load_max = count * _load_factor_max;
        _load_min = count * _load_factor_min;
        _data.reset(count);
        for (auto it = data.begin(); it != data.end(); ++it)
            insert(it.extract());
    }
    void reserve( size_type count ) {
        rehash(std::ceil(count / max_load_factor()));
    }
    hasher hash_function() const {
        return _hasher;
    }
    key_equal key_eq() const {
        return _key_equal;
    }
    bool operator == (const unordered_inline_set& rhs) const {
        return _data.size() == rhs._data.size() && alleq(rhs);
    }
    bool operator != (const unordered_inline_set& rhs) const {
        return !(*this == rhs);
    }
    template<typename Pred>
    size_type erase_if(const Pred& pred) {
        size_type s0 = size();
        for (auto it = begin(); it != end(); )
            it = pred(*it) ? erase(it) : ++it;
        return size() - s0;
    }
protected:
    bool alleq(const unordered_inline_set& rhs) {
        for (auto& x: _data)
            if (rhs.count(x) == 0)
                return false;
        return true;
    }
};

template< class Key, class Hash, class KeyEqual, class Alloc >
void swap( unordered_inline_set<Key, Hash, KeyEqual, Alloc>& lhs,
           unordered_inline_set<Key, Hash, KeyEqual, Alloc>& rhs ) noexcept {
    lhs.swap(rhs);
}

template< class Key, class Hash, class KeyEqual, class Alloc, class Pred >
typename unordered_inline_set<Key, Hash, KeyEqual, Alloc>::size_type
erase_if(unordered_inline_set<Key, Hash, KeyEqual, Alloc>& c, const Pred& pred) {
    return c.erase_if(pred);
}
