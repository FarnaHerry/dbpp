module;

#include <arrow-adbc/adbc.h>
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

namespace {

std::string SqlPlaceholder(Dialect dialect, std::size_t one_based_index) {
  if (dialect == Dialect::Postgresql) {
    return "$" + std::to_string(one_based_index);
  }
  return "?";
}

bool IsTextualColumnType(std::string_view type) {
  const std::string lower = detail::DbLowerAscii(type);
  if (lower.empty()) {
    return true;
  }
  return lower.find("text") != std::string::npos || lower.find("char") != std::string::npos ||
         lower.find("clob") != std::string::npos || lower.find("string") != std::string::npos ||
         lower.find("json") != std::string::npos || lower.find("uuid") != std::string::npos;
}

bool IsCellNullInput(const ColumnMeta& column, std::string_view text) {
  if (detail::DbEqualsIgnoreCaseAscii(text, "NULL")) {
    return true;
  }
  if (!text.empty()) {
    return false;
  }
  return column.nullable || !IsTextualColumnType(column.type);
}

bool ShouldBypassPreparedForTemporal(Dialect dialect,
                                     const std::vector<PreparedParamValue>& params,
                                     const std::vector<std::string>& param_types) {
  if (dialect != Dialect::Mysql) {
    return false;
  }
  const std::size_t count = std::min(params.size(), param_types.size());
  for (std::size_t i = 0; i < count; ++i) {
    if (params[i].is_null) {
      continue;
    }
    const std::string lower = detail::DbLowerAscii(param_types[i]);
    if (lower.find("int") != std::string::npos) {
      continue;
    }
    if (lower.find("timestamp") != std::string::npos ||
        lower.find("datetime") != std::string::npos ||
        lower.find("date") != std::string::npos ||
        lower.find("time") != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool ShouldReportPreparedErrorDirectly(int prep_status, const AdbcQueryResult& prep_res) {
  if (prep_res.connection_lost || IsConnectionLostError(prep_status, prep_res.error, prep_res.sqlstate)) {
    return true;
  }
  if (prep_status == ADBC_STATUS_ALREADY_EXISTS || prep_status == ADBC_STATUS_INTEGRITY ||
      prep_status == ADBC_STATUS_UNAUTHENTICATED || prep_status == ADBC_STATUS_UNAUTHORIZED ||
      prep_status == ADBC_STATUS_CANCELLED || prep_status == ADBC_STATUS_TIMEOUT) {
    return true;
  }
  if (prep_res.sqlstate.starts_with("23") || prep_res.sqlstate.starts_with("425") ||
      prep_res.sqlstate.starts_with("08")) {
    return true;
  }
  return false;
}

bool SupportsManagedTransactions(Dialect dialect) {
  return dialect == Dialect::Sqlite || dialect == Dialect::Postgresql ||
         dialect == Dialect::Mysql || dialect == Dialect::Duckdb;
}

class ScopedTransaction {
 public:
  explicit ScopedTransaction(AdbcSession& session) : session_(session) {
    if (SupportsManagedTransactions(session_.dialect) && !session_.in_transaction) {
      std::string tx_err;
      if (SetAutocommitBlocking(session_, false, tx_err)) {
        managed_ = true;
        active_ = true;
      }
    }
  }

  ScopedTransaction(const ScopedTransaction&) = delete;
  ScopedTransaction& operator=(const ScopedTransaction&) = delete;

  ~ScopedTransaction() { Rollback(false); }

  [[nodiscard]] bool WasManaged() const noexcept { return managed_; }

  bool Rollback(bool connection_lost = false) {
    if (!active_) {
      return false;
    }
    active_ = false;
    if (connection_lost) {
      return false;
    }
    std::string rb_err;
    (void)RollbackTransactionBlocking(session_, rb_err);
    (void)SetAutocommitBlocking(session_, true, rb_err);
    return true;
  }

  bool Commit(std::string& error_out) {
    if (!active_) {
      return true;
    }
    active_ = false;
    if (!CommitTransactionBlocking(session_, error_out)) {
      std::string rb_err;
      (void)RollbackTransactionBlocking(session_, rb_err);
      (void)SetAutocommitBlocking(session_, true, rb_err);
      return false;
    }
    std::string ac_err;
    (void)SetAutocommitBlocking(session_, true, ac_err);
    return true;
  }

 private:
  AdbcSession& session_;
  bool managed_ = false;
  bool active_ = false;
};

template <typename LiteralSqlFn>
AdbcQueryResult ExecuteWriteWithFallback(AdbcSession& session, bool can_prepare,
                                         std::string_view param_sql,
                                         const std::vector<PreparedParamValue>& params,
                                         const std::vector<std::string>& param_types,
                                         LiteralSqlFn&& build_literal_sql) {
  if (can_prepare && !param_sql.empty() &&
      !ShouldBypassPreparedForTemporal(session.dialect, params, param_types)) {
    int prep_status = 0;
    AdbcQueryResult prep_res;
    if (ExecutePreparedNonQuery(session, param_sql, params, param_types, prep_res, prep_status)) {
      if (prep_res.error.empty() || ShouldReportPreparedErrorDirectly(prep_status, prep_res)) {
        if (IsConnectionLostError(prep_status, prep_res.error, prep_res.sqlstate)) {
          prep_res.connection_lost = true;
        }
        return prep_res;
      }
    } else if (!prep_res.error.empty() && prep_status != ADBC_STATUS_NOT_IMPLEMENTED &&
               ShouldReportPreparedErrorDirectly(prep_status, prep_res)) {
      if (IsConnectionLostError(prep_status, prep_res.error, prep_res.sqlstate)) {
        prep_res.connection_lost = true;
      }
      return prep_res;
    }
  }
  const std::string literal_sql = build_literal_sql();
  if (literal_sql.empty()) {
    AdbcQueryResult err;
    err.error = Tr("Table has no usable row identifier", "该表没有可用的行标识");
    return err;
  }
  return ExecuteBlocking(session, literal_sql, false, 0);
}

bool BuildParameterizedWhere(Dialect dialect, const TableModel& model, const GridRow& row,
                             std::string& where_out, std::vector<PreparedParamValue>& params_out,
                             std::vector<std::string>& types_out, std::size_t& next_param_idx) {
  where_out.clear();
  if ((model.row_locator == RowLocatorKind::HiddenRowId || model.uses_rowid) &&
      (!row.row_locator_token.empty() || row.rowid.has_value())) {
    const std::string& col =
        model.hidden_rowid_where_col.empty() ? std::string{"rowid"} : model.hidden_rowid_where_col;
    const std::string token = !row.row_locator_token.empty() ? row.row_locator_token
                                                             : std::to_string(*row.rowid);
    where_out = col + " = " + SqlPlaceholder(dialect, next_param_idx++) + model.hidden_rowid_cast;
    params_out.push_back(PreparedParamValue{.text = token, .is_null = false});
    types_out.emplace_back(model.hidden_rowid_is_numeric ? "INTEGER" : "TEXT");
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

AdbcQueryResult UpdateRowInternal(AdbcSession& session, const TableModel& model,
                                  std::size_t row_index,
                                  const std::vector<std::pair<std::size_t, std::string>>& updates) {
  AdbcQueryResult result;
  if (updates.empty()) {
    return result;
  }
  if (!model.snapshot || row_index >= model.snapshot->rows.size()) {
    result.error = Tr("Target row or column is invalid", "目标行或列已失效");
    return result;
  }
  if (model.read_only || !model.editable) {
    result.error = Tr("Current object is not editable", "当前对象不允许编辑");
    return result;
  }
  if (session.dialect == Dialect::Redis && session.redis) {
    if (updates.size() == 1) {
      return UpdateRedisCellBlocking(*session.redis, model, row_index, updates[0].first,
                                     updates[0].second);
    }
    TableModel cur_model = model;
    for (const auto& [col_idx, text] : updates) {
      AdbcQueryResult step =
          UpdateRedisCellBlocking(*session.redis, cur_model, row_index, col_idx, text);
      if (!step.error.empty()) {
        return step;
      }
    }
    return result;
  }

  for (const auto& [col_idx, _] : updates) {
    if (col_idx >= model.columns.size()) {
      result.error = Tr("Target row or column is invalid", "目标行或列已失效");
      return result;
    }
  }

  const GridRow& row = model.snapshot->rows[row_index];
  std::size_t next_idx = 1;
  std::vector<PreparedParamValue> params;
  std::vector<std::string> param_types;
  std::string param_set;
  std::string literal_set;
  for (std::size_t i = 0; i < updates.size(); ++i) {
    const std::size_t col_idx = updates[i].first;
    const std::string& raw_text = updates[i].second;
    const ColumnMeta& column = model.columns[col_idx];
    const bool is_null = IsCellNullInput(column, raw_text);
    const std::string col_ident = QuoteIdentifier(session.dialect, column.name);
    if (i > 0) {
      param_set += ", ";
      literal_set += ", ";
    }
    param_set += col_ident + " = " + SqlPlaceholder(session.dialect, next_idx++);
    params.push_back(
        PreparedParamValue{.text = is_null ? std::string{} : raw_text, .is_null = is_null});
    param_types.push_back(column.type);
    literal_set += col_ident + " = " + SqlLiteral(session.dialect, raw_text, column.type, is_null);
  }

  const std::string limit_suffix =
      (model.row_locator == RowLocatorKind::AllColumns && session.dialect == Dialect::Mysql)
          ? std::string{" LIMIT 1"}
          : std::string{};

  std::string param_where;
  const bool can_prepare = BuildParameterizedWhere(session.dialect, model, row, param_where, params,
                                                   param_types, next_idx);
  const std::string param_sql =
      can_prepare
          ? ("UPDATE " + model.qualified_name + " SET " + param_set + " WHERE " + param_where +
             limit_suffix)
          : std::string{};
  return ExecuteWriteWithFallback(
      session, can_prepare, param_sql, params, param_types, [&]() -> std::string {
        const std::string where = detail::DbBuildWhere(session.dialect, model, row);
        if (where.empty()) {
          return {};
        }
        return "UPDATE " + model.qualified_name + " SET " + literal_set + " WHERE " + where +
               limit_suffix;
      });
}

AdbcQueryResult InsertRowBlocking(AdbcSession& session, const TableModel& model,
                                  std::vector<std::string> values) {
  AdbcQueryResult result;
  if (model.read_only || !model.editable) {
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
  for (std::size_t index = 0; index < model.columns.size() && index < values.size(); ++index) {
    if (values[index].empty()) {
      continue;
    }
    if (!columns.empty()) {
      columns += ", ";
      param_placeholders += ", ";
      literal_placeholders += ", ";
    }
    columns += QuoteIdentifier(session.dialect, model.columns[index].name);
    const bool is_null = detail::DbEqualsIgnoreCaseAscii(values[index], "NULL");
    param_placeholders += SqlPlaceholder(session.dialect, next_idx++);
    params.push_back(PreparedParamValue{
        .text = is_null ? std::string{} : values[index],
        .is_null = is_null,
    });
    param_types.push_back(model.columns[index].type);
    literal_placeholders +=
        SqlLiteral(session.dialect, values[index], model.columns[index].type, is_null);
  }

  const std::string param_sql =
      !columns.empty() ? ("INSERT INTO " + model.qualified_name + " (" + columns +
                          ") VALUES (" + param_placeholders + ")")
                       : std::string{};
  return ExecuteWriteWithFallback(
      session, !columns.empty(), param_sql, params, param_types, [&]() -> std::string {
        if (columns.empty()) {
          return session.dialect == Dialect::Mysql
                     ? "INSERT INTO " + model.qualified_name + " () VALUES ()"
                     : "INSERT INTO " + model.qualified_name + " DEFAULT VALUES";
        }
        return "INSERT INTO " + model.qualified_name + " (" + columns + ") VALUES (" +
               literal_placeholders + ")";
      });
}

} // namespace

BatchInsertResult InsertRowsBlocking(AdbcSession& session, const TableModel& model,
                                     const std::vector<PendingInsertRow>& rows) {
  if (rows.size() == 1 && !rows.empty() && session.dialect != Dialect::Redis) {
    // Preserve single-row insert convenience semantics cleanly via CommitTableChangesBlocking
  }
  const BatchCommitResult res = CommitTableChangesBlocking(session, model, {}, {}, rows);
  return BatchInsertResult{
      .inserted = res.inserted_rows,
      .rolled_back = res.rolled_back,
      .connection_lost = res.connection_lost,
      .error = res.error,
  };
}

BatchCommitResult CommitTableChangesBlocking(AdbcSession& session, const TableModel& model,
                                             const std::vector<PendingCellEdit>& edits,
                                             const std::vector<std::size_t>& deletes,
                                             const std::vector<PendingInsertRow>& inserts) {
  if (session.IsClosed()) {
    return BatchCommitResult{.rolled_back = true,
                             .connection_lost = true,
                             .error = std::string(Tr("Connection is closed", "连接已关闭"))};
  }
  if (edits.empty() && deletes.empty() && inserts.empty()) {
    return {};
  }
  if (model.read_only || !model.editable) {
    return BatchCommitResult{.rolled_back = true,
                             .error = std::string(Tr("Current object is not editable",
                                                     "当前对象不允许编辑"))};
  }

  std::vector<std::size_t> unique_deletes;
  unique_deletes.reserve(deletes.size());
  for (const std::size_t row_idx : deletes) {
    if (std::ranges::find(unique_deletes, row_idx) == unique_deletes.end()) {
      unique_deletes.push_back(row_idx);
    }
  }

  struct GroupedRowUpdate {
    std::size_t row_index = 0;
    std::vector<std::pair<std::size_t, std::string>> columns;
  };
  std::vector<GroupedRowUpdate> grouped_updates;
  for (const PendingCellEdit& edit : edits) {
    if (std::ranges::find(unique_deletes, edit.row_index) != unique_deletes.end()) {
      continue;
    }
    auto row_it = std::ranges::find_if(
        grouped_updates, [&](const GroupedRowUpdate& g) { return g.row_index == edit.row_index; });
    if (row_it == grouped_updates.end()) {
      grouped_updates.push_back(GroupedRowUpdate{
          .row_index = edit.row_index,
          .columns = {{edit.column_index, edit.raw_text}},
      });
      continue;
    }
    auto col_it = std::ranges::find_if(
        row_it->columns, [&](const auto& c) { return c.first == edit.column_index; });
    if (col_it != row_it->columns.end()) {
      col_it->second = edit.raw_text;
    } else {
      row_it->columns.emplace_back(edit.column_index, edit.raw_text);
    }
  }

  ScopedTransaction tx(session);
  auto finish_error = [&](std::size_t updated_so_far, std::size_t deleted_so_far,
                          std::size_t inserted_so_far, bool connection_lost,
                          const std::string& msg) -> BatchCommitResult {
    const bool rolled = tx.Rollback(connection_lost);
    return BatchCommitResult{
        .updated_rows = rolled ? 0 : updated_so_far,
        .deleted_rows = rolled ? 0 : deleted_so_far,
        .inserted_rows = rolled ? 0 : inserted_so_far,
        .rolled_back = rolled,
        .connection_lost = connection_lost,
        .error = msg,
    };
  };

  std::size_t updated_count = 0;
  for (const GroupedRowUpdate& upd : grouped_updates) {
    const AdbcQueryResult res = UpdateRowBlocking(session, model, upd.row_index, upd.columns);
    if (!res.error.empty()) {
      return finish_error(updated_count, 0, 0, res.connection_lost, res.error);
    }
    ++updated_count;
  }

  std::size_t deleted_count = 0;
  for (const std::size_t del_idx : unique_deletes) {
    const AdbcQueryResult res = DeleteRowBlocking(session, model, del_idx);
    if (!res.error.empty()) {
      return finish_error(updated_count, deleted_count, 0, res.connection_lost, res.error);
    }
    ++deleted_count;
  }

  std::size_t inserted_count = 0;
  if (!inserts.empty()) {
    if (session.dialect == Dialect::Redis && session.redis) {
      const BatchInsertResult redis_res = InsertRedisRowsBlocking(*session.redis, model, inserts);
      if (!redis_res.error.empty()) {
        return finish_error(updated_count, deleted_count, redis_res.inserted,
                            redis_res.connection_lost, redis_res.error);
      }
      inserted_count = redis_res.inserted;
    } else {
      for (std::size_t i = 0; i < inserts.size(); ++i) {
        const AdbcQueryResult res = InsertRowBlocking(session, model, inserts[i].values);
        if (!res.error.empty()) {
          const std::string msg = TrFmt("Row {0} insert failed: {1}", "第 {0} 行插入失败: {1}",
                                        {std::to_string(i + 1), res.error});
          return finish_error(updated_count, deleted_count, inserted_count, res.connection_lost,
                              msg);
        }
        ++inserted_count;
      }
    }
  }

  std::string commit_err;
  if (!tx.Commit(commit_err)) {
    return BatchCommitResult{
        .updated_rows = 0,
        .deleted_rows = 0,
        .inserted_rows = 0,
        .rolled_back = true,
        .connection_lost = IsConnectionLostError(0, commit_err),
        .error = TrFmt("Failed to commit transaction: {0}", "提交事务失败: {0}", {commit_err}),
    };
  }

  return BatchCommitResult{
      .updated_rows = updated_count,
      .deleted_rows = deleted_count,
      .inserted_rows = inserted_count,
      .rolled_back = false,
      .connection_lost = false,
      .error = {},
  };
}

AdbcQueryResult UpdateCellBlocking(AdbcSession& session, const TableModel& model,
                                   std::size_t row_index, std::size_t column_index,
                                   const std::string& text) {
  return UpdateRowInternal(session, model, row_index, {{column_index, text}});
}

AdbcQueryResult UpdateRowBlocking(
    AdbcSession& session, const TableModel& model, std::size_t row_index,
    const std::vector<std::pair<std::size_t, std::string>>& updates) {
  return UpdateRowInternal(session, model, row_index, updates);
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
  if (model.read_only || !model.editable) {
    result.error = Tr("Current object does not allow deleting rows", "当前对象不允许删除行");
    return result;
  }
  const GridRow& row = model.snapshot->rows[row_index];

  const std::string limit_suffix =
      (model.row_locator == RowLocatorKind::AllColumns && session.dialect == Dialect::Mysql)
          ? std::string{" LIMIT 1"}
          : std::string{};

  std::size_t next_idx = 1;
  std::vector<PreparedParamValue> params;
  std::vector<std::string> param_types;
  std::string param_where;
  const bool can_prepare = BuildParameterizedWhere(session.dialect, model, row, param_where, params,
                                                   param_types, next_idx);
  const std::string param_sql =
      can_prepare
          ? ("DELETE FROM " + model.qualified_name + " WHERE " + param_where + limit_suffix)
          : std::string{};
  return ExecuteWriteWithFallback(
      session, can_prepare, param_sql, params, param_types, [&]() -> std::string {
        const std::string where = detail::DbBuildWhere(session.dialect, model, row);
        if (where.empty()) {
          return {};
        }
        return "DELETE FROM " + model.qualified_name + " WHERE " + where + limit_suffix;
      });
}

namespace detail {

AdbcQueryResult DbUpdateCellBlocking(AdbcSession& session, const TableModel& model,
                                     std::size_t row_index, std::size_t column_index,
                                     const std::string& text) {
  return UpdateCellBlocking(session, model, row_index, column_index, text);
}

AdbcQueryResult DbUpdateRowBlocking(
    AdbcSession& session, const TableModel& model, std::size_t row_index,
    const std::vector<std::pair<std::size_t, std::string>>& updates) {
  return UpdateRowBlocking(session, model, row_index, updates);
}

AdbcQueryResult DbDeleteRowBlocking(AdbcSession& session, const TableModel& model,
                                    std::size_t row_index) {
  return DeleteRowBlocking(session, model, row_index);
}

} // namespace detail

} // namespace dbpp
