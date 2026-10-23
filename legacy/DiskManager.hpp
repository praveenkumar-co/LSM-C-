#pragma once

#include <string>
#include <fstream>
#include <iostream>
#include <mutex>
#include <cstdint>
#include <cstring>
#include "SlottedPage.hpp"

// Type aliases matching database systems standards
using page_id_t = uint32_t;
constexpr page_id_t INVALID_PAGE_ID = 0xFFFFFFFF;

// MongoDB RecordId / PostgreSQL ctid representation
struct RecordId {
    page_id_t page_id{INVALID_PAGE_ID};
    uint16_t slot_id{0};

    bool isValid() const {
        return page_id != INVALID_PAGE_ID;
    }

    std::string toString() const {
        return "RecordId(Page: " + std::to_string(page_id) + ", Slot: " + std::to_string(slot_id) + ")";
    }
};

class DiskManager {
private:
    std::string db_filename;
    std::fstream db_io;
    page_id_t num_pages{0};
    std::mutex db_io_latch; // Protects file seeks & multi-threaded I/O

public:
    DiskManager(const std::string& filename) : db_filename(filename) {
        // Open in binary read/write mode. If file doesn't exist, create it.
        db_io.open(db_filename, std::ios::binary | std::ios::in | std::ios::out);
        if (!db_io.is_open()) {
            // Create file if it does not exist
            db_io.clear();
            db_io.open(db_filename, std::ios::binary | std::ios::trunc | std::ios::out | std::ios::in);
            if (!db_io.is_open()) {
                std::cerr << "[DiskManager] Fatal: Could not create database file: " << db_filename << "\n";
                return;
            }
        }

        // Calculate total existing pages based on physical file size
        db_io.seekp(0, std::ios::end);
        std::streampos file_size = db_io.tellp();
        num_pages = static_cast<page_id_t>(file_size / PAGE_SIZE);

        std::cout << "[DiskManager] Initialized: " << db_filename 
                  << " (Size: " << file_size << " bytes, Total Pages: " << num_pages << ")\n";
    }

    ~DiskManager() {
        if (db_io.is_open()) {
            db_io.flush();
            db_io.close();
        }
    }

    // Allocate a brand-new 4KB page at the end of the file
    page_id_t allocatePage() {
        std::lock_guard<std::mutex> lock(db_io_latch);
        page_id_t new_page_id = num_pages;

        // Create an empty 4096-byte blank buffer
        uint8_t blank_buffer[PAGE_SIZE];
        std::memset(blank_buffer, 0, PAGE_SIZE);

        // Seek to the end of the file and write 4096 bytes
        db_io.seekp(static_cast<std::streamoff>(new_page_id) * PAGE_SIZE);
        db_io.write(reinterpret_cast<const char*>(blank_buffer), PAGE_SIZE);
        db_io.flush();

        num_pages++;
        return new_page_id;
    }

    // Write a 4KB SlottedPage to physical disk at its page_id offset
    bool writePage(page_id_t page_id, const uint8_t* page_data) {
        std::lock_guard<std::mutex> lock(db_io_latch);
        if (page_id >= num_pages) {
            std::cerr << "[DiskManager] Error: Attempting to write out-of-bounds page " << page_id << "\n";
            return false;
        }

        db_io.seekp(static_cast<std::streamoff>(page_id) * PAGE_SIZE);
        db_io.write(reinterpret_cast<const char*>(page_data), PAGE_SIZE);
        if (db_io.bad()) {
            std::cerr << "[DiskManager] Error: Failed to write page " << page_id << " to disk.\n";
            return false;
        }
        db_io.flush();
        return true;
    }

    // Read a 4KB page from physical disk at page_id offset into memory
    bool readPage(page_id_t page_id, uint8_t* page_data) {
        std::lock_guard<std::mutex> lock(db_io_latch);
        if (page_id >= num_pages) {
            std::cerr << "[DiskManager] Error: Attempting to read out-of-bounds page " << page_id << "\n";
            return false;
        }

        db_io.seekg(static_cast<std::streamoff>(page_id) * PAGE_SIZE);
        db_io.read(reinterpret_cast<char*>(page_data), PAGE_SIZE);

        std::streamsize bytes_read = db_io.gcount();
        if (bytes_read < static_cast<std::streamsize>(PAGE_SIZE)) {
            std::cerr << "[DiskManager] Warning: Partial page read (" << bytes_read << " bytes) on page " << page_id << "\n";
            return false;
        }
        return true;
    }

    page_id_t getNumPages() const {
        return num_pages;
    }

    const std::string& getFilename() const {
        return db_filename;
    }
};
