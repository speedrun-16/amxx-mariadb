#ifndef AMXX_MARIADB_MODULE_H
#define AMXX_MARIADB_MODULE_H

#include "amxxmodule.h"
#include "mariadb_async.h"
#include "mariadb_connection.h"
#include "mariadb_handle_table.h"
#include "mariadb_result.h"
#include "mariadb_statement.h"

#include <memory>
#include <string>

// ============================================================================
// GLOBAL STATE
// ============================================================================

// sentinel returned to Pawn for any invalid handle
constexpr cell k_invalid_handle = -1;

extern handle_table<connection_data> g_connections;
extern handle_table<result_data> g_results;
extern handle_table<statement_data> g_statements;
extern handle_table<async_job> g_jobs;
extern handle_table<async_stmt_data> g_async_stmts;
extern std::unique_ptr<async_worker> g_async_worker;

extern AMX_NATIVE_INFO g_mariadb_natives[];
extern AMX_NATIVE_INFO g_mariadb_async_natives[];

// ============================================================================
// AMX HELPERS
// ============================================================================

std::string get_amx_string(AMX* amx, cell param, int buffer_id = 0);
void set_amx_string(AMX* amx, cell param, cell maxlen_param, const std::string& value);
void set_amx_string_raw(AMX* amx, cell param, cell maxlen, const std::string& value);
void set_cell_ref(AMX* amx, cell param, cell value);
void set_float_ref(AMX* amx, cell param, float value);
// writes k_invalid_handle into the by-ref param to invalidate a Pawn handle
void invalidate_handle_ref(AMX* amx, cell param);
bool is_invalid_handle(cell handle);

// ============================================================================
// MODULE LIFECYCLE
// ============================================================================

#ifdef USE_METAMOD
void on_meta_query();
void on_meta_attach();
void on_meta_detach();
#endif

void on_amxx_query();
void on_amxx_attach();
void on_amxx_detach();
void on_plugins_loaded();
void on_plugins_unloading();
void on_plugins_unloaded();
void start_frame();
void server_deactivate();

#endif
