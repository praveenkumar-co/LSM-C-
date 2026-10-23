#pragma once

#include "BufferPoolManager.hpp"
#include "SlottedPage.hpp"
#include <string>
#include <iostream>
#include <mutex>

class DocumentStore {
private:
    BufferPoolManager& bpm;
    page_id_t active_page_id{INVALID_PAGE_ID};
    std::mutex insert_latch; // Protects active page appends and overflow allocations

public:
    DocumentStore(BufferPoolManager& buffer_mgr) : bpm(buffer_mgr) {
        DiskManager& dm = bpm.getDiskManager();
        if (dm.getNumPages() == 0) {
            SlottedPage* page = bpm.newPage(&active_page_id);
            if (page != nullptr) {
                bpm.unpinPage(active_page_id, true);
                std::cout << "[DocumentStore] Bootstrapped first page in Buffer Pool -> Page ID: " 
                          << active_page_id << "\n";
            }
        } else {
            active_page_id = dm.getNumPages() - 1;
            std::cout << "[DocumentStore] Active page set to Page ID: " << active_page_id << "\n";
        }
    }

    // Insert document using Buffer Pool caching (Thread-Safe across multi-threaded writers)
    RecordId insert(const std::string& key, const std::string& json_doc) {
        std::lock_guard<std::mutex> lock(insert_latch);

        SlottedPage* page = bpm.fetchPage(active_page_id);
        if (page == nullptr) {
            std::cerr << "[DocumentStore] Error: Failed to fetch active page " << active_page_id << "\n";
            return RecordId{INVALID_PAGE_ID, 0};
        }

        int slot_id = page->insertRecord(key, json_doc);

        // Page full -> Trigger page overflow
        if (slot_id == -1) {
            bpm.unpinPage(active_page_id, false); // Active page full, unpin clean

            // Allocate a new page in Buffer Pool
            page = bpm.newPage(&active_page_id);
            if (page == nullptr) {
                std::cerr << "[DocumentStore] Error: Buffer Pool failed to allocate new page.\n";
                return RecordId{INVALID_PAGE_ID, 0};
            }

            slot_id = page->insertRecord(key, json_doc);
            if (slot_id == -1) {
                bpm.unpinPage(active_page_id, true);
                std::cerr << "[DocumentStore] Error: Document exceeds maximum 4KB page capacity.\n";
                return RecordId{INVALID_PAGE_ID, 0};
            }
        }

        bpm.unpinPage(active_page_id, true); // Mark dirty, unpin
        return RecordId{active_page_id, static_cast<uint16_t>(slot_id)};
    }

    // Read document value through Buffer Pool (Concurrent reads run with zero mutex in DocumentStore!)
    std::string find(const RecordId& rid) {
        if (!rid.isValid()) return "";

        SlottedPage* page = bpm.fetchPage(rid.page_id);
        if (page == nullptr) {
            return "";
        }

        std::string val = page->getValue(rid.slot_id);
        bpm.unpinPage(rid.page_id, false); // Read-only, not dirty
        return val;
    }

    // Delete document through Buffer Pool
    bool erase(const RecordId& rid) {
        if (!rid.isValid()) return false;

        SlottedPage* page = bpm.fetchPage(rid.page_id);
        if (page == nullptr) return false;

        bool ok = page->deleteRecord(rid.slot_id);
        bpm.unpinPage(rid.page_id, ok); // Dirty if deletion succeeded
        return ok;
    }

    BufferPoolManager& getBufferPoolManager() { return bpm; }
};
