// Append-only sequence stored in fixed-size chunks. Growing never moves the elements, so there is no reallocation
// peak (old and new array alive at once) and no slack beyond one chunk; references stay valid on push_back.
// Random access by index, forward iteration in index order.
//
// A chunk is raw storage: an element is constructed when it is appended, so a chunk that is only partly used costs
// neither construction time nor resident memory for the unused part.
//
// release_before(n) gives the chunks that hold only elements below n back; the indices of the other elements stay.
#pragma once

#include <cstddef>
#include <iterator>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace eqx {

template <class T, std::size_t ChunkSize = 1024>
class ChunkedVector {
    static_assert((ChunkSize & (ChunkSize - 1)) == 0, "ChunkSize must be a power of two");

public:
    using value_type = T;
    using size_type = std::size_t;

    ChunkedVector() = default;
    ChunkedVector(const ChunkedVector&) = delete;
    ChunkedVector& operator=(const ChunkedVector&) = delete;
    ChunkedVector(ChunkedVector&& o) noexcept : chunks_(std::move(o.chunks_)), size_(o.size_), released_(o.released_) {
        o.chunks_.clear();
        o.size_ = 0;
        o.released_ = 0;
    }
    ChunkedVector& operator=(ChunkedVector&& o) noexcept {
        if (this != &o) {
            clear();
            chunks_ = std::move(o.chunks_);
            size_ = o.size_;
            released_ = o.released_;
            o.chunks_.clear();
            o.size_ = 0;
            o.released_ = 0;
        }
        return *this;
    }
    ~ChunkedVector() { clear(); }

    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    T& operator[](std::size_t i) { return chunks_[i / ChunkSize][i % ChunkSize]; }
    const T& operator[](std::size_t i) const { return chunks_[i / ChunkSize][i % ChunkSize]; }
    T& back() { return (*this)[size_ - 1]; }
    const T& back() const { return (*this)[size_ - 1]; }

    template <class... A>
    T& emplace_back(A&&... args) {
        const std::size_t at = size_ % ChunkSize;
        if (at == 0 && size_ / ChunkSize == chunks_.size()) {
            // room for the pointer first, so that a chunk that was allocated is always kept
            if (chunks_.size() == chunks_.capacity()) chunks_.reserve(chunks_.empty() ? 16 : 2 * chunks_.size());
            chunks_.push_back(static_cast<T*>(::operator new(ChunkSize * sizeof(T), std::align_val_t(alignof(T)))));
        }
        T* p = ::new (static_cast<void*>(chunks_[size_ / ChunkSize] + at)) T(std::forward<A>(args)...);
        ++size_;
        return *p;
    }

    template <class U>
    void push_back(U&& value) {
        emplace_back(std::forward<U>(value));
    }

    void clear() {
        if (!std::is_trivially_destructible_v<T>)
            for (std::size_t i = released_ * ChunkSize; i < size_; ++i) (*this)[i].~T();
        for (T* c : chunks_)
            if (c) ::operator delete(c, std::align_val_t(alignof(T)));
        chunks_.clear();
        size_ = 0;
        released_ = 0;
    }

    // Destroys the elements of every chunk that lies wholly below index `n` (n <= size()) and frees those chunks.
    // The elements below released_end() are gone and must not be accessed; size() and every other index are unchanged.
    void release_before(std::size_t n) {
        const std::size_t whole = (n < size_ ? n : size_) / ChunkSize;
        for (; released_ < whole; ++released_) {
            T* c = chunks_[released_];
            if (!std::is_trivially_destructible_v<T>)
                for (std::size_t i = 0; i < ChunkSize; ++i) c[i].~T();
            ::operator delete(c, std::align_val_t(alignof(T)));
            chunks_[released_] = nullptr;
        }
    }
    // The first index whose element is still stored.
    std::size_t released_end() const { return released_ * ChunkSize; }

    template <bool Const>
    class Iter {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = std::conditional_t<Const, const T*, T*>;
        using reference = std::conditional_t<Const, const T&, T&>;
        using Owner = std::conditional_t<Const, const ChunkedVector, ChunkedVector>;

        Iter() = default;
        Iter(Owner* v, std::size_t i) : v_(v), i_(i) {}
        reference operator*() const { return (*v_)[i_]; }
        pointer operator->() const { return &(*v_)[i_]; }
        Iter& operator++() {
            ++i_;
            return *this;
        }
        Iter operator++(int) {
            Iter t = *this;
            ++i_;
            return t;
        }
        bool operator==(const Iter& o) const { return i_ == o.i_; }
        bool operator!=(const Iter& o) const { return i_ != o.i_; }

    private:
        Owner* v_ = nullptr;
        std::size_t i_ = 0;
    };

    Iter<false> begin() { return {this, 0}; }
    Iter<false> end() { return {this, size_}; }
    Iter<true> begin() const { return {this, 0}; }
    Iter<true> end() const { return {this, size_}; }

private:
    std::vector<T*> chunks_;
    std::size_t size_ = 0;
    std::size_t released_ = 0;   // chunks [0, released_) are freed
};

}  // namespace eqx
