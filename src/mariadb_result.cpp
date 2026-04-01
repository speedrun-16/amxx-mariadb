#include "mariadb_result.h"

#include <cstdlib>
#include <cstring>
#include <memory>

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

namespace
{
    const std::string k_empty_string;

    // picks a fetch buffer size for a column based on field metadata
    unsigned long default_buffer_length(const MYSQL_FIELD& field)
    {
        if (field.max_length > 0)
        {
            return field.max_length + 1;
        }

        switch (field.type)
        {
            case MYSQL_TYPE_TINY:
            case MYSQL_TYPE_SHORT:
            case MYSQL_TYPE_LONG:
            case MYSQL_TYPE_INT24:
            case MYSQL_TYPE_LONGLONG:
            case MYSQL_TYPE_DECIMAL:
            case MYSQL_TYPE_NEWDECIMAL:
            case MYSQL_TYPE_FLOAT:
            case MYSQL_TYPE_DOUBLE:
                return 64;
            default:
                return 256;
        }
    }
}

// ============================================================================
// RESULT DATA
// ============================================================================

result_data::result_data(std::vector<std::string> columns, std::vector<result_row> rows) :
    columns_(std::move(columns)),
    rows_(std::move(rows))
{
}

std::shared_ptr<result_data> result_data::build(std::vector<std::string> columns, std::vector<result_row> rows)
{
    return std::make_shared<result_data>(std::move(columns), std::move(rows));
}

std::shared_ptr<result_data> result_data::from_mysql_result(MYSQL_RES* result)
{
    if (!result)
    {
        return nullptr;
    }

    const auto field_count = static_cast<unsigned int>(mysql_num_fields(result));
    const auto* fields = mysql_fetch_fields(result);

    std::vector<std::string> columns;
    columns.reserve(field_count);
    for (unsigned int i = 0; i < field_count; ++i)
    {
        columns.emplace_back(fields[i].name ? fields[i].name : "");
    }

    std::vector<result_row> rows;
    MYSQL_ROW row = nullptr;
    while ((row = mysql_fetch_row(result)) != nullptr)
    {
        const auto* lengths = mysql_fetch_lengths(result);
        result_row copied_row;
        copied_row.cells.reserve(field_count);

        for (unsigned int i = 0; i < field_count; ++i)
        {
            result_cell cell;
            cell.is_null = (row[i] == nullptr);
            if (!cell.is_null)
            {
                if (lengths)
                    cell.text.assign(row[i], lengths[i]);
                else
                    cell.text.assign(row[i]);
            }
            copied_row.cells.push_back(std::move(cell));
        }

        rows.push_back(std::move(copied_row));
    }

    return build(std::move(columns), std::move(rows));
}

std::shared_ptr<result_data> result_data::from_stmt_result(MYSQL_STMT* stmt, std::string& error, unsigned int& error_code)
{
    error.clear();
    error_code = 0;

    auto metadata = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>(
        mysql_stmt_result_metadata(stmt), mysql_free_result);
    if (!metadata)
    {
        error_code = mysql_stmt_errno(stmt);
        error = mysql_stmt_error(stmt);
        return nullptr;
    }

    const auto field_count = static_cast<unsigned int>(mysql_num_fields(metadata.get()));
    const auto* fields = mysql_fetch_fields(metadata.get());

    std::vector<std::string> columns;
    columns.reserve(field_count);
    for (unsigned int i = 0; i < field_count; ++i)
    {
        columns.emplace_back(fields[i].name ? fields[i].name : "");
    }

    std::vector<MYSQL_BIND> binds(field_count);
    std::vector<std::vector<char>> buffers(field_count);
    std::vector<unsigned long> lengths(field_count);
    std::vector<my_bool> is_null(field_count);
    std::vector<my_bool> has_error(field_count);

    for (unsigned int i = 0; i < field_count; ++i)
    {
        buffers[i].resize(default_buffer_length(fields[i]));
        memset(&binds[i], 0, sizeof(MYSQL_BIND));
        binds[i].buffer_type = MYSQL_TYPE_STRING;
        binds[i].buffer = buffers[i].data();
        binds[i].buffer_length = static_cast<unsigned long>(buffers[i].size());
        binds[i].length = &lengths[i];
        binds[i].is_null = &is_null[i];
        binds[i].error = &has_error[i];
    }

    if (mysql_stmt_bind_result(stmt, binds.data()) != 0)
    {
        error_code = mysql_stmt_errno(stmt);
        error = mysql_stmt_error(stmt);
        return nullptr;
    }

    std::vector<result_row> rows;
    for (;;)
    {
        const auto fetch_result = mysql_stmt_fetch(stmt);
        if (fetch_result == MYSQL_NO_DATA)
        {
            break;
        }

        if (fetch_result != 0 && fetch_result != MYSQL_DATA_TRUNCATED)
        {
            error_code = mysql_stmt_errno(stmt);
            error = mysql_stmt_error(stmt);
            return nullptr;
        }

        result_row row;
        row.cells.reserve(field_count);

        for (unsigned int i = 0; i < field_count; ++i)
        {
            result_cell cell;
            cell.is_null = (is_null[i] != 0);

            if (!cell.is_null)
            {
                if (has_error[i] != 0)
                {
                    // buffer was too small - re-fetch this column with exact length
                    buffers[i].assign(lengths[i] + 1, '\0');
                    MYSQL_BIND fetch_bind;
                    memset(&fetch_bind, 0, sizeof(fetch_bind));
                    fetch_bind.buffer_type = MYSQL_TYPE_STRING;
                    fetch_bind.buffer = buffers[i].data();
                    fetch_bind.buffer_length = static_cast<unsigned long>(buffers[i].size());
                    fetch_bind.length = &lengths[i];
                    fetch_bind.is_null = &is_null[i];

                    if (mysql_stmt_fetch_column(stmt, &fetch_bind, i, 0) != 0)
                    {
                        error_code = mysql_stmt_errno(stmt);
                        error = mysql_stmt_error(stmt);
                        return nullptr;
                    }
                }

                cell.text.assign(buffers[i].data(), lengths[i]);
            }

            row.cells.push_back(std::move(cell));
        }

        rows.push_back(std::move(row));
    }

    return build(std::move(columns), std::move(rows));
}

bool result_data::next_row()
{
    if (current_row_ + 1 >= row_count())
    {
        return false;
    }

    ++current_row_;
    return true;
}

bool result_data::rewind()
{
    current_row_ = -1;
    return true;
}

int result_data::row_count() const
{
    return static_cast<int>(rows_.size());
}

int result_data::column_count() const
{
    return static_cast<int>(columns_.size());
}

int result_data::column_index(const std::string& name) const
{
    for (size_t i = 0; i < columns_.size(); ++i)
    {
        if (columns_[i] == name)
        {
            return static_cast<int>(i);
        }
    }

    return -1;
}

const std::string& result_data::column_name(int column) const
{
    if (!is_column_valid(column))
    {
        return k_empty_string;
    }

    return columns_[static_cast<size_t>(column)];
}

bool result_data::has_current_row() const
{
    return current_row_ >= 0 && current_row_ < row_count();
}

bool result_data::is_column_valid(int column) const
{
    return column >= 0 && column < column_count();
}

bool result_data::is_null(int column) const
{
    if (!has_current_row() || !is_column_valid(column))
    {
        return true;
    }

    return rows_[static_cast<size_t>(current_row_)].cells[static_cast<size_t>(column)].is_null;
}

int result_data::read_int(int column) const
{
    if (is_null(column))
    {
        return 0;
    }

    return std::atoi(read_string(column).c_str());
}

bool result_data::read_bool(int column) const
{
    return read_int(column) != 0;
}

float result_data::read_float(int column) const
{
    if (is_null(column))
    {
        return 0.0f;
    }

    return static_cast<float>(std::atof(read_string(column).c_str()));
}

const std::string& result_data::read_string(int column) const
{
    if (!has_current_row() || !is_column_valid(column))
    {
        return k_empty_string;
    }

    return rows_[static_cast<size_t>(current_row_)].cells[static_cast<size_t>(column)].text;
}
