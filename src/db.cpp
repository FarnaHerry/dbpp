#include "model.h"

#include "adbc_session.h"

#include <huxerui/huxerui.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dbpp {

using namespace huxerui;

namespace {

const DbSession* FindSession(const AppModel& model, std::uint64_t id) {
  for (const auto& session : model.sessions) {
    if (session.id == id) {
      return &session;
    }
  }
  return nullptr;
}

const DbObject* FindObject(const DbSession& session, std::string_view name) {
  for (const auto& object : session.objects) {
    if (object.name == name) {
      return &object;
    }
  }
  return nullptr;
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

std::string DisplayNameFromUri(std::string_view uri, std::string_view fallback) {
  if (uri.empty()) {
    return std::string(fallback);
  }
  const auto slash = uri.find_last_of("/\\");
  std::string name = slash == std::string_view::npos ? std::string(uri)
                                                     : std::string(uri.substr(slash + 1));
  if (name.empty()) {
    return std::string(fallback);
  }
  return name;
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

TableModel LoadTableBlocking(AdbcSession& session, std::uint64_t session_id,
                             DbObject object, std::int64_t page, std::int64_t page_size,
                             std::optional<DataTableSort> sort, std::string filter) {
  TableModel model;
  model.session_id = session_id;
  model.object = object.name;
  model.qualified_name = QualifiedName(session.dialect, object);
  model.is_view = object.is_view;
  model.read_only = false;
  model.page = std::max<std::int64_t>(0, page);
  model.page_size = std::max<std::int64_t>(1, page_size);
  model.sort = std::move(sort);
  model.filter = std::move(filter);

  AdbcSchemaResult schema = LoadSchemaBlocking(session, object);
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
    model.error = count.error.empty() ? "COUNT 查询没有返回结果" : count.error;
    return model;
  }
  model.total = ParseInt64(count.rows.front().values.front().text);

  const std::int64_t page_count =
      std::max<std::int64_t>(1, (model.total + model.page_size - 1) / model.page_size);
  model.page = std::clamp<std::int64_t>(model.page, 0, page_count - 1);
  const std::int64_t offset = model.page * model.page_size;

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
  model.message = std::to_string(model.total) + " 行 · 第 " +
                  std::to_string(model.page + 1) + " / " + std::to_string(page_count) +
                  " 页";
  return model;
}

AdbcQueryResult UpdateCellBlocking(AdbcSession& session, const TableModel& model,
                                   std::size_t row_index, std::size_t column_index,
                                   const std::string& text) {
  AdbcQueryResult result;
  if (!model.snapshot || row_index >= model.snapshot->rows.size() ||
      column_index >= model.columns.size()) {
    result.error = "目标行或列已失效";
    return result;
  }
  if (!model.editable) {
    result.error = "当前对象不允许编辑";
    return result;
  }
  const GridRow& row = model.snapshot->rows[row_index];
  const std::string where = BuildWhere(session.dialect, model, row);
  if (where.empty()) {
    result.error = "该表没有可用的行标识";
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
  AdbcQueryResult result;
  if (!model.snapshot || row_index >= model.snapshot->rows.size()) {
    result.error = "目标行已失效";
    return result;
  }
  if (!model.editable) {
    result.error = "当前对象不允许删除行";
    return result;
  }
  const GridRow& row = model.snapshot->rows[row_index];
  const std::string where = BuildWhere(session.dialect, model, row);
  if (where.empty()) {
    result.error = "该表没有可用的行标识";
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
    result.error = "当前对象不允许插入行";
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

void ApplyOpenedSession(const State<AppModel>& app, const State<TableModel>& table,
                        const State<std::optional<DataTableCellId>>& selected,
                        const State<TextEditingValue>& filter_input,
                        const TaskScope& tasks, AdbcOpenResult opened,
                        ConnectionRequest request, AdbcObjectsResult objects,
                        NotifyFn notify) {
  if (!opened.session) {
    const std::string message = "连接失败: " + opened.error;
    app.Update([message](AppModel& model) {
      model.status.clear();
      model.error = message;
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
  app.Update([&](AppModel& model) {
    for (const auto& existing : model.sessions) {
      if (existing.uri == session->uri && existing.driver_path == session->driver_path) {
        model.active_session = existing.id;
        model.active_object =
            existing.objects.empty() ? std::string{} : existing.objects.front().name;
        model.active_is_view = !existing.objects.empty() &&
                               existing.objects.front().is_view;
        model.status = "已切换到 " + existing.name;
        model.error.clear();
        duplicated = true;
        return;
      }
    }
    session->id = model.next_session_id++;
    model.sessions.push_back(*session);
    model.active_session = model.sessions.back().id;
    model.active_object = model.sessions.back().objects.empty()
                              ? std::string{}
                              : model.sessions.back().objects.front().name;
    model.active_is_view = !model.sessions.back().objects.empty() &&
                           model.sessions.back().objects.front().is_view;
    model.status = "已连接 " + session->name;
    model.error = objects.error;
    ++model.revision;
  });
  filter_input = TextEditingValue::FromText({});

  if (!duplicated) {
    RequestLoadTable(app, table, selected, tasks, 0, std::nullopt, {}, false, notify);
  }
  if (notify) {
    if (duplicated) {
      notify("该连接已存在");
    } else if (!objects.error.empty()) {
      notify("已连接，但读取对象失败: " + objects.error);
    } else {
      notify("已连接 " + session->name);
    }
  }
}

} // namespace

Task<AdbcQueryResult> ExecuteQueryAsync(std::shared_ptr<AdbcSession> session,
                                        std::string sql, bool want_rows,
                                        std::size_t max_rows) {
  if (!session) {
    AdbcQueryResult result;
    result.error = "连接已关闭";
    co_return result;
  }
  co_return co_await session->sequence.Run(
      [session, sql = std::move(sql), want_rows, max_rows](std::stop_token) mutable {
        return ExecuteBlocking(*session, std::move(sql), want_rows, max_rows);
      });
}

Task<AdbcQueryResult> UpdateCellAsync(std::shared_ptr<AdbcSession> session, TableModel model,
                                      std::size_t row_index, std::size_t column_index,
                                      std::string text) {
  if (!session) {
    AdbcQueryResult result;
    result.error = "连接已关闭";
    co_return result;
  }
  co_return co_await session->sequence.Run(
      [session, model = std::move(model), row_index, column_index, text = std::move(text)](
          std::stop_token) mutable {
        return UpdateCellBlocking(*session, model, row_index, column_index, text);
      });
}

Task<AdbcQueryResult> DeleteRowAsync(std::shared_ptr<AdbcSession> session, TableModel model,
                                     std::size_t row_index) {
  if (!session) {
    AdbcQueryResult result;
    result.error = "连接已关闭";
    co_return result;
  }
  co_return co_await session->sequence.Run(
      [session, model = std::move(model), row_index](std::stop_token) mutable {
        return DeleteRowBlocking(*session, model, row_index);
      });
}

Task<AdbcQueryResult> InsertRowAsync(std::shared_ptr<AdbcSession> session, TableModel model,
                                     std::vector<std::string> values) {
  if (!session) {
    AdbcQueryResult result;
    result.error = "连接已关闭";
    co_return result;
  }
  co_return co_await session->sequence.Run(
      [session, model = std::move(model), values = std::move(values)](
          std::stop_token) mutable {
        return InsertRowBlocking(*session, model, std::move(values));
      });
}

void RequestLoadTable(const State<AppModel>& app, const State<TableModel>& table,
                      const State<std::optional<DataTableCellId>>& selected,
                      const TaskScope& tasks, std::int64_t page,
                      std::optional<DataTableSort> sort, std::string filter,
                      bool keep_selection, NotifyFn notify) {
  const AppModel& model = app.Get();
  const DbSession* session = FindSession(model, model.active_session);
  const DbObject* object =
      session != nullptr ? FindObject(*session, model.active_object) : nullptr;
  if (session == nullptr || object == nullptr || !session->session) {
    table = TableModel{};
    selected = std::nullopt;
    return;
  }

  table.Update([&](TableModel& value) {
    value.loading = true;
    value.error.clear();
    value.page = page;
    value.filter = filter;
    value.sort = sort;
  });
  if (!keep_selection) {
    selected = std::nullopt;
  }

  const std::uint64_t session_id = session->id;
  const std::string object_name = object->name;
  auto adbc_session = session->session;
  DbObject object_copy = *object;

  tasks.Launch([app, table, selected, notify = std::move(notify), adbc_session, session_id,
                object_copy = std::move(object_copy), object_name, page, sort, filter]() mutable
                   -> Task<void> {
    TableModel result = co_await adbc_session->sequence.Run(
        [adbc_session, session_id, object_copy = std::move(object_copy), page, sort, filter](
            std::stop_token) mutable {
          return LoadTableBlocking(*adbc_session, session_id, std::move(object_copy), page,
                                   kPageSize, sort, filter);
        });
    const AppModel& current = app.Get();
    if (current.active_session != session_id || current.active_object != object_name) {
      co_return;
    }
    if (!result.error.empty() && result.snapshot == nullptr) {
      const std::string message = result.error;
      table.Update([message](TableModel& value) {
        value.loading = false;
        value.error = message;
      });
      if (notify) {
        notify("加载失败: " + message);
      }
      co_return;
    }
    table = std::move(result);
    selected = std::nullopt;
  });
}

void RequestSaveCell(const State<AppModel>& app, const State<TableModel>& table,
                     const State<std::optional<DataTableCellId>>& selected,
                     const TaskScope& tasks, const DataTableEditRequest& edit,
                     NotifyFn notify) {
  const AppModel& model = app.Get();
  const DbSession* session = FindSession(model, model.active_session);
  if (session == nullptr || !session->session) {
    return;
  }
  TableModel table_value = table.Get();
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

  tasks.Launch([app, table, selected, tasks, notify = std::move(notify), adbc_session,
                table_value = std::move(table_value), session_id, object_name, row_index,
                column_index, text = edit.text]() mutable -> Task<void> {
    AdbcQueryResult result =
        co_await UpdateCellAsync(adbc_session, table_value, row_index, column_index, text);
    if (!result.error.empty()) {
      const std::string message = result.error;
      table.Update([message](TableModel& value) { value.error = message; });
      if (notify) {
        notify("保存失败: " + message);
      }
      co_return;
    }
    const AppModel& current = app.Get();
    if (current.active_session != session_id || current.active_object != object_name) {
      co_return;
    }
    RequestLoadTable(app, table, selected, tasks, table_value.page, table_value.sort,
                     table_value.filter, true, notify);
  });
}

void RequestDeleteSelected(const State<AppModel>& app, const State<TableModel>& table,
                           const State<std::optional<DataTableCellId>>& selected,
                           const TaskScope& tasks, NotifyFn notify) {
  if (!selected.Get().has_value()) {
    return;
  }
  const AppModel& model = app.Get();
  const DbSession* session = FindSession(model, model.active_session);
  if (session == nullptr || !session->session) {
    return;
  }
  TableModel table_value = table.Get();
  if (!table_value.snapshot) {
    return;
  }
  const std::size_t row_index = ParseRowIndex(selected.Get()->row_id);
  if (row_index >= table_value.snapshot->rows.size()) {
    return;
  }
  auto adbc_session = session->session;
  const std::uint64_t session_id = session->id;
  const std::string object_name = table_value.object;
  const std::int64_t page = table_value.page;
  const std::int64_t remaining = static_cast<std::int64_t>(table_value.snapshot->rows.size());

  tasks.Launch([app, table, selected, tasks, notify = std::move(notify), adbc_session,
                table_value = std::move(table_value), session_id, object_name, row_index,
                page, remaining]() mutable -> Task<void> {
    AdbcQueryResult result = co_await DeleteRowAsync(adbc_session, table_value, row_index);
    if (!result.error.empty()) {
      const std::string message = result.error;
      table.Update([message](TableModel& value) { value.error = message; });
      if (notify) {
        notify("删除失败: " + message);
      }
      co_return;
    }
    const AppModel& current = app.Get();
    if (current.active_session != session_id || current.active_object != object_name) {
      co_return;
    }
    const std::int64_t next_page = remaining <= 1 && page > 0 ? page - 1 : page;
    RequestLoadTable(app, table, selected, tasks, next_page, table_value.sort,
                     table_value.filter, false, notify);
    if (notify) {
      notify("已删除 1 行");
    }
  });
}

void SelectObject(const State<AppModel>& app, const State<TableModel>& table,
                  const State<std::optional<DataTableCellId>>& selected,
                  const State<TextEditingValue>& filter_input, const TaskScope& tasks,
                  std::uint64_t session_id, std::string object, bool is_view,
                  NotifyFn notify) {
  app.Update([&](AppModel& model) {
    model.active_session = session_id;
    model.active_object = std::move(object);
    model.active_is_view = is_view;
    model.error.clear();
  });
  filter_input = TextEditingValue::FromText({});
  RequestLoadTable(app, table, selected, tasks, 0, std::nullopt, {}, false,
                   std::move(notify));
}

void OpenConnection(const State<AppModel>& app, const State<TableModel>& table,
                    const State<std::optional<DataTableCellId>>& selected,
                    const State<TextEditingValue>& filter_input, const TaskScope& tasks,
                    ConnectionRequest request, NotifyFn notify) {
  app.Update([&](AppModel& model) {
    model.error.clear();
    model.status = "正在连接 " + DisplayNameFromUri(request.uri, "数据库") + "…";
  });
  tasks.Launch([app, table, selected, filter_input, tasks, request = std::move(request),
                notify = std::move(notify)]() mutable -> Task<void> {
    AdbcOpenResult opened =
        co_await RunWorker([request] { return OpenAdbcBlocking(request); });
    if (!opened.session) {
      const std::string message = "连接失败: " + opened.error;
      app.Update([message](AppModel& model) {
        model.status.clear();
        model.error = message;
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
    ApplyOpenedSession(app, table, selected, filter_input, tasks, std::move(opened),
                       std::move(request), std::move(objects), std::move(notify));
  });
}

void CloseSession(const State<AppModel>& app, const State<TableModel>& table,
                  const State<std::optional<DataTableCellId>>& selected,
                  const State<TextEditingValue>& filter_input, const TaskScope& tasks,
                  std::uint64_t session_id, NotifyFn notify) {
  bool active_closed = false;
  app.Update([&](AppModel& model) {
    active_closed = model.active_session == session_id;
    std::erase_if(model.sessions, [session_id](const DbSession& session) {
      return session.id == session_id;
    });
    if (!active_closed) {
      return;
    }
    if (model.sessions.empty()) {
      model.active_session = 0;
      model.active_object.clear();
      model.active_is_view = false;
      model.status = "没有打开的数据库";
      return;
    }
    model.active_session = model.sessions.front().id;
    model.active_object = model.sessions.front().objects.empty()
                              ? std::string{}
                              : model.sessions.front().objects.front().name;
    model.active_is_view = !model.sessions.front().objects.empty() &&
                           model.sessions.front().objects.front().is_view;
    ++model.revision;
  });
  filter_input = TextEditingValue::FromText({});
  if (active_closed) {
    RequestLoadTable(app, table, selected, tasks, 0, std::nullopt, {}, false,
                     std::move(notify));
  }
}

} // namespace dbpp
