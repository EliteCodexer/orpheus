#pragma once

#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace orpheus {

/**
 * @brief Statistics for memory cache performance.
 */
struct CacheStats {
    uint64_t hits{0};
    uint64_t misses{0};
    uint64_t evictions{0};
    uint64_t invalidations{0};
    size_t bytes_cached{0};

    [[nodiscard]] double HitRate() const noexcept {
        const uint64_t total = hits + misses;
        if (total == 0) return 0.0;
        return static_cast<double>(hits) / static_cast<double>(total);
    }
};

/**
 * @brief Static memory region descriptor with infinite/custom TTL.
 */
struct StaticRegion {
    uint32_t pid{0};
    uint64_t start_address{0};
    size_t size{0};

    [[nodiscard]] bool Overlaps(uint32_t target_pid, uint64_t addr, size_t s) const noexcept {
        if (pid != target_pid || s == 0 || size == 0) return false;
        return addr < (start_address + size) && (addr + s) > start_address;
    }

    [[nodiscard]] bool Contains(uint32_t target_pid, uint64_t addr, size_t s) const noexcept {
        if (pid != target_pid) return false;
        if (s == 0) return true;
        return addr >= start_address && (addr + s) <= (start_address + size);
    }
};

/**
 * @brief Page key representing (pid, 4KB page aligned address).
 */
struct PageKey {
    uint32_t pid{0};
    uint64_t page_address{0};

    bool operator==(const PageKey& other) const noexcept {
        return pid == other.pid && page_address == other.page_address;
    }
};

struct PageKeyHash {
    size_t operator()(const PageKey& k) const noexcept {
        size_t h1 = std::hash<uint32_t>{}(k.pid);
        size_t h2 = std::hash<uint64_t>{}(k.page_address);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

/**
 * @brief Entry for a single 4KB page in the memory cache.
 */
struct PageCacheEntry {
    static constexpr size_t PAGE_SIZE = 4096;

    uint32_t pid{0};
    uint64_t page_address{0}; // Always aligned to 4096 bytes
    std::array<uint8_t, PAGE_SIZE> data{};
    std::bitset<PAGE_SIZE> valid_mask; // Tracks which byte offsets in page are valid
    std::chrono::steady_clock::time_point timestamp{};
    mutable uint64_t access_generation{0}; // For LRU tracking
    std::chrono::milliseconds ttl{0};      // 0 = default TTL, >0 = custom TTL, max() = infinite
    bool is_static{false};                 // If true, infinite TTL (e.g. .text, .rdata)
    uint64_t entry_generation{0};          // Cache generation counter at time of writing

    PageCacheEntry() = default;

    PageCacheEntry(const PageCacheEntry& other)
        : pid(other.pid),
          page_address(other.page_address),
          data(other.data),
          valid_mask(other.valid_mask),
          timestamp(other.timestamp),
          access_generation(other.access_generation),
          ttl(other.ttl),
          is_static(other.is_static),
          entry_generation(other.entry_generation) {}

    PageCacheEntry& operator=(const PageCacheEntry& other) {
        if (this != &other) {
            pid = other.pid;
            page_address = other.page_address;
            data = other.data;
            valid_mask = other.valid_mask;
            timestamp = other.timestamp;
            access_generation = other.access_generation;
            ttl = other.ttl;
            is_static = other.is_static;
            entry_generation = other.entry_generation;
        }
        return *this;
    }

    PageCacheEntry(PageCacheEntry&& other) noexcept
        : pid(other.pid),
          page_address(other.page_address),
          data(other.data),
          valid_mask(other.valid_mask),
          timestamp(other.timestamp),
          access_generation(other.access_generation),
          ttl(other.ttl),
          is_static(other.is_static),
          entry_generation(other.entry_generation) {}

    PageCacheEntry& operator=(PageCacheEntry&& other) noexcept {
        if (this != &other) {
            pid = other.pid;
            page_address = other.page_address;
            data = other.data;
            valid_mask = other.valid_mask;
            timestamp = other.timestamp;
            access_generation = other.access_generation;
            ttl = other.ttl;
            is_static = other.is_static;
            entry_generation = other.entry_generation;
        }
        return *this;
    }

    [[nodiscard]] bool IsExpired(std::chrono::milliseconds default_ttl,
                                 std::chrono::steady_clock::time_point now) const noexcept {
        if (is_static) return false;
        auto effective_ttl = (ttl.count() > 0) ? ttl : default_ttl;
        if (effective_ttl == std::chrono::milliseconds::max()) return false;
        return (now - timestamp) > effective_ttl;
    }

    [[nodiscard]] bool IsRangeValid(size_t offset, size_t size) const noexcept {
        if (offset + size > PAGE_SIZE) return false;
        if (valid_mask.all()) return true;
        for (size_t i = offset; i < offset + size; ++i) {
            if (!valid_mask.test(i)) return false;
        }
        return true;
    }

    [[nodiscard]] bool IsFullPage() const noexcept {
        return valid_mask.all();
    }

    [[nodiscard]] size_t ValidBytesCount() const noexcept {
        return valid_mask.count();
    }
};

/**
 * @brief Thread-safe page-aligned memory cache with reader-writer locking,
 * multi-page stitching, prefetching support, and section-aware TTL/generation tracking.
 */
class MemoryCache {
public:
    static constexpr size_t PAGE_SIZE = 4096;
    static constexpr size_t DEFAULT_MAX_PAGES = 1024; // 4MB cache
    static constexpr std::chrono::milliseconds DEFAULT_TTL{500};

    explicit MemoryCache(size_t max_pages = DEFAULT_MAX_PAGES,
                         std::chrono::milliseconds ttl = DEFAULT_TTL);
    ~MemoryCache() = default;

    // Non-copyable, non-movable
    MemoryCache(const MemoryCache&) = delete;
    MemoryCache& operator=(const MemoryCache&) = delete;
    MemoryCache(MemoryCache&&) = delete;
    MemoryCache& operator=(MemoryCache&&) = delete;

    /**
     * @brief Retrieve memory from cache. Supports multi-page and cross-page spanning reads.
     * @return Cached data if all requested bytes across all spanned pages are present & valid, std::nullopt otherwise.
     */
    [[nodiscard]] std::optional<std::vector<uint8_t>> Get(uint32_t pid, uint64_t address, size_t size);

    /**
     * @brief Store memory in cache. Breaks data down across page boundaries and updates valid masks.
     */
    void Put(uint32_t pid, uint64_t address, const std::vector<uint8_t>& data);

    /**
     * @brief Put a full or partial 4KB page into cache. Ideal for prefetching.
     * @param pid Process ID
     * @param page_addr 4KB page-aligned address (or will be aligned down)
     * @param data Pointer to page data
     * @param size Number of bytes in page (up to 4096)
     * @param custom_ttl Optional custom TTL (0 = use default TTL, max() = infinite)
     */
    void PutPage(uint32_t pid, uint64_t page_addr, const uint8_t* data, size_t size = PAGE_SIZE,
                 std::chrono::milliseconds custom_ttl = std::chrono::milliseconds(0));

    /**
     * @brief Put a full or partial 4KB page into cache.
     */
    void PutPage(uint32_t pid, uint64_t page_addr, const std::vector<uint8_t>& data,
                 std::chrono::milliseconds custom_ttl = std::chrono::milliseconds(0));

    /**
     * @brief Get a full 4KB page if cached and fully valid.
     */
    [[nodiscard]] std::optional<std::vector<uint8_t>> GetPage(uint32_t pid, uint64_t page_addr);

    /**
     * @brief Check if a 4KB page is resident and unexpired.
     */
    [[nodiscard]] bool IsPageCached(uint32_t pid, uint64_t page_addr) const;

    /**
     * @brief Check if an entire arbitrary memory range is resident and unexpired in cache.
     */
    [[nodiscard]] bool IsRangeCached(uint32_t pid, uint64_t address, size_t size) const;

    /**
     * @brief Invalidate a range of memory across any spanned pages.
     */
    void Invalidate(uint32_t pid, uint64_t address, size_t size);

    /**
     * @brief Invalidate all cached data for a process.
     */
    void InvalidatePid(uint32_t pid);

    /**
     * @brief Invalidate all cached data.
     */
    void InvalidateAll();

    /**
     * @brief Evict expired entries from the cache.
     */
    void EvictExpired();

    /**
     * @brief Clear all entries (alias for InvalidateAll).
     */
    void Clear();

    // Static / Section-aware TTL configuration
    void SetStaticRegion(uint32_t pid, uint64_t address, size_t size);
    void RemoveStaticRegion(uint32_t pid, uint64_t address, size_t size);
    [[nodiscard]] bool IsInStaticRegion(uint32_t pid, uint64_t address, size_t size = 1) const;
    void ClearStaticRegions();

    // Generation tracking
    [[nodiscard]] uint64_t GetCurrentGeneration() const noexcept {
        return global_generation_.load(std::memory_order_acquire);
    }
    uint64_t IncrementGeneration() noexcept {
        return global_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    // Configuration & Stats
    void SetTTL(std::chrono::milliseconds ttl);
    [[nodiscard]] std::chrono::milliseconds GetTTL() const;

    void SetMaxPages(size_t max_pages);
    [[nodiscard]] size_t GetMaxPages() const;

    void SetEnabled(bool enabled) noexcept;
    [[nodiscard]] bool IsEnabled() const noexcept;

    [[nodiscard]] CacheStats GetStats() const;
    void ResetStats();

    [[nodiscard]] size_t GetCacheSize() const; // Number of cached pages
    [[nodiscard]] uint64_t GetReadBytesSaved() const noexcept {
        return read_bytes_saved_.load(std::memory_order_relaxed);
    }

private:
    [[nodiscard]] static uint64_t PageAlign(uint64_t addr) noexcept {
        return addr & ~(PAGE_SIZE - 1);
    }

    [[nodiscard]] bool IsInStaticRegionLocked(uint32_t pid, uint64_t address, size_t size) const;
    void EvictOneLRUEntryLocked();

    size_t max_pages_;
    std::chrono::milliseconds ttl_;
    std::atomic<bool> enabled_{true};

    mutable std::shared_mutex mutex_;
    std::unordered_map<PageKey, PageCacheEntry, PageKeyHash> entries_;
    std::vector<StaticRegion> static_regions_;

    std::atomic<uint64_t> global_generation_{1};
    mutable std::atomic<uint64_t> access_tick_counter_{1};

    // Stats
    mutable std::atomic<uint64_t> hits_{0};
    mutable std::atomic<uint64_t> misses_{0};
    mutable std::atomic<uint64_t> evictions_{0};
    mutable std::atomic<uint64_t> invalidations_{0};
    mutable std::atomic<uint64_t> read_bytes_saved_{0};
};

} // namespace orpheus
