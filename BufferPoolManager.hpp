#pragma once

#include "DiskManager.hpp"
#include "SlottedPage.hpp"
#include "LRUReplacer.hpp"
#include <vector>
#include <unordered_map>
#include <list>
#include <mutex>
#include <iostream>

struct Frame {
    SlottedPage page;
    page_id_t page_id{INVALID_PAGE_ID};
    int pin_count{0};
    bool is_dirty{false};

    void reset() {
        page_id = INVALID_PAGE_ID;
        pin_count = 0;
        is_dirty = false;
    }
};

class BufferPoolManager {
private:
    size_t pool_size;
    DiskManager& disk_manager;
    std::vector<Frame> frames;
    std::unordered_map<page_id_t, frame_id_t> page_table; // page_id -> frame_id
    std::list<frame_id_t> free_list;                       // Unused frames
    LRUReplacer replacer;
    std::mutex bpm_latch;

    // Performance Metrics
    size_t cache_hits{0};
    size_t cache_misses{0};

    // Helper: Find an available frame (either from free list or via LRU eviction)
    bool findAvailableFrame(frame_id_t* out_frame_id) {
        // 1. Try free list first
        if (!free_list.empty()) {
            *out_frame_id = free_list.front();
            free_list.pop_front();
            return true;
        }

        // 2. Try evicting an unpinned frame using LRU
        if (replacer.victim(out_frame_id)) {
            Frame& victim = frames[*out_frame_id];

            // If the victim page is dirty, flush it to disk first (Petrov Steal Policy)
            if (victim.is_dirty) {
                disk_manager.writePage(victim.page_id, victim.page.rawData());
                victim.is_dirty = false;
            }

            // Remove old page from page table
            page_table.erase(victim.page_id);
            victim.reset();
            return true;
        }

        std::cerr << "[BufferPoolManager] Fatal Error: Buffer pool is pinned completely! No evictable frames.\n";
        return false;
    }

public:
    BufferPoolManager(size_t pool_size, DiskManager& dm) 
        : pool_size(pool_size), disk_manager(dm), frames(pool_size), replacer(pool_size) {
        for (size_t i = 0; i < pool_size; ++i) {
            free_list.push_back(i);
        }
        std::cout << "[BufferPoolManager] Initialized with " << pool_size 
                  << " frames (" << (pool_size * PAGE_SIZE / 1024) << " KB RAM Cache)\n";
    }

    ~BufferPoolManager() {
        flushAllPages();
    }

    // Fetch a 4KB page into RAM (Cache-First Read Path)
    SlottedPage* fetchPage(page_id_t page_id) {
        std::lock_guard<std::mutex> lock(bpm_latch);

        // 1. Check if page is already cached in RAM (CACHE HIT!)
        auto it = page_table.find(page_id);
        if (it != page_table.end()) {
            cache_hits++;
            frame_id_t frame_id = it->second;
            frames[frame_id].pin_count++;
            replacer.pin(frame_id);
            return &frames[frame_id].page;
        }

        // 2. Page is not in RAM (CACHE MISS!) -> Read from disk
        cache_misses++;
        frame_id_t frame_id;
        if (!findAvailableFrame(&frame_id)) {
            return nullptr;
        }

        Frame& frame = frames[frame_id];
        frame.page_id = page_id;
        frame.pin_count = 1;
        frame.is_dirty = false;

        // Fetch physical page from disk
        if (!disk_manager.readPage(page_id, const_cast<uint8_t*>(frame.page.rawData()))) {
            std::cerr << "[BufferPoolManager] Error: Failed to read page " << page_id << " from disk.\n";
            free_list.push_back(frame_id);
            return nullptr;
        }

        page_table[page_id] = frame_id;
        replacer.pin(frame_id);
        return &frame.page;
    }

    // Allocate a brand-new page directly into the Buffer Pool
    SlottedPage* newPage(page_id_t* out_page_id) {
        std::lock_guard<std::mutex> lock(bpm_latch);

        frame_id_t frame_id;
        if (!findAvailableFrame(&frame_id)) {
            return nullptr;
        }

        // Allocate physically on disk first
        page_id_t new_page_id = disk_manager.allocatePage();
        *out_page_id = new_page_id;

        Frame& frame = frames[frame_id];
        frame.page.init(new_page_id);
        frame.page_id = new_page_id;
        frame.pin_count = 1;
        frame.is_dirty = true; // Newly allocated page is dirty

        page_table[new_page_id] = frame_id;
        replacer.pin(frame_id);

        return &frame.page;
    }

    // Unpin page: Mark query complete, update dirty status
    bool unpinPage(page_id_t page_id, bool is_dirty) {
        std::lock_guard<std::mutex> lock(bpm_latch);

        auto it = page_table.find(page_id);
        if (it == page_table.end()) {
            return false;
        }

        frame_id_t frame_id = it->second;
        Frame& frame = frames[frame_id];

        if (is_dirty) {
            frame.is_dirty = true;
        }

        if (frame.pin_count <= 0) {
            return false;
        }

        frame.pin_count--;
        if (frame.pin_count == 0) {
            replacer.unpin(frame_id);
        }

        return true;
    }

    // Flush a specific page to disk if dirty
    bool flushPage(page_id_t page_id) {
        std::lock_guard<std::mutex> lock(bpm_latch);

        auto it = page_table.find(page_id);
        if (it == page_table.end()) return false;

        Frame& frame = frames[it->second];
        if (frame.is_dirty) {
            disk_manager.writePage(frame.page_id, frame.page.rawData());
            frame.is_dirty = false;
        }
        return true;
    }

    // Flush all dirty pages to disk (Checkpointing)
    void flushAllPages() {
        std::lock_guard<std::mutex> lock(bpm_latch);
        size_t flushed_count = 0;
        for (auto& frame : frames) {
            if (frame.page_id != INVALID_PAGE_ID && frame.is_dirty) {
                disk_manager.writePage(frame.page_id, frame.page.rawData());
                frame.is_dirty = false;
                flushed_count++;
            }
        }
        if (flushed_count > 0) {
            std::cout << "[BufferPoolManager] Flushed " << flushed_count << " dirty pages to disk.\n";
        }
    }

    // Performance Metrics
    double getCacheHitRatio() const {
        size_t total = cache_hits + cache_misses;
        if (total == 0) return 0.0;
        return (static_cast<double>(cache_hits) / total) * 100.0;
    }

    void printStats() const {
        std::cout << "\n--- BUFFER POOL PERFORMANCE METRICS ---\n";
        std::cout << "Cache Hits       : " << cache_hits << "\n";
        std::cout << "Cache Misses     : " << cache_misses << "\n";
        std::cout << "Cache Hit Ratio  : " << getCacheHitRatio() << " %\n";
        std::cout << "Active In-RAM    : " << page_table.size() << " pages\n";
        std::cout << "--------------------------------------\n\n";
    }

    DiskManager& getDiskManager() { return disk_manager; }
};
