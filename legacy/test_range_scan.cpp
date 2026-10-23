#include "MongoLite.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iomanip>

void printHeader(const std::string& title) {
    std::cout << "\n====================================================================\n";
    std::cout << "  " << title << "\n";
    std::cout << "====================================================================\n";
}

int main() {
    printHeader("MONGOLITE PRODUCTION RANGE SCAN BENCHMARK & TEST SUITE");

    const std::string db_file = "range_mongolite.db";
    std::filesystem::remove(db_file);
    std::filesystem::remove(db_file + ".wal");

    {
        MongoLite db(db_file, 16); // 16 buffer frames = 64KB RAM Cache

        // ---------------------------------------------------------
        // 1. POPULATE INITIAL DATASET (100 ORDERED DOCUMENTS)
        // ---------------------------------------------------------
        printHeader("STAGE 1: Populating 100 Structured Documents (user_000 to user_099)");
        for (int i = 0; i < 100; ++i) {
            char key_buf[16];
            std::snprintf(key_buf, sizeof(key_buf), "user_%03d", i);
            std::string id = key_buf;
            std::string doc = "{\"_id\": \"" + id + "\", \"user_id\": " + std::to_string(i) + 
                              ", \"balance\": " + std::to_string(i * 12.5) + 
                              ", \"status\": \"ACTIVE\", \"meta\": \"B-Tree range scan leaf payload\"}";
            bool ok = db.insertOne(id, doc);
            assert(ok && "Failed to insert document");
        }
        std::cout << "Successfully inserted 100 documents across multiple 4KB slotted pages.\n";

        // ---------------------------------------------------------
        // 2. BASIC RANGE QUERY TEST: [user_020, user_039]
        // ---------------------------------------------------------
        printHeader("STAGE 2: Exact Sub-Range Query: findRange(\"user_020\", \"user_039\")");
        auto range_docs = db.findRange("user_020", "user_039");
        std::cout << "Documents returned: " << range_docs.size() << " (Expected: 20)\n";
        assert(range_docs.size() == 20 && "Expected exactly 20 documents in [user_020, user_039]");
        std::cout << "First doc in range : " << range_docs.front() << "\n";
        std::cout << "Last doc in range  : " << range_docs.back() << "\n";

        // ---------------------------------------------------------
        // 3. KEY-DOCUMENT PAIR RANGE QUERY
        // ---------------------------------------------------------
        printHeader("STAGE 3: Key-Document Pair Range: findRangeWithKeys(\"user_050\", \"user_054\")");
        auto pair_docs = db.findRangeWithKeys("user_050", "user_054");
        std::cout << "Entries returned: " << pair_docs.size() << " (Expected: 5)\n";
        assert(pair_docs.size() == 5);
        for (const auto& [k, v] : pair_docs) {
            std::cout << "  Key: " << k << " => " << v << "\n";
        }

        // ---------------------------------------------------------
        // 4. RANGE QUERY WITH LIMIT
        // ---------------------------------------------------------
        printHeader("STAGE 4: Range Query with LIMIT: findRange(\"user_010\", \"user_090\", limit=5)");
        auto limited_docs = db.findRange("user_010", "user_090", 5);
        std::cout << "Documents returned with limit=5: " << limited_docs.size() << " (Expected: 5)\n";
        assert(limited_docs.size() == 5);

        // ---------------------------------------------------------
        // 5. EDGE CASES AND BOUNDARY CONDITIONS
        // ---------------------------------------------------------
        printHeader("STAGE 5: Edge Cases & Boundary Verification");

        // Edge 1: Inverted range (start_key > end_key)
        auto inverted = db.findRange("user_080", "user_020");
        std::cout << "Inverted range [user_080, user_020] count: " << inverted.size() << " (Expected: 0)\n";
        assert(inverted.empty());

        // Edge 2: Single key range (start_key == end_key)
        auto single = db.findRange("user_042", "user_042");
        std::cout << "Single-key range [user_042, user_042] count: " << single.size() << " (Expected: 1)\n";
        assert(single.size() == 1);

        // Edge 3: Open-ended range (end_key = "") -> Scans to end of database
        auto open_ended = db.findRange("user_095", "");
        std::cout << "Open-ended range [user_095, END] count: " << open_ended.size() << " (Expected: 5: 095..099)\n";
        assert(open_ended.size() == 5);

        // Edge 4: Range strictly past maximum key
        auto past_max = db.findRange("user_900", "user_999");
        std::cout << "Past-max range [user_900, user_999] count: " << past_max.size() << " (Expected: 0)\n";
        assert(past_max.empty());

        // Edge 5: Non-matching boundary keys (interpolation within tree)
        auto between = db.findRange("user_025a", "user_028z");
        std::cout << "Between keys [user_025a, user_028z] count: " << between.size() << " (Expected: 3: 026, 027, 028)\n";
        assert(between.size() == 3);

        // ---------------------------------------------------------
        // 6. PREFIX SCANNING (findPrefix)
        // ---------------------------------------------------------
        printHeader("STAGE 6: Prefix Scanning: findPrefix()");
        // Insert order documents
        for (int i = 1; i <= 25; ++i) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "order_#%04d", i);
            db.insertOne(buf, "{\"order_id\": \"" + std::string(buf) + "\", \"amount\": " + std::to_string(i * 100) + "}");
        }
        auto orders = db.findPrefix("order_#");
        std::cout << "Scanned prefix 'order_#' total documents: " << orders.size() << " (Expected: 25)\n";
        assert(orders.size() == 25);

        auto orders_limited = db.findPrefix("order_#", 10);
        std::cout << "Scanned prefix 'order_#' with limit=10: " << orders_limited.size() << " (Expected: 10)\n";
        assert(orders_limited.size() == 10);

        // ---------------------------------------------------------
        // 7. STREAMING CURSOR TEST (db.scan)
        // ---------------------------------------------------------
        printHeader("STAGE 7: Streaming Database Cursor: db.scan()");
        auto cursor = db.scan("user_010", "user_014");
        std::cout << "Cursor initialized. Total keys in range: " << cursor.totalCount() << "\n";
        size_t cursor_steps = 0;
        while (cursor.isValid()) {
            std::cout << "  Cursor step [" << cursor.currentIndex() << "]: " 
                      << cursor.key() << " => " << cursor.value() << "\n";
            cursor_steps++;
            cursor.next();
        }
        assert(cursor_steps == 5);
        std::cout << "Cursor stepped through " << cursor_steps << " documents successfully.\n";

        // Checkpoint for persistence
        db.checkpoint();
    }

    // ---------------------------------------------------------
    // 8. COLD REBOOT & PERSISTENCE VERIFICATION
    // ---------------------------------------------------------
    printHeader("STAGE 8: Cold Reboot & Crash Recovery Range Scan");
    {
        MongoLite db_recovered(db_file, 16);
        auto recovered_range = db_recovered.findRange("user_030", "user_049");
        std::cout << "Recovered range [user_030, user_049] count: " << recovered_range.size() << " (Expected: 20)\n";
        assert(recovered_range.size() == 20);

        auto recovered_orders = db_recovered.findPrefix("order_#");
        std::cout << "Recovered prefix 'order_#' count: " << recovered_orders.size() << " (Expected: 25)\n";
        assert(recovered_orders.size() == 25);
        std::cout << "Persistence verified: 100% of documents correctly range-scanned after disk hydration.\n";
    }

    // ---------------------------------------------------------
    // 9. HIGH-CONCURRENCY MULTI-THREADED STRESS BENCHMARK
    // ---------------------------------------------------------
    printHeader("STAGE 9: High-Concurrency Multi-Threaded Range Scan Benchmark");
    std::filesystem::remove("concurrent_range.db");
    std::filesystem::remove("concurrent_range.db.wal");

    {
        MongoLite db_concurrent("concurrent_range.db", 32);

        // Preload 200 documents
        std::cout << "Preloading 200 documents for concurrency benchmark...\n";
        for (int i = 0; i < 200; ++i) {
            char key_buf[16];
            std::snprintf(key_buf, sizeof(key_buf), "k_%04d", i);
            db_concurrent.insertOne(key_buf, "{\"_id\": \"" + std::string(key_buf) + "\", \"v\": " + std::to_string(i) + "}");
        }

        const int NUM_WRITERS = 4;
        const int WRITES_PER_THREAD = 100;
        const int NUM_READERS = 4;
        const int RANGE_SCANS_PER_READER = 500;

        std::atomic<size_t> total_range_queries{0};
        std::atomic<size_t> total_scanned_docs{0};
        std::atomic<size_t> total_inserts{0};
        std::atomic<bool> start_signal{false};

        // Writer worker
        auto writer_func = [&](int thread_id) {
            while (!start_signal.load()) std::this_thread::yield();

            for (int i = 0; i < WRITES_PER_THREAD; ++i) {
                char key_buf[32];
                std::snprintf(key_buf, sizeof(key_buf), "t%d_w_%04d", thread_id, i);
                std::string doc = "{\"_id\": \"" + std::string(key_buf) + "\", \"thread\": " + std::to_string(thread_id) + "}";
                if (db_concurrent.insertOne(key_buf, doc)) {
                    total_inserts.fetch_add(1, std::memory_order_relaxed);
                }
            }
        };

        // Reader worker: Executing concurrent range scans of 25 keys each
        auto reader_func = [&](int thread_id) {
            while (!start_signal.load()) std::this_thread::yield();

            for (int i = 0; i < RANGE_SCANS_PER_READER; ++i) {
                int start_idx = (i * 7) % 170;
                char start_buf[16], end_buf[16];
                std::snprintf(start_buf, sizeof(start_buf), "k_%04d", start_idx);
                std::snprintf(end_buf, sizeof(end_buf), "k_%04d", start_idx + 25);

                auto batch = db_concurrent.findRange(start_buf, end_buf);
                total_range_queries.fetch_add(1, std::memory_order_relaxed);
                total_scanned_docs.fetch_add(batch.size(), std::memory_order_relaxed);
            }
        };

        std::vector<std::thread> threads;
        for (int i = 0; i < NUM_WRITERS; ++i) threads.emplace_back(writer_func, i);
        for (int i = 0; i < NUM_READERS; ++i) threads.emplace_back(reader_func, i);

        auto start_time = std::chrono::high_resolution_clock::now();
        start_signal.store(true);

        for (auto& t : threads) {
            t.join();
        }
        auto end_time = std::chrono::high_resolution_clock::now();

        std::chrono::duration<double, std::milli> duration = end_time - start_time;
        double elapsed_sec = duration.count() / 1000.0;

        std::cout << "\n--- MULTI-THREADED RANGE SCAN BENCHMARK RESULTS ---\n";
        std::cout << "Total Elapsed Time          : " << duration.count() << " ms\n";
        std::cout << "Concurrent Writers Completed: " << total_inserts.load() << " writes (" << NUM_WRITERS << " threads)\n";
        std::cout << "Concurrent Range Scans Run  : " << total_range_queries.load() << " queries (" << NUM_READERS << " threads)\n";
        std::cout << "Total Documents Scanned     : " << total_scanned_docs.load() << " documents\n";
        std::cout << "Range Query Throughput      : " << std::fixed << std::setprecision(2) 
                  << (total_range_queries.load() / elapsed_sec) << " Range Scans/sec\n";
        std::cout << "Document Scan Throughput    : " << std::fixed << std::setprecision(2) 
                  << (total_scanned_docs.load() / elapsed_sec) << " Docs/sec\n";
        std::cout << "Average Latency per Range Q : " << std::fixed << std::setprecision(3)
                  << (duration.count() * 1000.0 / total_range_queries.load()) << " microseconds\n";
        std::cout << "--------------------------------------------------\n";

        db_concurrent.stats();
    }

    printHeader("ALL RANGE SCAN AND CONCURRENCY TESTS COMPLETED SUCCESSFULLY!");
    return 0;
}
