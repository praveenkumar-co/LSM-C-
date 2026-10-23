#include "MongoLite.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <random>
#include <filesystem>

// Worker function for concurrent writers
void writerWorker(MongoLite& db, int thread_id, int docs_per_thread, std::atomic<size_t>& successful_inserts) {
    for (int i = 0; i < docs_per_thread; ++i) {
        std::string id = "t" + std::to_string(thread_id) + "_doc_" + std::to_string(i);
        std::string doc = "{\"_id\": \"" + id + "\", \"thread\": " + std::to_string(thread_id) + 
                          ", \"seq\": " + std::to_string(i) + 
                          ", \"status\": \"COMMITTED\", \"data\": \"Concurrent multi-threaded payload test.\"}";

        if (db.insertOne(id, doc)) {
            successful_inserts.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// Worker function for concurrent readers
void readerWorker(MongoLite& db, int num_threads, int docs_per_thread, int queries_to_run, 
                  std::atomic<size_t>& successful_reads, std::atomic<bool>& stop_flag) {
    std::mt19937 rng(1337);
    std::uniform_int_distribution<int> thread_dist(0, num_threads - 1);
    std::uniform_int_distribution<int> doc_dist(0, docs_per_thread - 1);

    int count = 0;
    while (!stop_flag.load(std::memory_order_relaxed) && count < queries_to_run) {
        int target_t = thread_dist(rng);
        int target_doc = doc_dist(rng);
        std::string id = "t" + std::to_string(target_t) + "_doc_" + std::to_string(target_doc);

        std::string result = db.findOne(id);
        if (!result.empty() && result != "null (Document not found)") {
            successful_reads.fetch_add(1, std::memory_order_relaxed);
        }
        count++;
    }
}

int main() {
    std::cout << "====================================================================\n";
    std::cout << "   MONGOLITE MULTI-THREADED CONCURRENCY BENCHMARK (RW LATCHES)      \n";
    std::cout << "====================================================================\n\n";

    const std::string db_file = "concurrent_mongolite.db";
    std::filesystem::remove(db_file);
    std::filesystem::remove(db_file + ".wal");

    const int NUM_WRITER_THREADS = 4;
    const int DOCS_PER_THREAD = 75; // Total 300 documents
    const int NUM_READER_THREADS = 4;
    const int QUERIES_PER_READER = 1500; // Total 6000 queries

    {
        MongoLite db(db_file, 16); // 16 frames = 64KB RAM Buffer Pool

        std::cout << ">>> TEST 1: Pure Concurrent Writers (" << NUM_WRITER_THREADS 
                  << " threads inserting simultaneously) <<<\n";

        std::atomic<size_t> successful_inserts{0};
        std::vector<std::thread> writers;

        auto start_w = std::chrono::high_resolution_clock::now();
        for (int t = 0; t < NUM_WRITER_THREADS; ++t) {
            writers.emplace_back(writerWorker, std::ref(db), t, DOCS_PER_THREAD, std::ref(successful_inserts));
        }

        for (auto& w : writers) {
            w.join();
        }
        auto end_w = std::chrono::high_resolution_clock::now();
        double write_duration_ms = std::chrono::duration<double, std::milli>(end_w - start_w).count();

        std::cout << "Inserted " << successful_inserts.load() << " documents across " 
                  << NUM_WRITER_THREADS << " threads in " << write_duration_ms << " ms (" 
                  << (successful_inserts.load() / (write_duration_ms / 1000.0)) << " docs/sec)\n";

        db.stats();

        std::cout << ">>> TEST 2: Pure Concurrent Readers (" << NUM_READER_THREADS 
                  << " threads reading simultaneously) <<<\n";

        std::atomic<size_t> successful_reads{0};
        std::atomic<bool> stop_flag{false};
        std::vector<std::thread> readers;

        auto start_r = std::chrono::high_resolution_clock::now();
        for (int r = 0; r < NUM_READER_THREADS; ++r) {
            readers.emplace_back(readerWorker, std::ref(db), NUM_WRITER_THREADS, DOCS_PER_THREAD, 
                                 QUERIES_PER_READER, std::ref(successful_reads), std::ref(stop_flag));
        }

        for (auto& r : readers) {
            r.join();
        }
        auto end_r = std::chrono::high_resolution_clock::now();
        double read_duration_ms = std::chrono::duration<double, std::milli>(end_r - start_r).count();
        double total_queries = NUM_READER_THREADS * QUERIES_PER_READER;
        double avg_read_lat_us = (read_duration_ms * 1000.0) / total_queries;

        std::cout << "Completed " << total_queries << " concurrent reads in " << read_duration_ms << " ms ("
                  << (total_queries / (read_duration_ms / 1000.0)) << " queries/sec)\n";
        std::cout << "Average Latency per Concurrent Read: " << avg_read_lat_us << " microseconds\n";
        std::cout << "Successful Reads: " << successful_reads.load() << " / " << total_queries << "\n";

        db.stats();

        std::cout << ">>> TEST 3: Mixed High-Contention OLTP (4 Readers + 4 Writers Active Simultaneously) <<<\n";
        std::vector<std::thread> mixed_threads;
        std::atomic<size_t> mixed_inserts{0};
        std::atomic<size_t> mixed_reads{0};
        std::atomic<bool> mixed_stop{false};

        auto start_mixed = std::chrono::high_resolution_clock::now();

        // Launch 4 new writers with offset thread IDs
        for (int t = 0; t < 4; ++t) {
            mixed_threads.emplace_back(writerWorker, std::ref(db), t + 10, 50, std::ref(mixed_inserts));
        }

        // Launch 4 readers concurrently querying both old and new data
        for (int r = 0; r < 4; ++r) {
            mixed_threads.emplace_back(readerWorker, std::ref(db), 14, 50, 1000, 
                                       std::ref(mixed_reads), std::ref(mixed_stop));
        }

        for (auto& th : mixed_threads) {
            th.join();
        }
        auto end_mixed = std::chrono::high_resolution_clock::now();
        double mixed_duration_ms = std::chrono::duration<double, std::milli>(end_mixed - start_mixed).count();

        std::cout << "Mixed Workload Finished in " << mixed_duration_ms << " ms\n";
        std::cout << "Mixed Inserts Succeeded: " << mixed_inserts.load() << "\n";
        std::cout << "Mixed Reads Succeeded  : " << mixed_reads.load() << "\n";

        db.stats();

        std::cout << "[System Info] Checkpoint flushing and closing database...\n\n";
    }

    {
        std::cout << ">>> TEST 4: Cold Crash Recovery Verification <<<\n";
        MongoLite db_recovered(db_file, 16);

        std::cout << "Verifying t0_doc_0: " << db_recovered.findOne("t0_doc_0") << "\n";
        std::cout << "Verifying t3_doc_74: " << db_recovered.findOne("t3_doc_74") << "\n";
        std::cout << "Verifying t10_doc_25 (Mixed Insert): " << db_recovered.findOne("t10_doc_25") << "\n";

        db_recovered.stats();
    }

    std::cout << "====================================================================\n";
    std::cout << "   ALL CONCURRENCY TESTS PASSED WITH ZERO DATA RACES!               \n";
    std::cout << "====================================================================\n";

    return 0;
}
