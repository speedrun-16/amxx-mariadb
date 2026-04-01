#ifndef AMXX_MARIADB_CONNECTION_H
#define AMXX_MARIADB_CONNECTION_H

#include "mariadb_mysql.h"

#include <memory>
#include <string>

// ============================================================================
// CONNECTION OPTIONS
// ============================================================================

// parameters passed to create_connection() and stored for pool key generation
struct connection_options
{
    std::string host;
    std::string user;
    std::string password;
    std::string database;
    std::string charset;
    unsigned int port{3306};
    unsigned int timeout_ms{5000};
    bool auto_reconnect{true};
};

// ============================================================================
// CONNECTION DATA
// ============================================================================

// wraps a MYSQL* and tracks the last error on this connection
class connection_data
{
public:
    connection_data(connection_options options, MYSQL* mysql);
    ~connection_data();

    // returns the raw MYSQL* pointer
    MYSQL* raw() const;
    const connection_options& options() const;

    bool ping();
    bool set_charset(const std::string& charset);
    // escapes value into escaped using mysql_real_escape_string, returns byte count written
    int escape_string(const std::string& value, std::string& escaped);
    bool begin();
    bool commit();
    bool rollback();
    std::string server_version() const;

    void set_last_error(const std::string& message, unsigned int code);
    void set_last_error_from_mysql();
    void set_last_error_from_stmt(MYSQL_STMT* stmt);
    const std::string& last_error() const;
    unsigned int last_error_code() const;

private:
    connection_options options_;
    MYSQL* mysql_{nullptr};
    std::string last_error_;
    unsigned int last_error_code_{0};
};

// ============================================================================
// FACTORY FUNCTIONS
// ============================================================================

std::shared_ptr<connection_data> create_connection(
    const connection_options& options,
    std::string& error,
    unsigned int& error_code);

// creates a raw MYSQL* without wrapping it - caller owns the pointer
MYSQL* create_raw_connection(
    const connection_options& options,
    std::string& error,
    unsigned int& error_code);

#endif
