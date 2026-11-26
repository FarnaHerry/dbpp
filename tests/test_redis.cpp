#include <cstdlib>
#include <cstring>
#include "test_redis_helpers.h"

import std;
import dbpp.types;
import dbpp.redis;
import dbpp.adbc;
import dbpp.db;
import dbpp.cli;

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    ++g_failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

struct ScopedTempDir {
  fs::path path;
  ScopedTempDir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() / ("dbpp_test_redis_" + std::to_string(stamp));
    fs::create_directories(path);
  }
  ~ScopedTempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void SetEnv(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value != nullptr ? value : "");
#else
  if (value != nullptr && *value != '\0') {
    setenv(name, value, 1);
  } else {
    unsetenv(name);
  }
#endif
}

struct ScopedEnvGuard {
  std::string name;
  bool had = false;
  std::string prev;
  ScopedEnvGuard(std::string env_name, const std::string& val) : name(std::move(env_name)) {
    if (const char* cur = std::getenv(name.c_str()); cur != nullptr) {
      had = true;
      prev = cur;
    }
    SetEnv(name.c_str(), val.c_str());
  }
  ~ScopedEnvGuard() { SetEnv(name.c_str(), had ? prev.c_str() : nullptr); }
};

struct CliRunResult {
  int code = -1;
  std::string out;
  std::string err;
};

CliRunResult InvokeCli(const std::vector<std::string>& args) {
  std::ostringstream out;
  std::ostringstream err;
  std::istringstream in;
  const int code = dbpp::RunCli(args, out, err, in);
  return CliRunResult{.code = code, .out = out.str(), .err = err.str()};
}


void TestUriAndParsingHelpers() {
  const auto p1 = dbpp::ParseRedisUri("redis://127.0.0.1:6380/4");
  Expect(p1.error.empty() && p1.endpoint.host == "127.0.0.1" && p1.endpoint.port == 6380 &&
             p1.endpoint.db == 4,
         "ParseRedisUri basic host/port/db mismatch");

  const auto p2 = dbpp::ParseRedisUri("redis://alice:p%40ss@[::1]:6379/?db=2");
  Expect(p2.error.empty() && p2.endpoint.host == "::1" && p2.endpoint.port == 6379 &&
             p2.endpoint.db == 2 && p2.endpoint.username == "alice" &&
             p2.endpoint.password == "p@ss",
         "ParseRedisUri IPv6 + credentials + query db mismatch");

  const auto p_tls = dbpp::ParseRedisUri("rediss://redis.example.com:6380/0");
  Expect(!p_tls.error.empty() && p_tls.endpoint.tls,
         "ParseRedisUri must reject rediss:// with clear error");

  Expect(dbpp::FormatRedisDisplayName("redis://user:secret@db.local:6379/3") ==
             "db.local:6379/3",
         "FormatRedisDisplayName must strip credentials");

  Expect(dbpp::EncodeRespCommand({"SET", "k", "v"}) ==
             "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n",
         "EncodeRespCommand mismatch");

  std::size_t consumed = 0;
  dbpp::RedisValue val;
  std::string err;
  Expect(!dbpp::TryParseRespValue("$5\r\nhel", consumed, val, err) && err.empty(),
         "Incomplete RESP frame should pause cleanly without error");
  Expect(dbpp::TryParseRespValue("%1\r\n+a\r\n:42\r\n", consumed, val, err) &&
             err.empty() && consumed == 13 && val.is_map && val.elements.size() == 2 &&
             val.elements[0].str == "a" && val.elements[1].integer == 42,
         "TryParseRespValue RESP3 map decode failed");

  const auto cmd = dbpp::ParseRedisCommandLine("SET \"my key\" 'a\\'b\\n' 123");
  Expect(cmd.error.empty() && cmd.args.size() == 4 && cmd.args[0] == "SET" &&
             cmd.args[1] == "my key" && cmd.args[2] == "a'b\\n" && cmd.args[3] == "123",
         "ParseRedisCommandLine quoting mismatch");

  const auto script = dbpp::SplitRedisScript("# comment\nSET a \"line1\nline2\"\n\nGET a\n");
  Expect(script.size() == 2 && script[1] == "GET a", "SplitRedisScript mismatch");

  const auto p_db_prefix = dbpp::ParseRedisUri("redis://127.0.0.1:6379/db2");
  Expect(p_db_prefix.error.empty() && p_db_prefix.endpoint.db == 2,
         "ParseRedisUri must accept /db2");

  const auto p_query_db_prefix = dbpp::ParseRedisUri("redis://127.0.0.1:6379/?db=db3");
  Expect(p_query_db_prefix.error.empty() && p_query_db_prefix.endpoint.db == 3,
         "ParseRedisUri must accept ?db=db3");

  const dbpp::DatabasePreset* redis_preset = nullptr;
  for (const auto& preset : dbpp::SupportedDatabasePresets()) {
    if (preset.id == "redis") {
      redis_preset = &preset;
      break;
    }
  }
  Expect(redis_preset != nullptr, "redis preset must exist");
  if (redis_preset != nullptr) {
    Expect(dbpp::ComposePresetUri(*redis_preset, "redis://10.0.0.1:6379/2", "6379", "0") ==
               "redis://10.0.0.1:6379/2",
           "ComposePresetUri must keep full pasted URI");
    Expect(dbpp::ComposePresetUri(*redis_preset, "127.0.0.1:6380", "6379", "0") ==
               "redis://127.0.0.1:6380/0",
           "ComposePresetUri must not bracket host:port as IPv6");
    Expect(dbpp::ComposePresetUri(*redis_preset, "::1", "6379", "0") == "redis://[::1]:6379/0",
           "ComposePresetUri must bracket raw IPv6 host");
  }

  {
    const auto s1 = dbpp::SanitizeUriCredentials("redis://:secret@127.0.0.1:6379/0");
    Expect(s1.uri == "redis://127.0.0.1:6379/0" && s1.username.empty() &&
               s1.password == "secret" && s1.stripped_password,
           "SanitizeUriCredentials redis://:secret@ mismatch");

    const auto s2 = dbpp::SanitizeUriCredentials("redis://alice:secret@127.0.0.1:6379/1");
    Expect(s2.uri == "redis://127.0.0.1:6379/1" && s2.username == "alice" &&
               s2.password == "secret" && s2.stripped_password,
           "SanitizeUriCredentials redis://alice:secret@ mismatch");

    const auto s3 = dbpp::SanitizeUriCredentials("redis://legacy_pass@127.0.0.1:6379/0");
    Expect(s3.uri == "redis://127.0.0.1:6379/0" && s3.password == "legacy_pass" &&
               s3.stripped_password,
           "SanitizeUriCredentials legacy single-token pass mismatch");

    const auto s4 = dbpp::SanitizeUriCredentials("postgresql://bob:pw@127.0.0.1:5432/app");
    Expect(s4.uri == "postgresql://127.0.0.1:5432/app" && s4.username == "bob" &&
               s4.password == "pw" && s4.stripped_password,
           "SanitizeUriCredentials postgresql:// mismatch");

    const auto s5 = dbpp::SanitizeUriCredentials("/tmp/a@b.sqlite");
    Expect(s5.uri == "/tmp/a@b.sqlite" && !s5.stripped_password,
           "SanitizeUriCredentials must preserve local file path");
  }

  Expect(dbpp::IsRedisReadCommand("INFO server"), "INFO must be read-only");
  Expect(dbpp::IsRedisReadCommand("CONFIG GET databases"), "CONFIG GET must be read-only");
  Expect(!dbpp::IsRedisReadCommand("CONFIG SET save \"\""), "CONFIG SET must not be read-only");
  Expect(!dbpp::IsRedisReadCommand("SET foo bar"), "SET must not be read-only");
  Expect(!dbpp::IsRedisReadCommand("PING\nSET x 1"),
         "Mixed read+write script must not be read-only");
  Expect(dbpp::IsRedisReadCommand("# comment\nPING\nDBSIZE"),
         "Comment + read-only lines must be read-only");

  const std::string preview = dbpp::SingleLineCellPreview("前缀\r\n\xD3\xC9\xD3\xDA");
  Expect(dbpp::IsValidUtf8(preview) && preview.starts_with("前缀 "),
         "SingleLineCellPreview must return valid UTF-8 on invalid byte sequences");
}

void TestRedisSessionCrudAndCli(const fs::path& dir) {
  dbpp_test::MockRedisServer server("secret123", "alice");
  const std::string base_uri = "redis://127.0.0.1:" + std::to_string(server.Port()) + "/1";

  // Wrong password must fail cleanly.
  dbpp::ConnectionRequest bad_req{
      .driver_path = "redis",
      .uri = base_uri,
      .username = "alice",
      .password = "wrong",
      .title = "bad",
  };
  const dbpp::AdbcOpenResult bad_open = dbpp::OpenAdbcBlocking(bad_req);
  Expect(!bad_open.error.empty() && bad_open.session == nullptr,
         "Invalid Redis password must fail OpenAdbcBlocking");

  // Valid credentials open and select db1.
  dbpp::ConnectionRequest req{
      .driver_path = "redis",
      .uri = base_uri,
      .username = "alice",
      .password = "secret123",
      .title = "local-redis",
  };
  const dbpp::AdbcOpenResult opened = dbpp::OpenAdbcBlocking(req);
  Expect(opened.error.empty() && opened.session != nullptr &&
             opened.session->dialect == dbpp::Dialect::Redis &&
             opened.session->redis != nullptr && opened.session->redis->CurrentDb() == 1,
         "Valid OpenAdbcBlocking redis failed: " + opened.error);
  if (!opened.session || !opened.session->redis) {
    return;
  }

  // LoadObjectsBlocking reports db0 (5 keys) .. db15 and db1 (1 key).
  const dbpp::AdbcObjectsResult objs = dbpp::LoadObjectsBlocking(*opened.session);
  Expect(objs.error.empty() && objs.objects.size() == 16 &&
             objs.objects[0].name == "db0" && objs.objects[0].item_count == 5 &&
             objs.objects[1].name == "db1" && objs.objects[1].item_count == 1,
         "LoadObjectsBlocking db counts mismatch: " + objs.error);

  const dbpp::AdbcSchemaResult schema = dbpp::LoadSchemaBlocking(*opened.session, objs.objects[0]);
  Expect(schema.error.empty() && schema.columns.size() == 5 &&
             schema.columns[0].name == "key" && schema.columns[0].primary_key,
         "LoadSchemaBlocking redis mismatch");

  // Browse db0 with FILTER + SORT.
  dbpp::TableModel table = dbpp::LoadTableBlocking(
      *opened.session, 1, objs.objects[0], 0, 100, std::nullopt, {});
  Expect(table.error.empty() && table.total == 5 && table.snapshot &&
             table.snapshot->rows.size() == 5,
         "LoadTableBlocking db0 failed: " + table.error);

  dbpp::TableModel filtered = dbpp::LoadTableBlocking(
      *opened.session, 1, objs.objects[0], 0, 100, std::nullopt, "user:*");
  Expect(filtered.error.empty() && filtered.total == 2 && filtered.snapshot &&
             filtered.snapshot->rows.size() == 2,
         "LoadTableBlocking MATCH user:* mismatch");

  // Edit string value preserves TTL 120.
  std::size_t greeting_idx = static_cast<std::size_t>(-1);
  for (std::size_t i = 0; i < table.snapshot->rows.size(); ++i) {
    if (table.snapshot->rows[i].values[0].text == "greeting") {
      greeting_idx = i;
      break;
    }
  }
  Expect(greeting_idx != static_cast<std::size_t>(-1), "greeting key must exist");
  if (greeting_idx != static_cast<std::size_t>(-1)) {
    const dbpp::AdbcQueryResult upd = dbpp::UpdateRedisCellBlocking(
        *opened.session->redis, table, greeting_idx, 4, "updated greeting");
    Expect(upd.error.empty(), "UpdateRedisCellBlocking string failed: " + upd.error);

    dbpp::TableModel after_upd = dbpp::LoadTableBlocking(
        *opened.session, 1, objs.objects[0], 0, 100, std::nullopt, "greeting");
    Expect(after_upd.snapshot && after_upd.snapshot->rows.size() == 1 &&
               after_upd.snapshot->rows[0].values[2].text == "120" &&
               after_upd.snapshot->rows[0].values[4].text == "updated greeting",
           "String edit must preserve TTL via KEEPTTL");

    // Change TTL then persist (-1).
    const dbpp::AdbcQueryResult set_ttl = dbpp::UpdateRedisCellBlocking(
        *opened.session->redis, after_upd, 0, 2, "450");
    Expect(set_ttl.error.empty(), "UpdateRedisCellBlocking TTL failed: " + set_ttl.error);
    const dbpp::AdbcQueryResult persist_ttl = dbpp::UpdateRedisCellBlocking(
        *opened.session->redis, after_upd, 0, 2, "-1");
    Expect(persist_ttl.error.empty(), "UpdateRedisCellBlocking PERSIST failed: " + persist_ttl.error);
  }

  // Insert batch of 2 new keys into db0.
  dbpp::PendingInsertRow new_str{.draft_id = 1, .values = {"k:new", "string", "300", "", "val1"}};
  dbpp::PendingInsertRow new_hash{
      .draft_id = 2, .values = {"h:new", "hash", "", "", "{\"a\":1,\"b\":\"two\"}"}};
  const dbpp::BatchInsertResult ins =
      dbpp::InsertRowsBlocking(*opened.session, table, {new_str, new_hash});
  Expect(ins.inserted == 2 && ins.error.empty(), "InsertRowsBlocking redis failed: " + ins.error);

  // Command console queries.
  const dbpp::AdbcQueryResult info_res =
      dbpp::ExecuteBlocking(*opened.session, "INFO server", true, 50);
  Expect(info_res.error.empty() && info_res.columns.size() == 3 && !info_res.rows.empty(),
         "ExecuteBlocking INFO server mismatch: " + info_res.error);

  const dbpp::AdbcQueryResult zset_res =
      dbpp::ExecuteBlocking(*opened.session, "ZRANGE leaderboard 0 -1 WITHSCORES", true, 50);
  Expect(zset_res.error.empty() && zset_res.columns == std::vector<std::string>({"member", "score"}) &&
             zset_res.rows.size() == 2 && zset_res.rows[0].values[0].text == "alice",
         "ExecuteBlocking ZRANGE WITHSCORES mismatch: " + zset_res.error);

  // Delete k:new row.
  dbpp::TableModel k_new_tbl = dbpp::LoadTableBlocking(
      *opened.session, 1, objs.objects[0], 0, 100, std::nullopt, "k:new");
  Expect(k_new_tbl.snapshot && k_new_tbl.snapshot->rows.size() == 1,
         "Expected inserted k:new row");
  if (k_new_tbl.snapshot && !k_new_tbl.snapshot->rows.empty()) {
    const dbpp::AdbcQueryResult del =
        dbpp::DeleteRedisKeyBlocking(*opened.session->redis, k_new_tbl, 0);
    Expect(del.error.empty(), "DeleteRedisKeyBlocking failed: " + del.error);
  }

  // Headless CLI coverage (--redis, --read-only, profile + --password-env).
  const std::string auth_uri =
      "redis://alice:secret123@127.0.0.1:" + std::to_string(server.Port()) + "/0";
  const CliRunResult tables_cli = InvokeCli({"tables", "--redis", auth_uri, "--json"});
  Expect(tables_cli.code == 0 && tables_cli.out.find("\"dialect\":\"redis\"") != std::string::npos &&
             tables_cli.out.find("\"name\":\"db0\"") != std::string::npos,
         "CLI tables --redis --json failed: " + tables_cli.err + " / " + tables_cli.out);

  const CliRunResult rows_cli = InvokeCli(
      {"rows", "--redis", auth_uri, "db0", "--where", "user:*", "--order", "key:desc", "--json"});
  Expect(rows_cli.code == 0 && rows_cli.out.find("\"total\":2") != std::string::npos &&
             rows_cli.out.find("user:200") < rows_cli.out.find("user:100"),
         "CLI rows --redis failed: " + rows_cli.err + " / " + rows_cli.out);

  const CliRunResult ro_deny =
      InvokeCli({"query", "--redis", auth_uri, "--read-only", "SET x 1", "--json"});
  Expect(ro_deny.code == dbpp::kCliExitUsage,
         "CLI query --read-only must reject Redis writes with exit 2");

  const CliRunResult exec_script = InvokeCli(
      {"exec", "--redis", auth_uri, "--script", "SET cli:k1 hello\nSET cli:k2 world", "--json"});
  Expect(exec_script.code == 0 &&
             exec_script.out.find("\"statements_executed\":2") != std::string::npos,
         "CLI exec --script on Redis failed: " + exec_script.err);

  const fs::path profiles_path = dir / "dbpp.sqlite3";
  const CliRunResult save_prof = InvokeCli(
      {"profile", "save", "myredis", "--driver", "redis", "--uri", base_uri, "--user", "alice",
       "--profiles", profiles_path.string(), "--json"});
  Expect(save_prof.code == 0, "CLI profile save redis failed: " + save_prof.err);

  {
    ScopedEnvGuard pw_env("DBPP_TEST_REDIS_PW", "secret123");
    const CliRunResult use_prof = InvokeCli(
        {"query", "-p", "myredis", "--profiles", profiles_path.string(), "--password-env",
         "DBPP_TEST_REDIS_PW", "GET cache:token", "--json"});
    Expect(use_prof.code == 0 && use_prof.out.find("xyz") != std::string::npos,
           "CLI query -p myredis --password-env failed: " + use_prof.err + " / " + use_prof.out);
  }

  // Saved profile with persisted password connects directly without extra password flags.
  const CliRunResult save_auth_prof = InvokeCli(
      {"profile", "save", "myredis_saved", "--driver", "redis", "--uri", base_uri, "--user",
       "alice", "--password", "secret123", "--profiles", profiles_path.string(), "--json"});
  Expect(save_auth_prof.code == 0 &&
             save_auth_prof.out.find("secret123") == std::string::npos,
         "CLI profile save redis with --password failed: " + save_auth_prof.err);
  const CliRunResult use_saved_prof = InvokeCli(
      {"query", "-p", "myredis_saved", "--profiles", profiles_path.string(), "GET cache:token",
       "--json"});
  Expect(use_saved_prof.code == 0 && use_saved_prof.out.find("xyz") != std::string::npos,
         "CLI query -p myredis_saved using saved password failed: " + use_saved_prof.err + " / " +
             use_saved_prof.out);

  // Multi-line & comment console execution.
  const dbpp::AdbcQueryResult comment_ping =
      dbpp::ExecuteBlocking(*opened.session, "# comment\nPING", true, 50);
  Expect(comment_ping.error.empty() && !comment_ping.rows.empty() &&
             comment_ping.rows[0].values[0].text == "PONG",
         "ExecuteBlocking # comment + PING failed: " + comment_ping.error);

  const dbpp::AdbcQueryResult multi_exec =
      dbpp::ExecuteBlocking(*opened.session, "SET multi:k hello\nGET multi:k", true, 50);
  Expect(multi_exec.error.empty() && !multi_exec.rows.empty() &&
             multi_exec.rows[0].values[0].text == "hello",
         "ExecuteBlocking multi-line script failed: " + multi_exec.error);
}

void TestRedisAuditRegressions() {
  // 1. DetectDialect must prioritize redis:// scheme over default SQLite driver path.
  dbpp_test::MockRedisServer basic_server("secret123", "alice");
  const std::string auth_uri =
      "redis://alice:secret123@127.0.0.1:" + std::to_string(basic_server.Port()) + "/0";
  const dbpp::AdbcOpenResult via_sqlite_driver = dbpp::OpenAdbcBlocking(dbpp::ConnectionRequest{
      .driver_path = dbpp::DefaultSqliteDriverPath(),
      .uri = auth_uri,
      .username = {},
      .password = {},
      .title = "scheme-priority",
  });
  Expect(via_sqlite_driver.error.empty() && via_sqlite_driver.session != nullptr &&
             via_sqlite_driver.session->dialect == dbpp::Dialect::Redis,
         "redis:// URI with default SQLite driver path must still dispatch to Redis: " +
             via_sqlite_driver.error);

  // 2. Legacy Redis < 6.0 server rejects 3-arg AUTH; client must fall back to 2-arg AUTH.
  dbpp_test::MockRedisServer legacy_server("secret5", "");
  legacy_server.SetLegacyAuthOnly(true);
  const dbpp::AdbcOpenResult legacy_open = dbpp::OpenAdbcBlocking(dbpp::ConnectionRequest{
      .driver_path = "redis",
      .uri = "redis://127.0.0.1:" + std::to_string(legacy_server.Port()) + "/0",
      .username = "admin",
      .password = "secret5",
      .title = "legacy-auth",
  });
  Expect(legacy_open.error.empty() && legacy_open.session != nullptr &&
             legacy_open.session->dialect == dbpp::Dialect::Redis,
         "Legacy 2-arg AUTH fallback failed: " + legacy_open.error);

  // 3. Restricted cloud/proxy where INFO returns NOPERM and CONFIG closes socket.
  dbpp_test::MockRedisServer restricted_server;
  restricted_server.SetDisableInfo(true);
  restricted_server.SetDropOnConfig(true);
  const dbpp::AdbcOpenResult restricted_open = dbpp::OpenAdbcBlocking(dbpp::ConnectionRequest{
      .driver_path = "redis",
      .uri = "redis://127.0.0.1:" + std::to_string(restricted_server.Port()) + "/0",
      .username = {},
      .password = {},
      .title = "restricted-cloud",
  });
  Expect(restricted_open.error.empty() && restricted_open.session != nullptr,
         "Connect to restricted Redis failed: " + restricted_open.error);
  if (restricted_open.session) {
    const dbpp::AdbcObjectsResult objs = dbpp::LoadObjectsBlocking(*restricted_open.session);
    Expect(objs.error.empty() && objs.objects.size() == 16 &&
               objs.objects[0].name == "db0" && objs.objects[0].item_count == 5,
           "LoadObjectsBlocking must tolerate NOPERM INFO + CONFIG drop and fall back to DBSIZE: " +
               objs.error);
  }
}

void TestRedisDisconnectDetection() {
  auto server = std::make_unique<dbpp_test::MockRedisServer>();
  const std::string uri = "redis://127.0.0.1:" + std::to_string(server->Port()) + "/0";
  const dbpp::AdbcOpenResult opened = dbpp::OpenAdbcBlocking(dbpp::ConnectionRequest{
      .driver_path = "redis",
      .uri = uri,
      .username = {},
      .password = {},
      .title = "disconnect-check",
  });
  Expect(opened.error.empty() && opened.session != nullptr && opened.session->IsAlive(),
         "Initial Redis connect should succeed: " + opened.error);
  if (!opened.session) {
    return;
  }

  const dbpp::AdbcQueryResult ping_ok = dbpp::PingSessionBlocking(*opened.session);
  Expect(ping_ok.error.empty() && !ping_ok.connection_lost && opened.session->IsAlive(),
         "Ping before server shutdown should succeed: " + ping_ok.error);

  // Stop the mock Redis server completely so subsequent queries and reconnect fail.
  server.reset();

  const dbpp::AdbcQueryResult after_drop =
      dbpp::ExecuteBlocking(*opened.session, "GET any_key", true, 50);
  Expect(after_drop.connection_lost && !after_drop.error.empty(),
         "ExecuteBlocking after server shutdown must report connection_lost");
  Expect(!opened.session->IsAlive() &&
             (!opened.session->redis || !opened.session->redis->IsConnected()),
         "Session and RedisClient must report disconnected after server shutdown");

  const dbpp::AdbcQueryResult ping_drop = dbpp::PingSessionBlocking(*opened.session);
  Expect(ping_drop.connection_lost && !opened.session->IsAlive(),
         "PingSessionBlocking after server shutdown must report connection_lost");
}

} // namespace

int main() {
  ScopedTempDir temp;
  TestUriAndParsingHelpers();
  TestRedisSessionCrudAndCli(temp.path);
  TestRedisAuditRegressions();
  TestRedisDisconnectDetection();

  if (g_failures != 0) {
    std::cerr << g_failures << " Redis test assertion(s) failed\n";
    return 1;
  }
  std::cout << "test_redis passed\n";
  return 0;
}
