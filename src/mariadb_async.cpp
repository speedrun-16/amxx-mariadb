#include "mariadb_module.h"

#include <cstring>

// ============================================================================
// ASYNC STATE CONSTANTS
// ============================================================================

namespace
{
    constexpr int k_async_connect_failed = -2;
    constexpr int k_async_query_failed   = -1;
    constexpr int k_async_ok             =  0;

    // executes a cached prepared statement with the given typed params;
    // sets completion fields on success or failure; returns true on success
    static bool execute_prepared(
        MYSQL_STMT* stmt,
        const std::vector<async_param_value>& params,
        bool exec_mode,
        async_completion& completion)
    {
        const auto count = static_cast<unsigned int>(params.size());

        std::vector<MYSQL_BIND> binds(count);
        std::vector<int> int_buf(count, 0);
        std::vector<float> float_buf(count, 0.0f);
        std::vector<unsigned long> str_len(count, 0);
        std::vector<my_bool> null_buf(count, 0);
        std::vector<std::string> str_buf(count);

        for (unsigned int i = 0; i < count; ++i)
        {
            auto& bind = binds[i];
            memset(&bind, 0, sizeof(MYSQL_BIND));

            switch (params[i].type)
            {
                case stmt_param_type::t_int:
                case stmt_param_type::t_bool:
                    int_buf[i] = params[i].int_value;
                    bind.buffer_type = MYSQL_TYPE_LONG;
                    bind.buffer = &int_buf[i];
                    break;

                case stmt_param_type::t_float:
                    float_buf[i] = params[i].float_value;
                    bind.buffer_type = MYSQL_TYPE_FLOAT;
                    bind.buffer = &float_buf[i];
                    break;

                case stmt_param_type::t_string:
                    str_buf[i] = params[i].string_value;
                    str_len[i] = static_cast<unsigned long>(str_buf[i].size());
                    bind.buffer_type = MYSQL_TYPE_STRING;
                    bind.buffer = str_buf[i].data();
                    bind.buffer_length = str_len[i];
                    bind.length = &str_len[i];
                    break;

                case stmt_param_type::t_null:
                    null_buf[i] = 1;
                    bind.buffer_type = MYSQL_TYPE_NULL;
                    bind.is_null = &null_buf[i];
                    break;

                case stmt_param_type::unset:
                default:
                    completion.callback_state = k_async_query_failed;
                    completion.error = "all parameters must be bound before async prepared execution.";
                    return false;
            }
        }

        if (count > 0 && mysql_stmt_bind_param(stmt, binds.data()) != 0)
        {
            completion.callback_state = k_async_query_failed;
            completion.error = mysql_stmt_error(stmt);
            completion.error_code = mysql_stmt_errno(stmt);
            return false;
        }

        if (!exec_mode)
        {
            bool update_max = true;
            (void)mysql_stmt_attr_set(stmt, STMT_ATTR_UPDATE_MAX_LENGTH, &update_max);
        }

        if (mysql_stmt_execute(stmt) != 0)
        {
            completion.callback_state = k_async_query_failed;
            completion.error = mysql_stmt_error(stmt);
            completion.error_code = mysql_stmt_errno(stmt);
            return false;
        }

        if (exec_mode)
        {
            mysql_stmt_free_result(stmt);
            completion.callback_state = k_async_ok;
            completion.affected_rows = static_cast<int>(mysql_stmt_affected_rows(stmt));
            completion.insert_id = static_cast<int>(mysql_stmt_insert_id(stmt));
        }
        else
        {
            if (mysql_stmt_field_count(stmt) == 0)
            {
                completion.callback_state = k_async_query_failed;
                completion.error = "prepared statement did not return a result set, use mariadb_async_stmt_exec().";
                return false;
            }

            if (mysql_stmt_store_result(stmt) != 0)
            {
                completion.callback_state = k_async_query_failed;
                completion.error = mysql_stmt_error(stmt);
                completion.error_code = mysql_stmt_errno(stmt);
                mysql_stmt_free_result(stmt);
                return false;
            }

            std::string err;
            unsigned int err_code = 0;
            completion.result = result_data::from_stmt_result(stmt, err, err_code);
            mysql_stmt_free_result(stmt);

            if (!completion.result)
            {
                completion.callback_state = k_async_query_failed;
                completion.error = err;
                completion.error_code = err_code;
                return false;
            }

            completion.callback_state = k_async_ok;
            completion.affected_rows = completion.result->row_count();
            completion.insert_id = static_cast<int>(mysql_stmt_insert_id(stmt));
        }

        return true;
    }
}

// ============================================================================
// CONNECTION POOL
// ============================================================================

connection_pool::~connection_pool()
{
    drain();
}

std::string connection_pool::make_key(const connection_options& options) const
{
    return options.host + "|" + options.user + "|" + options.password + "|" + options.database + "|" +
        options.charset + "|" + std::to_string(options.port) + "|" + std::to_string(options.timeout_ms) + "|" +
        (options.auto_reconnect ? "1" : "0");
}

pooled_conn connection_pool::acquire(const connection_options& options, std::string& error, unsigned int& error_code)
{
    const auto key = make_key(options);

    // take a candidate out of the pool under the lock, then release the lock
    // before pinging - a dead connection can block for the full timeout and
    // holding pool_mutex_ during that would stall all other worker threads
    pooled_conn candidate;
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        auto iter = idle_.find(key);
        if (iter != idle_.end() && !iter->second.empty())
        {
            candidate = std::move(iter->second.back());
            iter->second.pop_back();
        }
    }

    if (candidate.mysql)
    {
        if (mysql_ping(candidate.mysql) == 0)
        {
            return candidate;
        }

        // connection dead - close its cached stmts before the connection
        for (auto& [query, stmt] : candidate.stmts)
        {
            mysql_stmt_close(stmt);
        }
        mysql_close(candidate.mysql);
    }

    MYSQL* mysql = create_raw_connection(options, error, error_code);
    if (!mysql)
    {
        return pooled_conn{};
    }
    return pooled_conn{mysql};
}

void connection_pool::release(const connection_options& options, pooled_conn conn)
{
    if (!conn.mysql)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(pool_mutex_);
    idle_[make_key(options)].push_back(std::move(conn));
}

void connection_pool::drain()
{
    std::lock_guard<std::mutex> lock(pool_mutex_);
    for (auto& [key, conns] : idle_)
    {
        for (auto& conn : conns)
        {
            for (auto& [query, stmt] : conn.stmts)
            {
                mysql_stmt_close(stmt);
            }
            mysql_close(conn.mysql);
        }
    }
    idle_.clear();
}

// ============================================================================
// ASYNC WORKER
// ============================================================================

async_worker::~async_worker()
{
    stop(false);
}

bool async_worker::start(unsigned int thread_count)
{
    if (running_)
    {
        return true;
    }

    running_ = true;
    drain_pending_ = true;
    threads_.reserve(thread_count);
    for (unsigned int i = 0; i < thread_count; ++i)
    {
        threads_.emplace_back(&async_worker::thread_main, this);
    }
    return true;
}

void async_worker::stop(bool drain_pending)
{
    if (threads_.empty())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        running_ = false;
        drain_pending_ = drain_pending;
    }

    pending_cv_.notify_all();

    for (auto& thread : threads_)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
    threads_.clear();
    pool_.drain();
}

void async_worker::enqueue(const std::shared_ptr<async_job>& job)
{
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.push_back(job);
    }

    pending_cv_.notify_one();
}

void async_worker::push_completion(async_completion completion)
{
    std::lock_guard<std::mutex> lock(completions_mutex_);
    completions_.push_back(std::move(completion));
}

void async_worker::thread_main()
{
    mysql_thread_init();

    for (;;)
    {
        std::shared_ptr<async_job> job;
        {
            std::unique_lock<std::mutex> lock(pending_mutex_);
            pending_cv_.wait(lock, [this] { return !pending_.empty() || !running_; });

            if (!running_ && (!drain_pending_ || pending_.empty()))
            {
                break;
            }

            if (pending_.empty())
            {
                continue;
            }

            job = pending_.front();
            pending_.pop_front();
        }

        if (!job)
        {
            continue;
        }

        // skip cancelled jobs without firing a callback
        async_job_state expected = async_job_state::queued;
        if (!job->state.compare_exchange_strong(expected, async_job_state::running))
        {
            async_completion completion;
            completion.job = job;
            completion.fire_callback = false;
            push_completion(std::move(completion));
            continue;
        }

        async_completion completion;
        completion.job = job;

        std::string error;
        unsigned int error_code = 0;
        pooled_conn conn = pool_.acquire(job->options, error, error_code);
        if (!conn.mysql)
        {
            completion.callback_state = k_async_connect_failed;
            completion.error = error;
            completion.error_code = error_code;
            job->state.store(async_job_state::finished);
            push_completion(std::move(completion));
            continue;
        }

        if (job->use_prepared)
        {
            // find or create a cached prepared statement for this query+connection
            MYSQL_STMT* stmt = nullptr;
            auto cache_it = conn.stmts.find(job->query);
            if (cache_it != conn.stmts.end())
            {
                stmt = cache_it->second;
                mysql_stmt_free_result(stmt);  // clear any leftover result from prior use
            }
            else
            {
                stmt = mysql_stmt_init(conn.mysql);
                if (!stmt)
                {
                    completion.callback_state = k_async_query_failed;
                    completion.error = mysql_error(conn.mysql);
                    completion.error_code = mysql_errno(conn.mysql);
                }
                else if (mysql_stmt_prepare(stmt, job->query.c_str(), static_cast<unsigned long>(job->query.size())) != 0)
                {
                    completion.callback_state = k_async_query_failed;
                    completion.error = mysql_stmt_error(stmt);
                    completion.error_code = mysql_stmt_errno(stmt);
                    mysql_stmt_close(stmt);
                    stmt = nullptr;
                }
                else
                {
                    conn.stmts[job->query] = stmt;
                }
            }

            if (stmt)
            {
                execute_prepared(stmt, job->params, job->exec_mode, completion);
            }
        }
        else
        {
            if (mysql_real_query(conn.mysql, job->query.c_str(), static_cast<unsigned long>(job->query.size())) != 0)
            {
                completion.callback_state = k_async_query_failed;
                completion.error = mysql_error(conn.mysql);
                completion.error_code = mysql_errno(conn.mysql);
                pool_.release(job->options, std::move(conn));
                job->state.store(async_job_state::finished);
                push_completion(std::move(completion));
                continue;
            }

            if (job->exec_mode)
            {
                MYSQL_RES* result = mysql_store_result(conn.mysql);
                if (result != nullptr)
                {
                    mysql_free_result(result);
                    completion.callback_state = k_async_query_failed;
                    completion.error = "Query returned a result set, use mariadb_async_query().";
                }
                else if (mysql_field_count(conn.mysql) != 0)
                {
                    completion.callback_state = k_async_query_failed;
                    completion.error = mysql_error(conn.mysql);
                    completion.error_code = mysql_errno(conn.mysql);
                }
                else
                {
                    completion.callback_state = k_async_ok;
                    completion.affected_rows = static_cast<int>(mysql_affected_rows(conn.mysql));
                    completion.insert_id = static_cast<int>(mysql_insert_id(conn.mysql));
                }
            }
            else
            {
                MYSQL_RES* result = mysql_store_result(conn.mysql);
                if (!result)
                {
                    if (mysql_field_count(conn.mysql) != 0)
                    {
                        completion.callback_state = k_async_query_failed;
                        completion.error = mysql_error(conn.mysql);
                        completion.error_code = mysql_errno(conn.mysql);
                    }
                    else
                    {
                        completion.callback_state = k_async_query_failed;
                        completion.error = "Query did not return a result set.";
                    }
                }
                else
                {
                    completion.result = result_data::from_mysql_result(result);
                    mysql_free_result(result);
                    completion.callback_state = completion.result ? k_async_ok : k_async_query_failed;
                    if (completion.result)
                    {
                        completion.affected_rows = completion.result->row_count();
                        completion.insert_id = static_cast<int>(mysql_insert_id(conn.mysql));
                    }
                    else
                    {
                        completion.error = "Failed to copy the result set.";
                    }
                }
            }
        }

        pool_.release(job->options, std::move(conn));
        job->state.store(async_job_state::finished);
        push_completion(std::move(completion));
    }

    mysql_thread_end();
}

void async_worker::process_completions()
{
    std::deque<async_completion> completions;
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        completions.swap(completions_);
    }

    static cell empty_data[1] = {0};

    for (auto& completion : completions)
    {
        if (!completion.job)
        {
            continue;
        }

        cell result_handle = k_invalid_handle;
        if (completion.fire_callback && completion.result)
        {
            result_handle = g_results.create(completion.result);
        }

        if (completion.fire_callback)
        {
            cell data_array = MF_PrepareCellArray(
                completion.job->data.empty() ? empty_data : completion.job->data.data(),
                completion.job->data.empty() ? 1u : static_cast<unsigned int>(completion.job->data.size()));

            const double queue_time = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - completion.job->enqueue_time).count();
            MF_ExecuteForward(
                completion.job->forward_id,
                static_cast<cell>(completion.callback_state),
                result_handle,
                static_cast<cell>(completion.affected_rows),
                static_cast<cell>(completion.insert_id),
                completion.error.c_str(),
                static_cast<cell>(completion.error_code),
                data_array,
                static_cast<cell>(completion.job->data.size()),
                queue_time);
        }

        if (result_handle > 0)
        {
            g_results.destroy(result_handle);
        }

        if (completion.job->forward_id > 0)
        {
            MF_UnregisterSPForward(completion.job->forward_id);
        }

        g_jobs.destroy(completion.job->handle);
    }
}

// ============================================================================
// ASYNC STMT DATA
// ============================================================================

async_stmt_data::async_stmt_data(std::string query, unsigned int param_count)
    : query_(std::move(query)), params_(param_count)
{
}

bool async_stmt_data::prepare_slot(unsigned int index)
{
    if (index >= params_.size())
    {
        last_error_ = "parameter index out of range.";
        return false;
    }
    return true;
}

bool async_stmt_data::bind_int(unsigned int index, int value)
{
    if (!prepare_slot(index)) return false;
    params_[index].type = stmt_param_type::t_int;
    params_[index].int_value = value;
    return true;
}

bool async_stmt_data::bind_bool(unsigned int index, bool value)
{
    if (!prepare_slot(index)) return false;
    params_[index].type = stmt_param_type::t_bool;
    params_[index].int_value = value ? 1 : 0;
    return true;
}

bool async_stmt_data::bind_float(unsigned int index, float value)
{
    if (!prepare_slot(index)) return false;
    params_[index].type = stmt_param_type::t_float;
    params_[index].float_value = value;
    return true;
}

bool async_stmt_data::bind_string(unsigned int index, const std::string& value)
{
    if (!prepare_slot(index)) return false;
    params_[index].type = stmt_param_type::t_string;
    params_[index].string_value = value;
    return true;
}

bool async_stmt_data::bind_null(unsigned int index)
{
    if (!prepare_slot(index)) return false;
    params_[index].type = stmt_param_type::t_null;
    params_[index].string_value.clear();
    return true;
}

const std::string& async_stmt_data::query() const { return query_; }
const std::vector<async_param_value>& async_stmt_data::params() const { return params_; }
unsigned int async_stmt_data::param_count() const { return static_cast<unsigned int>(params_.size()); }
const std::string& async_stmt_data::last_error() const { return last_error_; }

std::shared_ptr<async_stmt_data> create_async_stmt(const std::string& query)
{
    unsigned int count = 0;
    for (char c : query)
    {
        if (c == '?') ++count;
    }
    return std::make_shared<async_stmt_data>(query, count);
}
