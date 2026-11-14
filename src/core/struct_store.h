// Storage of the structure tree (core/structure.h): a block arena for the kid lists and the index from an object
// to its element.
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace eqx {

// Memory for many small arrays that live as long as their owner: handed out from blocks, never moved, released
// together. Each array is contiguous and 8-byte aligned. No slack beyond the unused end of a block: blocks start
// small and grow to 64 KiB, and an array of a quarter block or more gets an allocation of its own.
class BlockArena {
public:
    BlockArena() = default;
    BlockArena(const BlockArena&) = delete;
    BlockArena& operator=(const BlockArena&) = delete;
    BlockArena(BlockArena&& o) noexcept { swap(o); }
    BlockArena& operator=(BlockArena&& o) noexcept {
        if (this != &o) {
            clear();
            swap(o);
        }
        return *this;
    }
    ~BlockArena() { clear(); }

    // `bytes` bytes, 8-byte aligned, uninitialised (nullptr for 0 bytes).
    void* allocate(std::size_t bytes) {
        if (bytes == 0) return nullptr;
        bytes = (bytes + 7) & ~std::size_t(7);
        if (bytes > left_) {
            if (bytes * 4 >= next_block_) {   // large: its own allocation, the open block stays open
                blocks_.push_back(::operator new(bytes));
                return blocks_.back();
            }
            blocks_.push_back(::operator new(next_block_));
            at_ = static_cast<std::byte*>(blocks_.back());
            left_ = next_block_;
            if (next_block_ < kMaxBlock) next_block_ *= 2;
        }
        void* p = at_;
        at_ += bytes;
        left_ -= bytes;
        return p;
    }

    void clear() {
        for (void* b : blocks_) ::operator delete(b);
        blocks_.clear();
        at_ = nullptr;
        left_ = 0;
        next_block_ = kFirstBlock;
    }

private:
    static constexpr std::size_t kFirstBlock = 1024, kMaxBlock = 64 * 1024;
    void swap(BlockArena& o) noexcept {
        std::swap(blocks_, o.blocks_);
        std::swap(at_, o.at_);
        std::swap(left_, o.left_);
        std::swap(next_block_, o.next_block_);
    }
    std::vector<void*> blocks_;
    std::byte* at_ = nullptr;
    std::size_t left_ = 0;
    std::size_t next_block_ = kFirstBlock;
};

// ObjRef::key() -> element index, for keys that are inserted once and never removed: what an
// unordered_map<uint64_t, int> answers, in 8 bytes per slot of one open-addressing table instead of a node per key.
// Keys of generation 0 (all but a few in any file) are in the table by object number; the others in a map.
class RefIndex {
public:
    // The value stored for `key`, -1 when there is none.
    int find(std::uint64_t key) const {
        if (!in_table(key)) {
            auto it = other_.find(key);
            return it == other_.end() ? -1 : it->second;
        }
        if (slots_.empty()) return -1;
        const std::uint32_t num = static_cast<std::uint32_t>(key >> 16);
        for (std::size_t i = home(num);; i = (i + 1) & (slots_.size() - 1)) {
            if (slots_[i].num == num) return slots_[i].value;
            if (slots_[i].num == 0) return -1;
        }
    }

    // Stores `value` (>= 0) for `key`; a key that is already there keeps its value.
    void insert(std::uint64_t key, int value) {
        if (!in_table(key)) {
            other_.emplace(key, value);
            return;
        }
        if ((used_ + 1) * 4 > slots_.size() * 3) grow();
        const std::uint32_t num = static_cast<std::uint32_t>(key >> 16);
        for (std::size_t i = home(num);; i = (i + 1) & (slots_.size() - 1)) {
            if (slots_[i].num == num) return;
            if (slots_[i].num == 0) {
                slots_[i] = {num, value};
                ++used_;
                return;
            }
        }
    }

    std::size_t size() const { return used_ + other_.size(); }

private:
    struct Slot {
        std::uint32_t num = 0;   // object number, 0 = free
        std::int32_t value = 0;
    };
    static bool in_table(std::uint64_t key) { return (key & 0xffff) == 0 && (key >> 16) != 0 && (key >> 48) == 0; }
    std::size_t home(std::uint32_t num) const { return (num * 0x9E3779B1u) >> shift_; }
    void grow() {
        std::vector<Slot> old;
        old.swap(slots_);
        const std::size_t n = old.empty() ? 16 : old.size() * 2;
        slots_.resize(n);
        shift_ = 32;
        for (std::size_t s = n; s > 1; s >>= 1) --shift_;
        for (const Slot& o : old) {
            if (o.num == 0) continue;
            std::size_t i = home(o.num);
            while (slots_[i].num != 0) i = (i + 1) & (n - 1);
            slots_[i] = o;
        }
    }
    std::vector<Slot> slots_;   // size a power of two, at most three quarters used
    std::size_t used_ = 0;
    unsigned shift_ = 32;
    std::unordered_map<std::uint64_t, int> other_;
};

}  // namespace eqx
