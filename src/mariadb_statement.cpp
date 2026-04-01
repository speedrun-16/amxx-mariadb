#include "mariadb_statement.h"

#include <cstring>

// ============================================================================
// FACTORY FUNCTION
// ============================================================================

std::shared_ptr<statement_data> create_statement(const std::shared_ptr<connection_data>& connection, const std::string& query)
{
    auto* stmt = mysql_stmt_init(connection->raw());
    if (!stmt)
    {
        connection->set_last_error("mysql_stmt_init() failed.", 0);
        return nullptr;
    }

    if (mysql_stmt_prepare(stmt, query.c_str(), static_cast<unsigned long>(query.size())) != 0)
    {
        connection->set_last_error_from_stmt(stmt);
        mysql_stmt_close(stmt);
        return nullptr;
    }

    return std::make_shared<statement_data>(connection, stmt);
}

// ============================================================================
// STATEMENT DATA
// ============================================================================

statement_data::statement_data(std::shared_ptr<connection_data> connection, MYSQL_STMT* stmt) :
    connection_(std::move(connection)),
    stmt_(stmt),
    params_(mysql_stmt_param_count(stmt_)),
    param_binds_(mysql_stmt_param_count(stmt_)),
    int_values_(mysql_stmt_param_count(stmt_), 0),
    float_values_(mysql_stmt_param_count(stmt_), 0.0f),
    string_lengths_(mysql_stmt_param_count(stmt_), 0),
    is_null_(mysql_stmt_param_count(stmt_), 0),
    string_values_(mysql_stmt_param_count(stmt_))
{
    reset_binding_storage();
}

statement_data::~statement_data()
{
    if (stmt_)
    {
        mysql_stmt_close(stmt_);
        stmt_ = nullptr;
    }
}

bool statement_data::prepare_param_slot(unsigned int index)
{
    if (index >= param_count())
    {
        set_last_error("Invalid statement parameter index.", 0);
        connection_->set_last_error(last_error(), last_error_code());
        return false;
    }

    return true;
}

bool statement_data::bind_int(unsigned int index, int value)
{
    if (!prepare_param_slot(index))
    {
        return false;
    }

    params_[index].type = stmt_param_type::t_int;
    params_[index].int_value = value;
    update_bind_slot(index);
    return true;
}

bool statement_data::bind_bool(unsigned int index, bool value)
{
    if (!prepare_param_slot(index))
    {
        return false;
    }

    params_[index].type = stmt_param_type::t_bool;
    params_[index].int_value = value ? 1 : 0;
    update_bind_slot(index);
    return true;
}

bool statement_data::bind_float(unsigned int index, float value)
{
    if (!prepare_param_slot(index))
    {
        return false;
    }

    params_[index].type = stmt_param_type::t_float;
    params_[index].float_value = value;
    update_bind_slot(index);
    return true;
}

bool statement_data::bind_string(unsigned int index, const std::string& value)
{
    if (!prepare_param_slot(index))
    {
        return false;
    }

    params_[index].type = stmt_param_type::t_string;
    params_[index].string_value = value;
    update_bind_slot(index);
    return true;
}

bool statement_data::bind_null(unsigned int index)
{
    if (!prepare_param_slot(index))
    {
        return false;
    }

    params_[index].type = stmt_param_type::t_null;
    params_[index].string_value.clear();
    update_bind_slot(index);
    return true;
}

bool statement_data::reset()
{
    mysql_stmt_free_result(stmt_);

    if (mysql_stmt_reset(stmt_) != 0)
    {
        set_last_error(mysql_stmt_error(stmt_), mysql_stmt_errno(stmt_));
        connection_->set_last_error(last_error(), last_error_code());
        return false;
    }

    for (auto& param : params_)
    {
        param = stmt_param_value{};
    }

    reset_binding_storage();
    set_last_error("", 0);
    return true;
}

void statement_data::reset_binding_storage()
{
    const auto count = param_count();
    for (unsigned int i = 0; i < count; ++i)
    {
        auto& bind = param_binds_[i];
        memset(&bind, 0, sizeof(MYSQL_BIND));
        int_values_[i] = 0;
        float_values_[i] = 0.0f;
        string_lengths_[i] = 0;
        is_null_[i] = 0;
        string_values_[i].clear();
    }

    bindings_dirty_ = true;
}

void statement_data::update_bind_slot(unsigned int index)
{
    auto& bind = param_binds_[index];
    memset(&bind, 0, sizeof(MYSQL_BIND));

    string_lengths_[index] = 0;
    is_null_[index] = 0;

    switch (params_[index].type)
    {
        case stmt_param_type::t_int:
            int_values_[index] = params_[index].int_value;
            bind.buffer_type = MYSQL_TYPE_LONG;
            bind.buffer = &int_values_[index];
            break;

        case stmt_param_type::t_bool:
            int_values_[index] = params_[index].int_value ? 1 : 0;
            bind.buffer_type = MYSQL_TYPE_LONG;
            bind.buffer = &int_values_[index];
            break;

        case stmt_param_type::t_float:
            float_values_[index] = params_[index].float_value;
            bind.buffer_type = MYSQL_TYPE_FLOAT;
            bind.buffer = &float_values_[index];
            break;

        case stmt_param_type::t_string:
            string_values_[index] = params_[index].string_value;
            string_lengths_[index] = static_cast<unsigned long>(string_values_[index].size());
            bind.buffer_type = MYSQL_TYPE_STRING;
            bind.buffer = string_values_[index].data();
            bind.buffer_length = string_lengths_[index];
            bind.length = &string_lengths_[index];
            break;

        case stmt_param_type::t_null:
            is_null_[index] = 1;
            bind.buffer_type = MYSQL_TYPE_NULL;
            bind.is_null = &is_null_[index];
            break;

        case stmt_param_type::unset:
        default:
            break;
    }

    bindings_dirty_ = true;
}

bool statement_data::bind_params_if_needed()
{
    const auto count = param_count();

    for (unsigned int i = 0; i < count; ++i)
    {
        if (params_[i].type == stmt_param_type::unset)
        {
            set_last_error("All statement parameters must be bound before execution.", 0);
            connection_->set_last_error(last_error(), last_error_code());
            return false;
        }
    }

    if (!bindings_dirty_)
    {
        return true;
    }

    if (count > 0 && mysql_stmt_bind_param(stmt_, param_binds_.data()) != 0)
    {
        set_last_error(mysql_stmt_error(stmt_), mysql_stmt_errno(stmt_));
        connection_->set_last_error(last_error(), last_error_code());
        return false;
    }

    bindings_dirty_ = false;
    return true;
}

bool statement_data::exec(int& affected_rows, int& insert_id)
{
    affected_rows = 0;
    insert_id = 0;

    mysql_stmt_free_result(stmt_);

    if (!bind_params_if_needed())
    {
        return false;
    }

    if (mysql_stmt_execute(stmt_) != 0)
    {
        set_last_error(mysql_stmt_error(stmt_), mysql_stmt_errno(stmt_));
        connection_->set_last_error(last_error(), last_error_code());
        return false;
    }

    affected_rows = static_cast<int>(mysql_stmt_affected_rows(stmt_));
    insert_id = static_cast<int>(mysql_stmt_insert_id(stmt_));
    set_last_error("", 0);
    connection_->set_last_error("", 0);
    return true;
}

std::shared_ptr<result_data> statement_data::query()
{
    mysql_stmt_free_result(stmt_);

    bool update_max_length = true;
    mysql_stmt_attr_set(stmt_, STMT_ATTR_UPDATE_MAX_LENGTH, &update_max_length);

    if (!bind_params_if_needed())
    {
        return nullptr;
    }

    if (mysql_stmt_execute(stmt_) != 0)
    {
        set_last_error(mysql_stmt_error(stmt_), mysql_stmt_errno(stmt_));
        connection_->set_last_error(last_error(), last_error_code());
        return nullptr;
    }

    if (mysql_stmt_field_count(stmt_) == 0)
    {
        set_last_error("Statement did not return a result set.", 0);
        connection_->set_last_error(last_error(), last_error_code());
        return nullptr;
    }

    if (mysql_stmt_store_result(stmt_) != 0)
    {
        set_last_error(mysql_stmt_error(stmt_), mysql_stmt_errno(stmt_));
        connection_->set_last_error(last_error(), last_error_code());
        return nullptr;
    }

    std::string error;
    unsigned int err_code = 0;
    auto result = result_data::from_stmt_result(stmt_, error, err_code);
    mysql_stmt_free_result(stmt_);

    if (!result)
    {
        set_last_error(error, err_code);
        connection_->set_last_error(last_error(), last_error_code());
        return nullptr;
    }

    set_last_error("", 0);
    connection_->set_last_error("", 0);
    return result;
}

void statement_data::set_last_error(const std::string& message, unsigned int code)
{
    last_error_ = message;
    last_error_code_ = code;
}

const std::string& statement_data::last_error() const
{
    return last_error_;
}

unsigned int statement_data::last_error_code() const
{
    return last_error_code_;
}

unsigned int statement_data::param_count() const
{
    return static_cast<unsigned int>(params_.size());
}
