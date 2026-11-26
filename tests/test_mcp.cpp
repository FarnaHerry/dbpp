#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

import std;
import dbpp.types;
import dbpp.settings;
import dbpp.adbc;
import dbpp.db;
import dbpp.mcp;

namespace {

namespace fs = std::filesystem;

void Expect(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct TempDir {
  fs::path path;
  TempDir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() / ("dbpp_test_mcp_" + std::to_string(stamp));
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

#ifdef _WIN32
using ClientSock = SOCKET;
inline constexpr ClientSock kInvalidClient = INVALID_SOCKET;
void CloseClient(ClientSock s) {
  if (s != INVALID_SOCKET) {
    ::closesocket(s);
  }
}
#else
using ClientSock = int;
inline constexpr ClientSock kInvalidClient = -1;
void CloseClient(ClientSock s) {
  if (s >= 0) {
    ::close(s);
  }
}
#endif

struct HttpResult {
  int status = 0;
  std::string raw;
  std::string body;
};

HttpResult SendHttp(std::uint16_t port, std::string_view method, std::string_view path,
                    std::string_view body = {}) {
  const ClientSock fd = ::socket(AF_INET, SOCK_STREAM, 0);
  Expect(fd != kInvalidClient, "Failed to create test client socket");
  struct Guard {
    ClientSock s;
    ~Guard() { CloseClient(s); }
  } guard{fd};

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  Expect(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1, "inet_pton failed");
  Expect(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0,
         "Failed to connect to local MCP server");

  std::string req;
  req += std::string(method) + " " + std::string(path) + " HTTP/1.1\r\n";
  req += "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";
  req += "Content-Type: application/json\r\n";
  req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  req += "Connection: close\r\n\r\n";
  req += body;

  std::size_t sent = 0;
  while (sent < req.size()) {
    const auto n = ::send(fd, req.data() + sent, static_cast<int>(req.size() - sent), 0);
    Expect(n > 0, "send failed");
    sent += static_cast<std::size_t>(n);
  }

  std::string raw;
  char buf[4096];
  for (;;) {
    const auto n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      break;
    }
    raw.append(buf, static_cast<std::size_t>(n));
  }

  HttpResult out;
  out.raw = raw;
  const auto hdr_end = raw.find("\r\n\r\n");
  Expect(hdr_end != std::string::npos, "Invalid HTTP response (missing CRLFCRLF)");
  const std::string headers = raw.substr(0, hdr_end);
  out.body = raw.substr(hdr_end + 4);

  const auto sp1 = headers.find(' ');
  Expect(sp1 != std::string::npos, "Missing status code in HTTP response");
  out.status = std::stoi(headers.substr(sp1 + 1, 3));
  return out;
}

void TestMcpServerEndToEnd() {
  TempDir temp;
  const fs::path target_db = temp.path / "sample.sqlite";
  const fs::path profiles_db = temp.path / "dbpp.sqlite3";

  {
    dbpp::ConnectionRequest req{
        .driver_path = DBPP_SQLITE_DRIVER_PATH,
        .uri = target_db.string(),
        .username = {},
        .password = {},
        .title = {},
    };
    auto opened = dbpp::OpenAdbcBlocking(req);
    Expect(opened.session != nullptr && opened.error.empty(),
           "Failed to create sample SQLite DB: " + opened.error);
    auto ddl = dbpp::ExecuteBlocking(
        *opened.session,
        "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT NOT NULL, qty INTEGER);", false, 0);
    Expect(ddl.error.empty(), "Failed to create sample table: " + ddl.error);
    auto ins = dbpp::ExecuteBlocking(
        *opened.session,
        "INSERT INTO items (id, name, qty) VALUES (1, 'alpha', 10), (2, 'beta', 20);", false, 0);
    Expect(ins.error.empty(), "Failed to seed sample rows: " + ins.error);
  }

  Expect(dbpp::SaveConnectionProfiles(
             profiles_db.string(),
             {dbpp::ConnectionProfile{
                 .name = "LocalSample",
                 .driver_path = DBPP_SQLITE_DRIVER_PATH,
                 .uri = target_db.string(),
                 .username = "reader",
                 .password = "super-secret-password",
             }}),
         "Failed to save profile DB");

  dbpp::McpServer server;
  std::string err;
  Expect(server.Start(
             dbpp::McpServerConfig{
                 .host = "127.0.0.1",
                 .port = 0,
                 .read_only = true,
                 .profiles_db_path = profiles_db.string(),
             },
             err),
         "McpServer::Start failed: " + err);
  const std::uint16_t port = server.BoundPort();
  Expect(port != 0 && server.IsRunning(), "Server should bind ephemeral port and be running");

  const HttpResult health = SendHttp(port, "GET", "/health");
  Expect(health.status == 200, "GET /health should return 200");
  Expect(health.body.find("dbpp-mcp") != std::string::npos, "/health missing service name");

  const HttpResult cors = SendHttp(port, "OPTIONS", "/mcp");
  Expect(cors.status == 204, "OPTIONS /mcp should return 204");

  const HttpResult init_res = SendHttp(
      port, "POST", "/mcp",
      R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}})");
  Expect(init_res.status == 200, "initialize should return 200");
  Expect(init_res.body.find("2025-03-26") != std::string::npos &&
             init_res.body.find("dbpp") != std::string::npos,
         "initialize response missing protocolVersion or serverInfo");

  const HttpResult notif_res =
      SendHttp(port, "POST", "/mcp", R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
  Expect(notif_res.status == 202, "notifications/initialized should return 202");

  const HttpResult list_res =
      SendHttp(port, "POST", "/mcp", R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
  Expect(list_res.status == 200, "tools/list should return 200");
  Expect(list_res.body.find("list_profiles") != std::string::npos &&
             list_res.body.find("list_tables") != std::string::npos &&
             list_res.body.find("describe_table") != std::string::npos &&
             list_res.body.find("run_query") != std::string::npos &&
             list_res.body.find("get_connection_info") != std::string::npos &&
             list_res.body.find("get_table_stats") != std::string::npos &&
             list_res.body.find("inspect_query_schema") != std::string::npos,
         "tools/list missing expected tools");

  const HttpResult prof_res = SendHttp(
      port, "POST", "/mcp",
      R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"list_profiles","arguments":{}}})");
  Expect(prof_res.status == 200 && prof_res.body.find("LocalSample") != std::string::npos,
         "list_profiles should include saved profile");
  Expect(prof_res.body.find("super-secret-password") == std::string::npos,
         "list_profiles MUST NOT leak saved passwords");

  const HttpResult tables_res = SendHttp(
      port, "POST", "/mcp",
      R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"list_tables","arguments":{"profile":"LocalSample"}}})");
  Expect(tables_res.status == 200 && tables_res.body.find("items") != std::string::npos,
         "list_tables should list items table");

  const HttpResult desc_res = SendHttp(
      port, "POST", "/mcp",
      R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"describe_table","arguments":{"profile":"LocalSample","table":"items"}}})");
  Expect(desc_res.status == 200 && desc_res.body.find("qty") != std::string::npos,
         "describe_table should list columns");

  const HttpResult info_res = SendHttp(
      port, "POST", "/mcp",
      R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"get_connection_info","arguments":{"profile":"LocalSample"}}})");
  Expect(info_res.status == 200 && info_res.body.find("SQLite") != std::string::npos,
         "get_connection_info should include SQLite vendor");

  const HttpResult stats_res = SendHttp(
      port, "POST", "/mcp",
      R"({"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"get_table_stats","arguments":{"profile":"LocalSample","table":"items"}}})");
  Expect(stats_res.status == 200 && stats_res.body.find("items") != std::string::npos,
         "get_table_stats should succeed");

  const HttpResult inspect_res = SendHttp(
      port, "POST", "/mcp",
      R"({"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"inspect_query_schema","arguments":{"profile":"LocalSample","sql":"SELECT id, name FROM items"}}})");
  Expect(inspect_res.status == 200 && inspect_res.body.find("name") != std::string::npos,
         "inspect_query_schema should return output columns");

  const HttpResult sel_res = SendHttp(
      port, "POST", "/mcp",
      R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"run_query","arguments":{"profile":"LocalSample","sql":"SELECT name, qty FROM items ORDER BY id"}}})");
  Expect(sel_res.status == 200 && sel_res.body.find("alpha") != std::string::npos &&
             sel_res.body.find("beta") != std::string::npos,
         "run_query SELECT should return seeded rows");

  const HttpResult write_blocked = SendHttp(
      port, "POST", "/mcp",
      R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"run_query","arguments":{"profile":"LocalSample","sql":"DELETE FROM items WHERE id = 1"}}})");
  Expect(write_blocked.status == 200 &&
             write_blocked.body.find("\"isError\":true") != std::string::npos,
         "run_query DELETE must be blocked when read_only=true");

  server.Stop();
  Expect(!server.IsRunning() && server.BoundPort() == 0, "Server should stop cleanly");

  Expect(server.Start(
             dbpp::McpServerConfig{
                 .host = "127.0.0.1",
                 .port = 0,
                 .read_only = false,
                 .profiles_db_path = profiles_db.string(),
             },
             err),
         "Restarting server with read_only=false failed: " + err);
  const HttpResult write_ok = SendHttp(
      server.BoundPort(), "POST", "/mcp",
      R"json({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"run_query","arguments":{"profile":"LocalSample","sql":"INSERT INTO items (id, name, qty) VALUES (3, 'gamma', 30)"}}})json");
  Expect(write_ok.status == 200 && write_ok.body.find("\"isError\":false") != std::string::npos,
         "run_query INSERT should succeed when read_only=false");
  server.Stop();
}

} // namespace

int main() {
  try {
    TestMcpServerEndToEnd();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "test_mcp failed: " << e.what() << "\n";
    return 1;
  }
}
