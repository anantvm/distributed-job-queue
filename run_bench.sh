#!/bin/bash
cd /Users/manoj/Desktop/distributed_job_queue/build
killall job_manager_server 2>/dev/null || true
killall job_worker_process 2>/dev/null || true
rm -f /tmp/*.db

./server/job_manager_server > server.log 2>&1 &
SERVER_PID=$!
sleep 2

./worker_process/job_worker_process > worker1.log 2>&1 &
./worker_process/job_worker_process > worker2.log 2>&1 &
./worker_process/job_worker_process > worker3.log 2>&1 &
./worker_process/job_worker_process > worker4.log 2>&1 &
sleep 2

./benchmark/benchmark_runner --profile baseline --output report.csv --json report.json

killall job_worker_process 2>/dev/null || true
killall job_manager_server 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true
