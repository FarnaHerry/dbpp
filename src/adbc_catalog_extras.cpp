module;

#include <arrow-adbc/adbc.h>
#include <huxerui/datatable.h>

module dbpp.adbc;

import std;
import dbpp.types;
import dbpp.i18n;
import dbpp.redis;

namespace dbpp::detail {

namespace {

bool RunBestEffortQuery(AdbcSession& session, std::string sql, AdbcQueryResult& out,
                        std::size_t max_rows = 2000) {
  out = AdbcExecuteBlockingRaw(session, std::move(sql), true, max_rows);
  return out.error.empty() && !out.connection_lost;
}

std::string CellAt(const GridRow& row, std::size_t col) {
  if (col >= row.values.size() || row.values[col].is_null) {
    return {};
  }
  return row.values[col].text;
}

void AddUniqueDatabaseName(std::vector<std::string>& databases, std::string_view name) {
  const std::string trimmed(AdbcTrimAscii(name));
  if (trimmed.empty()) {
    return;
  }
  if (std::ranges::find(databases, trimmed) == databases.end()) {
    databases.push_back(trimmed);
  }
}

void ParseSqliteTriggerTimingAndEvent(std::string_view sql, std::string& timing_out,
                                      std::string& event_out) {
  const std::string upper_raw(sql);
  std::string norm;
  norm.reserve(upper_raw.size());
  bool prev_space = true;
  for (const unsigned char ch : upper_raw) {
    if (std::isspace(ch) != 0) {
      if (!prev_space) {
        norm.push_back(' ');
        prev_space = true;
      }
      continue;
    }
    norm.push_back(static_cast<char>(std::toupper(ch)));
    prev_space = false;
  }
  const std::size_t on_pos = norm.find(" ON ");
  const std::string_view header =
      on_pos != std::string::npos ? std::string_view(norm).substr(0, on_pos + 1)
                                  : std::string_view(norm);

  if (header.find(" INSTEAD OF ") != std::string_view::npos) {
    timing_out = "INSTEAD OF";
  } else if (header.find(" BEFORE ") != std::string_view::npos) {
    timing_out = "BEFORE";
  } else if (header.find(" AFTER ") != std::string_view::npos) {
    timing_out = "AFTER";
  }

  std::string evt;
  const auto add_evt = [&evt, header](std::string_view word) {
    const std::string needle = " " + std::string(word);
    if (header.find(needle + " ") != std::string_view::npos ||
        header.find(needle + " OF ") != std::string_view::npos) {
      if (!evt.empty()) {
        evt += "/";
      }
      evt += word;
    }
  };
  add_evt("INSERT");
  add_evt("UPDATE");
  add_evt("DELETE");
  event_out = std::move(evt);
}

std::vector<std::string> SplitCsvIdentifiers(std::string_view text) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::string_view part =
        comma == std::string_view::npos ? text.substr(start) : text.substr(start, comma - start);
    const std::string trimmed(AdbcTrimAscii(part));
    if (!trimmed.empty()) {
      out.push_back(trimmed);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return out;
}

DbObject* FindObjectMutable(std::vector<DbObject>& objects, std::string_view schema,
                            std::string_view table_name) {
  if (!schema.empty()) {
    for (DbObject& obj : objects) {
      if (AdbcEqualsIgnoreCase(obj.name, table_name) &&
          (AdbcEqualsIgnoreCase(obj.db_schema, schema) ||
           AdbcEqualsIgnoreCase(obj.catalog, schema))) {
        return &obj;
      }
    }
  }
  for (DbObject& obj : objects) {
    if (AdbcEqualsIgnoreCase(obj.name, table_name)) {
      return &obj;
    }
  }
  return nullptr;
}

void EnrichSqliteCatalog(AdbcSession& session, AdbcObjectsResult& result) {
  std::vector<std::string> sqlite_dbs;
  AdbcQueryResult db_list;
  if (RunBestEffortQuery(session, "PRAGMA database_list", db_list, 256)) {
    for (const GridRow& row : db_list.rows) {
      if (row.values.size() >= 2) {
        const std::string db_name = CellAt(row, 1);
        if (!db_name.empty()) {
          sqlite_dbs.push_back(db_name);
          AddUniqueDatabaseName(result.databases, db_name);
        }
      }
    }
  }
  if (sqlite_dbs.empty()) {
    sqlite_dbs.push_back("main");
  }

  for (const std::string& db_name : sqlite_dbs) {
    const std::string master_tbl =
        (db_name == "main")
            ? "sqlite_master"
            : ((db_name == "temp")
                   ? "sqlite_temp_master"
                   : (QuoteIdentifier(Dialect::Sqlite, db_name) + ".sqlite_master"));
    AdbcQueryResult master_res;
    if (RunBestEffortQuery(
            session,
            "SELECT type, name, tbl_name, COALESCE(sql, '') FROM " + master_tbl +
                " WHERE type IN ('table', 'view', 'index', 'trigger') ORDER BY name",
            master_res, 5000)) {
      for (const GridRow& row : master_res.rows) {
        const std::string kind = AdbcLowerAscii(CellAt(row, 0));
        const std::string name = CellAt(row, 1);
        const std::string tbl = CellAt(row, 2);
        const std::string sql = CellAt(row, 3);
        if (name.empty()) {
          continue;
        }
        if (kind == "trigger") {
          TriggerMeta trg;
          trg.name = name;
          trg.db_schema = db_name;
          trg.table_name = tbl;
          trg.definition = sql;
          ParseSqliteTriggerTimingAndEvent(sql, trg.timing, trg.event);
          result.triggers.push_back(std::move(trg));
        } else if (kind == "view") {
          if (FindObjectMutable(result.objects, db_name, name) == nullptr) {
            DbObject view_obj;
            view_obj.name = name;
            view_obj.db_schema = db_name;
            view_obj.table_type = "VIEW";
            view_obj.is_view = true;
            result.objects.push_back(std::move(view_obj));
          }
        } else if (kind == "table" && !name.starts_with("sqlite_")) {
          if (FindObjectMutable(result.objects, db_name, name) == nullptr) {
            DbObject tbl_obj;
            tbl_obj.name = name;
            tbl_obj.db_schema = db_name;
            tbl_obj.table_type = "TABLE";
            tbl_obj.is_view = false;
            result.objects.push_back(std::move(tbl_obj));
          }
        }
      }
    }
  }

  // Enrich indexes, PKs, and foreign keys per table via SQLite PRAGMAs.
  std::size_t scanned_tables = 0;
  for (DbObject& obj : result.objects) {
    if (obj.is_view || scanned_tables >= 200) {
      continue;
    }
    ++scanned_tables;
    const std::string schema_prefix =
        obj.db_schema.empty() ? std::string{} : (QuoteIdentifier(Dialect::Sqlite, obj.db_schema) + ".");
    const std::string q_tbl = QuoteIdentifier(Dialect::Sqlite, obj.name);

    AdbcQueryResult idx_list;
    if (RunBestEffortQuery(session, "PRAGMA " + schema_prefix + "index_list(" + q_tbl + ")",
                           idx_list, 512)) {
      for (const GridRow& row : idx_list.rows) {
        if (row.values.size() < 4) {
          continue;
        }
        IndexMeta idx;
        idx.name = CellAt(row, 1);
        idx.unique = (CellAt(row, 2) == "1");
        idx.primary = (AdbcLowerAscii(CellAt(row, 3)) == "pk");
        if (idx.name.empty()) {
          continue;
        }
        AdbcQueryResult idx_info;
        if (RunBestEffortQuery(session,
                               "PRAGMA " + schema_prefix + "index_info(" +
                                   QuoteIdentifier(Dialect::Sqlite, idx.name) + ")",
                               idx_info, 64)) {
          for (const GridRow& col_row : idx_info.rows) {
            if (col_row.values.size() >= 3) {
              const std::string cname = CellAt(col_row, 2);
              if (!cname.empty()) {
                idx.columns.push_back(cname);
              }
            }
          }
        }
        obj.indexes.push_back(std::move(idx));
      }
    }

    if (obj.foreign_keys.empty()) {
      AdbcQueryResult fk_res;
      if (RunBestEffortQuery(
              session, "PRAGMA " + schema_prefix + "foreign_key_list(" + q_tbl + ")", fk_res, 256)) {
        std::unordered_map<std::string, ForeignKeyMeta> grouped;
        std::vector<std::string> fk_ids;
        for (const GridRow& row : fk_res.rows) {
          if (row.values.size() < 5) {
            continue;
          }
          const std::string id = CellAt(row, 0);
          const std::string ref_table = CellAt(row, 2);
          const std::string from_col = CellAt(row, 3);
          const std::string to_col = CellAt(row, 4);
          auto it = grouped.find(id);
          if (it == grouped.end()) {
            ForeignKeyMeta fk;
            fk.constraint_name = "fk_" + obj.name + "_" + id;
            fk.fk_db_schema = obj.db_schema;
            fk.fk_table = ref_table;
            fk.columns.push_back(from_col);
            if (!to_col.empty()) {
              fk.fk_columns.push_back(to_col);
            }
            grouped.emplace(id, std::move(fk));
            fk_ids.push_back(id);
          } else {
            it->second.columns.push_back(from_col);
            if (!to_col.empty()) {
              it->second.fk_columns.push_back(to_col);
            }
          }
        }
        for (const std::string& id : fk_ids) {
          obj.foreign_keys.push_back(std::move(grouped[id]));
        }
      }
    }
  }
}

void EnrichPostgresqlCatalog(AdbcSession& session, AdbcObjectsResult& result) {
  AdbcQueryResult ns_res;
  if (RunBestEffortQuery(
          session, "SELECT nspname FROM pg_catalog.pg_namespace ORDER BY nspname", ns_res, 512)) {
    for (const GridRow& row : ns_res.rows) {
      AddUniqueDatabaseName(result.databases, CellAt(row, 0));
    }
  }

  AdbcQueryResult idx_res;
  if (RunBestEffortQuery(
          session,
          "SELECT schemaname, tablename, indexname, indexdef "
          "FROM pg_catalog.pg_indexes ORDER BY schemaname, tablename, indexname LIMIT 2000",
          idx_res, 2000)) {
    for (const GridRow& row : idx_res.rows) {
      const std::string sch = CellAt(row, 0);
      const std::string tbl = CellAt(row, 1);
      const std::string idx_name = CellAt(row, 2);
      const std::string def = CellAt(row, 3);
      if (DbObject* obj = FindObjectMutable(result.objects, sch, tbl)) {
        IndexMeta idx;
        idx.name = idx_name;
        idx.unique = AdbcContainsLower(def, "unique index");
        idx.primary = AdbcContainsLower(idx_name, "_pkey");
        idx.definition = def;
        obj->indexes.push_back(std::move(idx));
      }
    }
  }

  AdbcQueryResult trg_res;
  if (RunBestEffortQuery(
          session,
          "SELECT trigger_schema, trigger_name, event_object_table, action_timing, "
          "STRING_AGG(event_manipulation, '/'), COALESCE(MAX(action_statement), '') "
          "FROM information_schema.triggers "
          "GROUP BY trigger_schema, trigger_name, event_object_table, action_timing "
          "ORDER BY trigger_schema, trigger_name LIMIT 1000",
          trg_res, 1000)) {
    for (const GridRow& row : trg_res.rows) {
      TriggerMeta trg;
      trg.db_schema = CellAt(row, 0);
      trg.name = CellAt(row, 1);
      trg.table_name = CellAt(row, 2);
      trg.timing = CellAt(row, 3);
      trg.event = CellAt(row, 4);
      trg.definition = CellAt(row, 5);
      if (!trg.name.empty()) {
        result.triggers.push_back(std::move(trg));
      }
    }
  }

  AdbcQueryResult rt_res;
  if (RunBestEffortQuery(
          session,
          "SELECT n.nspname, p.proname, p.prokind::text, "
          "COALESCE(pg_catalog.format_type(p.prorettype, NULL), ''), "
          "COALESCE(pg_catalog.pg_get_function_identity_arguments(p.oid), '') "
          "FROM pg_catalog.pg_proc p "
          "JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace "
          "WHERE n.nspname NOT IN ('pg_catalog', 'information_schema') "
          "ORDER BY n.nspname, p.proname LIMIT 1000",
          rt_res, 1000)) {
    for (const GridRow& row : rt_res.rows) {
      RoutineMeta rt;
      rt.db_schema = CellAt(row, 0);
      rt.name = CellAt(row, 1);
      rt.is_function = (AdbcLowerAscii(CellAt(row, 2)) != "p");
      rt.return_type = CellAt(row, 3);
      rt.signature = CellAt(row, 4);
      if (!rt.name.empty()) {
        result.routines.push_back(std::move(rt));
      }
    }
  }

  AdbcQueryResult evt_res;
  if (RunBestEffortQuery(
          session,
          "SELECT evtname, evtevent, evtenabled::text FROM pg_catalog.pg_event_trigger "
          "ORDER BY evtname LIMIT 500",
          evt_res, 500)) {
    for (const GridRow& row : evt_res.rows) {
      EventMeta ev;
      ev.db_schema = "pg_catalog";
      ev.name = CellAt(row, 0);
      ev.schedule = CellAt(row, 1);
      ev.status = CellAt(row, 2);
      if (!ev.name.empty()) {
        result.events.push_back(std::move(ev));
      }
    }
  }
}

void EnrichMysqlCatalog(AdbcSession& session, AdbcObjectsResult& result) {
  AdbcQueryResult db_res;
  if (RunBestEffortQuery(session, "SHOW DATABASES", db_res, 512)) {
    for (const GridRow& row : db_res.rows) {
      AddUniqueDatabaseName(result.databases, CellAt(row, 0));
    }
  }

  AdbcQueryResult idx_res;
  if (RunBestEffortQuery(
          session,
          "SELECT TABLE_SCHEMA, TABLE_NAME, INDEX_NAME, MIN(NON_UNIQUE), "
          "GROUP_CONCAT(COLUMN_NAME ORDER BY SEQ_IN_INDEX SEPARATOR ',') "
          "FROM information_schema.STATISTICS "
          "GROUP BY TABLE_SCHEMA, TABLE_NAME, INDEX_NAME "
          "ORDER BY TABLE_SCHEMA, TABLE_NAME, INDEX_NAME LIMIT 3000",
          idx_res, 3000)) {
    for (const GridRow& row : idx_res.rows) {
      const std::string sch = CellAt(row, 0);
      const std::string tbl = CellAt(row, 1);
      const std::string idx_name = CellAt(row, 2);
      if (DbObject* obj = FindObjectMutable(result.objects, sch, tbl)) {
        IndexMeta idx;
        idx.name = idx_name;
        idx.unique = (CellAt(row, 3) == "0");
        idx.primary = AdbcEqualsIgnoreCase(idx_name, "PRIMARY");
        idx.columns = SplitCsvIdentifiers(CellAt(row, 4));
        obj->indexes.push_back(std::move(idx));
      }
    }
  }

  AdbcQueryResult trg_res;
  if (RunBestEffortQuery(
          session,
          "SELECT TRIGGER_SCHEMA, TRIGGER_NAME, EVENT_OBJECT_TABLE, ACTION_TIMING, "
          "EVENT_MANIPULATION, COALESCE(ACTION_STATEMENT, '') "
          "FROM information_schema.TRIGGERS "
          "ORDER BY TRIGGER_SCHEMA, TRIGGER_NAME LIMIT 1000",
          trg_res, 1000)) {
    for (const GridRow& row : trg_res.rows) {
      TriggerMeta trg;
      trg.db_schema = CellAt(row, 0);
      trg.name = CellAt(row, 1);
      trg.table_name = CellAt(row, 2);
      trg.timing = CellAt(row, 3);
      trg.event = CellAt(row, 4);
      trg.definition = CellAt(row, 5);
      if (!trg.name.empty()) {
        result.triggers.push_back(std::move(trg));
      }
    }
  }

  AdbcQueryResult rt_res;
  if (RunBestEffortQuery(
          session,
          "SELECT ROUTINE_SCHEMA, ROUTINE_NAME, ROUTINE_TYPE, "
          "COALESCE(DTD_IDENTIFIER, ''), COALESCE(ROUTINE_DEFINITION, '') "
          "FROM information_schema.ROUTINES "
          "ORDER BY ROUTINE_SCHEMA, ROUTINE_NAME LIMIT 1000",
          rt_res, 1000)) {
    for (const GridRow& row : rt_res.rows) {
      RoutineMeta rt;
      rt.db_schema = CellAt(row, 0);
      rt.name = CellAt(row, 1);
      rt.is_function = (AdbcLowerAscii(CellAt(row, 2)) == "function");
      rt.return_type = CellAt(row, 3);
      rt.definition = CellAt(row, 4);
      if (!rt.name.empty()) {
        result.routines.push_back(std::move(rt));
      }
    }
  }

  AdbcQueryResult evt_res;
  if (RunBestEffortQuery(
          session,
          "SELECT EVENT_SCHEMA, EVENT_NAME, "
          "COALESCE(NULLIF(CONCAT_WS(' ', INTERVAL_VALUE, INTERVAL_FIELD), ''), EVENT_TYPE, ''), "
          "COALESCE(STATUS, ''), COALESCE(EVENT_DEFINITION, '') "
          "FROM information_schema.EVENTS ORDER BY EVENT_SCHEMA, EVENT_NAME LIMIT 500",
          evt_res, 500)) {
    for (const GridRow& row : evt_res.rows) {
      EventMeta ev;
      ev.db_schema = CellAt(row, 0);
      ev.name = CellAt(row, 1);
      ev.schedule = CellAt(row, 2);
      ev.status = CellAt(row, 3);
      ev.definition = CellAt(row, 4);
      if (!ev.name.empty()) {
        result.events.push_back(std::move(ev));
      }
    }
  }
}

void EnrichDuckdbCatalog(AdbcSession& session, AdbcObjectsResult& result) {
  AdbcQueryResult sch_res;
  if (RunBestEffortQuery(
          session, "SELECT DISTINCT schema_name FROM duckdb_schemas() ORDER BY schema_name",
          sch_res, 512)) {
    for (const GridRow& row : sch_res.rows) {
      AddUniqueDatabaseName(result.databases, CellAt(row, 0));
    }
  }
  AdbcQueryResult idx_res;
  if (RunBestEffortQuery(
          session,
          "SELECT schema_name, table_name, index_name, is_unique, is_primary, COALESCE(sql, '') "
          "FROM duckdb_indexes() ORDER BY schema_name, table_name, index_name LIMIT 2000",
          idx_res, 2000)) {
    for (const GridRow& row : idx_res.rows) {
      const std::string sch = CellAt(row, 0);
      const std::string tbl = CellAt(row, 1);
      if (DbObject* obj = FindObjectMutable(result.objects, sch, tbl)) {
        IndexMeta idx;
        idx.name = CellAt(row, 2);
        const std::string uniq = AdbcLowerAscii(CellAt(row, 3));
        const std::string prim = AdbcLowerAscii(CellAt(row, 4));
        idx.unique = (uniq == "true" || uniq == "1");
        idx.primary = (prim == "true" || prim == "1");
        idx.definition = CellAt(row, 5);
        obj->indexes.push_back(std::move(idx));
      }
    }
  }
  AdbcQueryResult fn_res;
  if (RunBestEffortQuery(
          session,
          "SELECT schema_name, function_name, COALESCE(return_type, '') "
          "FROM duckdb_functions() WHERE internal = false ORDER BY schema_name, function_name "
          "LIMIT 1000",
          fn_res, 1000)) {
    for (const GridRow& row : fn_res.rows) {
      RoutineMeta rt;
      rt.db_schema = CellAt(row, 0);
      rt.name = CellAt(row, 1);
      rt.is_function = true;
      rt.return_type = CellAt(row, 2);
      if (!rt.name.empty()) {
        result.routines.push_back(std::move(rt));
      }
    }
  }
}

} // namespace

void AdbcEnrichCatalogObjects(AdbcSession& session, AdbcObjectsResult& result) {
  if (session.IsClosed() || !session.connection_created || session.dialect == Dialect::Redis) {
    return;
  }
  for (const DbObject& obj : result.objects) {
    const std::string db_name = ObjectDatabaseName(session.dialect, obj);
    if (!db_name.empty()) {
      AddUniqueDatabaseName(result.databases, db_name);
    }
  }
  switch (session.dialect) {
    case Dialect::Sqlite: EnrichSqliteCatalog(session, result); break;
    case Dialect::Postgresql: EnrichPostgresqlCatalog(session, result); break;
    case Dialect::Mysql: EnrichMysqlCatalog(session, result); break;
    case Dialect::Duckdb: EnrichDuckdbCatalog(session, result); break;
    case Dialect::Redis:
    case Dialect::Generic: break;
  }

  for (DbObject& obj : result.objects) {
    if (!obj.primary_key.empty()) {
      continue;
    }
    for (const IndexMeta& idx : obj.indexes) {
      if (idx.primary && !idx.columns.empty()) {
        obj.primary_key = idx.columns;
        for (std::size_t ki = 0; ki < obj.primary_key.size(); ++ki) {
          for (ColumnMeta& col : obj.columns) {
            if (col.name == obj.primary_key[ki] ||
                AdbcEqualsIgnoreCase(col.name, obj.primary_key[ki])) {
              col.primary_key = true;
              if (col.primary_key_order == 0) {
                col.primary_key_order = static_cast<int>(ki + 1);
              }
              col.nullable = false;
            }
          }
        }
        break;
      }
    }
  }
}

} // namespace dbpp::detail
