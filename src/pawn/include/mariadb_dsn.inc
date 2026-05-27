#if defined _mariadb_dsn_included
    #endinput
#endif
#define _mariadb_dsn_included

#pragma semicolon 1

#include <mariadb>

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

stock mdsn_set_error(error[], error_len, const format[], any:...)
{
    if (error_len < 0)
        return;

    vformat(error, error_len, format, 4);
}

stock mdsn_find_char_range(const input[], needle, start, end_exclusive = -1)
{
    if (end_exclusive < 0)
        end_exclusive = strlen(input);

    for (new i = start; i < end_exclusive && input[i] != EOS; i++)
    {
        if (input[i] == needle)
            return i;
    }

    return -1;
}

stock mdsn_find_last_char_range(const input[], needle, start, end_exclusive = -1)
{
    if (end_exclusive < 0)
        end_exclusive = strlen(input);

    for (new i = end_exclusive - 1; i >= start; i--)
    {
        if (input[i] == needle)
            return i;
    }

    return -1;
}

stock bool:mdsn_is_hex_digit(ch)
{
    return (ch >= '0' && ch <= '9')
        || (ch >= 'a' && ch <= 'f')
        || (ch >= 'A' && ch <= 'F');
}

stock mdsn_hex_value(ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';

    if (ch >= 'a' && ch <= 'f')
        return 10 + (ch - 'a');

    if (ch >= 'A' && ch <= 'F')
        return 10 + (ch - 'A');

    return -1;
}

stock bool:mdsn_copy_decoded_range(const input[], start, end_exclusive, output[], output_len, error[], error_len)
{
    new out_idx;

    if (output_len < 0)
    {
        mdsn_set_error(error, error_len, "output buffer length must be non-negative");
        return false;
    }

    output[0] = EOS;

    for (new i = start; i < end_exclusive && input[i] != EOS; i++)
    {
        new ch = input[i];

        if (ch == '%')
        {
            if (i + 2 >= end_exclusive || !mdsn_is_hex_digit(input[i + 1]) || !mdsn_is_hex_digit(input[i + 2]))
            {
                mdsn_set_error(error, error_len, "invalid percent-encoding in DSN");
                return false;
            }

            ch = (mdsn_hex_value(input[i + 1]) << 4) | mdsn_hex_value(input[i + 2]);
            i += 2;
        }

        if (out_idx >= output_len)
        {
            mdsn_set_error(error, error_len, "decoded DSN value exceeds output buffer");
            return false;
        }

        output[out_idx++] = ch;
    }

    output[out_idx] = EOS;
    return true;
}

stock bool:mdsn_parse_int_range(const input[], start, end_exclusive, &value)
{
    if (start >= end_exclusive)
        return false;

    new buffer[16];
    new length;

    for (new i = start; i < end_exclusive && input[i] != EOS; i++)
    {
        if (input[i] < '0' || input[i] > '9' || length >= charsmax(buffer))
            return false;

        buffer[length++] = input[i];
    }

    if (length == 0)
        return false;

    buffer[length] = EOS;
    value = str_to_num(buffer);
    return true;
}

stock bool:mdsn_parse_bool_value(const value[], &bool:result)
{
    if (equali(value, "1") || equali(value, "true") || equali(value, "yes") || equali(value, "on"))
    {
        result = true;
        return true;
    }

    if (equali(value, "0") || equali(value, "false") || equali(value, "no") || equali(value, "off"))
    {
        result = false;
        return true;
    }

    return false;
}

// ============================================================================
// PUBLIC API
// ============================================================================

/**
 * @brief Parses a practical MariaDB/MySQL DSN.
 *
 * Supported forms:
 *   mysql://user:pass@host:3306/database
 *   mysql://user@host/database
 *   mysql://host/database
 *   mariadb://user:pass@host/database?charset=utf8mb4&timeout_ms=100&auto_reconnect=1
 *
 * The passed output values act as defaults. Any field omitted from the DSN
 * remains unchanged.
 *
 * Unsupported by design:
 *   IPv6 host literals
 *   Full RFC URI edge-case handling beyond simple percent-decoding
 *
 * @return true on success, false on parse failure
 */
stock bool:mariadb_parse_dsn(
    const dsn[],
    host[], host_len,
    &port,
    user[], user_len,
    password[], password_len,
    database[], database_len,
    charset[], charset_len,
    &timeout_ms,
    &bool:auto_reconnect,
    error[], error_len
)
{
    new main_end;
    new scheme_end;

    error[0] = EOS;

    if (!dsn[0])
    {
        mdsn_set_error(error, error_len, "empty DSN");
        return false;
    }

    if (equali(dsn, "mysql://", 8))
        scheme_end = 8;
    else if (equali(dsn, "mariadb://", 10))
        scheme_end = 10;
    else
    {
        mdsn_set_error(error, error_len, "DSN must start with mysql:// or mariadb://");
        return false;
    }

    new query_start = mdsn_find_char_range(dsn, '?', scheme_end);
    main_end = (query_start >= 0) ? query_start : strlen(dsn);

    new slash_pos = mdsn_find_char_range(dsn, '/', scheme_end, main_end);
    if (slash_pos < 0)
    {
        mdsn_set_error(error, error_len, "DSN must include /database");
        return false;
    }

    if (slash_pos + 1 >= main_end)
    {
        mdsn_set_error(error, error_len, "DSN database name cannot be empty");
        return false;
    }

    new authority_start = scheme_end;
    new at_pos = mdsn_find_last_char_range(dsn, '@', scheme_end, slash_pos);
    if (at_pos >= 0)
    {
        new colon_pos = mdsn_find_char_range(dsn, ':', scheme_end, at_pos);
        if (colon_pos >= 0)
        {
            if (!mdsn_copy_decoded_range(dsn, scheme_end, colon_pos, user, user_len, error, error_len))
                return false;

            if (!mdsn_copy_decoded_range(dsn, colon_pos + 1, at_pos, password, password_len, error, error_len))
                return false;
        }
        else
        {
            if (!mdsn_copy_decoded_range(dsn, scheme_end, at_pos, user, user_len, error, error_len))
                return false;
        }

        authority_start = at_pos + 1;
    }

    if (authority_start >= slash_pos)
    {
        mdsn_set_error(error, error_len, "DSN host cannot be empty");
        return false;
    }

    new port_sep = mdsn_find_last_char_range(dsn, ':', authority_start, slash_pos);
    if (port_sep >= 0)
    {
        if (!mdsn_copy_decoded_range(dsn, authority_start, port_sep, host, host_len, error, error_len))
            return false;

        if (!mdsn_parse_int_range(dsn, port_sep + 1, slash_pos, port) || port <= 0 || port > 65535)
        {
            mdsn_set_error(error, error_len, "DSN port must be between 1 and 65535");
            return false;
        }
    }
    else
    {
        if (!mdsn_copy_decoded_range(dsn, authority_start, slash_pos, host, host_len, error, error_len))
            return false;
    }

    if (!host[0])
    {
        mdsn_set_error(error, error_len, "DSN host cannot be empty");
        return false;
    }

    if (!mdsn_copy_decoded_range(dsn, slash_pos + 1, main_end, database, database_len, error, error_len))
        return false;

    if (!database[0])
    {
        mdsn_set_error(error, error_len, "DSN database name cannot be empty");
        return false;
    }

    if (query_start < 0 || dsn[query_start + 1] == EOS)
        return true;

    new param_start = query_start + 1;
    new dsn_len = strlen(dsn);

    while (param_start < dsn_len)
    {
        new param_end = mdsn_find_char_range(dsn, '&', param_start);
        if (param_end < 0)
            param_end = dsn_len;

        if (param_end > param_start)
        {
            new eq_pos = mdsn_find_char_range(dsn, '=', param_start, param_end);
            new key[32];
            new value[128];

            if (eq_pos >= 0)
            {
                if (!mdsn_copy_decoded_range(dsn, param_start, eq_pos, key, charsmax(key), error, error_len))
                    return false;

                if (!mdsn_copy_decoded_range(dsn, eq_pos + 1, param_end, value, charsmax(value), error, error_len))
                    return false;
            }
            else
            {
                if (!mdsn_copy_decoded_range(dsn, param_start, param_end, key, charsmax(key), error, error_len))
                    return false;

                value[0] = EOS;
            }

            if (equali(key, "charset"))
            {
                if (value[0] && !mdsn_copy_decoded_range(value, 0, strlen(value), charset, charset_len, error, error_len))
                    return false;
            }
            else if (equali(key, "timeout_ms"))
            {
                if (!mdsn_parse_int_range(value, 0, strlen(value), timeout_ms) || timeout_ms <= 0)
                {
                    mdsn_set_error(error, error_len, "DSN timeout_ms must be a positive integer");
                    return false;
                }
            }
            else if (equali(key, "auto_reconnect"))
            {
                if (!mdsn_parse_bool_value(value, auto_reconnect))
                {
                    mdsn_set_error(error, error_len, "DSN auto_reconnect must be one of: 0, 1, true, false, yes, no, on, off");
                    return false;
                }
            }
        }

        param_start = param_end + 1;
    }

    return true;
}

/**
 * @brief Convenience wrapper that parses a DSN and opens a synchronous connection.
 *
 * This uses default MariaDB options for fields omitted from the DSN.
 *
 * @return Connection handle on success, invalid_mariadb_connection on failure
 */
stock mariadb_connection:mariadb_connect_dsn(const dsn[], error[], maxlen, &error_code = 0)
{
    new host[128] = "127.0.0.1";
    new user[64] = "root";
    new password[128];
    new database[128];
    new charset[32] = MARIADB_DEFAULT_CHARSET;
    new port = MARIADB_DEFAULT_PORT;
    new timeout_ms = MARIADB_DEFAULT_TIMEOUT_MS;
    new bool:auto_reconnect = true;

    if (!mariadb_parse_dsn(
        dsn,
        host, charsmax(host),
        port,
        user, charsmax(user),
        password, charsmax(password),
        database, charsmax(database),
        charset, charsmax(charset),
        timeout_ms,
        auto_reconnect,
        error, maxlen
    ))
    {
        error_code = 0;
        return invalid_mariadb_connection;
    }

    return mariadb_connect(
        host,
        user,
        password,
        database,
        .port = port,
        .charset = charset,
        .timeout_ms = timeout_ms,
        .auto_reconnect = auto_reconnect,
        .error = error,
        .maxlen = maxlen,
        .error_code = error_code
    );
}
