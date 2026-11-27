module;

#include <arrow-adbc/adbc.h>
#include <nanoarrow/nanoarrow.h>

#include <cstdio>

module dbpp.adbc;

import std;
import dbpp.types;
import dbpp.i18n;
import dbpp.redis;

namespace dbpp {

namespace {

struct ArrayViewHolder {
  ArrowArrayView view{};
  bool initialized = false;
  ArrayViewHolder() = default;
  ArrayViewHolder(const ArrayViewHolder&) = delete;
  ArrayViewHolder& operator=(const ArrayViewHolder&) = delete;
  ~ArrayViewHolder() { Reset(); }

  void Reset() noexcept {
    if (initialized) {
      ArrowArrayViewReset(&view);
      view = ArrowArrayView{};
      initialized = false;
    }
  }
};

std::string TimeUnitLabel(ArrowTimeUnit unit) {
  switch (unit) {
    case NANOARROW_TIME_UNIT_MILLI: return "ms";
    case NANOARROW_TIME_UNIT_MICRO: return "us";
    case NANOARROW_TIME_UNIT_NANO: return "ns";
    case NANOARROW_TIME_UNIT_SECOND: return "s";
  }
  return "s";
}

void AppendJsonEscapedString(std::string& out, std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  out.push_back('"');
  for (const unsigned char ch : text) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (ch < 0x20U) {
          out += "\\u00";
          out.push_back(kHex[(ch >> 4) & 0x0FU]);
          out.push_back(kHex[ch & 0x0FU]);
        } else {
          out.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  out.push_back('"');
}

std::string FormatUuidBytes(const std::uint8_t* data) {
  char buf[40];
  std::snprintf(
      buf, sizeof(buf),
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7],
      data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15]);
  return buf;
}

std::string FormatBytesHexPreview(const std::uint8_t* data, std::int64_t len) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = "0x";
  const std::int64_t capped = std::min<std::int64_t>(len, 128);
  out.reserve(2 + static_cast<std::size_t>(capped) * 2 + 4);
  for (std::int64_t i = 0; i < capped; ++i) {
    out.push_back(kHex[(data[i] >> 4) & 0x0FU]);
    out.push_back(kHex[data[i] & 0x0FU]);
  }
  if (len > capped) {
    out += "...";
  }
  return out;
}

bool IsFiniteNumberText(std::string_view text) {
  return !text.empty() && text != "NaN" && text != "Inf" && text != "-Inf";
}

CellValue ExtractArrowCellImpl(const ArrowArrayView& view, std::int64_t row,
                               const ArrowSchema* schema, int depth, bool nested);

void AppendJsonValue(std::string& out, const ArrowArrayView& view, std::int64_t row,
                     const ArrowSchema* schema, int depth) {
  if (depth > 16) {
    out += "\"<max-depth>\"";
    return;
  }
  if (ArrowArrayViewIsNull(&view, row) != 0 || view.storage_type == NANOARROW_TYPE_NA) {
    out += "null";
    return;
  }

  ArrowSchemaView schema_view{};
  ArrowError arrow_error;
  ArrowErrorInit(&arrow_error);
  const bool has_schema =
      schema != nullptr && ArrowSchemaViewInit(&schema_view, schema, &arrow_error) == NANOARROW_OK;

  if (has_schema && schema_view.type == NANOARROW_TYPE_DICTIONARY &&
      view.dictionary != nullptr && schema->dictionary != nullptr) {
    const std::int64_t dict_idx = ArrowArrayViewGetIntUnsafe(&view, row);
    if (dict_idx < 0 || dict_idx >= view.dictionary->length) {
      out += "null";
      return;
    }
    AppendJsonValue(out, *view.dictionary, dict_idx, schema->dictionary, depth + 1);
    return;
  }

  switch (view.storage_type) {
    case NANOARROW_TYPE_BOOL:
      out += ArrowArrayViewGetIntUnsafe(&view, row) != 0 ? "true" : "false";
      return;
    case NANOARROW_TYPE_INT8:
    case NANOARROW_TYPE_INT16:
    case NANOARROW_TYPE_INT32:
    case NANOARROW_TYPE_INT64:
      if (!has_schema || schema_view.type == schema_view.storage_type) {
        out += std::to_string(ArrowArrayViewGetIntUnsafe(&view, row));
        return;
      }
      break;
    case NANOARROW_TYPE_UINT8:
    case NANOARROW_TYPE_UINT16:
    case NANOARROW_TYPE_UINT32:
    case NANOARROW_TYPE_UINT64:
      out += std::to_string(ArrowArrayViewGetUIntUnsafe(&view, row));
      return;
    case NANOARROW_TYPE_FLOAT:
    case NANOARROW_TYPE_DOUBLE:
    case NANOARROW_TYPE_HALF_FLOAT: {
      const std::string num = detail::AdbcFormatDouble(ArrowArrayViewGetDoubleUnsafe(&view, row));
      if (IsFiniteNumberText(num)) {
        out += num;
      } else {
        AppendJsonEscapedString(out, num);
      }
      return;
    }
    case NANOARROW_TYPE_LIST:
    case NANOARROW_TYPE_LARGE_LIST: {
      if (view.n_children < 1 || view.children[0] == nullptr) {
        out += "[]";
        return;
      }
      const std::int64_t idx = view.offset + row;
      const std::int64_t start = ArrowArrayViewListChildOffset(&view, idx);
      const std::int64_t end =
          (row + 1 < view.length) ? ArrowArrayViewListChildOffset(&view, idx + 1)
                                  : view.children[0]->length;
      const ArrowSchema* child_schema =
          (schema != nullptr && schema->n_children > 0) ? schema->children[0] : nullptr;
      out.push_back('[');
      for (std::int64_t i = start; i < end; ++i) {
        if (i > start) {
          out += ", ";
        }
        AppendJsonValue(out, *view.children[0], i, child_schema, depth + 1);
      }
      out.push_back(']');
      return;
    }
    case NANOARROW_TYPE_FIXED_SIZE_LIST: {
      if (view.n_children < 1 || view.children[0] == nullptr ||
          view.layout.child_size_elements <= 0) {
        out += "[]";
        return;
      }
      const std::int64_t n = view.layout.child_size_elements;
      const std::int64_t start = (view.offset + row) * n;
      const std::int64_t end = std::min(start + n, view.children[0]->length);
      const ArrowSchema* child_schema =
          (schema != nullptr && schema->n_children > 0) ? schema->children[0] : nullptr;
      out.push_back('[');
      for (std::int64_t i = start; i < end; ++i) {
        if (i > start) {
          out += ", ";
        }
        AppendJsonValue(out, *view.children[0], i, child_schema, depth + 1);
      }
      out.push_back(']');
      return;
    }
    case NANOARROW_TYPE_MAP: {
      if (view.n_children < 1 || view.children[0] == nullptr ||
          view.children[0]->n_children < 2) {
        out += "{}";
        return;
      }
      const ArrowArrayView* entries = view.children[0];
      const ArrowSchema* entry_schema =
          (schema != nullptr && schema->n_children > 0) ? schema->children[0] : nullptr;
      const ArrowSchema* key_schema =
          (entry_schema != nullptr && entry_schema->n_children > 0) ? entry_schema->children[0]
                                                                    : nullptr;
      const ArrowSchema* val_schema =
          (entry_schema != nullptr && entry_schema->n_children > 1) ? entry_schema->children[1]
                                                                    : nullptr;
      const std::int64_t idx = view.offset + row;
      const std::int64_t start = ArrowArrayViewListChildOffset(&view, idx);
      const std::int64_t end =
          (row + 1 < view.length) ? ArrowArrayViewListChildOffset(&view, idx + 1)
                                  : entries->length;
      out.push_back('{');
      for (std::int64_t i = start; i < end; ++i) {
        if (i > start) {
          out += ", ";
        }
        const CellValue key_cell =
            ExtractArrowCellImpl(*entries->children[0], i, key_schema, depth + 1, true);
        AppendJsonEscapedString(out, key_cell.is_null ? "null" : key_cell.text);
        out += ": ";
        AppendJsonValue(out, *entries->children[1], i, val_schema, depth + 1);
      }
      out.push_back('}');
      return;
    }
    case NANOARROW_TYPE_STRUCT: {
      out.push_back('{');
      for (std::int64_t c = 0; c < view.n_children; ++c) {
        if (c > 0) {
          out += ", ";
        }
        const ArrowSchema* child_schema =
            (schema != nullptr && c < schema->n_children) ? schema->children[c] : nullptr;
        const std::string field_name =
            (child_schema != nullptr && child_schema->name != nullptr)
                ? std::string(child_schema->name)
                : ("field_" + std::to_string(c));
        AppendJsonEscapedString(out, field_name);
        out += ": ";
        if (view.children[c] != nullptr) {
          AppendJsonValue(out, *view.children[c], view.offset + row, child_schema, depth + 1);
        } else {
          out += "null";
        }
      }
      out.push_back('}');
      return;
    }
    case NANOARROW_TYPE_DENSE_UNION:
    case NANOARROW_TYPE_SPARSE_UNION: {
      const std::int8_t child_idx = ArrowArrayViewUnionChildIndex(&view, row);
      const std::int64_t child_off = ArrowArrayViewUnionChildOffset(&view, row);
      if (child_idx < 0 || child_idx >= view.n_children || view.children[child_idx] == nullptr) {
        out += "null";
        return;
      }
      const ArrowSchema* child_schema =
          (schema != nullptr && child_idx < schema->n_children) ? schema->children[child_idx]
                                                                : nullptr;
      AppendJsonValue(out, *view.children[child_idx], child_off, child_schema, depth + 1);
      return;
    }
    default:
      break;
  }

  const CellValue cell = ExtractArrowCellImpl(view, row, schema, depth, true);
  if (cell.is_null) {
    out += "null";
  } else {
    AppendJsonEscapedString(out, cell.text);
  }
}

CellValue ExtractArrowCellImpl(const ArrowArrayView& view, std::int64_t row,
                               const ArrowSchema* schema, int depth, bool nested) {
  CellValue cell;
  if (depth > 16) {
    cell.text = "<max-depth>";
    return cell;
  }
  if (ArrowArrayViewIsNull(&view, row) != 0 || view.storage_type == NANOARROW_TYPE_NA) {
    cell.text = "NULL";
    cell.is_null = true;
    return cell;
  }

  ArrowSchemaView schema_view{};
  ArrowError arrow_error;
  ArrowErrorInit(&arrow_error);
  const bool has_schema =
      schema != nullptr && ArrowSchemaViewInit(&schema_view, schema, &arrow_error) == NANOARROW_OK;

  if (has_schema && schema_view.type == NANOARROW_TYPE_DICTIONARY &&
      view.dictionary != nullptr && schema->dictionary != nullptr) {
    const std::int64_t dict_idx = ArrowArrayViewGetIntUnsafe(&view, row);
    if (dict_idx < 0 || dict_idx >= view.dictionary->length) {
      cell.text = "NULL";
      cell.is_null = true;
      return cell;
    }
    return ExtractArrowCellImpl(*view.dictionary, dict_idx, schema->dictionary, depth + 1, nested);
  }

  if (view.storage_type == NANOARROW_TYPE_DENSE_UNION ||
      view.storage_type == NANOARROW_TYPE_SPARSE_UNION) {
    const std::int8_t child_idx = ArrowArrayViewUnionChildIndex(&view, row);
    const std::int64_t child_off = ArrowArrayViewUnionChildOffset(&view, row);
    if (child_idx < 0 || child_idx >= view.n_children || view.children[child_idx] == nullptr) {
      cell.text = "NULL";
      cell.is_null = true;
      return cell;
    }
    const ArrowSchema* child_schema =
        (schema != nullptr && child_idx < schema->n_children) ? schema->children[child_idx]
                                                              : nullptr;
    return ExtractArrowCellImpl(*view.children[child_idx], child_off, child_schema, depth + 1,
                                nested);
  }

  if (view.storage_type == NANOARROW_TYPE_LIST ||
      view.storage_type == NANOARROW_TYPE_LARGE_LIST ||
      view.storage_type == NANOARROW_TYPE_FIXED_SIZE_LIST ||
      view.storage_type == NANOARROW_TYPE_MAP ||
      view.storage_type == NANOARROW_TYPE_STRUCT) {
    AppendJsonValue(cell.text, view, row, schema, depth);
    return cell;
  }

  const ArrowType storage = view.storage_type;
  switch (storage) {
    case NANOARROW_TYPE_BOOL:
      cell.text = ArrowArrayViewGetIntUnsafe(&view, row) != 0 ? "true" : "false";
      return cell;
    case NANOARROW_TYPE_INT8:
    case NANOARROW_TYPE_INT16:
    case NANOARROW_TYPE_INT32:
    case NANOARROW_TYPE_INT64:
      if (!has_schema || schema_view.type == storage) {
        cell.text = std::to_string(ArrowArrayViewGetIntUnsafe(&view, row));
        return cell;
      }
      break;
    case NANOARROW_TYPE_UINT8:
    case NANOARROW_TYPE_UINT16:
    case NANOARROW_TYPE_UINT32:
    case NANOARROW_TYPE_UINT64:
      cell.text = std::to_string(ArrowArrayViewGetUIntUnsafe(&view, row));
      return cell;
    case NANOARROW_TYPE_FLOAT:
    case NANOARROW_TYPE_DOUBLE:
    case NANOARROW_TYPE_HALF_FLOAT:
      cell.text = detail::AdbcFormatDouble(ArrowArrayViewGetDoubleUnsafe(&view, row));
      return cell;
    case NANOARROW_TYPE_STRING:
    case NANOARROW_TYPE_LARGE_STRING:
    case NANOARROW_TYPE_STRING_VIEW: {
      const ArrowStringView value = ArrowArrayViewGetStringUnsafe(&view, row);
      if (value.data != nullptr) {
        const std::string_view sv(value.data, static_cast<std::size_t>(value.size_bytes));
        cell.text = IsValidUtf8(sv) ? std::string(sv) : SanitizeUtf8(sv);
      }
      return cell;
    }
    case NANOARROW_TYPE_BINARY:
    case NANOARROW_TYPE_LARGE_BINARY:
    case NANOARROW_TYPE_FIXED_SIZE_BINARY:
    case NANOARROW_TYPE_BINARY_VIEW: {
      const ArrowBufferView value = ArrowArrayViewGetBytesUnsafe(&view, row);
      if (has_schema && schema_view.extension_name.data != nullptr) {
        const std::string_view ext(
            schema_view.extension_name.data,
            static_cast<std::size_t>(schema_view.extension_name.size_bytes));
        if (ext == "arrow.uuid" && value.size_bytes == 16 && value.data.as_uint8 != nullptr) {
          cell.text = FormatUuidBytes(value.data.as_uint8);
          return cell;
        }
      }
      if (nested && value.data.as_uint8 != nullptr) {
        cell.text = FormatBytesHexPreview(value.data.as_uint8, value.size_bytes);
      } else {
        cell.text = "<blob " + std::to_string(value.size_bytes) + " bytes>";
      }
      return cell;
    }
    default:
      break;
  }

  if (has_schema) {
    switch (schema_view.type) {
      case NANOARROW_TYPE_DATE32: {
        const auto days = std::chrono::sys_days{
            std::chrono::days{ArrowArrayViewGetIntUnsafe(&view, row)}};
        cell.text = detail::AdbcFormatDate(days);
        return cell;
      }
      case NANOARROW_TYPE_DATE64: {
        const auto milliseconds = std::chrono::milliseconds{
            ArrowArrayViewGetIntUnsafe(&view, row)};
        cell.text = detail::AdbcFormatDate(std::chrono::sys_days{
            std::chrono::floor<std::chrono::days>(milliseconds)});
        return cell;
      }
      case NANOARROW_TYPE_TIMESTAMP: {
        const std::int64_t ts_val = ArrowArrayViewGetIntUnsafe(&view, row);
        const std::string_view tz =
            schema_view.timezone != nullptr ? std::string_view(schema_view.timezone)
                                            : std::string_view{};
        cell.text = detail::AdbcFormatTimestampTz(ts_val, static_cast<int>(schema_view.time_unit),
                                                  tz);
        if (!tz.empty() && !nested) {
          const std::string local_text =
              detail::AdbcFormatLocalTimestamp(ts_val, static_cast<int>(schema_view.time_unit));
          if (!local_text.empty() && local_text != cell.text) {
            cell.display_text = local_text;
          }
        }
        return cell;
      }
      case NANOARROW_TYPE_TIME32:
      case NANOARROW_TYPE_TIME64:
        cell.text = detail::AdbcFormatTimeUnit(ArrowArrayViewGetIntUnsafe(&view, row),
                                               static_cast<int>(schema_view.time_unit));
        return cell;
      case NANOARROW_TYPE_DURATION:
        cell.text = std::to_string(ArrowArrayViewGetIntUnsafe(&view, row)) + " " +
                    TimeUnitLabel(schema_view.time_unit);
        return cell;
      case NANOARROW_TYPE_DECIMAL32:
      case NANOARROW_TYPE_DECIMAL64:
      case NANOARROW_TYPE_DECIMAL128:
      case NANOARROW_TYPE_DECIMAL256: {
        ArrowDecimal decimal;
        ArrowDecimalInit(&decimal, schema_view.decimal_bitwidth, schema_view.decimal_precision,
                         schema_view.decimal_scale);
        ArrowArrayViewGetDecimalUnsafe(&view, row, &decimal);
        ArrowBuffer buffer;
        ArrowBufferInit(&buffer);
        if (ArrowDecimalAppendStringToBuffer(&decimal, &buffer) == NANOARROW_OK) {
          cell.text.assign(reinterpret_cast<const char*>(buffer.data), buffer.size_bytes);
        } else {
          cell.text = "<decimal>";
        }
        ArrowBufferReset(&buffer);
        return cell;
      }
      default:
        break;
    }
    cell.text = std::string("<") + ArrowTypeString(schema_view.type) + ">";
  } else {
    cell.text = std::string("<") + ArrowTypeString(view.storage_type) + ">";
  }
  return cell;
}

void WriteCsvField(std::ostream& out, std::string_view value) {
  bool needs_quotes = false;
  for (const char ch : value) {
    if (ch == ',' || ch == '"' || ch == '\n' || ch == '\r') {
      needs_quotes = true;
      break;
    }
  }
  if (!needs_quotes) {
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
    return;
  }
  out.put('"');
  for (const char ch : value) {
    if (ch == '"') {
      out.put('"');
    }
    out.put(ch);
  }
  out.put('"');
}

void ApplyStreamError(ArrowArrayStream& stream, AdbcQueryResult& out) {
  AdbcStatusCode stream_status = ADBC_STATUS_OK;
  const detail::AdbcCapturedError captured =
      detail::AdbcCaptureStreamError(stream, &stream_status);
  out.error = captured.message;
  out.sqlstate = captured.sqlstate;
  out.vendor_code = captured.vendor_code;
  out.error_details = captured.details;
  if (stream_status == ADBC_STATUS_CANCELLED) {
    out.cancelled = true;
  }
  if (IsConnectionLostError(stream_status, out.error, out.sqlstate)) {
    out.connection_lost = true;
  }
}

} // namespace

namespace detail {

bool AdbcConsumeStreamRows(ArrowArrayStream& stream, std::size_t max_rows,
                           AdbcSession* cancel_session, AdbcQueryResult& out) {
  AdbcSchemaHolder schema;
  if (stream.get_schema(&stream, &schema.schema) != 0) {
    ApplyStreamError(stream, out);
    return false;
  }

  const std::int64_t field_count = schema.schema.n_children;
  out.columns.reserve(static_cast<std::size_t>(field_count));
  for (std::int64_t index = 0; index < field_count; ++index) {
    const ArrowSchema* field = schema.schema.children[index];
    out.columns.push_back(field != nullptr && field->name != nullptr
                              ? std::string(field->name)
                              : ("column " + std::to_string(index + 1)));
  }

  {
    ArrayViewHolder schema_check;
    ArrowError arrow_error;
    ArrowErrorInit(&arrow_error);
    if (ArrowArrayViewInitFromSchema(&schema_check.view, &schema.schema, &arrow_error) !=
        NANOARROW_OK) {
      out.error = detail::AdbcDecodeSystemTextToUtf8(arrow_error.message);
      return false;
    }
    schema_check.initialized = true;
  }

  bool stop = false;
  while (!stop) {
    if (cancel_session != nullptr &&
        cancel_session->cancel_requested.load(std::memory_order_acquire)) {
      out.cancelled = true;
      out.error = Tr("Query cancelled", "查询已取消");
      break;
    }

    AdbcArrayHolder array;
    if (stream.get_next(&stream, &array.array) != 0) {
      ApplyStreamError(stream, out);
      break;
    }
    if (array.array.release == nullptr) {
      break;
    }

    ArrayViewHolder root;
    ArrowError arrow_error;
    ArrowErrorInit(&arrow_error);
    if (ArrowArrayViewInitFromSchema(&root.view, &schema.schema, &arrow_error) != NANOARROW_OK) {
      out.error = detail::AdbcDecodeSystemTextToUtf8(arrow_error.message);
      break;
    }
    root.initialized = true;
    ArrowErrorInit(&arrow_error);
    if (ArrowArrayViewSetArray(&root.view, &array.array, &arrow_error) != NANOARROW_OK) {
      out.error = detail::AdbcDecodeSystemTextToUtf8(arrow_error.message);
      break;
    }

    const std::int64_t rows = array.array.length;
    for (std::int64_t row = 0; row < rows; ++row) {
      if (cancel_session != nullptr &&
          cancel_session->cancel_requested.load(std::memory_order_acquire)) {
        out.cancelled = true;
        out.error = Tr("Query cancelled", "查询已取消");
        stop = true;
        break;
      }
      if (max_rows > 0 && out.rows.size() >= max_rows) {
        out.truncated = true;
        stop = true;
        break;
      }
      GridRow grid_row;
      grid_row.values.reserve(static_cast<std::size_t>(field_count));
      for (std::int64_t column = 0; column < field_count; ++column) {
        grid_row.values.push_back(
            ExtractArrowCellImpl(*root.view.children[column], row, schema.schema.children[column],
                                 0, false));
      }
      out.rows.push_back(std::move(grid_row));
    }
  }
  if (out.error.empty()) {
    ApplyResultTimestampLocalization(out.columns, nullptr, out.rows);
  }
  return out.error.empty();
}

void AdbcPopulateColumnsFromArrowSchema(const ArrowSchema& schema, const DbObject* hint_object,
                                        std::vector<ColumnMeta>& out) {
  out.clear();
  for (std::int64_t index = 0; index < schema.n_children; ++index) {
    const ArrowSchema* field = schema.children[index];
    ColumnMeta column;
    column.name = (field != nullptr && field->name != nullptr)
                      ? std::string(field->name)
                      : ("column " + std::to_string(index + 1));
    if (field != nullptr) {
      ArrowSchemaView view{};
      ArrowError arrow_error;
      ArrowErrorInit(&arrow_error);
      if (ArrowSchemaViewInit(&view, field, &arrow_error) == NANOARROW_OK) {
        column.type = ArrowTypeString(view.type);
        if (view.extension_name.data != nullptr && view.extension_name.size_bytes > 0) {
          column.extension_name.assign(view.extension_name.data,
                                       static_cast<std::size_t>(view.extension_name.size_bytes));
        }
        if (view.type == NANOARROW_TYPE_DECIMAL32 || view.type == NANOARROW_TYPE_DECIMAL64 ||
            view.type == NANOARROW_TYPE_DECIMAL128 || view.type == NANOARROW_TYPE_DECIMAL256) {
          column.column_size = view.decimal_precision;
          column.decimal_digits = static_cast<std::int16_t>(view.decimal_scale);
        }
      } else {
        column.type = "ANY";
      }
      column.nullable = (field->flags & ARROW_FLAG_NULLABLE) != 0;
    } else {
      column.type = "ANY";
    }

    if (hint_object != nullptr) {
      for (const ColumnMeta& known : hint_object->columns) {
        if (known.name == column.name) {
          if (!known.type.empty()) {
            column.type = known.type;
          }
          if (column.default_expr.empty()) {
            column.default_expr = known.default_expr;
          }
          if (column.remarks.empty()) {
            column.remarks = known.remarks;
          }
          if (!column.column_size.has_value()) {
            column.column_size = known.column_size;
          }
          if (!column.decimal_digits.has_value()) {
            column.decimal_digits = known.decimal_digits;
          }
          column.unique = column.unique || known.unique;
          column.autoincrement = column.autoincrement || known.autoincrement;
          column.generated = column.generated || known.generated;
          break;
        }
      }
      for (std::size_t key_index = 0; key_index < hint_object->primary_key.size(); ++key_index) {
        if (hint_object->primary_key[key_index] == column.name ||
            detail::AdbcEqualsIgnoreCase(hint_object->primary_key[key_index], column.name)) {
          column.primary_key = true;
          column.primary_key_order = static_cast<int>(key_index + 1);
          break;
        }
      }
    }
    out.push_back(std::move(column));
  }
}

std::string AdbcFormatArrowSqlLiteral(const ArrowArrayView& view, std::int64_t row,
                                      const ArrowSchema* schema, Dialect dialect,
                                      std::string_view declared_type) {
  if (row < 0 || ArrowArrayViewIsNull(&view, row) != 0 ||
      view.storage_type == NANOARROW_TYPE_NA) {
    return "NULL";
  }

  if (view.storage_type == NANOARROW_TYPE_BINARY ||
      view.storage_type == NANOARROW_TYPE_LARGE_BINARY ||
      view.storage_type == NANOARROW_TYPE_FIXED_SIZE_BINARY ||
      view.storage_type == NANOARROW_TYPE_BINARY_VIEW) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    const ArrowBufferView bytes = ArrowArrayViewGetBytesUnsafe(&view, row);
    const auto* raw = reinterpret_cast<const std::uint8_t*>(bytes.data.data);
    std::string out = dialect == Dialect::Postgresql ? "E'\\\\x" : "X'";
    out.reserve(out.size() + static_cast<std::size_t>(bytes.size_bytes) * 2 + 1);
    for (std::int64_t i = 0; i < bytes.size_bytes; ++i) {
      out.push_back(kHex[(raw[i] >> 4) & 0x0FU]);
      out.push_back(kHex[raw[i] & 0x0FU]);
    }
    out.push_back('\'');
    return out;
  }

  const CellValue cell = ExtractArrowCellImpl(view, row, schema, 0, false);
  if (cell.is_null) {
    return "NULL";
  }
  std::string effective_type(declared_type);
  if (effective_type.empty() || AdbcEqualsIgnoreCase(effective_type, "ANY")) {
    switch (view.storage_type) {
      case NANOARROW_TYPE_BOOL:
        effective_type = "BOOLEAN";
        break;
      case NANOARROW_TYPE_INT8:
      case NANOARROW_TYPE_UINT8:
      case NANOARROW_TYPE_INT16:
      case NANOARROW_TYPE_UINT16:
      case NANOARROW_TYPE_INT32:
      case NANOARROW_TYPE_UINT32:
      case NANOARROW_TYPE_INT64:
      case NANOARROW_TYPE_UINT64:
        effective_type = "INTEGER";
        break;
      case NANOARROW_TYPE_HALF_FLOAT:
      case NANOARROW_TYPE_FLOAT:
      case NANOARROW_TYPE_DOUBLE:
      case NANOARROW_TYPE_DECIMAL32:
      case NANOARROW_TYPE_DECIMAL64:
      case NANOARROW_TYPE_DECIMAL128:
      case NANOARROW_TYPE_DECIMAL256:
        effective_type = "DOUBLE";
        break;
      default:
        effective_type = "TEXT";
        break;
    }
  }
  return SqlLiteral(dialect, cell.text, effective_type, false);
}

} // namespace detail

ExportStreamResult ExportQueryStreamBlocking(AdbcSession& session, std::string sql,
                                             ExportFormat format, std::ostream& out,
                                             std::size_t max_rows) {
  ExportStreamResult result;
  if (session.IsClosed()) {
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }
  if (!session.connection_created) {
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }

  session.cancel_requested.store(false, std::memory_order_release);
  detail::AdbcErrorGuard error;
  detail::AdbcStatementGuard statement_guard;
  AdbcStatusCode status =
      AdbcStatementNew(&session.connection, &statement_guard.statement, error.Ptr());
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    return result;
  }
  statement_guard.created = true;

  status = AdbcStatementSetSqlQuery(&statement_guard.statement, sql.c_str(), error.Ptr());
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    return result;
  }

  detail::AdbcStreamHolder stream;
  detail::AdbcRegisterActiveStatement(session, &statement_guard.statement);
  status = AdbcStatementExecuteQuery(&statement_guard.statement, &stream.stream, nullptr,
                                     error.Ptr());
  detail::AdbcClearActiveStatement(session, &statement_guard.statement);
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.cancelled = (status == ADBC_STATUS_CANCELLED) ||
                       session.cancel_requested.load(std::memory_order_acquire);
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    return result;
  }

  detail::AdbcSchemaHolder schema;
  if (stream.stream.get_schema(&stream.stream, &schema.schema) != 0) {
    AdbcStatusCode stream_status = ADBC_STATUS_OK;
    const auto cap = detail::AdbcCaptureStreamError(stream.stream, &stream_status);
    result.error = cap.message;
    result.cancelled = (stream_status == ADBC_STATUS_CANCELLED);
    result.connection_lost = IsConnectionLostError(stream_status, cap.message, cap.sqlstate);
    return result;
  }

  const std::int64_t field_count = schema.schema.n_children;
  std::vector<std::string> column_names;
  column_names.reserve(static_cast<std::size_t>(field_count));
  for (std::int64_t c = 0; c < field_count; ++c) {
    const ArrowSchema* field = schema.schema.children[c];
    column_names.push_back(field != nullptr && field->name != nullptr
                               ? std::string(field->name)
                               : ("column_" + std::to_string(c + 1)));
  }

  if (format == ExportFormat::Csv && field_count > 0) {
    for (std::int64_t c = 0; c < field_count; ++c) {
      if (c > 0) {
        out.put(',');
      }
      WriteCsvField(out, column_names[static_cast<std::size_t>(c)]);
    }
    out.put('\n');
  }

  bool stop = false;
  while (!stop) {
    if (session.cancel_requested.load(std::memory_order_acquire)) {
      result.cancelled = true;
      result.error = Tr("Export cancelled", "导出已取消");
      break;
    }
    detail::AdbcArrayHolder array;
    if (stream.stream.get_next(&stream.stream, &array.array) != 0) {
      AdbcStatusCode stream_status = ADBC_STATUS_OK;
      const auto cap = detail::AdbcCaptureStreamError(stream.stream, &stream_status);
      result.error = cap.message;
      result.cancelled = (stream_status == ADBC_STATUS_CANCELLED);
      result.connection_lost = IsConnectionLostError(stream_status, cap.message, cap.sqlstate);
      break;
    }
    if (array.array.release == nullptr) {
      break;
    }

    ArrayViewHolder root;
    ArrowError arrow_error;
    ArrowErrorInit(&arrow_error);
    if (ArrowArrayViewInitFromSchema(&root.view, &schema.schema, &arrow_error) != NANOARROW_OK) {
      result.error = detail::AdbcDecodeSystemTextToUtf8(arrow_error.message);
      break;
    }
    root.initialized = true;
    ArrowErrorInit(&arrow_error);
    if (ArrowArrayViewSetArray(&root.view, &array.array, &arrow_error) != NANOARROW_OK) {
      result.error = detail::AdbcDecodeSystemTextToUtf8(arrow_error.message);
      break;
    }

    for (std::int64_t row = 0; row < array.array.length; ++row) {
      if (max_rows > 0 && result.rows_written >= max_rows) {
        result.truncated = true;
        stop = true;
        break;
      }
      if (format == ExportFormat::Csv) {
        for (std::int64_t c = 0; c < field_count; ++c) {
          if (c > 0) {
            out.put(',');
          }
          const CellValue cell = ExtractArrowCellImpl(
              *root.view.children[c], row, schema.schema.children[c], 0, true);
          if (!cell.is_null) {
            WriteCsvField(out, cell.text);
          }
        }
        out.put('\n');
      } else {
        std::string line;
        line.push_back('{');
        for (std::int64_t c = 0; c < field_count; ++c) {
          if (c > 0) {
            line += ", ";
          }
          AppendJsonEscapedString(line, column_names[static_cast<std::size_t>(c)]);
          line += ": ";
          AppendJsonValue(line, *root.view.children[c], row, schema.schema.children[c], 0);
        }
        line += "}\n";
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
      }
      if (!out.good()) {
        result.error = Tr("Failed while writing export stream", "写入导出流失败");
        stop = true;
        break;
      }
      ++result.rows_written;
    }
  }
  out.flush();
  return result;
}

ExportStreamResult ExportQueryToFileBlocking(AdbcSession& session, std::string sql,
                                             ExportFormat format,
                                             const std::filesystem::path& path,
                                             std::size_t max_rows) {
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    ExportStreamResult res;
    res.error = TrFmt("Failed to open output file: {0}", "无法打开导出文件: {0}", {path.string()});
    return res;
  }
  return ExportQueryStreamBlocking(session, std::move(sql), format, file, max_rows);
}

std::string MakeTempExportPath(ExportFormat format) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::string ext = (format == ExportFormat::Csv) ? ".csv" : ".jsonl";
  return (std::filesystem::temp_directory_path() /
          ("dbpp_export_" + std::to_string(stamp) + ext))
      .string();
}

void RemoveFileQuietly(std::string_view path) {
  std::error_code ec;
  std::filesystem::remove(std::filesystem::path(path), ec);
}

ExportStreamResult ExportQueryToFilePathBlocking(AdbcSession& session, std::string sql,
                                                 ExportFormat format, std::string_view path,
                                                 std::size_t max_rows) {
  return ExportQueryToFileBlocking(session, std::move(sql), format, std::filesystem::path(path),
                                   max_rows);
}

} // namespace dbpp
