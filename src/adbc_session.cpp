#include "adbc_session.h"

#include <arrow-adbc/adbc.h>
#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dbpp {

namespace {

AdbcError MakeError() {
  AdbcError error = ADBC_ERROR_INIT;
  return error;
}

void ReleaseError(AdbcError& error) {
  if (error.release != nullptr) {
    error.release(&error);
    error.release = nullptr;
  }
}

std::string TakeError(AdbcError& error) {
  std::string message = error.message != nullptr ? error.message : "unknown ADBC error";
  ReleaseError(error);
  return message;
}

std::string LowerAscii(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const char c : text) {
    result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return result;
}

bool ContainsLower(std::string_view haystack, std::string_view needle) {
  return LowerAscii(haystack).find(LowerAscii(needle)) != std::string::npos;
}

Dialect DetectDialect(std::string_view driver_path, std::string_view uri) {
  const std::string key = LowerAscii(std::string(driver_path) + " " + std::string(uri));
  if (key.find("sqlite") != std::string::npos) {
    return Dialect::Sqlite;
  }
  if (key.find("postgres") != std::string::npos) {
    return Dialect::Postgresql;
  }
  if (key.find("mysql") != std::string::npos || key.find("mariadb") != std::string::npos) {
    return Dialect::Mysql;
  }
  if (key.find("duckdb") != std::string::npos) {
    return Dialect::Duckdb;
  }
  return Dialect::Generic;
}

struct StreamHolder {
  ArrowArrayStream stream{};
  ~StreamHolder() {
    if (stream.release != nullptr) {
      stream.release(&stream);
    }
  }
};

struct SchemaHolder {
  ArrowSchema schema{};
  ~SchemaHolder() {
    if (schema.release != nullptr) {
      schema.release(&schema);
    }
  }
};

struct FieldInfo {
  std::string name;
  ArrowType type = NANOARROW_TYPE_UNINITIALIZED;
  ArrowTimeUnit time_unit = NANOARROW_TIME_UNIT_SECOND;
  std::int32_t decimal_bitwidth = 0;
  std::int32_t decimal_precision = 0;
  std::int32_t decimal_scale = 0;
};

std::string StreamError(ArrowArrayStream& stream) {
  const char* message = stream.get_last_error != nullptr ? stream.get_last_error(&stream) : nullptr;
  return message != nullptr ? message : "Arrow stream error";
}

std::string FormatDouble(double value) {
  if (std::isnan(value)) {
    return "NaN";
  }
  if (std::isinf(value)) {
    return value < 0 ? "-Inf" : "Inf";
  }
  std::ostringstream stream;
  stream << std::setprecision(15) << value;
  return stream.str();
}

std::string FormatDate(std::chrono::sys_days days) {
  const std::chrono::year_month_day value(days);
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u",
                static_cast<int>(value.year()),
                static_cast<unsigned>(value.month()),
                static_cast<unsigned>(value.day()));
  return buffer;
}

std::string FormatTime(std::chrono::seconds seconds) {
  const auto day_seconds = static_cast<std::int64_t>(seconds.count());
  const std::int64_t normalized =
      ((day_seconds % 86400) + 86400) % 86400;
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld",
                static_cast<long long>(normalized / 3600),
                static_cast<long long>((normalized % 3600) / 60),
                static_cast<long long>(normalized % 60));
  return buffer;
}

std::string FormatTimestamp(std::int64_t value, ArrowTimeUnit unit) {
  std::int64_t seconds = value;
  switch (unit) {
    case NANOARROW_TIME_UNIT_MILLI: seconds = value / 1000; break;
    case NANOARROW_TIME_UNIT_MICRO: seconds = value / 1'000'000; break;
    case NANOARROW_TIME_UNIT_NANO: seconds = value / 1'000'000'000; break;
    case NANOARROW_TIME_UNIT_SECOND: break;
  }
  const auto time = std::chrono::sys_seconds{std::chrono::seconds{seconds}};
  const auto days = std::chrono::floor<std::chrono::days>(time);
  return FormatDate(days) + " " + FormatTime(std::chrono::floor<std::chrono::seconds>(time) - days);
}

std::string TimeUnitLabel(ArrowTimeUnit unit) {
  switch (unit) {
    case NANOARROW_TIME_UNIT_MILLI: return "ms";
    case NANOARROW_TIME_UNIT_MICRO: return "us";
    case NANOARROW_TIME_UNIT_NANO: return "ns";
    case NANOARROW_TIME_UNIT_SECOND: return "s";
  }
  return "s";
}

CellValue ExtractCell(const ArrowArrayView& view, int64_t row, const FieldInfo& field) {
  CellValue cell;
  if (ArrowArrayViewIsNull(&view, row) != 0 || view.storage_type == NANOARROW_TYPE_NA) {
    cell.text = "NULL";
    cell.is_null = true;
    return cell;
  }

  const ArrowType storage = view.storage_type;
  switch (storage) {
    case NANOARROW_TYPE_BOOL:
      cell.text = ArrowArrayViewGetIntUnsafe(&view, row) != 0 ? "true" : "false";
      return cell;
    case NANOARROW_TYPE_INT8:
    case NANOARROW_TYPE_INT16:
    case NANOARROW_TYPE_INT32:
    case NANOARROW_TYPE_INT64:
      cell.text = std::to_string(ArrowArrayViewGetIntUnsafe(&view, row));
      return cell;
    case NANOARROW_TYPE_UINT8:
    case NANOARROW_TYPE_UINT16:
    case NANOARROW_TYPE_UINT32:
    case NANOARROW_TYPE_UINT64:
      cell.text = std::to_string(ArrowArrayViewGetUIntUnsafe(&view, row));
      return cell;
    case NANOARROW_TYPE_FLOAT:
    case NANOARROW_TYPE_DOUBLE:
    case NANOARROW_TYPE_HALF_FLOAT:
      cell.text = FormatDouble(ArrowArrayViewGetDoubleUnsafe(&view, row));
      return cell;
    case NANOARROW_TYPE_STRING:
    case NANOARROW_TYPE_LARGE_STRING:
    case NANOARROW_TYPE_STRING_VIEW: {
      const ArrowStringView value = ArrowArrayViewGetStringUnsafe(&view, row);
      if (value.data != nullptr) {
        cell.text.assign(value.data, static_cast<std::size_t>(value.size_bytes));
      }
      return cell;
    }
    case NANOARROW_TYPE_BINARY:
    case NANOARROW_TYPE_LARGE_BINARY:
    case NANOARROW_TYPE_FIXED_SIZE_BINARY:
    case NANOARROW_TYPE_BINARY_VIEW: {
      const ArrowBufferView value = ArrowArrayViewGetBytesUnsafe(&view, row);
      cell.text = "<blob " + std::to_string(value.size_bytes) + " bytes>";
      return cell;
    }
    default:
      break;
  }

  switch (field.type) {
    case NANOARROW_TYPE_DATE32: {
      const auto days = std::chrono::sys_days{
          std::chrono::days{ArrowArrayViewGetIntUnsafe(&view, row)}};
      cell.text = FormatDate(days);
      return cell;
    }
    case NANOARROW_TYPE_DATE64: {
      const auto milliseconds = std::chrono::milliseconds{
          ArrowArrayViewGetIntUnsafe(&view, row)};
      cell.text = FormatDate(std::chrono::sys_days{
          std::chrono::duration_cast<std::chrono::days>(milliseconds)});
      return cell;
    }
    case NANOARROW_TYPE_TIMESTAMP:
      cell.text = FormatTimestamp(ArrowArrayViewGetIntUnsafe(&view, row), field.time_unit);
      return cell;
    case NANOARROW_TYPE_TIME32:
    case NANOARROW_TYPE_TIME64: {
      std::int64_t value = ArrowArrayViewGetIntUnsafe(&view, row);
      switch (field.time_unit) {
        case NANOARROW_TIME_UNIT_MILLI: value /= 1000; break;
        case NANOARROW_TIME_UNIT_MICRO: value /= 1'000'000; break;
        case NANOARROW_TIME_UNIT_NANO: value /= 1'000'000'000; break;
        case NANOARROW_TIME_UNIT_SECOND: break;
      }
      cell.text = FormatTime(std::chrono::seconds{value});
      return cell;
    }
    case NANOARROW_TYPE_DURATION:
      cell.text = std::to_string(ArrowArrayViewGetIntUnsafe(&view, row)) + " " +
                  TimeUnitLabel(field.time_unit);
      return cell;
    case NANOARROW_TYPE_DECIMAL32:
    case NANOARROW_TYPE_DECIMAL64:
    case NANOARROW_TYPE_DECIMAL128:
    case NANOARROW_TYPE_DECIMAL256: {
      ArrowDecimal decimal;
      ArrowDecimalInit(&decimal, field.decimal_bitwidth, field.decimal_precision,
                       field.decimal_scale);
      ArrowArrayViewGetDecimalUnsafe(&view, row, &decimal);
      ArrowBuffer buffer;
      ArrowBufferInit(&buffer);
      if (ArrowDecimalAppendStringToBuffer(&decimal, &buffer) == NANOARROW_OK) {
        cell.text.assign(reinterpret_cast<const char*>(buffer.data), buffer.size_bytes);
      } else {
        cell.text = "<decimal>";
      }
      ArrowBufferReset(&buffer);
      return cell;
    }
    default:
      break;
  }

  cell.text = std::string("<") + ArrowTypeString(field.type) + ">";
  return cell;
}

std::string StringAt(const ArrowArrayView* view, int64_t index) {
  if (view == nullptr || index < 0 || index >= view->length ||
      ArrowArrayViewIsNull(view, index) != 0) {
    return {};
  }
  const ArrowStringView value = ArrowArrayViewGetStringUnsafe(view, index);
  if (value.data == nullptr) {
    return {};
  }
  return std::string(value.data, static_cast<std::size_t>(value.size_bytes));
}

int64_t ListEnd(const ArrowArrayView* list, int64_t index) {
  if (list == nullptr || list->n_children == 0) {
    return 0;
  }
  if (index + 1 < list->length) {
    return ArrowArrayViewListChildOffset(list, index + 1);
  }
  return list->children[0]->length;
}

int64_t ListStart(const ArrowArrayView* list, int64_t index) {
  if (list == nullptr || list->n_children == 0 || index < 0 || index >= list->length) {
    return 0;
  }
  return ArrowArrayViewListChildOffset(list, index);
}


} // namespace

std::string DefaultSqliteDriverPath() {
  if (const char* environment = std::getenv("DBPP_SQLITE_DRIVER"); environment != nullptr &&
                                                                    *environment != '\0') {
    return environment;
  }
#ifdef DBPP_SQLITE_DRIVER_PATH
  return DBPP_SQLITE_DRIVER_PATH;
#else
  return "adbc_driver_sqlite";
#endif
}

AdbcSession::~AdbcSession() {
  AdbcError error = MakeError();
  if (connection_created) {
    AdbcConnectionRelease(&connection, &error);
    ReleaseError(error);
  }
  if (database_created) {
    AdbcDatabaseRelease(&database, &error);
    ReleaseError(error);
  }
}

AdbcOpenResult OpenAdbcBlocking(const ConnectionRequest& request) {
  AdbcOpenResult result;
  auto session = std::make_shared<AdbcSession>();
  AdbcError error = MakeError();

  const auto fail = [&](std::string_view operation) -> AdbcOpenResult {
    result.error = std::string(operation) + ": " + TakeError(error);
    result.session.reset();
    return result;
  };

  if (AdbcDatabaseNew(&session->database, &error) != ADBC_STATUS_OK) {
    return fail("创建 ADBC database");
  }
  session->database_created = true;

  if (AdbcDatabaseSetOption(&session->database, "driver",
                            request.driver_path.c_str(), &error) != ADBC_STATUS_OK) {
    return fail("设置 ADBC driver");
  }
  if (!request.uri.empty() &&
      AdbcDatabaseSetOption(&session->database, ADBC_OPTION_URI, request.uri.c_str(),
                            &error) != ADBC_STATUS_OK) {
    return fail("设置 URI");
  }

  const Dialect dialect = DetectDialect(request.driver_path, request.uri);
  if (dialect == Dialect::Sqlite && !request.uri.empty() &&
      request.uri.rfind("file:", 0) != 0 && request.uri.find("://") == std::string::npos) {
    std::error_code filesystem_error;
    const std::filesystem::path database_path(request.uri);
    if (database_path.has_parent_path()) {
      std::filesystem::create_directories(database_path.parent_path(), filesystem_error);
    }
  }
  if (!request.username.empty() &&
      AdbcDatabaseSetOption(&session->database, "username", request.username.c_str(),
                            &error) != ADBC_STATUS_OK) {
    return fail("设置用户名");
  }
  if (!request.password.empty() &&
      AdbcDatabaseSetOption(&session->database, "password", request.password.c_str(),
                            &error) != ADBC_STATUS_OK) {
    return fail("设置密码");
  }
  if (AdbcDatabaseInit(&session->database, &error) != ADBC_STATUS_OK) {
    return fail("初始化数据库");
  }

  if (AdbcConnectionNew(&session->connection, &error) != ADBC_STATUS_OK) {
    return fail("创建连接");
  }
  session->connection_created = true;
  if (AdbcConnectionInit(&session->connection, &session->database, &error) !=
      ADBC_STATUS_OK) {
    return fail("初始化连接");
  }

  session->dialect = dialect;
  session->initialized = true;
  result.session = std::move(session);
  return result;
}

AdbcObjectsResult LoadObjectsBlocking(AdbcSession& session) {
  AdbcObjectsResult result;
  AdbcError error = MakeError();
  StreamHolder stream;
  if (AdbcConnectionGetObjects(&session.connection, ADBC_OBJECT_DEPTH_ALL, nullptr, nullptr,
                               nullptr, nullptr, nullptr, &stream.stream,
                               &error) != ADBC_STATUS_OK) {
    result.error = TakeError(error);
    return result;
  }

  SchemaHolder schema;
  if (stream.stream.get_schema(&stream.stream, &schema.schema) != 0) {
    result.error = StreamError(stream.stream);
    return result;
  }

  ArrowArrayView root{};
  auto reset_root = [&] {
    if (root.array != nullptr) {
      ArrowArrayViewReset(&root);
    }
  };

  while (true) {
    ArrowArray array{};
    if (stream.stream.get_next(&stream.stream, &array) != 0) {
      result.error = StreamError(stream.stream);
      reset_root();
      return result;
    }
    if (array.release == nullptr) {
      break;
    }
    root = ArrowArrayView{};
    ArrowError arrow_error;
    ArrowErrorInit(&arrow_error);
    if (ArrowArrayViewInitFromSchema(&root, &schema.schema, &arrow_error) != NANOARROW_OK) {
      result.error = arrow_error.message;
      array.release(&array);
      return result;
    }
    ArrowErrorInit(&arrow_error);
    if (ArrowArrayViewSetArray(&root, &array, &arrow_error) != NANOARROW_OK) {
      result.error = arrow_error.message;
      array.release(&array);
      ArrowArrayViewReset(&root);
      return result;
    }

    const ArrowArrayView* catalogs = root.n_children > 1 ? root.children[1] : nullptr;
    for (int64_t catalog_index = 0; catalog_index < root.array->length; ++catalog_index) {
      const std::string catalog_name =
          root.n_children > 0 ? StringAt(root.children[0], catalog_index) : std::string{};
      if (catalogs == nullptr || ArrowArrayViewIsNull(catalogs, catalog_index) != 0) {
        continue;
      }
      const int64_t schema_start = ListStart(catalogs, catalog_index);
      const int64_t schema_end = ListEnd(catalogs, catalog_index);
      const ArrowArrayView* schema_struct = catalogs->children[0];
      for (int64_t schema_index = schema_start; schema_index < schema_end; ++schema_index) {
        const std::string db_schema = StringAt(schema_struct->children[0], schema_index);
        const ArrowArrayView* tables = schema_struct->children[1];
        if (ArrowArrayViewIsNull(tables, schema_index) != 0) {
          continue;
        }
        const int64_t table_start = ListStart(tables, schema_index);
        const int64_t table_end = ListEnd(tables, schema_index);
        const ArrowArrayView* table_struct = tables->children[0];
        for (int64_t table_index = table_start; table_index < table_end; ++table_index) {
          DbObject object;
          object.catalog = catalog_name;
          object.db_schema = db_schema;
          object.name = StringAt(table_struct->children[0], table_index);
          object.table_type = StringAt(table_struct->children[1], table_index);
          object.is_view = ContainsLower(object.table_type, "view");

          const ArrowArrayView* columns_list = table_struct->children[2];
          if (ArrowArrayViewIsNull(columns_list, table_index) == 0) {
            const int64_t column_start = ListStart(columns_list, table_index);
            const int64_t column_end = ListEnd(columns_list, table_index);
            const ArrowArrayView* column_struct = columns_list->children[0];
            for (int64_t column_index = column_start; column_index < column_end;
                 ++column_index) {
              ColumnMeta column;
              column.name = StringAt(column_struct->children[0], column_index);
              column.type = StringAt(column_struct->children[4], column_index);
              object.columns.push_back(std::move(column));
            }
          }

          const ArrowArrayView* constraints_list = table_struct->children[3];
          if (ArrowArrayViewIsNull(constraints_list, table_index) == 0) {
            const int64_t constraint_start = ListStart(constraints_list, table_index);
            const int64_t constraint_end = ListEnd(constraints_list, table_index);
            const ArrowArrayView* constraint_struct = constraints_list->children[0];
            for (int64_t constraint_index = constraint_start;
                 constraint_index < constraint_end; ++constraint_index) {
              const std::string constraint_type =
                  StringAt(constraint_struct->children[1], constraint_index);
              if (LowerAscii(constraint_type) != "primary key") {
                continue;
              }
              const ArrowArrayView* names = constraint_struct->children[2];
              if (names == nullptr || ArrowArrayViewIsNull(names, constraint_index) != 0) {
                continue;
              }
              const int64_t name_start = ListStart(names, constraint_index);
              const int64_t name_end = ListEnd(names, constraint_index);
              for (int64_t name_index = name_start; name_index < name_end; ++name_index) {
                object.primary_key.push_back(StringAt(names->children[0], name_index));
              }
            }
          }

          if (!object.name.empty()) {
            result.objects.push_back(std::move(object));
          }
        }
      }
    }

    ArrowArrayViewReset(&root);
    array.release(&array);
  }

  std::ranges::sort(result.objects, [](const DbObject& left, const DbObject& right) {
    if (left.is_view != right.is_view) {
      return !left.is_view;
    }
    if (left.db_schema != right.db_schema) {
      return left.db_schema < right.db_schema;
    }
    return left.name < right.name;
  });
  return result;
}

AdbcSchemaResult LoadSchemaBlocking(AdbcSession& session, const DbObject& object) {
  AdbcSchemaResult result;
  AdbcError error = MakeError();
  SchemaHolder schema;
  const char* catalog = object.catalog.empty() ? nullptr : object.catalog.c_str();
  const char* db_schema = object.db_schema.empty() ? nullptr : object.db_schema.c_str();

  if (AdbcConnectionGetTableSchema(&session.connection, catalog, db_schema,
                                   object.name.c_str(), &schema.schema,
                                   &error) != ADBC_STATUS_OK) {
    result.error = TakeError(error);
    result.columns = object.columns;
    return result;
  }

  for (int64_t index = 0; index < schema.schema.n_children; ++index) {
    const ArrowSchema* field = schema.schema.children[index];
    ColumnMeta column;
    column.name = field->name != nullptr ? field->name : "column " + std::to_string(index + 1);
    ArrowSchemaView view;
    ArrowError arrow_error;
    ArrowErrorInit(&arrow_error);
    if (ArrowSchemaViewInit(&view, field, &arrow_error) == NANOARROW_OK) {
      column.type = ArrowTypeString(view.type);
    } else {
      column.type = "ANY";
    }
    for (const ColumnMeta& known : object.columns) {
      if (known.name == column.name && !known.type.empty()) {
        column.type = known.type;
        break;
      }
    }
    column.nullable = (field->flags & ARROW_FLAG_NULLABLE) != 0;
    for (std::size_t key_index = 0; key_index < object.primary_key.size(); ++key_index) {
      if (object.primary_key[key_index] == column.name) {
        column.primary_key = true;
        column.primary_key_order = static_cast<int>(key_index + 1);
        break;
      }
    }
    result.columns.push_back(std::move(column));
  }

  if (result.columns.empty()) {
    result.columns = object.columns;
  }
  return result;
}

AdbcQueryResult ExecuteBlocking(AdbcSession& session, std::string sql, bool want_rows,
                                std::size_t max_rows) {
  AdbcQueryResult result;
  AdbcError error = MakeError();
  AdbcStatement statement{};
  if (AdbcStatementNew(&session.connection, &statement, &error) != ADBC_STATUS_OK) {
    result.error = TakeError(error);
    return result;
  }

  const auto cleanup = [&] {
    AdbcError release_error = MakeError();
    AdbcStatementRelease(&statement, &release_error);
    ReleaseError(release_error);
  };

  if (AdbcStatementSetSqlQuery(&statement, sql.c_str(), &error) != ADBC_STATUS_OK) {
    result.error = TakeError(error);
    cleanup();
    return result;
  }

  StreamHolder stream;
  std::int64_t affected = -1;
  const AdbcStatusCode status = AdbcStatementExecuteQuery(
      &statement, want_rows ? &stream.stream : nullptr, &affected, &error);
  if (status != ADBC_STATUS_OK) {
    result.error = TakeError(error);
    cleanup();
    return result;
  }
  result.affected = affected;

  if (!want_rows) {
    cleanup();
    return result;
  }

  SchemaHolder schema;
  if (stream.stream.get_schema(&stream.stream, &schema.schema) != 0) {
    result.error = StreamError(stream.stream);
    cleanup();
    return result;
  }

  const int64_t field_count = schema.schema.n_children;
  std::vector<FieldInfo> fields;
  fields.reserve(static_cast<std::size_t>(field_count));
  for (int64_t index = 0; index < field_count; ++index) {
    const ArrowSchema* field = schema.schema.children[index];
    FieldInfo info;
    info.name = field->name != nullptr ? field->name : "column " + std::to_string(index + 1);
    ArrowSchemaView view;
    ArrowError arrow_error;
    ArrowErrorInit(&arrow_error);
    if (ArrowSchemaViewInit(&view, field, &arrow_error) == NANOARROW_OK) {
      info.type = view.type;
      info.time_unit = view.time_unit;
      info.decimal_bitwidth = view.decimal_bitwidth;
      info.decimal_precision = view.decimal_precision;
      info.decimal_scale = view.decimal_scale;
    }
    fields.push_back(std::move(info));
    result.columns.push_back(fields.back().name);
  }

  ArrowArrayView root{};
  ArrowError arrow_error;
  ArrowErrorInit(&arrow_error);
  if (ArrowArrayViewInitFromSchema(&root, &schema.schema, &arrow_error) != NANOARROW_OK) {
    result.error = arrow_error.message;
    cleanup();
    return result;
  }

  bool stop = false;
  while (!stop) {
    ArrowArray array{};
    if (stream.stream.get_next(&stream.stream, &array) != 0) {
      result.error = StreamError(stream.stream);
      break;
    }
    if (array.release == nullptr) {
      break;
    }

    ArrowArrayViewReset(&root);
    ArrowErrorInit(&arrow_error);
    if (ArrowArrayViewInitFromSchema(&root, &schema.schema, &arrow_error) != NANOARROW_OK) {
      result.error = arrow_error.message;
      array.release(&array);
      break;
    }
    ArrowErrorInit(&arrow_error);
    if (ArrowArrayViewSetArray(&root, &array, &arrow_error) != NANOARROW_OK) {
      result.error = arrow_error.message;
      array.release(&array);
      break;
    }

    const int64_t rows = array.length;
    for (int64_t row = 0; row < rows; ++row) {
      if (result.rows.size() >= max_rows) {
        result.truncated = true;
        stop = true;
        break;
      }
      GridRow grid_row;
      grid_row.values.reserve(fields.size());
      for (int64_t column = 0; column < field_count; ++column) {
        grid_row.values.push_back(ExtractCell(*root.children[column], row, fields[column]));
      }
      result.rows.push_back(std::move(grid_row));
    }

    ArrowArrayViewReset(&root);
    array.release(&array);
  }

  ArrowArrayViewReset(&root);
  cleanup();
  return result;
}

std::string QuoteIdentifier(Dialect dialect, std::string_view identifier) {
  const char quote = dialect == Dialect::Mysql ? '`' : '"';
  std::string result;
  result.reserve(identifier.size() + 2);
  result.push_back(quote);
  for (const char c : identifier) {
    if (c == quote) {
      result.push_back(quote);
    }
    result.push_back(c);
  }
  result.push_back(quote);
  return result;
}

std::string QualifiedName(Dialect dialect, const DbObject& object) {
  if (object.db_schema.empty()) {
    return QuoteIdentifier(dialect, object.name);
  }
  return QuoteIdentifier(dialect, object.db_schema) + "." +
         QuoteIdentifier(dialect, object.name);
}

namespace {

bool IsNumericType(std::string_view type) {
  const std::string lower = LowerAscii(type);
  return lower.find("int") != std::string::npos || lower.find("real") != std::string::npos ||
         lower.find("double") != std::string::npos || lower.find("float") != std::string::npos ||
         lower.find("numeric") != std::string::npos ||
         lower.find("decimal") != std::string::npos;
}

bool IsBoolType(std::string_view type) {
  const std::string lower = LowerAscii(type);
  return lower.find("bool") != std::string::npos;
}

bool IsValidNumber(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  double value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size() && std::isfinite(value);
}

} // namespace

std::string SqlLiteral(Dialect dialect, std::string_view text, std::string_view declared_type,
                       bool is_null) {
  if (is_null) {
    return "NULL";
  }
  if (IsBoolType(declared_type)) {
    const std::string lower = LowerAscii(text);
    if (lower == "1" || lower == "true") {
      return "TRUE";
    }
    if (lower == "0" || lower == "false") {
      return "FALSE";
    }
  }
  if (IsNumericType(declared_type) && IsValidNumber(text)) {
    return std::string(text);
  }
  std::string result;
  result.reserve(text.size() + 2);
  result.push_back('\'');
  for (const char c : text) {
    if (dialect == Dialect::Mysql && c == '\\') {
      result.push_back('\\');
    }
    if (c == '\'') {
      result.push_back('\'');
    }
    result.push_back(c);
  }
  result.push_back('\'');
  return result;
}

bool IsSqlReadStatement(std::string_view sql) {
  std::string_view text = sql;
  for (;;) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
      return false;
    }
    text.remove_prefix(first);
    if (text.starts_with("--")) {
      const auto newline = text.find('\n');
      text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
      continue;
    }
    if (text.starts_with("/*")) {
      const auto close = text.find("*/");
      text = close == std::string_view::npos ? std::string_view{} : text.substr(close + 2);
      continue;
    }
    break;
  }
  if (text.empty()) {
    return false;
  }
  const auto end = text.find_first_of(" \t\r\n(");
  const std::string keyword = LowerAscii(text.substr(0, end));
  if (keyword == "select" || keyword == "pragma" || keyword == "explain" ||
      keyword == "values" || keyword == "table" || keyword == "with") {
    return true;
  }
  return LowerAscii(text).find(" returning ") != std::string::npos;
}

namespace {

} // namespace

namespace {

std::string DriverDisplayName(const std::filesystem::path& path) {
  std::string filename = path.filename().string();
  std::string base = filename;
  for (const std::string_view suffix : {".so", ".dylib", ".dll"}) {
    const auto position = base.find(suffix);
    if (position != std::string::npos) {
      base.resize(position);
      break;
    }
  }
  if (base.starts_with("lib")) {
    base.erase(0, 3);
  }
  if (base.starts_with("adbc_driver_")) {
    base.erase(0, 12);
  }
  const std::string lower = LowerAscii(base);
  if (lower == "sqlite") return "SQLite";
  if (lower == "postgresql" || lower == "postgres") return "PostgreSQL";
  if (lower == "mysql" || lower == "mariadb") return "MySQL";
  if (lower == "duckdb") return "DuckDB";
  if (lower == "flightsql") return "Flight SQL";
  if (lower == "snowflake") return "Snowflake";
  if (lower == "bigquery") return "BigQuery";
  std::string display = base;
  std::ranges::replace(display, '_', ' ');
  std::ranges::replace(display, '-', ' ');
  return display.empty() ? filename : display;
}

bool IsDriverFileName(std::string_view filename) {
  const std::string lower = LowerAscii(filename);
  if (lower.find("adbc_driver") == std::string::npos) {
    return false;
  }
  return lower.ends_with(".so") || lower.ends_with(".dylib") ||
         lower.ends_with(".dll") || lower.find(".so.") != std::string::npos;
}

std::string DriverBaseKey(std::string_view filename) {
  std::string base = LowerAscii(filename);
  const auto position = base.find(".so");
  if (position != std::string::npos) {
    base.resize(position);
  } else {
    for (const std::string_view suffix : {".dylib", ".dll"}) {
      const auto extension = base.rfind(suffix);
      if (extension != std::string::npos) {
        base.resize(extension);
        break;
      }
    }
  }
  return base;
}

std::vector<std::string> SplitSearchPath(std::string_view value) {
#if defined(_WIN32)
  constexpr char kPathSeparator = ';';
#else
  constexpr char kPathSeparator = ':';
#endif
  std::vector<std::string> entries;
  std::size_t start = 0;
  for (std::size_t index = 0; index <= value.size(); ++index) {
    if (index == value.size() || value[index] == kPathSeparator) {
      if (index > start) {
        entries.emplace_back(value.substr(start, index - start));
      }
      start = index + 1;
    }
  }
  return entries;
}

} // namespace

std::vector<AdbcDriverInfo> DiscoverAdbcDrivers() {
  std::map<std::string, AdbcDriverInfo> by_base;

  const auto add_file = [&](const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
      return;
    }
    const std::string filename = path.filename().string();
    if (!IsDriverFileName(filename)) {
      return;
    }
    const std::string key = DriverBaseKey(filename);
    std::error_code absolute_error;
    std::filesystem::path absolute = std::filesystem::absolute(path, absolute_error);
    if (absolute_error) {
      absolute = path;
    }
    const auto existing = by_base.find(key);
    if (existing != by_base.end() &&
        std::filesystem::path(existing->second.path).filename().string().size() <=
            filename.size()) {
      return;
    }
    by_base[key] = AdbcDriverInfo{
        .name = DriverDisplayName(path),
        .path = absolute.string(),
    };
  };

  const auto add_entry = [&](std::string entry) {
    if (entry.empty()) {
      return;
    }
    std::filesystem::path path(entry);
    std::error_code error;
    if (std::filesystem::is_directory(path, error)) {
      for (std::filesystem::directory_iterator it(path, error), end; it != end;
           it.increment(error)) {
        if (error) {
          break;
        }
        add_file(it->path());
      }
    } else {
      add_file(path);
    }
  };

  for (const char* variable : {"DBPP_ADBC_DRIVERS", "ADBC_DRIVER_PATH"}) {
    if (const char* value = std::getenv(variable); value != nullptr) {
      for (const std::string& entry : SplitSearchPath(value)) {
        add_entry(entry);
      }
    }
  }

  add_file(DefaultSqliteDriverPath());
  const std::filesystem::path default_driver(DefaultSqliteDriverPath());
  if (default_driver.has_parent_path()) {
    add_entry(default_driver.parent_path().string());
  }

  for (const char* directory :
       {"/usr/lib64", "/usr/lib", "/usr/local/lib64", "/usr/local/lib",
        "/opt/homebrew/lib"}) {
    add_entry(directory);
  }

  std::vector<AdbcDriverInfo> drivers;
  drivers.reserve(by_base.size());
  for (auto& [key, driver] : by_base) {
    (void)key;
    drivers.push_back(std::move(driver));
  }
  std::ranges::sort(drivers, [](const AdbcDriverInfo& left, const AdbcDriverInfo& right) {
    if (left.name != right.name) {
      return left.name < right.name;
    }
    return left.path < right.path;
  });
  return drivers;
}


} // namespace dbpp
