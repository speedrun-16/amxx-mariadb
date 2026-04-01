#pragma semicolon 1
#pragma compress 1

#include <amxmodx>
#include <amxmisc>

#define PLUGIN  "MariaDB Driver Test Suite"
#define VERSION "1.0"
#define AUTHOR  "PWNED"

#include <mariadb>

// ============================================================================
// CONSTANTS
// ============================================================================

#define DEFAULT_HOST       "localhost"
#define DEFAULT_USER       "root"
#define DEFAULT_PASS       "123456"
#define DEFAULT_DATABASE   "amxx_mariadb_test"
#define DEFAULT_PREFIX     "amxx_mariadb_test"
#define DEFAULT_PORT       "3394"

#define TEST_BATCH_SYNC                100
#define TEST_BATCH_TX_ROLLBACK         200
#define TEST_BATCH_TX_COMMIT           201
#define TEST_BATCH_STMT               300
#define TEST_BATCH_STMT_QUERY         301
#define TEST_BATCH_INJECTION_PREP     400
#define TEST_BATCH_SYNC_PERF          500
#define TEST_BATCH_ASYNC_STRESS       600
#define TEST_BATCH_CANCEL_TARGET      700
#define TEST_BATCH_ASYNC_STMT         800

// value set - async callback kinds
enum async_kind
{
    async_none = 0,
    async_cancel_blocker,
    async_cancel_target,
    async_stress_exec,
    async_stress_verify,
    async_stmt_exec,
    async_stmt_verify
};

// struct-like payload carried through async callbacks
enum _:async_payload_t
{
    m_kind = 0,
    m_index,
    m_batch_id
};

// task IDs used with set_task to sequence the suite stages
enum suite_task
{
    suite_task_connection_schema = 38001,
    suite_task_sync,
    suite_task_transactions,
    suite_task_statements,
    suite_task_errors,
    suite_task_injection,
    suite_task_sync_perf
};

// ============================================================================
// GLOBALS
// ============================================================================

new g_pcvar_host;
new g_pcvar_user;
new g_pcvar_pass;
new g_pcvar_database;
new g_pcvar_port;
new g_pcvar_charset;
new g_pcvar_timeout;
new g_pcvar_prefix;
new g_pcvar_keep_tables;
new g_pcvar_autorun;
new g_pcvar_sync_loops;
new g_pcvar_async_jobs;
new g_pcvar_disable_sync;
new g_pcvar_verbose;

new mariadb_connection:g_db = invalid_mariadb_connection;
new g_table_name[96];

new bool:g_run_active;
new g_total_asserts;
new g_failed_asserts;

new g_async_expected_jobs;
new g_async_seen_jobs;
new g_async_success_jobs;
new g_async_error_jobs;
new bool:g_cancel_callback_fired;
new mariadb_job:g_cancelled_job = invalid_mariadb_job;
new Float:g_async_started_at;
new Float:g_async_total_queue_time;
new Float:g_async_min_queue_time;
new Float:g_async_max_queue_time;

// ============================================================================
// LIFECYCLE
// ============================================================================

public plugin_init()
{
    register_plugin(PLUGIN, VERSION, AUTHOR);

    register_concmd("amx_mariadb_test_run", "command_run_tests", ADMIN_RCON, "Runs the MariaDB module integration suite.");
    register_concmd("amx_mariadb_test_cleanup", "command_cleanup", ADMIN_RCON, "Drops the MariaDB test table.");

    g_pcvar_host = create_cvar("mariadb_test_host", DEFAULT_HOST, FCVAR_PROTECTED, "MariaDB host for the integration suite.");
    g_pcvar_user = create_cvar("mariadb_test_user", DEFAULT_USER, FCVAR_PROTECTED, "MariaDB user for the integration suite.");
    g_pcvar_pass = create_cvar("mariadb_test_pass", DEFAULT_PASS, FCVAR_PROTECTED, "MariaDB password for the integration suite.");
    g_pcvar_database = create_cvar("mariadb_test_db", DEFAULT_DATABASE, FCVAR_PROTECTED, "MariaDB database for the integration suite.");
    g_pcvar_port = create_cvar("mariadb_test_port", DEFAULT_PORT, FCVAR_PROTECTED, "MariaDB port for the integration suite.");
    g_pcvar_charset = create_cvar("mariadb_test_charset", MARIADB_DEFAULT_CHARSET, FCVAR_PROTECTED, "Connection charset used by the integration suite.");
    g_pcvar_timeout = create_cvar("mariadb_test_timeout_ms", "5000", FCVAR_PROTECTED, "Connection timeout in milliseconds.");
    g_pcvar_prefix = create_cvar("mariadb_test_prefix", DEFAULT_PREFIX, FCVAR_PROTECTED, "Prefix used for test tables.");
    g_pcvar_keep_tables = create_cvar("mariadb_test_keep_tables", "1", FCVAR_PROTECTED, "Keep the generated test table after the suite finishes.");
    g_pcvar_autorun = create_cvar("mariadb_test_autorun", "0", FCVAR_PROTECTED, "Automatically run the suite on plugin_cfg.");
    g_pcvar_sync_loops = create_cvar("mariadb_test_sync_loops", "100", FCVAR_PROTECTED, "Number of sync statement execs in the throughput sample.");
    g_pcvar_async_jobs = create_cvar("mariadb_test_async_jobs", "48", FCVAR_PROTECTED, "Number of async execs in the throughput sample.");
    g_pcvar_disable_sync = create_cvar("mariadb_test_disable_sync", "0", FCVAR_PROTECTED, "Skip synchronous validation stages and run only setup plus async tests.");
    g_pcvar_verbose = create_cvar("mariadb_test_verbose", "0", FCVAR_PROTECTED, "Log every PASS line to the server console/logs.");
}

public plugin_cfg()
{
    if (get_pcvar_num(g_pcvar_autorun) != 0)
    {
        set_task(2.0, "task_autorun");
    }
}

// ============================================================================
// COMMAND HANDLERS
// ============================================================================

public command_run_tests(id, level, cid)
{
    if (!cmd_access(id, level, cid, 1))
    {
        return PLUGIN_HANDLED;
    }

    start_full_suite();
    return PLUGIN_HANDLED;
}

public command_cleanup(id, level, cid)
{
    new mariadb_connection:db;
    new query[160];

    if (!cmd_access(id, level, cid, 1))
    {
        return PLUGIN_HANDLED;
    }

    if (g_run_active)
    {
        console_print(id, "[MariaDBTest] Cannot cleanup while a suite run is active.");
        return PLUGIN_HANDLED;
    }

    build_table_name();
    db = connect_for_run();

    if (!mariadb_connection_valid(db))
    {
        console_print(id, "[MariaDBTest] Cleanup connection failed. Check mariadb_test_* cvars and logs.");
        return PLUGIN_HANDLED;
    }

    formatex(query, charsmax(query), "DROP TABLE IF EXISTS `%s`", g_table_name);
    if (mariadb_exec(db, query))
    {
        console_print(id, "[MariaDBTest] Dropped test table `%s`.", g_table_name);
    }
    else
    {
        console_print(id, "[MariaDBTest] Cleanup failed. Check logs.");
    }

    mariadb_disconnect(db);
    return PLUGIN_HANDLED;
}

// ============================================================================
// TASK HANDLERS
// ============================================================================

public task_autorun()
{
    start_full_suite();
}

public task_run_connection_schema()
{
    if (!can_run_stage())
    {
        return;
    }

    if (!sync_tests_disabled())
    {
        run_connection_suite();
    }
    else
    {
        log_suite("Sync test stages disabled via mariadb_test_disable_sync; async-only path will run after schema setup.");
    }

    if (!prepare_schema())
    {
        finish_suite();
        return;
    }

    if (sync_tests_disabled())
    {
        start_async_cancel_suite();
        return;
    }

    queue_suite_task(suite_task_sync, "task_run_sync");
}

public task_run_sync()
{
    if (!can_run_stage())
    {
        return;
    }

    run_sync_suite();
    queue_suite_task(suite_task_transactions, "task_run_transactions");
}

public task_run_transactions()
{
    if (!can_run_stage())
    {
        return;
    }

    run_transaction_suite();
    queue_suite_task(suite_task_statements, "task_run_statements");
}

public task_run_statements()
{
    if (!can_run_stage())
    {
        return;
    }

    run_statement_suite();
    queue_suite_task(suite_task_errors, "task_run_errors");
}

public task_run_errors()
{
    if (!can_run_stage())
    {
        return;
    }

    run_error_suite();
    queue_suite_task(suite_task_injection, "task_run_injection");
}

public task_run_injection()
{
    if (!can_run_stage())
    {
        return;
    }

    run_injection_suite();
    queue_suite_task(suite_task_sync_perf, "task_run_sync_perf");
}

public task_run_sync_perf()
{
    if (!can_run_stage())
    {
        return;
    }

    run_sync_perf_sample();
    start_async_cancel_suite();
}

public task_verify_cancelled_job()
{
    new count;

    assert_true(!g_cancel_callback_fired, "Cancelled async job did not invoke its callback");
    assert_true(fetch_batch_count(TEST_BATCH_CANCEL_TARGET, count), "Can query row count for cancelled job batch");
    assert_true(count == 0, "Cancelled async job did not write data");

    start_async_stress_suite();
}

// ============================================================================
// ASYNC CALLBACKS
// ============================================================================

public on_mariadb_async(async_state_cell, result, affected_rows, insert_id, error[], error_code, data[], data_size, Float:queue_time)
{
    new mariadb_async_state:async_state;
    new mariadb_result:result_handle;

    async_state = mariadb_async_state:async_state_cell;
    result_handle = mariadb_result:result;

    if (!assert_true(data_size == async_payload_t, "Async callback preserved the payload size"))
    {
        return;
    }

    switch (data[m_kind])
    {
        case async_cancel_blocker:
        {
            assert_true(async_state == mariadb_async_ok, "Async blocker query completed");
            assert_true(mariadb_result_valid(result_handle), "Async blocker query returned a result handle");
            if (mariadb_result_valid(result_handle))
            {
                assert_true(mariadb_next_row(result_handle), "Async blocker result has one row");
            }
            set_task(0.2, "task_verify_cancelled_job");
        }

        case async_cancel_target:
        {
            g_cancel_callback_fired = true;
            record_failure("Cancelled async job unexpectedly fired a callback (state=%d, err=%d, msg=%s)", async_state_cell, error_code, error);
        }

        case async_stress_exec:
        {
            g_async_seen_jobs++;
            g_async_total_queue_time += queue_time;
            if (g_async_min_queue_time < 0.0 || queue_time < g_async_min_queue_time)
            {
                g_async_min_queue_time = queue_time;
            }
            if (queue_time > g_async_max_queue_time)
            {
                g_async_max_queue_time = queue_time;
            }

            if (async_state == mariadb_async_ok && affected_rows == 1 && insert_id > 0)
            {
                g_async_success_jobs++;
            }
            else
            {
                g_async_error_jobs++;
                record_failure("Async stress job %d failed (state=%d, err=%d, msg=%s)", data[m_index], async_state_cell, error_code, error);
            }

            if (g_async_seen_jobs >= g_async_expected_jobs)
            {
                new verify_query[160];
                new verify_data[async_payload_t];
                new mariadb_job:verify_job;
                new Float:elapsed_seconds = mariadb_monotonic_time() - g_async_started_at;
                new elapsed_ms = floatround(elapsed_seconds * 1000.0);
                new rate = (elapsed_seconds > 0.0) ? floatround(float(g_async_success_jobs) / elapsed_seconds) : g_async_success_jobs;
                new avg_queue_us = (g_async_seen_jobs > 0)
                    ? floatround(g_async_total_queue_time * 1000000.0 / float(g_async_seen_jobs))
                    : 0;
                new min_queue_us = (g_async_min_queue_time >= 0.0) ? floatround(g_async_min_queue_time * 1000000.0) : 0;
                new max_queue_us = floatround(g_async_max_queue_time * 1000000.0);

                log_suite(
                    "Async throughput sample: %d/%d callbacks OK in %d ms (~%d ops/s), end-to-end latency avg/min/max %d/%d/%d us.",
                    g_async_success_jobs,
                    g_async_expected_jobs,
                    elapsed_ms,
                    rate,
                    avg_queue_us,
                    min_queue_us,
                    max_queue_us
                );

                formatex(verify_query, charsmax(verify_query),
                    "SELECT COUNT(*) FROM `%s` WHERE batch_id = %d",
                    g_table_name,
                    TEST_BATCH_ASYNC_STRESS);

                verify_data[m_kind] = _:async_stress_verify;
                verify_data[m_index] = g_async_expected_jobs;
                verify_data[m_batch_id] = TEST_BATCH_ASYNC_STRESS;

                verify_job = mariadb_async_query(g_db, "on_mariadb_async", verify_query, verify_data, sizeof(verify_data));
                if (!mariadb_job_valid(verify_job))
                {
                    record_failure("Failed to queue async verification query");
                    finish_suite();
                }
            }
        }

        case async_stress_verify:
        {
            assert_true(async_state == mariadb_async_ok, "Async verification SELECT completed");
            assert_true(g_async_error_jobs == 0, "Async stress run finished without callback failures");
            assert_true(mariadb_result_valid(result_handle), "Async verification SELECT returned a result handle");

            if (mariadb_result_valid(result_handle))
            {
                assert_true(mariadb_next_row(result_handle), "Async verification SELECT contains a row");
                assert_true(mariadb_read_int(result_handle, 0) == g_async_expected_jobs, "Async stress inserted exactly the queued number of rows");
            }

            start_async_stmt_suite();
        }

        case async_stmt_exec:
        {
            g_async_seen_jobs++;
            g_async_total_queue_time += queue_time;
            if (g_async_min_queue_time < 0.0 || queue_time < g_async_min_queue_time)
            {
                g_async_min_queue_time = queue_time;
            }
            if (queue_time > g_async_max_queue_time)
            {
                g_async_max_queue_time = queue_time;
            }

            if (async_state == mariadb_async_ok && affected_rows == 1 && insert_id > 0)
            {
                g_async_success_jobs++;
            }
            else
            {
                g_async_error_jobs++;
                record_failure("Async stmt job %d failed (state=%d, err=%d, msg=%s)", data[m_index], async_state_cell, error_code, error);
            }

            if (g_async_seen_jobs >= g_async_expected_jobs)
            {
                new verify_query[160];
                new verify_data[async_payload_t];
                new mariadb_job:verify_job;
                new Float:elapsed_seconds = mariadb_monotonic_time() - g_async_started_at;
                new elapsed_ms = floatround(elapsed_seconds * 1000.0);
                new rate = (elapsed_seconds > 0.0) ? floatround(float(g_async_success_jobs) / elapsed_seconds) : g_async_success_jobs;

                log_suite(
                    "Async params throughput sample: %d/%d callbacks OK in %d ms (~%d ops/s).",
                    g_async_success_jobs,
                    g_async_expected_jobs,
                    elapsed_ms,
                    rate
                );

                formatex(verify_query, charsmax(verify_query),
                    "SELECT COUNT(*) FROM `%s` WHERE batch_id = %d",
                    g_table_name,
                    TEST_BATCH_ASYNC_STMT);

                verify_data[m_kind] = _:async_stmt_verify;
                verify_data[m_index] = g_async_expected_jobs;
                verify_data[m_batch_id] = TEST_BATCH_ASYNC_STMT;

                verify_job = mariadb_async_query(g_db, "on_mariadb_async", verify_query, verify_data, sizeof(verify_data));
                if (!mariadb_job_valid(verify_job))
                {
                    record_failure("Failed to queue async stmt verification query");
                    finish_suite();
                }
            }
        }

        case async_stmt_verify:
        {
            assert_true(async_state == mariadb_async_ok, "Async stmt verification SELECT completed");
            assert_true(g_async_error_jobs == 0, "Async stmt run finished without callback failures");
            assert_true(mariadb_result_valid(result_handle), "Async stmt verification SELECT returned a result handle");

            if (mariadb_result_valid(result_handle))
            {
                assert_true(mariadb_next_row(result_handle), "Async stmt verification SELECT contains a row");
                assert_true(mariadb_read_int(result_handle, 0) == g_async_expected_jobs, "Async stmt inserted exactly the queued number of rows");
            }

            finish_suite();
        }
    }
}

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

stock start_full_suite()
{
    new database[64];

    if (g_run_active)
    {
        log_suite("A run is already active; ignoring duplicate start request.");
        return;
    }

    reset_suite_state();
    build_table_name();
    get_pcvar_string(g_pcvar_database, database, charsmax(database));

    log_suite("Starting full suite against database `%s`, table `%s`.", database, g_table_name);

    g_db = connect_for_run();
    if (!mariadb_connection_valid(g_db))
    {
        record_failure("mariadb_connect failed; aborting suite");
        finish_suite();
        return;
    }

    queue_suite_task(suite_task_connection_schema, "task_run_connection_schema");
}

stock queue_suite_task(suite_task:task_id, const function[])
{
    set_task(0.0, function, _:task_id);
}

stock bool:can_run_stage()
{
    return g_run_active && mariadb_connection_valid(g_db);
}

stock bool:sync_tests_disabled()
{
    return get_pcvar_num(g_pcvar_disable_sync) != 0;
}

stock reset_suite_state()
{
    g_run_active = true;
    g_total_asserts = 0;
    g_failed_asserts = 0;

    g_async_expected_jobs = 0;
    g_async_seen_jobs = 0;
    g_async_success_jobs = 0;
    g_async_error_jobs = 0;
    g_cancel_callback_fired = false;
    g_cancelled_job = invalid_mariadb_job;
    g_async_started_at = 0.0;
    g_async_total_queue_time = 0.0;
    g_async_min_queue_time = -1.0;
    g_async_max_queue_time = 0.0;
    g_table_name[0] = EOS;

    if (mariadb_connection_valid(g_db))
    {
        mariadb_disconnect(g_db);
    }
}

stock build_table_name()
{
    new prefix[64];
    new safe_prefix[64];

    get_pcvar_string(g_pcvar_prefix, prefix, charsmax(prefix));
    sanitize_identifier(prefix, safe_prefix, charsmax(safe_prefix));

    if (!safe_prefix[0])
    {
        copy(safe_prefix, charsmax(safe_prefix), DEFAULT_PREFIX);
    }

    formatex(g_table_name, charsmax(g_table_name), "%s_rows", safe_prefix);
}

stock sanitize_identifier(const source[], dest[], maxlen)
{
    new length;
    new ch;
    new i;

    for (i = 0; source[i] != EOS && length < maxlen; ++i)
    {
        ch = source[i];
        if (is_identifier_char(ch))
        {
            dest[length++] = ch;
        }
    }

    dest[length] = EOS;
}

stock bool:is_identifier_char(ch)
{
    return (ch >= 'a' && ch <= 'z')
        || (ch >= 'A' && ch <= 'Z')
        || (ch >= '0' && ch <= '9')
        || ch == '_';
}

stock mariadb_connection:connect_for_run()
{
    new host[64];
    new user[64];
    new pass[64];
    new database[64];
    new charset[32];
    new error[MARIADB_MAX_ERROR_LENGTH];
    new error_code;
    new mariadb_connection:db;

    get_pcvar_string(g_pcvar_host, host, charsmax(host));
    get_pcvar_string(g_pcvar_user, user, charsmax(user));
    get_pcvar_string(g_pcvar_pass, pass, charsmax(pass));
    get_pcvar_string(g_pcvar_database, database, charsmax(database));
    get_pcvar_string(g_pcvar_charset, charset, charsmax(charset));

    db = mariadb_connect(
        host,
        user,
        pass,
        database,
        bound_pcvar_int(g_pcvar_port, 1, 65535),
        charset,
        bound_pcvar_int(g_pcvar_timeout, 100, 60000),
        true,
        error,
        charsmax(error),
        error_code
    );

    if (!mariadb_connection_valid(db))
    {
        log_suite(
            "mariadb_connect(%s@%s:%d/%s, charset=%s, timeout=%dms) failed: %s (%d)",
            user,
            host,
            bound_pcvar_int(g_pcvar_port, 1, 65535),
            database,
            charset,
            bound_pcvar_int(g_pcvar_timeout, 100, 60000),
            error,
            error_code
        );
    }

    return db;
}

stock bound_pcvar_int(pcvar, minimum, maximum)
{
    new value = get_pcvar_num(pcvar);

    if (value < minimum)
    {
        return minimum;
    }

    if (value > maximum)
    {
        return maximum;
    }

    return value;
}

// ============================================================================
// CONNECTION STAGES
// ============================================================================

stock run_connection_suite()
{
    new version[64];
    new charset[32];

    get_pcvar_string(g_pcvar_charset, charset, charsmax(charset));

    assert_true(mariadb_ping(g_db), "mariadb_ping() succeeds immediately after connect");
    assert_true(mariadb_set_charset(g_db, charset), "mariadb_set_charset(%s) succeeds", charset);
    assert_true(mariadb_server_version(g_db, version, charsmax(version)) != 0, "mariadb_server_version() returns text");
    assert_true(version[0] != EOS, "Server version string is non-empty");
}

stock bool:prepare_schema()
{
    new query[512];

    formatex(query, charsmax(query), "DROP TABLE IF EXISTS `%s`", g_table_name);
    if (!mariadb_exec(g_db, query))
    {
        log_db_error("DROP TABLE failed");
        record_failure("Schema cleanup failed");
        return false;
    }

    formatex(query, charsmax(query), "CREATE TABLE `%s` (id INT NOT NULL AUTO_INCREMENT, batch_id INT NOT NULL, int_value INT NOT NULL, bool_value TINYINT(1) NOT NULL, float_value FLOAT NOT NULL, text_value VARCHAR(255) NOT NULL, nullable_value VARCHAR(255) NULL, created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY (id), KEY idx_batch_id (batch_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4", g_table_name);

    if (!mariadb_exec(g_db, query))
    {
        log_db_error("CREATE TABLE failed");
        record_failure("Schema creation failed");
        return false;
    }

    assert_true(true, "Test table `%s` created", g_table_name);
    return true;
}

// ============================================================================
// SYNC STAGES
// ============================================================================

stock run_sync_suite()
{
    new original_text[128] = "test \\ input: ' OR 1=1 -- x";
    new escaped[256];
    new query[512];
    new mariadb_result:result;
    new affected_rows;
    new insert_id;
    new column_name[32];
    new roundtrip[128];
    new read_len;
    new Float:float_value;

    assert_true(mariadb_escape_string(g_db, escaped, charsmax(escaped), original_text) >= 0, "mariadb_escape_string() succeeds on quote/backslash payload");

    formatex(query, charsmax(query), "INSERT INTO `%s` (batch_id, int_value, bool_value, float_value, text_value, nullable_value) VALUES (%d, 42, 1, 3.5, '%s', NULL)", g_table_name, TEST_BATCH_SYNC, escaped);

    assert_true(mariadb_exec(g_db, query, affected_rows, insert_id), "mariadb_exec() inserts a sync row");
    assert_true(affected_rows == 1, "Sync INSERT reports affected_rows == 1");
    assert_true(insert_id > 0, "Sync INSERT reports a positive insert_id");

    formatex(query, charsmax(query), "SELECT int_value, bool_value, float_value, text_value, nullable_value FROM `%s` WHERE batch_id = %d ORDER BY id DESC LIMIT 1", g_table_name, TEST_BATCH_SYNC);

    result = mariadb_query(g_db, query);
    assert_true(mariadb_result_valid(result), "mariadb_query() returns a result handle");
    if (!mariadb_result_valid(result))
    {
        log_db_error("Sync SELECT failed");
        return;
    }

    assert_true(mariadb_row_count(result) == 1, "Sync SELECT row_count == 1");
    assert_true(mariadb_column_count(result) == 5, "Sync SELECT column_count == 5");
    assert_true(mariadb_column_index(result, "text_value") == 3, "mariadb_column_index(text_value) == 3");
    assert_true(mariadb_column_name(result, 3, column_name, charsmax(column_name)) != 0, "mariadb_column_name() succeeds");
    assert_true(equal(column_name, "text_value"), "mariadb_column_name() returns expected name");
    assert_true(mariadb_next_row(result), "mariadb_next_row() advances to the first row");

    assert_true(mariadb_read_int(result, 0) == 42, "mariadb_read_int() returns inserted integer");
    assert_true(mariadb_read_bool(result, 1), "mariadb_read_bool() returns inserted bool");

    float_value = mariadb_read_float(result, 2);
    assert_true(floatabs(float_value - 3.5) < 0.01, "mariadb_read_float() returns inserted float");

    read_len = mariadb_read_string(result, 3, roundtrip, charsmax(roundtrip));
    assert_true(read_len == strlen(original_text), "mariadb_read_string() reports expected length");
    assert_true(equal(roundtrip, original_text), "mariadb_read_string() round-trips quote/backslash text");
    assert_true(mariadb_is_null(result, 4), "mariadb_is_null() reports NULL values correctly");
    assert_true(!mariadb_next_row(result), "mariadb_next_row() stops at the end of the result");
    assert_true(mariadb_rewind(result), "mariadb_rewind() succeeds");
    assert_true(mariadb_next_row(result), "mariadb_rewind() resets the cursor before the first row");

    assert_true(mariadb_result_close(result), "mariadb_result_close() succeeds on a live handle");
    assert_true(!mariadb_result_valid(result), "mariadb_result_close() invalidates the Pawn handle");
    assert_true(mariadb_result_close(result), "mariadb_result_close() is idempotent on an invalid handle");
}

// ============================================================================
// TRANSACTION STAGES
// ============================================================================

stock run_transaction_suite()
{
    new query[384];
    new count;

    assert_true(mariadb_begin(g_db), "mariadb_begin() starts a transaction");

    formatex(query, charsmax(query), "INSERT INTO `%s` (batch_id, int_value, bool_value, float_value, text_value, nullable_value) VALUES (%d, 7, 1, 1.0, 'rollback-row', NULL)", g_table_name, TEST_BATCH_TX_ROLLBACK);
    assert_true(mariadb_exec(g_db, query), "INSERT inside rollback transaction succeeds");
    assert_true(mariadb_rollback(g_db), "mariadb_rollback() succeeds");
    assert_true(fetch_batch_count(TEST_BATCH_TX_ROLLBACK, count), "Can count rows after rollback");
    assert_true(count == 0, "Rollback removes uncommitted rows");

    assert_true(mariadb_begin(g_db), "mariadb_begin() starts a second transaction");

    formatex(query, charsmax(query), "INSERT INTO `%s` (batch_id, int_value, bool_value, float_value, text_value, nullable_value) VALUES (%d, 8, 0, 2.0, 'commit-row', NULL)", g_table_name, TEST_BATCH_TX_COMMIT);
    assert_true(mariadb_exec(g_db, query), "INSERT inside commit transaction succeeds");
    assert_true(mariadb_commit(g_db), "mariadb_commit() succeeds");
    assert_true(fetch_batch_count(TEST_BATCH_TX_COMMIT, count), "Can count rows after commit");
    assert_true(count == 1, "Committed row remains visible");
}

// ============================================================================
// STATEMENT STAGES
// ============================================================================

stock run_statement_suite()
{
    new insert_query[256];
    new select_query[192];
    new text_value[96] = "stmt-insert";
    new nullable_value[64] = "nullable";
    new mariadb_statement:stmt;
    new mariadb_result:result;
    new affected_rows;
    new insert_id;
    new buffer[128];

    formatex(insert_query, charsmax(insert_query), "INSERT INTO `%s` (batch_id, int_value, bool_value, float_value, text_value, nullable_value) VALUES (?, ?, ?, ?, ?, ?)", g_table_name);

    stmt = mariadb_prepare(g_db, insert_query);
    assert_true(mariadb_statement_valid(stmt), "mariadb_prepare() creates an INSERT statement");
    if (!mariadb_statement_valid(stmt))
    {
        log_db_error("Statement prepare failed");
        return;
    }

    assert_true(mariadb_bind_int(stmt, 0, TEST_BATCH_STMT), "mariadb_bind_int() binds parameter 0");
    assert_true(mariadb_bind_int(stmt, 1, 77), "mariadb_bind_int() binds parameter 1");
    assert_true(mariadb_bind_bool(stmt, 2, true), "mariadb_bind_bool() binds parameter 2");
    assert_true(mariadb_bind_float(stmt, 3, 2.25), "mariadb_bind_float() binds parameter 3");
    assert_true(mariadb_bind_string(stmt, 4, text_value), "mariadb_bind_string() binds parameter 4");
    assert_true(mariadb_bind_null(stmt, 5), "mariadb_bind_null() binds parameter 5");
    assert_true(mariadb_stmt_exec(stmt, affected_rows, insert_id), "mariadb_stmt_exec() runs INSERT");
    assert_true(affected_rows == 1, "Prepared INSERT reports affected_rows == 1");
    assert_true(insert_id > 0, "Prepared INSERT reports a positive insert_id");
    assert_true(mariadb_stmt_reset(stmt), "mariadb_stmt_reset() succeeds after exec");
    assert_true(mariadb_bind_int(stmt, 0, TEST_BATCH_STMT_QUERY), "Statement can be rebound after reset");
    assert_true(mariadb_bind_int(stmt, 1, 88), "Rebound int parameter succeeds");
    assert_true(mariadb_bind_bool(stmt, 2, false), "Rebound bool parameter succeeds");
    assert_true(mariadb_bind_float(stmt, 3, 8.5), "Rebound float parameter succeeds");
    assert_true(mariadb_bind_string(stmt, 4, "stmt-query"), "Rebound string parameter succeeds");
    assert_true(mariadb_bind_string(stmt, 5, nullable_value), "Rebound nullable column as string succeeds");
    assert_true(mariadb_stmt_exec(stmt), "mariadb_stmt_exec() works without optional by-ref outputs");
    assert_true(mariadb_stmt_close(stmt), "mariadb_stmt_close() succeeds on INSERT statement");

    formatex(select_query, charsmax(select_query),
        "SELECT int_value, bool_value, float_value, text_value, nullable_value FROM `%s` WHERE batch_id = ?",
        g_table_name);

    stmt = mariadb_prepare(g_db, select_query);
    assert_true(mariadb_statement_valid(stmt), "mariadb_prepare() creates a SELECT statement");
    if (!mariadb_statement_valid(stmt))
    {
        return;
    }

    assert_true(mariadb_bind_int(stmt, 0, TEST_BATCH_STMT_QUERY), "mariadb_bind_int() uses 0-based indices for SELECT");
    result = mariadb_stmt_query(stmt);
    assert_true(mariadb_result_valid(result), "mariadb_stmt_query() returns a result handle");

    if (mariadb_result_valid(result))
    {
        assert_true(mariadb_row_count(result) == 1, "Prepared SELECT row_count == 1");
        assert_true(mariadb_next_row(result), "Prepared SELECT advances to first row");
        assert_true(mariadb_read_int(result, 0) == 88, "Prepared SELECT reads bound integer");
        assert_true(!mariadb_read_bool(result, 1), "Prepared SELECT reads bound bool");
        assert_true(floatabs(mariadb_read_float(result, 2) - 8.5) < 0.01, "Prepared SELECT reads bound float");
        mariadb_read_string(result, 3, buffer, charsmax(buffer));
        assert_true(equal(buffer, "stmt-query"), "Prepared SELECT reads bound string");
        assert_true(!mariadb_is_null(result, 4), "Prepared SELECT keeps non-NULL string values");
        mariadb_read_string(result, 4, buffer, charsmax(buffer));
        assert_true(equal(buffer, nullable_value), "Prepared SELECT reads nullable string column");
        mariadb_result_close(result);
    }

    assert_true(mariadb_stmt_close(stmt), "mariadb_stmt_close() succeeds on SELECT statement");
}

// ============================================================================
// ERROR STAGES
// ============================================================================

stock run_error_suite()
{
    new query[160];
    new error[MARIADB_MAX_ERROR_LENGTH];
    new error_code;
    new mariadb_result:result;
    new mariadb_statement:stmt;

    formatex(query, charsmax(query), "SELECT missing_column FROM `%s`", g_table_name);
    result = mariadb_query(g_db, query);
    assert_true(!mariadb_result_valid(result), "Invalid sync query returns invalid result handle");
    mariadb_get_error(g_db, error, charsmax(error));
    error_code = mariadb_get_error_code(g_db);
    assert_true(error[0] != EOS, "mariadb_get_error() returns a non-empty message after query failure");
    assert_true(error_code != 0, "mariadb_get_error_code() returns a non-zero value after query failure");

    formatex(query, charsmax(query), "INSERT INTO `%s` (batch_id, int_value) VALUES (?, ?)", g_table_name);
    stmt = mariadb_prepare(g_db, query);
    assert_true(mariadb_statement_valid(stmt), "mariadb_prepare() still works after a query error");
    if (!mariadb_statement_valid(stmt))
    {
        return;
    }

    assert_true(!mariadb_bind_int(stmt, 9, 1), "Binding an out-of-range parameter index fails");
    assert_true(mariadb_bind_int(stmt, 0, 999), "Binding the first parameter still succeeds");
    assert_true(!mariadb_stmt_exec(stmt), "Executing with missing bound parameters fails");
    mariadb_stmt_error(stmt, error, charsmax(error));
    error_code = mariadb_stmt_error_code(stmt);
    assert_true(error[0] != EOS, "mariadb_stmt_error() returns a non-empty message after statement failure");
    assert_true(error_code == 0 || error_code > 0, "mariadb_stmt_error_code() is readable after statement failure");
    assert_true(mariadb_stmt_close(stmt), "mariadb_stmt_close() succeeds after an error");
}

// ============================================================================
// INJECTION STAGES
// ============================================================================

stock run_injection_suite()
{
    new payload[128] = "missing' OR 1=1 -- ";
    new escaped[256];
    new query[512];
    new count;
    new mariadb_statement:stmt;
    new mariadb_result:result;
    new roundtrip[128];

    formatex(query, charsmax(query),
        "SELECT COUNT(*) FROM `%s` WHERE text_value = '%s'",
        g_table_name,
        payload);
    assert_true(fetch_single_int(query, count), "Unsafe interpolation control query executes");
    assert_true(count > 0, "Unsafe interpolation widens the predicate and proves raw SQL is injectable");

    assert_true(mariadb_escape_string(g_db, escaped, charsmax(escaped), payload) >= 0, "mariadb_escape_string() escapes the classic OR payload");
    formatex(query, charsmax(query),
        "SELECT COUNT(*) FROM `%s` WHERE text_value = '%s'",
        g_table_name,
        escaped);
    assert_true(fetch_single_int(query, count), "Escaped control query executes");
    assert_true(count == 0, "Escaping prevents the raw SQL injection payload from widening the predicate");

    formatex(query, charsmax(query), "INSERT INTO `%s` (batch_id, int_value, bool_value, float_value, text_value, nullable_value) VALUES (?, ?, ?, ?, ?, ?)", g_table_name);

    stmt = mariadb_prepare(g_db, query);
    assert_true(mariadb_statement_valid(stmt), "Prepared INSERT exists for injection-resistance test");
    if (!mariadb_statement_valid(stmt))
    {
        return;
    }

    assert_true(mariadb_bind_int(stmt, 0, TEST_BATCH_INJECTION_PREP), "Prepared injection test binds batch_id");
    assert_true(mariadb_bind_int(stmt, 1, 123), "Prepared injection test binds int_value");
    assert_true(mariadb_bind_bool(stmt, 2, true), "Prepared injection test binds bool_value");
    assert_true(mariadb_bind_float(stmt, 3, 9.75), "Prepared injection test binds float_value");
    assert_true(mariadb_bind_string(stmt, 4, "literal'); DROP TABLE users; --"), "Prepared injection test binds raw payload as data");
    assert_true(mariadb_bind_null(stmt, 5), "Prepared injection test binds NULL nullable column");
    assert_true(mariadb_stmt_exec(stmt), "Prepared INSERT accepts raw injection payload as literal text");
    assert_true(mariadb_stmt_close(stmt), "Prepared injection INSERT statement closes cleanly");

    formatex(query, charsmax(query),
        "SELECT text_value FROM `%s` WHERE batch_id = %d ORDER BY id DESC LIMIT 1",
        g_table_name,
        TEST_BATCH_INJECTION_PREP);
    result = mariadb_query(g_db, query);
    assert_true(mariadb_result_valid(result), "Prepared injection verification SELECT succeeds");
    if (mariadb_result_valid(result))
    {
        assert_true(mariadb_next_row(result), "Prepared injection verification has a row");
        mariadb_read_string(result, 0, roundtrip, charsmax(roundtrip));
        assert_true(equal(roundtrip, "literal'); DROP TABLE users; --"), "Prepared statements store the payload as literal data");
        mariadb_result_close(result);
    }
}

// ============================================================================
// PERF STAGES
// ============================================================================

stock run_sync_perf_sample()
{
    new loops = bound_pcvar_int(g_pcvar_sync_loops, 1, 1000);
    new insert_query[256];
    new value[48];
    new mariadb_statement:stmt;
    new Float:start_time;
    new Float:elapsed_seconds;
    new elapsed_ms;
    new rate;
    new avg_latency_us;

    formatex(insert_query, charsmax(insert_query), "INSERT INTO `%s` (batch_id, int_value, bool_value, float_value, text_value, nullable_value) VALUES (?, ?, ?, ?, ?, NULL)", g_table_name);

    stmt = mariadb_prepare(g_db, insert_query);
    assert_true(mariadb_statement_valid(stmt), "Prepared statement exists for sync throughput sample");
    if (!mariadb_statement_valid(stmt))
    {
        return;
    }

    start_time = mariadb_monotonic_time();

    for (new i = 0; i < loops; ++i)
    {
        formatex(value, charsmax(value), "sync-perf-%d", i);

        if (!mariadb_bind_int(stmt, 0, TEST_BATCH_SYNC_PERF)
            || !mariadb_bind_int(stmt, 1, i)
            || !mariadb_bind_bool(stmt, 2, (i % 2) == 0)
            || !mariadb_bind_float(stmt, 3, float(i) / 10.0)
            || !mariadb_bind_string(stmt, 4, value)
            || !mariadb_stmt_exec(stmt))
        {
            log_stmt_error(stmt, "Sync throughput sample failed");
            record_failure("Sync throughput sample aborted before completion");
            mariadb_stmt_close(stmt);
            return;
        }
    }

    assert_true(mariadb_stmt_close(stmt), "Sync throughput statement closes cleanly");

    elapsed_seconds = mariadb_monotonic_time() - start_time;
    elapsed_ms = floatround(elapsed_seconds * 1000.0);
    rate = (elapsed_seconds > 0.0) ? floatround(float(loops) / elapsed_seconds) : loops;
    avg_latency_us = floatround(elapsed_seconds * 1000000.0 / float(loops));

    log_suite(
        "Sync throughput sample: %d prepared INSERTs in %d ms (~%d ops/s, avg %d us/op).",
        loops,
        elapsed_ms,
        rate,
        avg_latency_us
    );
    assert_true(true, "Sync throughput sample completed");
}

// ============================================================================
// ASYNC STAGES
// ============================================================================

stock start_async_cancel_suite()
{
    new blocker_data[async_payload_t];
    new target_data[async_payload_t];
    new blocker_query[48];
    new target_query[256];
    new mariadb_job:blocker_job;

    copy(blocker_query, charsmax(blocker_query), "SELECT SLEEP(1)");

    blocker_data[m_kind] = _:async_cancel_blocker;
    blocker_data[m_index] = 0;
    blocker_data[m_batch_id] = 0;

    blocker_job = mariadb_async_query(g_db, "on_mariadb_async", blocker_query, blocker_data, sizeof(blocker_data));
    assert_true(mariadb_job_valid(blocker_job), "Queued async blocker query for cancellation test");

    formatex(target_query, charsmax(target_query), "INSERT INTO `%s` (batch_id, int_value, bool_value, float_value, text_value, nullable_value) VALUES (%d, 1, 1, 1.0, 'cancel-target', NULL)", g_table_name, TEST_BATCH_CANCEL_TARGET);

    target_data[m_kind] = _:async_cancel_target;
    target_data[m_index] = 1;
    target_data[m_batch_id] = TEST_BATCH_CANCEL_TARGET;

    g_cancel_callback_fired = false;
    g_cancelled_job = mariadb_async_exec(g_db, "on_mariadb_async", target_query, target_data, sizeof(target_data));

    assert_true(mariadb_job_valid(g_cancelled_job), "Queued async target job for cancellation");
    assert_true(mariadb_job_cancel(g_cancelled_job), "mariadb_job_cancel() succeeds while the job is still queued");
}

stock start_async_stress_suite()
{
    new jobs = bound_pcvar_int(g_pcvar_async_jobs, 1, 256);
    new query[256];
    new value[48];
    new data[async_payload_t];
    new mariadb_job:job;

    g_async_expected_jobs = jobs;
    g_async_seen_jobs = 0;
    g_async_success_jobs = 0;
    g_async_error_jobs = 0;
    g_async_started_at = mariadb_monotonic_time();
    g_async_total_queue_time = 0.0;
    g_async_min_queue_time = -1.0;
    g_async_max_queue_time = 0.0;

    for (new i = 0; i < jobs; ++i)
    {
        formatex(value, charsmax(value), "async-perf-%d", i);
        formatex(query, charsmax(query), "INSERT INTO `%s` (batch_id, int_value, bool_value, float_value, text_value, nullable_value) VALUES (%d, %d, %d, 1.0, '%s', NULL)", g_table_name, TEST_BATCH_ASYNC_STRESS, i, (i % 2), value);

        data[m_kind] = _:async_stress_exec;
        data[m_index] = i;
        data[m_batch_id] = TEST_BATCH_ASYNC_STRESS;

        job = mariadb_async_exec(g_db, "on_mariadb_async", query, data, sizeof(data));
        if (!mariadb_job_valid(job))
        {
            record_failure("Failed to queue async stress job %d", i);
            finish_suite();
            return;
        }
    }

    log_suite("Queued %d async INSERT jobs for throughput/callback validation.", jobs);
}

// ============================================================================
// ASYNC STMT STAGES
// ============================================================================

stock start_async_stmt_suite()
{
    new jobs = bound_pcvar_int(g_pcvar_async_jobs, 1, 256);
    new insert_query[256];
    new data[async_payload_t];
    new mariadb_async_stmt:stmt;
    new mariadb_job:job;

    g_async_expected_jobs = jobs;
    g_async_seen_jobs = 0;
    g_async_success_jobs = 0;
    g_async_error_jobs = 0;
    g_async_started_at = mariadb_monotonic_time();
    g_async_total_queue_time = 0.0;
    g_async_min_queue_time = -1.0;
    g_async_max_queue_time = 0.0;

    formatex(insert_query, charsmax(insert_query),
        "INSERT INTO `%s` (batch_id, int_value, bool_value, float_value, text_value, nullable_value) VALUES (?, ?, ?, ?, ?, NULL)",
        g_table_name);

    stmt = mariadb_async_stmt_create(insert_query);
    assert_true(mariadb_async_stmt_valid(stmt), "mariadb_async_stmt_create() returns a valid handle");
    if (!mariadb_async_stmt_valid(stmt))
    {
        record_failure("Async stmt suite could not create a statement handle");
        finish_suite();
        return;
    }

    // verify bind helpers return true for valid indices
    assert_true(mariadb_async_stmt_bind_int(stmt, 0, TEST_BATCH_ASYNC_STMT), "mariadb_async_stmt_bind_int() succeeds for index 0");
    assert_true(mariadb_async_stmt_bind_int(stmt, 1, 0), "mariadb_async_stmt_bind_int() succeeds for index 1");
    assert_true(mariadb_async_stmt_bind_bool(stmt, 2, true), "mariadb_async_stmt_bind_bool() succeeds");
    assert_true(mariadb_async_stmt_bind_float(stmt, 3, 1.0), "mariadb_async_stmt_bind_float() succeeds");
    assert_true(mariadb_async_stmt_bind_string(stmt, 4, "async-stmt-test"), "mariadb_async_stmt_bind_string() succeeds");

    // verify out-of-range bind fails
    assert_true(!mariadb_async_stmt_bind_int(stmt, 5, 999), "mariadb_async_stmt_bind_int() rejects an out-of-range index");

    for (new i = 0; i < jobs; ++i)
    {
        // rebind the varying fields; handle is reused across all iterations
        mariadb_async_stmt_bind_int(stmt, 1, i);
        mariadb_async_stmt_bind_bool(stmt, 2, (i % 2) == 0);

        data[m_kind] = _:async_stmt_exec;
        data[m_index] = i;
        data[m_batch_id] = TEST_BATCH_ASYNC_STMT;

        job = mariadb_async_stmt_exec(g_db, "on_mariadb_async", stmt, data, sizeof(data));
        if (!mariadb_job_valid(job))
        {
            record_failure("Failed to queue async stmt job %d", i);
            mariadb_async_stmt_close(stmt);
            finish_suite();
            return;
        }
    }

    assert_true(mariadb_async_stmt_close(stmt), "mariadb_async_stmt_close() succeeds after all jobs are queued");
    assert_true(!mariadb_async_stmt_valid(stmt), "mariadb_async_stmt_close() invalidates the Pawn handle");

    log_suite("Queued %d async stmt INSERT jobs.", jobs);
}

// ============================================================================
// ASSERT / LOG
// ============================================================================

stock bool:fetch_batch_count(batch_id, &count)
{
    new query[160];

    formatex(query, charsmax(query),
        "SELECT COUNT(*) FROM `%s` WHERE batch_id = %d",
        g_table_name,
        batch_id);

    return fetch_single_int(query, count);
}

stock bool:fetch_single_int(const query[], &value)
{
    new mariadb_result:result = mariadb_query(g_db, query);

    if (!mariadb_result_valid(result))
    {
        log_db_error("fetch_single_int failed");
        value = 0;
        return false;
    }

    if (!mariadb_next_row(result))
    {
        record_failure("fetch_single_int received an empty result set");
        mariadb_result_close(result);
        value = 0;
        return false;
    }

    value = mariadb_read_int(result, 0);
    mariadb_result_close(result);
    return true;
}

stock finish_suite()
{
    new summary[192];

    if (mariadb_connection_valid(g_db))
    {
        if (get_pcvar_num(g_pcvar_keep_tables) == 0)
        {
            cleanup_schema();
        }
        else
        {
            log_suite("Keeping test table `%s` after the run.", g_table_name);
        }

        mariadb_disconnect(g_db);
    }

    formatex(summary, charsmax(summary),
        "Suite finished: %d assertions, %d failures.",
        g_total_asserts,
        g_failed_asserts);

    log_suite(summary);
    server_print("[MariaDBTest] %s", summary);

    if (g_failed_asserts == 0)
    {
        log_suite("All checks passed.");
    }

    g_run_active = false;
}

stock cleanup_schema()
{
    new query[160];

    formatex(query, charsmax(query), "DROP TABLE IF EXISTS `%s`", g_table_name);
    if (!mariadb_exec(g_db, query))
    {
        log_db_error("DROP TABLE during cleanup failed");
    }
}

stock bool:assert_true(condition, const fmt[], any:...)
{
    new message[192];

    g_total_asserts++;
    vformat(message, charsmax(message), fmt, 3);

    if (condition)
    {
        if (get_pcvar_num(g_pcvar_verbose) != 0)
        {
            log_suite("PASS: %s", message);
        }
        return true;
    }

    g_failed_asserts++;
    log_suite_important("FAIL: %s", message);
    return false;
}

stock record_failure(const fmt[], any:...)
{
    new message[192];

    g_total_asserts++;
    g_failed_asserts++;
    vformat(message, charsmax(message), fmt, 2);
    log_suite_important("FAIL: %s", message);
}

stock log_suite(const fmt[], any:...)
{
    new message[192];
    vformat(message, charsmax(message), fmt, 2);
    log_amx("[MariaDBTest] %s", message);
}

stock log_suite_important(const fmt[], any:...)
{
    new message[192];
    vformat(message, charsmax(message), fmt, 2);
    log_amx("[MariaDBTest] %s", message);
    server_print("[MariaDBTest] %s", message);
}

stock log_db_error(const context[])
{
    new error[MARIADB_MAX_ERROR_LENGTH];
    new error_code = mariadb_get_error_code(g_db);
    mariadb_get_error(g_db, error, charsmax(error));
    log_suite_important("%s: %s (%d)", context, error, error_code);
}

stock log_stmt_error(mariadb_statement:stmt, const context[])
{
    new error[MARIADB_MAX_ERROR_LENGTH];
    new error_code = mariadb_stmt_error_code(stmt);
    mariadb_stmt_error(stmt, error, charsmax(error));
    log_suite_important("%s: %s (%d)", context, error, error_code);
}
