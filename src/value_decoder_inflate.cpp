module dbpp.decoder;

import std;
import dbpp.i18n;

namespace dbpp::detail {

namespace {

constexpr std::size_t kMaxInflateOutputBytes = 4u * 1024u * 1024u;
constexpr int kMaxBits = 15;
constexpr int kMaxLCodes = 286;
constexpr int kMaxDCodes = 30;
constexpr int kFixLCodes = 288;

struct BitReader {
  std::span<const unsigned char> data;
  std::size_t byte_pos = 0;
  std::uint32_t bit_buf = 0;
  int bit_cnt = 0;

  bool Ensure(int n, std::string& err) noexcept {
    while (bit_cnt < n) {
      if (byte_pos >= data.size()) {
        err = Tr("Unexpected end of compressed stream", "压缩数据流意外结束");
        return false;
      }
      bit_buf |= static_cast<std::uint32_t>(data[byte_pos++]) << bit_cnt;
      bit_cnt += 8;
    }
    return true;
  }

  bool ReadBits(int n, std::uint32_t& out, std::string& err) noexcept {
    if (n == 0) {
      out = 0;
      return true;
    }
    if (!Ensure(n, err)) {
      return false;
    }
    out = bit_buf & ((1u << n) - 1u);
    bit_buf >>= n;
    bit_cnt -= n;
    return true;
  }

  void AlignToByte() noexcept {
    const int drop = bit_cnt & 7;
    bit_buf >>= drop;
    bit_cnt -= drop;
  }

  [[nodiscard]] std::size_t BytesConsumed() const noexcept {
    return byte_pos - static_cast<std::size_t>(bit_cnt / 8);
  }
};

struct HuffmanTable {
  std::array<int, kMaxBits + 1> count{};
  std::vector<int> symbols;
};

bool ConstructHuffman(std::span<const int> lengths, HuffmanTable& out) {
  out.count.fill(0);
  out.symbols.assign(lengths.size(), 0);
  for (const int len : lengths) {
    if (len < 0 || len > kMaxBits) {
      return false;
    }
    ++out.count[static_cast<std::size_t>(len)];
  }
  if (out.count[0] == static_cast<int>(lengths.size())) {
    return true;
  }
  int left = 1;
  for (int len = 1; len <= kMaxBits; ++len) {
    left <<= 1;
    left -= out.count[static_cast<std::size_t>(len)];
    if (left < 0) {
      return false;
    }
  }
  std::array<int, kMaxBits + 1> offs{};
  for (int len = 1; len < kMaxBits; ++len) {
    offs[static_cast<std::size_t>(len + 1)] =
        offs[static_cast<std::size_t>(len)] + out.count[static_cast<std::size_t>(len)];
  }
  for (std::size_t sym = 0; sym < lengths.size(); ++sym) {
    const int len = lengths[sym];
    if (len != 0) {
      out.symbols[static_cast<std::size_t>(offs[static_cast<std::size_t>(len)]++)] =
          static_cast<int>(sym);
    }
  }
  return true;
}

bool DecodeSymbol(BitReader& br, const HuffmanTable& h, int& sym_out, std::string& err) noexcept {
  int code = 0;
  int first = 0;
  int index = 0;
  for (int len = 1; len <= kMaxBits; ++len) {
    if (!br.Ensure(1, err)) {
      return false;
    }
    code |= static_cast<int>(br.bit_buf & 1u);
    br.bit_buf >>= 1;
    --br.bit_cnt;
    const int cnt = h.count[static_cast<std::size_t>(len)];
    if (code - cnt < first) {
      sym_out = h.symbols[static_cast<std::size_t>(index + (code - first))];
      return true;
    }
    index += cnt;
    first += cnt;
    first <<= 1;
    code <<= 1;
  }
  err = Tr("Invalid Huffman code in compressed stream", "压缩流包含无效的 Huffman 编码");
  return false;
}

bool DecodeCodesBlock(BitReader& br, const HuffmanTable& lencode, const HuffmanTable& distcode,
                      std::string& out, std::string& err) {
  static constexpr std::array<std::uint16_t, 29> kLenBase = {
      3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
  };
  static constexpr std::array<int, 29> kLenExtra = {
      0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
  };
  static constexpr std::array<std::uint16_t, 30> kDistBase = {
      1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
      193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
  };
  static constexpr std::array<int, 30> kDistExtra = {
      0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
      6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
  };

  for (;;) {
    int sym = 0;
    if (!DecodeSymbol(br, lencode, sym, err)) {
      return false;
    }
    if (sym < 256) {
      if (out.size() >= kMaxInflateOutputBytes) {
        err = Tr("Decompressed payload exceeds 4 MiB safety limit", "解压后数据超过 4 MiB 安全上限");
        return false;
      }
      out.push_back(static_cast<char>(sym));
      continue;
    }
    if (sym == 256) {
      return true;
    }
    sym -= 257;
    if (sym < 0 || sym >= 29) {
      err = Tr("Invalid DEFLATE length code", "无效的 DEFLATE 长度码");
      return false;
    }
    std::uint32_t extra = 0;
    if (!br.ReadBits(kLenExtra[static_cast<std::size_t>(sym)], extra, err)) {
      return false;
    }
    const std::size_t len = kLenBase[static_cast<std::size_t>(sym)] + extra;

    int dist_sym = 0;
    if (!DecodeSymbol(br, distcode, dist_sym, err)) {
      return false;
    }
    if (dist_sym < 0 || dist_sym >= 30) {
      err = Tr("Invalid DEFLATE distance code", "无效的 DEFLATE 距离码");
      return false;
    }
    if (!br.ReadBits(kDistExtra[static_cast<std::size_t>(dist_sym)], extra, err)) {
      return false;
    }
    const std::size_t dist = kDistBase[static_cast<std::size_t>(dist_sym)] + extra;
    if (dist == 0 || dist > out.size()) {
      err = Tr("Invalid DEFLATE back-reference distance", "无效的 DEFLATE 回溯偏移距离");
      return false;
    }
    if (out.size() + len > kMaxInflateOutputBytes) {
      err = Tr("Decompressed payload exceeds 4 MiB safety limit", "解压后数据超过 4 MiB 安全上限");
      return false;
    }
    for (std::size_t i = 0; i < len; ++i) {
      out.push_back(out[out.size() - dist]);
    }
  }
}

bool InflateFixedBlock(BitReader& br, std::string& out, std::string& err) {
  std::array<int, kFixLCodes> l_lens{};
  for (int i = 0; i <= 143; ++i) l_lens[static_cast<std::size_t>(i)] = 8;
  for (int i = 144; i <= 255; ++i) l_lens[static_cast<std::size_t>(i)] = 9;
  for (int i = 256; i <= 279; ++i) l_lens[static_cast<std::size_t>(i)] = 7;
  for (int i = 280; i <= 287; ++i) l_lens[static_cast<std::size_t>(i)] = 8;
  std::array<int, kMaxDCodes> d_lens{};
  d_lens.fill(5);

  HuffmanTable lencode;
  HuffmanTable distcode;
  if (!ConstructHuffman(l_lens, lencode) || !ConstructHuffman(d_lens, distcode)) {
    err = Tr("Failed to initialize fixed Huffman table", "初始化固定 Huffman 表失败");
    return false;
  }
  return DecodeCodesBlock(br, lencode, distcode, out, err);
}

bool InflateDynamicBlock(BitReader& br, std::string& out, std::string& err) {
  static constexpr std::array<int, 19> kClOrder = {
      16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
  };
  std::uint32_t v1 = 0;
  std::uint32_t v2 = 0;
  std::uint32_t v3 = 0;
  if (!br.ReadBits(5, v1, err) || !br.ReadBits(5, v2, err) || !br.ReadBits(4, v3, err)) {
    return false;
  }
  const int nlen = static_cast<int>(v1) + 257;
  const int ndist = static_cast<int>(v2) + 1;
  const int ncode = static_cast<int>(v3) + 4;
  if (nlen > kMaxLCodes || ndist > kMaxDCodes) {
    err = Tr("Invalid dynamic Huffman header counts", "无效的动态 Huffman 码表长度");
    return false;
  }

  std::array<int, 19> cl_lens{};
  for (int i = 0; i < ncode; ++i) {
    std::uint32_t bits = 0;
    if (!br.ReadBits(3, bits, err)) {
      return false;
    }
    cl_lens[static_cast<std::size_t>(kClOrder[static_cast<std::size_t>(i)])] = static_cast<int>(bits);
  }
  HuffmanTable cl_table;
  if (!ConstructHuffman(cl_lens, cl_table)) {
    err = Tr("Invalid dynamic Huffman code-length table", "动态 Huffman 码长表无效");
    return false;
  }

  std::vector<int> lengths(static_cast<std::size_t>(nlen + ndist), 0);
  int idx = 0;
  while (idx < nlen + ndist) {
    int sym = 0;
    if (!DecodeSymbol(br, cl_table, sym, err)) {
      return false;
    }
    if (sym < 16) {
      lengths[static_cast<std::size_t>(idx++)] = sym;
      continue;
    }
    int rep_val = 0;
    std::uint32_t extra = 0;
    int rep_cnt = 0;
    if (sym == 16) {
      if (idx == 0) {
        err = Tr("Invalid repeat symbol in dynamic Huffman table", "动态 Huffman 码表重复符号越界");
        return false;
      }
      rep_val = lengths[static_cast<std::size_t>(idx - 1)];
      if (!br.ReadBits(2, extra, err)) return false;
      rep_cnt = 3 + static_cast<int>(extra);
    } else if (sym == 17) {
      if (!br.ReadBits(3, extra, err)) return false;
      rep_cnt = 3 + static_cast<int>(extra);
    } else if (sym == 18) {
      if (!br.ReadBits(7, extra, err)) return false;
      rep_cnt = 11 + static_cast<int>(extra);
    } else {
      err = Tr("Invalid symbol in dynamic Huffman code-length stream", "动态 Huffman 码长包含非法符号");
      return false;
    }
    if (idx + rep_cnt > nlen + ndist) {
      err = Tr("Dynamic Huffman code lengths overflow", "动态 Huffman 码长数量溢出");
      return false;
    }
    for (int j = 0; j < rep_cnt; ++j) {
      lengths[static_cast<std::size_t>(idx++)] = rep_val;
    }
  }

  if (lengths[256] == 0) {
    err = Tr("Missing end-of-block symbol in DEFLATE table", "DEFLATE 码表缺少结束符号 256");
    return false;
  }

  HuffmanTable lencode;
  HuffmanTable distcode;
  const std::span<const int> len_span(lengths.data(), static_cast<std::size_t>(nlen));
  const std::span<const int> dist_span(lengths.data() + nlen, static_cast<std::size_t>(ndist));
  if (!ConstructHuffman(len_span, lencode) || !ConstructHuffman(dist_span, distcode)) {
    err = Tr("Incomplete or invalid dynamic Huffman table", "动态 Huffman 码表无效");
    return false;
  }
  return DecodeCodesBlock(br, lencode, distcode, out, err);
}

bool InflateRawDeflate(std::span<const unsigned char> stream, std::size_t& consumed_out,
                       std::string& out, std::string& err) {
  BitReader br{.data = stream};
  out.clear();
  for (;;) {
    std::uint32_t bfinal = 0;
    std::uint32_t btype = 0;
    if (!br.ReadBits(1, bfinal, err) || !br.ReadBits(2, btype, err)) {
      return false;
    }
    if (btype == 0) {
      br.AlignToByte();
      if (br.byte_pos + 4 > br.data.size()) {
        err = Tr("Truncated stored DEFLATE block header", "未压缩块头不完整");
        return false;
      }
      const std::uint16_t len = static_cast<std::uint16_t>(br.data[br.byte_pos]) |
                                (static_cast<std::uint16_t>(br.data[br.byte_pos + 1]) << 8);
      const std::uint16_t nlen = static_cast<std::uint16_t>(br.data[br.byte_pos + 2]) |
                                 (static_cast<std::uint16_t>(br.data[br.byte_pos + 3]) << 8);
      br.byte_pos += 4;
      if (static_cast<std::uint16_t>(len ^ 0xFFFFu) != nlen) {
        err = Tr("Corrupted stored DEFLATE block length", "未压缩块长度校验失败");
        return false;
      }
      if (br.byte_pos + len > br.data.size()) {
        err = Tr("Truncated stored DEFLATE block payload", "未压缩块数据不完整");
        return false;
      }
      if (out.size() + len > kMaxInflateOutputBytes) {
        err = Tr("Decompressed payload exceeds 4 MiB safety limit", "解压后数据超过 4 MiB 安全上限");
        return false;
      }
      out.append(reinterpret_cast<const char*>(br.data.data() + br.byte_pos), len);
      br.byte_pos += len;
    } else if (btype == 1) {
      if (!InflateFixedBlock(br, out, err)) {
        return false;
      }
    } else if (btype == 2) {
      if (!InflateDynamicBlock(br, out, err)) {
        return false;
      }
    } else {
      err = Tr("Reserved DEFLATE block type (3)", "压缩流包含保留块类型 (3)");
      return false;
    }
    if (bfinal != 0) {
      break;
    }
  }
  br.AlignToByte();
  consumed_out = br.BytesConsumed();
  return true;
}

std::uint32_t ComputeCrc32(std::string_view data) noexcept {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const unsigned char b : data) {
    crc ^= b;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) != 0 ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

std::uint32_t ComputeAdler32(std::string_view data) noexcept {
  constexpr std::uint32_t kMod = 65521u;
  std::uint32_t s1 = 1u;
  std::uint32_t s2 = 0u;
  for (const unsigned char b : data) {
    s1 = (s1 + b) % kMod;
    s2 = (s2 + s1) % kMod;
  }
  return (s2 << 16) | s1;
}

std::uint32_t ReadLe32(const unsigned char* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint32_t ReadBe32(const unsigned char* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

bool InflateGzipContainer(std::span<const unsigned char> data, std::string& out, std::string& err) {
  if (data.size() < 18 || data[0] != 0x1Fu || data[1] != 0x8Bu || data[2] != 0x08u) {
    err = Tr("Invalid Gzip header", "无效的 Gzip 文件头");
    return false;
  }
  const unsigned char flg = data[3];
  if ((flg & 0xE0u) != 0u) {
    err = Tr("Unsupported Gzip reserved flags", "不支持的 Gzip 保留标志位");
    return false;
  }
  std::size_t pos = 10;
  if ((flg & 0x04u) != 0u) { // FEXTRA
    if (pos + 2 > data.size()) {
      err = Tr("Truncated Gzip extra header", "Gzip 扩展头不完整");
      return false;
    }
    const std::size_t xlen = static_cast<std::size_t>(data[pos]) |
                             (static_cast<std::size_t>(data[pos + 1]) << 8);
    pos += 2 + xlen;
  }
  if ((flg & 0x08u) != 0u) { // FNAME
    while (pos < data.size() && data[pos] != 0) ++pos;
    ++pos;
  }
  if ((flg & 0x10u) != 0u) { // FCOMMENT
    while (pos < data.size() && data[pos] != 0) ++pos;
    ++pos;
  }
  if ((flg & 0x02u) != 0u) { // FHCRC
    pos += 2;
  }
  if (pos + 8 > data.size()) {
    err = Tr("Truncated Gzip header or trailer", "Gzip 头部或尾部不完整");
    return false;
  }

  std::size_t consumed = 0;
  const std::span<const unsigned char> deflate_span(data.data() + pos, data.size() - pos - 8);
  if (!InflateRawDeflate(deflate_span, consumed, out, err)) {
    return false;
  }
  if (pos + consumed + 8 > data.size()) {
    err = Tr("Missing Gzip CRC32/ISIZE trailer", "缺少 Gzip CRC32/ISIZE 校验尾");
    return false;
  }
  const std::size_t trailer_pos = pos + consumed;
  const std::uint32_t expected_crc = ReadLe32(data.data() + trailer_pos);
  const std::uint32_t expected_isize = ReadLe32(data.data() + trailer_pos + 4);
  if (ComputeCrc32(out) != expected_crc) {
    err = Tr("Gzip CRC32 checksum mismatch", "Gzip CRC32 校验失败");
    return false;
  }
  if (static_cast<std::uint32_t>(out.size() & 0xFFFFFFFFu) != expected_isize) {
    err = Tr("Gzip ISIZE length mismatch", "Gzip 解压长度校验失败");
    return false;
  }
  return true;
}

bool InflateZlibContainer(std::span<const unsigned char> data, std::string& out, std::string& err) {
  if (data.size() < 6) {
    err = Tr("Truncated Zlib stream", "Zlib 数据不完整");
    return false;
  }
  const unsigned char cmf = data[0];
  const unsigned char flg = data[1];
  const std::uint16_t check =
      (static_cast<std::uint16_t>(cmf) << 8) | static_cast<std::uint16_t>(flg);
  if ((cmf & 0x0Fu) != 8u || (cmf >> 4) > 7u || (check % 31u) != 0u) {
    err = Tr("Invalid Zlib header", "无效的 Zlib 文件头");
    return false;
  }
  if ((flg & 0x20u) != 0u) {
    err = Tr("Zlib preset dictionary (FDICT) is not supported", "暂不支持带预设字典的 Zlib 流");
    return false;
  }

  std::size_t consumed = 0;
  const std::span<const unsigned char> deflate_span(data.data() + 2, data.size() - 6);
  if (!InflateRawDeflate(deflate_span, consumed, out, err)) {
    return false;
  }
  if (2 + consumed + 4 > data.size()) {
    err = Tr("Missing Zlib Adler-32 trailer", "缺少 Zlib Adler-32 校验尾");
    return false;
  }
  const std::uint32_t expected_adler = ReadBe32(data.data() + 2 + consumed);
  if (ComputeAdler32(out) != expected_adler) {
    err = Tr("Zlib Adler-32 checksum mismatch", "Zlib Adler-32 校验失败");
    return false;
  }
  return true;
}

} // namespace

InflateContainerKind DetectCompressedContainer(std::string_view bytes) noexcept {
  if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0x1Fu &&
      static_cast<unsigned char>(bytes[1]) == 0x8Bu &&
      static_cast<unsigned char>(bytes[2]) == 0x08u) {
    return InflateContainerKind::Gzip;
  }
  if (bytes.size() >= 6) {
    const unsigned char cmf = static_cast<unsigned char>(bytes[0]);
    const unsigned char flg = static_cast<unsigned char>(bytes[1]);
    const std::uint16_t check =
        (static_cast<std::uint16_t>(cmf) << 8) | static_cast<std::uint16_t>(flg);
    if ((cmf & 0x0Fu) == 8u && (cmf >> 4) <= 7u && (check % 31u) == 0u && (flg & 0x20u) == 0u) {
      return InflateContainerKind::Zlib;
    }
  }
  return InflateContainerKind::None;
}

bool InflateGzipOrZlib(std::string_view bytes, InflateContainerKind& kind_out, std::string& out,
                       std::string& error_out) {
  kind_out = DetectCompressedContainer(bytes);
  const std::span<const unsigned char> span(reinterpret_cast<const unsigned char*>(bytes.data()),
                                            bytes.size());
  if (kind_out == InflateContainerKind::Gzip) {
    return InflateGzipContainer(span, out, error_out);
  }
  if (kind_out == InflateContainerKind::Zlib) {
    return InflateZlibContainer(span, out, error_out);
  }
  error_out = Tr("Input is not a valid Gzip or Zlib stream", "输入不是有效的 Gzip 或 Zlib 压缩流");
  return false;
}

} // namespace dbpp::detail
