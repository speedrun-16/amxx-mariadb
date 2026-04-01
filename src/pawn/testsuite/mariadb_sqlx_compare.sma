#pragma semicolon 1
#pragma compress 1

#include <amxmodx>
#include <amxmisc>

#define PLUGIN  "MariaDB vs SQLX Compare"
#define VERSION "1.0"
#define AUTHOR  "PWNED"

#include <mariadb>
#include <sqlx>

// ============================================================================
// CONSTANTS
// ============================================================================

#define DEFAULT_HOST       "localhost"
#define DEFAULT_USER       "root"
#define DEFAULT_PASS       "123456"
#define DEFAULT_DATABASE   "amxx_mariadb_test"
#define DEFAULT_PREFIX     "amxx_mariadb_test"
#define DEFAULT_PORT       "3394"

#define BENCH_ID_MARIADB_SYNC_RAW  1100
#define BENCH_ID_SQLX_SYNC_RAW     1200
#define BENCH_ID_MARIADB_PREPARED  1300
#define BENCH_ID_MARIADB_SYNC_TX   1400
#define BENCH_ID_SQLX_SYNC_TX      1500
#define BENCH_ID_MARIADB_ASYNC     2100
#define BENCH_ID_SQLX_ASYNC        2200
#define BENCH_ID_MARIADB_ASYNC_PREPARED 2300

// ============================================================================
// ENUMS
// ============================================================================

enum compare_task
{
	compare_task_mariadb_sync_raw = 48101,
	compare_task_sqlx_sync_raw,
	compare_task_mariadb_prepared,
	compare_task_mariadb_sync_tx,
	compare_task_sqlx_sync_tx,
	compare_task_mariadb_async,
	compare_task_mariadb_async_prepared,
	compare_task_sqlx_async
};

enum benchmark_slot
{
	slot_mariadb_sync_raw = 0,
	slot_sqlx_sync_raw,
	slot_mariadb_prepared,
	slot_mariadb_sync_tx,
	slot_sqlx_sync_tx,
	slot_mariadb_async,
	slot_sqlx_async,
	slot_mariadb_async_prepared,
	slot_count
};

enum compare_async_kind
{
	compare_async_none = 0,
	compare_async_mariadb,
	compare_async_sqlx,
	compare_async_mariadb_prepared
};

enum _:async_payload_t
{
	m_kind = 0,
	m_index,
	m_bench_id
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
new g_pcvar_timeout_ms;
new g_pcvar_prefix;
new g_pcvar_keep_tables;
new g_pcvar_autorun;
new g_pcvar_sync_loops;
new g_pcvar_async_jobs;
new g_pcvar_verbose;

new mariadb_connection:g_mariadb_db = invalid_mariadb_connection;
new Handle:g_sqlx_tuple = Empty_Handle;
new Handle:g_sqlx_db = Empty_Handle;

new g_table_name[96];

new bool:g_run_active;
new g_failure_count;

new g_result_rows[slot_count];
new g_result_elapsed_ms[slot_count];
new g_result_rate_ops[slot_count];
new g_result_avg_latency_us[slot_count];
new g_result_min_latency_us[slot_count];
new g_result_max_latency_us[slot_count];

new compare_async_kind:g_async_kind = compare_async_none;
new g_async_expected_jobs;
new g_async_seen_jobs;
new g_async_success_jobs;
new g_async_error_jobs;
new Float:g_async_started_at;
new Float:g_async_total_arrival_time;
new Float:g_async_min_arrival_time;
new Float:g_async_max_arrival_time;

// ============================================================================
// LIFECYCLE
// ============================================================================

public plugin_init()
{
	register_plugin(PLUGIN, VERSION, AUTHOR);

	register_concmd("amx_mariadb_sqlx_compare_run", "command_run_compare", ADMIN_RCON, "Runs the MariaDB vs SQLX benchmark plugin.");
	register_concmd("amx_mariadb_sqlx_compare_cleanup", "command_cleanup", ADMIN_RCON, "Drops the MariaDB vs SQLX benchmark table.");

	g_pcvar_host = create_cvar("mariadb_compare_host", DEFAULT_HOST, FCVAR_PROTECTED, "Database host for the comparison benchmark.");
	g_pcvar_user = create_cvar("mariadb_compare_user", DEFAULT_USER, FCVAR_PROTECTED, "Database user for the comparison benchmark.");
	g_pcvar_pass = create_cvar("mariadb_compare_pass", DEFAULT_PASS, FCVAR_PROTECTED, "Database password for the comparison benchmark.");
	g_pcvar_database = create_cvar("mariadb_compare_db", DEFAULT_DATABASE, FCVAR_PROTECTED, "Database name for the comparison benchmark.");
	g_pcvar_port = create_cvar("mariadb_compare_port", DEFAULT_PORT, FCVAR_PROTECTED, "Database port for the comparison benchmark.");
	g_pcvar_charset = create_cvar("mariadb_compare_charset", MARIADB_DEFAULT_CHARSET, FCVAR_PROTECTED, "Connection charset for both drivers.");
	g_pcvar_timeout_ms = create_cvar("mariadb_compare_timeout_ms", "5000", FCVAR_PROTECTED, "Connection timeout in milliseconds.");
	g_pcvar_prefix = create_cvar("mariadb_compare_prefix", DEFAULT_PREFIX, FCVAR_PROTECTED, "Prefix used for generated comparison tables.");
	g_pcvar_keep_tables = create_cvar("mariadb_compare_keep_tables", "1", FCVAR_PROTECTED, "Keep the generated comparison table after the run.");
	g_pcvar_autorun = create_cvar("mariadb_compare_autorun", "0", FCVAR_PROTECTED, "Automatically run the comparison in plugin_cfg.");
	g_pcvar_sync_loops = create_cvar("mariadb_compare_sync_loops", "200", FCVAR_PROTECTED, "Number of sync INSERTs per driver.");
	g_pcvar_async_jobs = create_cvar("mariadb_compare_async_jobs", "48", FCVAR_PROTECTED, "Number of async INSERTs per driver.");
	g_pcvar_verbose = create_cvar("mariadb_compare_verbose", "0", FCVAR_PROTECTED, "Emit extra progress logs during each benchmark stage.");
}

public plugin_cfg()
{
	if (get_pcvar_num(g_pcvar_autorun) != 0)
	{
		set_task(2.0, "task_autorun");
	}
}

// ============================================================================
// TASK HANDLERS
// ============================================================================

public task_autorun()
{
	start_compare_run();
}

public task_run_mariadb_sync_raw()
{
	if (!can_run_stage())
	{
		return;
	}

	if (!run_mariadb_sync_raw_benchmark())
	{
		finish_run();
		return;
	}

	queue_stage_task(compare_task_sqlx_sync_raw, "task_run_sqlx_sync_raw");
}

public task_run_sqlx_sync_raw()
{
	if (!can_run_stage())
	{
		return;
	}

	if (!run_sqlx_sync_raw_benchmark())
	{
		finish_run();
		return;
	}

	queue_stage_task(compare_task_mariadb_prepared, "task_run_mariadb_prepared");
}

public task_run_mariadb_prepared()
{
	if (!can_run_stage())
	{
		return;
	}

	if (!run_mariadb_prepared_benchmark())
	{
		finish_run();
		return;
	}

	queue_stage_task(compare_task_mariadb_sync_tx, "task_run_mariadb_sync_tx");
}

public task_run_mariadb_sync_tx()
{
	if (!can_run_stage())
	{
		return;
	}

	if (!run_mariadb_sync_tx_benchmark())
	{
		finish_run();
		return;
	}

	queue_stage_task(compare_task_sqlx_sync_tx, "task_run_sqlx_sync_tx");
}

public task_run_sqlx_sync_tx()
{
	if (!can_run_stage())
	{
		return;
	}

	if (!run_sqlx_sync_tx_benchmark())
	{
		finish_run();
		return;
	}

	queue_stage_task(compare_task_mariadb_async, "task_run_mariadb_async");
}

public task_run_mariadb_async()
{
	if (!can_run_stage())
	{
		return;
	}

	start_mariadb_async_benchmark();
}

public task_run_sqlx_async()
{
	if (!can_run_stage())
	{
		return;
	}

	start_sqlx_async_benchmark();
}

public task_run_mariadb_async_prepared()
{
	if (!can_run_stage())
	{
		return;
	}

	start_mariadb_async_prepared_benchmark();
}

// ============================================================================
// COMMAND HANDLERS
// ============================================================================

public command_run_compare(id, level, cid)
{
	if (!cmd_access(id, level, cid, 1))
	{
		return PLUGIN_HANDLED;
	}

	start_compare_run();
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
		console_print(id, "[MariaDBCompare] Cannot cleanup while a benchmark run is active.");
		return PLUGIN_HANDLED;
	}

	build_table_name();
	db = connect_mariadb();
	if (!mariadb_connection_valid(db))
	{
		console_print(id, "[MariaDBCompare] Cleanup connection failed. Check mariadb_compare_* cvars.");
		return PLUGIN_HANDLED;
	}

	formatex(query, charsmax(query), "DROP TABLE IF EXISTS `%s`", g_table_name);
	if (mariadb_exec(db, query))
	{
		console_print(id, "[MariaDBCompare] Dropped comparison table `%s`.", g_table_name);
	}
	else
	{
		console_print(id, "[MariaDBCompare] Cleanup failed. Check server logs.");
	}

	mariadb_disconnect(db);
	return PLUGIN_HANDLED;
}

// ============================================================================
// ASYNC CALLBACKS
// ============================================================================

public on_mariadb_async(async_state_cell, result, affected_rows, insert_id, error[], error_code, data[], data_size, Float:queue_time)
{
	new mariadb_async_state:async_state;
	new Float:arrival_time;

	async_state = mariadb_async_state:async_state_cell;

	if (!g_run_active || (g_async_kind != compare_async_mariadb && g_async_kind != compare_async_mariadb_prepared))
	{
		return;
	}

	if (data_size != async_payload_t)
	{
		record_failure("MariaDB async callback returned unexpected data size %d", data_size);
		finish_run();
		return;
	}

	arrival_time = mariadb_monotonic_time() - g_async_started_at;
	accumulate_async_arrival(arrival_time);
	g_async_seen_jobs++;

	if (async_state == mariadb_async_ok && affected_rows == 1 && insert_id > 0)
	{
		g_async_success_jobs++;
	}
	else
	{
		g_async_error_jobs++;
		record_failure("MariaDB async job %d failed (state=%d, err=%d, msg=%s, q=%.6f)", data[m_index], async_state_cell, error_code, error, queue_time);
	}

	if (g_async_seen_jobs >= g_async_expected_jobs)
	{
		if (g_async_kind == compare_async_mariadb)
		{
			finalize_async_stage(slot_mariadb_async, BENCH_ID_MARIADB_ASYNC, "MariaDB async");
			if (!g_run_active)
			{
				return;
			}

			queue_stage_task(compare_task_mariadb_async_prepared, "task_run_mariadb_async_prepared");
		}
		else
		{
			finalize_async_stage(slot_mariadb_async_prepared, BENCH_ID_MARIADB_ASYNC_PREPARED, "MariaDB async prepared");
			if (!g_run_active)
			{
				return;
			}

			queue_stage_task(compare_task_sqlx_async, "task_run_sqlx_async");
		}
	}
}

public on_sqlx_async(fail_state, Handle:query, error[], errnum, data[], data_size, Float:queue_time)
{
	new Float:arrival_time;

	if (!g_run_active || g_async_kind != compare_async_sqlx)
	{
		return;
	}

	if (data_size != async_payload_t)
	{
		record_failure("SQLX async callback returned unexpected data size %d", data_size);
		finish_run();
		return;
	}

	arrival_time = mariadb_monotonic_time() - g_async_started_at;
	accumulate_async_arrival(arrival_time);
	g_async_seen_jobs++;

	if (fail_state == TQUERY_SUCCESS && SQL_AffectedRows(query) == 1)
	{
		g_async_success_jobs++;
	}
	else
	{
		g_async_error_jobs++;
		record_failure("SQLX async job %d failed (state=%d, err=%d, msg=%s, q=%.6f)", data[m_index], fail_state, errnum, error, queue_time);
	}

	if (g_async_seen_jobs >= g_async_expected_jobs)
	{
		finalize_async_stage(slot_sqlx_async, BENCH_ID_SQLX_ASYNC, "SQLX async");
		if (!g_run_active)
		{
			return;
		}

		finish_run();
	}
}

// ============================================================================
// RUN CONTROL
// ============================================================================

stock start_compare_run()
{
	new database[64];
	new affinity[32];

	if (g_run_active)
	{
		log_compare("A run is already active; ignoring duplicate start request.");
		return;
	}

	reset_run_state();
	build_table_name();
	get_pcvar_string(g_pcvar_database, database, charsmax(database));

	log_compare("Starting MariaDB vs SQLX benchmark against database `%s`, table `%s`.", database, g_table_name);

	if (!SQL_SetAffinity("mysql"))
	{
		record_failure("SQL_SetAffinity(mysql) failed; sqlx/mysql is unavailable for this plugin");
		finish_run();
		return;
	}

	SQL_GetAffinity(affinity, charsmax(affinity));
	log_compare("SQLX affinity for this plugin is `%s`.", affinity);

	g_mariadb_db = connect_mariadb();
	if (!mariadb_connection_valid(g_mariadb_db))
	{
		record_failure("mariadb_connect failed; aborting comparison");
		finish_run();
		return;
	}

	if (!connect_sqlx())
	{
		record_failure("SQLX connection failed; aborting comparison");
		finish_run();
		return;
	}

	if (!prepare_schema())
	{
		finish_run();
		return;
	}

	queue_stage_task(compare_task_mariadb_sync_raw, "task_run_mariadb_sync_raw");
}

stock reset_run_state()
{
	g_run_active = true;
	g_failure_count = 0;
	g_async_kind = compare_async_none;
	g_async_expected_jobs = 0;
	g_async_seen_jobs = 0;
	g_async_success_jobs = 0;
	g_async_error_jobs = 0;
	g_async_started_at = 0.0;
	g_async_total_arrival_time = 0.0;
	g_async_min_arrival_time = -1.0;
	g_async_max_arrival_time = 0.0;
	g_table_name[0] = EOS;

	arrayset(g_result_rows, 0, sizeof(g_result_rows));
	arrayset(g_result_elapsed_ms, 0, sizeof(g_result_elapsed_ms));
	arrayset(g_result_rate_ops, 0, sizeof(g_result_rate_ops));
	arrayset(g_result_avg_latency_us, 0, sizeof(g_result_avg_latency_us));
	arrayset(g_result_min_latency_us, 0, sizeof(g_result_min_latency_us));
	arrayset(g_result_max_latency_us, 0, sizeof(g_result_max_latency_us));

	close_handles();
}

stock finish_run()
{
	new summary[160];

	if (!g_run_active)
	{
		return;
	}

	if (mariadb_connection_valid(g_mariadb_db) && get_pcvar_num(g_pcvar_keep_tables) == 0)
	{
		cleanup_schema();
	}
	else if (mariadb_connection_valid(g_mariadb_db))
	{
		log_compare("Keeping comparison table `%s` after the run.", g_table_name);
	}

	log_comparison_summary();

	formatex(summary, charsmax(summary), "Comparison finished with %d failure(s).", g_failure_count);
	log_compare(summary);

	g_run_active = false;
	close_handles();
}

// ============================================================================
// CONNECTION
// ============================================================================

stock mariadb_connection:connect_mariadb()
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
		bound_pcvar_int(g_pcvar_timeout_ms, 100, 60000),
		true,
		error,
		charsmax(error),
		error_code
	);

	if (!mariadb_connection_valid(db))
	{
		log_compare(
			"mariadb_connect(%s@%s:%d/%s, charset=%s, timeout=%dms) failed: %s (%d)",
			user,
			host,
			bound_pcvar_int(g_pcvar_port, 1, 65535),
			database,
			charset,
			bound_pcvar_int(g_pcvar_timeout_ms, 100, 60000),
			error,
			error_code
		);
	}

	return db;
}

stock bool:connect_sqlx()
{
	new host[64];
	new host_with_port[96];
	new user[64];
	new pass[64];
	new database[64];
	new charset[32];
	new error[MARIADB_MAX_ERROR_LENGTH];
	new error_code;
	new timeout_seconds;

	get_pcvar_string(g_pcvar_host, host, charsmax(host));
	get_pcvar_string(g_pcvar_user, user, charsmax(user));
	get_pcvar_string(g_pcvar_pass, pass, charsmax(pass));
	get_pcvar_string(g_pcvar_database, database, charsmax(database));
	get_pcvar_string(g_pcvar_charset, charset, charsmax(charset));

	build_sqlx_host(host, bound_pcvar_int(g_pcvar_port, 1, 65535), host_with_port, charsmax(host_with_port));
	timeout_seconds = (bound_pcvar_int(g_pcvar_timeout_ms, 100, 60000) + 999) / 1000;

	g_sqlx_tuple = SQL_MakeDbTuple(host_with_port, user, pass, database, timeout_seconds);
	if (g_sqlx_tuple == Empty_Handle)
	{
		log_compare("SQL_MakeDbTuple(%s@%s/%s) failed.", user, host_with_port, database);
		return false;
	}

	if (!SQL_SetCharset(g_sqlx_tuple, charset))
	{
		log_compare("SQL_SetCharset(tuple, %s) failed before SQLX connect.", charset);
	}

	g_sqlx_db = SQL_Connect(g_sqlx_tuple, error_code, error, charsmax(error));
	if (g_sqlx_db == Empty_Handle)
	{
		log_compare("SQL_Connect(%s@%s/%s, timeout=%ds) failed: %s (%d)", user, host_with_port, database, timeout_seconds, error, error_code);
		return false;
	}

	if (!SQL_SetCharset(g_sqlx_db, charset))
	{
		log_compare("SQL_SetCharset(connection, %s) failed after SQLX connect.", charset);
	}

	return true;
}

stock close_handles()
{
	if (g_sqlx_db != Empty_Handle)
	{
		SQL_FreeHandle(g_sqlx_db);
		g_sqlx_db = Empty_Handle;
	}

	if (g_sqlx_tuple != Empty_Handle)
	{
		SQL_FreeHandle(g_sqlx_tuple);
		g_sqlx_tuple = Empty_Handle;
	}

	if (mariadb_connection_valid(g_mariadb_db))
	{
		mariadb_disconnect(g_mariadb_db);
	}
}

// ============================================================================
// SCHEMA
// ============================================================================

stock bool:prepare_schema()
{
	new query[512];

	formatex(query, charsmax(query), "DROP TABLE IF EXISTS `%s`", g_table_name);
	if (!mariadb_exec(g_mariadb_db, query))
	{
		log_db_error("DROP TABLE failed");
		record_failure("Schema cleanup failed");
		return false;
	}

	formatex(query, charsmax(query), "CREATE TABLE `%s` (id INT NOT NULL AUTO_INCREMENT, bench_id INT NOT NULL, item_index INT NOT NULL, bool_value TINYINT(1) NOT NULL, float_value FLOAT NOT NULL, text_value VARCHAR(64) NOT NULL, created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY (id), KEY idx_bench_id (bench_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4", g_table_name);
	if (!mariadb_exec(g_mariadb_db, query))
	{
		log_db_error("CREATE TABLE failed");
		record_failure("Schema creation failed");
		return false;
	}

	log_compare("Prepared shared benchmark table `%s`.", g_table_name);
	return true;
}

stock cleanup_schema()
{
	new query[160];

	formatex(query, charsmax(query), "DROP TABLE IF EXISTS `%s`", g_table_name);
	if (!mariadb_exec(g_mariadb_db, query))
	{
		log_db_error("Cleanup DROP TABLE failed");
	}
}

// ============================================================================
// BENCHMARKS
// ============================================================================

stock bool:run_mariadb_sync_raw_benchmark()
{
	new loops = bound_pcvar_int(g_pcvar_sync_loops, 1, 4000);
	new query[256];
	new count;
	new Float:start_time;
	new Float:elapsed_seconds;

	log_verbose("Running MariaDB sync raw benchmark with %d INSERTs.", loops);

	start_time = mariadb_monotonic_time();

	for (new i = 0; i < loops; ++i)
	{
		formatex(query, charsmax(query), "INSERT INTO `%s` (bench_id, item_index, bool_value, float_value, text_value) VALUES (%d, %d, %d, 1.0, 'maria-sync-raw')", g_table_name, BENCH_ID_MARIADB_SYNC_RAW, i, i % 2);

		if (!mariadb_exec(g_mariadb_db, query))
		{
			log_db_error("MariaDB sync raw benchmark failed");
			record_failure("MariaDB sync raw benchmark aborted before completion");
			return false;
		}
	}

	elapsed_seconds = mariadb_monotonic_time() - start_time;

	if (!fetch_batch_count(BENCH_ID_MARIADB_SYNC_RAW, count))
	{
		record_failure("MariaDB sync raw verification query failed");
		return false;
	}

	if (count != loops)
	{
		record_failure("MariaDB sync raw inserted %d/%d rows", count, loops);
		return false;
	}

	store_sync_result(slot_mariadb_sync_raw, loops, elapsed_seconds);
	log_sync_benchmark("MariaDB sync raw", slot_mariadb_sync_raw);
	return true;
}

stock bool:run_sqlx_sync_raw_benchmark()
{
	new loops = bound_pcvar_int(g_pcvar_sync_loops, 1, 4000);
	new Handle:query;
	new query_text[256];
	new error[MARIADB_MAX_ERROR_LENGTH];
	new count;
	new Float:start_time;
	new Float:elapsed_seconds;

	log_verbose("Running SQLX sync raw benchmark with %d INSERTs.", loops);

	start_time = mariadb_monotonic_time();

	for (new i = 0; i < loops; ++i)
	{
		formatex(query_text, charsmax(query_text), "INSERT INTO `%s` (bench_id, item_index, bool_value, float_value, text_value) VALUES (%d, %d, %d, 1.0, 'sqlx-sync-raw')", g_table_name, BENCH_ID_SQLX_SYNC_RAW, i, i % 2);

		query = SQL_PrepareQuery(g_sqlx_db, "%s", query_text);
		if (query == Empty_Handle)
		{
			record_failure("SQLX sync raw could not prepare iteration %d", i);
			return false;
		}

		if (!SQL_Execute(query))
		{
			SQL_QueryError(query, error, charsmax(error));
			SQL_FreeHandle(query);
			log_compare_important("SQLX sync raw benchmark failed: %s", error);
			record_failure("SQLX sync raw benchmark aborted before completion");
			return false;
		}

		SQL_FreeHandle(query);
	}

	elapsed_seconds = mariadb_monotonic_time() - start_time;

	if (!fetch_batch_count(BENCH_ID_SQLX_SYNC_RAW, count))
	{
		record_failure("SQLX sync raw verification query failed");
		return false;
	}

	if (count != loops)
	{
		record_failure("SQLX sync raw inserted %d/%d rows", count, loops);
		return false;
	}

	store_sync_result(slot_sqlx_sync_raw, loops, elapsed_seconds);
	log_sync_benchmark("SQLX sync raw", slot_sqlx_sync_raw);
	return true;
}

stock bool:run_mariadb_prepared_benchmark()
{
	new loops = bound_pcvar_int(g_pcvar_sync_loops, 1, 4000);
	new insert_query[256];
	new mariadb_statement:stmt;
	new count;
	new Float:start_time;
	new Float:elapsed_seconds;

	log_verbose("Running MariaDB sync prepared benchmark with %d INSERTs.", loops);

	formatex(insert_query, charsmax(insert_query), "INSERT INTO `%s` (bench_id, item_index, bool_value, float_value, text_value) VALUES (?, ?, ?, ?, ?)", g_table_name);

	stmt = mariadb_prepare(g_mariadb_db, insert_query);
	if (!mariadb_statement_valid(stmt))
	{
		log_db_error("MariaDB prepared benchmark could not prepare statement");
		record_failure("MariaDB prepared benchmark could not create a statement");
		return false;
	}

	start_time = mariadb_monotonic_time();

	for (new i = 0; i < loops; ++i)
	{
		if (!mariadb_bind_int(stmt, 0, BENCH_ID_MARIADB_PREPARED)
			|| !mariadb_bind_int(stmt, 1, i)
			|| !mariadb_bind_bool(stmt, 2, (i % 2) == 0)
			|| !mariadb_bind_float(stmt, 3, 1.0)
			|| !mariadb_bind_string(stmt, 4, "maria-prepared")
			|| !mariadb_stmt_exec(stmt))
		{
			log_stmt_error(stmt, "MariaDB prepared benchmark failed");
			record_failure("MariaDB prepared benchmark aborted before completion");
			mariadb_stmt_close(stmt);
			return false;
		}
	}

	elapsed_seconds = mariadb_monotonic_time() - start_time;

	if (!mariadb_stmt_close(stmt))
	{
		record_failure("MariaDB prepared benchmark statement did not close cleanly");
		return false;
	}

	if (!fetch_batch_count(BENCH_ID_MARIADB_PREPARED, count))
	{
		record_failure("MariaDB prepared verification query failed");
		return false;
	}

	if (count != loops)
	{
		record_failure("MariaDB prepared inserted %d/%d rows", count, loops);
		return false;
	}

	store_sync_result(slot_mariadb_prepared, loops, elapsed_seconds);
	log_sync_benchmark("MariaDB sync prepared", slot_mariadb_prepared);
	return true;
}

stock bool:run_mariadb_sync_tx_benchmark()
{
	new loops = bound_pcvar_int(g_pcvar_sync_loops, 1, 4000);
	new query[256];
	new count;
	new Float:start_time;
	new Float:elapsed_seconds;

	log_verbose("Running MariaDB sync transaction benchmark with %d INSERTs.", loops);

	if (!mariadb_begin(g_mariadb_db))
	{
		log_db_error("MariaDB sync transaction benchmark could not begin");
		record_failure("MariaDB sync transaction benchmark could not begin");
		return false;
	}

	start_time = mariadb_monotonic_time();

	for (new i = 0; i < loops; ++i)
	{
		formatex(query, charsmax(query), "INSERT INTO `%s` (bench_id, item_index, bool_value, float_value, text_value) VALUES (%d, %d, %d, 1.0, 'maria-sync-tx')", g_table_name, BENCH_ID_MARIADB_SYNC_TX, i, i % 2);

		if (!mariadb_exec(g_mariadb_db, query))
		{
			log_db_error("MariaDB sync transaction benchmark failed");
			mariadb_rollback(g_mariadb_db);
			record_failure("MariaDB sync transaction benchmark aborted before completion");
			return false;
		}
	}

	if (!mariadb_commit(g_mariadb_db))
	{
		log_db_error("MariaDB sync transaction benchmark commit failed");
		record_failure("MariaDB sync transaction benchmark commit failed");
		return false;
	}

	elapsed_seconds = mariadb_monotonic_time() - start_time;

	if (!fetch_batch_count(BENCH_ID_MARIADB_SYNC_TX, count))
	{
		record_failure("MariaDB sync transaction verification query failed");
		return false;
	}

	if (count != loops)
	{
		record_failure("MariaDB sync transaction inserted %d/%d rows", count, loops);
		return false;
	}

	store_sync_result(slot_mariadb_sync_tx, loops, elapsed_seconds);
	log_sync_benchmark("MariaDB sync transaction", slot_mariadb_sync_tx);
	return true;
}

stock bool:run_sqlx_sync_tx_benchmark()
{
	new loops = bound_pcvar_int(g_pcvar_sync_loops, 1, 4000);
	new Handle:query;
	new query_text[256];
	new error[MARIADB_MAX_ERROR_LENGTH];
	new count;
	new Float:start_time;
	new Float:elapsed_seconds;

	log_verbose("Running SQLX sync transaction benchmark with %d INSERTs.", loops);

	if (!SQL_SimpleQuery(g_sqlx_db, "START TRANSACTION", error, charsmax(error)))
	{
		log_compare_important("SQLX sync transaction benchmark could not begin: %s", error);
		record_failure("SQLX sync transaction benchmark could not begin");
		return false;
	}

	start_time = mariadb_monotonic_time();

	for (new i = 0; i < loops; ++i)
	{
		formatex(query_text, charsmax(query_text), "INSERT INTO `%s` (bench_id, item_index, bool_value, float_value, text_value) VALUES (%d, %d, %d, 1.0, 'sqlx-sync-tx')", g_table_name, BENCH_ID_SQLX_SYNC_TX, i, i % 2);

		query = SQL_PrepareQuery(g_sqlx_db, "%s", query_text);
		if (query == Empty_Handle)
		{
			SQL_SimpleQuery(g_sqlx_db, "ROLLBACK");
			record_failure("SQLX sync transaction could not prepare iteration %d", i);
			return false;
		}

		if (!SQL_Execute(query))
		{
			SQL_QueryError(query, error, charsmax(error));
			SQL_FreeHandle(query);
			SQL_SimpleQuery(g_sqlx_db, "ROLLBACK");
			log_compare_important("SQLX sync transaction benchmark failed: %s", error);
			record_failure("SQLX sync transaction benchmark aborted before completion");
			return false;
		}

		SQL_FreeHandle(query);
	}

	if (!SQL_SimpleQuery(g_sqlx_db, "COMMIT", error, charsmax(error)))
	{
		log_compare_important("SQLX sync transaction benchmark commit failed: %s", error);
		record_failure("SQLX sync transaction benchmark commit failed");
		return false;
	}

	elapsed_seconds = mariadb_monotonic_time() - start_time;

	if (!fetch_batch_count(BENCH_ID_SQLX_SYNC_TX, count))
	{
		record_failure("SQLX sync transaction verification query failed");
		return false;
	}

	if (count != loops)
	{
		record_failure("SQLX sync transaction inserted %d/%d rows", count, loops);
		return false;
	}

	store_sync_result(slot_sqlx_sync_tx, loops, elapsed_seconds);
	log_sync_benchmark("SQLX sync transaction", slot_sqlx_sync_tx);
	return true;
}

stock start_mariadb_async_benchmark()
{
	new jobs = bound_pcvar_int(g_pcvar_async_jobs, 1, 512);
	new query[256];
	new data[async_payload_t];
	new mariadb_job:job;

	reset_async_stage(compare_async_mariadb, jobs);
	log_verbose("Queueing %d MariaDB async INSERT jobs.", jobs);

	for (new i = 0; i < jobs; ++i)
	{
		formatex(query, charsmax(query), "INSERT INTO `%s` (bench_id, item_index, bool_value, float_value, text_value) VALUES (%d, %d, %d, 1.0, 'maria-async')", g_table_name, BENCH_ID_MARIADB_ASYNC, i, i % 2);

		data[m_kind] = _:compare_async_mariadb;
		data[m_index] = i;
		data[m_bench_id] = BENCH_ID_MARIADB_ASYNC;

		job = mariadb_async_exec(g_mariadb_db, "on_mariadb_async", query, data, sizeof(data));
		if (!mariadb_job_valid(job))
		{
			record_failure("Failed to queue MariaDB async job %d", i);
			finish_run();
			return;
		}
	}

	log_compare("Queued %d MariaDB async INSERT jobs.", jobs);
}

stock start_sqlx_async_benchmark()
{
	new jobs = bound_pcvar_int(g_pcvar_async_jobs, 1, 512);
	new query[256];
	new data[async_payload_t];

	reset_async_stage(compare_async_sqlx, jobs);
	log_verbose("Queueing %d SQLX async INSERT jobs.", jobs);

	for (new i = 0; i < jobs; ++i)
	{
		formatex(query, charsmax(query), "INSERT INTO `%s` (bench_id, item_index, bool_value, float_value, text_value) VALUES (%d, %d, %d, 1.0, 'sqlx-async')", g_table_name, BENCH_ID_SQLX_ASYNC, i, i % 2);

		data[m_kind] = _:compare_async_sqlx;
		data[m_index] = i;
		data[m_bench_id] = BENCH_ID_SQLX_ASYNC;

		SQL_ThreadQuery(g_sqlx_tuple, "on_sqlx_async", query, data, sizeof(data));
	}

	log_compare("Queued %d SQLX async INSERT jobs.", jobs);
}

stock start_mariadb_async_prepared_benchmark()
{
	new jobs = bound_pcvar_int(g_pcvar_async_jobs, 1, 512);
	new insert_query[256];
	new data[async_payload_t];
	new mariadb_async_stmt:stmt;
	new mariadb_job:job;

	reset_async_stage(compare_async_mariadb_prepared, jobs);
	log_verbose("Queueing %d MariaDB async prepared INSERT jobs.", jobs);

	formatex(insert_query, charsmax(insert_query),
		"INSERT INTO `%s` (bench_id, item_index, bool_value, float_value, text_value) VALUES (?, ?, ?, ?, ?)",
		g_table_name);

	stmt = mariadb_async_stmt_create(insert_query);
	if (!mariadb_async_stmt_valid(stmt))
	{
		record_failure("Failed to create async stmt handle for prepared benchmark");
		finish_run();
		return;
	}

	mariadb_async_stmt_bind_string(stmt, 4, "maria-async-prepared");

	for (new i = 0; i < jobs; ++i)
	{
		mariadb_async_stmt_bind_int(stmt, 0, BENCH_ID_MARIADB_ASYNC_PREPARED);
		mariadb_async_stmt_bind_int(stmt, 1, i);
		mariadb_async_stmt_bind_bool(stmt, 2, (i % 2) == 0);
		mariadb_async_stmt_bind_float(stmt, 3, 1.0);

		data[m_kind] = _:compare_async_mariadb_prepared;
		data[m_index] = i;
		data[m_bench_id] = BENCH_ID_MARIADB_ASYNC_PREPARED;

		job = mariadb_async_stmt_exec(g_mariadb_db, "on_mariadb_async", stmt, data, sizeof(data));
		if (!mariadb_job_valid(job))
		{
			record_failure("Failed to queue MariaDB async prepared job %d", i);
			mariadb_async_stmt_close(stmt);
			finish_run();
			return;
		}
	}

	mariadb_async_stmt_close(stmt);
	log_compare("Queued %d MariaDB async prepared INSERT jobs.", jobs);
}

stock reset_async_stage(compare_async_kind:kind, jobs)
{
	g_async_kind = kind;
	g_async_expected_jobs = jobs;
	g_async_seen_jobs = 0;
	g_async_success_jobs = 0;
	g_async_error_jobs = 0;
	g_async_started_at = mariadb_monotonic_time();
	g_async_total_arrival_time = 0.0;
	g_async_min_arrival_time = -1.0;
	g_async_max_arrival_time = 0.0;
}

stock finalize_async_stage(benchmark_slot:slot, bench_id, const label[])
{
	new count;
	new Float:elapsed_seconds;
	new Float:avg_arrival_time;

	elapsed_seconds = mariadb_monotonic_time() - g_async_started_at;
	avg_arrival_time = (g_async_seen_jobs > 0)
		? (g_async_total_arrival_time / float(g_async_seen_jobs))
		: 0.0;

	store_async_result(slot, g_async_success_jobs, elapsed_seconds, avg_arrival_time, g_async_min_arrival_time, g_async_max_arrival_time);
	log_async_benchmark(label, slot, g_async_success_jobs, g_async_expected_jobs, g_async_error_jobs);

	if (g_async_error_jobs != 0)
	{
		record_failure("%s completed with %d callback errors", label, g_async_error_jobs);
		finish_run();
		return;
	}

	if (!fetch_batch_count(bench_id, count))
	{
		record_failure("%s verification query failed", label);
		finish_run();
		return;
	}

	if (count != g_async_expected_jobs)
	{
		record_failure("%s inserted %d/%d rows", label, count, g_async_expected_jobs);
		finish_run();
	}
}

// ============================================================================
// RESULTS
// ============================================================================

stock store_sync_result(benchmark_slot:slot, rows, Float:elapsed_seconds)
{
	g_result_rows[slot] = rows;
	g_result_elapsed_ms[slot] = floatround(elapsed_seconds * 1000.0);
	g_result_rate_ops[slot] = (elapsed_seconds > 0.0) ? floatround(float(rows) / elapsed_seconds) : rows;
	g_result_avg_latency_us[slot] = (rows > 0) ? floatround(elapsed_seconds * 1000000.0 / float(rows)) : 0;
	g_result_min_latency_us[slot] = 0;
	g_result_max_latency_us[slot] = 0;
}

stock store_async_result(benchmark_slot:slot, rows, Float:elapsed_seconds, Float:avg_arrival_seconds, Float:min_arrival_seconds, Float:max_arrival_seconds)
{
	g_result_rows[slot] = rows;
	g_result_elapsed_ms[slot] = floatround(elapsed_seconds * 1000.0);
	g_result_rate_ops[slot] = (elapsed_seconds > 0.0) ? floatround(float(rows) / elapsed_seconds) : rows;
	g_result_avg_latency_us[slot] = floatround(avg_arrival_seconds * 1000000.0);
	g_result_min_latency_us[slot] = (min_arrival_seconds >= 0.0) ? floatround(min_arrival_seconds * 1000000.0) : 0;
	g_result_max_latency_us[slot] = floatround(max_arrival_seconds * 1000000.0);
}

stock bool:fetch_batch_count(batch_id, &count)
{
	new query[160];
	new mariadb_result:result;

	formatex(query, charsmax(query), "SELECT COUNT(*) FROM `%s` WHERE bench_id = %d", g_table_name, batch_id);
	result = mariadb_query(g_mariadb_db, query);

	if (!mariadb_result_valid(result))
	{
		log_db_error("COUNT(*) verification query failed");
		return false;
	}

	if (!mariadb_next_row(result))
	{
		mariadb_result_close(result);
		log_compare_important("COUNT(*) verification query returned no rows.");
		return false;
	}

	count = mariadb_read_int(result, 0);
	mariadb_result_close(result);
	return true;
}

stock accumulate_async_arrival(Float:arrival_time)
{
	g_async_total_arrival_time += arrival_time;

	if (g_async_min_arrival_time < 0.0 || arrival_time < g_async_min_arrival_time)
	{
		g_async_min_arrival_time = arrival_time;
	}

	if (arrival_time > g_async_max_arrival_time)
	{
		g_async_max_arrival_time = arrival_time;
	}
}

// ============================================================================
// SUMMARY
// ============================================================================

stock log_comparison_summary()
{
	log_compare("Summary:");
	log_sync_benchmark("MariaDB sync raw", slot_mariadb_sync_raw);
	log_sync_benchmark("SQLX sync raw", slot_sqlx_sync_raw);
	log_sync_benchmark("MariaDB sync prepared", slot_mariadb_prepared);
	log_sync_benchmark("MariaDB sync transaction", slot_mariadb_sync_tx);
	log_sync_benchmark("SQLX sync transaction", slot_sqlx_sync_tx);
	log_async_benchmark("MariaDB async", slot_mariadb_async, g_result_rows[slot_mariadb_async], bound_pcvar_int(g_pcvar_async_jobs, 1, 512), 0);
	log_async_benchmark("MariaDB async prepared", slot_mariadb_async_prepared, g_result_rows[slot_mariadb_async_prepared], bound_pcvar_int(g_pcvar_async_jobs, 1, 512), 0);
	log_async_benchmark("SQLX async", slot_sqlx_async, g_result_rows[slot_sqlx_async], bound_pcvar_int(g_pcvar_async_jobs, 1, 512), 0);

	log_winner_by_elapsed("Sync raw winner", "MariaDB", slot_mariadb_sync_raw, "SQLX", slot_sqlx_sync_raw);
	log_winner_by_elapsed("Sync transaction winner", "MariaDB", slot_mariadb_sync_tx, "SQLX", slot_sqlx_sync_tx);
	log_winner_by_elapsed("Async winner", "MariaDB", slot_mariadb_async, "SQLX", slot_sqlx_async);
	log_winner_by_elapsed("MariaDB async prepared vs text", "Prepared", slot_mariadb_async_prepared, "Text", slot_mariadb_async);
	log_winner_by_elapsed("MariaDB prepared vs raw", "Prepared", slot_mariadb_prepared, "Raw", slot_mariadb_sync_raw);
	log_winner_by_elapsed("MariaDB transaction vs raw", "Transaction", slot_mariadb_sync_tx, "Autocommit", slot_mariadb_sync_raw);

	log_compare("Interpretation: SQLX async numbers include one connect/disconnect cycle per query; MariaDB async reuses pooled worker connections.");
}

stock log_winner_by_elapsed(const title[], const left_name[], benchmark_slot:left_slot, const right_name[], benchmark_slot:right_slot)
{
	new left_ms = g_result_elapsed_ms[left_slot];
	new right_ms = g_result_elapsed_ms[right_slot];
	new faster_ms;
	new slower_ms;
	new faster_name[32];
	new slower_name[32];
	new ratio_times100;

	if (left_ms <= 0 || right_ms <= 0)
	{
		return;
	}

	if (left_ms <= right_ms)
	{
		faster_ms = left_ms;
		slower_ms = right_ms;
		copy(faster_name, charsmax(faster_name), left_name);
		copy(slower_name, charsmax(slower_name), right_name);
	}
	else
	{
		faster_ms = right_ms;
		slower_ms = left_ms;
		copy(faster_name, charsmax(faster_name), right_name);
		copy(slower_name, charsmax(slower_name), left_name);
	}

	ratio_times100 = (faster_ms > 0) ? floatround(float(slower_ms) * 100.0 / float(faster_ms)) : 0;

	log_compare(
		"%s: %s was faster than %s by about %d.%02dx (%d ms vs %d ms).",
		title,
		faster_name,
		slower_name,
		ratio_times100 / 100,
		ratio_times100 % 100,
		faster_ms,
		slower_ms
	);
}

stock log_sync_benchmark(const label[], benchmark_slot:slot)
{
	log_compare(
		"%s: %d INSERTs in %d ms (~%d ops/s, avg %d us/op).",
		label,
		g_result_rows[slot],
		g_result_elapsed_ms[slot],
		g_result_rate_ops[slot],
		g_result_avg_latency_us[slot]
	);
}

stock log_async_benchmark(const label[], benchmark_slot:slot, ok_count, total_count, error_count)
{
	log_compare(
		"%s: %d/%d callbacks OK in %d ms (~%d ops/s), wall-clock callback arrival avg/min/max %d/%d/%d us, errors=%d.",
		label,
		ok_count,
		total_count,
		g_result_elapsed_ms[slot],
		g_result_rate_ops[slot],
		g_result_avg_latency_us[slot],
		g_result_min_latency_us[slot],
		g_result_max_latency_us[slot],
		error_count
	);
}

// ============================================================================
// UTILITIES
// ============================================================================

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

	formatex(g_table_name, charsmax(g_table_name), "%s_compare_rows", safe_prefix);
}

stock sanitize_identifier(const source[], dest[], maxlen)
{
	new length;
	new ch;

	for (new i = 0; source[i] != EOS && length < maxlen; ++i)
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

stock queue_stage_task(compare_task:task_id, const function[])
{
	set_task(0.0, function, _:task_id);
}

stock bool:can_run_stage()
{
	return g_run_active && mariadb_connection_valid(g_mariadb_db) && g_sqlx_db != Empty_Handle && g_sqlx_tuple != Empty_Handle;
}

stock build_sqlx_host(const host[], port, buffer[], maxlen)
{
	if (port > 0)
	{
		formatex(buffer, maxlen, "%s:%d", host, port);
	}
	else
	{
		copy(buffer, maxlen, host);
	}
}

stock record_failure(const fmt[], any:...)
{
	static message[192];

	vformat(message, charsmax(message), fmt, 2);
	g_failure_count++;
	log_compare_important("FAIL: %s", message);
}

stock log_verbose(const fmt[], any:...)
{
	if (get_pcvar_num(g_pcvar_verbose) == 0)
	{
		return;
	}

	static message[192];
	vformat(message, charsmax(message), fmt, 2);
	log_compare("%s", message);
}

stock log_compare(const fmt[], any:...)
{
	static message[192];

	vformat(message, charsmax(message), fmt, 2);
	log_amx("[MariaDBCompare] %s", message);
	server_print("[MariaDBCompare] %s", message);
}

stock log_compare_important(const fmt[], any:...)
{
	static message[192];

	vformat(message, charsmax(message), fmt, 2);
	log_amx("[MariaDBCompare] %s", message);
	server_print("[MariaDBCompare] %s", message);
}

stock log_db_error(const context[])
{
	new error[MARIADB_MAX_ERROR_LENGTH];
	new error_code;

	mariadb_get_error(g_mariadb_db, error, charsmax(error));
	error_code = mariadb_get_error_code(g_mariadb_db);
	log_compare_important("%s: %s (%d)", context, error, error_code);
}

stock log_stmt_error(mariadb_statement:stmt, const context[])
{
	new error[MARIADB_MAX_ERROR_LENGTH];
	new error_code;

	mariadb_stmt_error(stmt, error, charsmax(error));
	error_code = mariadb_stmt_error_code(stmt);
	log_compare_important("%s: %s (%d)", context, error, error_code);
}

