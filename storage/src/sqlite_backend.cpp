// storage/src/sqlite_backend.cpp
#include <storage/sqlite_backend.hpp>

#include <common/logger.hpp>

#include <sqlite3.h>

#include <chrono>
#include <stdexcept>
#include <string>

// ─── Helpers ─────────────────────────────────────────────────────────────────

static inline const char* col_text(sqlite3_stmt* s, int i) {
    const unsigned char* t = sqlite3_column_text(s, i);
    return t ? reinterpret_cast<const char*>(t) : "";
}

// ─── Schema ──────────────────────────────────────────────────────────────────

static constexpr const char* k_schema = R"SQL(
PRAGMA journal_mode = WAL;
PRAGMA synchronous  = NORMAL;
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS jobs (
    job_id               TEXT    PRIMARY KEY,
    job_type             TEXT    NOT NULL,
    payload              TEXT    NOT NULL DEFAULT '',
    priority             TEXT    NOT NULL DEFAULT 'NORMAL',
    status               TEXT    NOT NULL DEFAULT 'PENDING',
    created_at_ms        INTEGER NOT NULL,
    updated_at_ms        INTEGER NOT NULL,
    max_retries          INTEGER NOT NULL DEFAULT 3,
    retry_count          INTEGER NOT NULL DEFAULT 0,
    last_error           TEXT    NOT NULL DEFAULT '',
    lease_expires_at_ms  INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_jobs_status
    ON jobs (status);

CREATE INDEX IF NOT EXISTS idx_jobs_priority_created
    ON jobs (priority DESC, created_at_ms ASC);

CREATE INDEX IF NOT EXISTS idx_jobs_lease
    ON jobs (lease_expires_at_ms)
    WHERE lease_expires_at_ms > 0;
)SQL";

// Schema migration for existing databases without the lease column.
static constexpr const char* k_migrate_lease_col = R"SQL(
ALTER TABLE jobs ADD COLUMN lease_expires_at_ms INTEGER NOT NULL DEFAULT 0;
)SQL";

// ─── Prepared-statement SQL ───────────────────────────────────────────────────

static constexpr const char* k_sql_insert = R"SQL(
INSERT INTO jobs
    (job_id, job_type, payload, priority, status,
     created_at_ms, updated_at_ms, max_retries, retry_count, last_error,
     lease_expires_at_ms)
VALUES
    (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
)SQL";

static constexpr const char* k_sql_update_status = R"SQL(
UPDATE jobs
SET status = ?, last_error = ?, updated_at_ms = ?
WHERE job_id = ?;
)SQL";

static constexpr const char* k_sql_increment_retry = R"SQL(
UPDATE jobs
SET retry_count   = retry_count + 1,
    status        = 'PENDING',
    updated_at_ms = ?
WHERE job_id = ?;
)SQL";

static constexpr const char* k_sql_load_recoverable = R"SQL(
SELECT job_id, job_type, payload, priority, status,
       created_at_ms, updated_at_ms, max_retries, retry_count, last_error,
       lease_expires_at_ms
FROM   jobs
WHERE  status IN ('PENDING', 'RUNNING')
ORDER  BY priority DESC, created_at_ms ASC;
)SQL";

static constexpr const char* k_sql_get_by_id = R"SQL(
SELECT job_id, job_type, payload, priority, status,
       created_at_ms, updated_at_ms, max_retries, retry_count, last_error,
       lease_expires_at_ms
FROM   jobs
WHERE  job_id = ?;
)SQL";

static constexpr const char* k_sql_count = R"SQL(
SELECT COUNT(*) FROM jobs;
)SQL";

static constexpr const char* k_sql_set_lease = R"SQL(
UPDATE jobs SET lease_expires_at_ms = ?, updated_at_ms = ? WHERE job_id = ?;
)SQL";

static constexpr const char* k_sql_clear_lease = R"SQL(
UPDATE jobs SET lease_expires_at_ms = 0, updated_at_ms = ? WHERE job_id = ?;
)SQL";

static constexpr const char* k_sql_expired_leases = R"SQL(
SELECT job_id, job_type, payload, priority, status,
       created_at_ms, updated_at_ms, max_retries, retry_count, last_error,
       lease_expires_at_ms
FROM   jobs
WHERE  lease_expires_at_ms > 0 AND lease_expires_at_ms <= ?
ORDER  BY lease_expires_at_ms ASC;
)SQL";

// ─── Helper: current epoch milliseconds ──────────────────────────────────────

static int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// ─── Constructor / Destructor ─────────────────────────────────────────────────

SQLiteBackend::SQLiteBackend(const std::string& db_path)
    : db_path_(db_path) {

    // SQLITE_OPEN_FULLMUTEX: SQLite serialises per-connection internally too.
    int rc = sqlite3_open_v2(
        db_path.c_str(), &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);

    if (rc != SQLITE_OK) {
        const std::string msg = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("SQLiteBackend: cannot open '" + db_path + "': " + msg);
    }

    sqlite3_busy_timeout(db_, 5000 /*ms*/);

    if (auto r = init_schema(); r.err()) {
        throw std::runtime_error("SQLiteBackend: schema init failed: " + r.error());
    }
    if (auto r = prepare_statements(); r.err()) {
        throw std::runtime_error("SQLiteBackend: statement prep failed: " + r.error());
    }

    Logger::info("SQLiteBackend", "Opened database: " + db_path_);
}

SQLiteBackend::~SQLiteBackend() {
    finalize_statements();
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
        Logger::info("SQLiteBackend", "Closed database: " + db_path_);
    }
}

// ─── Schema init ─────────────────────────────────────────────────────────────

VoidResult SQLiteBackend::init_schema() {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, k_schema, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string msg = err_msg ? err_msg : "unknown";
        sqlite3_free(err_msg);
        return VoidResult::Err("init_schema: " + msg);
    }

    // Migration: add lease column to pre-Phase-3 databases.
    // ALTER TABLE silently succeeds if we catch "duplicate column" error.
    err_msg = nullptr;
    rc = sqlite3_exec(db_, k_migrate_lease_col, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK && err_msg) {
        std::string msg = err_msg;
        sqlite3_free(err_msg);
        // "duplicate column name" means column already exists — safe to ignore.
        if (msg.find("duplicate column") == std::string::npos)
            return VoidResult::Err("migrate lease col: " + msg);
    } else if (err_msg) {
        sqlite3_free(err_msg);
    }
    return VoidResult::Ok();
}

// ─── Statement preparation ───────────────────────────────────────────────────

VoidResult SQLiteBackend::prepare_statements() {
    struct Pair { sqlite3_stmt** stmt; const char* sql; };
    const Pair pairs[] = {
        { &stmt_insert_,             k_sql_insert           },
        { &stmt_update_status_,      k_sql_update_status    },
        { &stmt_increment_retry_,    k_sql_increment_retry  },
        { &stmt_load_recoverable_,   k_sql_load_recoverable },
        { &stmt_get_by_id_,          k_sql_get_by_id        },
        { &stmt_count_,              k_sql_count            },
        { &stmt_set_lease_,          k_sql_set_lease        },
        { &stmt_clear_lease_,        k_sql_clear_lease      },
        { &stmt_expired_leases_,     k_sql_expired_leases   },
    };

    for (const auto& [stmt_ptr, sql] : pairs) {
        const int rc = sqlite3_prepare_v2(db_, sql, -1, stmt_ptr, nullptr);
        if (rc != SQLITE_OK) {
            return VoidResult::Err(
                std::string("prepare_statements: ") + sqlite3_errmsg(db_));
        }
    }
    return VoidResult::Ok();
}

void SQLiteBackend::finalize_statements() noexcept {
    sqlite3_finalize(stmt_insert_);             stmt_insert_             = nullptr;
    sqlite3_finalize(stmt_update_status_);      stmt_update_status_      = nullptr;
    sqlite3_finalize(stmt_increment_retry_);    stmt_increment_retry_    = nullptr;
    sqlite3_finalize(stmt_load_recoverable_);   stmt_load_recoverable_   = nullptr;
    sqlite3_finalize(stmt_get_by_id_);          stmt_get_by_id_          = nullptr;
    sqlite3_finalize(stmt_count_);              stmt_count_              = nullptr;
    sqlite3_finalize(stmt_set_lease_);          stmt_set_lease_          = nullptr;
    sqlite3_finalize(stmt_clear_lease_);        stmt_clear_lease_        = nullptr;
    sqlite3_finalize(stmt_expired_leases_);     stmt_expired_leases_     = nullptr;
}

// ─── Row extraction ───────────────────────────────────────────────────────────

Job SQLiteBackend::row_to_job(sqlite3_stmt* stmt) {
    Job j;
    j.job_id               = col_text(stmt, 0);
    j.job_type             = col_text(stmt, 1);
    j.payload              = col_text(stmt, 2);
    j.priority             = priority_from_string(col_text(stmt, 3));
    j.status               = status_from_string(col_text(stmt, 4));
    j.created_at_ms        = sqlite3_column_int64(stmt, 5);
    j.updated_at_ms        = sqlite3_column_int64(stmt, 6);
    j.max_retries          = sqlite3_column_int(stmt, 7);
    j.retry_count          = sqlite3_column_int(stmt, 8);
    j.last_error           = col_text(stmt, 9);
    j.lease_expires_at_ms  = sqlite3_column_int64(stmt, 10);
    return j;
}

// ─── persist_job ─────────────────────────────────────────────────────────────

VoidResult SQLiteBackend::persist_job(const Job& job) {
    std::lock_guard<std::mutex> lk{db_mutex_};

    sqlite3_reset(stmt_insert_);
    sqlite3_clear_bindings(stmt_insert_);

    sqlite3_bind_text  (stmt_insert_, 1,  job.job_id.c_str(),              -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt_insert_, 2,  job.job_type.c_str(),            -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt_insert_, 3,  job.payload.c_str(),             -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt_insert_, 4,  to_string(job.priority).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt_insert_, 5,  to_string(job.status).c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt_insert_, 6,  job.created_at_ms);
    sqlite3_bind_int64 (stmt_insert_, 7,  job.updated_at_ms);
    sqlite3_bind_int   (stmt_insert_, 8,  job.max_retries);
    sqlite3_bind_int   (stmt_insert_, 9,  job.retry_count);
    sqlite3_bind_text  (stmt_insert_, 10, job.last_error.c_str(),          -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt_insert_, 11, job.lease_expires_at_ms);

    const int rc = sqlite3_step(stmt_insert_);
    if (rc != SQLITE_DONE) {
        return VoidResult::Err(std::string("persist_job: ") + sqlite3_errmsg(db_));
    }
    return VoidResult::Ok();
}

// ─── update_status ───────────────────────────────────────────────────────────

VoidResult SQLiteBackend::update_status(
    const std::string& job_id,
    JobStatus           status,
    const std::string&  error) {

    std::lock_guard<std::mutex> lk{db_mutex_};

    sqlite3_reset(stmt_update_status_);
    sqlite3_clear_bindings(stmt_update_status_);

    sqlite3_bind_text (stmt_update_status_, 1, to_string(status).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt_update_status_, 2, error.c_str(),             -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_update_status_, 3, now_ms());
    sqlite3_bind_text (stmt_update_status_, 4, job_id.c_str(),            -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt_update_status_);
    if (rc != SQLITE_DONE) {
        return VoidResult::Err(std::string("update_status: ") + sqlite3_errmsg(db_));
    }
    return VoidResult::Ok();
}

// ─── increment_retry ─────────────────────────────────────────────────────────

VoidResult SQLiteBackend::increment_retry(const std::string& job_id) {
    std::lock_guard<std::mutex> lk{db_mutex_};

    sqlite3_reset(stmt_increment_retry_);
    sqlite3_clear_bindings(stmt_increment_retry_);

    sqlite3_bind_int64(stmt_increment_retry_, 1, now_ms());
    sqlite3_bind_text (stmt_increment_retry_, 2, job_id.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt_increment_retry_);
    if (rc != SQLITE_DONE) {
        return VoidResult::Err(std::string("increment_retry: ") + sqlite3_errmsg(db_));
    }
    return VoidResult::Ok();
}

// ─── load_recoverable_jobs ───────────────────────────────────────────────────

Result<std::vector<Job>> SQLiteBackend::load_recoverable_jobs() {
    std::lock_guard<std::mutex> lk{db_mutex_};

    sqlite3_reset(stmt_load_recoverable_);

    std::vector<Job> jobs;
    int rc;
    while ((rc = sqlite3_step(stmt_load_recoverable_)) == SQLITE_ROW) {
        jobs.push_back(row_to_job(stmt_load_recoverable_));
    }
    if (rc != SQLITE_DONE) {
        return Result<std::vector<Job>>::Err(
            std::string("load_recoverable_jobs: ") + sqlite3_errmsg(db_));
    }
    return Result<std::vector<Job>>::Ok(std::move(jobs));
}

// ─── get_job ─────────────────────────────────────────────────────────────────

Result<std::optional<Job>> SQLiteBackend::get_job(const std::string& job_id) {
    std::lock_guard<std::mutex> lk{db_mutex_};

    sqlite3_reset(stmt_get_by_id_);
    sqlite3_clear_bindings(stmt_get_by_id_);

    sqlite3_bind_text(stmt_get_by_id_, 1, job_id.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt_get_by_id_);
    if (rc == SQLITE_ROW) {
        return Result<std::optional<Job>>::Ok(row_to_job(stmt_get_by_id_));
    }
    if (rc == SQLITE_DONE) {
        return Result<std::optional<Job>>::Ok(std::nullopt);
    }
    return Result<std::optional<Job>>::Err(
        std::string("get_job: ") + sqlite3_errmsg(db_));
}

// ─── total_job_count ─────────────────────────────────────────────────────────

Result<int64_t> SQLiteBackend::total_job_count() {
    std::lock_guard<std::mutex> lk{db_mutex_};

    sqlite3_reset(stmt_count_);

    const int rc = sqlite3_step(stmt_count_);
    if (rc == SQLITE_ROW) {
        return Result<int64_t>::Ok(sqlite3_column_int64(stmt_count_, 0));
    }
    return Result<int64_t>::Err(std::string("total_job_count: ") + sqlite3_errmsg(db_));
}

// ─── set_lease ───────────────────────────────────────────────────────────────

VoidResult SQLiteBackend::set_lease(const std::string& job_id,
                                     int64_t expires_at_ms) {
    std::lock_guard<std::mutex> lk{db_mutex_};
    sqlite3_reset(stmt_set_lease_);
    sqlite3_clear_bindings(stmt_set_lease_);
    sqlite3_bind_int64(stmt_set_lease_, 1, expires_at_ms);
    sqlite3_bind_int64(stmt_set_lease_, 2, now_ms());
    sqlite3_bind_text (stmt_set_lease_, 3, job_id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt_set_lease_);
    if (rc != SQLITE_DONE)
        return VoidResult::Err("set_lease: " + std::string(sqlite3_errmsg(db_)));
    return VoidResult::Ok();
}

// ─── clear_lease ─────────────────────────────────────────────────────────────

VoidResult SQLiteBackend::clear_lease(const std::string& job_id) {
    std::lock_guard<std::mutex> lk{db_mutex_};
    sqlite3_reset(stmt_clear_lease_);
    sqlite3_clear_bindings(stmt_clear_lease_);
    sqlite3_bind_int64(stmt_clear_lease_, 1, now_ms());
    sqlite3_bind_text (stmt_clear_lease_, 2, job_id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt_clear_lease_);
    if (rc != SQLITE_DONE)
        return VoidResult::Err("clear_lease: " + std::string(sqlite3_errmsg(db_)));
    return VoidResult::Ok();
}

// ─── get_expired_leases ───────────────────────────────────────────────────────

Result<std::vector<Job>> SQLiteBackend::get_expired_leases(int64_t now) {
    std::lock_guard<std::mutex> lk{db_mutex_};
    sqlite3_reset(stmt_expired_leases_);
    sqlite3_clear_bindings(stmt_expired_leases_);
    sqlite3_bind_int64(stmt_expired_leases_, 1, now);

    std::vector<Job> jobs;
    int rc;
    while ((rc = sqlite3_step(stmt_expired_leases_)) == SQLITE_ROW)
        jobs.push_back(row_to_job(stmt_expired_leases_));
    if (rc != SQLITE_DONE)
        return Result<std::vector<Job>>::Err(
            "get_expired_leases: " + std::string(sqlite3_errmsg(db_)));
    return Result<std::vector<Job>>::Ok(std::move(jobs));
}
