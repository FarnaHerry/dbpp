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
             schema.columns[0].name == "key" && schema.columns[0].primary_key &&
             schema.columns[1].name == "value" && schema.columns[2].name == "type" &&
             schema.columns[3].name == "ttl" && schema.columns[4].name == "size",
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
        *opened.session->redis, table, greeting_idx, 1, "updated greeting");
    Expect(upd.error.empty(), "UpdateRedisCellBlocking string failed: " + upd.error);

    dbpp::TableModel after_upd = dbpp::LoadTableBlocking(
        *opened.session, 1, objs.objects[0], 0, 100, std::nullopt, "greeting");
    Expect(after_upd.snapshot && after_upd.snapshot->rows.size() == 1 &&
               after_upd.snapshot->rows[0].values[3].text == "120" &&
               after_upd.snapshot->rows[0].values[1].text == "updated greeting",
           "String edit must preserve TTL via KEEPTTL");

    // Change TTL then persist (-1).
    const dbpp::AdbcQueryResult set_ttl = dbpp::UpdateRedisCellBlocking(
        *opened.session->redis, after_upd, 0, 3, "450");
    Expect(set_ttl.error.empty(), "UpdateRedisCellBlocking TTL failed: " + set_ttl.error);
    const dbpp::AdbcQueryResult persist_ttl = dbpp::UpdateRedisCellBlocking(
        *opened.session->redis, after_upd, 0, 3, "-1");
    Expect(persist_ttl.error.empty(), "UpdateRedisCellBlocking PERSIST failed: " + persist_ttl.error);
  }

  // Insert batch of 2 new keys into db0 (column order: key, value, type, ttl, size).
  dbpp::PendingInsertRow new_str{.draft_id = 1, .values = {"k:new", "val1", "string", "300", ""}};
  dbpp::PendingInsertRow new_hash{
      .draft_id = 2, .values = {"h:new", "{\"a\":1,\"b\":\"two\"}", "hash", "", ""}};
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

void TestRedisBigKeyAndScanCapOptimizations() {
  dbpp_test::MockRedisServer server;
  const std::string db1_uri = "redis://127.0.0.1:" + std::to_string(server.Port()) + "/1";
  const dbpp::AdbcOpenResult opened = dbpp::OpenAdbcBlocking(dbpp::ConnectionRequest{
      .driver_path = "redis",
      .uri = db1_uri,
      .username = {},
      .password = {},
      .title = "bigkey-scan",
  });
  Expect(opened.error.empty() && opened.session != nullptr && opened.session->redis != nullptr,
         "Connect to mock db1 failed: " + opened.error);
  if (!opened.session || !opened.session->redis) {
    return;
  }
  dbpp::RedisClient& client = *opened.session->redis;

  // Start db1 cleanly without the seeded cache:token key.
  {
    dbpp::RedisValue reply;
    std::string err;
    Expect(client.Command({"DEL", "cache:token"}, reply, err) && err.empty(),
           "DEL cache:token failed: " + err);
  }

  // Construct a large UTF-8 string (~80 KB) with '中' straddling the 65536-byte boundary.
  constexpr std::size_t kTestMaxStringPreviewBytes = 65536;
  constexpr std::size_t kTestMaxScanIterations = 200;
  const std::string utf8_mid = "\xE4\xB8\xAD";
  const std::string big_str =
      std::string(kTestMaxStringPreviewBytes - 1, 'a') + utf8_mid + std::string(14460, 'b');
  const std::int64_t big_str_len = static_cast<std::int64_t>(big_str.size());

  auto exec_ok = [&](const std::vector<std::string>& args, std::string_view label) {
    dbpp::RedisValue reply;
    std::string err;
    Expect(client.Command(args, reply, err) && err.empty() && !reply.IsError(),
           std::string(label) + " failed: " + err + " / " + reply.str);
  };

  exec_ok({"SET", "big:str", big_str}, "SET big:str");
  exec_ok({"SET", "small:str", "ok"}, "SET small:str");

  std::vector<std::string> hset_big = {"HSET", "big:hash"};
  for (int i = 0; i < 250; ++i) {
    hset_big.push_back("f" + std::to_string(i));
    hset_big.push_back("v" + std::to_string(i));
  }
  exec_ok(hset_big, "HSET big:hash");
  exec_ok({"HSET", "small:hash", "a", "1", "b", "2"}, "HSET small:hash");

  std::vector<std::string> sadd_big = {"SADD", "big:set"};
  for (int i = 0; i < 250; ++i) {
    sadd_big.push_back("m" + std::to_string(i));
  }
  exec_ok(sadd_big, "SADD big:set");
  exec_ok({"SADD", "small:set", "x", "y"}, "SADD small:set");

  const dbpp::DbObject db1_obj{.name = "db1", .table_type = "REDIS DB"};
  server.ResetCommandCounts();
  const dbpp::TableModel table =
      dbpp::LoadRedisTableBlocking(client, 1, db1_obj, 0, 100, std::nullopt, {});
  Expect(table.error.empty() && table.snapshot && table.snapshot->rows.size() == 6,
         "LoadRedisTableBlocking bigkey mix failed: " + table.error);
  Expect(server.CommandCount("GETRANGE") == 1, "Expected 1 GETRANGE for big:str");
  Expect(server.CommandCount("GET") == 1, "Expected 1 GET for small:str only");
  Expect(server.CommandCount("HSCAN") == 1, "Expected 1 HSCAN for big:hash");
  Expect(server.CommandCount("HGETALL") == 1, "Expected 1 HGETALL for small:hash only");
  Expect(server.CommandCount("SSCAN") == 1, "Expected 1 SSCAN for big:set");
  Expect(server.CommandCount("SMEMBERS") == 1, "Expected 1 SMEMBERS for small:set only");

  auto find_row = [&](std::string_view key) -> std::optional<std::size_t> {
    if (!table.snapshot) return std::nullopt;
    for (std::size_t i = 0; i < table.snapshot->rows.size(); ++i) {
      if (table.snapshot->rows[i].values[0].text == key) return i;
    }
    return std::nullopt;
  };

  if (const auto idx = find_row("big:str")) {
    const dbpp::GridRow& row = table.snapshot->rows[*idx];
    Expect(row.values[4].text == std::to_string(big_str_len), "big:str size mismatch");
    Expect(row.values[1].display_text.empty(), "Redis cell must keep display_text empty");
    Expect(row.values[1].text.size() < big_str.size() &&
               row.values[1].text.find("65535") != std::string::npos &&
               row.values[1].text.find(std::to_string(big_str_len)) != std::string::npos &&
               row.values[1].text.find(utf8_mid[0]) == std::string::npos,
           "big:str preview must trim split UTF-8 byte and show truncation marker");

    const dbpp::AdbcQueryResult bad_val =
        dbpp::UpdateRedisCellBlocking(client, table, *idx, 1, "overwrite");
    Expect(!bad_val.error.empty(), "Truncated big:str must reject in-place value overwrite");

    dbpp::RedisValue strlen_reply;
    std::string err;
    Expect(client.Command({"STRLEN", "big:str"}, strlen_reply, err) &&
               strlen_reply.integer == big_str_len,
           "Rejected overwrite must leave big:str intact");

    const dbpp::AdbcQueryResult ttl_ok =
        dbpp::UpdateRedisCellBlocking(client, table, *idx, 3, "600");
    Expect(ttl_ok.error.empty(), "Updating TTL on big:str must still succeed: " + ttl_ok.error);
  } else {
    Expect(false, "big:str row missing");
  }

  if (const auto idx = find_row("small:str")) {
    Expect(table.snapshot->rows[*idx].values[1].text == "ok", "small:str value mismatch");
  } else {
    Expect(false, "small:str row missing");
  }

  if (const auto idx = find_row("big:hash")) {
    const dbpp::GridRow& row = table.snapshot->rows[*idx];
    Expect(row.values[4].text == "250", "big:hash size mismatch");
    Expect(row.values[1].text.find("250") != std::string::npos,
           "big:hash preview must show total field count truncation info");
    const dbpp::AdbcQueryResult bad_val =
        dbpp::UpdateRedisCellBlocking(client, table, *idx, 1, "{\"only\":1}");
    Expect(!bad_val.error.empty(), "Truncated big:hash must reject in-place value overwrite");
  } else {
    Expect(false, "big:hash row missing");
  }

  if (const auto idx = find_row("small:hash")) {
    const dbpp::AdbcQueryResult ok_val =
        dbpp::UpdateRedisCellBlocking(client, table, *idx, 1, "{\"a\":10,\"b\":20}");
    Expect(ok_val.error.empty(), "Small hash edit must still succeed: " + ok_val.error);
  } else {
    Expect(false, "small:hash row missing");
  }

  if (const auto idx = find_row("big:set")) {
    const dbpp::GridRow& row = table.snapshot->rows[*idx];
    Expect(row.values[4].text == "250", "big:set size mismatch");
    Expect(row.values[1].text.find("250") != std::string::npos,
           "big:set preview must show total member count truncation info");
    const dbpp::AdbcQueryResult bad_val =
        dbpp::UpdateRedisCellBlocking(client, table, *idx, 1, "[\"only\"]");
    Expect(!bad_val.error.empty(), "Truncated big:set must reject in-place value overwrite");
  } else {
    Expect(false, "big:set row missing");
  }

  if (const auto idx = find_row("small:set")) {
    const dbpp::AdbcQueryResult ok_val =
        dbpp::UpdateRedisCellBlocking(client, table, *idx, 1, "[\"x\",\"z\"]");
    Expect(ok_val.error.empty(), "Small set edit must still succeed: " + ok_val.error);
  } else {
    Expect(false, "small:set row missing");
  }

  // Stepped SCAN iteration cap and cancellation guard.
  dbpp_test::MockRedisServer scan_server;
  const std::string scan_uri = "redis://127.0.0.1:" + std::to_string(scan_server.Port()) + "/1";
  const dbpp::AdbcOpenResult scan_open = dbpp::OpenAdbcBlocking(dbpp::ConnectionRequest{
      .driver_path = "redis",
      .uri = scan_uri,
      .username = {},
      .password = {},
      .title = "scan-cap",
  });
  Expect(scan_open.error.empty() && scan_open.session != nullptr && scan_open.session->redis,
         "Connect to scan_server failed: " + scan_open.error);
  if (!scan_open.session || !scan_open.session->redis) {
    return;
  }
  dbpp::RedisClient& scan_client = *scan_open.session->redis;
  std::vector<std::vector<std::string>> seed_cmds;
  for (int i = 0; i < 250; ++i) {
    seed_cmds.push_back({"SET", "k:" + std::to_string(i), "v"});
  }
  std::vector<dbpp::RedisValue> seed_replies;
  std::string seed_err;
  Expect(scan_client.Pipeline(seed_cmds, seed_replies, seed_err) && seed_err.empty(),
         "Seed keys for SCAN cap test failed: " + seed_err);

  scan_server.SetScanStepSize(1);
  scan_server.ResetCommandCounts();
  const dbpp::RedisPageResult capped = dbpp::LoadRedisKeysPageBlocking(
      scan_client, db1_obj, 0, 100, std::nullopt, "nomatch:*");
  Expect(capped.error.empty() && capped.scan_capped && capped.scan_iterations_capped &&
             capped.total == 0 && scan_server.CommandCount("SCAN") == kTestMaxScanIterations,
         "SCAN iteration cap must stop after kMaxScanIterations batches");

  const dbpp::TableModel capped_tbl = dbpp::LoadRedisTableBlocking(
      scan_client, 1, db1_obj, 0, 100, std::nullopt, "nomatch:*");
  Expect(capped_tbl.error.empty() &&
             capped_tbl.message.find(std::to_string(kTestMaxScanIterations)) != std::string::npos,
         "LoadRedisTableBlocking message must report iteration cap: " + capped_tbl.message);

  scan_server.ResetCommandCounts();
  std::atomic<bool> cancel{true};
  const dbpp::RedisPageResult cancelled = dbpp::LoadRedisKeysPageBlocking(
      scan_client, db1_obj, 0, 100, std::nullopt, "*", &cancel);
  Expect(cancelled.scan_cancelled && !cancelled.error.empty() &&
             scan_server.CommandCount("SCAN") == 0,
         "Cancelled SCAN must abort immediately without issuing SCAN");
}

void TestRedisTypeFilterScanning() {
  dbpp_test::MockRedisServer server;
  const std::string uri = "redis://127.0.0.1:" + std::to_string(server.Port()) + "/0";
  const dbpp::AdbcOpenResult opened = dbpp::OpenAdbcBlocking(dbpp::ConnectionRequest{
      .driver_path = "redis",
      .uri = uri,
      .username = {},
      .password = {},
      .title = "type-filter",
  });
  Expect(opened.error.empty() && opened.session != nullptr && opened.session->redis != nullptr,
         "Connect to mock db0 failed: " + opened.error);
  if (!opened.session || !opened.session->redis) {
    return;
  }
  dbpp::RedisClient& client = *opened.session->redis;
  const dbpp::DbObject db0_obj{.name = "db0", .table_type = "REDIS DB"};

  // 1. Native SCAN ... TYPE (Redis 6.0+): only touches keys of the selected type.
  server.ResetCommandCounts();
  const dbpp::TableModel hash_only = dbpp::LoadRedisTableBlocking(
      client, 1, db0_obj, 0, 100, std::nullopt, {}, nullptr, "hash");
  Expect(hash_only.error.empty() && hash_only.total == 1 && hash_only.snapshot &&
             hash_only.snapshot->rows.size() == 1 &&
             hash_only.snapshot->rows[0].values[0].text == "user:100" &&
             hash_only.redis_type_filter == "hash" &&
             hash_only.executed_sql.find("TYPE hash") != std::string::npos,
         "LoadRedisTableBlocking TYPE hash mismatch: " + hash_only.error);
  Expect(server.CommandCount("HLEN") == 1 && server.CommandCount("STRLEN") == 0 &&
             server.CommandCount("LLEN") == 0 && server.CommandCount("SCARD") == 0 &&
             server.CommandCount("ZCARD") == 0,
         "SCAN ... TYPE hash must skip querying other key types");

  // 2. Combined MATCH + TYPE filtering (user:* + hash excludes list key user:200).
  const dbpp::TableModel matched_hash = dbpp::LoadRedisTableBlocking(
      client, 1, db0_obj, 0, 100, std::nullopt, "user:*", nullptr, "HASH");
  Expect(matched_hash.error.empty() && matched_hash.total == 1 && matched_hash.snapshot &&
             matched_hash.snapshot->rows.size() == 1 &&
             matched_hash.snapshot->rows[0].values[0].text == "user:100" &&
             matched_hash.redis_type_filter == "hash",
         "Combined MATCH + TYPE hash mismatch: " + matched_hash.error);

  // 3. Redis < 6.0 fallback (server rejects SCAN ... TYPE with ERR syntax error).
  server.SetDisableScanType(true);
  const dbpp::TableModel fallback_set = dbpp::LoadRedisTableBlocking(
      client, 1, db0_obj, 0, 100, std::nullopt, {}, nullptr, "set");
  Expect(fallback_set.error.empty() && fallback_set.total == 1 && fallback_set.snapshot &&
             fallback_set.snapshot->rows.size() == 1 &&
             fallback_set.snapshot->rows[0].values[0].text == "tags",
         "Redis < 6.0 TYPE fallback failed: " + fallback_set.error);
  server.SetDisableScanType(false);

  // 4. CLI rows --redis ... --type hash --json
  const CliRunResult cli_hash =
      InvokeCli({"rows", "--redis", uri, "db0", "--type", "hash", "--json"});
  Expect(cli_hash.code == 0 && cli_hash.out.find("\"total\":1") != std::string::npos &&
             cli_hash.out.find("user:100") != std::string::npos &&
             cli_hash.out.find("greeting") == std::string::npos,
         "CLI rows --redis --type hash failed: " + cli_hash.err + " / " + cli_hash.out);
}

void TestRedisTreeGroupingAndDetail() {
  // 1. Nested tree grouping & delimiter switching (including edge keys).
  const std::vector<std::string> sample_keys = {
      "a::b", "greeting", "user", "user:", "user:100", "user:profile:1", "user:profile:2",
  };
  std::unordered_set<std::string> expanded;
  std::unordered_set<std::string> all_folders;
  const auto top_rows =
      dbpp::BuildRedisTreeFlatRows(sample_keys, ":", expanded, &all_folders);
  Expect(top_rows.size() == 5 && top_rows[0].is_folder && top_rows[0].id == "a:" &&
             top_rows[0].key_count == 1 && top_rows[1].is_folder && top_rows[1].id == "user:" &&
             top_rows[1].label == "user" && top_rows[1].key_count == 3 &&
             !top_rows[2].is_folder && top_rows[2].full_key == "greeting" &&
             !top_rows[3].is_folder && top_rows[3].full_key == "user" &&
             !top_rows[4].is_folder && top_rows[4].full_key == "user:",
         "Collapsed ':' tree layout mismatch");
  Expect(all_folders.size() == 3 && all_folders.contains("user:profile:"),
         "All-folders output mismatch");

  expanded.insert("user:");
  const auto mid_rows = dbpp::BuildRedisTreeFlatRows(sample_keys, ":", expanded);
  Expect(mid_rows.size() == 7 && mid_rows[2].is_folder && mid_rows[2].id == "user:profile:" &&
             mid_rows[2].depth == 1 && mid_rows[2].key_count == 2 &&
             !mid_rows[3].is_folder && mid_rows[3].full_key == "user:100" &&
             mid_rows[3].label == "100" && mid_rows[3].depth == 1,
         "Expanded first level tree layout mismatch");

  expanded.insert("user:profile:");
  const auto leaf_rows = dbpp::BuildRedisTreeFlatRows(sample_keys, ":", expanded);
  Expect(leaf_rows.size() == 9 && !leaf_rows[3].is_folder &&
             leaf_rows[3].full_key == "user:profile:1" && leaf_rows[3].label == "1" &&
             leaf_rows[3].depth == 2 && !leaf_rows[4].is_folder &&
             leaf_rows[4].full_key == "user:profile:2" && leaf_rows[4].depth == 2,
         "Expanded second level tree layout mismatch");

  const auto flat_rows = dbpp::BuildRedisTreeFlatRows(sample_keys, "", expanded);
  Expect(flat_rows.size() == sample_keys.size() && !flat_rows[0].is_folder &&
             flat_rows[0].depth == 0,
         "Flat mode tree layout mismatch");

  const std::vector<std::string> slash_keys = {"app/v1/cfg", "app/v1/flag", "app/health"};
  const auto slash_rows =
      dbpp::BuildRedisTreeFlatRows(slash_keys, "/", std::unordered_set<std::string>{"app/", "app/v1/"});
  Expect(slash_rows.size() == 5 && slash_rows[0].id == "app/" && slash_rows[1].id == "app/v1/" &&
             slash_rows[2].full_key == "app/v1/cfg" && slash_rows[4].full_key == "app/health",
         "Custom '/' delimiter grouping mismatch");

  // 2. ScanRedisKeyNamesBlocking issues zero value/size/TTL commands.
  dbpp_test::MockRedisServer server;
  const std::string uri = "redis://127.0.0.1:" + std::to_string(server.Port()) + "/0";
  const dbpp::AdbcOpenResult opened = dbpp::OpenAdbcBlocking(dbpp::ConnectionRequest{
      .driver_path = "redis",
      .uri = uri,
      .username = {},
      .password = {},
      .title = "tree-scan",
  });
  Expect(opened.error.empty() && opened.session != nullptr && opened.session->redis != nullptr,
         "Connect for tree test failed: " + opened.error);
  if (!opened.session || !opened.session->redis) {
    return;
  }
  dbpp::RedisClient& client = *opened.session->redis;
  const dbpp::DbObject db0_obj{.name = "db0", .table_type = "REDIS DB"};

  server.ResetCommandCounts();
  const dbpp::RedisKeyListResult names =
      dbpp::ScanRedisKeyNamesBlocking(client, db0_obj, {}, {});
  Expect(names.error.empty() && names.keys.size() == 5 &&
             std::ranges::is_sorted(names.keys),
         "ScanRedisKeyNamesBlocking failed: " + names.error);
  Expect(server.CommandCount("GET") == 0 && server.CommandCount("HGETALL") == 0 &&
             server.CommandCount("STRLEN") == 0 && server.CommandCount("HLEN") == 0 &&
             server.CommandCount("TTL") == 0,
         "ScanRedisKeyNamesBlocking must not fetch values, sizes, or TTLs");

  // 3. LoadRedisKeyDetailBlocking on structured types & oversized string.
  const dbpp::RedisKeyDetail hash_det =
      dbpp::LoadRedisKeyDetailBlocking(client, db0_obj, "user:100");
  Expect(hash_det.error.empty() && hash_det.exists && hash_det.type == "hash" &&
             hash_det.size == 2 && hash_det.entries.size() == 2 &&
             hash_det.entries[0].first == "name" && hash_det.entries[0].second == "Alice",
         "LoadRedisKeyDetailBlocking hash mismatch");

  const dbpp::RedisKeyDetail zset_det =
      dbpp::LoadRedisKeyDetailBlocking(client, db0_obj, "leaderboard");
  Expect(zset_det.error.empty() && zset_det.exists && zset_det.type == "zset" &&
             zset_det.entries.size() == 2 && zset_det.entries[0].first == "alice",
         "LoadRedisKeyDetailBlocking zset mismatch");

  const std::string big_val(70000, 'x');
  {
    dbpp::RedisValue r;
    std::string e;
    Expect(client.Command({"SET", "tree:big", big_val, "EX", "200"}, r, e) && e.empty(),
           "SET tree:big failed: " + e);
    // Mock server SET doesn't parse EX, so set EXPIRE explicitly for TTL test.
    Expect(client.Command({"EXPIRE", "tree:big", "200"}, r, e) && e.empty(),
           "EXPIRE tree:big failed: " + e);
  }
  const dbpp::RedisKeyDetail preview_det =
      dbpp::LoadRedisKeyDetailBlocking(client, db0_obj, "tree:big", false);
  Expect(preview_det.error.empty() && preview_det.exists && preview_det.truncated &&
             preview_det.string_value.size() == 65536 && preview_det.size == 70000,
         "Detail preview for big string mismatch");

  const dbpp::RedisKeyDetail full_det =
      dbpp::LoadRedisKeyDetailBlocking(client, db0_obj, "tree:big", true);
  Expect(full_det.error.empty() && full_det.exists && !full_det.truncated &&
             full_det.string_value == big_val,
         "Detail load_full_string mismatch");

  // 4. Detail write operations (string update keeping TTL, TTL update, rename, delete).
  const dbpp::AdbcQueryResult save_res =
      dbpp::UpdateRedisKeyStringBlocking(client, db0_obj, "tree:big", "updated");
  Expect(save_res.error.empty(), "UpdateRedisKeyStringBlocking failed: " + save_res.error);
  const dbpp::RedisKeyDetail after_save =
      dbpp::LoadRedisKeyDetailBlocking(client, db0_obj, "tree:big");
  Expect(after_save.string_value == "updated" && after_save.ttl == 200,
         "UpdateRedisKeyStringBlocking must preserve TTL");

  const dbpp::AdbcQueryResult ttl_res =
      dbpp::UpdateRedisKeyTtlBlocking(client, db0_obj, "tree:big", "900");
  Expect(ttl_res.error.empty(), "UpdateRedisKeyTtlBlocking failed: " + ttl_res.error);
  Expect(dbpp::LoadRedisKeyDetailBlocking(client, db0_obj, "tree:big").ttl == 900,
         "TTL update to 900 mismatch");

  const dbpp::AdbcQueryResult ren_res =
      dbpp::RenameRedisKeyBlocking(client, db0_obj, "tree:big", "tree:big:renamed");
  Expect(ren_res.error.empty(), "RenameRedisKeyBlocking failed: " + ren_res.error);
  Expect(!dbpp::LoadRedisKeyDetailBlocking(client, db0_obj, "tree:big").exists &&
             dbpp::LoadRedisKeyDetailBlocking(client, db0_obj, "tree:big:renamed").exists,
         "Rename key check failed");

  const dbpp::AdbcQueryResult del_res =
      dbpp::DeleteRedisKeyByNameBlocking(client, db0_obj, "tree:big:renamed");
  Expect(del_res.error.empty() &&
             !dbpp::LoadRedisKeyDetailBlocking(client, db0_obj, "tree:big:renamed").exists,
         "DeleteRedisKeyByNameBlocking check failed");
}

} // namespace

int main() {
  ScopedTempDir temp;
  TestUriAndParsingHelpers();
  TestRedisSessionCrudAndCli(temp.path);
  TestRedisAuditRegressions();
  TestRedisDisconnectDetection();
  TestRedisBigKeyAndScanCapOptimizations();
  TestRedisTypeFilterScanning();
  TestRedisTreeGroupingAndDetail();

  if (g_failures != 0) {
    std::cerr << g_failures << " Redis test assertion(s) failed\n";
    return 1;
  }
  std::cout << "test_redis passed\n";
  return 0;
}
