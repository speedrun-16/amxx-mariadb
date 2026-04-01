#include "mariadb_connection.h"

#include <algorithm>

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

namespace
{
    // converts a millisecond timeout to whole seconds, clamped to at least 1
    unsigned int timeout_to_seconds(unsigned int timeout_ms)
    {
        if (timeout_ms == 0)
        {
            return 5;
        }

        return std::max(1u, (timeout_ms + 999u) / 1000u);
    }
}

// ============================================================================
// FACTORY FUNCTIONS
// ============================================================================

MYSQL* create_raw_connection(const connection_options& options, std::string& error, unsigned int& error_code)
{
    error.clear();
    error_code = 0;

    MYSQL* mysql = mysql_init(nullptr);
    if (!mysql)
    {
        error = "mysql_init() failed";
        error_code = 0;
        return nullptr;
    }

    const auto timeout_s = timeout_to_seconds(options.timeout_ms);
    mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_s);
    mysql_options(mysql, MYSQL_OPT_READ_TIMEOUT, &timeout_s);
    mysql_options(mysql, MYSQL_OPT_WRITE_TIMEOUT, &timeout_s);

    // we currently do not expose TLS configuration to Pawn, so force plain
    // TCP connections instead of relying on Connector/C defaults or any
    // incidental SSL-capable build configuration.
    const my_bool ssl_enforce = 0;
    const my_bool verify_server_cert = 0;
    mysql_options(mysql, MYSQL_OPT_SSL_ENFORCE, &ssl_enforce);
    mysql_options(mysql, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify_server_cert);

#ifdef MYSQL_OPT_RECONNECT
    bool reconnect = options.auto_reconnect;
    mysql_options(mysql, MYSQL_OPT_RECONNECT, &reconnect);
#endif

#ifdef MYSQL_SET_CHARSET_NAME
    if (!options.charset.empty())
    {
        mysql_options(mysql, MYSQL_SET_CHARSET_NAME, options.charset.c_str());
    }
#endif

    // suppress per-query session-tracking packets that MariaDB Connector/C
    // negotiates by default but that we never consume; reduces response
    // packet size and shaves per-query overhead on the sync path
    mysql_options(mysql, MYSQL_INIT_COMMAND,
        "SET session_track_schema=0,"
        "session_track_system_variables='',"
        "session_track_state_change=0");

    if (!mysql_real_connect(mysql,
        options.host.c_str(),
        options.user.c_str(),
        options.password.c_str(),
        options.database.c_str(),
        options.port,
        nullptr,
        0))
    {
        error_code = mysql_errno(mysql);
        error = mysql_error(mysql);
        mysql_close(mysql);
        return nullptr;
    }

    if (!options.charset.empty())
    {
        mysql_set_character_set(mysql, options.charset.c_str());
    }

    return mysql;
}

std::shared_ptr<connection_data> create_connection(const connection_options& options, std::string& error, unsigned int& error_code)
{
    auto* raw = create_raw_connection(options, error, error_code);
    if (!raw)
    {
        return nullptr;
    }

    return std::make_shared<connection_data>(options, raw);
}

// ============================================================================
// CONNECTION DATA
// ============================================================================

connection_data::connection_data(connection_options options, MYSQL* mysql) :
    options_(std::move(options)),
    mysql_(mysql)
{
}

connection_data::~connection_data()
{
    if (mysql_)
    {
        mysql_close(mysql_);
        mysql_ = nullptr;
    }
}

MYSQL* connection_data::raw() const
{
    return mysql_;
}

const connection_options& connection_data::options() const
{
    return options_;
}

bool connection_data::ping()
{
    if (!mysql_)
    {
        set_last_error("Connection is closed.", 0);
        return false;
    }

    if (mysql_ping(mysql_) != 0)
    {
        set_last_error_from_mysql();
        return false;
    }

    set_last_error("", 0);
    return true;
}

bool connection_data::set_charset(const std::string& charset)
{
    if (!mysql_)
    {
        set_last_error("Connection is closed.", 0);
        return false;
    }

    if (mysql_set_character_set(mysql_, charset.c_str()) != 0)
    {
        set_last_error_from_mysql();
        return false;
    }

    options_.charset = charset;
    set_last_error("", 0);
    return true;
}

int connection_data::escape_string(const std::string& value, std::string& escaped)
{
    if (!mysql_)
    {
        set_last_error("Connection is closed.", 0);
        return -1;
    }

    std::string buffer;
    buffer.resize(value.size() * 2 + 1);
    const auto written = mysql_real_escape_string(mysql_, buffer.data(), value.c_str(), static_cast<unsigned long>(value.size()));
    buffer.resize(written);
    escaped = std::move(buffer);
    return static_cast<int>(written);
}

bool connection_data::begin()
{
    if (!mysql_)
    {
        set_last_error("Connection is closed.", 0);
        return false;
    }

    if (mysql_autocommit(mysql_, 0) != 0)
    {
        set_last_error_from_mysql();
        return false;
    }

    set_last_error("", 0);
    return true;
}

bool connection_data::commit()
{
    if (!mysql_)
    {
        set_last_error("Connection is closed.", 0);
        return false;
    }

    if (mysql_commit(mysql_) != 0 || mysql_autocommit(mysql_, 1) != 0)
    {
        set_last_error_from_mysql();
        return false;
    }

    set_last_error("", 0);
    return true;
}

bool connection_data::rollback()
{
    if (!mysql_)
    {
        set_last_error("Connection is closed.", 0);
        return false;
    }

    if (mysql_rollback(mysql_) != 0 || mysql_autocommit(mysql_, 1) != 0)
    {
        set_last_error_from_mysql();
        return false;
    }

    set_last_error("", 0);
    return true;
}

std::string connection_data::server_version() const
{
    if (!mysql_)
    {
        return "";
    }

    const auto* version = mysql_get_server_info(mysql_);
    return version ? version : "";
}

void connection_data::set_last_error(const std::string& message, unsigned int code)
{
    last_error_ = message;
    last_error_code_ = code;
}

void connection_data::set_last_error_from_mysql()
{
    last_error_ = mysql_ ? mysql_error(mysql_) : "Connection is closed.";
    last_error_code_ = mysql_ ? mysql_errno(mysql_) : 0;
}

void connection_data::set_last_error_from_stmt(MYSQL_STMT* stmt)
{
    last_error_ = stmt ? mysql_stmt_error(stmt) : "Statement is closed.";
    last_error_code_ = stmt ? mysql_stmt_errno(stmt) : 0;
}

const std::string& connection_data::last_error() const
{
    return last_error_;
}

unsigned int connection_data::last_error_code() const
{
    return last_error_code_;
}
