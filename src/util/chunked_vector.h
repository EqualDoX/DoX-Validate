// Append-only sequence stored in fixed-size chunks. Growing never moves the elements, so there is no reallocation
// peak (old and new array alive at once) and no slack beyond one chunk; references stay valid on push_back.
// Random access by index, forward iteration in index order.
#pragma once

#include <cstddef>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

namespace eqx {

template <class T, std::size_t ChunkSize = 1024>
class ChunkedVector {
    static_assert((ChunkSize & (ChunkSize - 1)) == 0, "ChunkSize must be a power of two");

public:
    using value_type = T;
    using size_type = std::size_t;

    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    T& operator[](std::size_t i) { return chunks_[i / ChunkSize][i % ChunkSize]; }
    const T& operator[](std::size_t i) const { return chunks_[i / ChunkSize][i % ChunkSize]; }
    T& back() { return (*this)[size_ - 1]; }
    const T& back() const { return (*this)[size_ - 1]; }

    template <class U>
    void push_back(U&& value) {
        if (size_ % ChunkSize == 0) chunks_.push_back(std::make_unique<T[]>(ChunkSize));
        chunks_.back()[size_ % ChunkSize] = std::forward<U>(value);
        ++size_;
    }

    void clear() {
        chunks_.clear();
        size_ = 0;
    }

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
    std::vector<std::unique_ptr<T[]>> chunks_;
    std::size_t size_ = 0;
};

}  // namespace eqx
