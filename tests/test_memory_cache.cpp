#include "core/memory_cache.h"
#include "core/dma_interface.h"
#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <numeric>
#include <thread>
#include <vector>

using namespace orpheus;

class MemoryCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        cache = std::make_unique<MemoryCache>(1024, std::chrono::milliseconds(500));
    }

    std::unique_ptr<MemoryCache> cache;
};

// ============================================================================
// Basic Functional Tests
// ============================================================================

TEST_F(MemoryCacheTest, StoreAndRetrieve) {
    const uint32_t pid = 1234;
    const uint64_t addr = 0x7FFF0000;
    const std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};

    cache->Put(pid, addr, data);
    auto result = cache->Get(pid, addr, data.size());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, data);
}

TEST_F(MemoryCacheTest, MissOnDifferentPid) {
    const uint64_t addr = 0x7FFF0000;
    const std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04};

    cache->Put(100, addr, data);
    auto result = cache->Get(200, addr, data.size());

    EXPECT_FALSE(result.has_value());
}

TEST_F(MemoryCacheTest, MissOnDifferentAddress) {
    const uint32_t pid = 100;
    const std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04};

    cache->Put(pid, 0x1000, data);
    auto result = cache->Get(pid, 0x2000, data.size());

    EXPECT_FALSE(result.has_value());
}

TEST_F(MemoryCacheTest, DisabledCacheReturnsMiss) {
    cache->SetEnabled(false);
    EXPECT_FALSE(cache->IsEnabled());

    cache->Put(1, 0x1000, {0xAA, 0xBB});
    auto result = cache->Get(1, 0x1000, 2);

    EXPECT_FALSE(result.has_value());
}

TEST_F(MemoryCacheTest, EnableDisableToggle) {
    cache->Put(1, 0x1000, {0xAA, 0xBB});
    EXPECT_TRUE(cache->Get(1, 0x1000, 2).has_value());

    // Disabling should invalidate
    cache->SetEnabled(false);
    EXPECT_FALSE(cache->Get(1, 0x1000, 2).has_value());

    // Re-enabling starts fresh
    cache->SetEnabled(true);
    EXPECT_FALSE(cache->Get(1, 0x1000, 2).has_value());
}

TEST_F(MemoryCacheTest, ClearRemovesAll) {
    cache->Put(1, 0x1000, {0x01});
    cache->Put(2, 0x2000, {0x02});

    EXPECT_EQ(cache->GetCacheSize(), 2);
    cache->Clear();
    EXPECT_EQ(cache->GetCacheSize(), 0);

    EXPECT_FALSE(cache->Get(1, 0x1000, 1).has_value());
    EXPECT_FALSE(cache->Get(2, 0x2000, 1).has_value());
}

// ============================================================================
// Stats & Metrics Tests
// ============================================================================

TEST_F(MemoryCacheTest, Stats) {
    cache->Put(1, 0x1000, {0x01, 0x02});

    (void)cache->Get(1, 0x1000, 2); // Hit
    (void)cache->Get(1, 0x2000, 2); // Miss
    (void)cache->Get(1, 0x1000, 2); // Hit

    auto stats = cache->GetStats();
    EXPECT_EQ(stats.hits, 2);
    EXPECT_EQ(stats.misses, 1);
    EXPECT_EQ(stats.bytes_cached, 4096);
    EXPECT_DOUBLE_EQ(stats.HitRate(), 2.0 / 3.0);
}

TEST_F(MemoryCacheTest, ResetStats) {
    cache->Put(1, 0x1000, {0x01});
    (void)cache->Get(1, 0x1000, 1);
    (void)cache->Get(1, 0x2000, 1);

    cache->ResetStats();
    auto stats = cache->GetStats();
    EXPECT_EQ(stats.hits, 0);
    EXPECT_EQ(stats.misses, 0);
}

TEST_F(MemoryCacheTest, HitRateCalculation) {
    CacheStats stats;
    stats.hits = 75;
    stats.misses = 25;
    EXPECT_DOUBLE_EQ(stats.HitRate(), 0.75);
}

TEST_F(MemoryCacheTest, HitRateWithNoAccesses) {
    CacheStats stats;
    EXPECT_DOUBLE_EQ(stats.HitRate(), 0.0);
}

// ============================================================================
// Invalidation Tests
// ============================================================================

TEST_F(MemoryCacheTest, InvalidateAddress) {
    cache->Put(1, 0x1000, {0xAA, 0xBB, 0xCC, 0xDD});
    cache->Invalidate(1, 0x1000, 4);

    EXPECT_FALSE(cache->Get(1, 0x1000, 4).has_value());
}

TEST_F(MemoryCacheTest, InvalidateProcess) {
    cache->Put(1, 0x1000, {0x01});
    cache->Put(1, 0x2000, {0x02});
    cache->Put(2, 0x1000, {0x03});

    cache->InvalidatePid(1);

    EXPECT_FALSE(cache->Get(1, 0x1000, 1).has_value());
    EXPECT_FALSE(cache->Get(1, 0x2000, 1).has_value());
    EXPECT_TRUE(cache->Get(2, 0x1000, 1).has_value());
}

TEST_F(MemoryCacheTest, InvalidatePartialSubPageAndBoundary) {
    const uint32_t pid = 42;
    // Write 32 bytes from 0x1FF0 across page boundary to 0x2010 (16 bytes in page 0x1000, 16 in page 0x2000)
    std::vector<uint8_t> data(32, 0xEE);
    cache->Put(pid, 0x1FF0, data);

    EXPECT_TRUE(cache->Get(pid, 0x1FF0, 32).has_value());
    EXPECT_TRUE(cache->Get(pid, 0x1FF0, 16).has_value());
    EXPECT_TRUE(cache->Get(pid, 0x2000, 16).has_value());

    // Invalidate 8 bytes at 0x1FF8 (inside page 0x1000 only)
    cache->Invalidate(pid, 0x1FF8, 8);

    // Spanning read should now miss
    EXPECT_FALSE(cache->Get(pid, 0x1FF0, 32).has_value());
    // Sub-range at 0x1FF0..0x1FF8 (8 bytes) should still be valid
    EXPECT_TRUE(cache->Get(pid, 0x1FF0, 8).has_value());
    // Page 0x2000 portion should still be valid
    EXPECT_TRUE(cache->Get(pid, 0x2000, 16).has_value());
}

// ============================================================================
// TTL and Eviction Tests
// ============================================================================

TEST_F(MemoryCacheTest, TTLExpiry) {
    // 50ms TTL
    cache->SetTTL(std::chrono::milliseconds(50));
    cache->Put(1, 0x1000, {0x01, 0x02});

    // Immediate read should hit
    EXPECT_TRUE(cache->Get(1, 0x1000, 2).has_value());

    // Wait for TTL to expire
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    EXPECT_FALSE(cache->Get(1, 0x1000, 2).has_value());
}

TEST_F(MemoryCacheTest, MaxPagesEviction) {
    // Cache with max 2 pages
    auto small_cache = std::make_unique<MemoryCache>(2, std::chrono::milliseconds(5000));

    small_cache->Put(1, 0x1000, {0x01});
    small_cache->Put(1, 0x2000, {0x02});
    EXPECT_EQ(small_cache->GetCacheSize(), 2);

    // Adding a 3rd page should evict one (LRU: 0x1000)
    small_cache->Put(1, 0x3000, {0x03});
    EXPECT_LE(small_cache->GetCacheSize(), 2);

    auto stats = small_cache->GetStats();
    EXPECT_GE(stats.evictions, 1);
}

TEST_F(MemoryCacheTest, GetZeroSizeReturnsMiss) {
    cache->Put(1, 0x1000, {0x01, 0x02});
    auto result = cache->Get(1, 0x1000, 0);
    EXPECT_FALSE(result.has_value());
}

TEST_F(MemoryCacheTest, PageAlignment) {
    // Both addresses fall within page 0x1000
    cache->Put(1, 0x1000, {0xAA});
    cache->Put(1, 0x1010, {0xBB});

    // Should only be 1 page cached
    EXPECT_EQ(cache->GetCacheSize(), 1);

    auto r1 = cache->Get(1, 0x1000, 1);
    auto r2 = cache->Get(1, 0x1010, 1);

    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ((*r1)[0], 0xAA);
    EXPECT_EQ((*r2)[0], 0xBB);
}

TEST_F(MemoryCacheTest, ConfigRoundtrip) {
    cache->SetTTL(std::chrono::milliseconds(2000));
    EXPECT_EQ(cache->GetTTL(), std::chrono::milliseconds(2000));

    cache->SetMaxPages(512);
    EXPECT_EQ(cache->GetMaxPages(), 512);
}

// ============================================================================
// Multi-Page & Cross-Page Stitching Tests
// ============================================================================

TEST_F(MemoryCacheTest, MultiPageSpanningRead) {
    const uint32_t pid = 1000;
    // Buffer spanning 3 full pages: 4096 * 3 = 12288 bytes
    const size_t total_size = 12288;
    std::vector<uint8_t> large_data(total_size);
    for (size_t i = 0; i < total_size; ++i) {
        large_data[i] = static_cast<uint8_t>(i & 0xFF);
    }

    const uint64_t base_addr = 0x10000; // Aligned to page
    cache->Put(pid, base_addr, large_data);

    EXPECT_EQ(cache->GetCacheSize(), 3);

    // Read full 3 pages at once
    auto full_read = cache->Get(pid, base_addr, total_size);
    ASSERT_TRUE(full_read.has_value());
    EXPECT_EQ(*full_read, large_data);

    // Read cross-page unaligned sub-slice spanning across page 1 and page 2 boundary:
    // from 0x10FFF (last byte of page 0) for 10 bytes -> spans to 0x11008 in page 1
    const uint64_t cross_addr = base_addr + 4095;
    const size_t cross_size = 10;
    auto cross_read = cache->Get(pid, cross_addr, cross_size);
    ASSERT_TRUE(cross_read.has_value());
    std::vector<uint8_t> expected_cross(large_data.begin() + 4095, large_data.begin() + 4095 + cross_size);
    EXPECT_EQ(*cross_read, expected_cross);
}

TEST_F(MemoryCacheTest, MultiPagePartialMissingReturnsMiss) {
    const uint32_t pid = 1000;
    // Store page 0 (0x10000) and page 2 (0x12000), but omit page 1 (0x11000)
    std::vector<uint8_t> page_data(4096, 0xAB);
    cache->PutPage(pid, 0x10000, page_data);
    cache->PutPage(pid, 0x12000, page_data);

    // A read spanning from 0x10800 for 8192 bytes (spans pages 0, 1, 2)
    auto result = cache->Get(pid, 0x10800, 8192);
    EXPECT_FALSE(result.has_value());

    // Single page read on page 0 should succeed
    EXPECT_TRUE(cache->Get(pid, 0x10000, 4096).has_value());
    // Single page read on page 2 should succeed
    EXPECT_TRUE(cache->Get(pid, 0x12000, 4096).has_value());
}

// ============================================================================
// Page Prefetching & Full Page API Tests
// ============================================================================

TEST_F(MemoryCacheTest, PutPageAndGetPage) {
    const uint32_t pid = 500;
    const uint64_t page_addr = 0x20000;
    std::vector<uint8_t> page_data(4096);
    std::iota(page_data.begin(), page_data.end(), 0x10);

    // Put raw full page buffer
    cache->PutPage(pid, page_addr, page_data.data(), page_data.size());

    EXPECT_TRUE(cache->IsPageCached(pid, page_addr));

    auto retrieved = cache->GetPage(pid, page_addr);
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(*retrieved, page_data);

    // Read arbitrary subslice from prefetched page
    auto subslice = cache->Get(pid, page_addr + 128, 64);
    ASSERT_TRUE(subslice.has_value());
    std::vector<uint8_t> expected_sub(page_data.begin() + 128, page_data.begin() + 192);
    EXPECT_EQ(*subslice, expected_sub);
}

TEST_F(MemoryCacheTest, IsRangeCachedCheck) {
    const uint32_t pid = 500;
    cache->Put(pid, 0x1000, std::vector<uint8_t>(200, 0x55));

    EXPECT_TRUE(cache->IsRangeCached(pid, 0x1000, 200));
    EXPECT_TRUE(cache->IsRangeCached(pid, 0x1050, 50));
    EXPECT_FALSE(cache->IsRangeCached(pid, 0x1000, 300)); // Out of written range
    EXPECT_FALSE(cache->IsRangeCached(pid, 0x0FF0, 30));  // Out of written range
}

// ============================================================================
// Static Regions & Generation Tracking Tests
// ============================================================================

TEST_F(MemoryCacheTest, StaticRegionInfiniteTTL) {
    const uint32_t pid = 777;
    const uint64_t text_section = 0x140000000;
    const size_t text_size = 0x10000; // 64KB

    // Short default TTL of 20ms
    cache->SetTTL(std::chrono::milliseconds(20));

    // Register .text as static region
    cache->SetStaticRegion(pid, text_section, text_size);
    EXPECT_TRUE(cache->IsInStaticRegion(pid, text_section, text_size));

    std::vector<uint8_t> code = {0x48, 0x89, 0x5C, 0x24, 0x08}; // mov [rsp+8], rbx
    cache->Put(pid, text_section, code);

    // Non-static volatile heap write
    const uint64_t heap_addr = 0x200000000;
    std::vector<uint8_t> heap_data = {0xAA, 0xBB, 0xCC, 0xDD};
    cache->Put(pid, heap_addr, heap_data);

    // Sleep past TTL
    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    // Static code should still be resident and hit!
    auto code_result = cache->Get(pid, text_section, code.size());
    ASSERT_TRUE(code_result.has_value());
    EXPECT_EQ(*code_result, code);

    // Heap data should be expired!
    auto heap_result = cache->Get(pid, heap_addr, heap_data.size());
    EXPECT_FALSE(heap_result.has_value());
}

TEST_F(MemoryCacheTest, PutPageCustomTTL) {
    const uint32_t pid = 777;
    const uint64_t page_addr = 0x30000;
    std::vector<uint8_t> page_data(4096, 0x42);

    // Cache with standard TTL of 20ms, but this page has custom 300ms TTL
    cache->SetTTL(std::chrono::milliseconds(20));
    cache->PutPage(pid, page_addr, page_data, std::chrono::milliseconds(300));

    // Sleep 40ms (> default TTL, < custom TTL)
    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    auto result = cache->GetPage(pid, page_addr);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, page_data);
}

TEST_F(MemoryCacheTest, GenerationTracking) {
    uint64_t gen0 = cache->GetCurrentGeneration();
    cache->Put(1, 0x1000, {0x01});
    uint64_t gen1 = cache->GetCurrentGeneration();
    EXPECT_GT(gen1, gen0);

    cache->Invalidate(1, 0x1000, 1);
    uint64_t gen2 = cache->GetCurrentGeneration();
    EXPECT_GT(gen2, gen1);

    (void)cache->IncrementGeneration();
    uint64_t gen3 = cache->GetCurrentGeneration();
    EXPECT_GT(gen3, gen2);
}

// ============================================================================
// Multi-threaded Concurrency & Shared Mutex Tests
// ============================================================================

TEST_F(MemoryCacheTest, ConcurrentReadsAndWrites) {
    const uint32_t pid = 999;
    const int num_readers = 6;
    const int num_writers = 2;
    const int operations_per_thread = 500;

    // Seed initial cache data
    for (int p = 0; p < 10; ++p) {
        std::vector<uint8_t> data(4096, static_cast<uint8_t>(p));
        cache->PutPage(pid, 0x10000 + p * 4096, data);
    }

    std::atomic<bool> start_flag{false};
    std::vector<std::future<void>> futures;

    // Launch reader threads
    for (int r = 0; r < num_readers; ++r) {
        futures.push_back(std::async(std::launch::async, [&, r]() {
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < operations_per_thread; ++i) {
                int page_idx = (r + i) % 10;
                uint64_t addr = 0x10000 + page_idx * 4096 + (i % 64);
                auto res = cache->Get(pid, addr, 32);
                // Even if write happens concurrently, read should be either valid or nullopt without crash
                if (res.has_value()) {
                    EXPECT_EQ(res->size(), 32);
                }
            }
        }));
    }

    // Launch writer threads
    for (int w = 0; w < num_writers; ++w) {
        futures.push_back(std::async(std::launch::async, [&, w]() {
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < operations_per_thread; ++i) {
                int page_idx = (w + i) % 10;
                uint64_t addr = 0x10000 + page_idx * 4096;
                std::vector<uint8_t> data(64, static_cast<uint8_t>(i & 0xFF));
                cache->Put(pid, addr + (i % 128), data);

                if (i % 50 == 0) {
                    cache->Invalidate(pid, addr, 64);
                }
            }
        }));
    }

    // Start all threads
    start_flag.store(true, std::memory_order_release);

    // Wait for all threads to finish
    for (auto& f : futures) {
        f.get();
    }

    // Verify cache is still in consistent state
    EXPECT_NO_THROW((void)cache->GetStats());
    EXPECT_NO_THROW(cache->InvalidateAll());
    EXPECT_EQ(cache->GetCacheSize(), 0);
}

// ============================================================================
// DMAInterface Cache & Scatter Read Integration Tests
// ============================================================================

TEST(DMAInterfaceTest, ReadMemoryUsesCacheWhenDisconnected) {
    DMAInterface dma;
    // By default dma is not connected to hardware, but cache operations can be exercised or verified
    EXPECT_FALSE(dma.IsConnected());
    EXPECT_TRUE(dma.IsCacheEnabled());

    // When disconnected and not in cache, ReadMemory should return empty
    auto res1 = dma.ReadMemory(1234, 0x1000, 4);
    EXPECT_TRUE(res1.empty());

    // Populate cache manually via CacheMemory
    std::vector<uint8_t> test_bytes = {0x11, 0x22, 0x33, 0x44};
    dma.CacheMemory(1234, 0x1000, test_bytes);

    // ReadMemory should hit cache even when disconnected!
    auto res2 = dma.ReadMemory(1234, 0x1000, 4);
    ASSERT_EQ(res2.size(), 4);
    EXPECT_EQ(res2, test_bytes);

    auto stats = dma.GetCacheStats();
    EXPECT_GE(stats.hits, 1);
}

TEST(DMAInterfaceTest, ScatterReadPrefiltersCacheHits) {
    DMAInterface dma;
    // dma is disconnected, so uncached scatter would fail/fallback and return empty
    const uint32_t pid = 4321;
    std::vector<uint8_t> item1 = {0xAA, 0xBB};
    std::vector<uint8_t> item2 = {0xCC, 0xDD, 0xEE};

    dma.CacheMemory(pid, 0x1000, item1);
    dma.CacheMemory(pid, 0x2000, item2);

    std::vector<ScatterRequest> requests = {
        { .address = 0x1000, .size = 2, .data = {}, .success = false },
        { .address = 0x2000, .size = 3, .data = {}, .success = false },
        { .address = 0x3000, .size = 4, .data = {}, .success = false } // Uncached, will fail since disconnected
    };

    size_t successful = dma.ScatterRead(pid, requests);
    // Cached requests (indices 0 and 1) should be resolved immediately from cache
    EXPECT_EQ(successful, 2);
    EXPECT_TRUE(requests[0].success);
    EXPECT_EQ(requests[0].data, item1);
    EXPECT_TRUE(requests[1].success);
    EXPECT_EQ(requests[1].data, item2);
    EXPECT_FALSE(requests[2].success);
    EXPECT_TRUE(requests[2].data.empty());
}

TEST(DMAInterfaceTest, ScatterReadCrossPageCacheHit) {
    DMAInterface dma;
    const uint32_t pid = 777;
    // Page 1 ends at 0x1FFF, Page 2 begins at 0x2000
    // Cache page 1 ending bytes and page 2 beginning bytes
    std::vector<uint8_t> page1_data(4096, 0x11);
    std::vector<uint8_t> page2_data(4096, 0x22);
    dma.CacheMemory(pid, 0x1000, page1_data);
    dma.CacheMemory(pid, 0x2000, page2_data);

    // Request spanning 0x1FFE to 0x2002 (4 bytes: 2 bytes from page 1, 2 bytes from page 2)
    std::vector<ScatterRequest> requests = {
        { .address = 0x1FFE, .size = 4, .data = {}, .success = false }
    };

    size_t count = dma.ScatterRead(pid, requests);
    EXPECT_EQ(count, 1);
    ASSERT_TRUE(requests[0].success);
    std::vector<uint8_t> expected = {0x11, 0x11, 0x22, 0x22};
    EXPECT_EQ(requests[0].data, expected);
}
