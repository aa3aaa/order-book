#include <gtest/gtest.h>

#include <cstdint>
#include <list>
#include <set>
#include <vector>

#include "ob/pool_allocator.hpp"
#include "ob/types.hpp"

namespace {
struct Wide { std::uint64_t a, b, c, d, e; };  // a distinct size, so its own pool
}

TEST(PoolAllocatorTest, FreedBlockIsReusedFirst) {
    ob::PoolAllocator<Wide> alloc;
    Wide* first = alloc.allocate(1);
    alloc.deallocate(first, 1);
    Wide* second = alloc.allocate(1);
    EXPECT_EQ(first, second);  // LIFO free list: the block just freed comes back
    alloc.deallocate(second, 1);
}

TEST(PoolAllocatorTest, BlocksAreDistinctAndAligned) {
    ob::PoolAllocator<Wide> alloc;
    std::vector<Wide*> blocks;
    std::set<Wide*> unique;
    for (int i = 0; i < 10'000; ++i) {  // more than one chunk, so the pool must grow
        Wide* p = alloc.allocate(1);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % alignof(Wide), 0u);
        p->a = static_cast<std::uint64_t>(i);
        blocks.push_back(p);
        unique.insert(p);
    }
    EXPECT_EQ(unique.size(), blocks.size());
    for (int i = 0; i < 10'000; ++i) EXPECT_EQ(blocks[i]->a, static_cast<std::uint64_t>(i));
    for (Wide* p : blocks) alloc.deallocate(p, 1);
}

TEST(PoolAllocatorTest, ArrayRequestsFallBackToTheHeap) {
    ob::PoolAllocator<Wide> alloc;
    Wide* many = alloc.allocate(8);
    for (int i = 0; i < 8; ++i) many[i].a = 1;
    alloc.deallocate(many, 8);
}

TEST(PoolAllocatorTest, WorksAsAListAllocator) {
    std::list<ob::Order, ob::PoolAllocator<ob::Order>> orders;
    for (ob::OrderId id = 1; id <= 1000; ++id) {
        ob::Order o;
        o.id = id;
        orders.push_back(o);
    }
    orders.remove_if([](const ob::Order& o) { return o.id % 2 == 0; });
    ASSERT_EQ(orders.size(), 500u);
    EXPECT_EQ(orders.front().id, 1u);
    EXPECT_EQ(orders.back().id, 999u);

    std::list<ob::Order, ob::PoolAllocator<ob::Order>> other;
    other.splice(other.end(), orders);  // allocators compare equal, so splice is allowed
    EXPECT_TRUE(orders.empty());
    EXPECT_EQ(other.size(), 500u);
}

TEST(PoolAllocatorTest, AllInstancesCompareEqual) {
    ob::PoolAllocator<ob::Order> a;
    ob::PoolAllocator<int> b;
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
}
