module;

#include <arrow-adbc/adbc.h>
#include <nanoarrow/nanoarrow.h>

#include <cerrno>
#include <cstdlib>
#include <limits>

module dbpp.adbc;

import std;
import dbpp.types;
import dbpp.i18n;
import dbpp.redis;

namespace dbpp {

namespace {

std::optional<std::vector<std::uint8_t>> TryParseHexBytes(std::string_view text) {
  if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    text.remove_prefix(2);
  } else if (text.size() >= 2 && text[0] == '\\' && (text[1] == 'x' || text[1] == 'X')) {
    text.remove_prefix(2);
  } else {
    return std::nullopt;
  }
  if ((text.size() % 2) != 0) {
    return std::nullopt;
  }
  auto hex_nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::vector<std::uint8_t> out;
  out.reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const int hi = hex_nibble(text[i]);
    const int lo = hex_nibble(text[i + 1]);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return out;
}

ArrowType InferParamArrowType(const PreparedParamValue& param, std::string_view declared_type,
                              const ArrowSchema* hint_field) {
  if (hint_field != nullptr) {
    ArrowSchemaView view{};
    ArrowError ae;
    ArrowErrorInit(&ae);
    if (ArrowSchemaViewInit(&view, hint_field, &ae) == NANOARROW_OK &&
        view.type != NANOARROW_TYPE_NA && view.type != NANOARROW_TYPE_UNINITIALIZED) {
      switch (view.type) {
        case NANOARROW_TYPE_BOOL:
          if (param.is_null || detail::AdbcParseBool(param.text).has_value()) {
            return NANOARROW_TYPE_BOOL;
          }
          break;
        case NANOARROW_TYPE_INT8:
        case NANOARROW_TYPE_INT16:
        case NANOARROW_TYPE_INT32:
        case NANOARROW_TYPE_INT64:
          if (param.is_null || detail::AdbcParseInt64(param.text).has_value()) {
            return NANOARROW_TYPE_INT64;
          }
          break;
        case NANOARROW_TYPE_FLOAT:
        case NANOARROW_TYPE_DOUBLE:
          if (param.is_null || detail::AdbcParseDouble(param.text).has_value()) {
            return NANOARROW_TYPE_DOUBLE;
          }
          break;
        case NANOARROW_TYPE_BINARY:
        case NANOARROW_TYPE_LARGE_BINARY:
          if (param.is_null || TryParseHexBytes(param.text).has_value()) {
            return NANOARROW_TYPE_BINARY;
          }
          break;
        case NANOARROW_TYPE_TIMESTAMP: {
          if (param.is_null) {
            return NANOARROW_TYPE_TIMESTAMP;
          }
          const bool zoned = view.timezone != nullptr && view.timezone[0] != '\0';
          std::int64_t utc_sec = 0;
          std::int64_t sub_ns = 0;
          if (detail::AdbcParseTimestampInstant(param.text, zoned, utc_sec, sub_ns)) {
            return NANOARROW_TYPE_TIMESTAMP;
          }
          break;
        }
        case NANOARROW_TYPE_DATE32: {
          if (param.is_null) {
            return NANOARROW_TYPE_DATE32;
          }
          std::int32_t days = 0;
          if (detail::AdbcParseDateDays(param.text, days)) {
            return NANOARROW_TYPE_DATE32;
          }
          break;
        }
        default:
          break;
      }
    }
  }

  if (param.is_null) {
    return NANOARROW_TYPE_STRING;
  }
  const std::string lower = detail::AdbcLowerAscii(declared_type);
  if (lower.find("bool") != std::string::npos && detail::AdbcParseBool(param.text).has_value()) {
    return NANOARROW_TYPE_BOOL;
  }
  if (lower.find("int") != std::string::npos && detail::AdbcParseInt64(param.text).has_value()) {
    return NANOARROW_TYPE_INT64;
  }
  if ((lower.find("real") != std::string::npos || lower.find("double") != std::string::npos ||
       lower.find("float") != std::string::npos || lower.find("numeric") != std::string::npos ||
       lower.find("decimal") != std::string::npos) &&
      detail::AdbcParseDouble(param.text).has_value()) {
    return NANOARROW_TYPE_DOUBLE;
  }
  if ((lower.find("blob") != std::string::npos || lower.find("bytea") != std::string::npos ||
       lower.find("binary") != std::string::npos) &&
      TryParseHexBytes(param.text).has_value()) {
    return NANOARROW_TYPE_BINARY;
  }
  return NANOARROW_TYPE_STRING;
}

bool BuildParameterBatch(const std::vector<PreparedParamValue>& params,
                         const std::vector<std::string>& declared_types,
                         const ArrowSchema* hint_schema,
                         ArrowSchema& schema_out, ArrowArray& array_out,
                         std::string& error_out) {
  schema_out = ArrowSchema{};
  array_out = ArrowArray{};
  const std::size_t n = params.size();
  ArrowSchemaInit(&schema_out);
  if (ArrowSchemaSetTypeStruct(&schema_out, static_cast<std::int64_t>(n)) != NANOARROW_OK) {
    if (schema_out.release != nullptr) schema_out.release(&schema_out);
    error_out = "Failed to initialize parameter struct schema";
    return false;
  }

  std::vector<ArrowType> col_types(n, NANOARROW_TYPE_STRING);
  std::vector<ArrowTimeUnit> col_time_units(n, NANOARROW_TIME_UNIT_MICRO);
  std::vector<const char*> col_timezones(n, nullptr);
  for (std::size_t i = 0; i < n; ++i) {
    const ArrowSchema* hint_field =
        (hint_schema != nullptr && static_cast<std::int64_t>(i) < hint_schema->n_children)
            ? hint_schema->children[i]
            : nullptr;
    const std::string_view decl =
        i < declared_types.size() ? std::string_view(declared_types[i]) : std::string_view{};
    col_types[i] = InferParamArrowType(params[i], decl, hint_field);
    if (hint_field != nullptr) {
      ArrowSchemaView hv{};
      ArrowError hae;
      ArrowErrorInit(&hae);
      if (ArrowSchemaViewInit(&hv, hint_field, &hae) == NANOARROW_OK &&
          hv.type == NANOARROW_TYPE_TIMESTAMP) {
        col_time_units[i] = hv.time_unit;
        col_timezones[i] = hv.timezone;
      }
    }

    std::string param_name = "p" + std::to_string(i + 1);
    if (hint_field != nullptr && hint_field->name != nullptr && *hint_field->name != '\0') {
      param_name = hint_field->name;
    }
    ArrowSchema* child = schema_out.children[i];
    const ArrowErrorCode type_rc =
        col_types[i] == NANOARROW_TYPE_TIMESTAMP
            ? ArrowSchemaSetTypeDateTime(child, NANOARROW_TYPE_TIMESTAMP, col_time_units[i],
                                         col_timezones[i])
            : ArrowSchemaSetType(child, col_types[i]);
    if (type_rc != NANOARROW_OK ||
        ArrowSchemaSetName(child, param_name.c_str()) != NANOARROW_OK) {
      schema_out.release(&schema_out);
      schema_out = ArrowSchema{};
      error_out = "Failed to configure parameter schema field";
      return false;
    }
    child->flags |= ARROW_FLAG_NULLABLE;
  }

  ArrowError ae;
  ArrowErrorInit(&ae);
  if (ArrowArrayInitFromSchema(&array_out, &schema_out, &ae) != NANOARROW_OK ||
      ArrowArrayStartAppending(&array_out) != NANOARROW_OK) {
    if (array_out.release != nullptr) array_out.release(&array_out);
    schema_out.release(&schema_out);
    schema_out = ArrowSchema{};
    error_out = detail::AdbcDecodeSystemTextToUtf8(ae.message);
    return false;
  }

  for (std::size_t i = 0; i < n; ++i) {
    ArrowArray* child = array_out.children[i];
    if (params[i].is_null) {
      if (ArrowArrayAppendNull(child, 1) != NANOARROW_OK) {
        array_out.release(&array_out);
        schema_out.release(&schema_out);
        error_out = "Failed to append NULL parameter";
        return false;
      }
      continue;
    }
    ArrowErrorCode rc = NANOARROW_OK;
    switch (col_types[i]) {
      case NANOARROW_TYPE_BOOL: {
        const auto b = detail::AdbcParseBool(params[i].text);
        rc = ArrowArrayAppendInt(child, b.value_or(false) ? 1 : 0);
        break;
      }
      case NANOARROW_TYPE_INT64: {
        const auto iv = detail::AdbcParseInt64(params[i].text);
        rc = ArrowArrayAppendInt(child, iv.value_or(0));
        break;
      }
      case NANOARROW_TYPE_DOUBLE: {
        const auto dv = detail::AdbcParseDouble(params[i].text);
        rc = ArrowArrayAppendDouble(child, dv.value_or(0.0));
        break;
      }
      case NANOARROW_TYPE_TIMESTAMP: {
        const bool zoned = col_timezones[i] != nullptr && col_timezones[i][0] != '\0';
        std::int64_t utc_sec = 0;
        std::int64_t sub_ns = 0;
        if (!detail::AdbcParseTimestampInstant(params[i].text, zoned, utc_sec, sub_ns)) {
          rc = EINVAL;
          break;
        }
        std::int64_t ticks = utc_sec;
        auto scale_ticks = [&](std::int64_t mul, std::int64_t div) -> bool {
          if (utc_sec > std::numeric_limits<std::int64_t>::max() / mul - 1 ||
              utc_sec < std::numeric_limits<std::int64_t>::min() / mul + 1) {
            return false;
          }
          ticks = utc_sec * mul + sub_ns / div;
          return true;
        };
        bool ok = true;
        switch (col_time_units[i]) {
          case NANOARROW_TIME_UNIT_SECOND:
            break;
          case NANOARROW_TIME_UNIT_MILLI:
            ok = scale_ticks(1'000LL, 1'000'000LL);
            break;
          case NANOARROW_TIME_UNIT_MICRO:
            ok = scale_ticks(1'000'000LL, 1'000LL);
            break;
          case NANOARROW_TIME_UNIT_NANO:
            ok = scale_ticks(1'000'000'000LL, 1LL);
            break;
        }
        rc = ok ? ArrowArrayAppendInt(child, ticks) : EOVERFLOW;
        break;
      }
      case NANOARROW_TYPE_DATE32: {
        std::int32_t days = 0;
        if (!detail::AdbcParseDateDays(params[i].text, days)) {
          rc = EINVAL;
          break;
        }
        rc = ArrowArrayAppendInt(child, days);
        break;
      }
      case NANOARROW_TYPE_BINARY: {
        const auto bytes = TryParseHexBytes(params[i].text);
        if (bytes.has_value()) {
          ArrowBufferView bv;
          bv.data.as_uint8 = bytes->data();
          bv.size_bytes = static_cast<std::int64_t>(bytes->size());
          rc = ArrowArrayAppendBytes(child, bv);
        } else {
          rc = ArrowArrayAppendString(
              child, ArrowStringView{params[i].text.data(),
                                     static_cast<std::int64_t>(params[i].text.size())});
        }
        break;
      }
      default:
        rc = ArrowArrayAppendString(
            child, ArrowStringView{params[i].text.data(),
                                   static_cast<std::int64_t>(params[i].text.size())});
        break;
    }
    if (rc != NANOARROW_OK) {
      array_out.release(&array_out);
      schema_out.release(&schema_out);
      error_out = "Failed to append parameter value";
      return false;
    }
  }

  ArrowErrorInit(&ae);
  if (ArrowArrayFinishElement(&array_out) != NANOARROW_OK ||
      ArrowArrayFinishBuildingDefault(&array_out, &ae) != NANOARROW_OK) {
    array_out.release(&array_out);
    schema_out.release(&schema_out);
    error_out = detail::AdbcDecodeSystemTextToUtf8(ae.message);
    return false;
  }
  return true;
}

} // namespace

namespace detail {

std::optional<std::int64_t> AdbcParseInt64(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  std::int64_t value = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

std::optional<double> AdbcParseDouble(std::string_view text) {
  if (text.empty() || std::isspace(static_cast<unsigned char>(text.front())) ||
      std::isspace(static_cast<unsigned char>(text.back()))) {
    return std::nullopt;
  }
  const std::string copy(text);
  errno = 0;
  char* end = nullptr;
  const double value = std::strtod(copy.c_str(), &end);
  if (errno == ERANGE || end != copy.c_str() + copy.size() || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

std::optional<bool> AdbcParseBool(std::string_view text) {
  const std::string lower = AdbcLowerAscii(text);
  if (lower == "true" || lower == "1") {
    return true;
  }
  if (lower == "false" || lower == "0") {
    return false;
  }
  return std::nullopt;
}

} // namespace detail

bool ExecutePreparedNonQuery(AdbcSession& session, std::string_view sql,
                             const std::vector<PreparedParamValue>& params,
                             const std::vector<std::string>& declared_types,
                             AdbcQueryResult& out, int& status_out) {
  out = AdbcQueryResult{};
  status_out = ADBC_STATUS_OK;
  if (!session.connection_created) {
    status_out = ADBC_STATUS_INVALID_STATE;
    out.error = Tr("Connection is closed", "连接已关闭");
    return false;
  }
  if (session.read_only) {
    status_out = ADBC_STATUS_UNAUTHORIZED;
    out.error = ReadOnlyWriteBlockedMessage();
    return false;
  }

  detail::AdbcErrorGuard error;
  detail::AdbcStatementGuard stmt;
  AdbcStatusCode st = AdbcStatementNew(&session.connection, &stmt.statement, error.Ptr());
  status_out = st;
  if (st != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    out.error = cap.message;
    out.sqlstate = cap.sqlstate;
    out.vendor_code = cap.vendor_code;
    return false;
  }
  stmt.created = true;

  const std::string sql_copy(sql);
  st = AdbcStatementSetSqlQuery(&stmt.statement, sql_copy.c_str(), error.Ptr());
  status_out = st;
  if (st != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    out.error = cap.message;
    out.sqlstate = cap.sqlstate;
    return false;
  }

  st = AdbcStatementPrepare(&stmt.statement, error.Ptr());
  status_out = st;
  if (st == ADBC_STATUS_NOT_IMPLEMENTED) {
    return false;
  }
  if (st != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    out.error = cap.message;
    out.sqlstate = cap.sqlstate;
    return false;
  }

  detail::AdbcSchemaHolder param_hint;
  const ArrowSchema* hint_ptr = nullptr;
  if (AdbcStatementGetParameterSchema(&stmt.statement, &param_hint.schema, error.Ptr()) ==
      ADBC_STATUS_OK) {
    hint_ptr = &param_hint.schema;
  } else {
    error.Reset();
  }

  ArrowSchema bind_schema{};
  ArrowArray bind_array{};
  std::string build_err;
  if (!BuildParameterBatch(params, declared_types, hint_ptr, bind_schema, bind_array, build_err)) {
    status_out = ADBC_STATUS_INVALID_ARGUMENT;
    out.error = std::move(build_err);
    return false;
  }

  st = AdbcStatementBind(&stmt.statement, &bind_array, &bind_schema, error.Ptr());
  status_out = st;
  if (st != ADBC_STATUS_OK) {
    if (bind_array.release != nullptr) bind_array.release(&bind_array);
    if (bind_schema.release != nullptr) bind_schema.release(&bind_schema);
    if (st == ADBC_STATUS_NOT_IMPLEMENTED) {
      return false;
    }
    const auto cap = error.TakeDetailed();
    out.error = cap.message;
    out.sqlstate = cap.sqlstate;
    return false;
  }

  std::int64_t affected = -1;
  detail::AdbcRegisterActiveStatement(session, &stmt.statement);
  st = AdbcStatementExecuteQuery(&stmt.statement, nullptr, &affected, error.Ptr());
  detail::AdbcClearActiveStatement(session, &stmt.statement);
  status_out = st;
  if (st != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    out.error = cap.message;
    out.sqlstate = cap.sqlstate;
    out.vendor_code = cap.vendor_code;
    out.error_details = cap.details;
    out.cancelled = (st == ADBC_STATUS_CANCELLED);
    return true;
  }
  out.affected = affected;
  return true;
}

AdbcParamSchemaResult InspectStatementParametersBlocking(AdbcSession& session, std::string sql) {
  AdbcParamSchemaResult result;
  if (session.IsClosed()) {
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }
  if (session.dialect == Dialect::Redis || !session.connection_created) {
    return result;
  }

  detail::AdbcErrorGuard error;
  detail::AdbcStatementGuard stmt;
  AdbcStatusCode status = AdbcStatementNew(&session.connection, &stmt.statement, error.Ptr());
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    return result;
  }
  stmt.created = true;

  status = AdbcStatementSetSqlQuery(&stmt.statement, sql.c_str(), error.Ptr());
  if (status != ADBC_STATUS_OK) {
    result.error = error.Take();
    return result;
  }
  status = AdbcStatementPrepare(&stmt.statement, error.Ptr());
  if (status == ADBC_STATUS_NOT_IMPLEMENTED) {
    return result;
  }
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    return result;
  }

  detail::AdbcSchemaHolder param_schema;
  status = AdbcStatementGetParameterSchema(&stmt.statement, &param_schema.schema, error.Ptr());
  if (status == ADBC_STATUS_NOT_IMPLEMENTED) {
    return result;
  }
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    return result;
  }

  result.supported = true;
  std::vector<ColumnMeta> cols;
  detail::AdbcPopulateColumnsFromArrowSchema(param_schema.schema, nullptr, cols);
  result.parameters.reserve(cols.size());
  for (std::size_t i = 0; i < cols.size(); ++i) {
    result.parameters.push_back(QueryParameterMeta{
        .name = cols[i].name.empty() ? ("$" + std::to_string(i + 1)) : cols[i].name,
        .type = cols[i].type,
        .nullable = cols[i].nullable,
        .ordinal = i + 1,
    });
  }
  return result;
}

AdbcQueryResult ExecutePreparedBlocking(AdbcSession& session, std::string sql,
                                        const std::vector<PreparedParamValue>& params,
                                        bool want_rows, std::size_t max_rows) {
  AdbcQueryResult result;
  if (session.IsClosed() || !session.connection_created) {
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }
  const auto started = std::chrono::steady_clock::now();
  const auto elapsed = [started]() -> std::int64_t {
    return std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - started)
               .count());
  };

  session.cancel_requested.store(false, std::memory_order_release);
  if (session.read_only && !IsPureReadQuery(session.dialect, sql)) {
    result.error = ReadOnlyWriteBlockedMessage();
    result.elapsed_us = elapsed();
    return result;
  }
  detail::AdbcErrorGuard error;
  detail::AdbcStatementGuard stmt;
  AdbcStatusCode status = AdbcStatementNew(&session.connection, &stmt.statement, error.Ptr());
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.sqlstate = cap.sqlstate;
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    result.elapsed_us = elapsed();
    return result;
  }
  stmt.created = true;

  status = AdbcStatementSetSqlQuery(&stmt.statement, sql.c_str(), error.Ptr());
  if (status == ADBC_STATUS_OK) {
    status = AdbcStatementPrepare(&stmt.statement, error.Ptr());
  }
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.sqlstate = cap.sqlstate;
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    result.elapsed_us = elapsed();
    return result;
  }

  detail::AdbcSchemaHolder hint;
  const ArrowSchema* hint_ptr = nullptr;
  if (AdbcStatementGetParameterSchema(&stmt.statement, &hint.schema, error.Ptr()) ==
      ADBC_STATUS_OK) {
    hint_ptr = &hint.schema;
  } else {
    error.Reset();
  }

  if (!params.empty()) {
    ArrowSchema bind_schema{};
    ArrowArray bind_array{};
    std::string build_err;
    if (!BuildParameterBatch(params, {}, hint_ptr, bind_schema, bind_array, build_err)) {
      result.error = std::move(build_err);
      result.elapsed_us = elapsed();
      return result;
    }
    status = AdbcStatementBind(&stmt.statement, &bind_array, &bind_schema, error.Ptr());
    if (status != ADBC_STATUS_OK) {
      if (bind_array.release != nullptr) bind_array.release(&bind_array);
      if (bind_schema.release != nullptr) bind_schema.release(&bind_schema);
      const auto cap = error.TakeDetailed();
      result.error = cap.message;
      result.sqlstate = cap.sqlstate;
      result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
      result.elapsed_us = elapsed();
      return result;
    }
  }

  detail::AdbcStreamHolder stream;
  std::int64_t affected = -1;
  detail::AdbcRegisterActiveStatement(session, &stmt.statement);
  status = AdbcStatementExecuteQuery(&stmt.statement, want_rows ? &stream.stream : nullptr,
                                     &affected, error.Ptr());
  detail::AdbcClearActiveStatement(session, &stmt.statement);
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.sqlstate = cap.sqlstate;
    result.vendor_code = cap.vendor_code;
    result.error_details = cap.details;
    result.cancelled = (status == ADBC_STATUS_CANCELLED) ||
                       session.cancel_requested.load(std::memory_order_acquire);
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    result.elapsed_us = elapsed();
    return result;
  }
  result.affected = affected;

  if (want_rows) {
    (void)detail::AdbcConsumeStreamRows(stream.stream, max_rows, &session, result);
  }
  if (result.error.empty()) {
    session.alive.store(true, std::memory_order_release);
  }
  result.elapsed_us = elapsed();
  return result;
}

} // namespace dbpp
