module;

#include <huxerui/file.h>
#include <huxerui/http.h>
#include <huxerui/task.h>
#include <toml++/toml.hpp>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>

module dbpp.driver_installer;

import std;
import dbpp.types;
import dbpp.i18n;
import dbpp.adbc;

namespace dbpp {

namespace {

std::string UnquoteScalar(std::string_view text) {
  text = detail::InstallerTrimAscii(text);
  if (text.size() >= 2) {
    const char first = text.front();
    const char last = text.back();
    if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
      text.remove_prefix(1);
      text.remove_suffix(1);
    }
  }
  return std::string(text);
}

std::string NormalizeSha256Impl(std::string_view text) {
  std::string cleaned = UnquoteScalar(text);
  const std::string lower = detail::InstallerLowerAscii(cleaned);
  if (lower.starts_with("sha256:")) {
    cleaned = UnquoteScalar(std::string_view(cleaned).substr(7));
  }
  if (cleaned.size() != 64) {
    return {};
  }
  for (char& c : cleaned) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (!std::isxdigit(uc)) {
      return {};
    }
    c = static_cast<char>(std::tolower(uc));
  }
  return cleaned;
}

bool SplitKeyValue(std::string_view text, std::string_view& key, std::string_view& value) {
  const std::size_t colon = text.find(':');
  if (colon == std::string_view::npos) {
    return false;
  }
  if (colon + 1 < text.size() && text[colon + 1] != ' ' && text[colon + 1] != '\t') {
    return false;
  }
  key = detail::InstallerTrimAscii(text.substr(0, colon));
  value = colon + 1 < text.size() ? detail::InstallerTrimAscii(text.substr(colon + 1)) : std::string_view{};
  return true;
}

std::string ResolvePackageUrl(std::string_view base_url, std::string_view url) {
  if (url.empty()) {
    return {};
  }
  const std::string lower = detail::InstallerLowerAscii(url);
  if (lower.starts_with("http://") || lower.starts_with("https://") ||
      lower.starts_with("file://")) {
    return std::string(url);
  }
  std::string_view base = base_url;
  while (base.ends_with('/')) {
    base.remove_suffix(1);
  }
  std::string_view rel = url;
  while (rel.starts_with('/')) {
    rel.remove_prefix(1);
  }
  if (base.empty()) {
    return std::string(rel);
  }
  return std::string(base) + "/" + std::string(rel);
}

struct SemVer {
  int major = 0;
  int minor = 0;
  int patch = 0;
  bool has_prerelease = false;
  std::string prerelease;
  bool valid = false;
};

SemVer ParseSemVer(std::string_view text) {
  SemVer ver;
  text = detail::InstallerTrimAscii(text);
  if (text.empty()) {
    return ver;
  }
  if (text.front() == 'v' || text.front() == 'V') {
    text.remove_prefix(1);
  }
  if (const std::size_t plus = text.find('+'); plus != std::string_view::npos) {
    text = text.substr(0, plus);
  }
  if (const std::size_t dash = text.find('-'); dash != std::string_view::npos) {
    ver.has_prerelease = true;
    ver.prerelease = std::string(text.substr(dash + 1));
    text = text.substr(0, dash);
  }

  auto parse_part = [](std::string_view part, int& out) -> bool {
    if (part.empty()) {
      return false;
    }
    const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), out);
    return ec == std::errc{} && end == part.data() + part.size() && out >= 0;
  };

  const std::size_t dot1 = text.find('.');
  if (dot1 == std::string_view::npos) {
    ver.valid = parse_part(text, ver.major);
    return ver;
  }
  if (!parse_part(text.substr(0, dot1), ver.major)) {
    return ver;
  }
  const std::string_view rest = text.substr(dot1 + 1);
  const std::size_t dot2 = rest.find('.');
  if (dot2 == std::string_view::npos) {
    ver.valid = parse_part(rest, ver.minor);
    return ver;
  }
  if (!parse_part(rest.substr(0, dot2), ver.minor)) {
    return ver;
  }
  ver.valid = parse_part(rest.substr(dot2 + 1), ver.patch);
  return ver;
}

std::vector<std::string_view> SplitDots(std::string_view text) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == '.') {
      parts.push_back(text.substr(start, i - start));
      start = i + 1;
    }
  }
  return parts;
}

int ComparePrerelease(std::string_view left, std::string_view right) {
  const auto a = SplitDots(left);
  const auto b = SplitDots(right);
  const std::size_t count = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < count; ++i) {
    if (a[i] == b[i]) {
      continue;
    }
    std::uint64_t na = 0;
    std::uint64_t nb = 0;
    const auto ra = std::from_chars(a[i].data(), a[i].data() + a[i].size(), na);
    const auto rb = std::from_chars(b[i].data(), b[i].data() + b[i].size(), nb);
    const bool a_num = ra.ec == std::errc{} && ra.ptr == a[i].data() + a[i].size();
    const bool b_num = rb.ec == std::errc{} && rb.ptr == b[i].data() + b[i].size();
    if (a_num && b_num) {
      return na < nb ? -1 : 1;
    }
    if (a_num != b_num) {
      return a_num ? -1 : 1;
    }
    return a[i] < b[i] ? -1 : 1;
  }
  if (a.size() != b.size()) {
    return a.size() < b.size() ? -1 : 1;
  }
  return 0;
}

int CompareSemVer(const SemVer& left, const SemVer& right) {
  if (left.valid != right.valid) {
    return left.valid ? 1 : -1;
  }
  if (!left.valid && !right.valid) {
    return 0;
  }
  // Prefer stable releases over prereleases when selecting default packages.
  if (left.has_prerelease != right.has_prerelease) {
    return left.has_prerelease ? -1 : 1;
  }
  if (left.major != right.major) {
    return left.major < right.major ? -1 : 1;
  }
  if (left.minor != right.minor) {
    return left.minor < right.minor ? -1 : 1;
  }
  if (left.patch != right.patch) {
    return left.patch < right.patch ? -1 : 1;
  }
  if (left.has_prerelease && right.has_prerelease) {
    return ComparePrerelease(left.prerelease, right.prerelease);
  }
  return 0;
}

int CoreDriverRank(std::string_view slug) {
  if (slug == "postgresql") return 0;
  if (slug == "mysql") return 1;
  if (slug == "mariadb") return 2;
  if (slug == "duckdb") return 3;
  if (slug == "mssql") return 4;
  if (slug == "clickhouse") return 5;
  if (slug == "flightsql") return 6;
  if (slug == "sqlite") return 7;
  return 8;
}

class Sha256Hasher {
public:
  void Update(const unsigned char* data, std::size_t len) {
    total_bits_ += static_cast<std::uint64_t>(len) * 8ULL;
    while (len > 0) {
      const std::size_t take = std::min(len, 64 - buf_len_);
      std::memcpy(buf_.data() + buf_len_, data, take);
      buf_len_ += take;
      data += take;
      len -= take;
      if (buf_len_ == 64) {
        Transform(buf_.data());
        buf_len_ = 0;
      }
    }
  }

  std::string FinalHex() {
    buf_[buf_len_++] = 0x80;
    if (buf_len_ > 56) {
      while (buf_len_ < 64) {
        buf_[buf_len_++] = 0x00;
      }
      Transform(buf_.data());
      buf_len_ = 0;
    }
    while (buf_len_ < 56) {
      buf_[buf_len_++] = 0x00;
    }
    for (int i = 7; i >= 0; --i) {
      buf_[56 + (7 - i)] = static_cast<unsigned char>((total_bits_ >> (i * 8)) & 0xFFU);
    }
    Transform(buf_.data());

    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (const std::uint32_t word : state_) {
      for (int shift = 28; shift >= 0; shift -= 4) {
        hex.push_back(kHex[(word >> shift) & 0xFU]);
      }
    }
    return hex;
  }

private:
  static constexpr std::uint32_t RotR(std::uint32_t v, int bits) {
    return (v >> bits) | (v << (32 - bits));
  }

  void Transform(const unsigned char block[64]) {
    static constexpr std::array<std::uint32_t, 64> k = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
        0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
        0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
        0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
        0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
    };

    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
             (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
             static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
      const std::uint32_t s0 = RotR(w[i - 15], 7) ^ RotR(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = RotR(w[i - 2], 17) ^ RotR(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t i = 0; i < 64; ++i) {
      const std::uint32_t S1 = RotR(e, 6) ^ RotR(e, 11) ^ RotR(e, 25);
      const std::uint32_t ch = (e & f) ^ ((~e) & g);
      const std::uint32_t temp1 = h + S1 + ch + k[i] + w[i];
      const std::uint32_t S0 = RotR(a, 2) ^ RotR(a, 13) ^ RotR(a, 22);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temp2 = S0 + maj;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_ = {
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
  };
  std::array<unsigned char, 64> buf_{};
  std::size_t buf_len_ = 0;
  std::uint64_t total_bits_ = 0;
};


} // namespace

namespace detail {

std::string InstallerNormalizeSha256(std::string_view input) {
  return NormalizeSha256Impl(input);
}

std::optional<std::filesystem::path> InstallerParseFileUrlPath(std::string_view url) {
  const std::string lower = detail::InstallerLowerAscii(url);
  if (!lower.starts_with("file://")) {
    return std::nullopt;
  }
  std::string_view rest = url.substr(7);
  if (detail::InstallerLowerAscii(rest).starts_with("localhost/")) {
    rest.remove_prefix(9);
  }
  const auto hex_val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string decoded;
  decoded.reserve(rest.size());
  for (std::size_t i = 0; i < rest.size(); ++i) {
    if (rest[i] == '%' && i + 2 < rest.size()) {
      const int hi = hex_val(rest[i + 1]);
      const int lo = hex_val(rest[i + 2]);
      if (hi >= 0 && lo >= 0) {
        decoded.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    decoded.push_back(rest[i]);
  }
#if defined(_WIN32)
  if (decoded.size() >= 3 && decoded[0] == '/' &&
      std::isalpha(static_cast<unsigned char>(decoded[1])) && decoded[2] == ':') {
    decoded.erase(0, 1);
  }
#endif
  if (decoded.empty()) {
    return std::nullopt;
  }
  const std::u8string u8(reinterpret_cast<const char8_t*>(decoded.data()), decoded.size());
  return std::filesystem::path(u8);
}

} // namespace detail

std::string CurrentPlatformTuple() {
#if defined(_WIN32)
  const std::string os = "windows";
#elif defined(__APPLE__)
  const std::string os = "macos";
#elif defined(__linux__)
  const std::string os = "linux";
#else
  const std::string os = "unknown";
#endif

#if defined(__x86_64__) || defined(__amd64__) || defined(_M_X64) || defined(_M_AMD64)
  const std::string arch = "amd64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  const std::string arch = "arm64";
#elif defined(__i386__) || defined(_M_IX86)
  const std::string arch = "x86";
#else
  const std::string arch = "unknown";
#endif
  return os + "_" + arch;
}

std::string AdbcRegistryBaseUrl() {
  std::string url = "https://dbc-cdn.columnar.tech";
  if (const char* override_url = std::getenv("DBPP_ADBC_REGISTRY_URL");
      override_url != nullptr && *override_url != '\0') {
    url = override_url;
  }
  while (url.size() > 1 && url.back() == '/') {
    url.pop_back();
  }
  return url;
}

std::vector<RegistryDriverInfo> ParseRegistryIndexYaml(std::string_view yaml_text,
                                                       std::string_view base_url,
                                                       std::string_view platform_tuple) {
  struct RawPackage {
    std::string platform;
    std::string url;
    std::string sha256;
  };
  struct RawPkgInfo {
    std::string version;
    std::vector<RawPackage> packages;
  };
  struct RawDriver {
    std::string name;
    std::string description;
    std::string license;
    std::string slug;
    std::vector<RawPkgInfo> pkginfo;
  };

  enum class PendingPkgField { None, Url, Hash };

  std::vector<RegistryDriverInfo> result;
  RawDriver cur_driver;
  RawPkgInfo cur_pkginfo;
  RawPackage cur_pkg;
  bool has_driver = false;
  bool has_pkginfo = false;
  bool has_pkg = false;
  bool in_description = false;
  PendingPkgField pending_pkg_field = PendingPkgField::None;

  const auto flush_pkg = [&]() {
    if (has_pkg) {
      if (!cur_pkg.platform.empty() && !cur_pkg.url.empty()) {
        cur_pkginfo.packages.push_back(std::move(cur_pkg));
      }
      cur_pkg = RawPackage{};
      has_pkg = false;
    }
    pending_pkg_field = PendingPkgField::None;
  };

  const auto flush_pkginfo = [&]() {
    flush_pkg();
    if (has_pkginfo) {
      if (!cur_pkginfo.packages.empty()) {
        cur_driver.pkginfo.push_back(std::move(cur_pkginfo));
      }
      cur_pkginfo = RawPkgInfo{};
      has_pkginfo = false;
    }
  };

  const auto flush_driver = [&]() {
    flush_pkginfo();
    if (!has_driver) {
      return;
    }
    if (!cur_driver.slug.empty()) {
      bool found = false;
      SemVer best_ver{};
      std::string best_version_str;
      std::string best_url;
      std::string best_sha256;

      for (const RawPkgInfo& info : cur_driver.pkginfo) {
        for (const RawPackage& pkg : info.packages) {
          if (pkg.platform != platform_tuple || pkg.url.empty()) {
            continue;
          }
          const SemVer candidate = ParseSemVer(info.version);
          if (!found || CompareSemVer(candidate, best_ver) > 0) {
            found = true;
            best_ver = candidate;
            best_version_str = info.version;
            best_url = pkg.url;
            best_sha256 = pkg.sha256;
          }
        }
      }

      if (found) {
        result.push_back(RegistryDriverInfo{
            .slug = cur_driver.slug,
            .name = cur_driver.name.empty() ? cur_driver.slug : cur_driver.name,
            .description = cur_driver.description,
            .license = cur_driver.license,
            .version = best_version_str,
            .package_url = ResolvePackageUrl(base_url, best_url),
            .sha256 = best_sha256,
        });
      }
    }
    cur_driver = RawDriver{};
    has_driver = false;
    in_description = false;
  };

  std::size_t line_start = 0;
  while (line_start <= yaml_text.size()) {
    const std::size_t line_end = yaml_text.find('\n', line_start);
    std::string_view line = line_end == std::string_view::npos
                                ? yaml_text.substr(line_start)
                                : yaml_text.substr(line_start, line_end - line_start);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }

    std::size_t indent = 0;
    while (indent < line.size() && line[indent] == ' ') {
      ++indent;
    }
    const std::string_view trimmed = detail::InstallerTrimAscii(line.substr(indent));
    if (!trimmed.empty() && trimmed.front() != '#') {
      if (indent == 0) {
        if (trimmed.starts_with("- ")) {
          const std::string_view item = detail::InstallerTrimAscii(trimmed.substr(2));
          std::string_view key;
          std::string_view val;
          if (SplitKeyValue(item, key, val) && key == "name") {
            flush_driver();
            has_driver = true;
            cur_driver.name = UnquoteScalar(val);
          } else {
            flush_driver();
          }
        } else {
          flush_driver();
        }
      } else if (has_driver) {
        if (indent == 2) {
          in_description = false;
          pending_pkg_field = PendingPkgField::None;
          if (trimmed.starts_with("- ")) {
            const std::string_view item = detail::InstallerTrimAscii(trimmed.substr(2));
            std::string_view key;
            std::string_view val;
            if (SplitKeyValue(item, key, val) && key == "packages") {
              flush_pkginfo();
              has_pkginfo = true;
            }
          } else {
            std::string_view key;
            std::string_view val;
            if (SplitKeyValue(trimmed, key, val)) {
              if (key == "description") {
                cur_driver.description = UnquoteScalar(val);
                in_description = true;
              } else if (key == "license") {
                cur_driver.license = UnquoteScalar(val);
              } else if (key == "path") {
                cur_driver.slug = UnquoteScalar(val);
              }
            }
          }
        } else if (indent == 4) {
          pending_pkg_field = PendingPkgField::None;
          if (in_description) {
            if (!cur_driver.description.empty()) {
              cur_driver.description.push_back(' ');
            }
            cur_driver.description += UnquoteScalar(trimmed);
          } else if (has_pkginfo) {
            if (trimmed.starts_with("- ")) {
              const std::string_view item = detail::InstallerTrimAscii(trimmed.substr(2));
              std::string_view key;
              std::string_view val;
              if (SplitKeyValue(item, key, val) && key == "platform") {
                flush_pkg();
                has_pkg = true;
                cur_pkg.platform = UnquoteScalar(val);
              }
            } else {
              std::string_view key;
              std::string_view val;
              if (SplitKeyValue(trimmed, key, val) && key == "version") {
                cur_pkginfo.version = UnquoteScalar(val);
              }
            }
          }
        } else if (indent == 6) {
          if (in_description) {
            if (!cur_driver.description.empty()) {
              cur_driver.description.push_back(' ');
            }
            cur_driver.description += UnquoteScalar(trimmed);
          } else if (has_pkg) {
            pending_pkg_field = PendingPkgField::None;
            std::string_view key;
            std::string_view val;
            if (SplitKeyValue(trimmed, key, val)) {
              if (key == "url") {
                if (!val.empty()) {
                  cur_pkg.url = UnquoteScalar(val);
                } else {
                  pending_pkg_field = PendingPkgField::Url;
                }
              } else if (key == "hash") {
                if (!val.empty()) {
                  cur_pkg.sha256 = detail::InstallerNormalizeSha256(val);
                } else {
                  pending_pkg_field = PendingPkgField::Hash;
                }
              }
            }
          }
        } else if (indent >= 8 && has_pkg) {
          if (pending_pkg_field == PendingPkgField::Url) {
            cur_pkg.url = UnquoteScalar(trimmed);
            pending_pkg_field = PendingPkgField::None;
          } else if (pending_pkg_field == PendingPkgField::Hash) {
            cur_pkg.sha256 = detail::InstallerNormalizeSha256(trimmed);
            pending_pkg_field = PendingPkgField::None;
          }
        }
      }
    }

    if (line_end == std::string_view::npos) {
      break;
    }
    line_start = line_end + 1;
  }

  flush_driver();

  std::ranges::sort(result, [](const RegistryDriverInfo& left, const RegistryDriverInfo& right) {
    const int left_rank = CoreDriverRank(left.slug);
    const int right_rank = CoreDriverRank(right.slug);
    if (left_rank != right_rank) {
      return left_rank < right_rank;
    }
    return left.slug < right.slug;
  });
  return result;
}

std::string Sha256HexFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {};
  }
  Sha256Hasher hasher;
  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count > 0) {
      hasher.Update(reinterpret_cast<const unsigned char*>(buffer.data()),
                    static_cast<std::size_t>(count));
    }
  }
  if (input.bad()) {
    return {};
  }
  return hasher.FinalHex();
}


huxerui::Task<RegistryCatalogResult> FetchRegistryCatalogAsync(
    std::shared_ptr<huxerui::HttpClient> http) {
  RegistryCatalogResult result;
  if (!http) {
    result.error = Tr("HTTP client is unavailable", "HTTP 客户端不可用");
    co_return result;
  }

  const std::string base_url = AdbcRegistryBaseUrl();
  huxerui::HttpRequest request;
  request.url = base_url + "/index.yaml";
  request.headers.push_back(huxerui::HttpHeader{"Accept", "application/yaml"});
  request.timeout = std::chrono::milliseconds{30000};
  const huxerui::HttpResult<huxerui::HttpResponse> response =
      co_await http->SendAsync(std::move(request));
  if (!response.Succeeded()) {
    result.error = TrFmt("Failed to fetch driver catalog: {0}", "获取驱动目录失败: {0}",
                         {response.Error().message});
    co_return result;
  }

  const huxerui::HttpResponse& http_resp = response.Value();
  if (http_resp.status_code < 200 || http_resp.status_code >= 300) {
    result.error = TrFmt("Failed to fetch driver catalog: HTTP {0}",
                         "获取驱动目录失败: HTTP {0}",
                         {std::to_string(http_resp.status_code)});
    co_return result;
  }

  std::string yaml_text;
  yaml_text.resize(http_resp.body.size());
  if (!http_resp.body.empty()) {
    std::memcpy(yaml_text.data(), http_resp.body.data(), http_resp.body.size());
  }

  const std::string platform = CurrentPlatformTuple();
  auto parse_job = [yaml_text = std::move(yaml_text), base_url, platform]() {
    return ParseRegistryIndexYaml(yaml_text, base_url, platform);
  };
  auto parsed = co_await huxerui::RunWorker(std::move(parse_job));
  result.drivers = std::move(parsed);
  if (result.drivers.empty()) {
    result.error = TrFmt("No online drivers available for current platform ({0})",
                         "当前平台 ({0}) 暂无可用的在线驱动", {platform});
  }
  co_return result;
}

RegistryCatalogResult FetchRegistryCatalogBlocking(std::string_view registry_url_override) {
  RegistryCatalogResult result;
  std::string base_url =
      registry_url_override.empty() ? AdbcRegistryBaseUrl() : std::string(registry_url_override);
  while (base_url.size() > 1 && base_url.back() == '/') {
    base_url.pop_back();
  }
  const std::string index_url = base_url + "/index.yaml";

  std::string yaml_text;
  if (const auto local_index = detail::InstallerParseFileUrlPath(index_url)) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(*local_index, ec)) {
      result.error = TrFmt("Driver catalog file not found: {0}", "驱动目录文件不存在: {0}",
                           {local_index->string()});
      return result;
    }
    std::ifstream in(*local_index, std::ios::binary);
    if (!in) {
      result.error = TrFmt("Failed to read driver catalog file: {0}", "无法读取驱动目录文件: {0}",
                           {local_index->string()});
      return result;
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    yaml_text = oss.str();
  } else {
    std::error_code ec;
    const std::filesystem::path temp_dir = std::filesystem::temp_directory_path(ec);
    if (ec || temp_dir.empty()) {
      result.error = Tr("Failed to resolve temp directory", "无法获取临时目录");
      return result;
    }
    const std::filesystem::path tmp_yaml =
        temp_dir / ("dbpp-registry-" + detail::InstallerNextUniqueSuffix() + ".yaml");
    const struct TmpCleanup { std::filesystem::path p; ~TmpCleanup() { detail::InstallerRemoveFileQuiet(p); } } cleanup{tmp_yaml};

    std::string download_error;
    if (!detail::InstallerDownloadUrlToFileBlocking(index_url, tmp_yaml, "application/yaml", 30, download_error)) {
      result.error = TrFmt("Failed to fetch driver catalog: {0}", "获取驱动目录失败: {0}",
                           {download_error});
      return result;
    }
    std::ifstream in(tmp_yaml, std::ios::binary);
    if (!in) {
      result.error = Tr("Failed to read temp driver catalog file", "读取驱动目录临时文件失败");
      return result;
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    yaml_text = oss.str();
  }

  const std::string platform = CurrentPlatformTuple();
  result.drivers = ParseRegistryIndexYaml(yaml_text, base_url, platform);
  if (result.drivers.empty()) {
    result.error = TrFmt("No online drivers available for current platform ({0})",
                         "当前平台 ({0}) 暂无可用的在线驱动", {platform});
  }
  return result;
}


} // namespace dbpp
