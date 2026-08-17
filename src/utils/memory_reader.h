#pragma once

#include "core/dma_interface.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <type_traits>

namespace orpheus {

/**
 * Type-erased or callable read callback.
 * Signature: std::vector<uint8_t>(uint64_t address, size_t size)
 */
using ReadMemoryFunc = std::function<std::vector<uint8_t>(uint64_t, size_t)>;

/**
 * ScatterReadCallback signature:
 * bool(std::vector<ScatterRequest>&)
 */
using ScatterReadFunc = std::function<bool(std::vector<ScatterRequest>&)>;

/**
 * ScatterBatch - Fluent utility to queue typed reads and execute them in a single DMA scatter transaction.
 */
class ScatterBatch {
public:
    ScatterBatch() = default;

    explicit ScatterBatch(ScatterReadFunc scatter_fn)
        : default_scatter_fn_(std::move(scatter_fn)) {}

    /**
     * Add a typed read to the batch with a callback.
     * @param address Target virtual address to read
     * @param callback Callback receiving std::optional<T> after execution
     * @return Reference to this batch for method chaining
     */
    template <typename T>
    ScatterBatch& Add(uint64_t address, std::function<void(const std::optional<T>&)> callback) {
        static_assert(std::is_trivially_copyable_v<T>, "Type T must be trivially copyable");
        size_t index = requests_.size();
        ScatterRequest req{};
        req.address = address;
        req.size = static_cast<uint32_t>(sizeof(T));
        req.success = false;
        requests_.push_back(req);

        handlers_.push_back([index, cb = std::move(callback)](const std::vector<ScatterRequest>& results) {
            const auto& r = results[index];
            if (r.success && r.data.size() >= sizeof(T)) {
                T val{};
                std::memcpy(&val, r.data.data(), sizeof(T));
                cb(val);
            } else {
                cb(std::nullopt);
            }
        });
        return *this;
    }

    /**
     * Add a typed read directly to a destination pointer.
     * @param address Target virtual address to read
     * @param out_ptr Pointer to write the value to if successful
     * @return Reference to this batch for method chaining
     */
    template <typename T>
    ScatterBatch& Add(uint64_t address, T* out_ptr) {
        static_assert(std::is_trivially_copyable_v<T>, "Type T must be trivially copyable");
        return Add<T>(address, [out_ptr](const std::optional<T>& val) {
            if (val && out_ptr) {
                *out_ptr = *val;
            }
        });
    }

    /**
     * Add a raw byte buffer read to the batch with a callback.
     * @param address Target virtual address to read
     * @param size Number of bytes to read
     * @param callback Callback receiving std::vector<uint8_t> (empty on failure) after execution
     * @return Reference to this batch for method chaining
     */
    ScatterBatch& AddBuffer(uint64_t address, size_t size, std::function<void(const std::vector<uint8_t>&)> callback) {
        size_t index = requests_.size();
        ScatterRequest req{};
        req.address = address;
        req.size = static_cast<uint32_t>(size);
        req.success = false;
        requests_.push_back(req);

        handlers_.push_back([index, cb = std::move(callback)](const std::vector<ScatterRequest>& results) {
            const auto& r = results[index];
            if (r.success) {
                cb(r.data);
            } else {
                cb({});
            }
        });
        return *this;
    }

    /**
     * Add a raw byte buffer read directly into a memory buffer.
     * @param address Target virtual address to read
     * @param out_buffer Pointer to destination buffer
     * @param size Number of bytes to read
     * @return Reference to this batch for method chaining
     */
    ScatterBatch& AddBuffer(uint64_t address, void* out_buffer, size_t size) {
        return AddBuffer(address, size, [out_buffer, size](const std::vector<uint8_t>& data) {
            if (out_buffer && data.size() >= size) {
                std::memcpy(out_buffer, data.data(), size);
            }
        });
    }

    /**
     * Get number of queued read requests.
     */
    [[nodiscard]] size_t Size() const noexcept {
        return requests_.size();
    }

    /**
     * Check if batch is empty.
     */
    [[nodiscard]] bool IsEmpty() const noexcept {
        return requests_.empty();
    }

    /**
     * Clear all queued requests.
     */
    void Clear() {
        requests_.clear();
        handlers_.clear();
    }

    /**
     * Execute batch using a ScatterReadFunc callback.
     * @param scatter_fn Callable taking std::vector<ScatterRequest>&
     * @return true if scatter execution succeeded
     */
    bool Execute(const ScatterReadFunc& scatter_fn) {
        if (requests_.empty()) {
            return true;
        }
        if (!scatter_fn) {
            return false;
        }
        bool ok = scatter_fn(requests_);
        for (const auto& handler : handlers_) {
            handler(requests_);
        }
        return ok;
    }

    /**
     * Execute batch using the default scatter function provided in constructor.
     */
    bool Execute() {
        if (requests_.empty()) {
            return true;
        }
        if (!default_scatter_fn_) {
            return false;
        }
        bool ok = default_scatter_fn_(requests_);
        for (const auto& handler : handlers_) {
            handler(requests_);
        }
        return ok;
    }

    /**
     * Execute batch using a sequential ReadMemoryFunc callback fallback.
     * Useful when only ReadMemoryFunc is available.
     * @param read_fn Callable taking (address, size)
     * @return true if at least one request succeeded or batch was empty
     */
    bool Execute(const ReadMemoryFunc& read_fn) {
        if (requests_.empty()) {
            return true;
        }
        if (!read_fn) {
            return false;
        }
        bool any_success = false;
        for (auto& req : requests_) {
            req.data = read_fn(req.address, req.size);
            req.success = (req.data.size() == req.size);
            if (req.success) {
                any_success = true;
            }
        }
        for (const auto& handler : handlers_) {
            handler(requests_);
        }
        return any_success;
    }

    /**
     * Execute batch via DMAInterface for a given PID.
     * @param dma Pointer to DMAInterface
     * @param pid Target process ID
     * @return true if scatter read succeeded
     */
    bool Execute(DMAInterface* dma, uint32_t pid);

    /**
     * Direct access to requests (for inspection or custom execution).
     */
    [[nodiscard]] const std::vector<ScatterRequest>& GetRequests() const noexcept { return requests_; }
    [[nodiscard]] std::vector<ScatterRequest>& GetRequests() noexcept { return requests_; }

private:
    ScatterReadFunc default_scatter_fn_;
    std::vector<ScatterRequest> requests_;
    std::vector<std::function<void(const std::vector<ScatterRequest>&)>> handlers_;
};

/**
 * MemoryReader - High-level memory reading utility
 *
 * Wraps low-level memory read functions (like DMAInterface::ReadMemory)
 * with type-safe, convenient methods for reading primitives, structures,
 * strings, pointer chains, and batched scatter reads.
 *
 * Example usage:
 *   MemoryReader reader([&](uint64_t addr, size_t size) {
 *       return dma->ReadMemory(pid, addr, size);
 *   });
 *
 *   uint32_t health = reader.ReadU32(player_addr + 0x100).value_or(0);
 *   std::string name = reader.ReadString(player_addr + 0x200, 32);
 *   auto pos = reader.ReadStruct<Vector3>(player_addr + 0x300);
 */
class MemoryReader {
public:
    /**
     * Construct with a memory read function
     */
    explicit MemoryReader(ReadMemoryFunc read_func)
        : read_func_(std::move(read_func)) {}

    /**
     * Construct with both sequential read function and scatter read function
     */
    MemoryReader(ReadMemoryFunc read_func, ScatterReadFunc scatter_func)
        : read_func_(std::move(read_func)), scatter_func_(std::move(scatter_func)) {}

    /**
     * Construct from DMAInterface and PID
     */
    MemoryReader(DMAInterface* dma, uint32_t pid);

    /**
     * Read raw bytes
     */
    std::vector<uint8_t> Read(uint64_t address, size_t size) const {
        if (!read_func_) return {};
        return read_func_(address, size);
    }

    /**
     * Read a typed value
     */
    template <typename T>
    std::optional<T> Read(uint64_t address) const {
        static_assert(std::is_trivially_copyable_v<T>, "Type T must be trivially copyable");
        auto data = Read(address, sizeof(T));
        if (data.size() < sizeof(T)) {
            return std::nullopt;
        }
        T value{};
        std::memcpy(&value, data.data(), sizeof(T));
        return value;
    }

    /**
     * Convenience methods for primitive types
     */
    std::optional<uint8_t>  ReadU8(uint64_t address)  const { return Read<uint8_t>(address); }
    std::optional<uint16_t> ReadU16(uint64_t address) const { return Read<uint16_t>(address); }
    std::optional<uint32_t> ReadU32(uint64_t address) const { return Read<uint32_t>(address); }
    std::optional<uint64_t> ReadU64(uint64_t address) const { return Read<uint64_t>(address); }
    std::optional<int8_t>   ReadI8(uint64_t address)  const { return Read<int8_t>(address); }
    std::optional<int16_t>  ReadI16(uint64_t address) const { return Read<int16_t>(address); }
    std::optional<int32_t>  ReadI32(uint64_t address) const { return Read<int32_t>(address); }
    std::optional<int64_t>  ReadI64(uint64_t address) const { return Read<int64_t>(address); }
    std::optional<float>    ReadFloat(uint64_t address) const { return Read<float>(address); }
    std::optional<double>   ReadDouble(uint64_t address) const { return Read<double>(address); }
    std::optional<uint64_t> ReadPtr(uint64_t address)    const { return ReadU64(address); }
    std::optional<uint64_t> ReadPointer(uint64_t address) const { return ReadU64(address); }

    /**
     * Read a struct of type T
     */
    template <typename T>
    std::optional<T> ReadStruct(uint64_t address) const {
        return Read<T>(address);
    }

    /**
     * Read a null-terminated ASCII/UTF-8 string
     * @param address Virtual address
     * @param max_length Maximum bytes to read (default 256)
     * @return String up to null terminator, or empty string on failure
     */
    std::string ReadString(uint64_t address, size_t max_length = 256) const {
        auto data = Read(address, max_length);
        if (data.empty()) return {};
        // Find null terminator
        size_t len = 0;
        while (len < data.size() && data[len] != '\0') {
            len++;
        }
        return std::string(reinterpret_cast<const char*>(data.data()), len);
    }

    /**
     * Read a null-terminated UTF-16 wide string
     */
    std::wstring ReadWString(uint64_t address, size_t max_length = 256) const {
        auto data = Read(address, max_length * 2);
        if (data.size() < 2) return {};
        size_t len = 0;
        const auto* ptr = reinterpret_cast<const wchar_t*>(data.data());
        size_t max_wchars = data.size() / 2;
        while (len < max_wchars && ptr[len] != L'\0') {
            len++;
        }
        return std::wstring(ptr, len);
    }

    /**
     * Read a pointer chain (base -> offset1 -> offset2 -> ... -> target)
     * @param base_address Starting address
     * @param offsets Sequence of offsets to follow
     * @param is_64bit true for 64-bit pointers (8 bytes), false for 32-bit (4 bytes)
     * @return Resolved address, or nullopt if any step fails
     */
    std::optional<uint64_t> ReadPointerChain(
        uint64_t base_address,
        const std::vector<uint64_t>& offsets,
        bool is_64bit = true) const
    {
        uint64_t current = base_address;
        for (size_t i = 0; i < offsets.size(); i++) {
            current += offsets[i];
            if (i < offsets.size() - 1) {
                // Not the last offset — dereference
                if (is_64bit) {
                    auto ptr = ReadU64(current);
                    if (!ptr || *ptr == 0) return std::nullopt;
                    current = *ptr;
                } else {
                    auto ptr = ReadU32(current);
                    if (!ptr || *ptr == 0) return std::nullopt;
                    current = *ptr;
                }
            }
        }
        return current;
    }

    /**
     * Read multiple pointers in batch
     */
    std::vector<std::optional<uint64_t>> ReadPointers(
        const std::vector<uint64_t>& addresses,
        bool is_64bit = true) const
    {
        if (is_64bit) {
            return ReadBatch<uint64_t>(addresses);
        } else {
            auto u32_results = ReadBatch<uint32_t>(addresses);
            std::vector<std::optional<uint64_t>> results;
            results.reserve(u32_results.size());
            for (const auto& opt : u32_results) {
                if (opt) {
                    results.emplace_back(static_cast<uint64_t>(*opt));
                } else {
                    results.emplace_back(std::nullopt);
                }
            }
            return results;
        }
    }

    /**
     * Create a new ScatterBatch pre-configured or ready to execute with this reader.
     */
    [[nodiscard]] ScatterBatch CreateBatch() const {
        return ScatterBatch();
    }

    /**
     * Execute a ScatterBatch using this reader's scatter or sequential read capabilities.
     */
    bool ExecuteBatch(ScatterBatch& batch) const {
        if (scatter_func_) {
            return batch.Execute(scatter_func_);
        }
        return batch.Execute(read_func_);
    }

    /**
     * Read a homogeneous batch of typed values across multiple addresses in a single operation.
     * @tparam T Trivially copyable type
     * @param addresses List of addresses to read
     * @return Vector of optional values matching input addresses
     */
    template <typename T>
    std::vector<std::optional<T>> ReadBatch(const std::vector<uint64_t>& addresses) const {
        static_assert(std::is_trivially_copyable_v<T>, "Type T must be trivially copyable");
        std::vector<std::optional<T>> results(addresses.size(), std::nullopt);
        if (addresses.empty()) {
            return results;
        }

        if (scatter_func_) {
            ScatterBatch batch;
            for (size_t i = 0; i < addresses.size(); ++i) {
                batch.Add<T>(addresses[i], [&results, i](const std::optional<T>& val) {
                    results[i] = val;
                });
            }
            batch.Execute(scatter_func_);
        } else {
            for (size_t i = 0; i < addresses.size(); ++i) {
                results[i] = Read<T>(addresses[i]);
            }
        }
        return results;
    }

    /**
     * Static helper to read a batch using a ScatterReadFunc
     */
    template <typename T>
    static std::vector<std::optional<T>> ReadBatch(const ScatterReadFunc& scatter_func, const std::vector<uint64_t>& addresses) {
        static_assert(std::is_trivially_copyable_v<T>, "Type T must be trivially copyable");
        std::vector<std::optional<T>> results(addresses.size(), std::nullopt);
        if (addresses.empty()) return results;

        ScatterBatch batch;
        for (size_t i = 0; i < addresses.size(); ++i) {
            batch.Add<T>(addresses[i], [&results, i](const std::optional<T>& val) {
                results[i] = val;
            });
        }
        batch.Execute(scatter_func);
        return results;
    }

    /**
     * Static helper to read a batch using DMAInterface directly
     */
    template <typename T>
    static std::vector<std::optional<T>> ReadBatch(DMAInterface* dma, uint32_t pid, const std::vector<uint64_t>& addresses) {
        static_assert(std::is_trivially_copyable_v<T>, "Type T must be trivially copyable");
        std::vector<std::optional<T>> results(addresses.size(), std::nullopt);
        if (addresses.empty() || !dma) return results;

        ScatterBatch batch;
        for (size_t i = 0; i < addresses.size(); ++i) {
            batch.Add<T>(addresses[i], [&results, i](const std::optional<T>& val) {
                results[i] = val;
            });
        }
        batch.Execute(dma, pid);
        return results;
    }

    /**
     * Check if a scatter reading function is available
     */
    [[nodiscard]] bool HasScatter() const noexcept {
        return scatter_func_ != nullptr;
    }

    /**
     * Set/update the scatter read function
     */
    void SetScatterFunc(ScatterReadFunc scatter_func) {
        scatter_func_ = std::move(scatter_func);
    }

    /**
     * Check if reader has a valid read function
     */
    [[nodiscard]] explicit operator bool() const { return read_func_ != nullptr; }

private:
    ReadMemoryFunc read_func_;
    ScatterReadFunc scatter_func_;
};

/**
 * Helper to create a MemoryReader with a lambda
 */
inline MemoryReader MakeReader(ReadMemoryFunc func) {
    return MemoryReader(std::move(func));
}

/**
 * Helper to create a MemoryReader with both sequential and scatter lambdas
 */
inline MemoryReader MakeReader(ReadMemoryFunc read_func, ScatterReadFunc scatter_func) {
    return MemoryReader(std::move(read_func), std::move(scatter_func));
}

} // namespace orpheus

// Include dma_interface.h after class declarations so inline DMA methods can resolve DMAInterface
#include "core/dma_interface.h"

namespace orpheus {

inline bool ScatterBatch::Execute(DMAInterface* dma, uint32_t pid) {
    if (requests_.empty()) {
        return true;
    }
    if (!dma || !dma->IsConnected()) {
        return false;
    }
    bool ok = dma->ScatterRead(pid, requests_);
    for (const auto& handler : handlers_) {
        handler(requests_);
    }
    return ok;
}

inline MemoryReader::MemoryReader(DMAInterface* dma, uint32_t pid)
    : read_func_([dma, pid](uint64_t addr, size_t size) -> std::vector<uint8_t> {
          if (!dma) return std::vector<uint8_t>{};
          return dma->ReadMemory(pid, addr, size);
      }),
      scatter_func_([dma, pid](std::vector<ScatterRequest>& requests) -> bool {
          if (!dma) return false;
          return dma->ScatterRead(pid, requests);
      }) {
}

inline MemoryReader MakeReader(DMAInterface* dma, uint32_t pid) {
    return MemoryReader(dma, pid);
}

namespace utils {
    using ::orpheus::ReadMemoryFunc;
    using ::orpheus::ScatterReadFunc;
    using ::orpheus::ScatterBatch;
    using ::orpheus::MemoryReader;
    using ::orpheus::MakeReader;
}

} // namespace orpheus
