#pragma once

#include <string>
#include <fstream>
#include <iostream>
#include <mutex>
#include <cstdint>
#include <vector>

enum class LogRecordType : uint8_t {
    INSERT = 1,
    DELETE = 2
};

struct LogRecord {
    uint64_t lsn;
    LogRecordType type;
    std::string key;
    std::string value;
};

// Write-Ahead Log Manager for ACID Durability (Petrov Ch 5, Page 111)
class WALManager {
private:
    std::string wal_filename;
    std::ofstream wal_out;
    uint64_t current_lsn{0};
    std::mutex wal_latch;

public:
    WALManager(const std::string& filename) : wal_filename(filename) {
        wal_out.open(wal_filename, std::ios::binary | std::ios::app);
        if (!wal_out.is_open()) {
            std::cerr << "[WALManager] Error: Failed to open WAL file: " << wal_filename << "\n";
        }
    }

    ~WALManager() {
        flush();
        if (wal_out.is_open()) {
            wal_out.close();
        }
    }

    // Append an insert mutation to the sequential WAL
    uint64_t logInsert(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(wal_latch);
        current_lsn++;

        uint8_t op = static_cast<uint8_t>(LogRecordType::INSERT);
        uint32_t k_len = static_cast<uint32_t>(key.size());
        uint32_t v_len = static_cast<uint32_t>(value.size());

        // Sequential Binary Serialization: [LSN (8B)][Op (1B)][KeyLen (4B)][Key][ValLen (4B)][Val]
        wal_out.write(reinterpret_cast<const char*>(&current_lsn), sizeof(current_lsn));
        wal_out.write(reinterpret_cast<const char*>(&op), sizeof(op));
        wal_out.write(reinterpret_cast<const char*>(&k_len), sizeof(k_len));
        wal_out.write(key.data(), k_len);
        wal_out.write(reinterpret_cast<const char*>(&v_len), sizeof(v_len));
        wal_out.write(value.data(), v_len);

        wal_out.flush(); // Enforce durability
        return current_lsn;
    }

    // Append a delete mutation to the sequential WAL
    uint64_t logDelete(const std::string& key) {
        std::lock_guard<std::mutex> lock(wal_latch);
        current_lsn++;

        uint8_t op = static_cast<uint8_t>(LogRecordType::DELETE);
        uint32_t k_len = static_cast<uint32_t>(key.size());
        uint32_t v_len = 0;

        wal_out.write(reinterpret_cast<const char*>(&current_lsn), sizeof(current_lsn));
        wal_out.write(reinterpret_cast<const char*>(&op), sizeof(op));
        wal_out.write(reinterpret_cast<const char*>(&k_len), sizeof(k_len));
        wal_out.write(key.data(), k_len);
        wal_out.write(reinterpret_cast<const char*>(&v_len), sizeof(v_len));

        wal_out.flush();
        return current_lsn;
    }

    void flush() {
        std::lock_guard<std::mutex> lock(wal_latch);
        if (wal_out.is_open()) {
            wal_out.flush();
        }
    }

    // Read all records from WAL during crash recovery
    static std::vector<LogRecord> readLog(const std::string& filename) {
        std::vector<LogRecord> records;
        std::ifstream in(filename, std::ios::binary);
        if (!in.is_open()) return records;

        while (in.peek() != EOF) {
            LogRecord rec;
            uint8_t op = 0;
            uint32_t k_len = 0, v_len = 0;

            if (!in.read(reinterpret_cast<char*>(&rec.lsn), sizeof(rec.lsn))) break;
            if (!in.read(reinterpret_cast<char*>(&op), sizeof(op))) break;
            rec.type = static_cast<LogRecordType>(op);

            if (!in.read(reinterpret_cast<char*>(&k_len), sizeof(k_len))) break;
            rec.key.resize(k_len);
            in.read(&rec.key[0], k_len);

            if (!in.read(reinterpret_cast<char*>(&v_len), sizeof(v_len))) break;
            if (v_len > 0) {
                rec.value.resize(v_len);
                in.read(&rec.value[0], v_len);
            }

            records.push_back(std::move(rec));
        }
        return records;
    }

    // Clear WAL after successful full database checkpoint
    void truncate() {
        std::lock_guard<std::mutex> lock(wal_latch);
        if (wal_out.is_open()) wal_out.close();
        wal_out.open(wal_filename, std::ios::binary | std::ios::trunc | std::ios::out);
    }
};
