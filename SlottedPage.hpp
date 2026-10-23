#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>
#include <algorithm>

// Standard 4KB Database Page Size (as discussed in Petrov Ch 3, Page 72)
constexpr size_t PAGE_SIZE = 4096;
constexpr uint32_t PAGE_MAGIC = 0xD8DA7A01; // "DBDATA01" signature

#pragma pack(push, 1)

// Fixed-size Page Header (Petrov Page 73, Figure 3-5)
struct PageHeader {
    uint32_t magic;         // Magic bytes to detect corruption
    uint32_t page_id;       // Unique ID of this page in the DB file
    uint16_t num_slots;     // Number of records currently in this page
    uint16_t lower_offset;  // Boundary for slot pointers (grows left-to-right)
    uint16_t upper_offset;  // Boundary for data bytes (grows right-to-left)
};

// Slot entry in the Slot Directory (Petrov Page 74)
struct PageSlot {
    uint16_t offset;        // Byte offset within this page where record begins
    uint16_t length;        // Length of the record cell in bytes
};

#pragma pack(pop)

class SlottedPage {
private:
    uint8_t data[PAGE_SIZE]; // Exactly 4096 bytes of continuous memory

    PageHeader* getHeader() {
        return reinterpret_cast<PageHeader*>(data);
    }

    const PageHeader* getHeader() const {
        return reinterpret_cast<const PageHeader*>(data);
    }

    PageSlot* getSlots() {
        return reinterpret_cast<PageSlot*>(data + sizeof(PageHeader));
    }

    const PageSlot* getSlots() const {
        return reinterpret_cast<const PageSlot*>(data + sizeof(PageHeader));
    }

public:
    SlottedPage(uint32_t page_id = 0) {
        init(page_id);
    }

    void init(uint32_t page_id) {
        std::memset(data, 0, PAGE_SIZE);
        PageHeader* hdr = getHeader();
        hdr->magic = PAGE_MAGIC;
        hdr->page_id = page_id;
        hdr->num_slots = 0;
        hdr->lower_offset = sizeof(PageHeader);
        hdr->upper_offset = PAGE_SIZE; // Data starts filling from the very end
    }

    uint32_t getPageId() const {
        return getHeader()->page_id;
    }

    uint16_t getNumSlots() const {
        return getHeader()->num_slots;
    }

    // Free space available between the slot array and data boundary
    size_t getFreeSpace() const {
        const PageHeader* hdr = getHeader();
        if (hdr->upper_offset < hdr->lower_offset + sizeof(PageSlot)) {
            return 0;
        }
        return hdr->upper_offset - (hdr->lower_offset + sizeof(PageSlot));
    }

    // Insert a Key-Value Cell (Petrov Page 74, Cell Layout: [Key Len][Key][Val Len][Val])
    // Returns slot index, or -1 if page is full
    int insertRecord(const std::string& key, const std::string& value) {
        PageHeader* hdr = getHeader();
        uint16_t k_len = static_cast<uint16_t>(key.size());
        uint16_t v_len = static_cast<uint16_t>(value.size());
        uint16_t total_cell_len = sizeof(uint16_t) + k_len + sizeof(uint16_t) + v_len;

        // Check if both the slot pointer (4 bytes) AND cell body fit
        if (getFreeSpace() < total_cell_len) {
            return -1; // Page is full!
        }

        // 1. Allocate space from the end of the page (Right-to-Left)
        hdr->upper_offset -= total_cell_len;
        uint8_t* dest = data + hdr->upper_offset;

        // Write Key
        std::memcpy(dest, &k_len, sizeof(uint16_t));
        dest += sizeof(uint16_t);
        std::memcpy(dest, key.data(), k_len);
        dest += k_len;

        // Write Value
        std::memcpy(dest, &v_len, sizeof(uint16_t));
        dest += sizeof(uint16_t);
        std::memcpy(dest, value.data(), v_len);

        // 2. Add slot pointer to slot array (Left-to-Right)
        PageSlot* slots = getSlots();
        uint16_t slot_idx = hdr->num_slots;
        slots[slot_idx].offset = hdr->upper_offset;
        slots[slot_idx].length = total_cell_len;

        // 3. Move lower_offset forward
        hdr->num_slots++;
        hdr->lower_offset += sizeof(PageSlot);

        return slot_idx;
    }

    bool isDeleted(uint16_t slot_idx) const {
        const PageHeader* hdr = getHeader();
        if (slot_idx >= hdr->num_slots) return true;
        const PageSlot* slots = getSlots();
        return (slots[slot_idx].offset == 0 && slots[slot_idx].length == 0);
    }

    // Extract Key from Cell
    std::string getKey(uint16_t slot_idx) const {
        if (isDeleted(slot_idx)) return "";
        const PageSlot* slots = getSlots();
        const uint8_t* ptr = data + slots[slot_idx].offset;
        uint16_t k_len = 0;
        std::memcpy(&k_len, ptr, sizeof(uint16_t));
        return std::string(reinterpret_cast<const char*>(ptr + sizeof(uint16_t)), k_len);
    }

    // Extract Value (Document JSON) from Cell
    std::string getValue(uint16_t slot_idx) const {
        if (isDeleted(slot_idx)) return "[DELETED RECORD]";
        const PageSlot* slots = getSlots();
        const uint8_t* ptr = data + slots[slot_idx].offset;
        uint16_t k_len = 0;
        std::memcpy(&k_len, ptr, sizeof(uint16_t));
        ptr += sizeof(uint16_t) + k_len;

        uint16_t v_len = 0;
        std::memcpy(&v_len, ptr, sizeof(uint16_t));
        return std::string(reinterpret_cast<const char*>(ptr + sizeof(uint16_t)), v_len);
    }

    std::string getRecord(uint16_t slot_idx) const {
        return getValue(slot_idx);
    }

    // Soft delete: marks slot as deleted without breaking external pointers
    bool deleteRecord(uint16_t slot_idx) {
        PageHeader* hdr = getHeader();
        if (slot_idx >= hdr->num_slots) return false;

        PageSlot* slots = getSlots();
        slots[slot_idx].offset = 0;
        slots[slot_idx].length = 0;
        return true;
    }

    // Diagnostics: Print page visual layout
    void printLayout() const {
        const PageHeader* hdr = getHeader();
        std::cout << "\n========================================\n";
        std::cout << " SLOTTED PAGE (ID: " << hdr->page_id << ") LAYOUT\n";
        std::cout << "========================================\n";
        std::cout << "Magic: 0x" << std::hex << hdr->magic << std::dec << "\n";
        std::cout << "Total Records: " << hdr->num_slots << "\n";
        std::cout << "Slot Directory Ends at Byte: " << hdr->lower_offset << "\n";
        std::cout << "Data Boundary Begins at Byte: " << hdr->upper_offset << "\n";
        std::cout << "Free Space Remaining: " << getFreeSpace() << " Bytes\n";
        std::cout << "----------------------------------------\n";
        for (uint16_t i = 0; i < hdr->num_slots; ++i) {
            std::cout << "Slot [" << i << "]: Offset=" << getSlots()[i].offset 
                      << ", Len=" << getSlots()[i].length 
                      << " | Key: " << getKey(i)
                      << " | Value: " << getValue(i) << "\n";
        }
        std::cout << "========================================\n\n";
    }

    const uint8_t* rawData() const { return data; }
};
