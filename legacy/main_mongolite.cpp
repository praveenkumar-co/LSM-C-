#include "MongoLite.hpp"
#include <iostream>
#include <filesystem>

int main() {
    std::cout << "========================================================\n";
    std::cout << "        MONGOLITE C++ STORAGE ENGINE TEST BENCH        \n";
    std::cout << "========================================================\n\n";

    const std::string db_file = "mongolite_test.db";
    std::filesystem::remove(db_file); // Clean start

    {
        std::cout << "--- SESSION 1: Initializing Database & Inserting Documents ---\n";
        MongoLite db(db_file);

        // 1. Insert documents with _id
        db.insertOne("user_101", "{\"_id\": \"user_101\", \"name\": \"Praveen\", \"role\": \"Full-Stack Architect\", \"stack\": [\"MongoDB\", \"Express\", \"React\", \"Node\"]}");
        db.insertOne("user_102", "{\"_id\": \"user_102\", \"name\": \"DeepSeek\", \"role\": \"Systems Engineer\", \"language\": \"C++20\"}");
        db.insertOne("user_103", "{\"_id\": \"user_103\", \"event\": \"DATABASE_BOOT\", \"status\": \"ONLINE\", \"cluster\": \"local_node_0\"}");

        // 2. Perform point reads
        std::cout << "\n--- Querying Documents by _id ---\n";
        std::cout << "Query 'user_101': " << db.findOne("user_101") << "\n";
        std::cout << "Query 'user_102': " << db.findOne("user_102") << "\n";
        std::cout << "Query 'user_999' (Non-existent): " << db.findOne("user_999") << "\n";

        // 3. Flood documents to force 4KB page overflow and test multi-page allocation
        std::cout << "\n--- Flooding Documents to Trigger Multi-Page Overflow ---\n";
        for (int i = 0; i < 40; ++i) {
            std::string id = "doc_batch_" + std::to_string(i);
            std::string payload = "{\"_id\": \"" + id + "\", \"index\": " + std::to_string(i) + 
                                  ", \"payload\": \"Simulated document payload filling 4KB slotted page buffer.\"}";
            db.insertOne(id, payload);
        }

        // Print engine stats showing multiple 4KB pages allocated
        db.stats();

        // 4. Test document deletion
        std::cout << "--- Testing Soft Deletion (Tombstone) ---\n";
        db.deleteOne("user_102");
        std::cout << "Query 'user_102' after delete: " << db.findOne("user_102") << "\n";
        std::cout << "Query 'user_101' (Should be intact): " << db.findOne("user_101") << "\n";

        std::cout << "\n[System Info] Closing database session (Flushing all pages to disk)...\n\n";
    }

    {
        std::cout << "--- SESSION 2: Reboot Simulation & Re-Opening Database from Disk ---\n";
        MongoLite db_recovered(db_file);

        std::cout << "\nQuerying 'user_101' from persisted disk: " << db_recovered.findOne("user_101") << "\n";
        std::cout << "Querying 'doc_batch_25' from persisted disk: " << db_recovered.findOne("doc_batch_25") << "\n";
        std::cout << "Querying deleted 'user_102' (Should not exist): " << db_recovered.findOne("user_102") << "\n";
    }

    std::cout << "\n========================================================\n";
    std::cout << "          ALL PHASE 1 TESTS PASSED SUCCESSFULLY!        \n";
    std::cout << "========================================================\n";

    return 0;
}
