module;

#include <huxerui/state.h>

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
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

module dbpp.mcp;

import std;
import dbpp.settings;
import dbpp.types;
import dbpp.adbc;
import dbpp.db;

#ifndef DBPP_VERSION
#define DBPP_VERSION "0.4.3"
#endif

namespace dbpp {

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
inline constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
int PollSockets(pollfd* fds, unsigned long count, int timeout_ms) {
  return ::WSAPoll(fds, count, timeout_ms);
}
void CloseNativeSocket(NativeSocket fd) noexcept {
  if (fd != INVALID_SOCKET) {
    ::closesocket(fd);
  }
}
bool EnsureWsaInitialized() {
  static const bool initialized = [] {
    WSADATA data{};
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return initialized;
}
std::string LastSocketErrorString() {
  return "WSA error " + std::to_string(::WSAGetLastError());
}
void SetSocketTimeouts(NativeSocket fd) noexcept {
  const DWORD timeout_ms = 5000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms),
               sizeof(timeout_ms));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms),
               sizeof(timeout_ms));
}
#else
using NativeSocket = int;
inline constexpr NativeSocket kInvalidSocket = -1;
int PollSockets(pollfd* fds, nfds_t count, int timeout_ms) {
  return ::poll(fds, count, timeout_ms);
}
void CloseNativeSocket(NativeSocket fd) noexcept {
  if (fd >= 0) {
    ::close(fd);
  }
}
bool EnsureWsaInitialized() {
  return true;
}
std::string LastSocketErrorString() {
  return std::strerror(errno);
}
void SetSocketTimeouts(NativeSocket fd) noexcept {
  struct timeval tv {};
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
  const int on = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
}
#endif

struct UniqueSocket {
  NativeSocket fd = kInvalidSocket;

  UniqueSocket() = default;
  explicit UniqueSocket(NativeSocket handle) noexcept : fd(handle) {}
  UniqueSocket(const UniqueSocket&) = delete;
  UniqueSocket& operator=(const UniqueSocket&) = delete;
  UniqueSocket& operator=(UniqueSocket&& other) noexcept {
    if (this != &other) {
      Reset();
      fd = std::exchange(other.fd, kInvalidSocket);
    }
    return *this;
  }
  ~UniqueSocket() { Reset(); }

  void Reset(NativeSocket next = kInvalidSocket) noexcept {
    if (fd != kInvalidSocket) {
      CloseNativeSocket(fd);
    }
    fd = next;
  }

  [[nodiscard]] bool Valid() const noexcept { return fd != kInvalidSocket; }
};

struct AddrInfoGuard {
  struct addrinfo* ptr = nullptr;
  ~AddrInfoGuard() {
    if (ptr != nullptr) {
      ::freeaddrinfo(ptr);
    }
  }
};

using HttpResponse = detail::McpHttpResponse;

// ---------------------------------------------------------------------------
// HTTP/1.1 connection framing
// ---------------------------------------------------------------------------

constexpr std::size_t kMaxHttpHeaderBytes = 64 * 1024;
constexpr std::size_t kMaxHttpBodyBytes = 4 * 1024 * 1024;

bool SendAll(NativeSocket fd, std::string_view data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    const auto chunk = static_cast<int>(
        std::min<std::size_t>(data.size() - sent, 65536));
    const auto n = ::send(fd, data.data() + sent, chunk, flags);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

void WriteHttpResponse(NativeSocket fd, const HttpResponse& res) {
  std::string raw;
  raw.reserve(256 + res.body.size());
  raw += "HTTP/1.1 ";
  raw += std::to_string(res.status_code);
  raw.push_back(' ');
  raw += res.status_text;
  raw += "\r\nContent-Type: ";
  raw += res.content_type;
  raw += "\r\nContent-Length: ";
  raw += std::to_string(res.body.size());
  raw += "\r\nAccess-Control-Allow-Origin: *";
  raw += "\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS";
  raw += "\r\nAccess-Control-Allow-Headers: Content-Type, Accept, Mcp-Session-Id";
  raw += "\r\nCache-Control: no-store";
  raw += "\r\nConnection: close\r\n\r\n";
  raw += res.body;
  (void)SendAll(fd, raw);
}

void HandleHttpClient(NativeSocket client_fd, const McpServerConfig& config) {
  SetSocketTimeouts(client_fd);

  std::string buffer;
  buffer.reserve(4096);
  std::size_t header_end = std::string::npos;
  char chunk[4096];

  while (header_end == std::string::npos) {
    const auto n = ::recv(client_fd, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      return;
    }
    buffer.append(chunk, static_cast<std::size_t>(n));
    header_end = buffer.find("\r\n\r\n");
    if (header_end == std::string::npos && buffer.size() > kMaxHttpHeaderBytes) {
      WriteHttpResponse(client_fd, HttpResponse{
                                       .status_code = 431,
                                       .status_text = "Request Header Fields Too Large",
                                       .body = R"({"error":"Headers too large"})",
                                   });
      return;
    }
  }

  const std::string_view header_block(buffer.data(), header_end);
  const std::size_t first_crlf = header_block.find("\r\n");
  const std::string_view request_line =
      first_crlf == std::string_view::npos ? header_block : header_block.substr(0, first_crlf);

  const std::size_t sp1 = request_line.find(' ');
  const std::size_t sp2 = sp1 == std::string_view::npos ? std::string_view::npos
                                                        : request_line.find(' ', sp1 + 1);
  if (sp1 == std::string_view::npos) {
    WriteHttpResponse(client_fd, HttpResponse{
                                     .status_code = 400,
                                     .status_text = "Bad Request",
                                     .body = R"({"error":"Malformed request line"})",
                                 });
    return;
  }

  const std::string method = detail::McpLowerAscii(request_line.substr(0, sp1));
  std::string_view raw_target =
      sp2 == std::string_view::npos ? request_line.substr(sp1 + 1)
                                    : request_line.substr(sp1 + 1, sp2 - sp1 - 1);
  if (const auto q = raw_target.find('?'); q != std::string_view::npos) {
    raw_target = raw_target.substr(0, q);
  }
  const std::string path(raw_target);

  std::size_t content_length = 0;
  if (first_crlf != std::string_view::npos) {
    std::string_view rest = header_block.substr(first_crlf + 2);
    while (!rest.empty()) {
      const auto next_crlf = rest.find("\r\n");
      const std::string_view line =
          next_crlf == std::string_view::npos ? rest : rest.substr(0, next_crlf);
      if (const auto colon = line.find(':'); colon != std::string_view::npos) {
        const std::string hdr_name = detail::McpLowerAscii(detail::McpTrimView(line.substr(0, colon)));
        if (hdr_name == "content-length") {
          const std::string_view val = detail::McpTrimView(line.substr(colon + 1));
          std::size_t parsed = 0;
          const auto [end, ec] = std::from_chars(val.data(), val.data() + val.size(), parsed);
          if (ec != std::errc{} || end != val.data() + val.size() || parsed > kMaxHttpBodyBytes) {
            WriteHttpResponse(client_fd, HttpResponse{
                                             .status_code = 413,
                                             .status_text = "Payload Too Large",
                                             .body = R"({"error":"Payload too large"})",
                                         });
            return;
          }
          content_length = parsed;
        }
      }
      if (next_crlf == std::string_view::npos) {
        break;
      }
      rest.remove_prefix(next_crlf + 2);
    }
  }

  const std::size_t body_start = header_end + 4;
  while (buffer.size() - body_start < content_length) {
    const auto n = ::recv(client_fd, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      return;
    }
    buffer.append(chunk, static_cast<std::size_t>(n));
    if (buffer.size() - body_start > kMaxHttpBodyBytes) {
      WriteHttpResponse(client_fd, HttpResponse{
                                       .status_code = 413,
                                       .status_text = "Payload Too Large",
                                       .body = R"({"error":"Payload too large"})",
                                   });
      return;
    }
  }

  const std::string_view body(buffer.data() + body_start, content_length);

  if (method == "options") {
    WriteHttpResponse(client_fd, HttpResponse{
                                     .status_code = 204,
                                     .status_text = "No Content",
                                     .content_type = "text/plain; charset=utf-8",
                                     .body = {},
                                 });
    return;
  }

  if (method == "get" && path == "/health") {
    WriteHttpResponse(
        client_fd,
        HttpResponse{
            .status_code = 200,
            .status_text = "OK",
            .body = detail::McpBuildHealthJson(config.read_only),
        });
    return;
  }

  if (method == "post" && (path == "/mcp" || path == "/")) {
    WriteHttpResponse(client_fd, detail::HandleMcpJsonRpcBody(config, body));
    return;
  }

  WriteHttpResponse(client_fd, HttpResponse{
                                   .status_code = 404,
                                   .status_text = "Not Found",
                                   .body = R"({"error":"Not found"})",
                               });
}

bool OpenListenSocket(const std::string& host, std::uint16_t port, UniqueSocket& socket_out,
                      std::uint16_t& bound_port_out, std::string& error_out) {
  if (!EnsureWsaInitialized()) {
    error_out = "Failed to initialize Winsock";
    return false;
  }

  const std::string effective_host = NormalizeMcpHost(host);
  const std::string port_str = std::to_string(port);

  struct addrinfo hints {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  AddrInfoGuard res;
  const int gai = ::getaddrinfo(effective_host.c_str(), port_str.c_str(), &hints, &res.ptr);
  if (gai != 0 || res.ptr == nullptr) {
    // Fallback to AF_UNSPEC (e.g. IPv6 literal)
    hints.ai_family = AF_UNSPEC;
    res.ptr = nullptr;
    const int gai2 = ::getaddrinfo(effective_host.c_str(), port_str.c_str(), &hints, &res.ptr);
    if (gai2 != 0 || res.ptr == nullptr) {
      error_out = "Failed to resolve host `" + effective_host + "`";
      return false;
    }
  }

  for (struct addrinfo* rp = res.ptr; rp != nullptr; rp = rp->ai_next) {
    UniqueSocket sock(::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol));
    if (!sock.Valid()) {
      continue;
    }
#ifndef _WIN32
    const int opt = 1;
    ::setsockopt(sock.fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
    if (::bind(sock.fd, rp->ai_addr, static_cast<socklen_t>(rp->ai_addrlen)) != 0) {
      error_out = "Bind failed on " + effective_host + ":" + port_str + ": " + LastSocketErrorString();
      continue;
    }
    if (::listen(sock.fd, 16) != 0) {
      error_out = "Listen failed: " + LastSocketErrorString();
      continue;
    }

    struct sockaddr_storage bound {};
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(sock.fd, reinterpret_cast<struct sockaddr*>(&bound), &bound_len) != 0) {
      error_out = "getsockname failed: " + LastSocketErrorString();
      continue;
    }
    if (bound.ss_family == AF_INET) {
      bound_port_out = ntohs(reinterpret_cast<const struct sockaddr_in*>(&bound)->sin_port);
    } else if (bound.ss_family == AF_INET6) {
      bound_port_out = ntohs(reinterpret_cast<const struct sockaddr_in6*>(&bound)->sin6_port);
    } else {
      bound_port_out = port;
    }
    socket_out = std::move(sock);
    error_out.clear();
    return true;
  }

  if (error_out.empty()) {
    error_out = "Unable to bind socket on " + effective_host + ":" + port_str;
  }
  return false;
}

} // namespace

struct McpServer::Impl {
  mutable std::mutex mutex;
  McpServerConfig config;
  UniqueSocket listen_socket;
  std::atomic<bool> running{false};
  std::atomic<bool> stop_requested{false};
  std::atomic<std::uint16_t> bound_port{0};
  std::string last_error;
  std::thread accept_thread;
};

McpServer::McpServer() : impl_(std::make_unique<Impl>()) {}

McpServer::~McpServer() {
  Stop();
}

bool McpServer::Start(const McpServerConfig& config, std::string& error_out) {
  Stop();

  McpServerConfig normalized = config;
  normalized.host = NormalizeMcpHost(normalized.host);

  UniqueSocket listen_sock;
  std::uint16_t actual_port = 0;
  std::string err;
  if (!OpenListenSocket(normalized.host, normalized.port, listen_sock, actual_port, err)) {
    std::scoped_lock lock(impl_->mutex);
    impl_->config = normalized;
    impl_->bound_port.store(0, std::memory_order_release);
    impl_->running.store(false, std::memory_order_release);
    impl_->last_error = err;
    error_out = std::move(err);
    return false;
  }

  {
    std::scoped_lock lock(impl_->mutex);
    impl_->config = normalized;
    impl_->listen_socket = std::move(listen_sock);
    impl_->bound_port.store(actual_port, std::memory_order_release);
    impl_->stop_requested.store(false, std::memory_order_release);
    impl_->running.store(true, std::memory_order_release);
    impl_->last_error.clear();
  }

  Impl* state = impl_.get();
  impl_->accept_thread = std::thread([state, normalized]() {
    for (;;) {
      if (state->stop_requested.load(std::memory_order_acquire)) {
        break;
      }
      NativeSocket fd = kInvalidSocket;
      {
        std::scoped_lock lock(state->mutex);
        fd = state->listen_socket.fd;
      }
      if (fd == kInvalidSocket) {
        break;
      }
      pollfd pfd{};
      pfd.fd = fd;
      pfd.events = POLLIN;
      const int ready = PollSockets(&pfd, 1, 100);
      if (ready <= 0) {
        continue;
      }
      if (state->stop_requested.load(std::memory_order_acquire)) {
        break;
      }
      UniqueSocket client(::accept(fd, nullptr, nullptr));
      if (!client.Valid()) {
        continue;
      }
      HandleHttpClient(client.fd, normalized);
    }
    state->running.store(false, std::memory_order_release);
  });

  error_out.clear();
  return true;
}

void McpServer::Stop() {
  if (!impl_) {
    return;
  }
  impl_->stop_requested.store(true, std::memory_order_release);
  std::thread to_join;
  {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->accept_thread.joinable()) {
      to_join = std::move(impl_->accept_thread);
    }
  }
  if (to_join.joinable()) {
    to_join.join();
  }
  {
    std::scoped_lock lock(impl_->mutex);
    impl_->listen_socket.Reset();
    impl_->running.store(false, std::memory_order_release);
    impl_->bound_port.store(0, std::memory_order_release);
  }
}

bool McpServer::IsRunning() const noexcept {
  return impl_ && impl_->running.load(std::memory_order_acquire);
}

std::uint16_t McpServer::BoundPort() const noexcept {
  return impl_ ? impl_->bound_port.load(std::memory_order_acquire) : 0;
}

McpServerStatus McpServer::Status() const {
  if (!impl_) {
    return McpServerStatus{};
  }
  std::scoped_lock lock(impl_->mutex);
  return McpServerStatus{
      .running = impl_->running.load(std::memory_order_acquire),
      .host = impl_->config.host,
      .bound_port = impl_->bound_port.load(std::memory_order_acquire),
      .read_only = impl_->config.read_only,
      .error = impl_->last_error,
  };
}

std::string McpServer::EndpointUrl() const {
  const McpServerStatus st = Status();
  return BuildMcpEndpointUrl(st.host, st.bound_port != 0 ? st.bound_port : impl_->config.port);
}

bool McpServerService::ApplySettings(bool enabled, std::string host, std::uint16_t port,
                                     bool read_only) {
  const std::string norm_host = NormalizeMcpHost(host);
  const std::uint16_t effective_port = port == 0 ? kDefaultMcpPort : port;

  if (!enabled) {
    server.Stop();
    status = McpServerStatus{
        .running = false,
        .host = norm_host,
        .bound_port = 0,
        .read_only = read_only,
        .error = {},
    };
    return true;
  }

  const McpServerStatus current = server.Status();
  if (current.running && current.host == norm_host && current.bound_port == effective_port &&
      current.read_only == read_only) {
    status = current;
    return true;
  }

  std::string err;
  const bool ok = server.Start(
      McpServerConfig{
          .host = norm_host,
          .port = effective_port,
          .read_only = read_only,
          .profiles_db_path = profiles_db_path,
      },
      err);
  status = server.Status();
  return ok;
}

void McpServerService::Shutdown() {
  server.Stop();
  McpServerStatus st = status.Get();
  st.running = false;
  st.bound_port = 0;
  status = std::move(st);
}

std::string BuildMcpEndpointUrl(std::string_view host, std::uint16_t port) {
  const std::string h = NormalizeMcpHost(host);
  const std::uint16_t p = port == 0 ? kDefaultMcpPort : port;
  if (h.find(':') != std::string::npos && !h.starts_with('[')) {
    return "http://[" + h + "]:" + std::to_string(p) + "/mcp";
  }
  return "http://" + h + ":" + std::to_string(p) + "/mcp";
}

} // namespace dbpp
