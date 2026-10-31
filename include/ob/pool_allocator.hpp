#pragma once
#include <algorithm>
#include <cstddef>
#include <memory>
#include <new>
#include <vector>

namespace ob {

namespace detail {

// A free list of fixed-size blocks carved out of large chunks.
//
// allocate() pops the head of the free list; deallocate() pushes the block back.
// Both are O(1) with no system call and no lock. The "next free" pointer is stored
// inside the unused block itself, so the free list costs no extra memory.
//
// When the free list is empty the pool grows by one more chunk (doubling, up to
// kMaxChunkBlocks) rather than falling back to operator new, so a burst of new
// orders costs one big allocation instead of thousands of small ones. Chunks are
// never returned to the OS: memory is reused, not released.
//
// Not thread-safe; the order book is single-threaded by design.
template <std::size_t BlockSize, std::size_t BlockAlign>
class FixedPool {
    public:
        // One pool per block shape, shared by every allocator that needs it.
        // Deliberately never destroyed: a pool that outlived its users by being a
        // plain static could be destroyed before a global container frees into it.
        // The chunks stay reachable through this pointer, so LeakSanitizer is quiet.
        static FixedPool& instance() {
            static FixedPool* pool = new FixedPool();
            return *pool;
        }

        void* allocate() {
            if (free_ == nullptr) grow();
            Block* block = free_;
            free_ = block->next;
            return block;
        }

        void deallocate(void* p) noexcept {
            Block* block = static_cast<Block*>(p);
            block->next = free_;
            free_ = block;
        }

    private:
        union Block {
            Block* next;                                   // while free
            alignas(BlockAlign) std::byte storage[BlockSize];  // while in use
        };

        static constexpr std::size_t kFirstChunkBlocks = 4096;
        static constexpr std::size_t kMaxChunkBlocks = 1 << 20;

        void grow() {
            auto chunk = std::make_unique<Block[]>(next_chunk_blocks_);
            // Thread the new blocks onto the free list, lowest address first.
            for (std::size_t i = next_chunk_blocks_; i-- > 0;) {
                chunk[i].next = free_;
                free_ = &chunk[i];
            }
            chunks_.push_back(std::move(chunk));
            next_chunk_blocks_ = std::min(next_chunk_blocks_ * 2, kMaxChunkBlocks);
        }

        Block* free_{nullptr};
        std::size_t next_chunk_blocks_{kFirstChunkBlocks};
        std::vector<std::unique_ptr<Block[]>> chunks_;
};

}  // namespace detail

// A standard-conforming allocator backed by FixedPool.
//
// std::list<Order, PoolAllocator<Order>> never allocates an Order on its own: it
// rebinds the allocator to its internal node type (Order plus two pointers) and
// allocates nodes one at a time. So the pool's block size is the node's size, and
// it is chosen at compile time through the rebinding constructor below.
//
// The allocator is stateless (every instance shares the same pool), so all
// instances compare equal and containers can splice and move freely.
template <class T>
class PoolAllocator {
    public:
        using value_type = T;
        using is_always_equal = std::true_type;

        PoolAllocator() noexcept = default;
        template <class U>
        PoolAllocator(const PoolAllocator<U>&) noexcept {}  // rebinding: list<Order> -> list node

        T* allocate(std::size_t n) {
            if (n != 1) return std::allocator<T>{}.allocate(n);  // lists only ever ask for 1
            return static_cast<T*>(Pool::instance().allocate());
        }

        void deallocate(T* p, std::size_t n) noexcept {
            if (n != 1) { std::allocator<T>{}.deallocate(p, n); return; }
            Pool::instance().deallocate(p);
        }

        template <class U>
        bool operator==(const PoolAllocator<U>&) const noexcept { return true; }

    private:
        using Pool = detail::FixedPool<std::max(sizeof(T), sizeof(void*)),
                                       std::max(alignof(T), alignof(void*))>;
};

}  // namespace ob
