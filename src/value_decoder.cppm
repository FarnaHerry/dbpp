export module dbpp.decoder;

import std;
import dbpp.i18n;

export namespace dbpp {

enum class ValueDecoderKind {
  Raw,
  Json,
  Base64,
  Gzip,
  Jwt,
  Url,
  Hex,
  Unicode,
  Timestamp,
};

struct DecodedValueResult {
  ValueDecoderKind kind = ValueDecoderKind::Raw;
  bool ok = true;
  std::string text;
  std::string subtitle;
  std::string error;
};

[[nodiscard]] std::string_view DecoderDisplayName(ValueDecoderKind kind) noexcept;
[[nodiscard]] ValueDecoderKind DetectSuggestedDecoder(std::string_view raw) noexcept;
[[nodiscard]] DecodedValueResult DecodeValue(std::string_view raw, ValueDecoderKind kind);
[[nodiscard]] DecodedValueResult MinifyJsonValue(std::string_view raw);
[[nodiscard]] bool InflateRawDeflateBytes(std::string_view compressed, std::string& out,
                                          std::string& error_out,
                                          std::size_t max_output_bytes = 64u * 1024u * 1024u);

} // namespace dbpp

namespace dbpp::detail {

[[nodiscard]] std::string_view DecoderTrimAscii(std::string_view text) noexcept;
[[nodiscard]] int DecoderHexDigitValue(char c) noexcept;
void DecoderAppendCodepointUtf8(std::uint32_t cp, std::string& out);
[[nodiscard]] bool DecoderParseHex4(std::string_view s, std::size_t pos,
                                    std::uint16_t& out) noexcept;
[[nodiscard]] bool DecoderFormatJsonString(std::string_view raw, bool pretty, std::string& out,
                                            std::string& error);

enum class InflateContainerKind {
  None,
  Gzip,
  Zlib,
};

[[nodiscard]] InflateContainerKind DetectCompressedContainer(std::string_view bytes) noexcept;
[[nodiscard]] bool InflateGzipOrZlib(std::string_view bytes, InflateContainerKind& kind_out,
                                     std::string& out, std::string& error_out);

} // namespace dbpp::detail
