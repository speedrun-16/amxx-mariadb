#ifndef AMXX_MARIADB_ASYNC_H
#define AMXX_MARIADB_ASYNC_H

#include "mariadb_connection.h"
#include "mariadb_result.h"
#include "mariadb_statement.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ============================================================================
// ASYNC JOB STATE
// ============================================================================

// lifecycle state of a single async job - transitions are one-way
enum class async_job_state
{
    queued = 0,
    running,
    cancelled,
    finished
};

// ============================================================================
// ASYNC PARAM VALUE
// ============================================================================

// a single typed parameter slot for an async prepared job
struct async_param_value
{
    stmt_param_type type{stmt_param_type::unset};
    int int_value{0};
    float float_value{0.0f};
    std::string string_value;
};

// ============================================================================
// ASYNC JOB
// ============================================================================

// one unit of work submitted to the async worker thread
struct async_job
{
    cell handle{0};
    AMX* amx{nullptr};
    int forward_id{0};
    connection_options options;
    std::string query;
    std::vector<cell> data;
    bool exec_mode{false};  // true -> write query, false -> SELECT
    std::vector<async_param_value> params;  // non-empty when use_prepared is true
    bool use_prepared{false};
    std::chrono::steady_clock::time_point enqueue_time{};
    std::atomic<async_job_state> state{async_job_state::queued};
};

// ============================================================================
// ASYNC COMPLETION
// ============================================================================

// result of a finished or cancelled job, dispatched on the main thread
struct async_completion
{
    std::shared_ptr<async_job> job;
    std::shared_ptr<result_data> result;
    int affected_rows{0};
    int insert_id{0};
    std::string error;
    unsigned int error_code{0};
    int callback_state{0};
    bool fire_callback{true};
};

// ============================================================================
// CONNECTION POOL
// ============================================================================

// a live MYSQL* together with its per-connection cached prepared statements
struct pooled_conn
{
    MYSQL* mysql{nullptr};
    std::unordered_map<std::string, MYSQL_STMT*> stmts;

    pooled_conn() = default;
    explicit pooled_conn(MYSQL* m) : mysql(m) {}
    pooled_conn(pooled_conn&&) = default;
    pooled_conn& operator=(pooled_conn&&) = default;
    pooled_conn(const pooled_conn&) = delete;
    pooled_conn& operator=(const pooled_conn&) = delete;
};

// reuses idle pooled connections keyed by connection_options hash
class connection_pool
{
public:
    ~connection_pool();

    // returns an existing idle connection or opens a new one
    pooled_conn acquire(
        const connection_options& options,
        std::string& error,
        unsigned int& error_code);

    // returns conn to the idle pool for future reuse
    void release(const connection_options& options, pooled_conn conn);

    // closes and discards all idle connections
    void drain();

private:
    std::string make_key(const connection_options& options) const;

    std::mutex pool_mutex_;
    std::unordered_map<std::string, std::vector<pooled_conn>> idle_;
};

// ============================================================================
// ASYNC STMT DATA
// ============================================================================

// reusable async statement template:
// stores the query text and current bound parameter values;
// fire via mariadb_async_stmt_exec / mariadb_async_stmt_query
class async_stmt_data
{
public:
    async_stmt_data(std::string query, unsigned int param_count);

    bool bind_int(unsigned int index, int value);
    bool bind_bool(unsigned int index, bool value);
    bool bind_float(unsigned int index, float value);
    bool bind_string(unsigned int index, const std::string& value);
    bool bind_null(unsigned int index);

    const std::string& query() const;
    const std::vector<async_param_value>& params() const;
    unsigned int param_count() const;
    const std::string& last_error() const;

private:
    bool prepare_slot(unsigned int index);

    std::string query_;
    std::vector<async_param_value> params_;
    std::string last_error_;
};

// counts '?' placeholders in query and creates an async statement template
std::shared_ptr<async_stmt_data> create_async_stmt(const std::string& query);

// ============================================================================
// ASYNC WORKER
// ============================================================================

// pool of background threads that dequeue and execute async jobs
class async_worker
{
public:
    async_worker() = default;
    ~async_worker();

    bool start(unsigned int thread_count = 1);
    // stops the worker; if drain_pending is true, finishes queued jobs first
    void stop(bool drain_pending);
    void enqueue(const std::shared_ptr<async_job>& job);
    // dispatches completed callbacks - must be called from the main thread
    void process_completions();

private:
    void thread_main();
    void push_completion(async_completion completion);

    std::vector<std::thread> threads_;
    std::mutex pending_mutex_;
    std::condition_variable pending_cv_;
    std::deque<std::shared_ptr<async_job>> pending_;

    std::mutex completions_mutex_;
    std::deque<async_completion> completions_;

    connection_pool pool_;
    bool running_{false};
    bool drain_pending_{true};
};

#endif
