#pragma once

#include <list>
#include <unordered_map>
#include <mutex>
#include <cstddef>
#include <iostream>

using frame_id_t = size_t;

// LRU Replacer: Tracks unpinned frames eligible for eviction (Petrov Ch 5, Page 106)
class LRUReplacer {
private:
    std::mutex latch;
    std::list<frame_id_t> lru_list; // Front is Least Recently Used, Back is Most Recently Used
    std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> lru_map;
    size_t capacity;

public:
    LRUReplacer(size_t num_frames) : capacity(num_frames) {}

    // Find the least recently used frame and remove it from replacer
    bool victim(frame_id_t* frame_id) {
        std::lock_guard<std::mutex> lock(latch);
        if (lru_list.empty()) {
            return false; // No frames eligible for eviction (all frames pinned)
        }

        *frame_id = lru_list.front();
        lru_map.erase(*frame_id);
        lru_list.pop_front();
        return true;
    }

    // Pin a frame: Frame is being used by an active query, CANNOT be evicted
    void pin(frame_id_t frame_id) {
        std::lock_guard<std::mutex> lock(latch);
        auto it = lru_map.find(frame_id);
        if (it != lru_map.end()) {
            lru_list.erase(it->second);
            lru_map.erase(it);
        }
    }

    // Unpin a frame: Query finished with frame, CAN now be evicted if needed
    void unpin(frame_id_t frame_id) {
        std::lock_guard<std::mutex> lock(latch);
        if (lru_map.find(frame_id) != lru_map.end()) {
            return; // Already in replacer
        }

        lru_list.push_back(frame_id);
        lru_map[frame_id] = std::prev(lru_list.end());
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(latch);
        return lru_list.size();
    }
};
