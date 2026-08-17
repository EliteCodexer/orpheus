#include "core/memory_cache.h"
#include "utils/logger.h"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace orpheus {

MemoryCache::MemoryCache(size_t max_pages, std::chrono::milliseconds ttl)
    : max_pages_(max_pages), ttl_(ttl) {
    LOG_DEBUG("MemoryCache initialized (max_pages={}, ttl={}ms)", max_pages_, ttl_.count());
}

std::optional<std::vector<uint8_t>> MemoryCache::Get(uint32_t pid, uint64_t address, size_t size) {
    if (!enabled_.load(std::memory_order_relaxed) || size == 0) {
        misses_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    const uint64_t start_page = PageAlign(address);
    const uint64_t end_page = PageAlign(address + size - 1);
    const auto now = std::chrono::steady_clock::now();

    std::shared_lock<std::shared_mutex> lock(mutex_);

    // 1. First verify that all spanned pages are present, unexpired, and have valid bytes for the requested slice
    uint64_t current_addr = address;
    size_t remaining = size;
    uint64_t current_page_addr = start_page;

    while (current_page_addr <= end_page) {
        const size_t offset_in_page = static_cast<size_t>(current_addr - current_page_addr);
        const size_t bytes_in_this_page = std::min(remaining, PAGE_SIZE - offset_in_page);

        PageKey key{pid, current_page_addr};
        auto it = entries_.find(key);
        if (it == entries_.end()) {
            misses_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        const auto& entry = it->second;
        if (entry.IsExpired(ttl_, now)) {
            misses_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        if (!entry.IsRangeValid(offset_in_page, bytes_in_this_page)) {
            misses_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        current_addr += bytes_in_this_page;
        remaining -= bytes_in_this_page;
        current_page_addr += PAGE_SIZE;
    }

    // 2. All pages and ranges are valid! Assemble the stitched output buffer
    std::vector<uint8_t> result(size);
    current_addr = address;
    remaining = size;
    size_t result_offset = 0;
    current_page_addr = start_page;

    while (current_page_addr <= end_page) {
        const size_t offset_in_page = static_cast<size_t>(current_addr - current_page_addr);
        const size_t bytes_in_this_page = std::min(remaining, PAGE_SIZE - offset_in_page);

        PageKey key{pid, current_page_addr};
        auto it = entries_.find(key);
        const auto& entry = it->second;

        std::memcpy(result.data() + result_offset, entry.data.data() + offset_in_page, bytes_in_this_page);
        entry.access_generation = access_tick_counter_.fetch_add(1, std::memory_order_relaxed);

        current_addr += bytes_in_this_page;
        result_offset += bytes_in_this_page;
        remaining -= bytes_in_this_page;
        current_page_addr += PAGE_SIZE;
    }

    hits_.fetch_add(1, std::memory_order_relaxed);
    read_bytes_saved_.fetch_add(size, std::memory_order_relaxed);
    return result;
}

void MemoryCache::Put(uint32_t pid, uint64_t address, const std::vector<uint8_t>& data) {
    if (!enabled_.load(std::memory_order_relaxed) || data.empty()) {
        return;
    }

    const size_t size = data.size();
    const uint64_t start_page = PageAlign(address);
    const uint64_t end_page = PageAlign(address + size - 1);
    const auto now = std::chrono::steady_clock::now();

    std::unique_lock<std::shared_mutex> lock(mutex_);
    global_generation_.fetch_add(1, std::memory_order_relaxed);

    uint64_t current_addr = address;
    size_t remaining = size;
    size_t data_offset = 0;
    uint64_t current_page_addr = start_page;

    while (current_page_addr <= end_page) {
        const size_t offset_in_page = static_cast<size_t>(current_addr - current_page_addr);
        const size_t bytes_in_this_page = std::min(remaining, PAGE_SIZE - offset_in_page);

        PageKey key{pid, current_page_addr};
        auto it = entries_.find(key);

        if (it != entries_.end()) {
            auto& entry = it->second;
            std::memcpy(entry.data.data() + offset_in_page, data.data() + data_offset, bytes_in_this_page);
            for (size_t i = offset_in_page; i < offset_in_page + bytes_in_this_page; ++i) {
                entry.valid_mask.set(i);
            }
            entry.timestamp = now;
            entry.entry_generation = global_generation_.load(std::memory_order_relaxed);
            entry.access_generation = access_tick_counter_.fetch_add(1, std::memory_order_relaxed);
            if (IsInStaticRegionLocked(pid, current_page_addr, PAGE_SIZE)) {
                entry.is_static = true;
            }
        } else {
            if (entries_.size() >= max_pages_) {
                EvictOneLRUEntryLocked();
            }

            PageCacheEntry entry;
            entry.pid = pid;
            entry.page_address = current_page_addr;
            std::memcpy(entry.data.data() + offset_in_page, data.data() + data_offset, bytes_in_this_page);
            for (size_t i = offset_in_page; i < offset_in_page + bytes_in_this_page; ++i) {
                entry.valid_mask.set(i);
            }
            entry.timestamp = now;
            entry.entry_generation = global_generation_.load(std::memory_order_relaxed);
            entry.access_generation = access_tick_counter_.fetch_add(1, std::memory_order_relaxed);
            entry.is_static = IsInStaticRegionLocked(pid, current_page_addr, PAGE_SIZE);

            entries_.emplace(key, std::move(entry));
        }

        current_addr += bytes_in_this_page;
        data_offset += bytes_in_this_page;
        remaining -= bytes_in_this_page;
        current_page_addr += PAGE_SIZE;
    }
}

void MemoryCache::PutPage(uint32_t pid, uint64_t page_addr, const uint8_t* data, size_t size,
                         std::chrono::milliseconds custom_ttl) {
    if (!enabled_.load(std::memory_order_relaxed) || data == nullptr || size == 0) {
        return;
    }

    const uint64_t aligned_page = PageAlign(page_addr);
    const size_t store_size = std::min(size, PAGE_SIZE);
    const auto now = std::chrono::steady_clock::now();

    std::unique_lock<std::shared_mutex> lock(mutex_);
    global_generation_.fetch_add(1, std::memory_order_relaxed);

    PageKey key{pid, aligned_page};
    auto it = entries_.find(key);

    if (it != entries_.end()) {
        auto& entry = it->second;
        if (store_size == PAGE_SIZE) {
            std::memcpy(entry.data.data(), data, PAGE_SIZE);
            entry.valid_mask.set();
        } else {
            std::memcpy(entry.data.data(), data, store_size);
            for (size_t i = 0; i < store_size; ++i) {
                entry.valid_mask.set(i);
            }
        }
        entry.timestamp = now;
        entry.ttl = custom_ttl;
        entry.is_static = (custom_ttl == std::chrono::milliseconds::max()) ||
                          IsInStaticRegionLocked(pid, aligned_page, PAGE_SIZE);
        entry.entry_generation = global_generation_.load(std::memory_order_relaxed);
        entry.access_generation = access_tick_counter_.fetch_add(1, std::memory_order_relaxed);
    } else {
        if (entries_.size() >= max_pages_) {
            EvictOneLRUEntryLocked();
        }

        PageCacheEntry entry;
        entry.pid = pid;
        entry.page_address = aligned_page;
        if (store_size == PAGE_SIZE) {
            std::memcpy(entry.data.data(), data, PAGE_SIZE);
            entry.valid_mask.set();
        } else {
            std::memcpy(entry.data.data(), data, store_size);
            for (size_t i = 0; i < store_size; ++i) {
                entry.valid_mask.set(i);
            }
        }
        entry.timestamp = now;
        entry.ttl = custom_ttl;
        entry.is_static = (custom_ttl == std::chrono::milliseconds::max()) ||
                          IsInStaticRegionLocked(pid, aligned_page, PAGE_SIZE);
        entry.entry_generation = global_generation_.load(std::memory_order_relaxed);
        entry.access_generation = access_tick_counter_.fetch_add(1, std::memory_order_relaxed);

        entries_.emplace(key, std::move(entry));
    }
}

void MemoryCache::PutPage(uint32_t pid, uint64_t page_addr, const std::vector<uint8_t>& data,
                         std::chrono::milliseconds custom_ttl) {
    PutPage(pid, page_addr, data.data(), data.size(), custom_ttl);
}

std::optional<std::vector<uint8_t>> MemoryCache::GetPage(uint32_t pid, uint64_t page_addr) {
    const uint64_t aligned_page = PageAlign(page_addr);
    return Get(pid, aligned_page, PAGE_SIZE);
}

bool MemoryCache::IsPageCached(uint32_t pid, uint64_t page_addr) const {
    const uint64_t aligned_page = PageAlign(page_addr);
    const auto now = std::chrono::steady_clock::now();

    std::shared_lock<std::shared_mutex> lock(mutex_);
    PageKey key{pid, aligned_page};
    auto it = entries_.find(key);
    if (it == entries_.end()) return false;
    if (it->second.IsExpired(ttl_, now)) return false;
    return it->second.IsFullPage();
}

bool MemoryCache::IsRangeCached(uint32_t pid, uint64_t address, size_t size) const {
    if (size == 0) return false;
    const uint64_t start_page = PageAlign(address);
    const uint64_t end_page = PageAlign(address + size - 1);
    const auto now = std::chrono::steady_clock::now();

    std::shared_lock<std::shared_mutex> lock(mutex_);

    uint64_t current_addr = address;
    size_t remaining = size;
    uint64_t current_page_addr = start_page;

    while (current_page_addr <= end_page) {
        const size_t offset_in_page = static_cast<size_t>(current_addr - current_page_addr);
        const size_t bytes_in_this_page = std::min(remaining, PAGE_SIZE - offset_in_page);

        PageKey key{pid, current_page_addr};
        auto it = entries_.find(key);
        if (it == entries_.end()) return false;
        if (it->second.IsExpired(ttl_, now)) return false;
        if (!it->second.IsRangeValid(offset_in_page, bytes_in_this_page)) return false;

        current_addr += bytes_in_this_page;
        remaining -= bytes_in_this_page;
        current_page_addr += PAGE_SIZE;
    }

    return true;
}

void MemoryCache::Invalidate(uint32_t pid, uint64_t address, size_t size) {
    if (size == 0) return;

    const uint64_t start_page = PageAlign(address);
    const uint64_t end_page = PageAlign(address + size - 1);

    std::unique_lock<std::shared_mutex> lock(mutex_);
    global_generation_.fetch_add(1, std::memory_order_relaxed);

    uint64_t current_page_addr = start_page;
    while (current_page_addr <= end_page) {
        PageKey key{pid, current_page_addr};
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            const size_t offset_in_page = (address > current_page_addr)
                                              ? static_cast<size_t>(address - current_page_addr)
                                              : 0;
            const size_t end_offset_in_page = std::min(
                PAGE_SIZE, static_cast<size_t>((address + size) - current_page_addr));

            if (offset_in_page == 0 && end_offset_in_page == PAGE_SIZE) {
                // Whole page invalidated
                entries_.erase(it);
            } else {
                for (size_t i = offset_in_page; i < end_offset_in_page; ++i) {
                    it->second.valid_mask.reset(i);
                }
                if (it->second.valid_mask.none()) {
                    entries_.erase(it);
                }
            }
            invalidations_.fetch_add(1, std::memory_order_relaxed);
        }
        current_page_addr += PAGE_SIZE;
    }
}

void MemoryCache::InvalidatePid(uint32_t pid) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    global_generation_.fetch_add(1, std::memory_order_relaxed);

    uint64_t count = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->first.pid == pid) {
            it = entries_.erase(it);
            ++count;
        } else {
            ++it;
        }
    }
    invalidations_.fetch_add(count, std::memory_order_relaxed);
    LOG_DEBUG("Invalidated {} cached pages for PID {}", count, pid);
}

void MemoryCache::InvalidateAll() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    global_generation_.fetch_add(1, std::memory_order_relaxed);

    const size_t count = entries_.size();
    entries_.clear();
    invalidations_.fetch_add(count, std::memory_order_relaxed);
    LOG_DEBUG("Invalidated all cached pages ({})", count);
}

void MemoryCache::Clear() {
    InvalidateAll();
}

void MemoryCache::EvictExpired() {
    const auto now = std::chrono::steady_clock::now();
    std::unique_lock<std::shared_mutex> lock(mutex_);

    uint64_t count = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.IsExpired(ttl_, now)) {
            it = entries_.erase(it);
            ++count;
        } else {
            ++it;
        }
    }

    if (count > 0) {
        evictions_.fetch_add(count, std::memory_order_relaxed);
        LOG_DEBUG("Evicted {} expired cache pages", count);
    }
}

void MemoryCache::SetStaticRegion(uint32_t pid, uint64_t address, size_t size) {
    if (size == 0) return;
    std::unique_lock<std::shared_mutex> lock(mutex_);
    static_regions_.push_back(StaticRegion{pid, address, size});

    // Mark any existing matching entries as static
    const uint64_t start_page = PageAlign(address);
    const uint64_t end_page = PageAlign(address + size - 1);
    uint64_t current_page = start_page;
    while (current_page <= end_page) {
        PageKey key{pid, current_page};
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            it->second.is_static = true;
        }
        current_page += PAGE_SIZE;
    }
}

void MemoryCache::RemoveStaticRegion(uint32_t pid, uint64_t address, size_t size) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    static_regions_.erase(
        std::remove_if(static_regions_.begin(), static_regions_.end(),
                       [pid, address, size](const StaticRegion& r) {
                           return r.pid == pid && r.start_address == address && r.size == size;
                       }),
        static_regions_.end());

    // Update static flag on remaining entries
    for (auto& [key, entry] : entries_) {
        if (entry.pid == pid) {
            entry.is_static = IsInStaticRegionLocked(entry.pid, entry.page_address, PAGE_SIZE);
        }
    }
}

bool MemoryCache::IsInStaticRegion(uint32_t pid, uint64_t address, size_t size) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return IsInStaticRegionLocked(pid, address, size);
}

bool MemoryCache::IsInStaticRegionLocked(uint32_t pid, uint64_t address, size_t size) const {
    for (const auto& region : static_regions_) {
        if (region.Overlaps(pid, address, size)) {
            return true;
        }
    }
    return false;
}

void MemoryCache::ClearStaticRegions() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    static_regions_.clear();
    for (auto& [key, entry] : entries_) {
        entry.is_static = false;
    }
}

void MemoryCache::EvictOneLRUEntryLocked() {
    if (entries_.empty()) return;

    // First try evicting any expired entries
    const auto now = std::chrono::steady_clock::now();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->second.IsExpired(ttl_, now)) {
            entries_.erase(it);
            evictions_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    // Find the LRU entry (minimum access_generation), preferring non-static entries
    auto oldest_it = entries_.end();
    uint64_t min_gen = UINT64_MAX;

    // First pass: look for non-static entries
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (!it->second.is_static && it->second.access_generation < min_gen) {
            min_gen = it->second.access_generation;
            oldest_it = it;
        }
    }

    // Second pass: if all entries are static, evict the oldest static entry
    if (oldest_it == entries_.end()) {
        min_gen = UINT64_MAX;
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.access_generation < min_gen) {
                min_gen = it->second.access_generation;
                oldest_it = it;
            }
        }
    }

    if (oldest_it != entries_.end()) {
        entries_.erase(oldest_it);
        evictions_.fetch_add(1, std::memory_order_relaxed);
    }
}

void MemoryCache::SetTTL(std::chrono::milliseconds ttl) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ttl_ = ttl;
}

std::chrono::milliseconds MemoryCache::GetTTL() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return ttl_;
}

void MemoryCache::SetMaxPages(size_t max_pages) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    max_pages_ = max_pages;
    while (entries_.size() > max_pages_) {
        EvictOneLRUEntryLocked();
    }
}

size_t MemoryCache::GetMaxPages() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return max_pages_;
}

void MemoryCache::SetEnabled(bool enabled) noexcept {
    enabled_.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        InvalidateAll();
    }
}

bool MemoryCache::IsEnabled() const noexcept {
    return enabled_.load(std::memory_order_relaxed);
}

CacheStats MemoryCache::GetStats() const {
    CacheStats stats;
    stats.hits = hits_.load(std::memory_order_relaxed);
    stats.misses = misses_.load(std::memory_order_relaxed);
    stats.evictions = evictions_.load(std::memory_order_relaxed);
    stats.invalidations = invalidations_.load(std::memory_order_relaxed);

    std::shared_lock<std::shared_mutex> lock(mutex_);
    stats.bytes_cached = entries_.size() * PAGE_SIZE;
    return stats;
}

void MemoryCache::ResetStats() {
    hits_.store(0, std::memory_order_relaxed);
    misses_.store(0, std::memory_order_relaxed);
    evictions_.store(0, std::memory_order_relaxed);
    invalidations_.store(0, std::memory_order_relaxed);
    read_bytes_saved_.store(0, std::memory_order_relaxed);
}

size_t MemoryCache::GetCacheSize() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return entries_.size();
}

} // namespace orpheus
