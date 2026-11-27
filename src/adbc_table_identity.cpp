module;

#include <arrow-adbc/adbc.h>
#include <huxerui/datatable.h>

module dbpp.adbc;

import std;
import dbpp.types;

namespace dbpp {

namespace {

bool IsNonMatchableBlobType(std::string_view type) {
  const std::string lower = detail::AdbcLowerAscii(type);
  return lower.find("blob") != std::string::npos || lower.find("bytea") != std::string::npos ||
         lower.find("binary") != std::string::npos || lower.find("image") != std::string::npos;
}

ColumnMeta* FindColumnMutable(std::vector<ColumnMeta>& columns, std::string_view name) {
  for (ColumnMeta& col : columns) {
    if (col.name == name) {
      return &col;
    }
  }
  for (ColumnMeta& col : columns) {
    if (detail::AdbcEqualsIgnoreCase(col.name, name)) {
      return &col;
    }
  }
  return nullptr;
}

std::optional<std::size_t> FindColumnIndex(const std::vector<ColumnMeta>& columns,
                                           std::string_view name) {
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (columns[i].name == name) {
      return i;
    }
  }
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (detail::AdbcEqualsIgnoreCase(columns[i].name, name)) {
      return i;
    }
  }
  return std::nullopt;
}

std::string CellTextAt(const GridRow& row, std::size_t col) {
  if (col >= row.values.size() || row.values[col].is_null) {
    return {};
  }
  return row.values[col].text;
}

void ApplyPrimaryKeyList(std::vector<ColumnMeta>& columns, const std::vector<std::string>& names) {
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (ColumnMeta* col = FindColumnMutable(columns, names[i])) {
      col->primary_key = true;
      if (col->primary_key_order == 0) {
        col->primary_key_order = static_cast<int>(i + 1);
      }
      col->nullable = false;
    }
  }
}

void MergeObjectHints(const DbObject& object, std::vector<ColumnMeta>& columns,
                      std::vector<std::vector<std::string>>& unique_candidates) {
  for (const ColumnMeta& known : object.columns) {
    if (ColumnMeta* col = FindColumnMutable(columns, known.name)) {
      if (!known.type.empty() && (col->type.empty() || col->type == "ANY")) {
        col->type = known.type;
      }
      if (!known.nullable) {
        col->nullable = false;
      }
      if (col->default_expr.empty()) {
        col->default_expr = known.default_expr;
      }
      if (col->remarks.empty()) {
        col->remarks = known.remarks;
      }
      col->unique = col->unique || known.unique;
      col->autoincrement = col->autoincrement || known.autoincrement;
      col->generated = col->generated || known.generated;
      if (known.primary_key) {
        col->primary_key = true;
        if (col->primary_key_order == 0) {
          col->primary_key_order = known.primary_key_order;
        }
      }
    }
  }

  if (!object.primary_key.empty()) {
    ApplyPrimaryKeyList(columns, object.primary_key);
  }
  for (const IndexMeta& idx : object.indexes) {
    if (idx.columns.empty()) {
      continue;
    }
    if (idx.primary) {
      ApplyPrimaryKeyList(columns, idx.columns);
    } else if (idx.unique) {
      unique_candidates.push_back(idx.columns);
    }
  }
  for (const TableConstraintMeta& c : object.constraints) {
    if (c.columns.empty()) {
      continue;
    }
    const std::string t = detail::AdbcLowerAscii(c.type);
    if (t == "primary key") {
      ApplyPrimaryKeyList(columns, c.columns);
    } else if (t == "unique") {
      unique_candidates.push_back(c.columns);
    }
  }
}

void ProbeSqliteMetadata(AdbcSession& session, const DbObject& object,
                         std::vector<ColumnMeta>& columns,
                         std::vector<std::vector<std::string>>& unique_candidates) {
  const std::string schema_prefix =
      object.db_schema.empty()
          ? std::string{}
          : (QuoteIdentifier(Dialect::Sqlite, object.db_schema) + ".");
  const std::string q_tbl = QuoteIdentifier(Dialect::Sqlite, object.name);
  const std::string pragma_target = "PRAGMA " + schema_prefix + "table_info(" + q_tbl + ")";
  const AdbcQueryResult info = detail::AdbcExecuteBlockingRaw(session, pragma_target, true, 0);
  if (info.error.empty()) {
    for (const GridRow& r : info.rows) {
      if (r.values.size() < 6) {
        continue;
      }
      const std::string col_name = CellTextAt(r, 1);
      const std::string col_type = CellTextAt(r, 2);
      const auto notnull_flag = detail::AdbcParseInt64(CellTextAt(r, 3));
      const std::string dflt_val = r.values[4].is_null ? std::string{} : r.values[4].text;
      const auto pk_ord = detail::AdbcParseInt64(CellTextAt(r, 5));
      if (ColumnMeta* col = FindColumnMutable(columns, col_name)) {
        if (!col_type.empty() && (col->type.empty() || col->type == "ANY")) {
          col->type = col_type;
        }
        if (notnull_flag.value_or(0) != 0) {
          col->nullable = false;
        }
        if (col->default_expr.empty() && !dflt_val.empty()) {
          col->default_expr = dflt_val;
        }
        if (pk_ord.value_or(0) > 0) {
          col->primary_key = true;
          col->primary_key_order = static_cast<int>(*pk_ord);
        }
      }
    }
  }

  const AdbcQueryResult idx_list = detail::AdbcExecuteBlockingRaw(
      session, "PRAGMA " + schema_prefix + "index_list(" + q_tbl + ")", true, 512);
  if (idx_list.error.empty()) {
    for (const GridRow& r : idx_list.rows) {
      if (r.values.size() < 4 || CellTextAt(r, 2) != "1") {
        continue;
      }
      const std::string idx_name = CellTextAt(r, 1);
      if (idx_name.empty() || detail::AdbcEqualsIgnoreCase(CellTextAt(r, 3), "pk")) {
        continue;
      }
      const AdbcQueryResult idx_info = detail::AdbcExecuteBlockingRaw(
          session,
          "PRAGMA " + schema_prefix + "index_info(" + QuoteIdentifier(Dialect::Sqlite, idx_name) +
              ")",
          true, 64);
      if (!idx_info.error.empty()) {
        continue;
      }
      std::vector<std::string> idx_cols;
      for (const GridRow& cr : idx_info.rows) {
        if (cr.values.size() >= 3) {
          const std::string cname = CellTextAt(cr, 2);
          if (!cname.empty()) {
            idx_cols.push_back(cname);
          }
        }
      }
      if (!idx_cols.empty()) {
        unique_candidates.push_back(std::move(idx_cols));
      }
    }
  }
}

void ProbeMysqlMetadata(AdbcSession& session, const DbObject& object,
                        std::vector<ColumnMeta>& columns,
                        std::vector<std::vector<std::string>>& unique_candidates) {
  const std::string qualified = QualifiedName(Dialect::Mysql, object);

  AdbcQueryResult col_res = detail::AdbcExecuteBlockingRaw(
      session, "SHOW FULL COLUMNS FROM " + qualified, true, 1024);
  if (col_res.error.empty()) {
    for (const GridRow& r : col_res.rows) {
      if (r.values.size() < 6) {
        continue;
      }
      ColumnMeta* col = FindColumnMutable(columns, CellTextAt(r, 0));
      if (col == nullptr) {
        continue;
      }
      const std::string col_type = CellTextAt(r, 1);
      const std::string null_str = detail::AdbcLowerAscii(CellTextAt(r, 3));
      const std::string key_str = detail::AdbcLowerAscii(CellTextAt(r, 4));
      const std::string dflt_str = r.values[5].is_null ? std::string{} : r.values[5].text;
      const std::string extra_str = r.values.size() > 6 ? detail::AdbcLowerAscii(CellTextAt(r, 6))
                                                        : std::string{};
      const std::string comment_str = r.values.size() > 8 ? CellTextAt(r, 8) : std::string{};
      if (!col_type.empty() && (col->type.empty() || col->type == "ANY")) {
        col->type = col_type;
      }
      if (null_str == "no") {
        col->nullable = false;
      } else if (null_str == "yes") {
        col->nullable = true;
      }
      if (key_str == "pri") {
        col->primary_key = true;
      } else if (key_str == "uni") {
        col->unique = true;
      }
      if (col->default_expr.empty() && !dflt_str.empty()) {
        col->default_expr = dflt_str;
      }
      if (extra_str.find("auto_increment") != std::string::npos) {
        col->autoincrement = true;
      }
      if (extra_str.find("generated") != std::string::npos) {
        col->generated = true;
      }
      if (col->remarks.empty() && !comment_str.empty()) {
        col->remarks = comment_str;
      }
    }
  } else {
    AdbcQueryResult basic_res =
        detail::AdbcExecuteBlockingRaw(session, "SHOW COLUMNS FROM " + qualified, true, 1024);
    if (basic_res.error.empty()) {
      for (const GridRow& r : basic_res.rows) {
        if (r.values.size() < 5) {
          continue;
        }
        ColumnMeta* col = FindColumnMutable(columns, CellTextAt(r, 0));
        if (col == nullptr) {
          continue;
        }
        const std::string col_type = CellTextAt(r, 1);
        const std::string null_str = detail::AdbcLowerAscii(CellTextAt(r, 2));
        const std::string key_str = detail::AdbcLowerAscii(CellTextAt(r, 3));
        if (!col_type.empty() && (col->type.empty() || col->type == "ANY")) {
          col->type = col_type;
        }
        if (null_str == "no") {
          col->nullable = false;
        } else if (null_str == "yes") {
          col->nullable = true;
        }
        if (key_str == "pri") {
          col->primary_key = true;
        } else if (key_str == "uni") {
          col->unique = true;
        }
      }
    }
  }

  AdbcQueryResult idx_res =
      detail::AdbcExecuteBlockingRaw(session, "SHOW INDEX FROM " + qualified, true, 2048);
  if (idx_res.error.empty()) {
    struct GroupedIdx {
      bool is_primary = false;
      bool is_unique = false;
      std::vector<std::pair<std::int64_t, std::string>> seq_cols;
    };
    std::unordered_map<std::string, GroupedIdx> grouped;
    std::vector<std::string> order;
    for (const GridRow& r : idx_res.rows) {
      if (r.values.size() < 5) {
        continue;
      }
      const bool non_unique = (CellTextAt(r, 1) != "0");
      const std::string key_name = CellTextAt(r, 2);
      const std::int64_t seq = detail::AdbcParseInt64(CellTextAt(r, 3)).value_or(0);
      const std::string col_name = CellTextAt(r, 4);
      if (key_name.empty() || col_name.empty() || non_unique) {
        continue;
      }
      auto [it, inserted] = grouped.try_emplace(key_name);
      if (inserted) {
        it->second.is_primary = detail::AdbcEqualsIgnoreCase(key_name, "PRIMARY");
        it->second.is_unique = true;
        order.push_back(key_name);
      }
      it->second.seq_cols.emplace_back(seq, col_name);
    }
    for (const std::string& key_name : order) {
      GroupedIdx& g = grouped[key_name];
      std::ranges::sort(g.seq_cols, [](const auto& a, const auto& b) {
        return a.first < b.first;
      });
      std::vector<std::string> cols;
      cols.reserve(g.seq_cols.size());
      for (const auto& [_, cname] : g.seq_cols) {
        cols.push_back(cname);
      }
      if (g.is_primary) {
        ApplyPrimaryKeyList(columns, cols);
      } else if (g.is_unique) {
        unique_candidates.push_back(std::move(cols));
      }
    }
  }
}

void ProbePostgresMetadata(AdbcSession& session, const DbObject& object,
                           std::vector<ColumnMeta>& columns,
                           std::vector<std::vector<std::string>>& unique_candidates) {
  const std::string schema_expr =
      object.db_schema.empty()
          ? std::string{"current_schema()"}
          : SqlLiteral(Dialect::Postgresql, object.db_schema, "TEXT", false);
  const std::string tbl_lit = SqlLiteral(Dialect::Postgresql, object.name, "TEXT", false);

  const std::string col_sql =
      "SELECT column_name, COALESCE(udt_name, data_type, ''), is_nullable, "
      "COALESCE(column_default, '') "
      "FROM information_schema.columns "
      "WHERE table_schema = " +
      schema_expr + " AND table_name = " + tbl_lit + " ORDER BY ordinal_position";
  const AdbcQueryResult col_res = detail::AdbcExecuteBlockingRaw(session, col_sql, true, 1024);
  if (col_res.error.empty()) {
    for (const GridRow& r : col_res.rows) {
      if (r.values.size() < 4) {
        continue;
      }
      if (ColumnMeta* col = FindColumnMutable(columns, CellTextAt(r, 0))) {
        const std::string tp = CellTextAt(r, 1);
        const std::string is_null = detail::AdbcLowerAscii(CellTextAt(r, 2));
        const std::string dflt = CellTextAt(r, 3);
        if (!tp.empty() && (col->type.empty() || col->type == "ANY")) {
          col->type = tp;
        }
        if (is_null == "no") {
          col->nullable = false;
        }
        if (col->default_expr.empty() && !dflt.empty()) {
          col->default_expr = dflt;
        }
      }
    }
  }

  const std::string idx_sql =
      "SELECT c_idx.relname, i.indisprimary, a.attname, "
      "array_position(i.indkey::int[], a.attnum::int) AS ord "
      "FROM pg_catalog.pg_index i "
      "JOIN pg_catalog.pg_class c_tbl ON c_tbl.oid = i.indrelid "
      "JOIN pg_catalog.pg_namespace n ON n.oid = c_tbl.relnamespace "
      "JOIN pg_catalog.pg_class c_idx ON c_idx.oid = i.indexrelid "
      "JOIN pg_catalog.pg_attribute a ON a.attrelid = c_tbl.oid AND a.attnum = ANY(i.indkey) "
      "WHERE n.nspname = " +
      schema_expr + " AND c_tbl.relname = " + tbl_lit +
      " AND (i.indisprimary OR i.indisunique) AND i.indpred IS NULL AND a.attnum > 0 "
      "ORDER BY c_idx.relname, ord";
  const AdbcQueryResult idx_res = detail::AdbcExecuteBlockingRaw(session, idx_sql, true, 1024);
  if (idx_res.error.empty()) {
    struct GroupedPgIdx {
      bool is_primary = false;
      std::vector<std::string> cols;
    };
    std::unordered_map<std::string, GroupedPgIdx> grouped;
    std::vector<std::string> order;
    for (const GridRow& r : idx_res.rows) {
      if (r.values.size() < 3) {
        continue;
      }
      const std::string idx_name = CellTextAt(r, 0);
      const std::string pk_raw = detail::AdbcLowerAscii(CellTextAt(r, 1));
      const bool is_pk = (pk_raw == "t" || pk_raw == "true" || pk_raw == "1");
      const std::string col_name = CellTextAt(r, 2);
      if (idx_name.empty() || col_name.empty()) {
        continue;
      }
      auto [it, inserted] = grouped.try_emplace(idx_name);
      if (inserted) {
        it->second.is_primary = is_pk;
        order.push_back(idx_name);
      }
      it->second.cols.push_back(col_name);
    }
    for (const std::string& idx_name : order) {
      GroupedPgIdx& g = grouped[idx_name];
      if (g.is_primary) {
        ApplyPrimaryKeyList(columns, g.cols);
      } else {
        unique_candidates.push_back(std::move(g.cols));
      }
    }
  }
}

void ProbeDuckdbMetadata(AdbcSession& session, const DbObject& object,
                         std::vector<ColumnMeta>& columns) {
  const std::string target = QualifiedName(Dialect::Duckdb, object);
  const std::string sql =
      "PRAGMA table_info(" + SqlLiteral(Dialect::Duckdb, target, "TEXT", false) + ")";
  const AdbcQueryResult info = detail::AdbcExecuteBlockingRaw(session, sql, true, 0);
  if (!info.error.empty()) {
    return;
  }
  for (const GridRow& r : info.rows) {
    if (r.values.size() < 6) {
      continue;
    }
    const std::string col_name = CellTextAt(r, 1);
    const std::string col_type = CellTextAt(r, 2);
    const std::string nn = detail::AdbcLowerAscii(CellTextAt(r, 3));
    const std::string pk = detail::AdbcLowerAscii(CellTextAt(r, 5));
    if (ColumnMeta* col = FindColumnMutable(columns, col_name)) {
      if (!col_type.empty() && (col->type.empty() || col->type == "ANY")) {
        col->type = col_type;
      }
      if (nn == "1" || nn == "true") {
        col->nullable = false;
      }
      if (pk == "1" || pk == "true") {
        col->primary_key = true;
      }
    }
  }
}

void ProbeGenericMetadata(AdbcSession& session, const DbObject& object,
                          std::vector<ColumnMeta>& columns,
                          std::vector<std::vector<std::string>>& unique_candidates) {
  const std::string tbl_lit = SqlLiteral(Dialect::Generic, object.name, "TEXT", false);
  const std::string schema_clause =
      object.db_schema.empty()
          ? std::string{}
          : (" AND table_schema = " + SqlLiteral(Dialect::Generic, object.db_schema, "TEXT", false));

  const std::string col_sql =
      "SELECT column_name, data_type, is_nullable, COALESCE(column_default, '') "
      "FROM information_schema.columns WHERE table_name = " +
      tbl_lit + schema_clause + " ORDER BY ordinal_position";
  const AdbcQueryResult col_res = detail::AdbcExecuteBlockingRaw(session, col_sql, true, 1024);
  if (col_res.error.empty()) {
    for (const GridRow& r : col_res.rows) {
      if (r.values.size() < 4) {
        continue;
      }
      if (ColumnMeta* col = FindColumnMutable(columns, CellTextAt(r, 0))) {
        const std::string tp = CellTextAt(r, 1);
        const std::string is_null = detail::AdbcLowerAscii(CellTextAt(r, 2));
        const std::string dflt = CellTextAt(r, 3);
        if (!tp.empty() && (col->type.empty() || col->type == "ANY")) {
          col->type = tp;
        }
        if (is_null == "no") {
          col->nullable = false;
        }
        if (col->default_expr.empty() && !dflt.empty()) {
          col->default_expr = dflt;
        }
      }
    }
  }

  const std::string key_sql =
      "SELECT tc.constraint_name, tc.constraint_type, kcu.column_name "
      "FROM information_schema.table_constraints tc "
      "JOIN information_schema.key_column_usage kcu "
      "  ON tc.constraint_name = kcu.constraint_name "
      " AND tc.table_name = kcu.table_name "
      "WHERE tc.table_name = " +
      tbl_lit +
      (object.db_schema.empty()
           ? std::string{}
           : (" AND tc.table_schema = " +
              SqlLiteral(Dialect::Generic, object.db_schema, "TEXT", false))) +
      " AND tc.constraint_type IN ('PRIMARY KEY', 'UNIQUE') "
      "ORDER BY tc.constraint_name, kcu.ordinal_position";
  const AdbcQueryResult key_res = detail::AdbcExecuteBlockingRaw(session, key_sql, true, 1024);
  if (key_res.error.empty()) {
    struct GroupedKey {
      bool is_pk = false;
      std::vector<std::string> cols;
    };
    std::unordered_map<std::string, GroupedKey> grouped;
    std::vector<std::string> order;
    for (const GridRow& r : key_res.rows) {
      if (r.values.size() < 3) {
        continue;
      }
      const std::string cname = CellTextAt(r, 0);
      const std::string ctype = detail::AdbcLowerAscii(CellTextAt(r, 1));
      const std::string col_name = CellTextAt(r, 2);
      if (cname.empty() || col_name.empty()) {
        continue;
      }
      auto [it, inserted] = grouped.try_emplace(cname);
      if (inserted) {
        it->second.is_pk = (ctype == "primary key");
        order.push_back(cname);
      }
      it->second.cols.push_back(col_name);
    }
    for (const std::string& cname : order) {
      GroupedKey& g = grouped[cname];
      if (g.is_pk) {
        ApplyPrimaryKeyList(columns, g.cols);
      } else {
        unique_candidates.push_back(std::move(g.cols));
      }
    }
  }
}

bool TrySelectUniqueCandidate(const std::vector<ColumnMeta>& columns,
                              const std::vector<std::vector<std::string>>& unique_candidates,
                              std::vector<std::size_t>& key_cols_out) {
  std::vector<std::vector<std::string>> all_candidates = unique_candidates;
  for (const ColumnMeta& col : columns) {
    if (col.unique && !col.primary_key) {
      all_candidates.push_back({col.name});
    }
  }

  std::vector<std::size_t> best_nullable_set;
  for (const auto& candidate : all_candidates) {
    if (candidate.empty()) {
      continue;
    }
    std::vector<std::size_t> indices;
    bool all_found = true;
    bool all_not_null = true;
    for (const std::string& name : candidate) {
      const auto idx = FindColumnIndex(columns, name);
      if (!idx.has_value()) {
        all_found = false;
        break;
      }
      if (columns[*idx].nullable) {
        all_not_null = false;
      }
      indices.push_back(*idx);
    }
    if (!all_found) {
      continue;
    }
    if (all_not_null) {
      key_cols_out = std::move(indices);
      return true;
    }
    if (best_nullable_set.empty() || indices.size() < best_nullable_set.size()) {
      best_nullable_set = std::move(indices);
    }
  }
  if (!best_nullable_set.empty()) {
    key_cols_out = std::move(best_nullable_set);
    return true;
  }
  return false;
}

} // namespace

ResolvedRowIdentity ResolveTableIdentityBlocking(AdbcSession& session, const DbObject& object,
                                                 std::vector<ColumnMeta>& columns) {
  ResolvedRowIdentity resolved;
  std::vector<std::vector<std::string>> unique_candidates;

  MergeObjectHints(object, columns, unique_candidates);

  const bool is_read_only_object =
      object.is_view || detail::AdbcContainsLower(object.table_type, "foreign");
  if (!is_read_only_object && session.dialect != Dialect::Redis) {
    switch (session.dialect) {
      case Dialect::Sqlite: ProbeSqliteMetadata(session, object, columns, unique_candidates); break;
      case Dialect::Mysql: ProbeMysqlMetadata(session, object, columns, unique_candidates); break;
      case Dialect::Postgresql:
        ProbePostgresMetadata(session, object, columns, unique_candidates);
        break;
      case Dialect::Duckdb: ProbeDuckdbMetadata(session, object, columns); break;
      case Dialect::Generic:
        ProbeGenericMetadata(session, object, columns, unique_candidates);
        break;
      case Dialect::Redis: break;
    }
  }

  if (is_read_only_object || session.dialect == Dialect::Redis) {
    return resolved;
  }

  // 1. Explicit PRIMARY KEY
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (columns[i].primary_key) {
      resolved.key_columns.push_back(i);
    }
  }
  if (!resolved.key_columns.empty()) {
    std::ranges::sort(resolved.key_columns, [&columns](std::size_t l, std::size_t r) {
      return columns[l].primary_key_order < columns[r].primary_key_order;
    });
    resolved.kind = RowLocatorKind::PrimaryKey;
    return resolved;
  }

  // 2. UNIQUE index/constraint
  if (TrySelectUniqueCandidate(columns, unique_candidates, resolved.key_columns)) {
    resolved.kind = RowLocatorKind::UniqueKey;
    return resolved;
  }

  // 3. Engine-native hidden row locator (SQLite/DuckDB rowid, PostgreSQL ctid)
  const std::string qualified = QualifiedName(session.dialect, object);
  if (session.dialect == Dialect::Sqlite || session.dialect == Dialect::Duckdb) {
    const AdbcQueryResult probe =
        detail::AdbcExecuteBlockingRaw(session, "SELECT rowid FROM " + qualified + " LIMIT 0",
                                       true, 0);
    if (probe.error.empty()) {
      resolved.kind = RowLocatorKind::HiddenRowId;
      resolved.hidden_rowid_expr = "rowid";
      resolved.hidden_rowid_where_col = "rowid";
      resolved.hidden_rowid_is_numeric = true;
      return resolved;
    }
  } else if (session.dialect == Dialect::Postgresql) {
    const AdbcQueryResult probe =
        detail::AdbcExecuteBlockingRaw(session, "SELECT ctid::text FROM " + qualified + " LIMIT 0",
                                       true, 0);
    if (probe.error.empty()) {
      resolved.kind = RowLocatorKind::HiddenRowId;
      resolved.hidden_rowid_expr = "ctid::text";
      resolved.hidden_rowid_where_col = "ctid";
      resolved.hidden_rowid_cast = "::tid";
      resolved.hidden_rowid_is_numeric = false;
      return resolved;
    }
  }

  // 4. Keyless table fallback: match non-BLOB columns (+ LIMIT 1 on MySQL in UPDATE/DELETE)
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (!IsNonMatchableBlobType(columns[i].type)) {
      resolved.key_columns.push_back(i);
    }
  }
  if (!resolved.key_columns.empty()) {
    resolved.kind = RowLocatorKind::AllColumns;
  }
  return resolved;
}

} // namespace dbpp
