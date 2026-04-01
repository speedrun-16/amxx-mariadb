#include "mariadb_module.h"

#include <cstring>

// ============================================================================
// GLOBAL STATE
// ============================================================================

handle_table<connection_data> g_connections;
handle_table<result_data>     g_results;
handle_table<statement_data>  g_statements;
handle_table<async_job>       g_jobs;
handle_table<async_stmt_data>  g_async_stmts;
std::unique_ptr<async_worker>  g_async_worker;

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

namespace
{
    int count_registered_natives(const AMX_NATIVE_INFO* natives)
    {
        int count = 0;
        while (natives[count].name != nullptr)
        {
            ++count;
        }
        return count;
    }
}

// ============================================================================
// AMX HELPERS
// ============================================================================

std::string get_amx_string(AMX* amx, cell param, int buffer_id)
{
    int length = 0;
    const char* value = MF_GetAmxString(amx, param, buffer_id, &length);
    return value ? std::string(value, length) : std::string();
}

void set_amx_string_raw(AMX* amx, cell param, cell maxlen, const std::string& value)
{
    if (maxlen > 0)
    {
        MF_SetAmxString(amx, param, value.c_str(), maxlen);
    }
}

void set_amx_string(AMX* amx, cell param, cell maxlen_param, const std::string& value)
{
    set_amx_string_raw(amx, param, maxlen_param, value);
}

void set_cell_ref(AMX* amx, cell param, cell value)
{
    *MF_GetAmxAddr(amx, param) = value;
}

void set_float_ref(AMX* amx, cell param, float value)
{
    *MF_GetAmxAddr(amx, param) = amx_ftoc(value);
}

void invalidate_handle_ref(AMX* amx, cell param)
{
    set_cell_ref(amx, param, k_invalid_handle);
}

bool is_invalid_handle(cell handle)
{
    return handle == k_invalid_handle;
}

// ============================================================================
// DLLMAIN (WINDOWS)
// ============================================================================

#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
    }

    return TRUE;
}
#endif

// ============================================================================
// METAMOD CALLBACKS
// ============================================================================

#ifdef USE_METAMOD
void on_meta_query()
{
}

void on_meta_attach()
{
}

void on_meta_detach()
{
}
#endif

// ============================================================================
// AMXX LIFECYCLE
// ============================================================================

void on_amxx_query()
{
}

void on_amxx_attach()
{
    mysql_library_init(0, nullptr, nullptr);
    MF_AddNatives(g_mariadb_natives);
    MF_AddNatives(g_mariadb_async_natives);
}

void on_amxx_detach()
{
    if (g_async_worker)
    {
        g_async_worker->stop(false);
        g_async_worker.reset();
    }

    g_async_stmts.clear();
    g_jobs.clear();
    g_results.clear();
    g_statements.clear();
    g_connections.clear();
    mysql_library_end();
}

void on_plugins_loaded()
{
    if (!g_async_worker)
    {
        g_async_worker = std::make_unique<async_worker>();
        g_async_worker->start(MARIADB_ASYNC_WORKER_THREADS);
    }
}

void on_plugins_unloading()
{
    if (g_async_worker)
    {
        g_async_worker->stop(true);
        g_async_worker->process_completions();
    }
}

void on_plugins_unloaded()
{
    g_async_stmts.clear();
    g_jobs.clear();
    g_results.clear();
    g_statements.clear();
    g_connections.clear();

    if (g_async_worker)
    {
        g_async_worker.reset();
    }
}

void start_frame()
{
    if (g_async_worker)
    {
        g_async_worker->process_completions();
    }

    RETURN_META(MRES_IGNORED);
}

void server_deactivate()
{
    if (g_async_worker)
    {
        g_async_worker->process_completions();
    }

    RETURN_META(MRES_IGNORED);
}

extern "C" void __cxa_pure_virtual(void)
{
}
