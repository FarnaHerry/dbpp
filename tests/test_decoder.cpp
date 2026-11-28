import std;
import dbpp.decoder;

namespace {

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    ++g_failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

void TestJsonDecoderAndMinify() {
  const std::string raw = R"({"b":[1,true,null,"hi\u4e2d\uD83D\uDE00"],"a":{"x":3.14}})";
  const auto res = dbpp::DecodeValue(raw, dbpp::ValueDecoderKind::Json);
  Expect(res.ok && res.text.find('\n') != std::string::npos &&
             res.text.find("中\xF0\x9F\x98\x80") != std::string::npos &&
             res.text.find("  \"a\": {") != std::string::npos,
         "JSON pretty-print / surrogate decoding failed: " + res.error + "\n" + res.text);

  const auto min = dbpp::MinifyJsonValue(res.text);
  Expect(min.ok && min.text.find('\n') == std::string::npos &&
             min.text.find("3.14") != std::string::npos,
         "MinifyJsonValue failed: " + min.error);

  const auto bad = dbpp::DecodeValue("{bad:1}", dbpp::ValueDecoderKind::Json);
  Expect(!bad.ok && !bad.error.empty(), "Invalid JSON must fail");

  const auto trailing = dbpp::DecodeValue("{\"a\":1} extra", dbpp::ValueDecoderKind::Json);
  Expect(!trailing.ok, "JSON with trailing tokens must fail");
}

void TestBase64Decoder() {
  // Base64 -> JSON auto-format
  // {"hello":"world","n":42} -> eyJoZWxsbyI6IndvcmxkIiwibiI6NDJ9
  const auto b64_json = dbpp::DecodeValue("eyJoZWxsbyI6IndvcmxkIiwibiI6NDJ9",
                                          dbpp::ValueDecoderKind::Base64);
  Expect(b64_json.ok && b64_json.text.find("  \"hello\": \"world\"") != std::string::npos &&
             b64_json.subtitle.find("JSON") != std::string::npos,
         "Base64 -> JSON failed: " + b64_json.error);

  // URL-safe unpadded UTF-8 with newlines
  // "中文_ok?" = 5Lit5paHX29rPw== -> URL-safe unpadded "5Lit5paHX29rPw"
  const auto b64_url = dbpp::DecodeValue("5Lit5paH\nX29rPw", dbpp::ValueDecoderKind::Base64);
  Expect(b64_url.ok && b64_url.text == "中文_ok?",
         "Base64URL unpadded UTF-8 decode failed: " + b64_url.text);

  // Binary payload -> Hex fallback (\x00\x01\xfe\xff -> AAH+/w==)
  const auto b64_bin = dbpp::DecodeValue("AAH+/w==", dbpp::ValueDecoderKind::Base64);
  Expect(b64_bin.ok && b64_bin.text.find("00 01 fe ff") != std::string::npos,
         "Base64 binary hex fallback failed: " + b64_bin.text);

  const auto bad_b64 = dbpp::DecodeValue("not!valid!b64", dbpp::ValueDecoderKind::Base64);
  Expect(!bad_b64.ok, "Invalid Base64 must fail");
}

void TestJwtDecoder() {
  // Header: {"alg":"HS256","typ":"JWT"} -> eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9
  // Payload: {"sub":"123","iat":1700000000,"exp":1700003600} ->
  // eyJzdWIiOiIxMjMiLCJpYXQiOjE3MDAwMDAwMDAsImV4cCI6MTcwMDAwMzYwMH0
  const std::string jwt =
      "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
      "eyJzdWIiOiIxMjMiLCJpYXQiOjE3MDAwMDAwMDAsImV4cCI6MTcwMDAwMzYwMH0."
      "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
  const auto res = dbpp::DecodeValue(jwt, dbpp::ValueDecoderKind::Jwt);
  Expect(res.ok && res.text.find("\"alg\": \"HS256\"") != std::string::npos &&
             res.text.find("\"sub\": \"123\"") != std::string::npos &&
             res.text.find("exp: 1700003600") != std::string::npos &&
             res.text.find("2023-11-14") != std::string::npos,
         "JWT decode failed: " + res.error + "\n" + res.text);

  const auto bad_jwt = dbpp::DecodeValue("abc.def", dbpp::ValueDecoderKind::Jwt);
  Expect(!bad_jwt.ok, "2-part token must fail JWT check");
}

void TestUrlHexUnicodeAndTimestamp() {
  const auto url_ok = dbpp::DecodeValue("%E4%B8%AD%E6%96%87%20100%25", dbpp::ValueDecoderKind::Url);
  Expect(url_ok.ok && url_ok.text == "中文 100%", "URL decode mismatch: " + url_ok.text);

  const auto url_bad = dbpp::DecodeValue("abc%ZZ", dbpp::ValueDecoderKind::Url);
  Expect(!url_bad.ok, "Invalid URL %ZZ must fail");

  const std::string bin_raw("Hello\x00\xFF", 7);
  const auto hex_res = dbpp::DecodeValue(bin_raw, dbpp::ValueDecoderKind::Hex);
  Expect(hex_res.ok && hex_res.text.find("00000000  48 65 6c 6c 6f 00 ff") != std::string::npos &&
             hex_res.text.find("|Hello..|") != std::string::npos,
         "Hex dump mismatch: " + hex_res.text);

  const std::string big_raw(20000, 'A');
  const auto hex_big = dbpp::DecodeValue(big_raw, dbpp::ValueDecoderKind::Hex);
  Expect(hex_big.ok && hex_big.text.find("16384") != std::string::npos,
         "Large Hex dump must report truncation at 16384 bytes");

  const auto uni_res = dbpp::DecodeValue(R"(\u4e2d\u6587\n\uD83D\uDE00)", dbpp::ValueDecoderKind::Unicode);
  Expect(uni_res.ok && uni_res.text == "中文\n\xF0\x9F\x98\x80",
         "Unicode decode mismatch: " + uni_res.text);

  const auto ts_s = dbpp::DecodeValue("1700000000", dbpp::ValueDecoderKind::Timestamp);
  Expect(ts_s.ok && ts_s.text.find("2023-11-14 22:13:20 UTC") != std::string::npos,
         "10-digit timestamp mismatch: " + ts_s.text);

  const auto ts_ms = dbpp::DecodeValue("1700000000123", dbpp::ValueDecoderKind::Timestamp);
  Expect(ts_ms.ok && ts_ms.text.find("2023-11-14 22:13:20.123 UTC") != std::string::npos,
         "13-digit ms timestamp mismatch: " + ts_ms.text);

  const auto ts_bad = dbpp::DecodeValue("not_a_ts", dbpp::ValueDecoderKind::Timestamp);
  Expect(!ts_bad.ok, "Non-numeric timestamp must fail");
}

void TestAutoDetection() {
  const std::string jwt =
      "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
      "eyJzdWIiOiIxMjMifQ."
      "SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c";
  Expect(dbpp::DetectSuggestedDecoder(jwt) == dbpp::ValueDecoderKind::Jwt, "Detect JWT");
  Expect(dbpp::DetectSuggestedDecoder("{\"k\":1}") == dbpp::ValueDecoderKind::Json, "Detect JSON");
  Expect(dbpp::DetectSuggestedDecoder("1700000000") == dbpp::ValueDecoderKind::Timestamp,
         "Detect Timestamp");
  Expect(dbpp::DetectSuggestedDecoder(R"(\u4e2d\u6587)") == dbpp::ValueDecoderKind::Unicode,
         "Detect Unicode");
  Expect(dbpp::DetectSuggestedDecoder("%E4%B8%AD") == dbpp::ValueDecoderKind::Url, "Detect URL");
  Expect(dbpp::DetectSuggestedDecoder("eyJoZWxsbyI6IndvcmxkIn0=") == dbpp::ValueDecoderKind::Base64,
         "Detect Base64");
  Expect(dbpp::DetectSuggestedDecoder(std::string("a\x00\xFF", 3)) == dbpp::ValueDecoderKind::Hex,
         "Detect Hex for binary");

  // Guard against false positives on ordinary short values.
  Expect(dbpp::DetectSuggestedDecoder("hello") == dbpp::ValueDecoderKind::Raw,
         "Plain word -> Raw");
  Expect(dbpp::DetectSuggestedDecoder("password") == dbpp::ValueDecoderKind::Raw,
         "8-char lowercase word -> Raw");
  Expect(dbpp::DetectSuggestedDecoder("user_name_1") == dbpp::ValueDecoderKind::Raw,
         "Snake case identifier -> Raw");
  Expect(dbpp::DetectSuggestedDecoder("12345") == dbpp::ValueDecoderKind::Raw,
         "Small integer -> Raw");
  Expect(dbpp::DetectSuggestedDecoder("x^hello_plain_text") == dbpp::ValueDecoderKind::Raw,
         "ASCII text starting with 'x^' must not false-trigger Zlib");
}

void TestGzipAndZlibDecoder() {
  // 1. Base64-wrapped Gzip JSON (H4sI..., exercises fixed/dynamic Huffman + LZ77 backreferences).
  const std::string gz_b64 =
      "H4sIAAAAAAAC/6tWKi1OLVKyqlbKS8xNVbJSSszJTE5V0lEqys9JLVayilZKTMnNzAMKpKSWAUlkXqyOUn62klVJUWlqbS0AQHNF70kAAAA=";
  Expect(dbpp::DetectSuggestedDecoder(gz_b64) == dbpp::ValueDecoderKind::Gzip,
         "H4sI... must auto-detect as Gzip");

  const auto gz_res = dbpp::DecodeValue(gz_b64, dbpp::ValueDecoderKind::Gzip);
  Expect(gz_res.ok && gz_res.text.find("\"name\": \"alice\"") != std::string::npos &&
             gz_res.text.find("\"admin\"") != std::string::npos &&
             gz_res.subtitle.find("Gzip") != std::string::npos &&
             gz_res.subtitle.find("JSON") != std::string::npos,
         "Gzip Base64 JSON decode failed: " + gz_res.error + "\n" + gz_res.text);

  // Selecting Base64 on H4sI... should also seamlessly inflate to readable JSON.
  const auto b64_gz_res = dbpp::DecodeValue(gz_b64, dbpp::ValueDecoderKind::Base64);
  Expect(b64_gz_res.ok && b64_gz_res.text == gz_res.text &&
             b64_gz_res.subtitle.find("Gzip") != std::string::npos,
         "Base64 -> Gzip chained decode failed: " + b64_gz_res.error);

  // 2. Base64-wrapped Zlib (eJx...) dynamic Huffman UTF-8 Chinese payload.
  const std::string zl_b64 = "eJx7snfB06V73+/pCUpNySx+2tf9fM/KZ1u7X6yf+n5P4xN8kgB9lCb9";
  Expect(dbpp::DetectSuggestedDecoder(zl_b64) == dbpp::ValueDecoderKind::Gzip,
         "eJx... must auto-detect as Gzip / Zlib");
  const auto zl_res = dbpp::DecodeValue(zl_b64, dbpp::ValueDecoderKind::Gzip);
  Expect(zl_res.ok && zl_res.text == "你好，Redis压缩测试！你好，Redis压缩测试！",
         "Zlib Base64 UTF-8 decode failed: " + zl_res.error + " / " + zl_res.text);

  // 3. Zlib stored block (level 0 -> BTYPE=0) + raw Zlib binary stream.
  const std::string stored_b64 = "eAEBEwDs/3N0b3JlZCBoZWxsbyB3b3JsZCFKwwcv";
  Expect(dbpp::DetectSuggestedDecoder(stored_b64) == dbpp::ValueDecoderKind::Gzip,
         "Stored zlib block must auto-detect as Gzip / Zlib");
  const auto stored_res = dbpp::DecodeValue(stored_b64, dbpp::ValueDecoderKind::Gzip);
  Expect(stored_res.ok && stored_res.text == "stored hello world!",
         "Stored zlib block decode failed: " + stored_res.error + " / " + stored_res.text);

  // 4. Raw (non-Base64) Gzip binary stream & corrupted CRC rejection.
  // Construct raw bytes from a minimal Gzip stream: gzip("") =
  // 1f 8b 08 00 00 00 00 00 02 ff 03 00 00 00 00 00 00 00 00 00
  std::string raw_gz_empty(
      "\x1F\x8B\x08\x00\x00\x00\x00\x00\x02\xFF\x03\x00\x00\x00\x00\x00\x00\x00\x00\x00", 20);
  Expect(dbpp::DetectSuggestedDecoder(raw_gz_empty) == dbpp::ValueDecoderKind::Gzip,
         "Raw Gzip bytes must auto-detect as Gzip");
  const auto raw_ok = dbpp::DecodeValue(raw_gz_empty, dbpp::ValueDecoderKind::Gzip);
  Expect(raw_ok.ok && raw_ok.text.empty(), "Raw empty Gzip decode failed: " + raw_ok.error);

  raw_gz_empty.back() ^= 0x01; // corrupt ISIZE trailer
  const auto raw_bad = dbpp::DecodeValue(raw_gz_empty, dbpp::ValueDecoderKind::Gzip);
  Expect(!raw_bad.ok && !raw_bad.error.empty(), "Corrupted Gzip trailer must fail");

  const auto not_gz = dbpp::DecodeValue("plain hello text", dbpp::ValueDecoderKind::Gzip);
  Expect(!not_gz.ok && !not_gz.error.empty(), "Plain text must fail Gzip decoder");
}

} // namespace

int main() {
  TestJsonDecoderAndMinify();
  TestBase64Decoder();
  TestJwtDecoder();
  TestUrlHexUnicodeAndTimestamp();
  TestAutoDetection();
  TestGzipAndZlibDecoder();

  if (g_failures != 0) {
    std::cerr << g_failures << " decoder test assertion(s) failed\n";
    return 1;
  }
  std::cout << "test_decoder passed\n";
  return 0;
}
