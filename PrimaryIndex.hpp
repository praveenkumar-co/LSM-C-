#pragma once

#include "DiskManager.hpp"
#include <string>
#include <map>
#include <vector>
#include <utility>
#include <optional>
#include <iostream>
#include <shared_mutex>
#include <mutex>

// Thread-Safe Primary Index mapping Document _id -> RecordId (Page_ID, Slot_Index)
// Supports Concurrent Reads via std::shared_lock and Exclusive Writes via std::unique_lock
class PrimaryIndex {
private:
    std::map<std::string, RecordId> index_map;
    mutable std::shared_mutex index_latch; // Reader-Writer Latch

public:
    PrimaryIndex() = default;

    // Insert an index entry under an Exclusive Write Lock
    bool insert(const std::string& id, const RecordId& rid) {
        std::unique_lock<std::shared_mutex> lock(index_latch);
        if (index_map.find(id) != index_map.end()) {
            std::cerr << "[PrimaryIndex] Duplicate key error on _id: " << id << "\n";
            return false; // Unique constraint violation (MongoDB standard)
        }
        index_map[id] = rid;
        return true;
    }

    // Point lookup by _id under a Shared Read Lock (Concurrent readers never block each other!)
    std::optional<RecordId> find(const std::string& id) const {
        std::shared_lock<std::shared_mutex> lock(index_latch);
        auto it = index_map.find(id);
        if (it == index_map.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    // Remove an index entry under an Exclusive Write Lock
    bool erase(const std::string& id) {
        std::unique_lock<std::shared_mutex> lock(index_latch);
        return index_map.erase(id) > 0;
    }

    size_t size() const {
        std::shared_lock<std::shared_mutex> lock(index_latch);
        return index_map.size();
    }

    // Range Scan: Returns list of (key, RecordId) pairs in range [start_key, end_key]
    // If end_key is empty (""), it scans from start_key to the end of the index.
    // If limit > 0, it stops after 'limit' matching records.
    // Concurrency: Shared Read Lock allows multiple concurrent range scans without blocking readers.
    std::vector<std::pair<std::string, RecordId>> findRange(
        const std::string& start_key, 
        const std::string& end_key = "", 
        size_t limit = 0
    ) const {
        std::shared_lock<std::shared_mutex> lock(index_latch);
        std::vector<std::pair<std::string, RecordId>> results;

        if (!end_key.empty() && start_key > end_key) {
            return results; // Inverted range: empty result
        }

        auto it = index_map.lower_bound(start_key);
        while (it != index_map.end()) {
            if (!end_key.empty() && it->first > end_key) {
                break;
            }
            results.push_back(*it);
            if (limit > 0 && results.size() >= limit) {
                break;
            }
            ++it;
        }
        return results;
    }

    // Prefix Scan: Returns list of (key, RecordId) pairs where key starts with 'prefix'
    // Exploits ordered B-Tree traversal: stops as soon as key no longer shares prefix
    std::vector<std::pair<std::string, RecordId>> findPrefix(
        const std::string& prefix, 
        size_t limit = 0
    ) const {
        std::shared_lock<std::shared_mutex> lock(index_latch);
        std::vector<std::pair<std::string, RecordId>> results;

        auto it = index_map.lower_bound(prefix);
        while (it != index_map.end()) {
            if (it->first.rfind(prefix, 0) != 0) {
                break; // Because keys are ordered, no further keys can start with prefix
            }
            results.push_back(*it);
            if (limit > 0 && results.size() >= limit) {
                break;
            }
            ++it;
        }
        return results;
    }

    // Diagnostics
    void printIndex() const {
        std::shared_lock<std::shared_mutex> lock(index_latch);
        std::cout << "\n--- [PRIMARY INDEX (_id -> RecordId)] ---\n";
        for (const auto& [id, rid] : index_map) {
            std::cout << "Key: \"" << id << "\" ---> " << rid.toString() << "\n";
        }
        std::cout << "------------------------------------------\n\n";
    }
};
