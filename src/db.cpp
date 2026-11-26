module;

#include <huxerui/datatable.h>
#include <huxerui/http.h>
#include <huxerui/huxerui.h>
#include <huxerui/task.h>
#include <sqlite3.h>

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

  if (session.dialect == Dialect::Sqlite && !object.is_view) {
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

  std::string select_list;
  if (model.uses_rowid) {
    select_list = "rowid AS \"__dbpp_rowid\"";
  }
  for (const ColumnMeta& column : model.columns) {
    if (!select_list.empty()) {
      select_list += ", ";
    }
    select_list += QuoteIdentifier(session.dialect, column.name);
  }
  if (select_list.empty()) {
    select_list = "*";
  }

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
  model.message = TrFmt("{0} rows · Page {1} / {2}", "{0} 行 · 第 {1} / {2} 页",
                        {std::to_string(model.total), std::to_string(model.page + 1),
                         std::to_string(page_count)});
  return model;
}

namespace {

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
  const std::string where = BuildWhere(session.dialect, model, row);
  if (where.empty()) {
    result.error = Tr("Table has no usable row identifier", "该表没有可用的行标识");
    return result;
  }
  const ColumnMeta& column = model.columns[column_index];
  const bool is_null = text.empty() || EqualsIgnoreCaseAscii(text, "NULL");
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
  std::string placeholders;
  for (std::size_t index = 0; index < model.columns.size() && index < values.size();
       ++index) {
    if (values[index].empty()) {
      continue;
    }
    if (!columns.empty()) {
      columns += ", ";
      placeholders += ", ";
    }
    columns += QuoteIdentifier(session.dialect, model.columns[index].name);
    const bool is_null = EqualsIgnoreCaseAscii(values[index], "NULL");
    placeholders += SqlLiteral(session.dialect, values[index], model.columns[index].type,
                               is_null);
  }

  std::string sql;
  if (columns.empty()) {
    sql = session.dialect == Dialect::Mysql
              ? "INSERT INTO " + model.qualified_name + " () VALUES ()"
              : "INSERT INTO " + model.qualified_name + " DEFAULT VALUES";
  } else {
    sql = "INSERT INTO " + model.qualified_name + " (" + columns + ") VALUES (" +
          placeholders + ")";
  }
  return ExecuteBlocking(session, sql, false, 0);
}

void ApplyOpenedSession(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                        const TaskScope& tasks, AdbcOpenResult opened,
                        ConnectionRequest request, AdbcObjectsResult objects,
                        NotifyFn notify,
                        std::function<void(std::uint64_t)> on_opened = {}) {
  if (!opened.session) {
    const std::string message =
        TrFmt("Connection failed: {0}", "连接失败: {0}", {opened.error});
    app.Update([](AppModel& model) {
      model.status.clear();
      model.error.clear();
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
  session->read_only = false;
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

  app.Update([&](AppModel& model) {
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

Task<AdbcQueryResult> ExecuteQueryAsync(std::shared_ptr<AdbcSession> session,
                                        std::string sql, bool want_rows,
                                        std::size_t max_rows,
                                        std::string target_database) {
  if (!session || session->IsClosed()) {
    AdbcQueryResult result;
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    co_return result;
  }
  co_return co_await session->sequence.Run(
      [session, sql = std::move(sql), want_rows, max_rows,
       target_database = std::move(target_database)](std::stop_token) mutable {
        const std::string trimmed_db = TrimAsciiCopy(target_database);
        if (!trimmed_db.empty()) {
          if (session->dialect == Dialect::Redis && session->redis) {
            if (const auto db_idx = ParseRedisTargetDbIndex(trimmed_db)) {
              std::string err;
              if (!session->redis->EnsureDb(*db_idx, err)) {
                AdbcQueryResult result;
                result.connection_lost = !session->redis->IsConnected();
                result.error = std::move(err);
                return result;
              }
            }
          } else {
            const std::string switch_sql =
                BuildSwitchDatabaseSql(session->dialect, trimmed_db);
            if (!switch_sql.empty()) {
              AdbcQueryResult sw = ExecuteBlocking(*session, switch_sql, false, 0);
              if (sw.connection_lost) {
                return sw;
              }
              if (!sw.error.empty() &&
                  !(session->dialect == Dialect::Generic && trimmed_db == "default")) {
                return sw;
              }
            }
          }
        }
        return ExecuteBlocking(*session, std::move(sql), want_rows, max_rows);
      });
}

Task<AdbcQueryResult> UpdateCellAsync(std::shared_ptr<AdbcSession> session, TableModel model,
                                      std::size_t row_index, std::size_t column_index,
                                      std::string text) {
  if (!session || session->IsClosed()) {
    AdbcQueryResult result;
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    co_return result;
  }
  co_return co_await session->sequence.Run(
      [session, model = std::move(model), row_index, column_index, text = std::move(text)](
          std::stop_token) mutable {
        if (session->IsClosed()) {
          AdbcQueryResult res;
          res.connection_lost = true;
          res.error = Tr("Connection is closed", "连接已关闭");
          return res;
        }
        return UpdateCellBlocking(*session, model, row_index, column_index, text);
      });
}

Task<AdbcQueryResult> DeleteRowAsync(std::shared_ptr<AdbcSession> session, TableModel model,
                                     std::size_t row_index) {
  if (!session || session->IsClosed()) {
    AdbcQueryResult result;
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    co_return result;
  }
  co_return co_await session->sequence.Run(
      [session, model = std::move(model), row_index](std::stop_token) mutable {
        if (session->IsClosed()) {
          AdbcQueryResult res;
          res.connection_lost = true;
          res.error = Tr("Connection is closed", "连接已关闭");
          return res;
        }
        return DeleteRowBlocking(*session, model, row_index);
      });
}

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
  bool in_tx = false;
  if (supports_tx) {
    const AdbcQueryResult begin_res = ExecuteBlocking(session, "BEGIN", false, 0);
    if (begin_res.connection_lost) {
      return BatchInsertResult{.inserted = 0,
                               .rolled_back = true,
                               .connection_lost = true,
                               .error = begin_res.error};
    }
    if (begin_res.error.empty()) {
      in_tx = true;
    }
  }

  for (std::size_t i = 0; i < rows.size(); ++i) {
    AdbcQueryResult result = InsertRowBlocking(session, model, rows[i].values);
    if (!result.error.empty()) {
      const std::string msg = TrFmt("Row {0} insert failed: {1}", "第 {0} 行插入失败: {1}",
                                    {std::to_string(i + 1), result.error});
      if (in_tx && !result.connection_lost) {
        (void)ExecuteBlocking(session, "ROLLBACK", false, 0);
        return BatchInsertResult{.inserted = 0,
                                 .rolled_back = true,
                                 .connection_lost = false,
                                 .error = msg};
      }
      return BatchInsertResult{.inserted = result.connection_lost && in_tx ? 0 : i,
                               .rolled_back = result.connection_lost && in_tx,
                               .connection_lost = result.connection_lost,
                               .error = msg};
    }
  }

  if (in_tx) {
    AdbcQueryResult committed = ExecuteBlocking(session, "COMMIT", false, 0);
    if (!committed.error.empty()) {
      if (!committed.connection_lost) {
        (void)ExecuteBlocking(session, "ROLLBACK", false, 0);
      }
      return BatchInsertResult{
          .inserted = 0,
          .rolled_back = true,
          .connection_lost = committed.connection_lost,
          .error = TrFmt("Failed to commit transaction: {0}", "提交事务失败: {0}",
                         {committed.error})};
    }
  }

  return BatchInsertResult{.inserted = rows.size(), .rolled_back = false, .error = {}};
}

void RequestLoadTable(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                      const TaskScope& tasks, std::uint64_t tab_id, std::int64_t page,
                      std::optional<DataTableSort> sort, std::string filter,
                      bool keep_selection, NotifyFn notify, std::int64_t page_size) {
  const WorkspaceModel& ws_snapshot = workspace.Get();
  const WorkspaceTab* tab = ws_snapshot.FindTab(tab_id);
  if (tab == nullptr || tab->kind != WorkspaceTabKind::Table) {
    return;
  }

  const std::uint64_t session_id = tab->session_id;
  const std::string object_name = tab->object_name;
  const std::string object_schema = tab->object_schema;
  const std::int64_t effective_page_size =
      page_size > 0 ? page_size
                    : (tab->table.page_size > 0 ? tab->table.page_size : kPageSize);

  const AppModel& model = app.Get();
  const DbSession* session = FindSession(model, session_id);
  if (session == nullptr || !session->session || session->session->IsClosed()) {
    if (session != nullptr) {
      HandleSessionDisconnected(app, workspace, tasks, session_id, {}, notify);
      return;
    }
    workspace.Update([&](WorkspaceModel& ws) {
      WorkspaceTab* target = ws.FindTab(tab_id);
      if (target != nullptr && target->kind == WorkspaceTabKind::Table) {
        target->table = TableModel{};
        target->selected_cell = std::nullopt;
      }
    });
    return;
  }
  const DbObject* object = FindObject(*session, object_name, object_schema);
  if (object == nullptr) {
    workspace.Update([&](WorkspaceModel& ws) {
      WorkspaceTab* target = ws.FindTab(tab_id);
      if (target != nullptr && target->kind == WorkspaceTabKind::Table) {
        target->table = TableModel{};
        target->selected_cell = std::nullopt;
      }
    });
    return;
  }

  workspace.Update([&](WorkspaceModel& ws) {
    WorkspaceTab* target = ws.FindTab(tab_id);
    if (target == nullptr || target->kind != WorkspaceTabKind::Table) {
      return;
    }
    target->table.loading = true;
    target->table.error.clear();
    if (!keep_selection) {
      target->selected_cell = std::nullopt;
    }
  });

  auto adbc_session = session->session;
  DbObject object_copy = *object;

  tasks.Launch([app, workspace, tasks, notify = std::move(notify), adbc_session, tab_id, session_id,
                object_copy = std::move(object_copy), object_name, object_schema, page,
                effective_page_size, sort, filter]() mutable -> Task<void> {
    TableModel result = co_await adbc_session->sequence.Run(
        [adbc_session, session_id, object_copy = std::move(object_copy), page,
         effective_page_size, sort, filter](std::stop_token) mutable {
          return LoadTableBlocking(*adbc_session, session_id, std::move(object_copy), page,
                                   effective_page_size, sort, filter);
        });

    const bool load_lost = result.connection_lost;
    const std::string load_error = result.error;
    const bool has_snapshot = result.snapshot != nullptr;
    if (load_lost) {
      workspace.Update([&](WorkspaceModel& ws) {
        if (WorkspaceTab* target = ws.FindTab(tab_id);
            target != nullptr && target->kind == WorkspaceTabKind::Table) {
          target->table.loading = false;
        }
      });
      HandleSessionDisconnected(app, workspace, tasks, session_id, load_error, notify);
      co_return;
    }
    bool tab_matches = false;
    workspace.Update([&](WorkspaceModel& ws) {
      WorkspaceTab* target = ws.FindTab(tab_id);
      if (target == nullptr || target->kind != WorkspaceTabKind::Table ||
          target->session_id != session_id || target->object_name != object_name ||
          target->object_schema != object_schema) {
        return;
      }
      tab_matches = true;
      if (!load_error.empty() && !has_snapshot) {
        if (target->table.session_id != session_id || target->table.object != object_name) {
          target->table = TableModel{};
          target->table.session_id = session_id;
          target->table.object = object_name;
          target->table.error = load_error;
          return;
        }
        target->table.loading = false;
        target->table.error = load_error;
        return;
      }
      result.pending_inserts = std::move(target->table.pending_inserts);
      result.next_draft_id = target->table.next_draft_id;
      result.committing = target->table.committing;
      result.column_widths = std::move(target->table.column_widths);
      for (std::size_t i = 0; i < result.columns.size(); ++i) {
        const std::string& col_name = result.columns[i].name;
        if (!result.column_widths.contains(col_name)) {
          result.column_widths.emplace(
              col_name, ComputeAutoColumnWidth(col_name, i, result.snapshot.get()));
        }
      }
      if (!result.error.empty()) {
        result.error.clear();
      }
      result.snapshot_version = target->table.snapshot_version + 1;
      target->table = std::move(result);
      target->selected_cell = std::nullopt;
    });

    if (!tab_matches) {
      co_return;
    }
    if (!load_error.empty() && !has_snapshot) {
      if (notify) {
        notify(TrFmt("Failed to load: {0}", "加载失败: {0}", {load_error}));
      }
      co_return;
    }
    if (!load_error.empty() && notify) {
      notify(TrFmt("Load warning: {0}", "加载警告: {0}", {load_error}));
    }
  });
}

void RequestSaveCell(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                     const TaskScope& tasks, std::uint64_t tab_id,
                     const DataTableEditRequest& edit, NotifyFn notify) {
  if (edit.cell_id.row_id.starts_with("draft:")) {
    return;
  }
  const WorkspaceTab* tab = workspace.Get().FindTab(tab_id);
  if (tab == nullptr || tab->kind != WorkspaceTabKind::Table) {
    return;
  }
  const DbSession* session = FindSession(app.Get(), tab->session_id);
  if (session == nullptr || !session->session || session->session->IsClosed()) {
    if (session != nullptr) {
      HandleSessionDisconnected(app, workspace, tasks, tab->session_id, {}, notify);
    }
    return;
  }
  TableModel table_value = tab->table;
  if (!table_value.snapshot) {
    return;
  }
  const std::size_t row_index = ParseRowIndex(edit.cell_id.row_id);
  if (row_index >= table_value.snapshot->rows.size()) {
    return;
  }
  std::size_t column_index = table_value.columns.size();
  for (std::size_t i = 0; i < table_value.columns.size(); ++i) {
    if (table_value.columns[i].name == edit.cell_id.column_id) {
      column_index = i;
      break;
    }
  }
  if (column_index >= table_value.columns.size()) {
    return;
  }
  const CellValue& current = table_value.snapshot->rows[row_index].values[column_index];
  if (!current.is_null && current.text == edit.text) {
    return;
  }
  if (current.is_null && EqualsIgnoreCaseAscii(edit.text, "NULL")) {
    return;
  }

  auto adbc_session = session->session;
  const std::uint64_t session_id = session->id;
  const std::string object_name = table_value.object;

  tasks.Launch([app, workspace, tasks, notify = std::move(notify), adbc_session,
                table_value = std::move(table_value), tab_id, session_id, object_name,
                row_index, column_index, text = edit.text]() mutable -> Task<void> {
    AdbcQueryResult result =
        co_await UpdateCellAsync(adbc_session, table_value, row_index, column_index, text);
    if (result.connection_lost) {
      HandleSessionDisconnected(app, workspace, tasks, session_id, result.error, notify);
      co_return;
    }
    if (!result.error.empty()) {
      const std::string message = result.error;
      if (notify) {
        notify(TrFmt("Failed to save: {0}", "保存失败: {0}", {message}));
      }
      co_return;
    }
    const WorkspaceTab* current_tab = workspace.Get().FindTab(tab_id);
    if (current_tab == nullptr || current_tab->kind != WorkspaceTabKind::Table ||
        current_tab->session_id != session_id || current_tab->object_name != object_name) {
      co_return;
    }
    RequestLoadTable(app, workspace, tasks, tab_id, table_value.page, table_value.sort,
                     table_value.filter, true, notify);
  });
}

void RequestDeleteSelected(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                           const TaskScope& tasks, std::uint64_t tab_id, NotifyFn notify) {
  const WorkspaceTab* tab = workspace.Get().FindTab(tab_id);
  if (tab == nullptr || tab->kind != WorkspaceTabKind::Table || !tab->selected_cell.has_value()) {
    return;
  }
  if (tab->selected_cell->row_id.starts_with("draft:")) {
    return;
  }
  const DbSession* session = FindSession(app.Get(), tab->session_id);
  if (session == nullptr || !session->session || session->session->IsClosed()) {
    if (session != nullptr) {
      HandleSessionDisconnected(app, workspace, tasks, tab->session_id, {}, notify);
    }
    return;
  }
  TableModel table_value = tab->table;
  if (!table_value.snapshot) {
    return;
  }
  const std::size_t row_index = ParseRowIndex(tab->selected_cell->row_id);
  if (row_index >= table_value.snapshot->rows.size()) {
    return;
  }
  auto adbc_session = session->session;
  const std::uint64_t session_id = session->id;
  const std::string object_name = table_value.object;
  const std::int64_t page = table_value.page;
  const std::int64_t remaining = static_cast<std::int64_t>(table_value.snapshot->rows.size());

  tasks.Launch([app, workspace, tasks, notify = std::move(notify), adbc_session,
                table_value = std::move(table_value), tab_id, session_id, object_name,
                row_index, page, remaining]() mutable -> Task<void> {
    AdbcQueryResult result = co_await DeleteRowAsync(adbc_session, table_value, row_index);
    if (result.connection_lost) {
      HandleSessionDisconnected(app, workspace, tasks, session_id, result.error, notify);
      co_return;
    }
    if (!result.error.empty()) {
      const std::string message = result.error;
      if (notify) {
        notify(TrFmt("Failed to delete: {0}", "删除失败: {0}", {message}));
      }
      co_return;
    }
    const WorkspaceTab* current_tab = workspace.Get().FindTab(tab_id);
    if (current_tab == nullptr || current_tab->kind != WorkspaceTabKind::Table ||
        current_tab->session_id != session_id || current_tab->object_name != object_name) {
      co_return;
    }
    const std::int64_t next_page = remaining <= 1 && page > 0 ? page - 1 : page;
    RequestLoadTable(app, workspace, tasks, tab_id, next_page, table_value.sort,
                     table_value.filter, false, notify);
    if (notify) {
      notify(std::string(Tr("Deleted 1 row", "已删除 1 行")));
    }
  });
}

void RequestCommitPendingInserts(const State<AppModel>& app,
                                 const State<WorkspaceModel>& workspace,
                                 const TaskScope& tasks, std::uint64_t tab_id,
                                 NotifyFn notify) {
  const WorkspaceTab* tab = workspace.Get().FindTab(tab_id);
  if (tab == nullptr || tab->kind != WorkspaceTabKind::Table ||
      tab->table.pending_inserts.empty() || tab->table.committing) {
    return;
  }
  const DbSession* session = FindSession(app.Get(), tab->session_id);
  if (session == nullptr || !session->session || session->session->IsClosed()) {
    if (session != nullptr) {
      HandleSessionDisconnected(app, workspace, tasks, tab->session_id, {}, notify);
    } else if (notify) {
      notify(std::string(Tr("Connection is closed", "连接已关闭")));
    }
    return;
  }

  auto adbc = session->session;
  const std::uint64_t session_id = tab->session_id;
  const std::string object_name = tab->object_name;
  TableModel table_copy = tab->table;
  std::vector<PendingInsertRow> rows = tab->table.pending_inserts;

  workspace.Update([&](WorkspaceModel& ws) {
    if (WorkspaceTab* target = ws.FindTab(tab_id);
        target != nullptr && target->kind == WorkspaceTabKind::Table) {
      target->table.committing = true;
    }
  });

  tasks.Launch([app, workspace, tasks, notify = std::move(notify), adbc,
                table_copy = std::move(table_copy), rows = std::move(rows), tab_id,
                session_id, object_name]() mutable -> Task<void> {
    BatchInsertResult res = co_await adbc->sequence.Run(
        [adbc, table_copy = std::move(table_copy), rows = std::move(rows)](
            std::stop_token) mutable { return InsertRowsBlocking(*adbc, table_copy, rows); });

    if (res.connection_lost) {
      workspace.Update([&](WorkspaceModel& ws) {
        if (WorkspaceTab* target = ws.FindTab(tab_id);
            target != nullptr && target->kind == WorkspaceTabKind::Table) {
          target->table.committing = false;
        }
      });
      HandleSessionDisconnected(app, workspace, tasks, session_id, res.error, notify);
      co_return;
    }

    if (!res.error.empty()) {
      bool partial_reload = false;
      std::int64_t reload_page = 0;
      std::optional<DataTableSort> reload_sort;
      std::string reload_filter;
      workspace.Update([&](WorkspaceModel& ws) {
        WorkspaceTab* target = ws.FindTab(tab_id);
        if (target == nullptr || target->kind != WorkspaceTabKind::Table ||
            target->session_id != session_id || target->object_name != object_name) {
          return;
        }
        target->table.committing = false;
        if (!res.rolled_back && res.inserted > 0) {
          const std::size_t remove_count =
              std::min(res.inserted, target->table.pending_inserts.size());
          target->table.pending_inserts.erase(target->table.pending_inserts.begin(),
                                              target->table.pending_inserts.begin() +
                                                  static_cast<std::ptrdiff_t>(remove_count));
          partial_reload = true;
          reload_page = target->table.page;
          reload_sort = target->table.sort;
          reload_filter = target->table.filter;
        }
      });
      if (notify) {
        notify(TrFmt("Commit failed: {0}", "提交失败: {0}", {res.error}));
      }
      if (partial_reload) {
        RequestLoadTable(app, workspace, tasks, tab_id, reload_page, reload_sort,
                         std::move(reload_filter), false, notify);
      }
      co_return;
    }

    bool should_reload = false;
    std::int64_t target_page = 0;
    std::optional<DataTableSort> reload_sort;
    std::string reload_filter;
    workspace.Update([&](WorkspaceModel& ws) {
      WorkspaceTab* target = ws.FindTab(tab_id);
      if (target == nullptr || target->kind != WorkspaceTabKind::Table ||
          target->session_id != session_id || target->object_name != object_name) {
        return;
      }
      should_reload = true;
      target->table.pending_inserts.clear();
      target->table.committing = false;
      if (target->selected_cell.has_value() &&
          target->selected_cell->row_id.starts_with("draft:")) {
        target->selected_cell = std::nullopt;
      }
      reload_sort = target->table.sort;
      reload_filter = target->table.filter;
      if (!target->table.sort.has_value() && target->table.filter.empty()) {
        const std::int64_t next_total =
            target->table.total + static_cast<std::int64_t>(res.inserted);
        const std::int64_t page_size = std::max<std::int64_t>(1, target->table.page_size);
        target_page = std::max<std::int64_t>(0, (next_total - 1) / page_size);
      } else {
        target_page = target->table.page;
      }
    });

    if (should_reload) {
      RequestLoadTable(app, workspace, tasks, tab_id, target_page, reload_sort,
                       std::move(reload_filter), false, notify);
    }
    if (notify) {
      notify(TrFmt("Committed {0} row(s)", "已提交 {0} 行", {std::to_string(res.inserted)}));
    }
  });
}

void ActivateWorkspaceTab(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                          std::uint64_t tab_id) {
  if (workspace.Get().FindTab(tab_id) == nullptr) {
    return;
  }
  workspace.Update([tab_id](WorkspaceModel& ws) { ws.active_tab_id = tab_id; });
  const WorkspaceModel& ws_now = workspace.Get();
  app.Update([&ws_now](AppModel& model) { SyncAppFromActiveTab(model, ws_now); });
}

void CloseWorkspaceTabs(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                        const std::vector<std::uint64_t>& tab_ids) {
  if (tab_ids.empty()) {
    return;
  }
  workspace.Update([&tab_ids](WorkspaceModel& ws) {
    const std::uint64_t old_active = ws.active_tab_id;
    std::size_t old_active_index = 0;
    for (std::size_t i = 0; i < ws.tabs.size(); ++i) {
      if (ws.tabs[i].id == old_active) {
        old_active_index = i;
        break;
      }
    }
    const auto should_remove = [&tab_ids](const WorkspaceTab& tab) {
      return std::ranges::find(tab_ids, tab.id) != tab_ids.end();
    };
    const bool active_removed = ws.FindTab(old_active) != nullptr &&
                                std::ranges::find(tab_ids, old_active) != tab_ids.end();
    std::erase_if(ws.tabs, should_remove);
    if (ws.tabs.empty()) {
      ws.active_tab_id = 0;
    } else if (active_removed || ws.FindTab(ws.active_tab_id) == nullptr) {
      const std::size_t next_index = old_active_index > 0
                                         ? std::min(old_active_index - 1, ws.tabs.size() - 1)
                                         : 0;
      ws.active_tab_id = ws.tabs[next_index].id;
    }
  });
  const WorkspaceModel& ws_now = workspace.Get();
  app.Update([&ws_now](AppModel& model) { SyncAppFromActiveTab(model, ws_now); });
}

std::vector<std::string> ListSessionDatabases(const DbSession& session) {
  std::vector<std::string> databases;
  if (session.dialect == Dialect::Redis) {
    for (const DbObject& object : session.objects) {
      if (!object.name.empty() &&
          std::ranges::find(databases, object.name) == databases.end()) {
        databases.push_back(object.name);
      }
    }
    if (session.session && session.session->redis) {
      const std::string current = "db" + std::to_string(session.session->redis->CurrentDb());
      if (std::ranges::find(databases, current) == databases.end()) {
        databases.insert(databases.begin(), current);
      }
    }
    if (databases.empty()) {
      databases.emplace_back("db0");
    }
    return databases;
  }

  const bool multi_catalog = SessionHasMultipleCatalogsCopy(session);
  const DatabasePreset& preset = ResolvePresetForProfile(session.driver_path, session.uri);
  const std::string uri_db = TrimAsciiCopy(DecomposePresetUri(preset, session.uri).database);

  const auto add_unique = [&databases](std::string name) {
    name = TrimAsciiCopy(name);
    if (name.empty()) {
      return;
    }
    const std::string lower = LowerAsciiCopy(name);
    for (const std::string& existing : databases) {
      if (LowerAsciiCopy(existing) == lower) {
        return;
      }
    }
    databases.push_back(std::move(name));
  };

  for (const DbObject& object : session.objects) {
    add_unique(SessionObjectBucketCopy(session, object, multi_catalog));
  }

  if ((session.dialect == Dialect::Mysql || session.dialect == Dialect::Generic) &&
      !uri_db.empty()) {
    add_unique(uri_db);
  }
  if ((session.dialect == Dialect::Sqlite || session.dialect == Dialect::Duckdb) &&
      databases.empty()) {
    databases.emplace_back("main");
  }
  if (session.dialect == Dialect::Postgresql && databases.empty()) {
    databases.emplace_back("public");
  }

  std::string target_lower = LowerAsciiCopy(uri_db);
  if (target_lower.empty() &&
      (session.dialect == Dialect::Sqlite || session.dialect == Dialect::Duckdb)) {
    target_lower = "main";
  } else if (session.dialect == Dialect::Postgresql) {
    const bool matches_uri_db =
        !target_lower.empty() &&
        std::ranges::any_of(databases, [&target_lower](const std::string& db) {
          return LowerAsciiCopy(db) == target_lower;
        });
    if (!matches_uri_db) {
      target_lower = "public";
    }
  }

  std::ranges::sort(databases, [&target_lower](const std::string& a, const std::string& b) {
    const std::string la = LowerAsciiCopy(a);
    const std::string lb = LowerAsciiCopy(b);
    const bool a_target = !target_lower.empty() && la == target_lower;
    const bool b_target = !target_lower.empty() && lb == target_lower;
    if (a_target != b_target) {
      return a_target;
    }
    const bool a_sys = IsSystemDatabaseOrSchema(a);
    const bool b_sys = IsSystemDatabaseOrSchema(b);
    if (a_sys != b_sys) {
      return !a_sys;
    }
    return a < b;
  });
  return databases;
}

std::string DefaultSessionDatabase(const DbSession& session) {
  if (session.dialect == Dialect::Redis && session.session && session.session->redis) {
    return "db" + std::to_string(session.session->redis->CurrentDb());
  }
  const std::vector<std::string> databases = ListSessionDatabases(session);
  return databases.empty() ? std::string{} : databases.front();
}

std::string BuildSwitchDatabaseSql(Dialect dialect, std::string_view target_database) {
  const std::string trimmed = TrimAsciiCopy(target_database);
  if (trimmed.empty() || dialect == Dialect::Sqlite || dialect == Dialect::Redis) {
    return {};
  }
  if (dialect == Dialect::Mysql) {
    return "USE " + QuoteIdentifier(Dialect::Mysql, trimmed);
  }
  if (dialect == Dialect::Postgresql) {
    const std::size_t dot = trimmed.rfind('.');
    const std::string_view schema =
        (dot != std::string_view::npos && dot + 1 < trimmed.size())
            ? std::string_view(trimmed).substr(dot + 1)
            : std::string_view(trimmed);
    return "SET search_path TO " + QuoteIdentifier(Dialect::Postgresql, schema);
  }
  std::string joined;
  std::size_t pos = 0;
  while (pos < trimmed.size()) {
    const std::size_t next = trimmed.find('.', pos);
    const std::string_view part =
        next == std::string_view::npos
            ? std::string_view(trimmed).substr(pos)
            : std::string_view(trimmed).substr(pos, next - pos);
    if (!part.empty()) {
      if (!joined.empty()) {
        joined.push_back('.');
      }
      joined += QuoteIdentifier(dialect, part);
    }
    if (next == std::string_view::npos) {
      break;
    }
    pos = next + 1;
  }
  if (joined.empty()) {
    return {};
  }
  return "USE " + joined;
}

std::uint64_t OpenSqlTab(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                         std::uint64_t session_id, std::string initial_sql,
                         std::string custom_title, std::string initial_database) {
  const DbSession* session = FindSession(app.Get(), session_id);
  if (session == nullptr) {
    return 0;
  }
  std::string resolved_db = TrimAsciiCopy(initial_database);
  if (resolved_db.empty()) {
    resolved_db = DefaultSessionDatabase(*session);
  }
  std::uint64_t created_id = 0;
  workspace.Update([&](WorkspaceModel& ws) {
    WorkspaceTab tab;
    tab.id = ws.next_tab_id++;
    created_id = tab.id;
    tab.kind = WorkspaceTabKind::Sql;
    tab.session_id = session_id;
    tab.session_name = session->name;
    tab.session_uri = session->uri;
    const bool is_redis = session->dialect == Dialect::Redis;
    const std::string tab_prefix = is_redis ? "Redis " : std::string(Tr("Query ", "查询 "));
    tab.title = custom_title.empty()
                    ? (tab_prefix + std::to_string(ws.next_sql_index++))
                    : std::move(custom_title);
    tab.sql.editor = TextEditingValue::FromText(
        initial_sql.empty() ? std::string{is_redis ? "PING" : "SELECT 1;"}
                            : std::move(initial_sql));
    tab.sql.target_database = std::move(resolved_db);
    ws.tabs.push_back(std::move(tab));
    ws.active_tab_id = created_id;
  });
  app.Update([session_id](AppModel& model) {
    model.active_session = session_id;
    model.active_object.clear();
    model.active_is_view = false;
  });
  return created_id;
}

bool SwitchSqlTabSession(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                         std::uint64_t tab_id, std::uint64_t session_id) {
  const DbSession* session = FindSession(app.Get(), session_id);
  if (session == nullptr || !session->session || session->session->IsClosed()) {
    return false;
  }
  const std::string target_name = session->name;
  const std::string target_uri = session->uri;
  const bool target_is_redis = session->dialect == Dialect::Redis;
  const std::string target_db = DefaultSessionDatabase(*session);

  bool switched = false;
  bool sync_app = false;
  workspace.Update([&](WorkspaceModel& ws) {
    WorkspaceTab* target = ws.FindTab(tab_id);
    if (target == nullptr || target->kind != WorkspaceTabKind::Sql || target->sql.running) {
      return;
    }
    if (target->session_id == session_id && target->session_name == target_name &&
        target->session_uri == target_uri) {
      return;
    }
    target->session_id = session_id;
    target->session_name = target_name;
    target->session_uri = target_uri;
    target->sql.target_database = target_db;
    target->sql.page = 0;
    target->sql.paginated = false;
    target->sql.has_next_page = false;
    target->sql.last_sql.clear();
    target->sql.last_database.clear();
    target->sql.error.clear();
    target->sql.status.clear();
    const std::string trimmed = TrimAsciiCopy(target->sql.editor.text);
    if (target_is_redis && trimmed == "SELECT 1;") {
      target->sql.editor = TextEditingValue::FromText("PING");
    } else if (!target_is_redis && trimmed == "PING") {
      target->sql.editor = TextEditingValue::FromText("SELECT 1;");
    }
    switched = true;
    sync_app = (ws.active_tab_id == tab_id);
  });
  if (switched && sync_app) {
    app.Update([session_id](AppModel& model) {
      model.active_session = session_id;
      model.active_object.clear();
      model.active_is_view = false;
    });
  }
  return switched;
}

bool SwitchSqlTabDatabase(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                          std::uint64_t tab_id, std::string database) {
  (void)app;
  std::string next_db = TrimAsciiCopy(database);
  bool switched = false;
  workspace.Update([&](WorkspaceModel& ws) {
    WorkspaceTab* target = ws.FindTab(tab_id);
    if (target == nullptr || target->kind != WorkspaceTabKind::Sql || target->sql.running) {
      return;
    }
    if (target->sql.target_database == next_db) {
      return;
    }
    target->sql.target_database = std::move(next_db);
    target->sql.page = 0;
    target->sql.paginated = false;
    target->sql.has_next_page = false;
    target->sql.last_sql.clear();
    target->sql.last_database.clear();
    target->sql.error.clear();
    switched = true;
  });
  return switched;
}

std::uint64_t OpenSettingsTab(const State<AppModel>& app, const State<WorkspaceModel>& workspace) {
  if (const WorkspaceTab* existing = workspace.Get().FindSettingsTab()) {
    ActivateWorkspaceTab(app, workspace, existing->id);
    return existing->id;
  }
  std::uint64_t created_id = 0;
  workspace.Update([&](WorkspaceModel& ws) {
    if (const WorkspaceTab* existing = ws.FindSettingsTab()) {
      created_id = existing->id;
      ws.active_tab_id = created_id;
      return;
    }
    WorkspaceTab tab;
    tab.id = ws.next_tab_id++;
    created_id = tab.id;
    tab.kind = WorkspaceTabKind::Settings;
    tab.session_id = 0;
    tab.title = std::string(Tr("Settings", "设置"));
    ws.tabs.push_back(std::move(tab));
    ws.active_tab_id = created_id;
  });
  const WorkspaceModel& ws_now = workspace.Get();
  app.Update([&ws_now](AppModel& model) { SyncAppFromActiveTab(model, ws_now); });
  return created_id;
}

void SelectObject(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                  const TaskScope& tasks, std::uint64_t session_id, std::string object,
                  std::string db_schema, bool is_view, NotifyFn notify) {
  if (const WorkspaceTab* existing =
          workspace.Get().FindTableTab(session_id, object, db_schema, is_view)) {
    ActivateWorkspaceTab(app, workspace, existing->id);
    return;
  }
  const DbSession* session = FindSession(app.Get(), session_id);
  if (session == nullptr) {
    return;
  }

  const std::int64_t initial_page_size =
      app.Get().default_page_size > 0 ? app.Get().default_page_size : kPageSize;
  std::uint64_t created_id = 0;
  workspace.Update([&](WorkspaceModel& ws) {
    WorkspaceTab tab;
    tab.id = ws.next_tab_id++;
    created_id = tab.id;
    tab.kind = WorkspaceTabKind::Table;
    tab.session_id = session_id;
    tab.session_name = session->name;
    tab.session_uri = session->uri;
    tab.title = FormatTabTitle(*session, object, db_schema);
    tab.object_name = std::move(object);
    tab.object_schema = std::move(db_schema);
    tab.is_view = is_view;
    tab.table.page_size = initial_page_size;
    ws.tabs.push_back(std::move(tab));
    ws.active_tab_id = created_id;
  });
  const WorkspaceModel& ws_now = workspace.Get();
  app.Update([&ws_now](AppModel& model) {
    SyncAppFromActiveTab(model, ws_now);
    model.error.clear();
  });
  RequestLoadTable(app, workspace, tasks, created_id, 0, std::nullopt, {}, false,
                   std::move(notify), initial_page_size);
}

void OpenConnection(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                    const TaskScope& tasks, ConnectionRequest request, NotifyFn notify,
                    std::function<void(std::uint64_t)> on_opened) {
  app.Update([&](AppModel& model) {
    model.error.clear();
    model.status =
        TrFmt("Connecting to {0}...", "正在连接 {0}…",
              {DisplayNameFromUri(request.uri, Tr("Database", "数据库"))});
  });
  tasks.Launch([app, workspace, tasks, request = std::move(request),
                notify = std::move(notify),
                on_opened = std::move(on_opened)]() mutable -> Task<void> {
    AdbcOpenResult opened =
        co_await RunWorker([request] { return OpenAdbcBlocking(request); });
    if (!opened.session) {
      const std::string message =
          TrFmt("Connection failed: {0}", "连接失败: {0}", {opened.error});
      app.Update([](AppModel& model) {
        model.status.clear();
        model.error.clear();
      });
      if (notify) {
        notify(message);
      }
      co_return;
    }

    AdbcObjectsResult objects = co_await opened.session->sequence.Run(
        [session = opened.session](std::stop_token) {
          return LoadObjectsBlocking(*session);
        });
    ApplyOpenedSession(app, workspace, tasks, std::move(opened), std::move(request),
                       std::move(objects), std::move(notify), std::move(on_opened));
  });
}

void CloseSession(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                  const TaskScope& tasks, std::uint64_t session_id, NotifyFn notify) {
  (void)notify;
  std::shared_ptr<AdbcSession> adbc_to_close;
  std::vector<std::uint64_t> tabs_to_close;
  for (const WorkspaceTab& tab : workspace.Get().tabs) {
    if (tab.session_id == session_id) {
      tabs_to_close.push_back(tab.id);
    }
  }
  app.Update([&](AppModel& model) {
    for (DbSession& session : model.sessions) {
      if (session.id == session_id) {
        adbc_to_close = std::move(session.session);
        break;
      }
    }
    std::erase_if(model.sessions, [session_id](const DbSession& session) {
      return session.id == session_id;
    });
    if (model.sessions.empty()) {
      model.active_session = 0;
      model.active_object.clear();
      model.active_is_view = false;
      model.status.clear();
    } else if (model.active_session == session_id) {
      model.active_session = model.sessions.front().id;
      model.active_object.clear();
      model.active_is_view = false;
    }
    ++model.revision;
  });
  CloseSessionAsync(std::move(adbc_to_close), tasks);
  if (!tabs_to_close.empty()) {
    CloseWorkspaceTabs(app, workspace, tabs_to_close);
  }
}

void HandleSessionDisconnected(const State<AppModel>& app,
                                const State<WorkspaceModel>& workspace,
                                const TaskScope& tasks, std::uint64_t session_id,
                                std::string reason, NotifyFn notify) {
  std::shared_ptr<AdbcSession> adbc_to_close;
  std::string session_name;
  bool removed = false;
  app.Update([&](AppModel& model) {
    for (auto it = model.sessions.begin(); it != model.sessions.end(); ++it) {
      if (it->id == session_id) {
        session_name = it->name;
        adbc_to_close = std::move(it->session);
        model.sessions.erase(it);
        removed = true;
        break;
      }
    }
    if (!removed) {
      return;
    }
    if (model.sessions.empty()) {
      model.active_session = 0;
      model.active_object.clear();
      model.active_is_view = false;
      model.status.clear();
    } else if (model.active_session == session_id) {
      model.active_session = model.sessions.front().id;
      model.active_object.clear();
      model.active_is_view = false;
    }
    ++model.revision;
  });
  if (!removed) {
    return;
  }
  CloseSessionAsync(std::move(adbc_to_close), tasks);

  // Close clean table tabs for the lost session, while keeping SQL tabs and
  // dirty table tabs so unsaved drafts and editor queries remain intact.
  std::vector<std::uint64_t> tabs_to_close;
  for (const WorkspaceTab& tab : workspace.Get().tabs) {
    if (tab.session_id == session_id && tab.kind == WorkspaceTabKind::Table &&
        tab.table.pending_inserts.empty()) {
      tabs_to_close.push_back(tab.id);
    }
  }
  if (!tabs_to_close.empty()) {
    CloseWorkspaceTabs(app, workspace, tabs_to_close);
  }

  if (notify) {
    notify(reason.empty()
               ? TrFmt("Connection {0} disconnected", "连接 {0} 已断开", {session_name})
               : TrFmt("Connection {0} disconnected: {1}", "连接 {0} 已断开: {1}",
                       {session_name, reason}));
  }
}

void CloseAllSessions(const State<AppModel>& app, const TaskScope& tasks) {
  std::vector<std::shared_ptr<AdbcSession>> closing;
  app.Update([&](AppModel& model) {
    closing.reserve(model.sessions.size());
    for (DbSession& session : model.sessions) {
      if (session.session) {
        closing.push_back(std::move(session.session));
      }
    }
    model.sessions.clear();
    model.active_session = 0;
    model.active_object.clear();
    model.active_is_view = false;
    model.status.clear();
    ++model.revision;
  });
  for (std::shared_ptr<AdbcSession>& adbc : closing) {
    CloseSessionAsync(std::move(adbc), tasks);
  }
}

void ScheduleSessionHealthCheck(const State<AppModel>& app,
                                const State<WorkspaceModel>& workspace,
                                const TaskScope& tasks, NotifyFn notify) {
  struct ProbeTarget {
    std::uint64_t session_id = 0;
    std::shared_ptr<AdbcSession> adbc;
  };
  std::vector<ProbeTarget> targets;
  {
    const AppModel& model = app.Get();
    targets.reserve(model.sessions.size());
    for (const DbSession& session : model.sessions) {
      if (session.session && !session.session->IsClosed() &&
          ShouldHealthCheckSession(session.dialect, session.uri)) {
        targets.push_back(ProbeTarget{.session_id = session.id, .adbc = session.session});
      }
    }
  }
  for (ProbeTarget& target : targets) {
    if (target.adbc->ping_in_flight.exchange(true, std::memory_order_acq_rel)) {
      continue;
    }
    tasks.Launch([app, workspace, tasks, notify, session_id = target.session_id,
                  adbc = std::move(target.adbc)]() mutable -> Task<void> {
      struct InFlightGuard {
        AdbcSession* ptr = nullptr;
        ~InFlightGuard() {
          if (ptr != nullptr) {
            ptr->ping_in_flight.store(false, std::memory_order_release);
          }
        }
      } guard{adbc.get()};

      const AdbcQueryResult ping = co_await adbc->sequence.Run(
          [adbc](std::stop_token) { return PingSessionBlocking(*adbc); });
      if (ping.connection_lost && !adbc->IsClosed()) {
        HandleSessionDisconnected(app, workspace, tasks, session_id, ping.error, notify);
      }
    });
  }
}

void RefreshSessionObjects(const State<AppModel>& app, const State<WorkspaceModel>& workspace,
                           const TaskScope& tasks, std::uint64_t session_id, NotifyFn notify,
                           bool notify_success) {
  const AppModel& snapshot = app.Get();
  const DbSession* session = FindSession(snapshot, session_id);
  if (session == nullptr || !session->session || session->session->IsClosed()) {
    if (session != nullptr) {
      HandleSessionDisconnected(app, workspace, tasks, session_id, {}, notify);
    }
    return;
  }

  auto adbc = session->session;
  std::string session_name = session->name;
  tasks.Launch([app, workspace, tasks, notify = std::move(notify), adbc, session_id,
                session_name = std::move(session_name), notify_success]() mutable -> Task<void> {
    AdbcObjectsResult objects = co_await adbc->sequence.Run(
        [adbc](std::stop_token) { return LoadObjectsBlocking(*adbc); });
    if (objects.connection_lost) {
      HandleSessionDisconnected(app, workspace, tasks, session_id, objects.error, notify);
      co_return;
    }
    if (!objects.error.empty()) {
      if (notify) {
        notify(TrFmt("Refresh failed: {0}", "刷新失败: {0}", {objects.error}));
      }
      co_return;
    }

    bool session_present = false;
    const std::size_t object_count = objects.objects.size();
    app.Update([&](AppModel& value) {
      for (DbSession& open : value.sessions) {
        if (open.id != session_id) {
          continue;
        }
        session_present = true;
        open.objects = std::move(objects.objects);
        ++value.revision;
        break;
      }
    });

    if (!session_present) {
      co_return;
    }

    const DbSession* refreshed = FindSession(app.Get(), session_id);
    std::vector<std::uint64_t> stale_tabs;
    if (refreshed != nullptr) {
      for (const WorkspaceTab& tab : workspace.Get().tabs) {
        if (tab.session_id == session_id && tab.kind == WorkspaceTabKind::Table &&
            FindObject(*refreshed, tab.object_name, tab.object_schema) == nullptr) {
          stale_tabs.push_back(tab.id);
        }
      }
    }
    if (!stale_tabs.empty()) {
      CloseWorkspaceTabs(app, workspace, stale_tabs);
    }

    const WorkspaceTab* active_tab = workspace.Get().ActiveTab();
    if (active_tab != nullptr && active_tab->kind == WorkspaceTabKind::Table &&
        active_tab->session_id == session_id && active_tab->table.pending_inserts.empty()) {
      RequestLoadTable(app, workspace, tasks, active_tab->id, active_tab->table.page,
                       active_tab->table.sort, active_tab->table.filter, true, notify);
    }
    if (notify && notify_success) {
      notify(TrFmt("Refreshed {0} ({1} objects)", "已刷新 {0}（{1} 个对象）",
                   {session_name, std::to_string(object_count)}));
    }
  });
}

namespace {

std::string EncodeProfileField(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '%': result += "%25"; break;
      case '\t': result += "%09"; break;
      case '\n': result += "%0A"; break;
      case '\r': result += "%0D"; break;
      default: result.push_back(c); break;
    }
  }
  return result;
}

std::string DecodeProfileField(std::string_view text) {
  const auto hex_value = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string result;
  result.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      const int high = hex_value(text[i + 1]);
      const int low = hex_value(text[i + 2]);
      if (high >= 0 && low >= 0) {
        result.push_back(static_cast<char>((high << 4) | low));
        i += 2;
        continue;
      }
    }
    result.push_back(text[i]);
  }
  return result;
}

bool IsWideDisplayCodepoint(std::uint32_t cp) {
  return (cp >= 0x1100U && cp <= 0x115FU) || (cp >= 0x2E80U && cp <= 0x303EU) ||
         (cp >= 0x3040U && cp <= 0x33FFU) || (cp >= 0x3400U && cp <= 0x4DBFU) ||
         (cp >= 0x4E00U && cp <= 0x9FFFU) || (cp >= 0xA960U && cp <= 0xA97FU) ||
         (cp >= 0xAC00U && cp <= 0xD7A3U) || (cp >= 0xF900U && cp <= 0xFAFFU) ||
         (cp >= 0xFE10U && cp <= 0xFE6FU) || (cp >= 0xFF00U && cp <= 0xFF60U) ||
         (cp >= 0xFFE0U && cp <= 0xFFE6U) || cp >= 0x1F000U;
}

std::size_t Utf8CodepointLength(std::string_view text, std::size_t index, std::uint32_t* codepoint) {
  const auto b0 = static_cast<unsigned char>(text[index]);
  if (b0 < 0x80U) {
    if (codepoint != nullptr) {
      *codepoint = b0;
    }
    return 1;
  }
  const auto is_cont = [&](std::size_t pos) {
    return pos < text.size() && (static_cast<unsigned char>(text[pos]) & 0xC0U) == 0x80U;
  };
  if ((b0 & 0xE0U) == 0xC0U && is_cont(index + 1)) {
    if (codepoint != nullptr) {
      *codepoint = ((static_cast<std::uint32_t>(b0 & 0x1FU)) << 6U) |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(text[index + 1]) & 0x3FU);
    }
    return 2;
  }
  if ((b0 & 0xF0U) == 0xE0U && is_cont(index + 1) && is_cont(index + 2)) {
    if (codepoint != nullptr) {
      *codepoint =
          ((static_cast<std::uint32_t>(b0 & 0x0FU)) << 12U) |
          ((static_cast<std::uint32_t>(static_cast<unsigned char>(text[index + 1]) & 0x3FU)) << 6U) |
          static_cast<std::uint32_t>(static_cast<unsigned char>(text[index + 2]) & 0x3FU);
    }
    return 3;
  }
  if ((b0 & 0xF8U) == 0xF0U && is_cont(index + 1) && is_cont(index + 2) && is_cont(index + 3)) {
    if (codepoint != nullptr) {
      *codepoint =
          ((static_cast<std::uint32_t>(b0 & 0x07U)) << 18U) |
          ((static_cast<std::uint32_t>(static_cast<unsigned char>(text[index + 1]) & 0x3FU)) << 12U) |
          ((static_cast<std::uint32_t>(static_cast<unsigned char>(text[index + 2]) & 0x3FU)) << 6U) |
          static_cast<std::uint32_t>(static_cast<unsigned char>(text[index + 3]) & 0x3FU);
    }
    return 4;
  }
  if (codepoint != nullptr) {
    *codepoint = b0;
  }
  return 1;
}

float EstimateDisplayUnits(std::string_view text, float stop_units) {
  float units = 0.0F;
  std::size_t i = 0;
  while (i < text.size()) {
    std::uint32_t cp = 0;
    const std::size_t len = Utf8CodepointLength(text, i, &cp);
    if (cp < 0x80U) {
      units += 1.0F;
    } else if (IsWideDisplayCodepoint(cp)) {
      units += 1.8F;
    } else {
      units += 1.1F;
    }
    if (units >= stop_units) {
      return stop_units;
    }
    i += len;
  }
  return units;
}

} // namespace

std::string SingleLineCellPreview(std::string_view text) {
  std::string sanitized;
  if (!IsValidUtf8(text)) {
    sanitized = SanitizeUtf8(text);
    text = sanitized;
  }
  constexpr std::size_t kMaxPreviewBytes = 1024;
  std::string out;
  out.reserve(std::min(text.size(), kMaxPreviewBytes));
  bool truncated = false;
  std::size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '\r' || c == '\n' || c == '\t') {
      if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
        ++i;
      }
      if (out.empty() || out.back() != ' ') {
        if (out.size() + 1 > kMaxPreviewBytes) {
          truncated = true;
          break;
        }
        out.push_back(' ');
      }
      ++i;
      continue;
    }
    const std::size_t len = Utf8CodepointLength(text, i, nullptr);
    if (out.size() + len > kMaxPreviewBytes) {
      truncated = true;
      break;
    }
    out.append(text.substr(i, len));
    i += len;
  }
  if (truncated) {
    out += "…";
  }
  return out;
}

float ComputeAutoColumnWidth(std::string_view header, std::size_t col_index,
                             const GridSnapshot* snapshot) {
  constexpr std::size_t kMaxSampleRows = 50;
  constexpr float kMaxMeasuredUnits = 64.0F;
  const float header_units =
      EstimateDisplayUnits(SingleLineCellPreview(header), kMaxMeasuredUnits);
  const float header_px = header_units * 8.5F + 44.0F;

  float content_px = 0.0F;
  if (snapshot != nullptr && !snapshot->rows.empty()) {
    float max_cell_units = 0.0F;
    const std::size_t row_limit = std::min(snapshot->rows.size(), kMaxSampleRows);
    for (std::size_t row_idx = 0; row_idx < row_limit; ++row_idx) {
      const GridRow& row = snapshot->rows[row_idx];
      if (col_index >= row.values.size()) {
        continue;
      }
      const CellValue& cell = row.values[col_index];
      const float units =
          cell.is_null ? 4.0F
                       : EstimateDisplayUnits(SingleLineCellPreview(cell.text), kMaxMeasuredUnits);
      max_cell_units = std::max(max_cell_units, units);
      if (max_cell_units >= kMaxMeasuredUnits) {
        break;
      }
    }
    content_px = max_cell_units * 8.2F + 26.0F;
  }

  const float raw_width = std::max(header_px, content_px);
  return std::round(std::clamp(raw_width, kAutoMinColumnWidth, kAutoMaxColumnWidth));
}

std::vector<ConnectionProfile> LoadConnectionProfilesFromTsv(const std::string& path) {
  std::vector<ConnectionProfile> profiles;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return profiles;
  }
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line.front() == '#') {
      continue;
    }
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start <= line.size() && fields.size() < 5) {
      const std::size_t tab = line.find('\t', start);
      if (tab == std::string::npos) {
        fields.emplace_back(line.data() + start, line.size() - start);
        break;
      }
      fields.emplace_back(line.data() + start, tab - start);
      start = tab + 1;
    }
    // Accept both legacy 4-column rows (no password) and 5-column rows (with password).
    if (fields.size() != 4 && fields.size() != 5) {
      continue;
    }
    ConnectionProfile profile;
    profile.name = DecodeProfileField(fields[0]);
    profile.driver_path = DecodeProfileField(fields[1]);
    profile.uri = DecodeProfileField(fields[2]);
    profile.username = DecodeProfileField(fields[3]);
    if (fields.size() == 5) {
      profile.password = DecodeProfileField(fields[4]);
    }
    if (!profile.driver_path.empty() && !profile.uri.empty()) {
      profiles.push_back(std::move(profile));
    }
  }
  return profiles;
}

bool SaveConnectionProfilesToTsv(const std::string& path,
                                 const std::vector<ConnectionProfile>& profiles) {
  const std::filesystem::path file_path(path);
  if (file_path.has_parent_path()) {
    std::error_code error;
    std::filesystem::create_directories(file_path.parent_path(), error);
  }
  const std::filesystem::path temp_path =
      file_path.parent_path() / (file_path.filename().string() + ".tmp");
  {
    std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      return false;
    }
    output << "# db++ adbc connections v2\n";
    for (const ConnectionProfile& profile : profiles) {
      output << EncodeProfileField(profile.name) << '\t'
             << EncodeProfileField(profile.driver_path) << '\t'
             << EncodeProfileField(profile.uri) << '\t'
             << EncodeProfileField(profile.username) << '\t'
             << EncodeProfileField(profile.password) << '\n';
    }
    output.flush();
    if (!output) {
      std::error_code ec;
      std::filesystem::remove(temp_path, ec);
      return false;
    }
  }
#if !defined(_WIN32)
  {
    std::error_code perm_ec;
    std::filesystem::permissions(
        temp_path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, perm_ec);
  }
#endif
  std::error_code error;
  std::filesystem::rename(temp_path, file_path, error);
  if (error) {
    std::error_code remove_ec;
    std::filesystem::remove(file_path, remove_ec);
    error.clear();
    std::filesystem::rename(temp_path, file_path, error);
    if (error) {
      std::error_code cleanup_ec;
      std::filesystem::remove(temp_path, cleanup_ec);
      return false;
    }
  }
  return true;
}

struct SqliteDbGuard {
  sqlite3* db = nullptr;
  SqliteDbGuard() = default;
  SqliteDbGuard(const SqliteDbGuard&) = delete;
  SqliteDbGuard& operator=(const SqliteDbGuard&) = delete;
  ~SqliteDbGuard() {
    if (db != nullptr) {
      sqlite3_close_v2(db);
      db = nullptr;
    }
  }
};

struct SqliteStmtGuard {
  sqlite3_stmt* stmt = nullptr;
  SqliteStmtGuard() = default;
  SqliteStmtGuard(const SqliteStmtGuard&) = delete;
  SqliteStmtGuard& operator=(const SqliteStmtGuard&) = delete;
  ~SqliteStmtGuard() {
    if (stmt != nullptr) {
      sqlite3_finalize(stmt);
      stmt = nullptr;
    }
  }
};

std::string ColumnTextString(sqlite3_stmt* stmt, int col) {
  const unsigned char* ptr = sqlite3_column_text(stmt, col);
  const int bytes = sqlite3_column_bytes(stmt, col);
  if (ptr == nullptr || bytes <= 0) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(ptr), static_cast<std::size_t>(bytes));
}

std::vector<ConnectionProfile> LoadConnectionProfiles(const std::string& path) {
  if (path.empty()) {
    return {};
  }
  if (detail::IsLegacyTsvPath(path)) {
    return LoadConnectionProfilesFromTsv(path);
  }

  std::error_code ec;
  const std::filesystem::path target(path);
  const bool db_exists = std::filesystem::is_regular_file(target, ec);
  const std::filesystem::path legacy_tsv = target.parent_path() / "connections.tsv";
  const std::filesystem::path legacy_settings_tsv = target.parent_path() / "settings.tsv";
  if (!db_exists) {
    if (std::filesystem::is_regular_file(legacy_tsv, ec) ||
        std::filesystem::is_regular_file(legacy_settings_tsv, ec)) {
      if (!detail::EnsureConfigSqliteSync(path)) {
        return LoadConnectionProfilesFromTsv(legacy_tsv.string());
      }
    } else {
      return {};
    }
  }

  SqliteDbGuard db_guard;
  if (sqlite3_open_v2(path.c_str(), &db_guard.db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK ||
      db_guard.db == nullptr) {
    return {};
  }
  sqlite3_busy_timeout(db_guard.db, 5000);

  SqliteStmtGuard stmt_guard;
  std::vector<ConnectionProfile> profiles;
  if (sqlite3_prepare_v2(
          db_guard.db,
          "SELECT name, driver_path, uri, username, password "
          "FROM connections ORDER BY sort_order ASC, id ASC;",
          -1, &stmt_guard.stmt, nullptr) == SQLITE_OK) {
    while (sqlite3_step(stmt_guard.stmt) == SQLITE_ROW) {
      ConnectionProfile profile;
      profile.name = ColumnTextString(stmt_guard.stmt, 0);
      profile.driver_path = ColumnTextString(stmt_guard.stmt, 1);
      profile.uri = ColumnTextString(stmt_guard.stmt, 2);
      profile.username = ColumnTextString(stmt_guard.stmt, 3);
      profile.password = ColumnTextString(stmt_guard.stmt, 4);
      if (!profile.driver_path.empty() && !profile.uri.empty()) {
        profiles.push_back(std::move(profile));
      }
    }
  }
  return profiles;
}

bool SaveConnectionProfiles(const std::string& path,
                            const std::vector<ConnectionProfile>& profiles) {
  if (path.empty()) {
    return false;
  }
  if (detail::IsLegacyTsvPath(path)) {
    return SaveConnectionProfilesToTsv(path, profiles);
  }
  if (!detail::EnsureConfigSqliteSync(path)) {
    return false;
  }

  bool ok = false;
  {
    SqliteDbGuard db_guard;
    if (sqlite3_open_v2(path.c_str(), &db_guard.db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK ||
        db_guard.db == nullptr) {
      return false;
    }
    sqlite3_busy_timeout(db_guard.db, 5000);

    ok = sqlite3_exec(db_guard.db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK;
    if (ok) {
      ok = sqlite3_exec(db_guard.db, "DELETE FROM connections;", nullptr, nullptr, nullptr) ==
           SQLITE_OK;
    }
    if (ok && !profiles.empty()) {
      SqliteStmtGuard stmt_guard;
      if (sqlite3_prepare_v2(
              db_guard.db,
              "INSERT INTO connections "
              "(sort_order, name, driver_path, uri, username, password) "
              "VALUES (?, ?, ?, ?, ?, ?);",
              -1, &stmt_guard.stmt, nullptr) != SQLITE_OK) {
        ok = false;
      } else {
        for (std::size_t i = 0; i < profiles.size(); ++i) {
          const auto& p = profiles[i];
          sqlite3_reset(stmt_guard.stmt);
          sqlite3_clear_bindings(stmt_guard.stmt);
          sqlite3_bind_int64(stmt_guard.stmt, 1, static_cast<sqlite3_int64>(i));
          sqlite3_bind_text(stmt_guard.stmt, 2, p.name.data(), static_cast<int>(p.name.size()),
                            SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt_guard.stmt, 3, p.driver_path.data(),
                            static_cast<int>(p.driver_path.size()), SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt_guard.stmt, 4, p.uri.data(), static_cast<int>(p.uri.size()),
                            SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt_guard.stmt, 5, p.username.data(),
                            static_cast<int>(p.username.size()), SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt_guard.stmt, 6, p.password.data(),
                            static_cast<int>(p.password.size()), SQLITE_TRANSIENT);
          if (sqlite3_step(stmt_guard.stmt) != SQLITE_DONE) {
            ok = false;
            break;
          }
        }
      }
    }
    if (ok) {
      ok = sqlite3_exec(db_guard.db, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK;
    }
    if (!ok) {
      sqlite3_exec(db_guard.db, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
  }
  if (ok) {
    detail::RestrictConfigFilePermissions(path);
  }
  return ok;
}

std::vector<std::string> OrderConnectionKeys(const std::vector<std::string>& natural_keys,
                                             const std::vector<std::string>& custom_order) {
  std::vector<std::string> ordered;
  ordered.reserve(natural_keys.size());
  for (const std::string& key : custom_order) {
    if (std::ranges::find(natural_keys, key) != natural_keys.end() &&
        std::ranges::find(ordered, key) == ordered.end()) {
      ordered.push_back(key);
    }
  }
  for (const std::string& key : natural_keys) {
    if (std::ranges::find(ordered, key) == ordered.end()) {
      ordered.push_back(key);
    }
  }
  return ordered;
}

std::vector<std::string> ReorderConnectionKeys(const std::vector<std::string>& natural_keys,
                                               const std::vector<std::string>& custom_order,
                                               std::string_view from_key,
                                               std::string_view to_key) {
  std::vector<std::string> ordered = OrderConnectionKeys(natural_keys, custom_order);
  const auto from_it = std::ranges::find(ordered, from_key);
  const auto to_it = std::ranges::find(ordered, to_key);
  if (from_it == ordered.end() || to_it == ordered.end() || from_it == to_it) {
    return ordered;
  }
  const std::size_t from_idx = static_cast<std::size_t>(from_it - ordered.begin());
  const std::size_t to_idx = static_cast<std::size_t>(to_it - ordered.begin());
  std::string moved = std::move(ordered[from_idx]);
  ordered.erase(ordered.begin() + static_cast<std::ptrdiff_t>(from_idx));
  ordered.insert(ordered.begin() + static_cast<std::ptrdiff_t>(to_idx), std::move(moved));
  return ordered;
}

SqlLimitRewriteResult ApplyImplicitSqlLimit(Dialect dialect, std::string_view sql,
                                            std::int64_t page_size, std::int64_t page) {
  if (dialect == Dialect::Redis || page_size <= 0) {
    return {std::string(sql), false, 0, 0};
  }

  struct Depth0Token {
    bool is_word = false;
    std::string upper;
  };
  std::vector<Depth0Token> depth0_tokens;
  int depth = 0;
  bool seen_semicolon = false;
  std::size_t body_end = 0;
  std::size_t i = 0;
  const std::size_t n = sql.size();

  const auto is_word_start = [](unsigned char ch) {
    return std::isalpha(ch) != 0 || ch == '_';
  };
  const auto is_word_char = [](unsigned char ch) {
    return std::isalnum(ch) != 0 || ch == '_';
  };

  while (i < n) {
    const unsigned char ch = static_cast<unsigned char>(sql[i]);
    if (std::isspace(ch) != 0) {
      ++i;
      continue;
    }
    if (ch == '-' && i + 1 < n && sql[i + 1] == '-') {
      i += 2;
      while (i < n && sql[i] != '\n') {
        ++i;
      }
      continue;
    }
    if (ch == '/' && i + 1 < n && sql[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(sql[i] == '*' && sql[i + 1] == '/')) {
        ++i;
      }
      i = (i + 1 < n) ? (i + 2) : n;
      continue;
    }

    if (depth == 0 && ch == ';') {
      seen_semicolon = true;
      ++i;
      continue;
    }
    if (seen_semicolon) {
      return {std::string(sql), false, 0};
    }

    if (ch == '\'') {
      ++i;
      while (i < n) {
        if (dialect == Dialect::Mysql && sql[i] == '\\' && i + 1 < n) {
          i += 2;
          continue;
        }
        if (sql[i] == '\'') {
          if (i + 1 < n && sql[i + 1] == '\'') {
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      body_end = i;
      if (depth == 0) {
        depth0_tokens.push_back({false, {}});
      }
      continue;
    }

    if (ch == '"' || ch == '`') {
      const char quote = static_cast<char>(ch);
      ++i;
      while (i < n) {
        if (sql[i] == quote) {
          if (i + 1 < n && sql[i + 1] == quote) {
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      body_end = i;
      if (depth == 0) {
        depth0_tokens.push_back({false, {}});
      }
      continue;
    }

    if (ch == '[') {
      ++i;
      while (i < n) {
        if (sql[i] == ']') {
          if (i + 1 < n && sql[i + 1] == ']') {
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      body_end = i;
      if (depth == 0) {
        depth0_tokens.push_back({false, {}});
      }
      continue;
    }

    if (ch == '$') {
      std::size_t tag_end = i + 1;
      while (tag_end < n && is_word_char(static_cast<unsigned char>(sql[tag_end]))) {
        ++tag_end;
      }
      if (tag_end < n && sql[tag_end] == '$') {
        const std::string_view tag = sql.substr(i, tag_end - i + 1);
        const std::size_t close_pos = sql.find(tag, tag_end + 1);
        if (close_pos != std::string_view::npos) {
          i = close_pos + tag.size();
          body_end = i;
          if (depth == 0) {
            depth0_tokens.push_back({false, {}});
          }
          continue;
        }
      }
    }

    if (ch == '(') {
      if (depth == 0) {
        depth0_tokens.push_back({false, {}});
      }
      ++depth;
      ++i;
      body_end = i;
      continue;
    }

    if (ch == ')') {
      if (depth > 0) {
        --depth;
      }
      ++i;
      body_end = i;
      if (depth == 0) {
        depth0_tokens.push_back({false, {}});
      }
      continue;
    }

    if (is_word_start(ch)) {
      const std::size_t start = i;
      ++i;
      while (i < n && is_word_char(static_cast<unsigned char>(sql[i]))) {
        ++i;
      }
      body_end = i;
      if (depth == 0) {
        depth0_tokens.push_back({true, UpperAsciiCopy(sql.substr(start, i - start))});
      }
      continue;
    }

    ++i;
    body_end = i;
    if (depth == 0) {
      depth0_tokens.push_back({false, {}});
    }
  }

  if (depth0_tokens.empty() || !depth0_tokens.front().is_word || body_end == 0) {
    return {std::string(sql), false, 0};
  }

  const auto is_select_top = [&](std::size_t select_index) {
    if (select_index + 1 >= depth0_tokens.size() || !depth0_tokens[select_index + 1].is_word) {
      return false;
    }
    const std::string& next = depth0_tokens[select_index + 1].upper;
    if (next == "TOP") {
      return true;
    }
    if ((next == "DISTINCT" || next == "ALL") && select_index + 2 < depth0_tokens.size() &&
        depth0_tokens[select_index + 2].is_word &&
        depth0_tokens[select_index + 2].upper == "TOP") {
      return true;
    }
    return false;
  };

  const std::string& first = depth0_tokens.front().upper;
  if (first == "SELECT") {
    if (is_select_top(0)) {
      return {std::string(sql), false, 0};
    }
  } else if (first == "WITH") {
    bool found_read_verb = false;
    for (std::size_t idx = 1; idx < depth0_tokens.size(); ++idx) {
      if (!depth0_tokens[idx].is_word) {
        continue;
      }
      const std::string& word = depth0_tokens[idx].upper;
      if (word == "SELECT" || word == "TABLE" || word == "VALUES") {
        if (word == "SELECT" && is_select_top(idx)) {
          return {std::string(sql), false, 0};
        }
        found_read_verb = true;
        break;
      }
      if (word == "INSERT" || word == "UPDATE" || word == "DELETE" || word == "MERGE") {
        return {std::string(sql), false, 0};
      }
    }
    if (!found_read_verb) {
      return {std::string(sql), false, 0};
    }
  } else if (first != "TABLE") {
    return {std::string(sql), false, 0};
  }

  for (const Depth0Token& token : depth0_tokens) {
    if (!token.is_word) {
      continue;
    }
    if (token.upper == "LIMIT" || token.upper == "FETCH" || token.upper == "OFFSET" ||
        token.upper == "FOR" || token.upper == "INTO" || token.upper == "LOCK") {
      return {std::string(sql), false, 0};
    }
  }

  const std::int64_t limit = std::clamp(page_size, kMinPageSize, kMaxPageSize);
  const std::int64_t clamped_page = std::max<std::int64_t>(0, page);
  const std::int64_t offset =
      (clamped_page > 0 && limit > std::numeric_limits<std::int64_t>::max() / clamped_page)
          ? std::numeric_limits<std::int64_t>::max()
          : clamped_page * limit;
  std::string_view body = sql.substr(0, body_end);
  while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back())) != 0) {
    body.remove_suffix(1);
  }
  if (body.empty()) {
    return {std::string(sql), false, 0, 0};
  }
  std::string rewritten_sql = std::string(body) + "\nLIMIT " + std::to_string(limit + 1);
  if (offset > 0) {
    rewritten_sql += " OFFSET " + std::to_string(offset);
  }
  return SqlLimitRewriteResult{
      .sql = std::move(rewritten_sql),
      .applied = true,
      .effective_limit = limit,
      .effective_offset = offset,
  };
}

} // namespace dbpp
