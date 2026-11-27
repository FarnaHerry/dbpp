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

  std::string param_where;
  if (BuildParameterizedWhere(session.dialect, model, row, param_where, params, param_types,
                              next_idx)) {
    const std::string param_sql =
        "UPDATE " + model.qualified_name + " SET " + param_set + " WHERE " + param_where;
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

  const std::string where = detail::DbBuildWhere(session.dialect, model, row);
  if (where.empty()) {
    result.error = Tr("Table has no usable row identifier", "该表没有可用的行标识");
    return result;
  }
  const std::string sql =
      "UPDATE " + model.qualified_name + " SET " + literal_set + " WHERE " + where;
  return ExecuteBlocking(session, sql, false, 0);
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

  if (!columns.empty()) {
    const std::string param_sql = "INSERT INTO " + model.qualified_name + " (" + columns +
                                  ") VALUES (" + param_placeholders + ")";
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

  const std::string where = detail::DbBuildWhere(session.dialect, model, row);
  if (where.empty()) {
    result.error = Tr("Table has no usable row identifier", "该表没有可用的行标识");
    return result;
  }
  return ExecuteBlocking(session,
                         "DELETE FROM " + model.qualified_name + " WHERE " + where, false, 0);
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
