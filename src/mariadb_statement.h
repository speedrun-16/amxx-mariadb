#ifndef AMXX_MARIADB_STATEMENT_H
#define AMXX_MARIADB_STATEMENT_H

#include "mariadb_connection.h"
#include "mariadb_result.h"

#include <memory>
#include <string>
#include <vector>

// ============================================================================
// STATEMENT PARAM TYPE
// ============================================================================

// type tag for a single bound parameter slot
enum class stmt_param_type
{
    unset = 0,
    t_int,
    t_bool,
    t_float,
    t_string,
    t_null
};

// ============================================================================
// STATEMENT PARAM VALUE
// ============================================================================

// storage for one bound parameter - only the field matching type is valid
struct stmt_param_value
{
    stmt_param_type type{stmt_param_type::unset};
    int int_value{0};
    float float_value{0.0f};
    std::string string_value;
};

// ============================================================================
// STATEMENT DATA
// ============================================================================

// wraps a MYSQL_STMT* with lazy parameter binding and typed bind helpers
class statement_data
{
public:
    statement_data(std::shared_ptr<connection_data> connection, MYSQL_STMT* stmt);
    ~statement_data();

    bool bind_int(unsigned int index, int value);
    bool bind_bool(unsigned int index, bool value);
    bool bind_float(unsigned int index, float value);
    bool bind_string(unsigned int index, const std::string& value);
    bool bind_null(unsigned int index);

    // resets parameter bindings and the server-side cursor
    bool reset();
    // executes a write statement, fills affected_rows and insert_id
    bool exec(int& affected_rows, int& insert_id);
    // executes a SELECT statement, returns the result set or nullptr on error
    std::shared_ptr<result_data> query();

    void set_last_error(const std::string& message, unsigned int code);
    const std::string& last_error() const;
    unsigned int last_error_code() const;
    unsigned int param_count() const;

private:
    // validates index and sets an error if out of range
    bool prepare_param_slot(unsigned int index);
    void reset_binding_storage();
    void update_bind_slot(unsigned int index);
    // calls mysql_stmt_bind_param() if bindings are dirty
    bool bind_params_if_needed();

    std::shared_ptr<connection_data> connection_;
    MYSQL_STMT* stmt_{nullptr};
    std::vector<stmt_param_value> params_;
    std::vector<MYSQL_BIND> param_binds_;
    std::vector<int> int_values_;
    std::vector<float> float_values_;
    std::vector<unsigned long> string_lengths_;
    std::vector<my_bool> is_null_;
    std::vector<std::string> string_values_;
    std::string last_error_;
    unsigned int last_error_code_{0};
    bool bindings_dirty_{true};
};

// ============================================================================
// FACTORY FUNCTION
// ============================================================================

std::shared_ptr<statement_data> create_statement(
    const std::shared_ptr<connection_data>& connection,
    const std::string& query);

#endif
