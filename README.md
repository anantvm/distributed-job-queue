# High-Performance Distributed Job Queue

> **Phase 1 — Single-Node Queue** | C++20 | CMake | SQLite

A placement-worthy systems project demonstrating Distributed Systems foundations,
Concurrency, Storage, and Performance Engineering in modern C++.

---

## Table of Contents
1. [Project Overview](#1-project-overview)
2. [Architecture](#2-architecture)
3. [File Guide](#3-file-guide)
4. [Threading Model](#4-threading-model)
5. [Building](#5-building)
6. [Running](#6-running)
7. [Tests](#7-tests)
8. [C++20 Features Used](#8-c20-features-used)
9. [Phase Roadmap](#9-phase-roadmap)

---

## 1. Project Overview

This is Phase 1 of a 6-phase project culminating in a fully distributed,
fault-tolerant job queue. In Phase 1 everything runs in a **single process**
with an in-memory priority queue backed by **SQLite persistence**.

### What Phase 1 demonstrates

| Feature | Mechanism |
|---------|-----------|
| Thread-safe priority queue | `std::priority_queue` + `std::mutex` + `std::condition_variable_any` |
| Blocking consumer with clean shutdown | `std::stop_token` (C++20) + `std::jthread` |
| Write-ahead durability | Job persisted to SQLite *before* enqueue |
| Startup recovery | Manager reloads PENDING/RUNNING jobs on boot |
| Retry with backoff | `fail_job` re-enqueues if `retry_count < max_retries` |
| Concurrent workers | Multiple `std::jthread` workers polling the same queue |
| Handler plugin system | `HandlerRegistry` maps `job_type → std::function` |
| Storage abstraction | `IStorageBackend` interface → swap SQLite for PostgreSQL later |

---

## 2. Architecture

```
┌──────────────────────────────────────────┐
│          phase1_demo (main)              │
│                                          │
│  submit_job("fibonacci_job", "40", HIGH) │
│  submit_job("sleep_job", "100", NORMAL)  │
│  submit_job("flaky_job", "", NORMAL)     │
└──────────────┬───────────────────────────┘
               │
               ▼
┌──────────────────────────────────────────┐
│           JobManager                     │
│                                          │
│  ① Assign UUID + timestamp               │
│  ② persist_job() → SQLite (WAL mode)     │
│  ③ push() → ThreadSafePriorityQueue      │
└──────┬──────────────────┬────────────────┘
       │ wait_for_job()   │ complete_job() / fail_job()
       │ (blocking)       │
       ▼                  ▲
┌──────────────────────────────────────────┐
│   Worker-1  Worker-2  Worker-3  Worker-4 │
│   (std::jthread, stop_token)             │
│                                          │
│   run() loop:                            │
│     job = manager.wait_for_job(tok)      │
│     handler = registry.find(job.type)    │
│     try { (*handler)(job) }              │
│       → complete_job()                   │
│     catch { fail_job() }                 │
│       → retry or permanent FAIL          │
└──────────────────────────────────────────┘
               │
               ▼
┌──────────────────────────────────────────┐
│           SQLiteBackend                  │
│                                          │
│  WAL mode  |  prepared statements        │
│  Table: jobs (status, priority, retry_   │
│               count, last_error ...)     │
└──────────────────────────────────────────┘
```

### State Machine — Job Lifecycle

```
PENDING ──► RUNNING ──► COMPLETED
               │
               └──► FAILED ──► (retry_count < max_retries) ──► PENDING
                            └──► (retry_count >= max_retries) ──► FAILED (permanent)
```

---

## 3. File Guide

```
distributed_job_queue/
├── CMakeLists.txt              Root build file (C++20, SQLite3, GTest)
├── cmake/
│   └── CompilerOptions.cmake   Warnings + ASan/UBSan in Debug
│
├── common/                     ── Layer 0: shared primitives ──
│   ├── include/common/
│   │   ├── job.hpp             Job struct, Priority enum, JobStatus enum
│   │   ├── result.hpp          Result<T,E> monad (no exceptions across threads)
│   │   ├── uuid.hpp            UUID v4 generator (header)
│   │   └── logger.hpp          Thread-safe structured logger (header-only)
│   └── src/
│       └── uuid.cpp            UUID implementation (thread_local RNG)
│
├── storage/                    ── Layer 1: persistence ──
│   ├── include/storage/
│   │   ├── i_storage_backend.hpp  Pure virtual interface
│   │   ├── sqlite_backend.hpp     SQLite implementation header
│   │   └── in_memory_backend.hpp  Test/mock implementation
│   └── src/
│       ├── sqlite_backend.cpp  WAL mode, prepared stmts, schema init, CRUD
│       └── in_memory_backend.cpp  std::unordered_map + mutex
│
├── manager/                    ── Layer 2: scheduling ──
│   ├── include/manager/
│   │   ├── priority_queue.hpp  Thread-safe MPMC priority queue
│   │   └── job_manager.hpp     Central coordinator
│   └── src/
│       ├── priority_queue.cpp  push/try_pop/wait_and_pop/shutdown
│       └── job_manager.cpp     submit/pull/complete/fail/metrics
│
├── worker/                     ── Layer 3: execution ──
│   ├── include/worker/
│   │   ├── handler_registry.hpp  job_type → JobHandler map
│   │   └── worker.hpp            jthread-based execution loop
│   └── src/
│       ├── handler_registry.cpp
│       └── worker.cpp          run() event loop, execute_job()
│
├── demo/
│   └── phase1_demo.cpp         Integrated demo: submit → workers → metrics
│
└── tests/
    └── unit/
        ├── test_priority_queue.cpp  Ordering, MPMC, stop_token, shutdown
        ├── test_storage.cpp         Typed tests (InMemory + SQLite)
        ├── test_job_manager.cpp     Submit, priority, complete, retry, recovery
        └── test_worker.cpp          End-to-end: execution, retry, concurrency
```

---

## 4. Threading Model

### Threads in Phase 1

```
Process
│
├─ Main thread
│    Submits jobs → calls manager.submit_job()
│    Starts workers → waits for completion
│    Calls manager.shutdown()
│
├─ Worker-1 (std::jthread)
│    Loops: wait_for_job(stop_token) → execute → complete/fail
│    Exits when stop_token triggered OR queue shut down
│
├─ Worker-2 (std::jthread)  [same as above]
├─ Worker-3 (std::jthread)  [same as above]
└─ Worker-4 (std::jthread)  [same as above]
```

### Synchronization Strategy

| Shared Resource | Protection | Rationale |
|-----------------|------------|-----------|
| `priority_queue_` | `std::mutex` + `condition_variable_any` | Needed for blocking wait |
| Metrics counters | `std::atomic<uint64_t>` | Lock-free; reads and increments are always consistent |
| SQLite database | SQLite's internal WAL locking | WAL mode allows concurrent reads |
| Logger output | `std::mutex` (inline static) | Prevents interleaved console output |

### Why `condition_variable_any` not `condition_variable`?

`std::condition_variable` can only be used with `std::unique_lock<std::mutex>`.
`std::condition_variable_any` works with *any* lockable and also provides the
C++20 overload `wait(lock, stop_token, pred)` which registers a stop-callback
on the token — so when `jthread` requests a stop, blocked workers wake
immediately without needing a separate "poison pill" message.

### Shutdown Sequence

```
1. manager.shutdown()
     ↓  sets queue_.shutdown_ = true
     ↓  calls cv_.notify_all()

2. All Worker threads:
     condition_variable_any::wait() → woke up
     predicate check: !queue.empty() || shutdown_ → shutdown_ is true
     queue_.empty() → true
     wait_and_pop returns nullopt
     run() loop exits

3. jthread destructor (or w.stop()):
     calls thread_.request_stop() (no-op, already stopped)
     calls thread_.join() → returns immediately
```

---

## 5. Building

### Prerequisites

| Tool | Minimum Version |
|------|----------------|
| CMake | 3.20 |
| C++ compiler | GCC 11 or Clang 13 (C++20 with `std::jthread`) |
| SQLite3 dev headers | Any recent version |

**macOS:**
```bash
brew install cmake sqlite3
```

**Ubuntu/Debian:**
```bash
sudo apt install cmake build-essential libsqlite3-dev
```

### Configure & Build

```bash
git clone <repo>
cd distributed_job_queue

# Configure (Debug with ASan by default)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Build everything
cmake --build build --parallel

# (Optional) Release build — O3 + LTO
cmake -B build_rel -DCMAKE_BUILD_TYPE=Release
cmake --build build_rel --parallel
```

> **Note:** Google Test is fetched automatically via `FetchContent` on first
> configure. An internet connection is required once; subsequent builds use
> the cached copy.

---

## 6. Running

### Phase 1 Demo

```bash
./build/demo/phase1_demo
```

Expected output:
```
══════════════════════════════════════════
  Phase 1 — Single-Node Job Queue
══════════════════════════════════════════

12:30:00.123  INFO   [SQLiteBackend]  Opened database: jobs.db
12:30:00.124  INFO   [JobManager]     Recovered 0 job(s) from storage
12:30:00.125  INFO   [Demo]           Registered 4 handlers

══════════════════════════════════════════
  Submitting Jobs
══════════════════════════════════════════

12:30:00.130  INFO   [JobManager]     Submitted job <uuid> type=fibonacci_job priority=HIGH
...
12:30:00.145  INFO   [Demo]           Submitted 13 jobs  |  Queue depth: 13

══════════════════════════════════════════
  Starting Workers
══════════════════════════════════════════

12:30:00.150  INFO   [Worker-1]       Started
12:30:00.151  INFO   [Worker-1]       Executing job <uuid> type=fibonacci_job priority=HIGH
12:30:00.152  INFO   [Worker-2]       Executing job <uuid> type=fibonacci_job priority=HIGH
...

══════════════════════════════════════════
  Final Metrics
══════════════════════════════════════════

  Jobs submitted  : 13
  Jobs completed  : 12
  Jobs failed     :  1
  Jobs retried    :  2
  Queue remaining :  0

  Per-worker stats:
    Worker-1  executed=5  ok=4  fail=1  total_ms=412
    ...

✔ Phase 1 complete.
```

### Running a second time (recovery demo)

```bash
# First run creates jobs.db and processes all jobs.
./build/demo/phase1_demo

# If you manually kill the process mid-run (Ctrl-C), on the next run
# the manager will log "Recovered N job(s) from storage" and continue.
./build/demo/phase1_demo
```

---

## 7. Tests

```bash
# Run all unit tests
cd build && ctest --output-on-failure

# Or run the binary directly for verbose output
./build/tests/unit_tests --gtest_color=yes

# Run a specific test suite
./build/tests/unit_tests --gtest_filter="PriorityQueue*"
./build/tests/unit_tests --gtest_filter="StorageTest*"
./build/tests/unit_tests --gtest_filter="JobManagerTest*"
./build/tests/unit_tests --gtest_filter="WorkerTest*"
```

### Test Coverage Summary

| Suite | Count | What's tested |
|-------|-------|---------------|
| `PriorityQueue` | 8 | Ordering, FIFO within priority, `stop_token`, MPMC stress |
| `StorageTest` | 10 | CRUD on both `InMemoryBackend` and `SQLiteBackend` (typed test) |
| `JobManagerTest` | 10 | Submit, persistence, priority dispatch, retry, recovery |
| `WorkerTest` | 7 | End-to-end execution, retry, permanent failure, concurrent workers |

---

## 8. C++20 Features Used

| Feature | Where | Purpose |
|---------|-------|---------|
| `std::jthread` | `Worker` | RAII thread with automatic stop on destruction |
| `std::stop_token` / `std::stop_source` | `Worker`, `PriorityQueue` | Cooperative cancellation |
| `std::condition_variable_any::wait(lock, stop_token, pred)` | `ThreadSafePriorityQueue` | Block workers until job available *or* stop requested |
| Designated initialisers | `job_manager.cpp`, tests | Readable struct construction |
| `std::atomic<uint64_t>` with `memory_order_relaxed` | `JobManager`, `Worker` | Lock-free metric counters |
| Concepts (implicit via `auto` in lambdas) | Throughout | Modern idiom |
| Class template argument deduction (CTAD) | `std::lock_guard lk{mutex_}` | Cleaner lock acquisition |

---

## 9. Phase Roadmap

| Phase | Status | Focus |
|-------|--------|-------|
| **1 — Single-Node Queue** | ✅ **Done** | In-memory queue, SQLite persistence, local workers |
| 2 — Concurrent Workers | ⬜ Next | Thread pool, epoll server skeleton |
| 3 — Distributed Workers | ⬜ | TCP, binary protocol, worker registration |
| 4 — Fault Tolerance | ⬜ | Heartbeats, lease manager, crash detection |
| 5 — Monitoring | ⬜ | Metrics HTTP endpoint, ncurses TUI |
| 6 — Performance | ⬜ | Lock-free queue, batch DB writes, load tester |
