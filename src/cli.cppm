export module dbpp.cli;

import std;
import dbpp.types;
import dbpp.adbc;
import dbpp.redis;
import dbpp.driver_installer;
import dbpp.db;

export namespace dbpp {

inline constexpr int kCliExitOk = 0;
inline constexpr int kCliExitError = 1;
inline constexpr int kCliExitUsage = 2;

[[nodiscard]] bool ShouldDispatchCli(const std::vector<std::string>& args);
int RunCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
           std::istream& in);
std::optional<int> TryRunCli(int argc, char** argv);

} // namespace dbpp

namespace dbpp::detail {

inline constexpr std::int64_t kCliDefaultRowsLimit = 50;
inline constexpr std::int64_t kCliMaxRowsLimit = 100000;
inline constexpr std::size_t kCliDefaultQueryMaxRows = 500;
inline constexpr std::size_t kCliMaxQueryMaxRows = 100000;

enum class CliOutputFormat {
  Table,
  Tsv,
  Csv,
  Json,
};

enum class CliObjectTypeFilter {
  All,
  Table,
  View,
};

struct CliConnOptions {
  std::string profile_name;
  std::string profiles_path;
  std::string sqlite_path;
  std::string redis_uri;
  std::string driver;
  std::string uri;
  std::string user;
  std::string password;
  std::string password_env;
  bool has_sqlite = false;
  bool has_redis = false;
  bool has_user = false;
  bool has_password = false;
  bool has_password_env = false;
};

struct CliJsonWriter {
  std::string buf;

  void Raw(std::string_view text);
  void BeginObject();
  void EndObject();
  void BeginArray();
  void EndArray();
  void Comma(bool& first);
  void Null();
  void Bool(bool value);
  void Int64(std::int64_t value);
  void Size(std::size_t value);
  void String(std::string_view text);
  void Key(std::string_view key, bool& first);
  void KeyString(std::string_view key, std::string_view value, bool& first);
  void KeyBool(std::string_view key, bool value, bool& first);
  void KeyInt64(std::string_view key, std::int64_t value, bool& first);
  void KeySize(std::string_view key, std::size_t value, bool& first);
};

std::string CliLowerAscii(std::string_view text);
std::string_view CliTrimAscii(std::string_view text);
bool CliContainsIgnoreCase(std::string_view haystack, std::string_view needle);
bool CliEqualsIgnoreCase(std::string_view left, std::string_view right);
std::optional<std::int64_t> CliParseStrictInt64(std::string_view text);
void CliSetEnvVar(const char* name, const char* value);
bool CliIsKnownCommand(std::string_view cmd);
bool CliContainsHelpFlag(const std::vector<std::string>& args);
bool CliDetectJsonRequestedEarly(const std::vector<std::string>& args);
std::string_view CliDialectName(Dialect dialect);
int CliEmitError(std::ostream& err, bool json_mode, std::string_view message, int code);
bool CliConsumeFlagValue(const std::vector<std::string>& args, std::size_t& index,
                         std::optional<std::string_view> inline_val, std::string_view flag_name,
                         std::string& value_out, std::string& error_out);
void CliSplitFlag(std::string_view arg, std::string_view& flag,
                  std::optional<std::string_view>& inline_val);
bool CliTryConsumeCommonFormatFlag(const std::vector<std::string>& args, std::size_t& index,
                                   CliOutputFormat& format, bool& help_requested,
                                   std::string& error_out, bool& handled);
bool CliTryConsumeConnFlag(const std::vector<std::string>& args, std::size_t& index,
                           CliConnOptions& conn, std::string& error_out, bool& handled);
std::string CliDefaultProfilesPath();
int CliResolveConnectionRequest(const CliConnOptions& conn, ConnectionRequest& request_out,
                                std::string& error_out);
void CliWriteTabularText(std::ostream& out, CliOutputFormat format,
                         const std::vector<std::string>& columns,
                         const std::vector<GridRow>& rows);
void CliAppendJsonRowsColumns(CliJsonWriter& w, const std::vector<std::string>& columns,
                              const std::vector<GridRow>& rows, bool& first_key);
GridRow CliMakeTextRow(std::initializer_list<std::string_view> cells);
bool CliReadSqlInput(std::ostream& err, bool json_mode, std::istream& in,
                     const std::optional<std::string>& sql_arg,
                     const std::optional<std::string>& file_arg, std::string& sql_out,
                     int& exit_code_out);
std::vector<std::string> CliSplitSqlStatements(std::string_view script, Dialect dialect);
std::optional<DbObject> CliFindObjectByName(const std::vector<DbObject>& objects,
                                            std::string_view target,
                                            std::string_view schema_filter,
                                            std::string& error_out);

void CliPrintTopLevelHelp(std::ostream& out);
bool CliPrintCommandHelp(std::string_view cmd, std::ostream& out);
int CliHandleVersion(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                     bool json_mode);
int CliHandleDoctor(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                    bool json_mode);
int CliHandleDriver(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                    bool json_mode);
int CliHandleProfile(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                     bool json_mode);
int CliHandleTables(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                    bool json_mode);
int CliHandleSchema(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                    bool json_mode);
int CliHandleRows(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                  bool json_mode);
int CliHandleQueryOrExecOrSql(const std::string& mode, const std::vector<std::string>& args,
                              std::ostream& out, std::ostream& err, std::istream& in,
                              bool json_mode);
int CliHandleInfo(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                  bool json_mode);
int CliHandleStats(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                   bool json_mode);
int CliHandleImport(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                    bool json_mode);
int CliHandleExport(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                    std::istream& in, bool json_mode);
int CliHandleCopy(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                  bool json_mode);

} // namespace dbpp::detail
