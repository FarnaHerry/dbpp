module;

#include <huxerui/state.h>

export module dbpp.mcp;

import std;
import dbpp.settings;
import dbpp.types;
import dbpp.adbc;
import dbpp.db;

export namespace dbpp {

struct McpServerConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = kDefaultMcpPort;
  bool read_only = true;
  std::string profiles_db_path;

  bool operator==(const McpServerConfig&) const = default;
};

struct McpServerStatus {
  bool running = false;
  std::string host = "127.0.0.1";
  std::uint16_t bound_port = 0;
  bool read_only = true;
  std::string error;

  bool operator==(const McpServerStatus&) const = default;
};

/// Embedded localhost Streamable HTTP MCP (Model Context Protocol) server.
/// Exposes JSON-RPC 2.0 over `POST /mcp` (and `GET /health`) so local AI
/// clients can list saved connections, inspect table schemas, and run queries.
class McpServer {
public:
  McpServer();
  ~McpServer();
  McpServer(const McpServer&) = delete;
  McpServer& operator=(const McpServer&) = delete;

  bool Start(const McpServerConfig& config, std::string& error_out);
  void Stop();

  [[nodiscard]] bool IsRunning() const noexcept;
  [[nodiscard]] std::uint16_t BoundPort() const noexcept;
  [[nodiscard]] McpServerStatus Status() const;
  [[nodiscard]] std::string EndpointUrl() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Application-scoped wrapper holding the live MCP server and reactive UI status.
struct McpServerService {
  McpServer server;
  huxerui::State<McpServerStatus> status{McpServerStatus{}};
  std::string profiles_db_path;

  bool ApplySettings(bool enabled, std::string host, std::uint16_t port, bool read_only);
  void Shutdown();
};

[[nodiscard]] std::string BuildMcpEndpointUrl(std::string_view host, std::uint16_t port);
[[nodiscard]] std::string BuildMcpClientConfigJson(std::string_view endpoint_url);

} // namespace dbpp
