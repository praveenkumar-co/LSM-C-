#pragma once

#include "DiskManager.hpp"
#include "BufferPoolManager.hpp"
#include "WALManager.hpp"
#include "DocumentStore.hpp"
#include "PrimaryIndex.hpp"
#include <string>
#include <vector>
#include <utility>
#include <iostream>

class MongoLite {
private:
    DiskManager disk_manager;
    BufferPoolManager bpm;
    WALManager wal;
    DocumentStore doc_store;
    PrimaryIndex primary_index;

    // Crash Recovery: Hydrate Primary Index from disk pages + replay WAL if needed
    void recoverAndRebuildIndex() {
        page_id_t total_pages = disk_manager.getNumPages();
        size_t recovered_count = 0;

        for (page_id_t pid = 0; pid < total_pages; ++pid) {
            SlottedPage* page = bpm.fetchPage(pid);
            if (page == nullptr) continue;

            uint16_t num_slots = page->getNumSlots();
            for (uint16_t s = 0; s < num_slots; ++s) {
                if (!page->isDeleted(s)) {
                    std::string key = page->getKey(s);
                    if (!key.empty()) {
                        primary_index.insert(key, RecordId{pid, s});
                        recovered_count++;
                    }
                }
            }
            bpm.unpinPage(pid, false);
        }

        if (recovered_count > 0) {
            std::cout << "[MongoLite] Recovered " << recovered_count 
                      << " documents across " << total_pages << " pages.\n";
        }
    }

public:
    MongoLite(const std::string& db_path, size_t buffer_pool_frames = 16) 
        : disk_manager(db_path), 
          bpm(buffer_pool_frames, disk_manager), 
          wal(db_path + ".wal"), 
          doc_store(bpm) {
        recoverAndRebuildIndex();
        std::cout << "[MongoLite] Storage Engine online (RAM Cache + WAL Durability Active).\n";
    }

    ~MongoLite() {
        checkpoint();
    }

    // Insert document: WAL first (durability), then Buffer Pool (speed)
    bool insertOne(const std::string& id, const std::string& json_doc) {
        if (primary_index.find(id).has_value()) {
            std::cerr << "[MongoLite] Error: Duplicate key violation on _id: " << id << "\n";
            return false;
        }

        // 1. Sequential Write-Ahead Log (WAL) for ACID Durability
        wal.logInsert(id, json_doc);

        // 2. Insert into in-memory Buffer Pool
        RecordId rid = doc_store.insert(id, json_doc);
        if (!rid.isValid()) {
            std::cerr << "[MongoLite] Error: Failed to insert into buffer pool.\n";
            return false;
        }

        // 3. Update Primary Index
        primary_index.insert(id, rid);
        return true;
    }

    // Find document: Cache-first lookup in Buffer Pool
    std::string findOne(const std::string& id) {
        auto rid_opt = primary_index.find(id);
        if (!rid_opt.has_value()) {
            return "null (Document not found)";
        }

        return doc_store.find(rid_opt.value());
    }

    // Range Query: Query batch documents in range [start_key, end_key] up to limit
    // E.g.: db.findRange("user_10", "user_50")
    std::vector<std::string> findRange(const std::string& start_key, const std::string& end_key = "", size_t limit = 0) {
        auto entries = primary_index.findRange(start_key, end_key, limit);
        std::vector<std::string> docs;
        docs.reserve(entries.size());

        for (const auto& [key, rid] : entries) {
            std::string doc = doc_store.find(rid);
            if (!doc.empty()) {
                docs.push_back(std::move(doc));
            }
        }
        return docs;
    }

    // Range Query returning (Key, JSON Document) pairs: Ideal for inspection and key-value drivers
    std::vector<std::pair<std::string, std::string>> findRangeWithKeys(
        const std::string& start_key, 
        const std::string& end_key = "", 
        size_t limit = 0
    ) {
        auto entries = primary_index.findRange(start_key, end_key, limit);
        std::vector<std::pair<std::string, std::string>> docs;
        docs.reserve(entries.size());

        for (const auto& [key, rid] : entries) {
            std::string doc = doc_store.find(rid);
            if (!doc.empty()) {
                docs.emplace_back(key, std::move(doc));
            }
        }
        return docs;
    }

    // Prefix Query: Query batch documents whose _id starts with 'prefix'
    // E.g.: db.findPrefix("user_", 20)
    std::vector<std::string> findPrefix(const std::string& prefix, size_t limit = 0) {
        auto entries = primary_index.findPrefix(prefix, limit);
        std::vector<std::string> docs;
        docs.reserve(entries.size());

        for (const auto& [key, rid] : entries) {
            std::string doc = doc_store.find(rid);
            if (!doc.empty()) {
                docs.push_back(std::move(doc));
            }
        }
        return docs;
    }

    // Prefix Query returning (Key, JSON Document) pairs
    std::vector<std::pair<std::string, std::string>> findPrefixWithKeys(const std::string& prefix, size_t limit = 0) {
        auto entries = primary_index.findPrefix(prefix, limit);
        std::vector<std::pair<std::string, std::string>> docs;
        docs.reserve(entries.size());

        for (const auto& [key, rid] : entries) {
            std::string doc = doc_store.find(rid);
            if (!doc.empty()) {
                docs.emplace_back(key, std::move(doc));
            }
        }
        return docs;
    }

    // Production-Grade Database Cursor for streaming batch iteration
    class Cursor {
    private:
        MongoLite& db;
        std::vector<std::pair<std::string, RecordId>> entries;
        size_t current_idx{0};

    public:
        Cursor(MongoLite& db, std::vector<std::pair<std::string, RecordId>> entries)
            : db(db), entries(std::move(entries)), current_idx(0) {}

        bool isValid() const {
            return current_idx < entries.size();
        }

        void next() {
            if (isValid()) {
                current_idx++;
            }
        }

        std::string key() const {
            if (!isValid()) return "";
            return entries[current_idx].first;
        }

        std::string value() const {
            if (!isValid()) return "";
            return db.doc_store.find(entries[current_idx].second);
        }

        RecordId rid() const {
            if (!isValid()) return RecordId{INVALID_PAGE_ID, 0};
            return entries[current_idx].second;
        }

        size_t totalCount() const {
            return entries.size();
        }

        size_t currentIndex() const {
            return current_idx;
        }
    };

    // Open a streaming cursor over a range of keys
    Cursor scan(const std::string& start_key, const std::string& end_key = "", size_t limit = 0) {
        auto entries = primary_index.findRange(start_key, end_key, limit);
        return Cursor(*this, std::move(entries));
    }

    // Delete document: WAL log + in-memory soft deletion
    bool deleteOne(const std::string& id) {
        auto rid_opt = primary_index.find(id);
        if (!rid_opt.has_value()) {
            return false;
        }

        wal.logDelete(id);
        bool ok = doc_store.erase(rid_opt.value());
        if (ok) {
            primary_index.erase(id);
        }
        return ok;
    }

    // Checkpoint: Flush all dirty pages from RAM to disk and truncate WAL
    void checkpoint() {
        bpm.flushAllPages();
        wal.truncate();
    }

    // Diagnostics & Metrics
    void stats() {
        std::cout << "\n============================================\n";
        std::cout << "           MONGOLITE ENGINE STATS           \n";
        std::cout << "============================================\n";
        std::cout << "Database File   : " << disk_manager.getFilename() << "\n";
        std::cout << "Allocated Pages : " << disk_manager.getNumPages() << " (4KB each)\n";
        std::cout << "Disk Storage    : " << (disk_manager.getNumPages() * PAGE_SIZE) << " Bytes\n";
        std::cout << "Live Documents  : " << primary_index.size() << "\n";
        bpm.printStats();
        std::cout << "============================================\n\n";
    }

    BufferPoolManager& getBufferPoolManager() { return bpm; }
};
