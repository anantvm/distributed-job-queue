#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// sqlite_backend.hpp
//
// SQLite3-backed implementation of IStorageBackend.
//
// Notable choices:
//   • WAL (Write-Ahead Log) mode: concurrent reads don’t block writes.
//   • PRAGMA synchronous = NORMAL: balances durability and throughput.
//   • All writes use prepared statements (no string interpolation → SQL-safe).
//   • A std::mutex serialises all API calls because SQLite’s default
//     threading mode is “SERIALIZED” per connection — even if the library
//     was compiled with SQLITE_THREADSAFE=1, using a single connection from
//     multiple threads requires external locking to avoid SQLITE_BUSY errors.
// ─────────────────────────────────────────────────────────────────────────────

#include <storage/i_storage_backend.hpp>

#include <mutex>
#include <sqlite3.h>
#include <string>

class SQLiteBackend final : public IStorageBackend {
public:
    // Opens (and creates if necessary) the database at db_path.
    // Use ":memory:" for an ephemeral in-process database.
    explicit SQLiteBackend(const std::string& db_path);
    ~SQLiteBackend() override;

    // Non-copyable (owns a raw sqlite3* handle)
    SQLiteBackend(const SQLiteBackend&)            = delete;
    SQLiteBackend& operator=(const SQLiteBackend&) = delete;

    // Non-movable (mutex is not movable; always use via unique_ptr)
    SQLiteBackend(SQLiteBackend&&)            = delete;
    SQLiteBackend& operator=(SQLiteBackend&&) = delete;

    [[nodiscard]] VoidResult persist_job(const Job& job) override;

    [[nodiscard]] VoidResult update_status(
        const std::string& job_id,
        JobStatus           status,
        const std::string&  error = "") override;

    [[nodiscard]] VoidResult increment_retry(const std::string& job_id) override;

    [[nodiscard]] Result<std::vector<Job>> load_recoverable_jobs() override;

    [[nodiscard]] Result<std::optional<Job>> get_job(const std::string& job_id) override;

    [[nodiscard]] Result<int64_t> total_job_count() override;

private:
    sqlite3*            db_{nullptr};
    std::string         db_path_;
    mutable std::mutex  db_mutex_;   // serialises all SQLite API calls

    // Prepared statements (compiled once, reused many times)
    sqlite3_stmt*  stmt_insert_{nullptr};
    sqlite3_stmt*  stmt_update_status_{nullptr};
    sqlite3_stmt*  stmt_increment_retry_{nullptr};
    sqlite3_stmt*  stmt_load_recoverable_{nullptr};
    sqlite3_stmt*  stmt_get_by_id_{nullptr};
    sqlite3_stmt*  stmt_count_{nullptr};

    [[nodiscard]] VoidResult init_schema();
    [[nodiscard]] VoidResult prepare_statements();
    void                     finalize_statements() noexcept;

    // Extracts a Job from the current row of a SELECT statement.
    [[nodiscard]] static Job row_to_job(sqlite3_stmt* stmt);
};
