#if defined _mariadb_included
    #endinput
#endif
#define _mariadb_included

#pragma semicolon 1

// ============================================================================
// CONSTANTS
// ============================================================================

#define MARIADB_DEFAULT_PORT        3306
#define MARIADB_DEFAULT_TIMEOUT_MS  5000
#define MARIADB_DEFAULT_CHARSET     "utf8mb4"
#define MARIADB_MAX_QUERY_LENGTH    16384
#define MARIADB_MAX_ERROR_LENGTH    512
#define MARIADB_COLUMN_NOT_FOUND    -1

// ============================================================================
// HANDLE TYPES
// ============================================================================

enum mariadb_connection  { invalid_mariadb_connection  = -1 };
enum mariadb_result      { invalid_mariadb_result      = -1 };
enum mariadb_statement   { invalid_mariadb_statement   = -1 };
enum mariadb_job         { invalid_mariadb_job         = -1 };

// ============================================================================
// ASYNC STATE
// ============================================================================

enum mariadb_async_state
{
    mariadb_async_connect_failed = -2,
    mariadb_async_query_failed   = -1,
    mariadb_async_ok             = 0
};

// ============================================================================
// CONNECTION API
// ============================================================================

/**
 * @brief Opens a synchronous MariaDB connection.
 *
 * @param[in]  host          Server hostname or IP address
 * @param[in]  user          MySQL user name
 * @param[in]  password      MySQL password
 * @param[in]  database      Default database to select on connect
 * @param[in]  port          Server port (default: MARIADB_DEFAULT_PORT)
 * @param[in]  charset       Connection charset (default: MARIADB_DEFAULT_CHARSET)
 * @param[in]  timeout_ms    Connect/read/write timeout in milliseconds
 * @param[in]  auto_reconnect Enable automatic reconnect on dropped connections
 * @param[out] error         Buffer for the error message on failure
 * @param[in]  maxlen        Size of the error buffer
 * @param[out] error_code    MySQL error code on failure
 *
 * @return Connection handle on success, invalid_mariadb_connection on failure
 */
native mariadb_connection:mariadb_connect(
    const host[],
    const user[],
    const password[],
    const database[],
    port = MARIADB_DEFAULT_PORT,
    const charset[] = MARIADB_DEFAULT_CHARSET,
    timeout_ms = MARIADB_DEFAULT_TIMEOUT_MS,
    bool:auto_reconnect = true,
    error[] = "",
    maxlen = 0,
    &error_code = 0
);

/**
 * @brief Closes a connection and invalidates the handle.
 *
 * @param[in,out] db  Connection handle; set to invalid_mariadb_connection on return
 */
native mariadb_disconnect(&mariadb_connection:db);

/**
 * @brief Sends a ping to verify the connection is alive.
 *
 * @param[in] db  Connection handle
 *
 * @return true if the connection responded, false otherwise
 */
native bool:mariadb_ping(mariadb_connection:db);

/**
 * @brief Changes the connection charset.
 *
 * @param[in] db       Connection handle
 * @param[in] charset  Charset name (example: "utf8mb4")
 *
 * @return true on success
 */
native bool:mariadb_set_charset(mariadb_connection:db, const charset[]);

/**
 * @brief Reads the last error message on the connection.
 *
 * @param[in]  db      Connection handle
 * @param[out] buffer  Destination buffer
 * @param[in]  maxlen  Size of buffer
 */
native mariadb_get_error(mariadb_connection:db, buffer[], maxlen);

/**
 * @brief Returns the MySQL error code for the last connection error.
 *
 * @param[in] db  Connection handle
 *
 * @return MySQL error code, or 0 if no error
 */
native mariadb_get_error_code(mariadb_connection:db);

/**
 * @brief Reads the server version string.
 *
 * @param[in]  db      Connection handle
 * @param[out] buffer  Destination buffer
 * @param[in]  maxlen  Size of buffer
 */
native mariadb_server_version(mariadb_connection:db, buffer[], maxlen);

/**
 * @brief Escapes a value for safe use in a query string.
 *
 * Prefer prepared statements over escaping wherever possible.
 *
 * @param[in]  db      Connection handle (used for charset-aware escaping)
 * @param[out] buffer  Destination buffer for the escaped string
 * @param[in]  maxlen  Size of buffer
 * @param[in]  value   Raw input string to escape
 *
 * @return Number of bytes written into buffer, or -1 on error
 */
native mariadb_escape_string(mariadb_connection:db, buffer[], maxlen, const value[]);

/**
 * @brief Returns monotonic wall-clock seconds since module initialization.
 *
 * useful for measuring sync performance from Pawn without relying on
 * frame-quantized engine time
 *
 * @return elapsed seconds as a Float
 */
native Float:mariadb_monotonic_time();

// ============================================================================
// QUERY API
// ============================================================================

/**
 * @brief Executes a SELECT query and returns a result handle.
 *
 * @param[in] db     Connection handle
 * @param[in] query  SQL query string
 *
 * @return Result handle on success, invalid_mariadb_result on failure
 */
native mariadb_result:mariadb_query(mariadb_connection:db, const query[]);

/**
 * @brief Executes an INSERT, UPDATE, or DELETE query.
 *
 * @param[in]  db             Connection handle
 * @param[in]  query          SQL query string
 * @param[out] affected_rows  Number of rows affected
 * @param[out] insert_id      Last auto-increment insert ID
 *
 * @return true on success
 */
native bool:mariadb_exec(
    mariadb_connection:db,
    const query[],
    &affected_rows = 0,
    &insert_id = 0
);

// ============================================================================
// RESULT API
// ============================================================================

/**
 * @brief Releases a result handle.
 *
 * @param[in,out] result  Result handle; set to invalid_mariadb_result on return
 *
 * @return true on success, or if handle was already invalid
 */
native mariadb_result_close(&mariadb_result:result);

/**
 * @brief Advances the cursor to the next row.
 *
 * @param[in] result  Result handle
 *
 * @return true if a row is now current, false when past the last row
 */
native bool:mariadb_next_row(mariadb_result:result);

/**
 * @brief Resets the cursor to before the first row.
 *
 * @param[in] result  Result handle
 */
native bool:mariadb_rewind(mariadb_result:result);

/**
 * @brief Returns the number of rows in the result.
 *
 * @param[in] result  Result handle
 */
native mariadb_row_count(mariadb_result:result);

/**
 * @brief Returns the number of columns in the result.
 *
 * @param[in] result  Result handle
 */
native mariadb_column_count(mariadb_result:result);

/**
 * @brief Returns the 0-based column index for a column name.
 *
 * @param[in] result  Result handle
 * @param[in] name    Column name to look up
 *
 * @return Column index, or MARIADB_COLUMN_NOT_FOUND if name is absent
 */
native mariadb_column_index(mariadb_result:result, const name[]);

/**
 * @brief Reads the column name for a given 0-based column index.
 *
 * @param[in]  result  Result handle
 * @param[in]  column  0-based column index
 * @param[out] buffer  Destination buffer
 * @param[in]  maxlen  Size of buffer
 */
native mariadb_column_name(mariadb_result:result, column, buffer[], maxlen);

/**
 * @brief Returns true if the current row's value at column is SQL NULL.
 *
 * @param[in] result  Result handle
 * @param[in] column  0-based column index
 */
native bool:mariadb_is_null(mariadb_result:result, column);

/**
 * @brief Reads the current row's value at column as an integer.
 *
 * @param[in] result  Result handle
 * @param[in] column  0-based column index
 *
 * @return Integer value, or 0 if NULL
 */
native mariadb_read_int(mariadb_result:result, column);

/**
 * @brief Reads the current row's value at column as a boolean.
 *
 * @param[in] result  Result handle
 * @param[in] column  0-based column index
 *
 * @return true if value is non-zero, false if zero or NULL
 */
native bool:mariadb_read_bool(mariadb_result:result, column);

/**
 * @brief Reads the current row's value at column as a float.
 *
 * @param[in] result  Result handle
 * @param[in] column  0-based column index
 *
 * @return Float value, or 0.0 if NULL
 */
native Float:mariadb_read_float(mariadb_result:result, column);

/**
 * @brief Reads the current row's value at column into a string buffer.
 *
 * @param[in]  result  Result handle
 * @param[in]  column  0-based column index
 * @param[out] buffer  Destination buffer
 * @param[in]  maxlen  Size of buffer
 *
 * @return Number of characters written
 */
native mariadb_read_string(mariadb_result:result, column, buffer[], maxlen);

// ============================================================================
// PREPARED STATEMENT API
// ============================================================================

/**
 * @brief Prepares a parameterised SQL statement.
 *
 * Use `?` placeholders in the query and bind values with mariadb_bind_*.
 *
 * @param[in] db     Connection handle
 * @param[in] query  SQL query with `?` placeholders
 *
 * @return Statement handle on success, invalid_mariadb_statement on failure
 */
native mariadb_statement:mariadb_prepare(mariadb_connection:db, const query[]);

/**
 * @brief Closes a prepared statement and invalidates the handle.
 *
 * @param[in,out] stmt  Statement handle; set to invalid_mariadb_statement on return
 */
native mariadb_stmt_close(&mariadb_statement:stmt);

/**
 * @brief Clears all parameter bindings and resets the server-side cursor.
 *
 * @param[in] stmt  Statement handle
 *
 * @return true on success
 */
native bool:mariadb_stmt_reset(mariadb_statement:stmt);

/**
 * @brief Binds an integer to a `?` parameter.
 *
 * @param[in] stmt        Statement handle
 * @param[in] param_index 0-based parameter index
 * @param[in] value       Integer value to bind
 */
native bool:mariadb_bind_int(mariadb_statement:stmt, param_index, value);

/**
 * @brief Binds a boolean to a `?` parameter.
 *
 * @param[in] stmt        Statement handle
 * @param[in] param_index 0-based parameter index
 * @param[in] value       Bool value to bind
 */
native bool:mariadb_bind_bool(mariadb_statement:stmt, param_index, bool:value);

/**
 * @brief Binds a float to a `?` parameter.
 *
 * @param[in] stmt        Statement handle
 * @param[in] param_index 0-based parameter index
 * @param[in] value       Float value to bind
 */
native bool:mariadb_bind_float(mariadb_statement:stmt, param_index, Float:value);

/**
 * @brief Binds a string to a `?` parameter.
 *
 * @param[in] stmt        Statement handle
 * @param[in] param_index 0-based parameter index
 * @param[in] value       String value to bind
 */
native bool:mariadb_bind_string(mariadb_statement:stmt, param_index, const value[]);

/**
 * @brief Binds SQL NULL to a `?` parameter.
 *
 * @param[in] stmt        Statement handle
 * @param[in] param_index 0-based parameter index
 */
native bool:mariadb_bind_null(mariadb_statement:stmt, param_index);

/**
 * @brief Executes a prepared SELECT statement and returns a result handle.
 *
 * @param[in] stmt  Statement handle
 *
 * @return Result handle on success, invalid_mariadb_result on failure
 */
native mariadb_result:mariadb_stmt_query(mariadb_statement:stmt);

/**
 * @brief Executes a prepared INSERT, UPDATE, or DELETE statement.
 *
 * @param[in]  stmt          Statement handle
 * @param[out] affected_rows Number of rows affected
 * @param[out] insert_id     Last auto-increment insert ID
 *
 * @return true on success
 */
native bool:mariadb_stmt_exec(
    mariadb_statement:stmt,
    &affected_rows = 0,
    &insert_id = 0
);

/**
 * @brief Reads the last error message on the statement.
 *
 * @param[in]  stmt    Statement handle
 * @param[out] buffer  Destination buffer
 * @param[in]  maxlen  Size of buffer
 */
native mariadb_stmt_error(mariadb_statement:stmt, buffer[], maxlen);

/**
 * @brief Returns the MySQL error code for the last statement error.
 *
 * @param[in] stmt  Statement handle
 *
 * @return MySQL error code, or 0 if no error
 */
native mariadb_stmt_error_code(mariadb_statement:stmt);

// ============================================================================
// TRANSACTION API
// ============================================================================

/**
 * @brief Begins a transaction (disables auto-commit).
 *
 * @param[in] db  Connection handle
 *
 * @return true on success
 */
native bool:mariadb_begin(mariadb_connection:db);

/**
 * @brief Commits the current transaction and re-enables auto-commit.
 *
 * @param[in] db  Connection handle
 *
 * @return true on success
 */
native bool:mariadb_commit(mariadb_connection:db);

/**
 * @brief Rolls back the current transaction and re-enables auto-commit.
 *
 * @param[in] db  Connection handle
 *
 * @return true on success
 */
native bool:mariadb_rollback(mariadb_connection:db);

// ============================================================================
// ASYNC API
// ============================================================================

/**
 * @brief Executes a SELECT query on the worker thread.
 *
 * Callback signature:
 * `public callback(mariadb_async_state:state, mariadb_result:result, affected_rows, insert_id, const error[], error_code, const data[], data_size, Float:queue_time)`
 *
 * the result handle is released automatically after the callback returns
 * queue_time is end-to-end elapsed seconds from enqueue to callback dispatch
 *
 * @param[in] db        Connection handle (used only for its options)
 * @param[in] callback  Public function name in the calling plugin
 * @param[in] query     SQL SELECT query string
 * @param[in] data      Optional user payload array passed back in the callback
 * @param[in] data_size Number of cells in data
 *
 * @return Job handle on success, invalid_mariadb_job on failure
 */
native mariadb_job:mariadb_async_query(
    mariadb_connection:db,
    const callback[],
    const query[],
    const data[] = "",
    data_size = 0
);

/**
 * @brief Executes a write query on the worker thread.
 *
 * Callback signature matches mariadb_async_query().
 *
 * @param[in] db        Connection handle (used only for its options)
 * @param[in] callback  Public function name in the calling plugin
 * @param[in] query     SQL INSERT/UPDATE/DELETE query string
 * @param[in] data      Optional user payload array passed back in the callback
 * @param[in] data_size Number of cells in data
 *
 * @return Job handle on success, invalid_mariadb_job on failure
 */
native mariadb_job:mariadb_async_exec(
    mariadb_connection:db,
    const callback[],
    const query[],
    const data[] = "",
    data_size = 0
);

/**
 * @brief Attempts to cancel a queued async job.
 *
 * cancellation only succeeds while the job is still in the queue - once
 * running it cannot be stopped, and the callback will not fire on success
 *
 * @param[in] job  Job handle returned by mariadb_async_query or mariadb_async_exec
 *
 * @return true if the job was successfully cancelled
 */
native bool:mariadb_job_cancel(mariadb_job:job);

// ============================================================================
// ASYNC STMT API
// ============================================================================

enum mariadb_async_stmt { invalid_mariadb_async_stmt = -1 };

/**
 * @brief Creates a reusable async statement template.
 *
 * Count `?` placeholders in query to determine the number of parameter slots.
 *
 * @param[in] query  SQL query with `?` placeholders
 *
 * @return Statement handle on success, invalid_mariadb_async_stmt on failure
 */
native mariadb_async_stmt:mariadb_async_stmt_create(const query[]);

/**
 * @brief Closes and invalidates an async statement handle.
 *
 * @param[in,out] stmt  Async statement handle; set to invalid_mariadb_async_stmt on return
 *
 * @return true on success, or if handle was already invalid
 */
native bool:mariadb_async_stmt_close(&mariadb_async_stmt:stmt);

/**
 * @brief Binds an integer to a `?` parameter slot.
 *
 * @param[in] stmt        Async statement handle
 * @param[in] param_index 0-based parameter index
 * @param[in] value       Integer value to bind
 *
 * @return true on success
 */
native bool:mariadb_async_stmt_bind_int(mariadb_async_stmt:stmt, param_index, value);

/**
 * @brief Binds a boolean to a `?` parameter slot.
 *
 * @param[in] stmt        Async statement handle
 * @param[in] param_index 0-based parameter index
 * @param[in] value       Bool value to bind
 *
 * @return true on success
 */
native bool:mariadb_async_stmt_bind_bool(mariadb_async_stmt:stmt, param_index, bool:value);

/**
 * @brief Binds a float to a `?` parameter slot.
 *
 * @param[in] stmt        Async statement handle
 * @param[in] param_index 0-based parameter index
 * @param[in] value       Float value to bind
 *
 * @return true on success
 */
native bool:mariadb_async_stmt_bind_float(mariadb_async_stmt:stmt, param_index, Float:value);

/**
 * @brief Binds a string to a `?` parameter slot.
 *
 * @param[in] stmt        Async statement handle
 * @param[in] param_index 0-based parameter index
 * @param[in] value       String value to bind
 *
 * @return true on success
 */
native bool:mariadb_async_stmt_bind_string(mariadb_async_stmt:stmt, param_index, const value[]);

/**
 * @brief Binds SQL NULL to a `?` parameter slot.
 *
 * @param[in] stmt        Async statement handle
 * @param[in] param_index 0-based parameter index
 *
 * @return true on success
 */
native bool:mariadb_async_stmt_bind_null(mariadb_async_stmt:stmt, param_index);

/**
 * @brief Executes a prepared INSERT, UPDATE, or DELETE asynchronously.
 *
 * Current bindings are copied at call time; the handle remains valid and can
 * be rebound and re-fired immediately.
 *
 * Callback signature identical to mariadb_async_exec().
 *
 * @param[in] db        Connection handle (used for its options)
 * @param[in] callback  Public function name in the calling plugin
 * @param[in] stmt      Async statement handle with current bindings
 * @param[in] data      Optional user payload array passed back in the callback
 * @param[in] data_size Number of cells in data
 *
 * @return Job handle on success, invalid_mariadb_job on failure
 */
native mariadb_job:mariadb_async_stmt_exec(
    mariadb_connection:db,
    const callback[],
    mariadb_async_stmt:stmt,
    const data[] = "",
    data_size = 0
);

/**
 * @brief Executes a prepared SELECT asynchronously.
 *
 * Current bindings are copied at call time; the handle remains valid and can
 * be rebound and re-fired immediately.
 *
 * Callback signature identical to mariadb_async_query().
 *
 * @param[in] db        Connection handle
 * @param[in] callback  Public function name in the calling plugin
 * @param[in] stmt      Async statement handle with current bindings
 * @param[in] data      Optional user payload array passed back in the callback
 * @param[in] data_size Number of cells in data
 *
 * @return Job handle on success, invalid_mariadb_job on failure
 */
native mariadb_job:mariadb_async_stmt_query(
    mariadb_connection:db,
    const callback[],
    mariadb_async_stmt:stmt,
    const data[] = "",
    data_size = 0
);

// ============================================================================
// HANDLE VALIDITY STOCKS
// ============================================================================

stock bool:mariadb_connection_valid(mariadb_connection:db)
{
    return db != invalid_mariadb_connection;
}

stock bool:mariadb_result_valid(mariadb_result:result)
{
    return result != invalid_mariadb_result;
}

stock bool:mariadb_statement_valid(mariadb_statement:stmt)
{
    return stmt != invalid_mariadb_statement;
}

stock bool:mariadb_job_valid(mariadb_job:job)
{
    return job != invalid_mariadb_job;
}

stock bool:mariadb_async_stmt_valid(mariadb_async_stmt:stmt)
{
    return stmt != invalid_mariadb_async_stmt;
}
