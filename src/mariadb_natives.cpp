#include "mariadb_module.h"

#include <chrono>

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

namespace
{
    double get_monotonic_time_seconds()
    {
        static const auto start = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

    template <typename T>
    std::shared_ptr<T> require_handle(AMX* amx, cell handle, handle_table<T>& table, const char* kind)
    {
        auto value = table.get(handle);
        if (!value)
        {
            MF_LogError(amx, AMX_ERR_NATIVE, "Invalid %s handle: %d", kind, handle);
        }
        return value;
    }

    std::shared_ptr<connection_data> require_connection(AMX* amx, cell handle)
    {
        return require_handle(amx, handle, g_connections, "MariaDB connection");
    }

    std::shared_ptr<result_data> require_result(AMX* amx, cell handle)
    {
        return require_handle(amx, handle, g_results, "MariaDB result");
    }

    std::shared_ptr<statement_data> require_statement(AMX* amx, cell handle)
    {
        return require_handle(amx, handle, g_statements, "MariaDB statement");
    }

    std::shared_ptr<async_job> require_job(AMX* amx, cell handle)
    {
        return require_handle(amx, handle, g_jobs, "MariaDB job");
    }

    // ============================================================================
    // CONNECTION NATIVES
    // ============================================================================

    cell AMX_NATIVE_CALL native_mariadb_connect(AMX* amx, cell* params)
    {
        const auto param_count = params[0] / sizeof(cell);

        connection_options options;
        options.host         = get_amx_string(amx, params[1], 0);
        options.user         = get_amx_string(amx, params[2], 1);
        options.password     = get_amx_string(amx, params[3], 2);
        options.database     = get_amx_string(amx, params[4], 3);
        options.port         = param_count >= 5 ? static_cast<unsigned int>(params[5]) : 3306u;
        options.charset      = param_count >= 6 ? get_amx_string(amx, params[6], 4) : "utf8mb4";
        options.timeout_ms   = param_count >= 7 ? static_cast<unsigned int>(params[7]) : 5000u;
        options.auto_reconnect = param_count >= 8 ? (params[8] != 0) : true;

        std::string error;
        unsigned int error_code = 0;
        auto connection = create_connection(options, error, error_code);

        if (param_count >= 10)
        {
            set_amx_string_raw(amx, params[9], params[10], error);
        }
        if (param_count >= 11)
        {
            set_cell_ref(amx, params[11], static_cast<cell>(error_code));
        }

        if (!connection)
        {
            return k_invalid_handle;
        }

        return g_connections.create(connection);
    }

    cell AMX_NATIVE_CALL native_mariadb_disconnect(AMX* amx, cell* params)
    {
        cell* ref = MF_GetAmxAddr(amx, params[1]);
        const cell handle = *ref;

        if (is_invalid_handle(handle))
        {
            *ref = k_invalid_handle;
            return 1;
        }

        if (!g_connections.destroy(handle))
        {
            MF_LogError(amx, AMX_ERR_NATIVE, "Invalid MariaDB connection handle: %d", handle);
            return 0;
        }

        *ref = k_invalid_handle;
        return 1;
    }

    cell AMX_NATIVE_CALL native_mariadb_ping(AMX* amx, cell* params)
    {
        auto connection = require_connection(amx, params[1]);
        return connection && connection->ping();
    }

    cell AMX_NATIVE_CALL native_mariadb_set_charset(AMX* amx, cell* params)
    {
        auto connection = require_connection(amx, params[1]);
        if (!connection)
        {
            return 0;
        }

        return connection->set_charset(get_amx_string(amx, params[2], 0));
    }

    cell AMX_NATIVE_CALL native_mariadb_get_error(AMX* amx, cell* params)
    {
        auto connection = require_connection(amx, params[1]);
        if (!connection)
        {
            return 0;
        }

        set_amx_string_raw(amx, params[2], params[3], connection->last_error());
        return 1;
    }

    cell AMX_NATIVE_CALL native_mariadb_get_error_code(AMX* amx, cell* params)
    {
        auto connection = require_connection(amx, params[1]);
        return connection ? static_cast<cell>(connection->last_error_code()) : 0;
    }

    cell AMX_NATIVE_CALL native_mariadb_server_version(AMX* amx, cell* params)
    {
        auto connection = require_connection(amx, params[1]);
        if (!connection)
        {
            return 0;
        }

        set_amx_string_raw(amx, params[2], params[3], connection->server_version());
        return 1;
    }

    cell AMX_NATIVE_CALL native_mariadb_escape_string(AMX* amx, cell* params)
    {
        auto connection = require_connection(amx, params[1]);
        if (!connection)
        {
            return -1;
        }

        std::string escaped;
        const auto written = connection->escape_string(get_amx_string(amx, params[4], 0), escaped);
        if (written >= 0)
        {
            set_amx_string_raw(amx, params[2], params[3], escaped);
        }
        return written;
    }

    cell AMX_NATIVE_CALL native_mariadb_monotonic_time(AMX*, cell*)
    {
        return amx_ftoc(static_cast<float>(get_monotonic_time_seconds()));
    }

    // ============================================================================
    // QUERY NATIVES
    // ============================================================================

    cell AMX_NATIVE_CALL native_mariadb_query(AMX* amx, cell* params)
    {
        auto connection = require_connection(amx, params[1]);
        if (!connection)
        {
            return k_invalid_handle;
        }

        const auto query = get_amx_string(amx, params[2], 0);
        if (mysql_real_query(connection->raw(), query.c_str(), static_cast<unsigned long>(query.size())) != 0)
        {
            connection->set_last_error_from_mysql();
            return k_invalid_handle;
        }

        MYSQL_RES* result = mysql_store_result(connection->raw());
        if (!result)
        {
            if (mysql_field_count(connection->raw()) != 0)
            {
                connection->set_last_error_from_mysql();
            }
            else
            {
                connection->set_last_error("Query did not return a result set.", 0);
            }
            return k_invalid_handle;
        }

        auto copied = result_data::from_mysql_result(result);
        mysql_free_result(result);

        if (!copied)
        {
            connection->set_last_error("Failed to copy the result set.", 0);
            return k_invalid_handle;
        }

        connection->set_last_error("", 0);
        return g_results.create(copied);
    }

    cell AMX_NATIVE_CALL native_mariadb_exec(AMX* amx, cell* params)
    {
        const auto param_count = params[0] / sizeof(cell);
        auto connection = require_connection(amx, params[1]);
        if (!connection)
        {
            return 0;
        }

        const auto query = get_amx_string(amx, params[2], 0);
        if (mysql_real_query(connection->raw(), query.c_str(), static_cast<unsigned long>(query.size())) != 0)
        {
            connection->set_last_error_from_mysql();
            return 0;
        }

        if (mysql_field_count(connection->raw()) == 0)
        {
            if (param_count >= 3)
            {
                set_cell_ref(amx, params[3], static_cast<cell>(mysql_affected_rows(connection->raw())));
            }
            if (param_count >= 4)
            {
                set_cell_ref(amx, params[4], static_cast<cell>(mysql_insert_id(connection->raw())));
            }

            connection->set_last_error("", 0);
            return 1;
        }

        MYSQL_RES* result = mysql_store_result(connection->raw());
        if (result != nullptr)
        {
            mysql_free_result(result);
            connection->set_last_error("Query returned a result set, use mariadb_query().", 0);
            return 0;
        }

        connection->set_last_error_from_mysql();
        return 0;
    }

    // ============================================================================
    // RESULT NATIVES
    // ============================================================================

    cell AMX_NATIVE_CALL native_mariadb_result_close(AMX* amx, cell* params)
    {
        cell* ref = MF_GetAmxAddr(amx, params[1]);
        const cell handle = *ref;
        if (is_invalid_handle(handle))
        {
            *ref = k_invalid_handle;
            return 1;
        }

        if (!g_results.destroy(handle))
        {
            MF_LogError(amx, AMX_ERR_NATIVE, "Invalid MariaDB result handle: %d", handle);
            return 0;
        }

        *ref = k_invalid_handle;
        return 1;
    }

    cell AMX_NATIVE_CALL native_mariadb_next_row(AMX* amx, cell* params)
    {
        auto result = require_result(amx, params[1]);
        return result && result->next_row();
    }

    cell AMX_NATIVE_CALL native_mariadb_rewind(AMX* amx, cell* params)
    {
        auto result = require_result(amx, params[1]);
        return result && result->rewind();
    }

    cell AMX_NATIVE_CALL native_mariadb_row_count(AMX* amx, cell* params)
    {
        auto result = require_result(amx, params[1]);
        return result ? static_cast<cell>(result->row_count()) : 0;
    }

    cell AMX_NATIVE_CALL native_mariadb_column_count(AMX* amx, cell* params)
    {
        auto result = require_result(amx, params[1]);
        return result ? static_cast<cell>(result->column_count()) : 0;
    }

    cell AMX_NATIVE_CALL native_mariadb_column_index(AMX* amx, cell* params)
    {
        auto result = require_result(amx, params[1]);
        return result ? static_cast<cell>(result->column_index(get_amx_string(amx, params[2], 0))) : -1;
    }

    cell AMX_NATIVE_CALL native_mariadb_column_name(AMX* amx, cell* params)
    {
        auto result = require_result(amx, params[1]);
        if (!result)
        {
            return 0;
        }

        set_amx_string_raw(amx, params[3], params[4], result->column_name(params[2]));
        return 1;
    }

    cell AMX_NATIVE_CALL native_mariadb_is_null(AMX* amx, cell* params)
    {
        auto result = require_result(amx, params[1]);
        return result && result->is_null(params[2]);
    }

    cell AMX_NATIVE_CALL native_mariadb_read_int(AMX* amx, cell* params)
    {
        auto result = require_result(amx, params[1]);
        return result ? static_cast<cell>(result->read_int(params[2])) : 0;
    }

    cell AMX_NATIVE_CALL native_mariadb_read_bool(AMX* amx, cell* params)
    {
        auto result = require_result(amx, params[1]);
        return result && result->read_bool(params[2]);
    }

    cell AMX_NATIVE_CALL native_mariadb_read_float(AMX* amx, cell* params)
    {
        auto result = require_result(amx, params[1]);
        return result ? amx_ftoc(result->read_float(params[2])) : amx_ftoc(0.0f);
    }

    cell AMX_NATIVE_CALL native_mariadb_read_string(AMX* amx, cell* params)
    {
        auto result = require_result(amx, params[1]);
        if (!result)
        {
            return 0;
        }

        const auto& value = result->read_string(params[2]);
        set_amx_string_raw(amx, params[3], params[4], value);
        return static_cast<cell>(value.size());
    }

    // ============================================================================
    // PREPARED STATEMENT NATIVES
    // ============================================================================

    cell AMX_NATIVE_CALL native_mariadb_prepare(AMX* amx, cell* params)
    {
        auto connection = require_connection(amx, params[1]);
        if (!connection)
        {
            return k_invalid_handle;
        }

        auto statement = create_statement(connection, get_amx_string(amx, params[2], 0));
        if (!statement)
        {
            return k_invalid_handle;
        }

        return g_statements.create(statement);
    }

    cell AMX_NATIVE_CALL native_mariadb_stmt_close(AMX* amx, cell* params)
    {
        cell* ref = MF_GetAmxAddr(amx, params[1]);
        const cell handle = *ref;
        if (is_invalid_handle(handle))
        {
            *ref = k_invalid_handle;
            return 1;
        }

        if (!g_statements.destroy(handle))
        {
            MF_LogError(amx, AMX_ERR_NATIVE, "Invalid MariaDB statement handle: %d", handle);
            return 0;
        }

        *ref = k_invalid_handle;
        return 1;
    }

    cell AMX_NATIVE_CALL native_mariadb_stmt_reset(AMX* amx, cell* params)
    {
        auto statement = require_statement(amx, params[1]);
        return statement && statement->reset();
    }

    cell AMX_NATIVE_CALL native_mariadb_bind_int(AMX* amx, cell* params)
    {
        auto statement = require_statement(amx, params[1]);
        return statement && statement->bind_int(static_cast<unsigned int>(params[2]), params[3]);
    }

    cell AMX_NATIVE_CALL native_mariadb_bind_bool(AMX* amx, cell* params)
    {
        auto statement = require_statement(amx, params[1]);
        return statement && statement->bind_bool(static_cast<unsigned int>(params[2]), params[3] != 0);
    }

    cell AMX_NATIVE_CALL native_mariadb_bind_float(AMX* amx, cell* params)
    {
        auto statement = require_statement(amx, params[1]);
        return statement && statement->bind_float(static_cast<unsigned int>(params[2]), amx_ctof(params[3]));
    }

    cell AMX_NATIVE_CALL native_mariadb_bind_string(AMX* amx, cell* params)
    {
        auto statement = require_statement(amx, params[1]);
        return statement && statement->bind_string(static_cast<unsigned int>(params[2]), get_amx_string(amx, params[3], 0));
    }

    cell AMX_NATIVE_CALL native_mariadb_bind_null(AMX* amx, cell* params)
    {
        auto statement = require_statement(amx, params[1]);
        return statement && statement->bind_null(static_cast<unsigned int>(params[2]));
    }

    cell AMX_NATIVE_CALL native_mariadb_stmt_query(AMX* amx, cell* params)
    {
        auto statement = require_statement(amx, params[1]);
        if (!statement)
        {
            return k_invalid_handle;
        }

        auto result = statement->query();
        if (!result)
        {
            return k_invalid_handle;
        }

        return g_results.create(result);
    }

    cell AMX_NATIVE_CALL native_mariadb_stmt_exec(AMX* amx, cell* params)
    {
        const auto param_count = params[0] / sizeof(cell);
        auto statement = require_statement(amx, params[1]);
        if (!statement)
        {
            return 0;
        }

        int affected_rows = 0;
        int insert_id = 0;
        if (!statement->exec(affected_rows, insert_id))
        {
            return 0;
        }

        if (param_count >= 2)
        {
            set_cell_ref(amx, params[2], affected_rows);
        }
        if (param_count >= 3)
        {
            set_cell_ref(amx, params[3], insert_id);
        }

        return 1;
    }

    cell AMX_NATIVE_CALL native_mariadb_stmt_error(AMX* amx, cell* params)
    {
        auto statement = require_statement(amx, params[1]);
        if (!statement)
        {
            return 0;
        }

        set_amx_string_raw(amx, params[2], params[3], statement->last_error());
        return 1;
    }

    cell AMX_NATIVE_CALL native_mariadb_stmt_error_code(AMX* amx, cell* params)
    {
        auto statement = require_statement(amx, params[1]);
        return statement ? static_cast<cell>(statement->last_error_code()) : 0;
    }

    // ============================================================================
    // TRANSACTION NATIVES
    // ============================================================================

    cell AMX_NATIVE_CALL native_mariadb_begin(AMX* amx, cell* params)
    {
        auto connection = require_connection(amx, params[1]);
        return connection && connection->begin();
    }

    cell AMX_NATIVE_CALL native_mariadb_commit(AMX* amx, cell* params)
    {
        auto connection = require_connection(amx, params[1]);
        return connection && connection->commit();
    }

    cell AMX_NATIVE_CALL native_mariadb_rollback(AMX* amx, cell* params)
    {
        auto connection = require_connection(amx, params[1]);
        return connection && connection->rollback();
    }

    // ============================================================================
    // ASYNC NATIVES
    // ============================================================================

    cell create_async_job(AMX* amx, cell* params, bool exec_mode)
    {
        const auto param_count = params[0] / sizeof(cell);
        if (!g_async_worker)
        {
            MF_LogError(amx, AMX_ERR_NATIVE, "MariaDB async worker is not running.");
            return k_invalid_handle;
        }

        auto connection = require_connection(amx, params[1]);
        if (!connection)
        {
            return k_invalid_handle;
        }

        const auto callback = get_amx_string(amx, params[2], 0);
        const int forward = MF_RegisterSPForwardByName(
            amx,
            callback.c_str(),
            FP_CELL,
            FP_CELL,
            FP_CELL,
            FP_CELL,
            FP_STRING,
            FP_CELL,
            FP_ARRAY,
            FP_CELL,
            FP_FLOAT,
            FP_DONE);

        if (forward < 1)
        {
            MF_LogError(amx, AMX_ERR_NATIVE, "Function not found: %s", callback.c_str());
            return k_invalid_handle;
        }

        const auto data_size = param_count >= 5 ? static_cast<size_t>(params[5]) : 0u;
        auto job = std::make_shared<async_job>();
        job->amx        = amx;
        job->forward_id = forward;
        job->options    = connection->options();
        job->query      = get_amx_string(amx, params[3], 1);
        job->exec_mode  = exec_mode;
        job->enqueue_time = std::chrono::steady_clock::now();

        if (param_count >= 4 && data_size > 0)
        {
            cell* data = MF_GetAmxAddr(amx, params[4]);
            job->data.assign(data, data + data_size);
        }

        job->handle = g_jobs.create(job);
        g_async_worker->enqueue(job);
        return job->handle;
    }

    cell AMX_NATIVE_CALL native_mariadb_async_query(AMX* amx, cell* params)
    {
        return create_async_job(amx, params, false);
    }

    cell AMX_NATIVE_CALL native_mariadb_async_exec(AMX* amx, cell* params)
    {
        return create_async_job(amx, params, true);
    }

    cell AMX_NATIVE_CALL native_mariadb_async_stmt_create(AMX* amx, cell* params)
    {
        auto stmt = create_async_stmt(get_amx_string(amx, params[1], 0));
        if (!stmt)
        {
            return k_invalid_handle;
        }
        return g_async_stmts.create(stmt);
    }

    cell AMX_NATIVE_CALL native_mariadb_async_stmt_close(AMX* amx, cell* params)
    {
        cell* ref = MF_GetAmxAddr(amx, params[1]);
        const cell handle = *ref;
        if (is_invalid_handle(handle))
        {
            *ref = k_invalid_handle;
            return 1;
        }
        if (!g_async_stmts.destroy(handle))
        {
            MF_LogError(amx, AMX_ERR_NATIVE, "Invalid MariaDB async stmt handle: %d", handle);
            return 0;
        }
        *ref = k_invalid_handle;
        return 1;
    }

    cell AMX_NATIVE_CALL native_mariadb_async_stmt_bind_int(AMX* amx, cell* params)
    {
        auto stmt = require_handle(amx, params[1], g_async_stmts, "MariaDB async stmt");
        return stmt && stmt->bind_int(static_cast<unsigned int>(params[2]), params[3]);
    }

    cell AMX_NATIVE_CALL native_mariadb_async_stmt_bind_bool(AMX* amx, cell* params)
    {
        auto stmt = require_handle(amx, params[1], g_async_stmts, "MariaDB async stmt");
        return stmt && stmt->bind_bool(static_cast<unsigned int>(params[2]), params[3] != 0);
    }

    cell AMX_NATIVE_CALL native_mariadb_async_stmt_bind_float(AMX* amx, cell* params)
    {
        auto stmt = require_handle(amx, params[1], g_async_stmts, "MariaDB async stmt");
        return stmt && stmt->bind_float(static_cast<unsigned int>(params[2]), amx_ctof(params[3]));
    }

    cell AMX_NATIVE_CALL native_mariadb_async_stmt_bind_string(AMX* amx, cell* params)
    {
        auto stmt = require_handle(amx, params[1], g_async_stmts, "MariaDB async stmt");
        return stmt && stmt->bind_string(static_cast<unsigned int>(params[2]), get_amx_string(amx, params[3], 0));
    }

    cell AMX_NATIVE_CALL native_mariadb_async_stmt_bind_null(AMX* amx, cell* params)
    {
        auto stmt = require_handle(amx, params[1], g_async_stmts, "MariaDB async stmt");
        return stmt && stmt->bind_null(static_cast<unsigned int>(params[2]));
    }

    cell create_async_stmt_job(AMX* amx, cell* params, bool exec_mode)
    {
        const auto param_count = params[0] / sizeof(cell);
        if (!g_async_worker)
        {
            MF_LogError(amx, AMX_ERR_NATIVE, "MariaDB async worker is not running.");
            return k_invalid_handle;
        }

        auto connection = require_connection(amx, params[1]);
        if (!connection)
        {
            return k_invalid_handle;
        }

        auto stmt = require_handle(amx, params[3], g_async_stmts, "MariaDB async stmt");
        if (!stmt)
        {
            return k_invalid_handle;
        }

        const auto callback = get_amx_string(amx, params[2], 0);
        const int forward = MF_RegisterSPForwardByName(
            amx,
            callback.c_str(),
            FP_CELL,
            FP_CELL,
            FP_CELL,
            FP_CELL,
            FP_STRING,
            FP_CELL,
            FP_ARRAY,
            FP_CELL,
            FP_FLOAT,
            FP_DONE);

        if (forward < 1)
        {
            MF_LogError(amx, AMX_ERR_NATIVE, "Function not found: %s", callback.c_str());
            return k_invalid_handle;
        }

        const auto data_size = param_count >= 5 ? static_cast<size_t>(params[5]) : 0u;
        auto job = std::make_shared<async_job>();
        job->amx         = amx;
        job->forward_id  = forward;
        job->options     = connection->options();
        job->query       = stmt->query();
        job->params      = stmt->params();  // copy current bindings
        job->exec_mode   = exec_mode;
        job->use_prepared = true;
        job->enqueue_time = std::chrono::steady_clock::now();

        if (param_count >= 4 && data_size > 0)
        {
            cell* data = MF_GetAmxAddr(amx, params[4]);
            job->data.assign(data, data + data_size);
        }

        job->handle = g_jobs.create(job);
        g_async_worker->enqueue(job);
        return job->handle;
    }

    cell AMX_NATIVE_CALL native_mariadb_async_stmt_exec(AMX* amx, cell* params)
    {
        return create_async_stmt_job(amx, params, true);
    }

    cell AMX_NATIVE_CALL native_mariadb_async_stmt_query(AMX* amx, cell* params)
    {
        return create_async_stmt_job(amx, params, false);
    }

    cell AMX_NATIVE_CALL native_mariadb_job_cancel(AMX* amx, cell* params)
    {
        auto job = require_job(amx, params[1]);
        if (!job)
        {
            return 0;
        }

        async_job_state expected = async_job_state::queued;
        return job->state.compare_exchange_strong(expected, async_job_state::cancelled);
    }
}

// ============================================================================
// NATIVE TABLES
// ============================================================================

AMX_NATIVE_INFO g_mariadb_natives[] =
{
    {"mariadb_connect",          native_mariadb_connect},
    {"mariadb_disconnect",       native_mariadb_disconnect},
    {"mariadb_ping",             native_mariadb_ping},
    {"mariadb_set_charset",      native_mariadb_set_charset},
    {"mariadb_get_error",        native_mariadb_get_error},
    {"mariadb_get_error_code",   native_mariadb_get_error_code},
    {"mariadb_server_version",   native_mariadb_server_version},
    {"mariadb_escape_string",    native_mariadb_escape_string},
    {"mariadb_monotonic_time",   native_mariadb_monotonic_time},
    {"mariadb_query",            native_mariadb_query},
    {"mariadb_exec",             native_mariadb_exec},
    {"mariadb_result_close",     native_mariadb_result_close},
    {"mariadb_next_row",         native_mariadb_next_row},
    {"mariadb_rewind",           native_mariadb_rewind},
    {"mariadb_row_count",        native_mariadb_row_count},
    {"mariadb_column_count",     native_mariadb_column_count},
    {"mariadb_column_index",     native_mariadb_column_index},
    {"mariadb_column_name",      native_mariadb_column_name},
    {"mariadb_is_null",          native_mariadb_is_null},
    {"mariadb_read_int",         native_mariadb_read_int},
    {"mariadb_read_bool",        native_mariadb_read_bool},
    {"mariadb_read_float",       native_mariadb_read_float},
    {"mariadb_read_string",      native_mariadb_read_string},
    {"mariadb_prepare",          native_mariadb_prepare},
    {"mariadb_stmt_close",  native_mariadb_stmt_close},
    {"mariadb_stmt_reset",       native_mariadb_stmt_reset},
    {"mariadb_bind_int",         native_mariadb_bind_int},
    {"mariadb_bind_bool",        native_mariadb_bind_bool},
    {"mariadb_bind_float",       native_mariadb_bind_float},
    {"mariadb_bind_string",      native_mariadb_bind_string},
    {"mariadb_bind_null",        native_mariadb_bind_null},
    {"mariadb_stmt_query",       native_mariadb_stmt_query},
    {"mariadb_stmt_exec",        native_mariadb_stmt_exec},
    {"mariadb_stmt_error",       native_mariadb_stmt_error},
    {"mariadb_stmt_error_code",  native_mariadb_stmt_error_code},
    {"mariadb_begin",            native_mariadb_begin},
    {"mariadb_commit",           native_mariadb_commit},
    {"mariadb_rollback",         native_mariadb_rollback},
    {nullptr, nullptr}
};

AMX_NATIVE_INFO g_mariadb_async_natives[] =
{
    {"mariadb_async_query", native_mariadb_async_query},
    {"mariadb_async_exec",  native_mariadb_async_exec},
    {"mariadb_job_cancel",  native_mariadb_job_cancel},
    {"mariadb_async_stmt_create",         native_mariadb_async_stmt_create},
    {"mariadb_async_stmt_close",          native_mariadb_async_stmt_close},
    {"mariadb_async_stmt_bind_int",       native_mariadb_async_stmt_bind_int},
    {"mariadb_async_stmt_bind_bool",      native_mariadb_async_stmt_bind_bool},
    {"mariadb_async_stmt_bind_float",     native_mariadb_async_stmt_bind_float},
    {"mariadb_async_stmt_bind_string",    native_mariadb_async_stmt_bind_string},
    {"mariadb_async_stmt_bind_null",      native_mariadb_async_stmt_bind_null},
    {"mariadb_async_stmt_exec",           native_mariadb_async_stmt_exec},
    {"mariadb_async_stmt_query",          native_mariadb_async_stmt_query},
    {nullptr, nullptr}
};
