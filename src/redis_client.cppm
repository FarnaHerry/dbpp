module;

#include <huxerui/datatable.h>

export module dbpp.redis;

import std;
import dbpp.types;
import dbpp.i18n;

export namespace dbpp {

struct RedisEndpoint {
  std::string host = "127.0.0.1";
  std::uint16_t port = 6379;
  int db = 0;
  std::string username;
  std::string password;
  bool tls = false;
};

struct RedisParseUriResult {
  RedisEndpoint endpoint;
  std::string error;
};

enum class RedisValueKind {
  SimpleString,
  Error,
  Integer,
  BulkString,
  Null,
  Array,
  Bool,
  Double,
};

struct RedisValue {
  RedisValueKind kind = RedisValueKind::Null;
  std::string str;
  std::int64_t integer = 0;
  double number = 0.0;
  bool boolean = false;
  std::vector<RedisValue> elements;
  bool is_map = false;

  [[nodiscard]] bool IsError() const { return kind == RedisValueKind::Error; }
  [[nodiscard]] bool IsNull() const { return kind == RedisValueKind::Null; }
  [[nodiscard]] std::string ToDisplayString() const;
};

struct RedisCommandParseResult {
  std::vector<std::string> args;
  std::string error;
};

struct RedisPageResult {
  std::vector<ColumnMeta> columns;
  std::vector<GridRow> rows;
  std::int64_t total = 0;
  bool scan_capped = false;
  bool scan_iterations_capped = false;
  bool scan_cancelled = false;
  std::string error;
};

inline constexpr std::size_t kRedisMaxScanKeys = 10000;
inline constexpr std::size_t kRedisMaxScanIterations = 200;

struct RedisKeyListResult {
  std::vector<std::string> keys;
  std::int64_t elapsed_us = 0;
  bool scan_capped = false;
  bool scan_iterations_capped = false;
  bool scan_cancelled = false;
  std::string executed_sql;
  std::string error;
};

struct RedisTreeFlatRow {
  std::string id;
  std::string label;
  std::string full_key;
  int depth = 0;
  bool is_folder = false;
  bool expanded = false;
  std::size_t key_count = 0;
};

struct RedisKeyDetail {
  std::string key;
  std::string type;
  std::int64_t ttl = -2;
  std::int64_t size = 0;
  bool exists = false;
  bool truncated = false;
  std::string string_value;
  std::vector<std::pair<std::string, std::string>> entries;
  std::string error;
};

[[nodiscard]] RedisParseUriResult ParseRedisUri(std::string_view uri,
                                                std::string_view username_override = {},
                                                std::string_view password_override = {});
[[nodiscard]] std::string FormatRedisDisplayName(std::string_view uri);
[[nodiscard]] std::optional<int> ParseRedisDbName(std::string_view name);

[[nodiscard]] std::string EncodeRespCommand(const std::vector<std::string>& args);
[[nodiscard]] bool TryParseRespValue(std::string_view buffer, std::size_t& consumed,
                                     RedisValue& out, std::string& error);

[[nodiscard]] RedisCommandParseResult ParseRedisCommandLine(std::string_view line);
[[nodiscard]] std::vector<std::string> SplitRedisScript(std::string_view script);
[[nodiscard]] bool IsRedisReadCommand(std::string_view command_text);

class RedisClient {
public:
  RedisClient() = default;
  RedisClient(const RedisClient&) = delete;
  RedisClient& operator=(const RedisClient&) = delete;
  ~RedisClient();

  bool Connect(const RedisEndpoint& endpoint, std::string& error_out);
  void Disconnect();

  [[nodiscard]] bool IsConnected() const;
  [[nodiscard]] int CurrentDb() const { return current_db_; }
  [[nodiscard]] const RedisEndpoint& Endpoint() const { return endpoint_; }

  bool EnsureDb(int db, std::string& error_out);
  bool Command(const std::vector<std::string>& args, RedisValue& out, std::string& error_out);
  bool Pipeline(const std::vector<std::vector<std::string>>& commands,
                std::vector<RedisValue>& out, std::string& error_out);

private:
  bool OpenSocketAndHandshake(std::string& error_out);
  bool SendAll(std::string_view payload, std::string& error_out);
  bool RecvSome(std::string& error_out);
  bool ExecuteBatchRaw(const std::vector<std::vector<std::string>>& commands,
                       std::vector<RedisValue>& out, bool& received_any,
                       std::string& error_out);

  RedisEndpoint endpoint_;
  int current_db_ = 0;
#if defined(_WIN32)
  std::uintptr_t sock_ = static_cast<std::uintptr_t>(~0ULL);
#else
  int sock_ = -1;
#endif
  std::string recv_buf_;
};

[[nodiscard]] std::vector<ColumnMeta> RedisKeySchema();
AdbcObjectsResult LoadRedisObjectsBlocking(RedisClient& client);
RedisPageResult LoadRedisKeysPageBlocking(RedisClient& client, const DbObject& object,
                                          std::int64_t offset, std::int64_t limit,
                                          std::optional<huxerui::DataTableSort> sort,
                                          std::string filter,
                                          const std::atomic<bool>* cancel_flag = nullptr,
                                          std::string type_filter = {});
TableModel LoadRedisTableBlocking(RedisClient& client, std::uint64_t session_id,
                                  const DbObject& object, std::int64_t page,
                                  std::int64_t page_size,
                                  std::optional<huxerui::DataTableSort> sort, std::string filter,
                                  const std::atomic<bool>* cancel_flag = nullptr,
                                  std::string type_filter = {});
AdbcQueryResult ExecuteRedisCommandBlocking(RedisClient& client, std::string_view command_text,
                                            std::size_t max_rows);
AdbcQueryResult UpdateRedisCellBlocking(RedisClient& client, const TableModel& model,
                                        std::size_t row_index, std::size_t column_index,
                                        const std::string& text);
AdbcQueryResult DeleteRedisKeyBlocking(RedisClient& client, const TableModel& model,
                                       std::size_t row_index);
BatchInsertResult InsertRedisRowsBlocking(RedisClient& client, const TableModel& model,
                                          const std::vector<PendingInsertRow>& rows);

RedisKeyListResult ScanRedisKeyNamesBlocking(RedisClient& client, const DbObject& object,
                                             std::string filter = {},
                                             std::string type_filter = {},
                                             const std::atomic<bool>* cancel_flag = nullptr);
[[nodiscard]] std::vector<RedisTreeFlatRow> BuildRedisTreeFlatRows(
    const std::vector<std::string>& sorted_keys, std::string_view delimiter,
    const std::unordered_set<std::string>& expanded_folders,
    std::unordered_set<std::string>* all_folder_ids_out = nullptr);
RedisKeyDetail LoadRedisKeyDetailBlocking(RedisClient& client, const DbObject& object,
                                          std::string_view key, bool load_full_string = false);
AdbcQueryResult UpdateRedisKeyStringBlocking(RedisClient& client, const DbObject& object,
                                             std::string_view key, const std::string& new_value);
AdbcQueryResult UpdateRedisKeyTtlBlocking(RedisClient& client, const DbObject& object,
                                          std::string_view key, std::string_view ttl_text);
AdbcQueryResult RenameRedisKeyBlocking(RedisClient& client, const DbObject& object,
                                       std::string_view old_key, std::string_view new_key);
AdbcQueryResult DeleteRedisKeyByNameBlocking(RedisClient& client, const DbObject& object,
                                             std::string_view key);

} // namespace dbpp

namespace dbpp::detail {

inline constexpr std::size_t kRedisColKey = 0;
inline constexpr std::size_t kRedisColValue = 1;
inline constexpr std::size_t kRedisColType = 2;
inline constexpr std::size_t kRedisColTtl = 3;
inline constexpr std::size_t kRedisColSize = 4;

inline constexpr std::size_t kRedisMaxScanKeys = dbpp::kRedisMaxScanKeys;
inline constexpr std::size_t kRedisMaxScanIterations = dbpp::kRedisMaxScanIterations;
inline constexpr std::size_t kRedisMaxStringPreviewBytes = 65536;
inline constexpr std::size_t kRedisMaxCollectionPreview = 200;
inline constexpr std::size_t kRedisMaxListZsetPreview = 100;
inline constexpr std::size_t kRedisMaxStreamPreview = 20;

[[nodiscard]] bool IsRedisValuePreviewTruncated(std::string_view type,
                                                std::int64_t size) noexcept;
void RedisTrimIncompleteUtf8Tail(std::string& s);
[[nodiscard]] std::string RedisNormalizeTypeFilter(std::string_view raw);

std::string RedisLowerAscii(std::string_view text);
std::string RedisUpperAscii(std::string_view text);
std::string_view RedisTrimAscii(std::string_view text);
bool RedisEqualsIgnoreCase(std::string_view left, std::string_view right);
std::optional<std::int64_t> RedisParseStrictInt64(std::string_view text);
int RedisHexDigit(char c);
std::string RedisPercentDecode(std::string_view text);

std::string RedisFormatJsonArrayOfStrings(const std::vector<RedisValue>& elements,
                                          std::size_t cap);
std::string RedisFormatJsonHashPairs(const std::vector<RedisValue>& elements,
                                     std::size_t max_pairs);
std::string RedisFormatJsonZsetPairs(const std::vector<RedisValue>& elements,
                                     std::size_t max_pairs);
std::string RedisFormatJsonStreamEntries(const std::vector<RedisValue>& elements);
bool RedisBuildWriteCommandArgs(std::string_view type, std::string_view value_text,
                                std::vector<std::string>& args_out, std::string& error_out);

CellValue RedisTextCell(std::string text);
CellValue RedisNullCell();
CellValue RedisReplyToCell(const RedisValue& val);

} // namespace dbpp::detail
