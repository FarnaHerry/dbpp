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

#include <cstdlib>
#include <cstring>
#include "test_redis_helpers.h"

import std;
import dbpp.redis;

namespace {

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


} // namespace

namespace dbpp_test {

struct MockRedisServer::Impl {
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

  explicit Impl(std::string require_password = {}, std::string require_user = {})
      : required_password_(std::move(require_password)),
        required_user_(std::move(require_user)) {
    SeedInitialData();
    Start();
  }

  ~Impl() { Stop(); }

  [[nodiscard]] std::uint16_t Port() const { return port_; }
  void SetLegacyAuthOnly(bool value) { legacy_auth_only_.store(value); }
  void SetDisableInfo(bool value) { disable_info_.store(value); }
  void SetDropOnConfig(bool value) { drop_on_config_.store(value); }
  void SetScanStepSize(std::size_t step) { scan_step_size_.store(step); }
  void SetDisableScanType(bool value) { disable_scan_type_.store(value); }
  [[nodiscard]] std::size_t CommandCount(std::string_view cmd) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = cmd_counts_.find(UpperAscii(cmd));
    return it == cmd_counts_.end() ? 0 : it->second;
  }
  void ResetCommandCounts() {
    std::lock_guard<std::mutex> lock(mu_);
    cmd_counts_.clear();
  }

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
          if (!args.empty() && UpperAscii(args[0]) == "CONFIG" && drop_on_config_.load()) {
            drop = true;
            break;
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
        if (legacy_auth_only_.load()) {
          return Error("ERR wrong number of arguments for 'auth' command");
        }
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
    ++cmd_counts_[cmd];
    auto& store = dbs_[cur_db];

    const auto parse_slice_bounds = [](std::int64_t len, const std::string& s_arg,
                                       const std::string& e_arg, std::size_t& out_begin,
                                       std::size_t& out_end_exclusive) -> bool {
      if (len <= 0) {
        return false;
      }
      std::int64_t s = 0;
      std::int64_t e = len - 1;
      (void)std::from_chars(s_arg.data(), s_arg.data() + s_arg.size(), s);
      (void)std::from_chars(e_arg.data(), e_arg.data() + e_arg.size(), e);
      if (s < 0) s += len;
      if (e < 0) e += len;
      s = std::max<std::int64_t>(0, s);
      e = std::min<std::int64_t>(len - 1, e);
      if (s > e || s >= len) {
        return false;
      }
      out_begin = static_cast<std::size_t>(s);
      out_end_exclusive = static_cast<std::size_t>(e + 1);
      return true;
    };

    const auto parse_scan_opts = [](const std::vector<std::string>& cmd_args, std::size_t opt_idx,
                                    std::string& pattern, std::size_t& count) {
      for (std::size_t i = opt_idx; i + 1 < cmd_args.size(); i += 2) {
        const std::string key = UpperAscii(cmd_args[i]);
        if (key == "MATCH") {
          pattern = cmd_args[i + 1];
        } else if (key == "COUNT") {
          std::size_t parsed = 0;
          const auto [ptr, ec] = std::from_chars(
              cmd_args[i + 1].data(), cmd_args[i + 1].data() + cmd_args[i + 1].size(), parsed);
          if (ec == std::errc{} && ptr == cmd_args[i + 1].data() + cmd_args[i + 1].size() &&
              parsed > 0) {
            count = parsed;
          }
        }
      }
    };

    if (cmd == "CONFIG" && args.size() == 3 && UpperAscii(args[1]) == "GET" &&
        args[2] == "databases") {
      return "*2\r\n" + Bulk("databases") + Bulk("16");
    }
    if (cmd == "INFO") {
      if (disable_info_.load()) {
        return Error("NOPERM User has no permissions to run the 'info' command");
      }
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
      std::size_t count = 500;
      std::string type_filter;
      for (std::size_t i = 2; i + 1 < args.size(); i += 2) {
        const std::string opt = UpperAscii(args[i]);
        if (opt == "MATCH") {
          pattern = args[i + 1];
        } else if (opt == "TYPE") {
          type_filter = UpperAscii(args[i + 1]);
        } else if (opt == "COUNT") {
          std::size_t parsed = 0;
          const auto [ptr, ec] = std::from_chars(
              args[i + 1].data(), args[i + 1].data() + args[i + 1].size(), parsed);
          if (ec == std::errc{} && ptr == args[i + 1].data() + args[i + 1].size() &&
              parsed > 0) {
            count = parsed;
          }
        }
      }
      if (!type_filter.empty() && disable_scan_type_.load()) {
        return Error("ERR syntax error");
      }
      const auto matches_entry = [&](const std::string& k, const Entry& e) {
        if (!GlobMatch(pattern, k)) {
          return false;
        }
        return type_filter.empty() || UpperAscii(e.type) == type_filter;
      };
      const std::size_t step = scan_step_size_.load();
      if (step == 0) {
        std::vector<std::string> matched;
        for (const auto& [k, e] : store) {
          if (matches_entry(k, e)) {
            matched.push_back(k);
          }
        }
        std::string arr = "*" + std::to_string(matched.size()) + "\r\n";
        for (const std::string& k : matched) {
          arr += Bulk(k);
        }
        return "*2\r\n" + Bulk("0") + arr;
      }

      std::size_t cursor_idx = 0;
      (void)std::from_chars(args[1].data(), args[1].data() + args[1].size(), cursor_idx);
      std::vector<std::string> all_keys;
      all_keys.reserve(store.size());
      for (const auto& [k, e] : store) {
        (void)e;
        all_keys.push_back(k);
      }
      const std::size_t start = std::min(cursor_idx, all_keys.size());
      const std::size_t end = std::min(start + step, all_keys.size());
      std::vector<std::string> matched;
      for (std::size_t i = start; i < end; ++i) {
        const auto it = store.find(all_keys[i]);
        if (it != store.end() && matches_entry(all_keys[i], it->second)) {
          matched.push_back(all_keys[i]);
        }
      }
      const std::string next_cursor = end < all_keys.size() ? std::to_string(end) : std::string("0");
      std::string arr = "*" + std::to_string(matched.size()) + "\r\n";
      for (const std::string& k : matched) {
        arr += Bulk(k);
      }
      return "*2\r\n" + Bulk(next_cursor) + arr;
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
    if (cmd == "GETRANGE" && args.size() == 4) {
      const auto it = store.find(args[1]);
      if (it == store.end() || it->second.str_val.empty()) {
        return Bulk("");
      }
      std::size_t begin = 0;
      std::size_t end_excl = 0;
      if (!parse_slice_bounds(static_cast<std::int64_t>(it->second.str_val.size()), args[2],
                              args[3], begin, end_excl)) {
        return Bulk("");
      }
      return Bulk(std::string_view(it->second.str_val).substr(begin, end_excl - begin));
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
    if (cmd == "HSCAN" && args.size() >= 3) {
      const auto it = store.find(args[1]);
      if (it == store.end()) return "*2\r\n" + Bulk("0") + "*0\r\n";
      std::size_t cursor_idx = 0;
      (void)std::from_chars(args[2].data(), args[2].data() + args[2].size(), cursor_idx);
      std::string pattern = "*";
      std::size_t count = 10;
      parse_scan_opts(args, 3, pattern, count);
      const auto& h = it->second.hash_val;
      const std::size_t start = std::min(cursor_idx, h.size());
      const std::size_t end = std::min(start + count, h.size());
      std::vector<std::pair<std::string, std::string>> pairs;
      auto hit = std::next(h.begin(), static_cast<std::ptrdiff_t>(start));
      for (std::size_t i = start; i < end && hit != h.end(); ++i, ++hit) {
        if (GlobMatch(pattern, hit->first)) {
          pairs.emplace_back(hit->first, hit->second);
        }
      }
      const std::string next_cursor = end < h.size() ? std::to_string(end) : std::string("0");
      std::string arr = "*" + std::to_string(pairs.size() * 2) + "\r\n";
      for (const auto& [f, v] : pairs) {
        arr += Bulk(f) + Bulk(v);
      }
      return "*2\r\n" + Bulk(next_cursor) + arr;
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
      std::size_t begin = 0;
      std::size_t end_excl = 0;
      if (!parse_slice_bounds(static_cast<std::int64_t>(v.size()), args[2], args[3], begin,
                              end_excl)) {
        return "*0\r\n";
      }
      std::string out = "*" + std::to_string(end_excl - begin) + "\r\n";
      for (std::size_t i = begin; i < end_excl; ++i) out += Bulk(v[i]);
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
    if (cmd == "SSCAN" && args.size() >= 3) {
      const auto it = store.find(args[1]);
      if (it == store.end()) return "*2\r\n" + Bulk("0") + "*0\r\n";
      std::size_t cursor_idx = 0;
      (void)std::from_chars(args[2].data(), args[2].data() + args[2].size(), cursor_idx);
      std::string pattern = "*";
      std::size_t count = 10;
      parse_scan_opts(args, 3, pattern, count);
      const auto& s = it->second.set_val;
      const std::size_t start = std::min(cursor_idx, s.size());
      const std::size_t end = std::min(start + count, s.size());
      std::vector<std::string> items;
      auto sit = std::next(s.begin(), static_cast<std::ptrdiff_t>(start));
      for (std::size_t i = start; i < end && sit != s.end(); ++i, ++sit) {
        if (GlobMatch(pattern, *sit)) {
          items.push_back(*sit);
        }
      }
      const std::string next_cursor = end < s.size() ? std::to_string(end) : std::string("0");
      std::string arr = "*" + std::to_string(items.size()) + "\r\n";
      for (const std::string& item : items) {
        arr += Bulk(item);
      }
      return "*2\r\n" + Bulk(next_cursor) + arr;
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
      std::size_t begin = 0;
      std::size_t end_excl = 0;
      if (!parse_slice_bounds(static_cast<std::int64_t>(z.size()), args[2], args[3], begin,
                              end_excl)) {
        return "*0\r\n";
      }
      const std::size_t count = end_excl - begin;
      std::string out = "*" + std::to_string(withscores ? count * 2 : count) + "\r\n";
      for (std::size_t i = begin; i < end_excl; ++i) {
        out += Bulk(z[i].first);
        if (withscores) {
          std::ostringstream oss;
          oss << z[i].second;
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
  std::atomic<bool> legacy_auth_only_{false};
  std::atomic<bool> disable_info_{false};
  std::atomic<bool> drop_on_config_{false};
  std::atomic<std::size_t> scan_step_size_{0};
  std::atomic<bool> disable_scan_type_{false};
  Sock listen_sock_ = kBadSock;
  std::uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread worker_;
  mutable std::mutex mu_;
  std::map<std::string, std::size_t> cmd_counts_;
  std::map<int, std::map<std::string, Entry>> dbs_;
};

MockRedisServer::MockRedisServer(std::string require_password, std::string require_user)
    : impl_(std::make_unique<Impl>(std::move(require_password), std::move(require_user))) {}
MockRedisServer::~MockRedisServer() = default;
std::uint16_t MockRedisServer::Port() const { return impl_->Port(); }
void MockRedisServer::SetLegacyAuthOnly(bool value) { impl_->SetLegacyAuthOnly(value); }
void MockRedisServer::SetDisableInfo(bool value) { impl_->SetDisableInfo(value); }
void MockRedisServer::SetDropOnConfig(bool value) { impl_->SetDropOnConfig(value); }
void MockRedisServer::SetScanStepSize(std::size_t step) { impl_->SetScanStepSize(step); }
void MockRedisServer::SetDisableScanType(bool value) { impl_->SetDisableScanType(value); }
std::size_t MockRedisServer::CommandCount(std::string_view cmd) const {
  return impl_->CommandCount(cmd);
}
void MockRedisServer::ResetCommandCounts() { impl_->ResetCommandCounts(); }

} // namespace dbpp_test
