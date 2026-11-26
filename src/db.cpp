module;

#include <huxerui/datatable.h>
#include <huxerui/http.h>
#include <huxerui/huxerui.h>
#include <huxerui/task.h>

module dbpp.db;

import std;
import dbpp.settings;
import dbpp.i18n;
import dbpp.types;
import dbpp.adbc;
import dbpp.redis;

namespace dbpp {

using namespace huxerui;

namespace {

std::string TrimAsciiCopy(std::string_view text) {
  while (!text.empty() &&
         (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' ||
          text.front() == '\n')) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
          text.back() == '\n')) {
    text.remove_suffix(1);
  }
  return std::string(text);
}

std::string LowerAsciiCopy(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

std::string UpperAsciiCopy(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
  }
  return out;
}

bool ContainsIgnoreCaseCopy(std::string_view haystack, std::string_view needle) {
  return LowerAsciiCopy(haystack).find(LowerAsciiCopy(needle)) != std::string::npos;
}

std::optional<int> ParseRedisTargetDbIndex(std::string_view text) {
  std::string trimmed = TrimAsciiCopy(text);
  if (trimmed.size() >= 2 &&
      (trimmed[0] == 'd' || trimmed[0] == 'D') &&
      (trimmed[1] == 'b' || trimmed[1] == 'B')) {
    trimmed.erase(0, 2);
  }
  if (trimmed.empty()) {
    return std::nullopt;
  }
  int value = 0;
  const char* begin = trimmed.data();
  const char* end = begin + trimmed.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end || value < 0) {
    return std::nullopt;
  }
  return value;
}

bool SessionHasMultipleCatalogsCopy(const DbSession& session) {
  std::string first_catalog;
  for (const DbObject& object : session.objects) {
    if (object.catalog.empty()) {
      continue;
    }
    const std::string lower = LowerAsciiCopy(object.catalog);
    if (lower == "def" || lower == "main") {
      continue;
    }
    if (first_catalog.empty()) {
      first_catalog = lower;
    } else if (lower != first_catalog) {
      return true;
    }
  }
  return false;
}

std::string SessionObjectBucketCopy(const DbSession& session, const DbObject& object,
                                    bool multi_catalog) {
  if (session.dialect == Dialect::Redis) {
    return {};
  }
  if (session.dialect == Dialect::Mysql) {
    return ObjectDatabaseName(Dialect::Mysql, object);
  }
  if (session.dialect == Dialect::Sqlite) {
    if (!object.db_schema.empty()) {
      return object.db_schema;
    }
    if (!object.catalog.empty()) {
      return object.catalog;
    }
    return "main";
  }
  if (session.dialect == Dialect::Duckdb) {
    const std::string name = ObjectDatabaseName(Dialect::Duckdb, object);
    if (!name.empty()) {
      return name;
    }
    if (!object.catalog.empty()) {
      return object.catalog;
    }
    return "main";
  }
  if (multi_catalog && !object.catalog.empty() &&
      !ContainsIgnoreCaseCopy(object.catalog, "def") &&
      !ContainsIgnoreCaseCopy(object.catalog, "main")) {
    if (!object.db_schema.empty() && object.db_schema != object.catalog) {
      return object.catalog + "." + object.db_schema;
    }
    return object.catalog;
  }
  return ObjectDatabaseName(session.dialect, object);
}

const DbSession* FindSession(const AppModel& model, std::uint64_t id) {
  for (const auto& session : model.sessions) {
    if (session.id == id) {
      return &session;
    }
  }
  return nullptr;
}

const DbObject* FindObject(const DbSession& session, std::string_view name,
                           std::string_view schema = {}) {
  if (!schema.empty()) {
    for (const auto& object : session.objects) {
      if (object.name == name &&
          (object.db_schema == schema ||
           (object.db_schema.empty() && ObjectDatabaseName(session.dialect, object) == schema))) {
        return &object;
      }
    }
  }
  for (const auto& object : session.objects) {
    if (object.name == name) {
      return &object;
    }
  }
  return nullptr;
}

bool SessionHasMultiSchemas(const DbSession& session) {
  std::string first;
  for (const auto& object : session.objects) {
    const std::string db_name = ObjectDatabaseName(session.dialect, object);
    if (db_name.empty()) {
      continue;
    }
    if (first.empty()) {
      first = db_name;
    } else if (db_name != first) {
      return true;
    }
  }
  return false;
}

std::string FormatTabTitle(const DbSession& session, std::string_view object,
                           std::string_view db_schema) {
  if (!db_schema.empty() && SessionHasMultiSchemas(session)) {
    return std::string(db_schema) + "." + std::string(object);
  }
  return std::string(object);
}

void SyncAppFromActiveTab(AppModel& model, const WorkspaceModel& ws) {
  const WorkspaceTab* active = ws.ActiveTab();
  if (active == nullptr) {
    model.active_object.clear();
    model.active_is_view = false;
    return;
  }
  if (active->kind == WorkspaceTabKind::Settings) {
    model.active_object.clear();
    model.active_is_view = false;
    return;
  }
  model.active_session = active->session_id;
  if (active->kind == WorkspaceTabKind::Table) {
    model.active_object = active->object_name;
    model.active_is_view = active->is_view;
  } else {
    model.active_object.clear();
    model.active_is_view = false;
  }
}

std::size_t ParseRowIndex(std::string_view id) {
  std::size_t index = 0;
  const auto [end, error] = std::from_chars(id.data(), id.data() + id.size(), index);
  if (error != std::errc{} || end != id.data() + id.size()) {
    return static_cast<std::size_t>(-1);
  }
  return index;
}

std::int64_t ParseInt64(std::string_view text) {
  std::int64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    return 0;
  }
  return value;
}

bool EqualsIgnoreCaseAscii(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(left[i])) !=
        std::tolower(static_cast<unsigned char>(right[i]))) {
      return false;
    }
  }
  return true;
}

std::string BuildWhere(const Dialect dialect, const TableModel& model, const GridRow& row) {
  if (model.uses_rowid && row.rowid.has_value()) {
    return "rowid = " + std::to_string(*row.rowid);
  }
  std::string where;
  for (std::size_t key_index = 0; key_index < model.key_columns.size(); ++key_index) {
    const std::size_t column_index = model.key_columns[key_index];
    if (column_index >= model.columns.size() || key_index >= row.key_values.size()) {
      return {};
    }
    if (!where.empty()) {
      where += " AND ";
    }
    const ColumnMeta& column = model.columns[column_index];
    const CellValue& key = row.key_values[key_index];
    where += QuoteIdentifier(dialect, column.name);
    if (key.is_null) {
      where += " IS NULL";
    } else {
      where += " = ";
      where += SqlLiteral(dialect, key.text, column.type, false);
    }
  }
  return where;
}

void CloseSessionAsync(std::shared_ptr<AdbcSession> adbc, const TaskScope& tasks) {
  if (!adbc) {
    return;
  }
  adbc->MarkClosed();
  tasks.Launch([adbc = std::move(adbc)]() mutable -> Task<void> {
    co_await adbc->sequence.Run([adbc](std::stop_token) { adbc->ReleaseResourcesBlocking(); });
  });
}

} // namespace

TableModel LoadTableBlocking(AdbcSession& session, std::uint64_t session_id,
                             DbObject object, std::int64_t page, std::int64_t page_size,
                             std::optional<DataTableSort> sort, std::string filter) {
  if (session.IsClosed()) {
    TableModel model;
    model.session_id = session_id;
    model.object = object.name;
    model.connection_lost = true;
    model.error = Tr("Connection is closed", "连接已关闭");
    return model;
  }
  if (session.dialect == Dialect::Redis && session.redis) {
    return LoadRedisTableBlocking(*session.redis, session_id, object, page, page_size,
                                  std::move(sort), TrimAsciiCopy(filter));
  }
  TableModel model;
  model.session_id = session_id;
  model.object = object.name;
  model.qualified_name = QualifiedName(session.dialect, object);
  model.is_view = object.is_view;
  model.read_only = false;
  model.page = std::max<std::int64_t>(0, page);
  model.page_size = std::max<std::int64_t>(1, page_size);
  model.sort = std::move(sort);
  model.filter = NormalizeSqlFilter(filter);

  AdbcSchemaResult schema = LoadSchemaBlocking(session, object);
  if (schema.connection_lost) {
    model.connection_lost = true;
    model.error = schema.error;
    return model;
  }
  model.columns = schema.columns;
  if (model.columns.empty()) {
    if (!schema.error.empty()) {
      model.error = schema.error;
      return model;
    }
    auto header = ExecuteBlocking(session,
                                  "SELECT * FROM " + model.qualified_name + " LIMIT 0",
                                  true, 0);
    if (!header.error.empty()) {
      model.connection_lost = header.connection_lost;
      model.error = header.error;
      return model;
    }
    for (const std::string& name : header.columns) {
      model.columns.push_back(ColumnMeta{.name = name, .type = "ANY"});
    }
  }

  for (std::size_t index = 0; index < model.columns.size(); ++index) {
    if (model.columns[index].primary_key) {
      model.key_columns.push_back(index);
    }
  }
  std::ranges::sort(model.key_columns, [&model](std::size_t left, std::size_t right) {
    return model.columns[left].primary_key_order < model.columns[right].primary_key_order;
  });

  if (session.dialect == Dialect::Sqlite && !object.is_view && model.key_columns.empty()) {
    const AdbcQueryResult probe = ExecuteBlocking(
        session, "SELECT rowid FROM " + model.qualified_name + " LIMIT 0", true, 0);
    model.uses_rowid = probe.error.empty();
  }
  model.editable = !object.is_view && (model.uses_rowid || !model.key_columns.empty());

  std::string where;
  if (!model.filter.empty()) {
    where = " WHERE (" + model.filter + ")";
  }

  const AdbcQueryResult count = ExecuteBlocking(
      session, "SELECT COUNT(*) FROM " + model.qualified_name + where, true, 1);
  if (!count.error.empty() || count.rows.empty() || count.rows.front().values.empty()) {
    model.connection_lost = count.connection_lost;
    model.error = count.error.empty()
                      ? std::string(Tr("COUNT query returned no result", "COUNT 查询没有返回结果"))
                      : count.error;
    return model;
  }
  model.total = ParseInt64(count.rows.front().values.front().text);

  const std::int64_t page_count =
      model.total <= 0 ? 1 : ((model.total - 1) / model.page_size + 1);
  model.page = std::clamp<std::int64_t>(model.page, 0, page_count - 1);
  const std::int64_t offset =
      model.page > std::numeric_limits<std::int64_t>::max() / model.page_size
          ? std::numeric_limits<std::int64_t>::max()
          : model.page * model.page_size;

  const std::string select_list =
      model.uses_rowid ? std::string{"rowid AS \"__dbpp_rowid\", *"} : std::string{"*"};

  std::string order;
  if (model.sort.has_value() && !model.sort->column_id.empty()) {
    const bool found = std::ranges::any_of(model.columns, [&model](const ColumnMeta& column) {
      return column.name == model.sort->column_id;
    });
    if (found) {
      order = " ORDER BY " + QuoteIdentifier(session.dialect, model.sort->column_id);
      order += model.sort->direction == DataTableSortDirection::Descending ? " DESC" : " ASC";
    }
  }
  std::string tie_breaker;
  if (model.uses_rowid) {
    tie_breaker = "rowid";
  } else if (!model.key_columns.empty()) {
    for (const std::size_t index : model.key_columns) {
      if (!tie_breaker.empty()) {
        tie_breaker += ", ";
      }
      tie_breaker += QuoteIdentifier(session.dialect, model.columns[index].name);
    }
  }
  if (!tie_breaker.empty()) {
    order += order.empty() ? " ORDER BY " : ", ";
    order += tie_breaker;
  }

  const std::string data_sql =
      "SELECT " + select_list + " FROM " + model.qualified_name + where + order +
      " LIMIT " + std::to_string(model.page_size) + " OFFSET " + std::to_string(offset);

  AdbcQueryResult data = ExecuteBlocking(session, data_sql, true,
                                         static_cast<std::size_t>(model.page_size));
  model.executed_sql = data_sql;
  model.elapsed_us = data.elapsed_us;
  if (!data.error.empty() && data.rows.empty()) {
    model.connection_lost = data.connection_lost;
    model.error = data.error;
    return model;
  }

  std::vector<GridRow> rows;
  rows.reserve(data.rows.size());
  for (GridRow& row : data.rows) {
    if (model.uses_rowid && !row.values.empty()) {
      row.rowid = ParseInt64(row.values.front().text);
      row.values.erase(row.values.begin());
    }
    if (row.values.size() < model.columns.size()) {
      row.values.resize(model.columns.size());
    }
    if (row.values.size() > model.columns.size()) {
      row.values.resize(model.columns.size());
    }
    row.key_values.clear();
    for (const std::size_t index : model.key_columns) {
      row.key_values.push_back(index < row.values.size() ? row.values[index] : CellValue{});
    }
    rows.push_back(std::move(row));
  }

  model.snapshot = std::make_shared<const GridSnapshot>(GridSnapshot{.rows = std::move(rows)});
  if (!data.error.empty()) {
    model.error = data.error;
  }
  model.message = TrFmt("{0} rows · {1} pages", "{0} 行 · 共 {1} 页",
                        {std::to_string(model.total), std::to_string(page_count)});
  return model;
}


namespace {

std::string SqlPlaceholder(Dialect dialect, std::size_t one_based_index) {
  if (dialect == Dialect::Postgresql) {
    return "$" + std::to_string(one_based_index);
  }
  return "?";
}

bool BuildParameterizedWhere(Dialect dialect, const TableModel& model, const GridRow& row,
                             std::string& where_out, std::vector<PreparedParamValue>& params_out,
                             std::vector<std::string>& types_out, std::size_t& next_param_idx) {
  where_out.clear();
  if (model.uses_rowid && row.rowid.has_value()) {
    where_out = "rowid = " + SqlPlaceholder(dialect, next_param_idx++);
    params_out.push_back(PreparedParamValue{.text = std::to_string(*row.rowid), .is_null = false});
    types_out.emplace_back("INTEGER");
    return true;
  }
  for (std::size_t key_index = 0; key_index < model.key_columns.size(); ++key_index) {
    const std::size_t column_index = model.key_columns[key_index];
    if (column_index >= model.columns.size() || key_index >= row.key_values.size()) {
      return false;
    }
    if (!where_out.empty()) {
      where_out += " AND ";
    }
    const ColumnMeta& column = model.columns[column_index];
    const CellValue& key = row.key_values[key_index];
    where_out += QuoteIdentifier(dialect, column.name);
    if (key.is_null) {
      where_out += " IS NULL";
    } else {
      where_out += " = " + SqlPlaceholder(dialect, next_param_idx++);
      params_out.push_back(PreparedParamValue{.text = key.text, .is_null = false});
      types_out.push_back(column.type);
    }
  }
  return !where_out.empty();
}

AdbcQueryResult UpdateCellBlocking(AdbcSession& session, const TableModel& model,
                                   std::size_t row_index, std::size_t column_index,
                                   const std::string& text) {
  if (session.dialect == Dialect::Redis && session.redis) {
    return UpdateRedisCellBlocking(*session.redis, model, row_index, column_index, text);
  }
  AdbcQueryResult result;
  if (!model.snapshot || row_index >= model.snapshot->rows.size() ||
      column_index >= model.columns.size()) {
    result.error = Tr("Target row or column is invalid", "目标行或列已失效");
    return result;
  }
  if (!model.editable) {
    result.error = Tr("Current object is not editable", "当前对象不允许编辑");
    return result;
  }
  const GridRow& row = model.snapshot->rows[row_index];
  const ColumnMeta& column = model.columns[column_index];
  const bool is_null = text.empty() || EqualsIgnoreCaseAscii(text, "NULL");

  std::size_t next_idx = 1;
  std::vector<PreparedParamValue> params;
  std::vector<std::string> param_types;
  const std::string set_placeholder = SqlPlaceholder(session.dialect, next_idx++);
  params.push_back(PreparedParamValue{.text = is_null ? std::string{} : text, .is_null = is_null});
  param_types.push_back(column.type);

  std::string param_where;
  if (BuildParameterizedWhere(session.dialect, model, row, param_where, params, param_types,
                              next_idx)) {
    const std::string param_sql = "UPDATE " + model.qualified_name + " SET " +
                                  QuoteIdentifier(session.dialect, column.name) + " = " +
                                  set_placeholder + " WHERE " + param_where;
    int prep_status = 0;
    AdbcQueryResult prep_res;
    if (ExecutePreparedNonQuery(session, param_sql, params, param_types, prep_res, prep_status)) {
      return prep_res;
    }
    if (!prep_res.error.empty() && prep_status != 2 /* ADBC_STATUS_NOT_IMPLEMENTED */) {
      // Fall through only when driver does not implement prepared bind; report real errors directly
      if (IsConnectionLostError(prep_status, prep_res.error, prep_res.sqlstate)) {
        prep_res.connection_lost = true;
      }
      return prep_res;
    }
  }

  const std::string where = BuildWhere(session.dialect, model, row);
  if (where.empty()) {
    result.error = Tr("Table has no usable row identifier", "该表没有可用的行标识");
    return result;
  }
  const std::string value = SqlLiteral(session.dialect, text, column.type, is_null);
  const std::string sql = "UPDATE " + model.qualified_name + " SET " +
                          QuoteIdentifier(session.dialect, column.name) + " = " + value +
                          " WHERE " + where;
  return ExecuteBlocking(session, sql, false, 0);
}

AdbcQueryResult DeleteRowBlocking(AdbcSession& session, const TableModel& model,
                                  std::size_t row_index) {
  if (session.dialect == Dialect::Redis && session.redis) {
    return DeleteRedisKeyBlocking(*session.redis, model, row_index);
  }
  AdbcQueryResult result;
  if (!model.snapshot || row_index >= model.snapshot->rows.size()) {
    result.error = Tr("Target row is invalid", "目标行已失效");
    return result;
  }
  if (!model.editable) {
    result.error = Tr("Current object does not allow deleting rows", "当前对象不允许删除行");
    return result;
  }
  const GridRow& row = model.snapshot->rows[row_index];

  std::size_t next_idx = 1;
  std::vector<PreparedParamValue> params;
  std::vector<std::string> param_types;
  std::string param_where;
  if (BuildParameterizedWhere(session.dialect, model, row, param_where, params, param_types,
                              next_idx)) {
    const std::string param_sql = "DELETE FROM " + model.qualified_name + " WHERE " + param_where;
    int prep_status = 0;
    AdbcQueryResult prep_res;
    if (ExecutePreparedNonQuery(session, param_sql, params, param_types, prep_res, prep_status)) {
      return prep_res;
    }
    if (!prep_res.error.empty() && prep_status != 2 /* ADBC_STATUS_NOT_IMPLEMENTED */) {
      if (IsConnectionLostError(prep_status, prep_res.error, prep_res.sqlstate)) {
        prep_res.connection_lost = true;
      }
      return prep_res;
    }
  }

  const std::string where = BuildWhere(session.dialect, model, row);
  if (where.empty()) {
    result.error = Tr("Table has no usable row identifier", "该表没有可用的行标识");
    return result;
  }
  return ExecuteBlocking(session,
                         "DELETE FROM " + model.qualified_name + " WHERE " + where, false,
                         0);
}

AdbcQueryResult InsertRowBlocking(AdbcSession& session, const TableModel& model,
                                  std::vector<std::string> values) {
  AdbcQueryResult result;
  if (!model.editable) {
    result.error = Tr("Current object does not allow inserting rows", "当前对象不允许插入行");
    return result;
  }
  if (values.size() < model.columns.size()) {
    values.resize(model.columns.size());
  }

  std::string columns;
  std::string param_placeholders;
  std::string literal_placeholders;
  std::vector<PreparedParamValue> params;
  std::vector<std::string> param_types;
  std::size_t next_idx = 1;
  for (std::size_t index = 0; index < model.columns.size() && index < values.size();
       ++index) {
    if (values[index].empty()) {
      continue;
    }
    if (!columns.empty()) {
      columns += ", ";
      param_placeholders += ", ";
      literal_placeholders += ", ";
    }
    columns += QuoteIdentifier(session.dialect, model.columns[index].name);
    const bool is_null = EqualsIgnoreCaseAscii(values[index], "NULL");
    param_placeholders += SqlPlaceholder(session.dialect, next_idx++);
    params.push_back(PreparedParamValue{
        .text = is_null ? std::string{} : values[index],
        .is_null = is_null,
    });
    param_types.push_back(model.columns[index].type);
    literal_placeholders += SqlLiteral(session.dialect, values[index], model.columns[index].type,
                                       is_null);
  }

  if (!columns.empty()) {
    const std::string param_sql = "INSERT INTO " + model.qualified_name + " (" + columns +
                                  ") VALUES (" + param_placeholders + ")";
    int prep_status = 0;
    AdbcQueryResult prep_res;
    if (ExecutePreparedNonQuery(session, param_sql, params, param_types, prep_res, prep_status)) {
      return prep_res;
    }
    if (!prep_res.error.empty() && prep_status != 2 /* ADBC_STATUS_NOT_IMPLEMENTED */) {
      if (IsConnectionLostError(prep_status, prep_res.error, prep_res.sqlstate)) {
        prep_res.connection_lost = true;
      }
      return prep_res;
    }
  }

  std::string sql;
  if (columns.empty()) {
    sql = session.dialect == Dialect::Mysql
              ? "INSERT INTO " + model.qualified_name + " () VALUES ()"
              : "INSERT INTO " + model.qualified_name + " DEFAULT VALUES";
  } else {
    sql = "INSERT INTO " + model.qualified_name + " (" + columns + ") VALUES (" +
          literal_placeholders + ")";
  }
  return ExecuteBlocking(session, sql, false, 0);
}

void ApplyOpenedSession(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                        const TaskScope& tasks, AdbcOpenResult opened,
                        ConnectionRequest request, AdbcObjectsResult objects,
                        NotifyFn notify,
                        std::function<void(std::uint64_t)> on_opened = {}) {
  if (!opened.session) {
    const std::string fail_key = ConnectionFailureKey(request.driver_path, request.uri);
    const std::string fail_detail = opened.error;
    const std::string message =
        TrFmt("Connection failed: {0}", "连接失败: {0}", {opened.error});
    app.Update([&fail_key, &fail_detail](AppModel& model) {
      model.status.clear();
      model.error.clear();
      model.failed_connections[fail_key] = fail_detail;
    });
    if (notify) {
      notify(message);
    }
    return;
  }

  auto session = std::make_shared<DbSession>();
  session->name = request.title.empty()
                      ? DisplayNameFromUri(request.uri, "ADBC")
                      : request.title;
  session->uri = request.uri;
  session->driver_path = request.driver_path;
  session->dialect = opened.session->dialect;
  session->sqlite = opened.session->dialect == Dialect::Sqlite;
  session->read_only = opened.session->read_only;
  session->connection_info = opened.session->connection_info;
  session->session = std::move(opened.session);
  session->objects = std::move(objects.objects);

  bool duplicated = false;
  bool reconnected = false;
  std::shared_ptr<AdbcSession> stale_or_redundant;
  std::uint64_t target_session_id = 0;
  std::string first_object;
  std::string first_schema;
  bool first_is_view = false;
  std::vector<std::uint64_t> open_session_ids;

  const auto pick_first_object = [](const DbSession& s, std::string& obj_out,
                                    std::string& schema_out, bool& is_view_out) {
    if (s.objects.empty()) {
      return;
    }
    std::size_t first_idx = 0;
    if (s.dialect == Dialect::Redis && s.session && s.session->redis) {
      const std::string target_db = "db" + std::to_string(s.session->redis->CurrentDb());
      for (std::size_t idx = 0; idx < s.objects.size(); ++idx) {
        if (s.objects[idx].name == target_db) {
          first_idx = idx;
          break;
        }
      }
    }
    obj_out = s.objects[first_idx].name;
    schema_out = s.objects[first_idx].db_schema;
    is_view_out = s.objects[first_idx].is_view;
  };

  const std::string connected_fail_key = ConnectionFailureKey(request.driver_path, request.uri);
  app.Update([&](AppModel& model) {
    model.failed_connections.erase(connected_fail_key);
    for (DbSession& existing : model.sessions) {
      if (existing.uri == session->uri && existing.dialect == session->dialect) {
        model.active_session = existing.id;
        model.status.clear();
        model.error.clear();
        target_session_id = existing.id;
        if (!existing.session || !existing.session->IsAlive()) {
          stale_or_redundant = std::move(existing.session);
          existing.name = session->name;
          existing.driver_path = session->driver_path;
          existing.session = session->session;
          existing.objects = std::move(session->objects);
          ++model.revision;
          reconnected = true;
          pick_first_object(existing, first_object, first_schema, first_is_view);
        } else {
          stale_or_redundant = std::move(session->session);
          duplicated = true;
        }
        break;
      }
    }
    if (!duplicated && !reconnected) {
      session->id = model.next_session_id++;
      target_session_id = session->id;
      pick_first_object(*session, first_object, first_schema, first_is_view);
      model.sessions.push_back(*session);
      model.active_session = target_session_id;
      model.active_object.clear();
      model.active_is_view = false;
      model.status.clear();
      model.error.clear();
      ++model.revision;
    }
    open_session_ids.reserve(model.sessions.size());
    for (const DbSession& open : model.sessions) {
      open_session_ids.push_back(open.id);
    }
  });

  if (stale_or_redundant) {
    CloseSessionAsync(std::move(stale_or_redundant), tasks);
  }

  if (!duplicated) {
    workspace.Update([&](WorkspaceModel& ws) {
      for (WorkspaceTab& tab : ws.tabs) {
        const bool is_live = std::ranges::find(open_session_ids, tab.session_id) !=
                             open_session_ids.end();
        if (!is_live && tab.session_uri == session->uri) {
          tab.session_id = target_session_id;
          tab.session_name = session->name;
        }
      }
    });
  }

  if (on_opened) {
    on_opened(target_session_id);
  } else if (duplicated) {
    std::uint64_t existing_tab_id = 0;
    for (const WorkspaceTab& tab : workspace.Get().tabs) {
      if (tab.session_id == target_session_id) {
        existing_tab_id = tab.id;
        break;
      }
    }
    if (existing_tab_id != 0) {
      ActivateWorkspaceTab(app, workspace, existing_tab_id);
    } else {
      app.Update([](AppModel& model) {
        model.active_object.clear();
        model.active_is_view = false;
      });
    }
  } else if (reconnected) {
    bool has_session_tab = false;
    std::uint64_t reload_tab_id = 0;
    std::int64_t reload_page = 0;
    std::optional<DataTableSort> reload_sort;
    std::string reload_filter;
    const WorkspaceModel& ws_now = workspace.Get();
    for (const WorkspaceTab& tab : ws_now.tabs) {
      if (tab.session_id == target_session_id) {
        has_session_tab = true;
      }
    }
    if (const WorkspaceTab* active = ws_now.ActiveTab();
        active != nullptr && active->session_id == target_session_id &&
        active->kind == WorkspaceTabKind::Table && active->table.pending_inserts.empty()) {
      reload_tab_id = active->id;
      reload_page = active->table.page;
      reload_sort = active->table.sort;
      reload_filter = active->table.filter;
    }
    if (reload_tab_id != 0) {
      RequestLoadTable(app, workspace, tasks, reload_tab_id, reload_page, reload_sort,
                       std::move(reload_filter), true, notify);
    } else if (!has_session_tab && !first_object.empty()) {
      SelectObject(app, workspace, tasks, target_session_id, first_object, first_schema,
                   first_is_view, notify);
    }
  } else if (!first_object.empty()) {
    SelectObject(app, workspace, tasks, target_session_id, first_object, first_schema,
                 first_is_view, notify);
  }

  if (notify) {
    if (duplicated) {
      notify(std::string(Tr("Connection already open", "该连接已存在")));
    } else if (!objects.error.empty()) {
      notify(TrFmt("Connected, but failed to load objects: {0}", "已连接，但读取对象失败: {0}",
                   {objects.error}));
    } else {
      notify(TrFmt("Connected to {0}", "已连接 {0}", {session->name}));
    }
  }
}

} // namespace

BatchInsertResult InsertRowsBlocking(AdbcSession& session, const TableModel& model,
                                     const std::vector<PendingInsertRow>& rows) {
  if (session.IsClosed()) {
    return BatchInsertResult{.inserted = 0,
                             .rolled_back = true,
                             .connection_lost = true,
                             .error = std::string(Tr("Connection is closed", "连接已关闭"))};
  }
  if (session.dialect == Dialect::Redis && session.redis) {
    return InsertRedisRowsBlocking(*session.redis, model, rows);
  }
  if (rows.empty()) {
    return {};
  }
  if (rows.size() == 1) {
    AdbcQueryResult result = InsertRowBlocking(session, model, rows[0].values);
    if (!result.error.empty()) {
      return BatchInsertResult{.inserted = 0,
                               .rolled_back = true,
                               .connection_lost = result.connection_lost,
                               .error = result.error};
    }
    return BatchInsertResult{.inserted = 1, .rolled_back = false, .error = {}};
  }

  const bool supports_tx = session.dialect == Dialect::Sqlite ||
                           session.dialect == Dialect::Postgresql ||
                           session.dialect == Dialect::Mysql ||
                           session.dialect == Dialect::Duckdb;
  bool managed_tx = false;
  if (supports_tx && !session.in_transaction) {
    std::string tx_err;
    if (SetAutocommitBlocking(session, false, tx_err)) {
      managed_tx = true;
    }
  }

  for (std::size_t i = 0; i < rows.size(); ++i) {
    AdbcQueryResult result = InsertRowBlocking(session, model, rows[i].values);
    if (!result.error.empty()) {
      const std::string msg = TrFmt("Row {0} insert failed: {1}", "第 {0} 行插入失败: {1}",
                                    {std::to_string(i + 1), result.error});
      if (managed_tx && !result.connection_lost) {
        std::string rb_err;
        (void)RollbackTransactionBlocking(session, rb_err);
        (void)SetAutocommitBlocking(session, true, rb_err);
        return BatchInsertResult{.inserted = 0,
                                 .rolled_back = true,
                                 .connection_lost = false,
                                 .error = msg};
      }
      return BatchInsertResult{.inserted = result.connection_lost && managed_tx ? 0 : i,
                               .rolled_back = result.connection_lost && managed_tx,
                               .connection_lost = result.connection_lost,
                               .error = msg};
    }
  }

  if (managed_tx) {
    std::string commit_err;
    if (!CommitTransactionBlocking(session, commit_err)) {
      std::string rb_err;
      (void)RollbackTransactionBlocking(session, rb_err);
      (void)SetAutocommitBlocking(session, true, rb_err);
      return BatchInsertResult{
          .inserted = 0,
          .rolled_back = true,
          .connection_lost = IsConnectionLostError(0, commit_err),
          .error = TrFmt("Failed to commit transaction: {0}", "提交事务失败: {0}",
                         {commit_err})};
    }
    std::string ac_err;
    (void)SetAutocommitBlocking(session, true, ac_err);
  }

  return BatchInsertResult{.inserted = rows.size(), .rolled_back = false, .error = {}};
}

namespace detail {

std::string DbTrimAscii(std::string_view text) { return TrimAsciiCopy(text); }
std::string DbLowerAscii(std::string_view text) { return LowerAsciiCopy(text); }
std::string DbUpperAscii(std::string_view text) { return UpperAsciiCopy(text); }
std::optional<int> DbParseRedisTargetDbIndex(std::string_view text) {
  return ParseRedisTargetDbIndex(text);
}
bool DbSessionHasMultipleCatalogs(const DbSession& session) {
  return SessionHasMultipleCatalogsCopy(session);
}
std::string DbSessionObjectBucket(const DbSession& session, const DbObject& object,
                                  bool multi_catalog) {
  return SessionObjectBucketCopy(session, object, multi_catalog);
}
const DbSession* DbFindSession(const AppModel& model, std::uint64_t id) {
  return FindSession(model, id);
}
const DbObject* DbFindObject(const DbSession& session, std::string_view name,
                             std::string_view db_schema) {
  return FindObject(session, name, db_schema);
}
std::string DbFormatTabTitle(const DbSession& session, std::string_view object,
                             std::string_view db_schema) {
  return FormatTabTitle(session, object, db_schema);
}
void DbSyncAppFromActiveTab(AppModel& model, const WorkspaceModel& ws) {
  SyncAppFromActiveTab(model, ws);
}
std::size_t DbParseRowIndex(std::string_view id) { return ParseRowIndex(id); }
bool DbEqualsIgnoreCaseAscii(std::string_view left, std::string_view right) {
  return EqualsIgnoreCaseAscii(left, right);
}
void DbCloseSessionAsync(std::shared_ptr<AdbcSession> adbc, const TaskScope& tasks) {
  CloseSessionAsync(std::move(adbc), tasks);
}
AdbcQueryResult DbUpdateCellBlocking(AdbcSession& session, const TableModel& model,
                                     std::size_t row_index, std::size_t column_index,
                                     const std::string& text) {
  return UpdateCellBlocking(session, model, row_index, column_index, text);
}
AdbcQueryResult DbDeleteRowBlocking(AdbcSession& session, const TableModel& model,
                                    std::size_t row_index) {
  return DeleteRowBlocking(session, model, row_index);
}
void DbApplyOpenedSession(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                          const TaskScope& tasks, AdbcOpenResult opened,
                          ConnectionRequest request, AdbcObjectsResult objects,
                          NotifyFn notify, std::function<void(std::uint64_t)> on_opened) {
  ApplyOpenedSession(app, workspace, tasks, std::move(opened), std::move(request),
                     std::move(objects), std::move(notify), std::move(on_opened));
}

} // namespace detail

} // namespace dbpp
