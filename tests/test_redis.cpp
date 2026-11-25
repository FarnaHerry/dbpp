#include "adbc_session.h"
#include "cli.h"
#include "model.h"
#include "redis_client.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    ++g_failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

std::string UpperAscii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const unsigned char c : text) {
    out.push_back(static_cast<char>(std::toupper(c)));
  }
  return out;
}

bool GlobMatch(std::string_view pattern, std::string_view text) {
  std::size_t p = 0;
  std::size_t t = 0;
  std::size_t star = std::string_view::npos;
  std::size_t match = 0;
  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
      ++p;
      ++t;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      match = t;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      t = ++match;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') {
    ++p;
  }
  return p == pattern.size();
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

// ---- In-process loopback RESP2 mock server --------------------------------

class MockRedisServer {
public:
#if defined(_WIN32)
  using Sock = SOCKET;
  static constexpr Sock kBadSock = INVALID_SOCKET;
#else
  using Sock = int;
  static constexpr Sock kBadSock = -1;
#endif

  struct Entry {
    std::string type = "string";
    std::int64_t ttl = -1;
    std::string str_val;
    std::map<std::string, std::string> hash_val;
    std::vector<std::string> list_val;
    std::set<std::string> set_val;
    std::vector<std::pair<std::string, double>> zset_val;
  };

  explicit MockRedisServer(std::string require_password = {}, std::string require_user = {})
      : required_password_(std::move(require_password)),
        required_user_(std::move(require_user)) {
    SeedInitialData();
    Start();
  }

  ~MockRedisServer() { Stop(); }

  [[nodiscard]] std::uint16_t Port() const { return port_; }

private:
  static void CloseSock(Sock s) {
    if (s != kBadSock) {
#if defined(_WIN32)
      closesocket(s);
#else
      close(s);
#endif
    }
  }

  static std::string Simple(std::string_view s) {
    return "+" + std::string(s) + "\r\n";
  }
  static std::string Error(std::string_view s) {
    return "-" + std::string(s) + "\r\n";
  }
  static std::string Int(std::int64_t v) {
    return ":" + std::to_string(v) + "\r\n";
  }
  static std::string Bulk(std::string_view s) {
    return "$" + std::to_string(s.size()) + "\r\n" + std::string(s) + "\r\n";
  }
  static std::string NullBulk() { return "$-1\r\n"; }

  static bool SendAll(Sock s, std::string_view data) {
    std::size_t off = 0;
    while (off < data.size()) {
#if defined(_WIN32)
      const int n = ::send(s, data.data() + off, static_cast<int>(data.size() - off), 0);
#elif defined(MSG_NOSIGNAL)
      const ssize_t n = ::send(s, data.data() + off, data.size() - off, MSG_NOSIGNAL);
#else
      const ssize_t n = ::send(s, data.data() + off, data.size() - off, 0);
#endif
      if (n <= 0) {
        return false;
      }
      off += static_cast<std::size_t>(n);
    }
    return true;
  }

  void SeedInitialData() {
    Entry greeting;
    greeting.type = "string";
    greeting.ttl = 120;
    greeting.str_val = "hello redis";
    dbs_[0]["greeting"] = std::move(greeting);

    Entry user100;
    user100.type = "hash";
    user100.hash_val = {{"name", "Alice"}, {"role", "admin"}};
    dbs_[0]["user:100"] = std::move(user100);

    Entry user200;
    user200.type = "list";
    user200.list_val = {"step1", "step2"};
    dbs_[0]["user:200"] = std::move(user200);

    Entry tags;
    tags.type = "set";
    tags.set_val = {"alpha", "beta"};
    dbs_[0]["tags"] = std::move(tags);

    Entry lb;
    lb.type = "zset";
    lb.zset_val = {{"alice", 10.0}, {"bob", 25.5}};
    dbs_[0]["leaderboard"] = std::move(lb);

    Entry token;
    token.type = "string";
    token.ttl = 60;
    token.str_val = "xyz";
    dbs_[1]["cache:token"] = std::move(token);
  }

  void Start() {
#if defined(_WIN32)
    WSADATA wsa{};
    (void)WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    const int one = 1;
    (void)setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&one), sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    (void)::bind(listen_sock_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    (void)getsockname(listen_sock_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    (void)::listen(listen_sock_, 8);

    worker_ = std::thread([this] { AcceptLoop(); });
  }

  void Stop() {
    stop_.store(true);
    if (listen_sock_ != kBadSock) {
      CloseSock(listen_sock_);
      listen_sock_ = kBadSock;
    }
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void AcceptLoop() {
    struct ClientState {
      Sock fd = kBadSock;
      bool authed = false;
      int cur_db = 0;
      std::string buf;
    };
    std::vector<ClientState> clients;

    while (!stop_.load()) {
#if defined(_WIN32)
      std::vector<WSAPOLLFD> pfds;
      pfds.reserve(1 + clients.size());
      pfds.push_back(WSAPOLLFD{.fd = listen_sock_, .events = POLLRDNORM, .revents = 0});
      for (const ClientState& c : clients) {
        pfds.push_back(WSAPOLLFD{.fd = c.fd, .events = POLLRDNORM, .revents = 0});
      }
      const int rc = WSAPoll(pfds.data(), static_cast<ULONG>(pfds.size()), 50);
#else
      std::vector<pollfd> pfds;
      pfds.reserve(1 + clients.size());
      pfds.push_back(pollfd{.fd = listen_sock_, .events = POLLIN, .revents = 0});
      for (const ClientState& c : clients) {
        pfds.push_back(pollfd{.fd = c.fd, .events = POLLIN, .revents = 0});
      }
      const int rc = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 50);
#endif
      if (stop_.load()) {
        break;
      }
      if (rc <= 0) {
        continue;
      }

      if (pfds[0].revents != 0) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        const Sock fd = ::accept(listen_sock_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (fd != kBadSock) {
          clients.push_back(ClientState{
              .fd = fd,
              .authed = required_password_.empty(),
              .cur_db = 0,
              .buf = {},
          });
        }
      }

      char tmp[4096];
      for (std::size_t idx = clients.size(); idx-- > 0;) {
        if (idx + 1 >= pfds.size() || pfds[idx + 1].revents == 0) {
          continue;
        }
        ClientState& c = clients[idx];
#if defined(_WIN32)
        const int n = ::recv(c.fd, tmp, static_cast<int>(sizeof(tmp)), 0);
#else
        const ssize_t n = ::recv(c.fd, tmp, sizeof(tmp), 0);
#endif
        if (n <= 0) {
          CloseSock(c.fd);
          clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(idx));
          continue;
        }
        c.buf.append(tmp, static_cast<std::size_t>(n));
        bool drop = false;
        for (;;) {
          std::size_t consumed = 0;
          std::string err;
          dbpp::RedisValue cmd_val;
          if (!dbpp::TryParseRespValue(c.buf, consumed, cmd_val, err)) {
            if (!err.empty()) {
              drop = true;
            }
            break;
          }
          c.buf.erase(0, consumed);
          std::vector<std::string> args;
          if (cmd_val.kind == dbpp::RedisValueKind::Array) {
            for (const auto& el : cmd_val.elements) {
              args.push_back(el.ToDisplayString());
            }
          }
          const std::string reply = DispatchCommand(args, c.authed, c.cur_db);
          if (!SendAll(c.fd, reply)) {
            drop = true;
            break;
          }
        }
        if (drop) {
          CloseSock(c.fd);
          clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(idx));
        }
      }
    }

    for (const ClientState& c : clients) {
      CloseSock(c.fd);
    }
  }

  std::string DispatchCommand(const std::vector<std::string>& args, bool& authed, int& cur_db) {
    if (args.empty()) {
      return Error("ERR empty command");
    }
    const std::string cmd = UpperAscii(args[0]);
    if (cmd == "AUTH") {
      if (args.size() == 2) {
        if (args[1] == required_password_ && required_user_.empty()) {
          authed = true;
          return Simple("OK");
        }
      } else if (args.size() == 3) {
        if (args[1] == (required_user_.empty() ? "default" : required_user_) &&
            args[2] == required_password_) {
          authed = true;
          return Simple("OK");
        }
      }
      return Error("WRONGPASS invalid username-password pair");
    }
    if (!authed) {
      return Error("NOAUTH Authentication required.");
    }
    if (cmd == "PING") {
      return args.size() > 1 ? Bulk(args[1]) : Simple("PONG");
    }
    if (cmd == "SELECT" && args.size() == 2) {
      int db = 0;
      const auto [ptr, ec] =
          std::from_chars(args[1].data(), args[1].data() + args[1].size(), db);
      if (ec != std::errc{} || ptr != args[1].data() + args[1].size() || db < 0 || db >= 16) {
        return Error("ERR DB index is out of range");
      }
      cur_db = db;
      return Simple("OK");
    }

    std::lock_guard<std::mutex> lock(mu_);
    auto& store = dbs_[cur_db];

    if (cmd == "CONFIG" && args.size() == 3 && UpperAscii(args[1]) == "GET" &&
        args[2] == "databases") {
      return "*2\r\n" + Bulk("databases") + Bulk("16");
    }
    if (cmd == "INFO") {
      const std::string sec = args.size() > 1 ? UpperAscii(args[1]) : std::string("ALL");
      std::ostringstream oss;
      if (sec == "ALL" || sec == "SERVER") {
        oss << "# Server\r\nredis_version:7.2.0\r\ntcp_port:" << port_ << "\r\n";
      }
      if (sec == "ALL" || sec == "KEYSPACE") {
        oss << "# Keyspace\r\n";
        for (const auto& [idx, m] : dbs_) {
          if (!m.empty()) {
            oss << "db" << idx << ":keys=" << m.size() << ",expires=0,avg_ttl=0\r\n";
          }
        }
      }
      return Bulk(oss.str());
    }
    if (cmd == "DBSIZE") {
      return Int(static_cast<std::int64_t>(store.size()));
    }
    if (cmd == "SCAN" && args.size() >= 2) {
      std::string pattern = "*";
      for (std::size_t i = 2; i + 1 < args.size(); i += 2) {
        if (UpperAscii(args[i]) == "MATCH") {
          pattern = args[i + 1];
        }
      }
      std::vector<std::string> matched;
      for (const auto& [k, e] : store) {
        (void)e;
        if (GlobMatch(pattern, k)) {
          matched.push_back(k);
        }
      }
      std::string arr = "*" + std::to_string(matched.size()) + "\r\n";
      for (const std::string& k : matched) {
        arr += Bulk(k);
      }
      return "*2\r\n" + Bulk("0") + arr;
    }
    if (cmd == "TYPE" && args.size() == 2) {
      const auto it = store.find(args[1]);
      return Simple(it == store.end() ? "none" : it->second.type);
    }
    if ((cmd == "TTL" || cmd == "PTTL") && args.size() == 2) {
      const auto it = store.find(args[1]);
      if (it == store.end()) return Int(-2);
      if (it->second.ttl < 0) return Int(-1);
      return Int(cmd == "PTTL" ? it->second.ttl * 1000 : it->second.ttl);
    }
    if (cmd == "EXISTS" && args.size() >= 2) {
      std::int64_t count = 0;
      for (std::size_t i = 1; i < args.size(); ++i) {
        if (store.contains(args[i])) ++count;
      }
      return Int(count);
    }
    if (cmd == "DEL" && args.size() >= 2) {
      std::int64_t removed = 0;
      for (std::size_t i = 1; i < args.size(); ++i) {
        removed += static_cast<std::int64_t>(store.erase(args[i]));
      }
      return Int(removed);
    }
    if ((cmd == "EXPIRE" || cmd == "PEXPIRE") && args.size() == 3) {
      const auto it = store.find(args[1]);
      if (it == store.end()) return Int(0);
      std::int64_t v = 0;
      (void)std::from_chars(args[2].data(), args[2].data() + args[2].size(), v);
      it->second.ttl = (cmd == "PEXPIRE") ? std::max<std::int64_t>(1, (v + 999) / 1000) : v;
      return Int(1);
    }
    if (cmd == "PERSIST" && args.size() == 2) {
      const auto it = store.find(args[1]);
      if (it == store.end() || it->second.ttl < 0) return Int(0);
      it->second.ttl = -1;
      return Int(1);
    }
    if (cmd == "RENAMENX" && args.size() == 3) {
      const auto it = store.find(args[1]);
      if (it == store.end()) return Error("ERR no such key");
      if (store.contains(args[2])) return Int(0);
      Entry moved = std::move(it->second);
      store.erase(it);
      store[args[2]] = std::move(moved);
      return Int(1);
    }
    if (cmd == "STRLEN" && args.size() == 2) {
      const auto it = store.find(args[1]);
      return Int(it == store.end() ? 0 : static_cast<std::int64_t>(it->second.str_val.size()));
    }
    if (cmd == "GET" && args.size() == 2) {
      const auto it = store.find(args[1]);
      return it == store.end() ? NullBulk() : Bulk(it->second.str_val);
    }
    if (cmd == "SET" && args.size() >= 3) {
      bool keepttl = false;
      for (std::size_t i = 3; i < args.size(); ++i) {
        if (UpperAscii(args[i]) == "KEEPTTL") keepttl = true;
      }
      std::int64_t ttl = -1;
      if (keepttl && store.contains(args[1])) {
        ttl = store[args[1]].ttl;
      }
      Entry e;
      e.type = "string";
      e.ttl = ttl;
      e.str_val = args[2];
      store[args[1]] = std::move(e);
      return Simple("OK");
    }
    if (cmd == "HLEN" && args.size() == 2) {
      const auto it = store.find(args[1]);
      return Int(it == store.end() ? 0 : static_cast<std::int64_t>(it->second.hash_val.size()));
    }
    if (cmd == "HGETALL" && args.size() == 2) {
      const auto it = store.find(args[1]);
      if (it == store.end()) return "*0\r\n";
      std::string out = "*" + std::to_string(it->second.hash_val.size() * 2) + "\r\n";
      for (const auto& [f, v] : it->second.hash_val) {
        out += Bulk(f) + Bulk(v);
      }
      return out;
    }
    if (cmd == "HSET" && args.size() >= 4 && (args.size() % 2) == 0) {
      Entry& e = store[args[1]];
      e.type = "hash";
      std::int64_t added = 0;
      for (std::size_t i = 2; i + 1 < args.size(); i += 2) {
        if (!e.hash_val.contains(args[i])) ++added;
        e.hash_val[args[i]] = args[i + 1];
      }
      return Int(added);
    }
    if (cmd == "LLEN" && args.size() == 2) {
      const auto it = store.find(args[1]);
      return Int(it == store.end() ? 0 : static_cast<std::int64_t>(it->second.list_val.size()));
    }
    if (cmd == "LRANGE" && args.size() == 4) {
      const auto it = store.find(args[1]);
      if (it == store.end()) return "*0\r\n";
      const auto& v = it->second.list_val;
      std::string out = "*" + std::to_string(v.size()) + "\r\n";
      for (const auto& item : v) out += Bulk(item);
      return out;
    }
    if (cmd == "RPUSH" && args.size() >= 3) {
      Entry& e = store[args[1]];
      e.type = "list";
      for (std::size_t i = 2; i < args.size(); ++i) e.list_val.push_back(args[i]);
      return Int(static_cast<std::int64_t>(e.list_val.size()));
    }
    if (cmd == "SCARD" && args.size() == 2) {
      const auto it = store.find(args[1]);
      return Int(it == store.end() ? 0 : static_cast<std::int64_t>(it->second.set_val.size()));
    }
    if (cmd == "SMEMBERS" && args.size() == 2) {
      const auto it = store.find(args[1]);
      if (it == store.end()) return "*0\r\n";
      const auto& s = it->second.set_val;
      std::string out = "*" + std::to_string(s.size()) + "\r\n";
      for (const auto& item : s) out += Bulk(item);
      return out;
    }
    if (cmd == "SADD" && args.size() >= 3) {
      Entry& e = store[args[1]];
      e.type = "set";
      std::int64_t added = 0;
      for (std::size_t i = 2; i < args.size(); ++i) {
        if (e.set_val.insert(args[i]).second) ++added;
      }
      return Int(added);
    }
    if (cmd == "ZCARD" && args.size() == 2) {
      const auto it = store.find(args[1]);
      return Int(it == store.end() ? 0 : static_cast<std::int64_t>(it->second.zset_val.size()));
    }
    if (cmd == "ZRANGE" && args.size() >= 4) {
      const auto it = store.find(args[1]);
      if (it == store.end()) return "*0\r\n";
      const bool withscores = args.size() >= 5 && UpperAscii(args[4]) == "WITHSCORES";
      const auto& z = it->second.zset_val;
      std::string out = "*" + std::to_string(withscores ? z.size() * 2 : z.size()) + "\r\n";
      for (const auto& [m, score] : z) {
        out += Bulk(m);
        if (withscores) {
          std::ostringstream oss;
          oss << score;
          out += Bulk(oss.str());
        }
      }
      return out;
    }
    if (cmd == "ZADD" && args.size() >= 4 && (args.size() % 2) == 0) {
      Entry& e = store[args[1]];
      e.type = "zset";
      std::int64_t added = 0;
      for (std::size_t i = 2; i + 1 < args.size(); i += 2) {
        const double sc = std::strtod(args[i].c_str(), nullptr);
        e.zset_val.emplace_back(args[i + 1], sc);
        ++added;
      }
      std::ranges::sort(e.zset_val, [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second < b.second : a.first < b.first;
      });
      return Int(added);
    }
    return Error("ERR unknown command '" + args[0] + "'");
  }

  std::string required_password_;
  std::string required_user_;
  Sock listen_sock_ = kBadSock;
  std::uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread worker_;
  std::mutex mu_;
  std::map<int, std::map<std::string, Entry>> dbs_;
};

// ---- Test Cases -----------------------------------------------------------

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

  Expect(dbpp::IsRedisReadCommand("INFO server"), "INFO must be read-only");
  Expect(dbpp::IsRedisReadCommand("CONFIG GET databases"), "CONFIG GET must be read-only");
  Expect(!dbpp::IsRedisReadCommand("CONFIG SET save \"\""), "CONFIG SET must not be read-only");
  Expect(!dbpp::IsRedisReadCommand("SET foo bar"), "SET must not be read-only");
}

void TestRedisSessionCrudAndCli(const fs::path& dir) {
  MockRedisServer server("secret123", "alice");
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

  const fs::path profiles_path = dir / "connections.tsv";
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
}

} // namespace

int main() {
  ScopedTempDir temp;
  TestUriAndParsingHelpers();
  TestRedisSessionCrudAndCli(temp.path);

  if (g_failures != 0) {
    std::cerr << g_failures << " Redis test assertion(s) failed\n";
    return 1;
  }
  std::cout << "test_redis passed\n";
  return 0;
}
