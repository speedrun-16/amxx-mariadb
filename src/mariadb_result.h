#ifndef AMXX_MARIADB_RESULT_H
#define AMXX_MARIADB_RESULT_H

#include "mariadb_mysql.h"

#include <memory>
#include <string>
#include <vector>

// ============================================================================
// RESULT TYPES
// ============================================================================

// a single cell value from a result row - may be null
struct result_cell
{
    bool is_null{false};
    std::string text;
};

// one row from a result set - owns its cells
struct result_row
{
    std::vector<result_cell> cells;
};

// ============================================================================
// RESULT DATA
// ============================================================================

// in-memory snapshot of a result set with typed column reads
class result_data
{
public:
    result_data() = default;
    result_data(std::vector<std::string> columns, std::vector<result_row> rows);

    // advances the cursor to the next row, returns false when exhausted
    bool next_row();
    // resets the cursor to before the first row
    bool rewind();
    int row_count() const;
    int column_count() const;
    // returns the 0-based index of name, or -1 if not found
    int column_index(const std::string& name) const;
    const std::string& column_name(int column) const;
    bool is_null(int column) const;
    int read_int(int column) const;
    bool read_bool(int column) const;
    float read_float(int column) const;
    const std::string& read_string(int column) const;

    static std::shared_ptr<result_data> from_mysql_result(MYSQL_RES* result);
    static std::shared_ptr<result_data> from_stmt_result(
        MYSQL_STMT* stmt,
        std::string& error,
        unsigned int& error_code);

private:
    bool has_current_row() const;
    bool is_column_valid(int column) const;
    static std::shared_ptr<result_data> build(
        std::vector<std::string> columns,
        std::vector<result_row> rows);

    std::vector<std::string> columns_;
    std::vector<result_row> rows_;
    int current_row_{-1};
};

#endif
