#pragma once

#include <huxerui/datatable.h>
#include <huxerui/huxerui.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dbpp {

using NotifyFn = std::function<void(std::string)>;

constexpr std::int64_t kPageSize = 100;
constexpr std::size_t kMaxQueryRows = 5000;

/// SQL dialect used only for identifier quoting and pagination syntax.
enum class Dialect {
  Sqlite,
  Postgresql,
  Mysql,
  Duckdb,
  Generic,
};

struct AdbcSession;

struct ColumnMeta {
  std::string name;
  std::string type;
  bool nullable = true;
  bool primary_key = false;
  int primary_key_order = 0;

  [[nodiscard]] bool NotNull() const { return !nullable; }
};

struct DbObject {
  std::string name;
  std::string catalog;
  std::string db_schema;
  std::string table_type;
  bool is_view = false;
  std::vector<std::string> primary_key;
  std::vector<ColumnMeta> columns;
};

struct ConnectionRequest {
  std::string driver_path;
  std::string uri;
  std::string username;
  std::string password;
  std::string title;
};

struct DbSession {
  std::uint64_t id = 0;
  std::string name;
  std::string uri;
  std::string driver_path;
  Dialect dialect = Dialect::Generic;
  bool sqlite = false;
  bool read_only = false;
  std::shared_ptr<AdbcSession> session;
  std::vector<DbObject> objects;
};

struct AppModel {
  std::vector<DbSession> sessions;
  std::uint64_t active_session = 0;
  std::string active_object;
  bool active_is_view = false;
  std::string status;
  std::string error;
  std::uint64_t next_session_id = 1;
  int revision = 0;

  [[nodiscard]] const DbSession* ActiveSession() const {
    for (const auto& session : sessions) {
      if (session.id == active_session) {
        return &session;
      }
    }
    return nullptr;
  }
};

struct CellValue {
  std::string text;
  bool is_null = false;
};

struct GridRow {
  std::vector<CellValue> values;
  std::vector<CellValue> key_values;
  std::optional<std::int64_t> rowid;
};

struct GridSnapshot {
  std::vector<GridRow> rows;
};

struct TableModel {
  std::uint64_t session_id = 0;
  std::string object;
  std::string qualified_name;
  bool is_view = false;
  bool editable = false;
  bool read_only = false;
  bool uses_rowid = false;
  std::vector<ColumnMeta> columns;
  std::vector<std::size_t> key_columns;
  std::shared_ptr<const GridSnapshot> snapshot;
  std::int64_t total = 0;
  std::int64_t page = 0;
  std::int64_t page_size = kPageSize;
  std::string filter;
  std::optional<huxerui::DataTableSort> sort;
  bool loading = false;
  std::string error;
  std::string message;
};

struct QueryResult {
  std::vector<std::string> columns;
  std::shared_ptr<const GridSnapshot> snapshot;
  bool truncated = false;
};

struct SqlModel {
  huxerui::TextEditingValue editor;
  std::shared_ptr<const QueryResult> result;
  bool running = false;
  std::string status;
  std::string error;
};

struct AddRowContext {
  std::uint64_t session_id = 0;
  std::string object;
  std::string qualified_name;
  std::vector<ColumnMeta> columns;
  std::shared_ptr<AdbcSession> session;
  huxerui::State<AppModel> app;
  huxerui::State<TableModel> table;
  huxerui::State<std::optional<huxerui::DataTableCellId>> selected;
  huxerui::State<huxerui::TextEditingValue> filter_input;
  huxerui::TaskScope tasks;
  bool dark = false;
  NotifyFn notify;
};

struct ConnectContext {
  huxerui::State<AppModel> app;
  huxerui::State<TableModel> table;
  huxerui::State<SqlModel> sql;
  huxerui::State<std::optional<huxerui::DataTableCellId>> selected;
  huxerui::State<huxerui::TextEditingValue> filter_input;
  huxerui::TaskScope tasks;
  std::string default_driver;
  std::string default_uri;
  bool dark = false;
  NotifyFn notify;
};

// ---- value and identifier helpers -----------------------------------------
[[nodiscard]] std::string QuoteIdentifier(Dialect dialect, std::string_view identifier);
[[nodiscard]] std::string SqlLiteral(Dialect dialect, std::string_view text,
                                     std::string_view declared_type, bool is_null);
[[nodiscard]] std::string QualifiedName(Dialect dialect, const DbObject& object);
[[nodiscard]] bool IsSqlReadStatement(std::string_view sql);

// ---- ADBC operations (implemented in adbc_session.cpp) --------------------
struct AdbcOpenResult {
  std::shared_ptr<AdbcSession> session;
  std::string error;
};

struct AdbcObjectsResult {
  std::vector<DbObject> objects;
  std::string error;
};

struct AdbcSchemaResult {
  std::vector<ColumnMeta> columns;
  std::string error;
};

struct AdbcQueryResult {
  std::vector<std::string> columns;
  std::vector<GridRow> rows;
  std::int64_t affected = -1;
  bool truncated = false;
  std::string error;
};

AdbcOpenResult OpenAdbcBlocking(const ConnectionRequest& request);
AdbcObjectsResult LoadObjectsBlocking(AdbcSession& session);
AdbcSchemaResult LoadSchemaBlocking(AdbcSession& session, const DbObject& object);
AdbcQueryResult ExecuteBlocking(AdbcSession& session, std::string sql, bool want_rows,
                                std::size_t max_rows);

// Async wrappers. Each one serializes its work on the owning session's
// WorkerSequence, so ADBC connection handles are never used concurrently.
huxerui::Task<AdbcQueryResult> ExecuteQueryAsync(std::shared_ptr<AdbcSession> session,
                                                 std::string sql, bool want_rows,
                                                 std::size_t max_rows);
huxerui::Task<AdbcQueryResult> UpdateCellAsync(std::shared_ptr<AdbcSession> session,
                                               TableModel model, std::size_t row_index,
                                               std::size_t column_index, std::string text);
huxerui::Task<AdbcQueryResult> DeleteRowAsync(std::shared_ptr<AdbcSession> session,
                                              TableModel model, std::size_t row_index);
huxerui::Task<AdbcQueryResult> InsertRowAsync(std::shared_ptr<AdbcSession> session,
                                              TableModel model,
                                              std::vector<std::string> values);

// ---- UI coordination -------------------------------------------------------
void RequestLoadTable(const huxerui::State<AppModel>& app,
                      const huxerui::State<TableModel>& table,
                      const huxerui::State<std::optional<huxerui::DataTableCellId>>& selected,
                      const huxerui::TaskScope& tasks, std::int64_t page,
                      std::optional<huxerui::DataTableSort> sort, std::string filter,
                      bool keep_selection, NotifyFn notify);

void RequestSaveCell(const huxerui::State<AppModel>& app,
                     const huxerui::State<TableModel>& table,
                     const huxerui::State<std::optional<huxerui::DataTableCellId>>& selected,
                     const huxerui::TaskScope& tasks,
                     const huxerui::DataTableEditRequest& edit, NotifyFn notify);

void RequestDeleteSelected(
    const huxerui::State<AppModel>& app, const huxerui::State<TableModel>& table,
    const huxerui::State<std::optional<huxerui::DataTableCellId>>& selected,
    const huxerui::TaskScope& tasks, NotifyFn notify);

void SelectObject(const huxerui::State<AppModel>& app,
                  const huxerui::State<TableModel>& table,
                  const huxerui::State<std::optional<huxerui::DataTableCellId>>& selected,
                  const huxerui::State<huxerui::TextEditingValue>& filter_input,
                  const huxerui::TaskScope& tasks, std::uint64_t session_id,
                  std::string object, bool is_view, NotifyFn notify);

void OpenConnection(const huxerui::State<AppModel>& app,
                    const huxerui::State<TableModel>& table,
                    const huxerui::State<std::optional<huxerui::DataTableCellId>>& selected,
                    const huxerui::State<huxerui::TextEditingValue>& filter_input,
                    const huxerui::TaskScope& tasks, ConnectionRequest request,
                    NotifyFn notify);

void CloseSession(const huxerui::State<AppModel>& app,
                  const huxerui::State<TableModel>& table,
                  const huxerui::State<std::optional<huxerui::DataTableCellId>>& selected,
                  const huxerui::State<huxerui::TextEditingValue>& filter_input,
                  const huxerui::TaskScope& tasks, std::uint64_t session_id,
                  NotifyFn notify);

} // namespace dbpp
