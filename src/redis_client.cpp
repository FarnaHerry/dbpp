module;

#include <huxerui/datatable.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstring>

module dbpp.redis;

import std;
import dbpp.types;
import dbpp.i18n;

namespace dbpp {

namespace {

constexpr int kConnectTimeoutMs = 5000;
constexpr int kSocketIoTimeoutMs = 15000;

// ---- Platform Socket Helpers ----------------------------------------------

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

void EnsureWinSockInitialized() {
  struct WsaInit {
    WsaInit() {
      WSADATA data{};
      (void)WSAStartup(MAKEWORD(2, 2), &data);
    }
  };
  static WsaInit init;
}

void CloseSocketHandle(SocketHandle s) {
  if (s != kInvalidSocket) {
    closesocket(s);
  }
}

std::string WideToUtf8(const wchar_t* wide) {
  if (wide == nullptr || *wide == L'\0') {
    return {};
  }
  const int utf8_len =
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (utf8_len <= 1) {
    return {};
  }
  std::string out(static_cast<std::size_t>(utf8_len - 1), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), utf8_len, nullptr, nullptr) <= 0) {
    return {};
  }
  return out;
}

std::string LastSocketErrorText(int err_code = 0) {
  const int code = err_code != 0 ? err_code : WSAGetLastError();
  wchar_t* msg = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, static_cast<DWORD>(code), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                 reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
  std::string converted = WideToUtf8(msg);
  if (msg != nullptr) {
    LocalFree(msg);
  }
  const std::string_view trimmed = detail::RedisTrimAscii(converted);
  return trimmed.empty() ? ("WSA error " + std::to_string(code)) : std::string(trimmed);
}

std::string GaiErrorText(int err_code) {
  std::string converted = WideToUtf8(gai_strerrorW(err_code));
  const std::string_view trimmed = detail::RedisTrimAscii(converted);
  return trimmed.empty() ? ("getaddrinfo error " + std::to_string(err_code)) : std::string(trimmed);
}

bool SetNonBlocking(SocketHandle s, bool non_blocking) {
  u_long mode = non_blocking ? 1UL : 0UL;
  return ioctlsocket(s, FIONBIO, &mode) == 0;
}

void ApplySocketTimeouts(SocketHandle s, int timeout_ms) {
  const DWORD ms = static_cast<DWORD>(timeout_ms);
  (void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
  (void)setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}

bool WaitConnectReady(SocketHandle s, int timeout_ms, std::string& error_out) {
  WSAPOLLFD pfd{};
  pfd.fd = s;
  pfd.events = POLLWRNORM;
  const int rc = WSAPoll(&pfd, 1, timeout_ms);
  if (rc == 0) {
    error_out = Tr("Connection timed out", "连接超时");
    return false;
  }
  if (rc < 0) {
    error_out = LastSocketErrorText();
    return false;
  }
  int so_err = 0;
  int len = sizeof(so_err);
  if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &len) != 0) {
    error_out = LastSocketErrorText();
    return false;
  }
  if (so_err != 0) {
    error_out = LastSocketErrorText(so_err);
    return false;
  }
  return true;
}

bool IsWouldBlockError(int code) {
  return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS;
}

bool IsTimedOutError(int code) {
  return code == WSAETIMEDOUT;
}

int CurrentSocketError() {
  return WSAGetLastError();
}

#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

void EnsureWinSockInitialized() {}

void CloseSocketHandle(SocketHandle s) {
  if (s >= 0) {
    close(s);
  }
}

std::string LastSocketErrorText(int err_code = 0) {
  const int code = err_code != 0 ? err_code : errno;
  return std::strerror(code);
}

std::string GaiErrorText(int err_code) {
  const char* msg = gai_strerror(err_code);
  return (msg != nullptr && *msg != '\0') ? std::string(msg)
                                          : ("getaddrinfo error " + std::to_string(err_code));
}

bool SetNonBlocking(SocketHandle s, bool non_blocking) {
  const int flags = fcntl(s, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  const int next = non_blocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  return fcntl(s, F_SETFL, next) == 0;
}

void ApplySocketTimeouts(SocketHandle s, int timeout_ms) {
  struct timeval tv {};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
  (void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  (void)setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#if defined(SO_NOSIGPIPE)
  const int on = 1;
  (void)setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
}

bool WaitConnectReady(SocketHandle s, int timeout_ms, std::string& error_out) {
  struct pollfd pfd {};
  pfd.fd = s;
  pfd.events = POLLOUT;
  int rc = 0;
  do {
    rc = poll(&pfd, 1, timeout_ms);
  } while (rc < 0 && errno == EINTR);
  if (rc == 0) {
    error_out = Tr("Connection timed out", "连接超时");
    return false;
  }
  if (rc < 0) {
    error_out = LastSocketErrorText();
    return false;
  }
  int so_err = 0;
  socklen_t len = sizeof(so_err);
  if (getsockopt(s, SOL_SOCKET, SO_ERROR, &so_err, &len) != 0) {
    error_out = LastSocketErrorText();
    return false;
  }
  if (so_err != 0) {
    error_out = LastSocketErrorText(so_err);
    return false;
  }
  return true;
}

bool IsWouldBlockError(int code) {
  return code == EINPROGRESS || code == EWOULDBLOCK || code == EAGAIN;
}

bool IsTimedOutError(int code) {
  return code == EAGAIN || code == EWOULDBLOCK || code == ETIMEDOUT;
}

int CurrentSocketError() {
  return errno;
}
#endif

class UniqueSocket {
public:
  UniqueSocket() = default;
  explicit UniqueSocket(SocketHandle fd) noexcept : fd_(fd) {}
  UniqueSocket(const UniqueSocket&) = delete;
  UniqueSocket& operator=(const UniqueSocket&) = delete;
  ~UniqueSocket() { Reset(); }

  [[nodiscard]] SocketHandle Get() const noexcept { return fd_; }
  [[nodiscard]] bool Valid() const noexcept { return fd_ != kInvalidSocket; }

  SocketHandle Release() noexcept {
    const SocketHandle out = fd_;
    fd_ = kInvalidSocket;
    return out;
  }

  void Reset(SocketHandle next = kInvalidSocket) noexcept {
    if (fd_ != kInvalidSocket) {
      CloseSocketHandle(fd_);
    }
    fd_ = next;
  }

private:
  SocketHandle fd_ = kInvalidSocket;
};

struct AddrInfoGuard {
  struct addrinfo* ptr = nullptr;
  AddrInfoGuard() = default;
  AddrInfoGuard(const AddrInfoGuard&) = delete;
  AddrInfoGuard& operator=(const AddrInfoGuard&) = delete;
  ~AddrInfoGuard() {
    if (ptr != nullptr) {
      freeaddrinfo(ptr);
      ptr = nullptr;
    }
  }
};

// ---- RESP2 / RESP3 Wire Helpers -------------------------------------------

} // namespace

// ---- RedisClient Transport & Execution -------------------------------------

RedisClient::~RedisClient() {
  Disconnect();
}

bool RedisClient::IsConnected() const {
  return static_cast<SocketHandle>(sock_) != kInvalidSocket;
}

void RedisClient::Disconnect() {
  const SocketHandle handle = static_cast<SocketHandle>(sock_);
  if (handle != kInvalidSocket) {
    CloseSocketHandle(handle);
#if defined(_WIN32)
    sock_ = static_cast<std::uintptr_t>(kInvalidSocket);
#else
    sock_ = kInvalidSocket;
#endif
  }
  recv_buf_.clear();
}

bool RedisClient::Connect(const RedisEndpoint& endpoint, std::string& error_out) {
  Disconnect();
  endpoint_ = endpoint;
  current_db_ = std::max(0, endpoint.db);
  return OpenSocketAndHandshake(error_out);
}

bool RedisClient::OpenSocketAndHandshake(std::string& error_out) {
  Disconnect();
  EnsureWinSockInitialized();

  struct addrinfo hints {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  const std::string port_str = std::to_string(endpoint_.port);
  AddrInfoGuard res;
  const int gai = getaddrinfo(endpoint_.host.c_str(), port_str.c_str(), &hints, &res.ptr);
  if (gai != 0 || res.ptr == nullptr) {
    error_out = TrFmt("Failed to resolve host ({0}): {1}", "解析主机失败 ({0}): {1}",
                      {endpoint_.host, GaiErrorText(gai)});
    return false;
  }

  UniqueSocket connected;
  std::string last_err(Tr("Unable to connect to Redis server", "无法连接到 Redis 服务器"));
  for (struct addrinfo* p = res.ptr; p != nullptr; p = p->ai_next) {
    UniqueSocket fd(socket(p->ai_family, p->ai_socktype, p->ai_protocol));
    if (!fd.Valid()) {
      last_err = LastSocketErrorText();
      continue;
    }

    (void)SetNonBlocking(fd.Get(), true);
    const int rc = ::connect(fd.Get(), p->ai_addr, static_cast<int>(p->ai_addrlen));
    if (rc != 0) {
      const int err_code = CurrentSocketError();
      if (!IsWouldBlockError(err_code)) {
        last_err = LastSocketErrorText(err_code);
        continue;
      }
      std::string wait_err;
      if (!WaitConnectReady(fd.Get(), kConnectTimeoutMs, wait_err)) {
        last_err = wait_err;
        continue;
      }
    }

    if (!SetNonBlocking(fd.Get(), false)) {
      last_err = LastSocketErrorText();
      continue;
    }
    ApplySocketTimeouts(fd.Get(), kSocketIoTimeoutMs);
    connected.Reset(fd.Release());
    break;
  }

  if (!connected.Valid()) {
    error_out = TrFmt("Failed to connect to Redis ({0}:{1}): {2}",
                      "连接 Redis 失败 ({0}:{1}): {2}",
                      {endpoint_.host, port_str, last_err});
    return false;
  }

#if defined(_WIN32)
  sock_ = static_cast<std::uintptr_t>(connected.Release());
#else
  sock_ = connected.Release();
#endif

  const auto run_direct = [&](const std::vector<std::string>& cmd, RedisValue& out) -> bool {
    std::vector<RedisValue> replies;
    bool received_any = false;
    if (!ExecuteBatchRaw({cmd}, replies, received_any, error_out) || replies.empty()) {
      return false;
    }
    out = std::move(replies.front());
    return true;
  };

  const bool has_custom_user =
      !endpoint_.username.empty() && !detail::RedisEqualsIgnoreCase(endpoint_.username, "default");
  if (!endpoint_.password.empty() || has_custom_user) {
    RedisValue auth_reply;
    if (!endpoint_.username.empty()) {
      if (!run_direct({"AUTH", endpoint_.username, endpoint_.password}, auth_reply)) {
        Disconnect();
        return false;
      }
      if (auth_reply.IsError() && !endpoint_.password.empty() &&
          detail::RedisLowerAscii(auth_reply.str).find("wrong number of arguments") != std::string::npos) {
        if (!run_direct({"AUTH", endpoint_.password}, auth_reply)) {
          Disconnect();
          return false;
        }
      }
    } else {
      if (!run_direct({"AUTH", endpoint_.password}, auth_reply)) {
        Disconnect();
        return false;
      }
    }
    if (auth_reply.IsError()) {
      error_out = TrFmt("Redis authentication failed: {0}", "Redis 认证失败: {0}",
                        {auth_reply.str});
      Disconnect();
      return false;
    }
  }

  if (current_db_ > 0) {
    RedisValue sel_reply;
    if (!run_direct({"SELECT", std::to_string(current_db_)}, sel_reply)) {
      Disconnect();
      return false;
    }
    if (sel_reply.IsError()) {
      error_out = TrFmt("Failed to switch database db{0}: {1}", "切换数据库 db{0} 失败: {1}",
                        {std::to_string(current_db_), sel_reply.str});
      Disconnect();
      return false;
    }
  }

  RedisValue ping_reply;
  if (!run_direct({"PING"}, ping_reply)) {
    Disconnect();
    return false;
  }
  if (ping_reply.IsError()) {
    error_out = TrFmt("Redis PING failed: {0}", "Redis PING 失败: {0}", {ping_reply.str});
    Disconnect();
    return false;
  }

  return true;
}

bool RedisClient::SendAll(std::string_view payload, std::string& error_out) {
  const SocketHandle fd = static_cast<SocketHandle>(sock_);
  if (fd == kInvalidSocket) {
    error_out = Tr("Redis connection not established", "Redis 连接未建立");
    return false;
  }
  std::size_t sent_total = 0;
  while (sent_total < payload.size()) {
#if defined(_WIN32)
    const int n = ::send(fd, payload.data() + sent_total,
                         static_cast<int>(payload.size() - sent_total), 0);
#elif defined(MSG_NOSIGNAL)
    const ssize_t n = ::send(fd, payload.data() + sent_total,
                             payload.size() - sent_total, MSG_NOSIGNAL);
#else
    const ssize_t n = ::send(fd, payload.data() + sent_total, payload.size() - sent_total, 0);
#endif
    if (n > 0) {
      sent_total += static_cast<std::size_t>(n);
      continue;
    }
    const int code = CurrentSocketError();
#if !defined(_WIN32)
    if (n < 0 && code == EINTR) {
      continue;
    }
#endif
    error_out = IsTimedOutError(code)
                    ? std::string(Tr("Timed out sending Redis request", "发送 Redis 请求超时"))
                    : TrFmt("Failed to send Redis request: {0}", "发送 Redis 请求失败: {0}",
                            {LastSocketErrorText(code)});
    return false;
  }
  return true;
}

bool RedisClient::RecvSome(std::string& error_out) {
  const SocketHandle fd = static_cast<SocketHandle>(sock_);
  if (fd == kInvalidSocket) {
    error_out = Tr("Redis connection not established", "Redis 连接未建立");
    return false;
  }
  char buf[16384];
  for (;;) {
#if defined(_WIN32)
    const int n = ::recv(fd, buf, static_cast<int>(sizeof(buf)), 0);
#else
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
#endif
    if (n > 0) {
      recv_buf_.append(buf, static_cast<std::size_t>(n));
      return true;
    }
    if (n == 0) {
      error_out = Tr("Redis server closed connection", "Redis 服务器已关闭连接");
      return false;
    }
    const int code = CurrentSocketError();
#if !defined(_WIN32)
    if (code == EINTR) {
      continue;
    }
#endif
    error_out = IsTimedOutError(code)
                    ? std::string(Tr("Timed out reading Redis reply", "读取 Redis 响应超时"))
                    : TrFmt("Failed to read Redis reply: {0}", "读取 Redis 响应失败: {0}",
                            {LastSocketErrorText(code)});
    return false;
  }
}

bool RedisClient::ExecuteBatchRaw(const std::vector<std::vector<std::string>>& commands,
                                  std::vector<RedisValue>& out, bool& received_any,
                                  std::string& error_out) {
  out.clear();
  received_any = false;
  if (commands.empty()) {
    return true;
  }
  std::string payload;
  for (const auto& cmd : commands) {
    payload += EncodeRespCommand(cmd);
  }
  if (!SendAll(payload, error_out)) {
    return false;
  }

  out.reserve(commands.size());
  for (std::size_t idx = 0; idx < commands.size(); ++idx) {
    for (;;) {
      std::size_t consumed = 0;
      std::string parse_err;
      RedisValue value;
      if (TryParseRespValue(recv_buf_, consumed, value, parse_err)) {
        received_any = true;
        recv_buf_.erase(0, consumed);
        out.push_back(std::move(value));
        break;
      }
      if (!parse_err.empty()) {
        error_out = parse_err;
        Disconnect();
        return false;
      }
      if (!RecvSome(error_out)) {
        return false;
      }
      received_any = true;
    }
  }
  return true;
}

bool RedisClient::Pipeline(const std::vector<std::vector<std::string>>& commands,
                           std::vector<RedisValue>& out, std::string& error_out) {
  out.clear();
  if (commands.empty()) {
    return true;
  }
  if (!IsConnected() && !OpenSocketAndHandshake(error_out)) {
    return false;
  }
  bool received_any = false;
  if (ExecuteBatchRaw(commands, out, received_any, error_out)) {
    return true;
  }
  if (!received_any) {
    std::string reconnect_err;
    if (OpenSocketAndHandshake(reconnect_err)) {
      bool retry_received = false;
      if (ExecuteBatchRaw(commands, out, retry_received, error_out)) {
        return true;
      }
    }
  }
  Disconnect();
  return false;
}

bool RedisClient::Command(const std::vector<std::string>& args, RedisValue& out,
                          std::string& error_out) {
  std::vector<RedisValue> replies;
  if (!Pipeline({args}, replies, error_out) || replies.empty()) {
    return false;
  }
  out = std::move(replies.front());
  return true;
}

bool RedisClient::EnsureDb(int db, std::string& error_out) {
  if (db < 0) {
    error_out = Tr("Invalid Redis database index", "无效的 Redis 数据库编号");
    return false;
  }
  if (IsConnected() && current_db_ == db) {
    return true;
  }
  const int prev_db = current_db_;
  current_db_ = db;
  if (!IsConnected()) {
    if (!OpenSocketAndHandshake(error_out)) {
      current_db_ = prev_db;
      return false;
    }
    return true;
  }
  RedisValue reply;
  if (!Command({"SELECT", std::to_string(db)}, reply, error_out)) {
    current_db_ = prev_db;
    return false;
  }
  if (reply.IsError()) {
    current_db_ = prev_db;
    error_out = TrFmt("Failed to switch database db{0}: {1}", "切换数据库 db{0} 失败: {1}",
                      {std::to_string(db), reply.str});
    return false;
  }
  return true;
}

} // namespace dbpp
