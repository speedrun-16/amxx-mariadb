# amxx-mariadb

![Author](https://img.shields.io/badge/Author-PWNED-8cf?style=for-the-badge "Author") ![Version](https://img.shields.io/badge/Version-1.1.0-blue?style=for-the-badge "Version")

Native MariaDB/MySQL driver module for [AMX Mod X](https://www.amxmodx.org/). \
An alternative to the legacy SQLX driver, built on [MariaDB Connector/C 3.4.8](https://github.com/mariadb-corporation/mariadb-connector-c).

---

## ☰ Features

- **Typed handles** - four distinct types (`mariadb_connection`, `mariadb_result`,
  `mariadb_statement`, `mariadb_job`)
- **Prepared statements** - `?` placeholder binding for all value types, no manual escaping
- **Async worker** - single background thread with a persistent connection pool; callbacks
  fire on the main game frame, never mid-tick
- **Connection pooling** - idle connections are reused by config hash; no reconnect per job
- **Transactions** - `begin` / `commit` / `rollback` with auto-commit restoration
- **Job cancellation** - queued (not yet running) async jobs can be cancelled without
  a callback firing
- **Monotonic timing** - `mariadb_monotonic_time()` for frame-independent measurement

---

## ☰ Benchmarks

Measured on Linux, MariaDB 10.11.14 on the same host (loopback), InnoDB.  
500 sync INSERTs and 200 async INSERTs per mode, averaged over 2 warm runs.

<img width="674" height="634" alt="benchmark results" src="https://github.com/user-attachments/assets/5d985af7-32d0-416d-af26-9ca8165f776e" />


### Running the benchmarks yourself

```
// integration suite
mariadb_test_sync_loops  500
mariadb_test_async_jobs  200
mariadb_test_verbose     0
amx_mariadb_test_run

// driver comparison
mariadb_compare_sync_loops  500
mariadb_compare_async_jobs  200
mariadb_compare_verbose      0
amx_mariadb_sqlx_compare_run
```

Discard the first run - InnoDB buffer pool is cold. Use the second run.

---

## ☰ Requirements

- AMX Mod X 1.9+
- Metamod-R
- MariaDB Connector/C 3.4.8
- CMake 3.21+

---

## ☰ Installation

1. Copy the module to `addons/amxmodx/modules/`
   - Windows: `mariadb_amxx.dll`
   - Linux: `mariadb_amxx_i386.so`
2. Add `mariadb` to `addons/amxmodx/configs/modules.ini`
3. Copy `src/pawn/include/mariadb.inc` to your compiler include path

---

## ☰ Building

```sh
# Windows (Visual Studio, Win32)
cmake -B build -A Win32
cmake --build build --config Release

# Linux (32-bit cross-compile)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target mariadb_amxx
```

Output lands at `build/addons/amxmodx/modules/`.

CI builds run via `.github/workflows/build.yml` using
[setup-amxx](https://github.com/speedrun-16/setup-amxx) for script compilation.
The Linux build compiles against a static OpenSSL 1.0.2 built from source inside
an Ubuntu 14.04 container to keep the glibc requirement at 2.19.

---

## ☰ Examples

### Connect and disconnect

```pawn
#pragma semicolon 1
#include <amxmodx>
#include <mariadb>

new mariadb_connection:g_db = invalid_mariadb_connection;

public plugin_cfg()
{
    new error[MARIADB_MAX_ERROR_LENGTH];
    new error_code;

    g_db = mariadb_connect(
        "127.0.0.1", "root", "", "mydb",
        .error = error, .maxlen = charsmax(error), .error_code = error_code
    );

    if (!mariadb_connection_valid(g_db))
    {
        log_amx("[mariadb] connect failed: %s (%d)", error, error_code);
        return;
    }
}

public plugin_end()
{
    mariadb_disconnect(g_db);
}
```

### Async SELECT: mariadb vs SQLX

**SQLX**

```pawn
stock load_profile(id)
{
    new steam_id[32];
    get_user_authid(id, steam_id, charsmax(steam_id));

    // manual escape into a separate buffer, then interpolate into the query string
    new escaped[65];
    copy(escaped, charsmax(escaped), steam_id);
    SQL_QuoteString(Empty_Handle, escaped, charsmax(escaped), steam_id);

    new query[256];
    formatex(query, charsmax(query),
        "SELECT id, pseudo FROM `profiles` WHERE steamid = '%s'", escaped);

    new data[1];
    data[0] = id;
    SQL_ThreadQuery(g_sql_tuple, "handler_load_profile", query, data, sizeof(data));
}

public handler_load_profile(fail_state, Handle:query, error[], errnum, data[], data_size)
{
    if (fail_state != TQUERY_SUCCESS)
    {
        log_amx("[stats] SQL error: %s (%d)", error, errnum);
        return;
    }

    new id = data[0];

    if (SQL_NumResults(query) > 0)
    {
        new profile_id = SQL_ReadResult(query, 0);
        SQL_ReadResult(query, 1, g_pseudo[id], charsmax(g_pseudo[]));
    }
}
```

**mariadb**

```pawn
stock load_profile(id)
{
    new steam_id[32];
    get_user_authid(id, steam_id, charsmax(steam_id));

    // bind directly, no escaping needed
    new mariadb_statement:stmt = mariadb_prepare(g_db,
        "SELECT id, pseudo FROM `profiles` WHERE steamid = ?");
    mariadb_bind_string(stmt, 0, steam_id, charsmax(steam_id));

    new data[1];
    data[0] = id;
    mariadb_async_stmt_query(g_db, "handler_load_profile", stmt, data, sizeof(data));
    mariadb_async_stmt_close(stmt);
}

public handler_load_profile(mariadb_async_state:state, mariadb_result:result,
    affected_rows, insert_id, const error[], error_code,
    const data[], data_size, Float:queue_time)
{
    if (state != mariadb_async_ok)
    {
        log_amx("[stats] query failed: %s (%d)", error, error_code);
        return;
    }

    new id = data[0];

    if (mariadb_next_row(result))
    {
        new profile_id = mariadb_read_int(result, 0);
        mariadb_read_string(result, 1, g_pseudo[id], charsmax(g_pseudo[]));
    }
}
```

### Async INSERT with prepared statement

```pawn
stock save_run(id, time_ms, jumps, strafes, Float:sync)
{
    new mariadb_statement:stmt = mariadb_prepare(g_db,
        "INSERT INTO `runlog` (pid, mid, time, jumps, strafes, sync) \
         VALUES (?, ?, ?, ?, ?, ?)");

    mariadb_bind_int(stmt, 0, g_player_db_id[id]);
    mariadb_bind_int(stmt, 1, g_current_map_id);
    mariadb_bind_int(stmt, 2, time_ms);
    mariadb_bind_int(stmt, 3, jumps);
    mariadb_bind_int(stmt, 4, strafes);
    mariadb_bind_float(stmt, 5, sync);

    new data[1];
    data[0] = id;
    mariadb_async_stmt_exec(g_db, "handler_run_saved", stmt, data, sizeof(data));
    mariadb_async_stmt_close(stmt);
}

public handler_run_saved(mariadb_async_state:state, mariadb_result:result,
    affected_rows, insert_id, const error[], error_code,
    const data[], data_size, Float:queue_time)
{
    if (state != mariadb_async_ok)
    {
        log_amx("[stats] save failed: %s (%d)", error, error_code);
        return;
    }

    new id = data[0];
    // insert_id holds the new row's auto-increment id
    g_last_run_id[id] = insert_id;
}
```

### Transaction (batch inserts)

```pawn
stock flush_pending_time(id)
{
    mariadb_begin(g_db);

    new mariadb_statement:stmt = mariadb_prepare(g_db,
        "INSERT INTO `timespent` (pid, mid, ticks) VALUES (?, ?, ?) \
         ON DUPLICATE KEY UPDATE ticks = ticks + VALUES(ticks)");

    for (new i = 0; i < g_pending_count[id]; i++)
    {
        mariadb_bind_int(stmt, 0, g_player_db_id[id]);
        mariadb_bind_int(stmt, 1, g_pending_map_id[id][i]);
        mariadb_bind_int(stmt, 2, g_pending_ticks[id][i]);
        mariadb_stmt_exec(stmt);
        mariadb_stmt_reset(stmt);
    }

    mariadb_stmt_close(stmt);
    mariadb_commit(g_db);
}
```

---

## ☰ License

MariaDB Connector/C is licensed under LGPL v2.1. See the
[connector repository](https://github.com/mariadb-corporation/mariadb-connector-c)
for full terms.

---

## ☰ Author

[PWNED](https://github.com/5z3f)
