#pragma semicolon 1
#pragma compress 1

#include <amxmodx>
#include <amxmisc>

#define PLUGIN  "MariaDB DSN Test"
#define VERSION "1.1.0"
#define AUTHOR  "PWNED"

#include <mariadb>
#include <mariadb_dsn>

// ============================================================================
// GLOBALS
// ============================================================================

new g_pcvar_dsn;
new g_pcvar_autorun;
new g_total_asserts;
new g_failed_asserts;

// ============================================================================
// LIFECYCLE
// ============================================================================

public plugin_init()
{
    register_plugin(PLUGIN, VERSION, AUTHOR);

    register_concmd("amx_mariadb_dsn_test_run", "command_run_tests", ADMIN_RCON, "Runs the MariaDB DSN parser tests.");
    register_concmd("amx_mariadb_dsn_test_connect", "command_test_connect", ADMIN_RCON, "Attempts a real MariaDB connection using mariadb_dsn_test_dsn.");

    g_pcvar_dsn = create_cvar(
        "mariadb_dsn_test_dsn",
        "",
        FCVAR_PROTECTED,
        "DSN used by amx_mariadb_dsn_test_connect, for example mysql://user:pass@127.0.0.1:3306/db?timeout_ms=100"
    );
    g_pcvar_autorun = create_cvar("mariadb_dsn_test_autorun", "0", FCVAR_PROTECTED, "Automatically run the DSN parser tests in plugin_cfg.");
}

public plugin_cfg()
{
    if (get_pcvar_num(g_pcvar_autorun) != 0)
        set_task(1.0, "task_autorun");
}

public task_autorun()
{
    start_dsn_suite();
}

// ============================================================================
// COMMANDS
// ============================================================================

public command_run_tests(id, level, cid)
{
    if (!cmd_access(id, level, cid, 1))
        return PLUGIN_HANDLED;

    start_dsn_suite();
    console_print(id, "[MariaDBDSN] Parser suite completed. See server console/log for details.");
    return PLUGIN_HANDLED;
}

public command_test_connect(id, level, cid)
{
    new dsn[256];
    new error[MARIADB_MAX_ERROR_LENGTH];
    new error_code;
    new mariadb_connection:db;
    new version[64];

    if (!cmd_access(id, level, cid, 1))
        return PLUGIN_HANDLED;

    get_pcvar_string(g_pcvar_dsn, dsn, charsmax(dsn));
    if (!dsn[0])
    {
        console_print(id, "[MariaDBDSN] mariadb_dsn_test_dsn is empty.");
        return PLUGIN_HANDLED;
    }

    db = mariadb_connect_dsn(dsn, error, charsmax(error), error_code);
    if (!mariadb_connection_valid(db))
    {
        console_print(id, "[MariaDBDSN] Connect failed: %s (%d)", error, error_code);
        return PLUGIN_HANDLED;
    }

    mariadb_server_version(db, version, charsmax(version));
    console_print(id, "[MariaDBDSN] Connect ok. Server version: %s", version);
    mariadb_disconnect(db);
    return PLUGIN_HANDLED;
}

// ============================================================================
// SUITE
// ============================================================================

stock start_dsn_suite()
{
    g_total_asserts = 0;
    g_failed_asserts = 0;

    log_amx("[MariaDBDSN] Starting DSN parser suite...");

    test_parse_full_mysql_dsn();
    test_parse_full_mariadb_dsn_with_query();
    test_parse_preserves_defaults();
    test_parse_user_without_password();
    test_parse_percent_decoding();
    test_invalid_scheme();
    test_missing_database();
    test_invalid_port();
    test_invalid_timeout();
    test_invalid_auto_reconnect();
    test_invalid_percent_encoding();
    test_connect_wrapper_parse_failure();

    if (g_failed_asserts == 0)
        log_amx("[MariaDBDSN] PASS - %d assertions", g_total_asserts);
    else
        log_amx("[MariaDBDSN] FAIL - %d/%d assertions failed", g_failed_asserts, g_total_asserts);
}

// ============================================================================
// TEST CASES
// ============================================================================

stock test_parse_full_mysql_dsn()
{
    new host[128] = "default-host";
    new user[64] = "default-user";
    new pass[128] = "default-pass";
    new database[64] = "default-db";
    new charset[32] = "latin1";
    new error[128];
    new port = 1;
    new timeout_ms = 5000;
    new bool:auto_reconnect = false;

    new bool:ok = mariadb_parse_dsn(
        "mysql://root:secret@127.0.0.1:3306/speedrun",
        host, charsmax(host),
        port,
        user, charsmax(user),
        pass, charsmax(pass),
        database, charsmax(database),
        charset, charsmax(charset),
        timeout_ms,
        auto_reconnect,
        error, charsmax(error)
    );

    assert_true(ok, "full mysql dsn parses");
    assert_string_equal(host, "127.0.0.1", "full mysql dsn host");
    assert_num_equal(port, 3306, "full mysql dsn port");
    assert_string_equal(user, "root", "full mysql dsn user");
    assert_string_equal(pass, "secret", "full mysql dsn password");
    assert_string_equal(database, "speedrun", "full mysql dsn database");
    assert_string_equal(charset, "latin1", "full mysql dsn preserves charset default");
    assert_num_equal(timeout_ms, 5000, "full mysql dsn preserves timeout default");
    assert_bool_equal(auto_reconnect, false, "full mysql dsn preserves auto_reconnect default");
}

stock test_parse_full_mariadb_dsn_with_query()
{
    new host[128] = "default-host";
    new user[64];
    new pass[128];
    new database[64];
    new charset[32] = "latin1";
    new error[128];
    new port = 1;
    new timeout_ms = 1;
    new bool:auto_reconnect = true;

    new bool:ok = mariadb_parse_dsn(
        "mariadb://reader:pw@db.local:3394/game?charset=utf8mb4&timeout_ms=150&auto_reconnect=0",
        host, charsmax(host),
        port,
        user, charsmax(user),
        pass, charsmax(pass),
        database, charsmax(database),
        charset, charsmax(charset),
        timeout_ms,
        auto_reconnect,
        error, charsmax(error)
    );

    assert_true(ok, "full mariadb dsn with query parses");
    assert_string_equal(host, "db.local", "query dsn host");
    assert_num_equal(port, 3394, "query dsn port");
    assert_string_equal(user, "reader", "query dsn user");
    assert_string_equal(pass, "pw", "query dsn password");
    assert_string_equal(database, "game", "query dsn database");
    assert_string_equal(charset, "utf8mb4", "query dsn charset");
    assert_num_equal(timeout_ms, 150, "query dsn timeout");
    assert_bool_equal(auto_reconnect, false, "query dsn auto_reconnect");
}

stock test_parse_preserves_defaults()
{
    new host[128] = "fallback-host";
    new user[64] = "fallback-user";
    new pass[128] = "fallback-pass";
    new database[64] = "fallback-db";
    new charset[32] = "fallback-charset";
    new error[128];
    new port = 7777;
    new timeout_ms = 250;
    new bool:auto_reconnect = false;

    new bool:ok = mariadb_parse_dsn(
        "mysql://db.example.com/main",
        host, charsmax(host),
        port,
        user, charsmax(user),
        pass, charsmax(pass),
        database, charsmax(database),
        charset, charsmax(charset),
        timeout_ms,
        auto_reconnect,
        error, charsmax(error)
    );

    assert_true(ok, "dsn without auth parses");
    assert_string_equal(host, "db.example.com", "dsn without auth host");
    assert_num_equal(port, 7777, "dsn without auth preserves default port");
    assert_string_equal(user, "fallback-user", "dsn without auth preserves default user");
    assert_string_equal(pass, "fallback-pass", "dsn without auth preserves default password");
    assert_string_equal(database, "main", "dsn without auth database");
    assert_string_equal(charset, "fallback-charset", "dsn without auth preserves default charset");
    assert_num_equal(timeout_ms, 250, "dsn without auth preserves default timeout");
    assert_bool_equal(auto_reconnect, false, "dsn without auth preserves default auto_reconnect");
}

stock test_parse_user_without_password()
{
    new host[128];
    new user[64] = "fallback-user";
    new pass[128] = "fallback-pass";
    new database[64];
    new charset[32] = "utf8mb4";
    new error[128];
    new port = 3306;
    new timeout_ms = 5000;
    new bool:auto_reconnect = true;

    new bool:ok = mariadb_parse_dsn(
        "mysql://writer@db.internal/records",
        host, charsmax(host),
        port,
        user, charsmax(user),
        pass, charsmax(pass),
        database, charsmax(database),
        charset, charsmax(charset),
        timeout_ms,
        auto_reconnect,
        error, charsmax(error)
    );

    assert_true(ok, "dsn with user and no password parses");
    assert_string_equal(user, "writer", "dsn with user and no password user");
    assert_string_equal(pass, "fallback-pass", "dsn with user and no password preserves password");
    assert_string_equal(host, "db.internal", "dsn with user and no password host");
    assert_string_equal(database, "records", "dsn with user and no password database");
}

stock test_parse_percent_decoding()
{
    new host[128];
    new user[64];
    new pass[128];
    new database[64];
    new charset[32] = "utf8mb4";
    new error[128];
    new port = 3306;
    new timeout_ms = 5000;
    new bool:auto_reconnect = true;

    new bool:ok = mariadb_parse_dsn(
        "mysql://user%40name:p%3Ass@db.example.com/my%2Ddb?charset=utf8mb4",
        host, charsmax(host),
        port,
        user, charsmax(user),
        pass, charsmax(pass),
        database, charsmax(database),
        charset, charsmax(charset),
        timeout_ms,
        auto_reconnect,
        error, charsmax(error)
    );

    assert_true(ok, "percent-decoded dsn parses");
    assert_string_equal(user, "user@name", "percent-decoded dsn user");
    assert_string_equal(pass, "p:ss", "percent-decoded dsn password");
    assert_string_equal(database, "my-db", "percent-decoded dsn database");
}

stock test_invalid_scheme()
{
    assert_parse_fails("postgres://user:pass@host/db", "invalid scheme fails");
}

stock test_missing_database()
{
    assert_parse_fails("mysql://user:pass@host", "missing database fails");
}

stock test_invalid_port()
{
    assert_parse_fails("mysql://user:pass@host:nope/db", "invalid port fails");
}

stock test_invalid_timeout()
{
    assert_parse_fails("mysql://host/db?timeout_ms=fast", "invalid timeout fails");
}

stock test_invalid_auto_reconnect()
{
    assert_parse_fails("mysql://host/db?auto_reconnect=maybe", "invalid auto_reconnect fails");
}

stock test_invalid_percent_encoding()
{
    assert_parse_fails("mysql://user%ZZ:pass@host/db", "invalid percent-encoding fails");
}

stock test_connect_wrapper_parse_failure()
{
    new error[MARIADB_MAX_ERROR_LENGTH];
    new error_code = 777;
    new mariadb_connection:db = mariadb_connect_dsn("not-a-dsn", error, charsmax(error), error_code);

    assert_true(!mariadb_connection_valid(db), "connect wrapper rejects invalid dsn");
    assert_true(error[0] != EOS, "connect wrapper returns parse error text");
    assert_num_equal(error_code, 0, "connect wrapper parse failure keeps error_code at zero");
}

// ============================================================================
// ASSERTIONS
// ============================================================================

stock assert_parse_fails(const dsn[], const label[])
{
    new host[128] = "default-host";
    new user[64] = "default-user";
    new pass[128] = "default-pass";
    new database[64] = "default-db";
    new charset[32] = "utf8mb4";
    new error[128];
    new port = 3306;
    new timeout_ms = 5000;
    new bool:auto_reconnect = true;

    new bool:ok = mariadb_parse_dsn(
        dsn,
        host, charsmax(host),
        port,
        user, charsmax(user),
        pass, charsmax(pass),
        database, charsmax(database),
        charset, charsmax(charset),
        timeout_ms,
        auto_reconnect,
        error, charsmax(error)
    );

    assert_true(!ok, label);
    assert_true(error[0] != EOS, fmt("%s returns an error", label));
}

stock assert_true(bool:condition, const label[])
{
    g_total_asserts++;

    if (condition)
    {
        log_amx("[MariaDBDSN] PASS - %s", label);
        return;
    }

    g_failed_asserts++;
    log_amx("[MariaDBDSN] FAIL - %s", label);
}

stock assert_num_equal(actual, expected, const label[])
{
    g_total_asserts++;

    if (actual == expected)
    {
        log_amx("[MariaDBDSN] PASS - %s", label);
        return;
    }

    g_failed_asserts++;
    log_amx("[MariaDBDSN] FAIL - %s (expected %d, got %d)", label, expected, actual);
}

stock assert_bool_equal(bool:actual, bool:expected, const label[])
{
    assert_num_equal(_:actual, _:expected, label);
}

stock assert_string_equal(const actual[], const expected[], const label[])
{
    g_total_asserts++;

    if (equal(actual, expected))
    {
        log_amx("[MariaDBDSN] PASS - %s", label);
        return;
    }

    g_failed_asserts++;
    log_amx("[MariaDBDSN] FAIL - %s (expected '%s', got '%s')", label, expected, actual);
}
