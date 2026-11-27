#include "ui/ui_common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <ranges>

namespace dbpp {

using namespace huxerui;
using namespace dbpp::ui;

std::vector<SidebarObjectGroup> GroupObjectItems(
    Dialect dialect, const std::vector<const DbObject*>& items, bool include_empty_standard = false) {
  if (dialect == Dialect::Redis) {
    SidebarObjectGroup dbs{
        .key = "redis_dbs", .label = app::strings::sidebar_group_redis_dbs, .items = items};
    std::vector<SidebarObjectGroup> groups;
    if (!dbs.items.empty()) {
      groups.push_back(std::move(dbs));
    }
    return groups;
  }

  SidebarObjectGroup tables{
      .key = "tables", .label = app::strings::sidebar_group_tables, .items = {}};
  SidebarObjectGroup views{
      .key = "views", .label = app::strings::sidebar_group_views, .items = {}};
  SidebarObjectGroup mviews{
      .key = "mviews", .label = app::strings::sidebar_group_mviews, .items = {}};
  SidebarObjectGroup foreign{
      .key = "foreign", .label = app::strings::sidebar_group_foreign, .items = {}};

  for (const DbObject* object : items) {
    if (object->is_view) {
      if (ContainsIgnoreCaseAscii(object->table_type, "materialized")) {
        mviews.items.push_back(object);
      } else {
        views.items.push_back(object);
      }
    } else if (ContainsIgnoreCaseAscii(object->table_type, "foreign")) {
      foreign.items.push_back(object);
    } else {
      tables.items.push_back(object);
    }
  }

  std::vector<SidebarObjectGroup> groups;
  for (SidebarObjectGroup* group : {&tables, &views, &mviews, &foreign}) {
    const bool mandatory = include_empty_standard &&
                           (group->key == "tables" || group->key == "views");
    if (mandatory || !group->items.empty()) {
      groups.push_back(std::move(*group));
    }
  }
  return groups;
}

std::vector<SidebarObjectGroup> GroupSessionObjects(const DbSession& session) {
  std::vector<const DbObject*> all_items;
  all_items.reserve(session.objects.size());
  for (const DbObject& object : session.objects) {
    all_items.push_back(&object);
  }
  return GroupObjectItems(session.dialect, all_items, false);
}

std::string SessionObjectBucketName(const DbSession& session, const DbObject& object,
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
      !ContainsIgnoreCaseAscii(object.catalog, "def") &&
      !ContainsIgnoreCaseAscii(object.catalog, "main")) {
    if (!object.db_schema.empty() && object.db_schema != object.catalog) {
      return object.catalog + "." + object.db_schema;
    }
    return object.catalog;
  }
  return ObjectDatabaseName(session.dialect, object);
}

bool SessionHasMultipleCatalogs(const DbSession& session) {
  std::string first_catalog;
  for (const DbObject& object : session.objects) {
    if (object.catalog.empty()) {
      continue;
    }
    std::string lower = object.catalog;
    for (char& ch : lower) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
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

bool SessionUsesDatabaseHierarchy(const DbSession& session) {
  if (session.dialect == Dialect::Redis) {
    return false;
  }
  if (session.objects.empty() && session.databases.empty() && session.triggers.empty() &&
      session.routines.empty() && session.events.empty()) {
    return session.dialect == Dialect::Sqlite || session.dialect == Dialect::Duckdb ||
           session.dialect == Dialect::Mysql || session.dialect == Dialect::Postgresql;
  }
  const bool multi_catalog = SessionHasMultipleCatalogs(session);
  std::vector<std::string> distinct_buckets;
  for (const DbObject& object : session.objects) {
    const std::string bucket = SessionObjectBucketName(session, object, multi_catalog);
    if (bucket.empty()) {
      continue;
    }
    if (std::ranges::find(distinct_buckets, bucket) == distinct_buckets.end()) {
      distinct_buckets.push_back(bucket);
      if (distinct_buckets.size() >= 2) {
        return true;
      }
    }
  }
  for (const std::string& extra_db : session.databases) {
    if (!extra_db.empty() && std::ranges::find(distinct_buckets, extra_db) == distinct_buckets.end()) {
      distinct_buckets.push_back(extra_db);
      if (distinct_buckets.size() >= 2) {
        return true;
      }
    }
  }
  if (distinct_buckets.size() <= 1) {
    return session.dialect == Dialect::Sqlite || session.dialect == Dialect::Duckdb ||
           session.dialect == Dialect::Mysql || session.dialect == Dialect::Postgresql ||
           ContainsIgnoreCaseAscii(session.driver_path, "mssql") ||
           ContainsIgnoreCaseAscii(session.uri, "mssql://");
  }
  return false;
}

std::vector<SidebarDatabaseGroup> GroupSessionDatabases(const DbSession& session) {
  const bool multi_catalog = SessionHasMultipleCatalogs(session);
  const DatabasePreset& preset = ResolvePresetForProfile(session.driver_path, session.uri);
  const std::string target_db = DecomposePresetUri(preset, session.uri).database;
  std::string target_lower = target_db;
  for (char& ch : target_lower) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  if (target_lower.empty() &&
      (session.dialect == Dialect::Sqlite || session.dialect == Dialect::Duckdb)) {
    target_lower = "main";
  }

  struct RawBucket {
    std::string name;
    bool is_system = false;
    bool is_target = false;
    std::vector<const DbObject*> items;
    std::vector<const RoutineMeta*> procedures;
    std::vector<const RoutineMeta*> functions;
    std::vector<const TriggerMeta*> triggers;
    std::vector<const EventMeta*> events;
  };
  std::vector<RawBucket> raw_buckets;
  const auto ensure_bucket = [&](const std::string& bucket) -> RawBucket* {
    auto it = std::ranges::find_if(raw_buckets,
                                   [&bucket](const RawBucket& b) { return b.name == bucket; });
    if (it != raw_buckets.end()) {
      return &(*it);
    }
    std::string lower_name = bucket;
    for (char& ch : lower_name) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    raw_buckets.push_back(RawBucket{
        .name = bucket,
        .is_system = IsSystemDatabaseOrSchema(bucket),
        .is_target = !target_lower.empty() && lower_name == target_lower,
    });
    return &raw_buckets.back();
  };

  for (const DbObject& object : session.objects) {
    std::string bucket = SessionObjectBucketName(session, object, multi_catalog);
    if (bucket.empty()) {
      bucket = !target_db.empty() ? target_db : std::string("default");
    }
    ensure_bucket(bucket)->items.push_back(&object);
  }
  for (const std::string& extra_db : session.databases) {
    if (!extra_db.empty()) {
      (void)ensure_bucket(extra_db);
    }
  }
  if (raw_buckets.empty()) {
    const std::string fallback_name =
        !target_db.empty()
            ? target_db
            : ((session.dialect == Dialect::Sqlite || session.dialect == Dialect::Duckdb)
                   ? "main"
                   : "default");
    (void)ensure_bucket(fallback_name);
  }

  const auto bucket_name_for_meta = [&](std::string_view db_schema,
                                        std::string_view catalog) -> std::string {
    if (!db_schema.empty()) {
      return std::string(db_schema);
    }
    if (!catalog.empty() && !ContainsIgnoreCaseAscii(catalog, "def")) {
      return std::string(catalog);
    }
    if (!target_db.empty()) {
      return target_db;
    }
    return (session.dialect == Dialect::Sqlite || session.dialect == Dialect::Duckdb)
               ? "main"
               : raw_buckets.front().name;
  };

  for (const RoutineMeta& rt : session.routines) {
    RawBucket* bucket = ensure_bucket(bucket_name_for_meta(rt.db_schema, rt.catalog));
    if (rt.is_function) {
      bucket->functions.push_back(&rt);
    } else {
      bucket->procedures.push_back(&rt);
    }
  }
  for (const TriggerMeta& trg : session.triggers) {
    ensure_bucket(bucket_name_for_meta(trg.db_schema, trg.catalog))->triggers.push_back(&trg);
  }
  for (const EventMeta& ev : session.events) {
    ensure_bucket(bucket_name_for_meta(ev.db_schema, ev.catalog))->events.push_back(&ev);
  }

  if (!session.show_system_databases) {
    std::erase_if(raw_buckets, [](const RawBucket& b) { return b.is_system; });
  }

  std::ranges::sort(raw_buckets, [](const RawBucket& a, const RawBucket& b) {
    if (a.is_target != b.is_target) {
      return a.is_target;
    }
    if (a.is_system != b.is_system) {
      return !a.is_system;
    }
    return a.name < b.name;
  });

  std::size_t non_system_count = 0;
  bool any_target = false;
  for (const RawBucket& b : raw_buckets) {
    if (!b.is_system) {
      ++non_system_count;
    }
    if (b.is_target && !b.is_system) {
      any_target = true;
    }
  }

  std::vector<SidebarDatabaseGroup> result;
  result.reserve(raw_buckets.size());
  bool opened_first_non_system = false;
  bool opened_first_fallback = false;
  for (RawBucket& b : raw_buckets) {
    bool open = false;
    if (!b.is_system) {
      if (non_system_count <= 4 || b.is_target || (!any_target && !opened_first_non_system)) {
        open = true;
        opened_first_non_system = true;
      }
    } else if (non_system_count == 0 && !opened_first_fallback) {
      open = true;
      opened_first_fallback = true;
    }
    const std::size_t total_count = b.items.size() + b.procedures.size() + b.functions.size() +
                                    b.triggers.size() + b.events.size();
    result.push_back(SidebarDatabaseGroup{
        .name = std::move(b.name),
        .is_system = b.is_system,
        .is_target = b.is_target,
        .default_open = open,
        .object_count = total_count,
        .groups = GroupObjectItems(session.dialect, b.items, true),
        .procedures = std::move(b.procedures),
        .functions = std::move(b.functions),
        .triggers = std::move(b.triggers),
        .events = std::move(b.events),
    });
  }
  return result;
}

bool SessionHasMultipleSchemas(const DbSession& session) {
  std::string first_schema;
  for (const DbObject& object : session.objects) {
    const std::string db_name = ObjectDatabaseName(session.dialect, object);
    if (db_name.empty()) {
      continue;
    }
    if (first_schema.empty()) {
      first_schema = db_name;
    } else if (db_name != first_schema) {
      return true;
    }
  }
  return false;
}

} // namespace dbpp
