#include "utils/memory_reader.h"
#include "analysis/memory_watcher.h"
#include "analysis/pe_dumper.h"
#include <gtest/gtest.h>

#include <vector>
#include <cstdint>
#include <cstring>
#include <map>

using namespace orpheus;
using namespace orpheus::utils;
using namespace orpheus::analysis;

class MemoryReaderBatchTest : public ::testing::Test {
protected:
    std::map<uint64_t, uint8_t> mock_memory;

    void SetUp() override {
        // Populate mock memory
        uint32_t val32 = 0x12345678;
        uint64_t val64 = 0xDEADBEEFCAFE0000ULL;
        float valf = 3.14159f;

        WriteMem(0x1000, &val32, sizeof(val32));
        WriteMem(0x2000, &val64, sizeof(val64));
        WriteMem(0x3000, &valf, sizeof(valf));

        const char* str = "Hello DMA";
        WriteMem(0x4000, str, std::strlen(str) + 1);
    }

    void WriteMem(uint64_t addr, const void* src, size_t size) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(src);
        for (size_t i = 0; i < size; ++i) {
            mock_memory[addr + i] = p[i];
        }
    }

    std::vector<uint8_t> ReadMem(uint64_t addr, size_t size) {
        std::vector<uint8_t> buf(size);
        for (size_t i = 0; i < size; ++i) {
            auto it = mock_memory.find(addr + i);
            if (it == mock_memory.end()) {
                return {};
            }
            buf[i] = it->second;
        }
        return buf;
    }
};

TEST_F(MemoryReaderBatchTest, BasicScatterBatchCallbacks) {
    auto read_fn = [this](uint64_t addr, size_t size) {
        return this->ReadMem(addr, size);
    };
    auto scatter_fn = [this](std::vector<ScatterRequest>& reqs) -> bool {
        for (auto& r : reqs) {
            auto bytes = this->ReadMem(r.address, r.size);
            if (bytes.size() == r.size) {
                r.data = bytes;
                r.success = true;
            } else {
                r.data.clear();
                r.success = false;
            }
        }
        return true;
    };

    ScatterBatch batch(scatter_fn);

    uint32_t res_u32 = 0;
    uint64_t res_u64 = 0;
    float res_f = 0.0f;
    std::vector<uint8_t> res_buf;

    batch.Add<uint32_t>(0x1000, [&](const std::optional<uint32_t>& val) {
        ASSERT_TRUE(val.has_value());
        res_u32 = *val;
    });

    batch.Add<uint64_t>(0x2000, [&](const std::optional<uint64_t>& val) {
        ASSERT_TRUE(val.has_value());
        res_u64 = *val;
    });

    batch.Add<float>(0x3000, [&](const std::optional<float>& val) {
        ASSERT_TRUE(val.has_value());
        res_f = *val;
    });

    batch.AddBuffer(0x4000, 9, [&](const std::vector<uint8_t>& buf) {
        res_buf = buf;
    });

    EXPECT_TRUE(batch.Execute());
    EXPECT_EQ(res_u32, 0x12345678);
    EXPECT_EQ(res_u64, 0xDEADBEEFCAFE0000ULL);
    EXPECT_FLOAT_EQ(res_f, 3.14159f);
    EXPECT_EQ(res_buf.size(), 9);
    EXPECT_EQ(std::string(res_buf.begin(), res_buf.end()), "Hello DMA");
}

TEST_F(MemoryReaderBatchTest, ReadBatchTemplate) {
    auto read_fn = [this](uint64_t addr, size_t size) {
        return this->ReadMem(addr, size);
    };
    auto scatter_fn = [this](std::vector<ScatterRequest>& reqs) -> bool {
        for (auto& r : reqs) {
            auto bytes = this->ReadMem(r.address, r.size);
            if (bytes.size() == r.size) {
                r.data = bytes;
                r.success = true;
            } else {
                r.data.clear();
                r.success = false;
            }
        }
        return true;
    };

    MemoryReader reader(read_fn, scatter_fn);

    std::vector<uint64_t> addrs = {0x1000, 0x2000, 0x9999}; // 0x9999 is invalid
    auto results = reader.ReadBatch<uint32_t>(addrs);

    ASSERT_EQ(results.size(), 3);
    ASSERT_TRUE(results[0].has_value());
    EXPECT_EQ(*results[0], 0x12345678);

    // Address 0x2000 has 8 bytes; reading 4 bytes as uint32_t gets low 32 bits (0xCAFE0000 on LE)
    ASSERT_TRUE(results[1].has_value());
    EXPECT_EQ(*results[1], 0xCAFE0000);

    EXPECT_FALSE(results[2].has_value());
}

TEST_F(MemoryReaderBatchTest, MemoryWatcherScatterPoll) {
    auto read_fn = [this](uint64_t addr, size_t size) {
        return this->ReadMem(addr, size);
    };

    bool scatter_called = false;
    auto scatter_fn = [this, &scatter_called](std::vector<ScatterRequest>& reqs) -> bool {
        scatter_called = true;
        for (auto& r : reqs) {
            auto bytes = this->ReadMem(r.address, r.size);
            if (bytes.size() == r.size) {
                r.data = bytes;
                r.success = true;
            } else {
                r.data.clear();
                r.success = false;
            }
        }
        return true;
    };

    MemoryWatcher watcher(read_fn, scatter_fn);

    uint32_t id1 = watcher.AddWatch(0x1000, 4, WatchType::Write, "health");
    EXPECT_GT(id1, 0u);

    // Initial poll
    std::vector<MemoryChange> first_changes = watcher.Poll();
    EXPECT_TRUE(scatter_called);
    EXPECT_TRUE(first_changes.empty());

    // Modify memory
    uint32_t new_val = 100;
    WriteMem(0x1000, &new_val, sizeof(new_val));

    // Poll again
    std::vector<MemoryChange> second_changes = watcher.Poll();
    EXPECT_EQ(second_changes.size(), 1ull);
    if (!second_changes.empty()) {
        EXPECT_EQ(second_changes[0].address, 0x1000ULL);
        EXPECT_EQ(second_changes[0].change_count, 1u);
    }
}

TEST_F(MemoryReaderBatchTest, PEDumperBatchSections) {
    // Construct minimal PE DOS + NT headers + 2 Section Headers
    std::vector<uint8_t> pe_data(0x1000, 0);

    PE_DOS_HEADER dos{};
    dos.e_magic = 0x5A4D; // MZ
    dos.e_lfanew = 0x80;
    std::memcpy(pe_data.data(), &dos, sizeof(dos));

    uint32_t signature = 0x00004550; // PE\0\0
    std::memcpy(pe_data.data() + 0x80, &signature, 4);

    PE_FILE_HEADER file_hdr{};
    file_hdr.Machine = 0x8664; // AMD64
    file_hdr.NumberOfSections = 2;
    file_hdr.SizeOfOptionalHeader = sizeof(PE_OPTIONAL_HEADER64);
    std::memcpy(pe_data.data() + 0x84, &file_hdr, sizeof(file_hdr));

    PE_OPTIONAL_HEADER64 opt_hdr{};
    opt_hdr.Magic = 0x20B; // PE32+
    opt_hdr.SizeOfImage = 0x4000;
    opt_hdr.SizeOfHeaders = 0x1000;
    opt_hdr.SectionAlignment = 0x1000;
    opt_hdr.FileAlignment = 0x200;
    std::memcpy(pe_data.data() + 0x84 + sizeof(file_hdr), &opt_hdr, sizeof(opt_hdr));

    uint64_t sec_offset = 0x84 + sizeof(file_hdr) + sizeof(opt_hdr);

    PE_SECTION_HEADER sec1{};
    std::memcpy(sec1.Name, ".text", 5);
    sec1.VirtualSize = 0x1000;
    sec1.VirtualAddress = 0x1000;
    sec1.SizeOfRawData = 0x1000;
    sec1.PointerToRawData = 0x400;
    std::memcpy(pe_data.data() + sec_offset, &sec1, sizeof(sec1));

    PE_SECTION_HEADER sec2{};
    std::memcpy(sec2.Name, ".data", 5);
    sec2.VirtualSize = 0x1000;
    sec2.VirtualAddress = 0x2000;
    sec2.SizeOfRawData = 0x1000;
    sec2.PointerToRawData = 0x1400;
    std::memcpy(pe_data.data() + sec_offset + sizeof(sec1), &sec2, sizeof(sec2));

    uint64_t pe_base = 0x140000000ULL;
    WriteMem(pe_base, pe_data.data(), pe_data.size());

    auto read_fn = [this](uint64_t addr, size_t size) {
        return this->ReadMem(addr, size);
    };

    PEDumper dumper(read_fn);
    EXPECT_TRUE(dumper.ParseHeaders(pe_base));

    auto sections = dumper.GetSections(pe_base);
    ASSERT_EQ(sections.size(), 2u);
    EXPECT_EQ(sections[0].name, ".text");
    EXPECT_EQ(sections[0].virtual_address, 0x1000u);
    EXPECT_EQ(sections[1].name, ".data");
    EXPECT_EQ(sections[1].virtual_address, 0x2000u);
}
