module;

#include <arrow-adbc/adbc.h>
#include <nanoarrow/nanoarrow.h>

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

std::string StringAt(const ArrowArrayView* view, std::int64_t index) {
  if (view == nullptr || index < 0 || index >= view->length ||
      ArrowArrayViewIsNull(view, index) != 0) {
    return {};
  }
  const ArrowStringView value = ArrowArrayViewGetStringUnsafe(view, index);
  if (value.data == nullptr) {
    return {};
  }
  return std::string(value.data, static_cast<std::size_t>(value.size_bytes));
}

std::optional<std::int64_t> IntAt(const ArrowArrayView* view, std::int64_t index) {
  if (view == nullptr || index < 0 || index >= view->length ||
      ArrowArrayViewIsNull(view, index) != 0) {
    return std::nullopt;
  }
  return ArrowArrayViewGetIntUnsafe(view, index);
}

std::optional<bool> BoolAt(const ArrowArrayView* view, std::int64_t index) {
  if (view == nullptr || index < 0 || index >= view->length ||
      ArrowArrayViewIsNull(view, index) != 0) {
    return std::nullopt;
  }
  return ArrowArrayViewGetIntUnsafe(view, index) != 0;
}

std::int64_t ListStart(const ArrowArrayView* list, std::int64_t index) {
  if (list == nullptr || list->n_children == 0 || index < 0 || index >= list->length) {
    return 0;
  }
  return ArrowArrayViewListChildOffset(list, list->offset + index);
}

std::int64_t ListEnd(const ArrowArrayView* list, std::int64_t index) {
  if (list == nullptr || list->n_children == 0) {
    return 0;
  }
  if (index + 1 < list->length) {
    return ArrowArrayViewListChildOffset(list, list->offset + index + 1);
  }
  return list->children[0]->length;
}

std::string FormatStatValueUnion(const ArrowArrayView* value_union, std::int64_t row,
                                 std::optional<std::int64_t>& int_out) {
  int_out.reset();
  if (value_union == nullptr) {
    return {};
  }
  const std::int8_t child_idx = ArrowArrayViewUnionChildIndex(value_union, row);
  const std::int64_t child_off = ArrowArrayViewUnionChildOffset(value_union, row);
  if (child_idx < 0 || child_idx >= value_union->n_children ||
      value_union->children[child_idx] == nullptr) {
    return {};
  }
  const ArrowArrayView* child = value_union->children[child_idx];
  if (ArrowArrayViewIsNull(child, child_off) != 0) {
    return {};
  }
  switch (child->storage_type) {
    case NANOARROW_TYPE_INT64:
    case NANOARROW_TYPE_INT32: {
      const std::int64_t v = ArrowArrayViewGetIntUnsafe(child, child_off);
      int_out = v;
      return std::to_string(v);
    }
    case NANOARROW_TYPE_UINT64:
    case NANOARROW_TYPE_UINT32: {
      const std::uint64_t v = ArrowArrayViewGetUIntUnsafe(child, child_off);
      if (v <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        int_out = static_cast<std::int64_t>(v);
      }
      return std::to_string(v);
    }
    case NANOARROW_TYPE_FLOAT:
    case NANOARROW_TYPE_DOUBLE: {
      const double d = ArrowArrayViewGetDoubleUnsafe(child, child_off);
      if (std::isfinite(d) && d >= 0.0 &&
          d <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        int_out = static_cast<std::int64_t>(std::llround(d));
      }
      return detail::AdbcFormatDouble(d);
    }
    case NANOARROW_TYPE_BINARY:
    case NANOARROW_TYPE_LARGE_BINARY: {
      const ArrowBufferView bytes = ArrowArrayViewGetBytesUnsafe(child, child_off);
      if (bytes.data.as_char != nullptr && bytes.size_bytes > 0) {
        const std::string_view sv(bytes.data.as_char, static_cast<std::size_t>(bytes.size_bytes));
        return IsValidUtf8(sv) ? std::string(sv) : ("<blob " + std::to_string(bytes.size_bytes) + " bytes>");
      }
      return {};
    }
    default:
      return {};
  }
}

std::string StandardStatName(std::int16_t key) {
  switch (key) {
    case ADBC_STATISTIC_AVERAGE_BYTE_WIDTH_KEY: return ADBC_STATISTIC_AVERAGE_BYTE_WIDTH_NAME;
    case ADBC_STATISTIC_DISTINCT_COUNT_KEY: return ADBC_STATISTIC_DISTINCT_COUNT_NAME;
    case ADBC_STATISTIC_MAX_BYTE_WIDTH_KEY: return ADBC_STATISTIC_MAX_BYTE_WIDTH_NAME;
    case ADBC_STATISTIC_MAX_VALUE_KEY: return ADBC_STATISTIC_MAX_VALUE_NAME;
    case ADBC_STATISTIC_MIN_VALUE_KEY: return ADBC_STATISTIC_MIN_VALUE_NAME;
    case ADBC_STATISTIC_NULL_COUNT_KEY: return ADBC_STATISTIC_NULL_COUNT_NAME;
    case ADBC_STATISTIC_ROW_COUNT_KEY: return ADBC_STATISTIC_ROW_COUNT_NAME;
    default: return "stat_" + std::to_string(key);
  }
}

} // namespace

namespace detail {

AdbcObjectsResult AdbcLoadObjectsBlockingOnce(AdbcSession& session, AdbcStatusCode& status_out) {
  AdbcObjectsResult result;
  if (!session.connection_created) {
    status_out = ADBC_STATUS_INVALID_STATE;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }

  AdbcErrorGuard error;
  AdbcStreamHolder stream;
  status_out = AdbcConnectionGetObjects(&session.connection, ADBC_OBJECT_DEPTH_ALL, nullptr, nullptr,
                                        nullptr, nullptr, nullptr, &stream.stream, error.Ptr());
  if (status_out != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.sqlstate = cap.sqlstate;
    return result;
  }

  AdbcSchemaHolder schema;
  if (stream.stream.get_schema(&stream.stream, &schema.schema) != 0) {
    const auto cap = AdbcCaptureStreamError(stream.stream);
    result.error = cap.message;
    result.sqlstate = cap.sqlstate;
    return result;
  }

  while (true) {
    AdbcArrayHolder array;
    if (stream.stream.get_next(&stream.stream, &array.array) != 0) {
      const auto cap = AdbcCaptureStreamError(stream.stream);
      result.error = cap.message;
      result.sqlstate = cap.sqlstate;
      return result;
    }
    if (array.array.release == nullptr) {
      break;
    }

    ArrayViewHolder root;
    ArrowError arrow_error;
    ArrowErrorInit(&arrow_error);
    if (ArrowArrayViewInitFromSchema(&root.view, &schema.schema, &arrow_error) != NANOARROW_OK) {
      result.error = AdbcDecodeSystemTextToUtf8(arrow_error.message);
      return result;
    }
    root.initialized = true;
    ArrowErrorInit(&arrow_error);
    if (ArrowArrayViewSetArray(&root.view, &array.array, &arrow_error) != NANOARROW_OK) {
      result.error = AdbcDecodeSystemTextToUtf8(arrow_error.message);
      return result;
    }

    const ArrowArrayView* catalogs = root.view.n_children > 1 ? root.view.children[1] : nullptr;
    for (std::int64_t catalog_index = 0; catalog_index < root.view.array->length; ++catalog_index) {
      const std::string catalog_name =
          root.view.n_children > 0 ? StringAt(root.view.children[0], catalog_index) : std::string{};
      if (catalogs == nullptr || ArrowArrayViewIsNull(catalogs, catalog_index) != 0) {
        continue;
      }
      const std::int64_t schema_start = ListStart(catalogs, catalog_index);
      const std::int64_t schema_end = ListEnd(catalogs, catalog_index);
      const ArrowArrayView* schema_struct = catalogs->children[0];
      for (std::int64_t schema_index = schema_start; schema_index < schema_end; ++schema_index) {
        const std::string db_schema = StringAt(schema_struct->children[0], schema_index);
        const ArrowArrayView* tables = schema_struct->children[1];
        if (ArrowArrayViewIsNull(tables, schema_index) != 0) {
          continue;
        }
        const std::int64_t table_start = ListStart(tables, schema_index);
        const std::int64_t table_end = ListEnd(tables, schema_index);
        const ArrowArrayView* table_struct = tables->children[0];
        for (std::int64_t table_index = table_start; table_index < table_end; ++table_index) {
          DbObject object;
          object.catalog = catalog_name;
          object.db_schema = db_schema;
          object.name = StringAt(table_struct->children[0], table_index);
          object.table_type = StringAt(table_struct->children[1], table_index);
          object.is_view = AdbcContainsLower(object.table_type, "view");

          const ArrowArrayView* columns_list =
              table_struct->n_children > 2 ? table_struct->children[2] : nullptr;
          if (columns_list != nullptr && ArrowArrayViewIsNull(columns_list, table_index) == 0) {
            const std::int64_t column_start = ListStart(columns_list, table_index);
            const std::int64_t column_end = ListEnd(columns_list, table_index);
            const ArrowArrayView* cs = columns_list->children[0];
            for (std::int64_t col_idx = column_start; col_idx < column_end; ++col_idx) {
              ColumnMeta column;
              column.name = cs->n_children > 0 ? StringAt(cs->children[0], col_idx) : std::string{};
              if (cs->n_children > 2) {
                column.remarks = StringAt(cs->children[2], col_idx);
              }
              if (cs->n_children > 4) {
                column.type = StringAt(cs->children[4], col_idx);
              }
              if (cs->n_children > 5) {
                if (const auto sz = IntAt(cs->children[5], col_idx)) {
                  column.column_size = static_cast<std::int32_t>(*sz);
                }
              }
              if (cs->n_children > 6) {
                if (const auto digits = IntAt(cs->children[6], col_idx)) {
                  column.decimal_digits = static_cast<std::int16_t>(*digits);
                }
              }
              if (cs->n_children > 8) {
                if (const auto n = IntAt(cs->children[8], col_idx)) {
                  column.nullable = (*n != 0);
                }
              }
              if (cs->n_children > 9) {
                column.default_expr = StringAt(cs->children[9], col_idx);
              }
              if (cs->n_children > 13) {
                const std::string is_null_str = AdbcLowerAscii(StringAt(cs->children[13], col_idx));
                if (is_null_str == "no") {
                  column.nullable = false;
                } else if (is_null_str == "yes") {
                  column.nullable = true;
                }
              }
              if (cs->n_children > 17) {
                if (const auto b = BoolAt(cs->children[17], col_idx)) {
                  column.autoincrement = *b;
                }
              }
              if (cs->n_children > 18) {
                if (const auto b = BoolAt(cs->children[18], col_idx)) {
                  column.generated = *b;
                }
              }
              object.columns.push_back(std::move(column));
            }
          }

          const ArrowArrayView* constraints_list =
              table_struct->n_children > 3 ? table_struct->children[3] : nullptr;
          if (constraints_list != nullptr &&
              ArrowArrayViewIsNull(constraints_list, table_index) == 0) {
            const std::int64_t constraint_start = ListStart(constraints_list, table_index);
            const std::int64_t constraint_end = ListEnd(constraints_list, table_index);
            const ArrowArrayView* constraint_struct = constraints_list->children[0];
            for (std::int64_t ci = constraint_start; ci < constraint_end; ++ci) {
              TableConstraintMeta constraint;
              if (constraint_struct->n_children > 0) {
                constraint.name = StringAt(constraint_struct->children[0], ci);
              }
              if (constraint_struct->n_children > 1) {
                constraint.type = StringAt(constraint_struct->children[1], ci);
              }
              const std::string type_lower = AdbcLowerAscii(constraint.type);

              const ArrowArrayView* names =
                  constraint_struct->n_children > 2 ? constraint_struct->children[2] : nullptr;
              if (names != nullptr && ArrowArrayViewIsNull(names, ci) == 0) {
                const std::int64_t name_start = ListStart(names, ci);
                const std::int64_t name_end = ListEnd(names, ci);
                for (std::int64_t ni = name_start; ni < name_end; ++ni) {
                  constraint.columns.push_back(StringAt(names->children[0], ni));
                }
              }

              if (type_lower == "primary key") {
                for (const std::string& pk_col : constraint.columns) {
                  object.primary_key.push_back(pk_col);
                }
              } else if (type_lower == "unique" && constraint.columns.size() == 1) {
                for (ColumnMeta& col : object.columns) {
                  if (col.name == constraint.columns.front()) {
                    col.unique = true;
                  }
                }
              } else if (type_lower == "foreign key") {
                ForeignKeyMeta fk;
                fk.constraint_name = constraint.name;
                fk.columns = constraint.columns;
                const ArrowArrayView* usage =
                    constraint_struct->n_children > 3 ? constraint_struct->children[3] : nullptr;
                if (usage != nullptr && ArrowArrayViewIsNull(usage, ci) == 0) {
                  const std::int64_t u_start = ListStart(usage, ci);
                  const std::int64_t u_end = ListEnd(usage, ci);
                  const ArrowArrayView* us = usage->children[0];
                  for (std::int64_t ui = u_start; ui < u_end; ++ui) {
                    if (ui == u_start) {
                      if (us->n_children > 0) fk.fk_catalog = StringAt(us->children[0], ui);
                      if (us->n_children > 1) fk.fk_db_schema = StringAt(us->children[1], ui);
                      if (us->n_children > 2) fk.fk_table = StringAt(us->children[2], ui);
                    }
                    if (us->n_children > 3) {
                      fk.fk_columns.push_back(StringAt(us->children[3], ui));
                    }
                  }
                }
                object.foreign_keys.push_back(std::move(fk));
              }

              if (!constraint.type.empty()) {
                object.constraints.push_back(std::move(constraint));
              }
            }
          }

          if (!object.name.empty()) {
            result.objects.push_back(std::move(object));
          }
        }
      }
    }
  }

  std::ranges::sort(result.objects, [](const DbObject& left, const DbObject& right) {
    if (left.is_view != right.is_view) {
      return !left.is_view;
    }
    if (left.db_schema != right.db_schema) {
      return left.db_schema < right.db_schema;
    }
    return left.name < right.name;
  });
  return result;
}

AdbcSchemaResult AdbcLoadSchemaBlockingOnce(AdbcSession& session, const DbObject& object,
                                            AdbcStatusCode& status_out) {
  AdbcSchemaResult result;
  if (!session.connection_created) {
    status_out = ADBC_STATUS_INVALID_STATE;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }

  AdbcErrorGuard error;
  AdbcSchemaHolder schema;
  const char* catalog = object.catalog.empty() ? nullptr : object.catalog.c_str();
  const char* db_schema = object.db_schema.empty() ? nullptr : object.db_schema.c_str();

  status_out = AdbcConnectionGetTableSchema(&session.connection, catalog, db_schema,
                                            object.name.c_str(), &schema.schema, error.Ptr());
  if (status_out != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.sqlstate = cap.sqlstate;
    result.error_details = cap.details;
    result.columns = object.columns;
    return result;
  }

  AdbcPopulateColumnsFromArrowSchema(schema.schema, &object, result.columns);
  if (result.columns.empty()) {
    result.columns = object.columns;
  }
  return result;
}

} // namespace detail

AdbcConnectionInfo LoadConnectionInfoBlocking(AdbcSession& session) {
  AdbcConnectionInfo info;
  if (session.IsClosed()) {
    return info;
  }
  if (session.dialect == Dialect::Redis) {
    info.vendor_name = "Redis";
    info.driver_name = "Built-in RESP2/RESP3 Client";
    info.supported = true;
    return info;
  }
  if (!session.connection_created) {
    return info;
  }

  detail::AdbcErrorGuard error;
  detail::AdbcStreamHolder stream;
  const AdbcStatusCode status =
      AdbcConnectionGetInfo(&session.connection, nullptr, 0, &stream.stream, error.Ptr());
  if (status != ADBC_STATUS_OK) {
    return info;
  }

  detail::AdbcSchemaHolder schema;
  if (stream.stream.get_schema(&stream.stream, &schema.schema) != 0) {
    return info;
  }

  info.supported = true;
  while (true) {
    detail::AdbcArrayHolder array;
    if (stream.stream.get_next(&stream.stream, &array.array) != 0 ||
        array.array.release == nullptr) {
      break;
    }

    ArrayViewHolder root;
    ArrowError arrow_error;
    ArrowErrorInit(&arrow_error);
    if (ArrowArrayViewInitFromSchema(&root.view, &schema.schema, &arrow_error) != NANOARROW_OK) {
      break;
    }
    root.initialized = true;
    ArrowErrorInit(&arrow_error);
    if (ArrowArrayViewSetArray(&root.view, &array.array, &arrow_error) != NANOARROW_OK) {
      break;
    }
    if (root.view.n_children < 2) {
      continue;
    }

    const ArrowArrayView* info_names = root.view.children[0];
    const ArrowArrayView* info_values = root.view.children[1];
    for (std::int64_t row = 0; row < array.array.length; ++row) {
      const std::uint64_t code = ArrowArrayViewGetUIntUnsafe(info_names, row);
      const std::int8_t child_idx = ArrowArrayViewUnionChildIndex(info_values, row);
      const std::int64_t child_off = ArrowArrayViewUnionChildOffset(info_values, row);
      if (child_idx < 0 || child_idx >= info_values->n_children ||
          info_values->children[child_idx] == nullptr) {
        continue;
      }
      const ArrowArrayView* val_child = info_values->children[child_idx];
      if (ArrowArrayViewIsNull(val_child, child_off) != 0) {
        continue;
      }
      const auto as_string = [&]() -> std::string {
        if (val_child->storage_type == NANOARROW_TYPE_STRING ||
            val_child->storage_type == NANOARROW_TYPE_LARGE_STRING) {
          return StringAt(val_child, child_off);
        }
        return {};
      };
      const auto as_bool = [&]() -> std::optional<bool> {
        if (val_child->storage_type == NANOARROW_TYPE_BOOL) {
          return ArrowArrayViewGetIntUnsafe(val_child, child_off) != 0;
        }
        return std::nullopt;
      };
      const auto as_int64 = [&]() -> std::optional<std::int64_t> {
        if (val_child->storage_type == NANOARROW_TYPE_INT64 ||
            val_child->storage_type == NANOARROW_TYPE_INT32) {
          return ArrowArrayViewGetIntUnsafe(val_child, child_off);
        }
        return std::nullopt;
      };

      switch (code) {
        case ADBC_INFO_VENDOR_NAME: info.vendor_name = as_string(); break;
        case ADBC_INFO_VENDOR_VERSION: info.vendor_version = as_string(); break;
        case ADBC_INFO_VENDOR_ARROW_VERSION: info.vendor_arrow_version = as_string(); break;
        case ADBC_INFO_VENDOR_SQL: info.vendor_sql = as_bool(); break;
        case ADBC_INFO_VENDOR_SUBSTRAIT: info.vendor_substrait = as_bool(); break;
        case ADBC_INFO_DRIVER_NAME: info.driver_name = as_string(); break;
        case ADBC_INFO_DRIVER_VERSION: info.driver_version = as_string(); break;
        case ADBC_INFO_DRIVER_ARROW_VERSION: info.driver_arrow_version = as_string(); break;
        case ADBC_INFO_DRIVER_ADBC_VERSION: info.driver_adbc_version = as_int64(); break;
        default: break;
      }
    }
  }
  return info;
}

std::vector<std::string> LoadTableTypesBlocking(AdbcSession& session, std::string& error_out) {
  error_out.clear();
  std::vector<std::string> types;
  if (session.IsClosed() || !session.connection_created) {
    error_out = Tr("Connection is closed", "连接已关闭");
    return types;
  }
  if (session.dialect == Dialect::Redis) {
    return {"database"};
  }

  detail::AdbcErrorGuard error;
  detail::AdbcStreamHolder stream;
  const AdbcStatusCode status =
      AdbcConnectionGetTableTypes(&session.connection, &stream.stream, error.Ptr());
  if (status != ADBC_STATUS_OK) {
    error_out = error.Take();
    return types;
  }

  detail::AdbcSchemaHolder schema;
  if (stream.stream.get_schema(&stream.stream, &schema.schema) != 0) {
    error_out = detail::AdbcCaptureStreamError(stream.stream).message;
    return types;
  }

  while (true) {
    detail::AdbcArrayHolder array;
    if (stream.stream.get_next(&stream.stream, &array.array) != 0) {
      error_out = detail::AdbcCaptureStreamError(stream.stream).message;
      break;
    }
    if (array.array.release == nullptr) {
      break;
    }
    ArrayViewHolder root;
    ArrowError arrow_error;
    ArrowErrorInit(&arrow_error);
    if (ArrowArrayViewInitFromSchema(&root.view, &schema.schema, &arrow_error) != NANOARROW_OK ||
        ArrowArrayViewSetArray(&root.view, &array.array, &arrow_error) != NANOARROW_OK) {
      break;
    }
    root.initialized = true;
    if (root.view.n_children > 0) {
      for (std::int64_t r = 0; r < array.array.length; ++r) {
        std::string t = StringAt(root.view.children[0], r);
        if (!t.empty()) {
          types.push_back(std::move(t));
        }
      }
    }
  }
  return types;
}

TableStatisticsResult LoadTableStatisticsBlocking(AdbcSession& session, const DbObject& object,
                                                  bool approximate) {
  TableStatisticsResult result;
  if (session.IsClosed()) {
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }
  if (session.dialect == Dialect::Redis) {
    result.supported = false;
    return result;
  }
  if (!session.connection_created) {
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }

  std::unordered_map<std::int16_t, std::string> custom_names;
  {
    detail::AdbcErrorGuard name_err;
    detail::AdbcStreamHolder name_stream;
    if (AdbcConnectionGetStatisticNames(&session.connection, &name_stream.stream,
                                        name_err.Ptr()) == ADBC_STATUS_OK) {
      detail::AdbcSchemaHolder name_schema;
      if (name_stream.stream.get_schema(&name_stream.stream, &name_schema.schema) == 0) {
        while (true) {
          detail::AdbcArrayHolder arr;
          if (name_stream.stream.get_next(&name_stream.stream, &arr.array) != 0 ||
              arr.array.release == nullptr) {
            break;
          }
          ArrayViewHolder root;
          ArrowError ae;
          ArrowErrorInit(&ae);
          if (ArrowArrayViewInitFromSchema(&root.view, &name_schema.schema, &ae) != NANOARROW_OK) {
            break;
          }
          root.initialized = true;
          ArrowErrorInit(&ae);
          if (ArrowArrayViewSetArray(&root.view, &arr.array, &ae) != NANOARROW_OK) {
            break;
          }
          if (root.view.n_children >= 2) {
            for (std::int64_t r = 0; r < arr.array.length; ++r) {
              const std::string sname = StringAt(root.view.children[0], r);
              const auto skey = IntAt(root.view.children[1], r);
              if (!sname.empty() && skey.has_value()) {
                custom_names[static_cast<std::int16_t>(*skey)] = sname;
              }
            }
          }
        }
      }
    }
  }

  detail::AdbcErrorGuard error;
  detail::AdbcStreamHolder stream;
  const char* catalog = object.catalog.empty() ? nullptr : object.catalog.c_str();
  const char* db_schema = object.db_schema.empty() ? nullptr : object.db_schema.c_str();
  const AdbcStatusCode status = AdbcConnectionGetStatistics(
      &session.connection, catalog, db_schema, object.name.c_str(), approximate ? 1 : 0,
      &stream.stream, error.Ptr());
  if (status == ADBC_STATUS_NOT_IMPLEMENTED) {
    result.supported = false;
    return result;
  }
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    return result;
  }

  result.supported = true;
  detail::AdbcSchemaHolder schema;
  if (stream.stream.get_schema(&stream.stream, &schema.schema) != 0) {
    AdbcStatusCode st = ADBC_STATUS_OK;
    const auto cap = detail::AdbcCaptureStreamError(stream.stream, &st);
    result.error = cap.message;
    result.connection_lost = IsConnectionLostError(st, cap.message, cap.sqlstate);
    return result;
  }

  while (true) {
    detail::AdbcArrayHolder array;
    if (stream.stream.get_next(&stream.stream, &array.array) != 0) {
      AdbcStatusCode st = ADBC_STATUS_OK;
      const auto cap = detail::AdbcCaptureStreamError(stream.stream, &st);
      result.error = cap.message;
      result.connection_lost = IsConnectionLostError(st, cap.message, cap.sqlstate);
      break;
    }
    if (array.array.release == nullptr) {
      break;
    }

    ArrayViewHolder root;
    ArrowError ae;
    ArrowErrorInit(&ae);
    if (ArrowArrayViewInitFromSchema(&root.view, &schema.schema, &ae) != NANOARROW_OK) {
      result.error = detail::AdbcDecodeSystemTextToUtf8(ae.message);
      break;
    }
    root.initialized = true;
    ArrowErrorInit(&ae);
    if (ArrowArrayViewSetArray(&root.view, &array.array, &ae) != NANOARROW_OK) {
      result.error = detail::AdbcDecodeSystemTextToUtf8(ae.message);
      break;
    }

    const ArrowArrayView* schemas_list = root.view.n_children > 1 ? root.view.children[1] : nullptr;
    for (std::int64_t ci = 0; ci < array.array.length; ++ci) {
      if (schemas_list == nullptr || ArrowArrayViewIsNull(schemas_list, ci) != 0) {
        continue;
      }
      const std::int64_t s_start = ListStart(schemas_list, ci);
      const std::int64_t s_end = ListEnd(schemas_list, ci);
      const ArrowArrayView* schema_struct = schemas_list->children[0];
      for (std::int64_t si = s_start; si < s_end; ++si) {
        const ArrowArrayView* stats_list =
            schema_struct->n_children > 1 ? schema_struct->children[1] : nullptr;
        if (stats_list == nullptr || ArrowArrayViewIsNull(stats_list, si) != 0) {
          continue;
        }
        const std::int64_t st_start = ListStart(stats_list, si);
        const std::int64_t st_end = ListEnd(stats_list, si);
        const ArrowArrayView* st_struct = stats_list->children[0];
        for (std::int64_t sti = st_start; sti < st_end; ++sti) {
          const std::string tname = st_struct->n_children > 0 ? StringAt(st_struct->children[0], sti)
                                                              : std::string{};
          if (!object.name.empty() && tname != object.name) {
            continue;
          }
          ColumnStatistic stat;
          stat.column_name = st_struct->n_children > 1 ? StringAt(st_struct->children[1], sti)
                                                       : std::string{};
          const auto key_opt =
              st_struct->n_children > 2 ? IntAt(st_struct->children[2], sti) : std::nullopt;
          stat.statistic_key = static_cast<std::int16_t>(key_opt.value_or(0));
          if (const auto it = custom_names.find(stat.statistic_key); it != custom_names.end()) {
            stat.statistic_name = it->second;
          } else {
            stat.statistic_name = StandardStatName(stat.statistic_key);
          }
          std::optional<std::int64_t> int_val;
          stat.value_text =
              st_struct->n_children > 3
                  ? FormatStatValueUnion(st_struct->children[3], sti, int_val)
                  : std::string{};
          stat.approximate = st_struct->n_children > 4 &&
                             BoolAt(st_struct->children[4], sti).value_or(false);

          if (stat.column_name.empty() && stat.statistic_key == ADBC_STATISTIC_ROW_COUNT_KEY &&
              int_val.has_value()) {
            result.row_count = *int_val;
            result.row_count_approximate = stat.approximate;
          }
          result.statistics.push_back(std::move(stat));
        }
      }
    }
  }
  return result;
}

bool SwitchSessionNamespaceBlocking(AdbcSession& session, std::string_view catalog,
                                    std::string_view db_schema, std::string& error_out) {
  error_out.clear();
  if (session.IsClosed() || !session.connection_created) {
    error_out = Tr("Connection is closed", "连接已关闭");
    return false;
  }

  bool any_native_ok = false;
  bool any_not_impl = false;
  const auto try_opt = [&](const char* key, std::string_view val) -> bool {
    if (val.empty()) {
      return true;
    }
    const std::string copy(val);
    detail::AdbcErrorGuard err;
    const AdbcStatusCode st =
        AdbcConnectionSetOption(&session.connection, key, copy.c_str(), err.Ptr());
    if (st == ADBC_STATUS_OK) {
      any_native_ok = true;
      return true;
    }
    if (st == ADBC_STATUS_NOT_IMPLEMENTED) {
      any_not_impl = true;
      return true;
    }
    error_out = err.Take();
    return false;
  };

  if (!try_opt(ADBC_CONNECTION_OPTION_CURRENT_CATALOG, catalog)) {
    return false;
  }
  if (!try_opt(ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA, db_schema)) {
    return false;
  }
  if (any_native_ok && !any_not_impl) {
    return true;
  }
  return !any_not_impl;
}

AdbcSchemaResult InspectQuerySchemaBlocking(AdbcSession& session, std::string sql) {
  AdbcSchemaResult result;
  if (session.IsClosed()) {
    result.connection_lost = true;
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }
  if (session.dialect == Dialect::Redis) {
    result.error = Tr("Schema inspection is not applicable to Redis", "Redis 不支持 SQL 结果 Schema 预检");
    return result;
  }
  if (!session.connection_created) {
    result.error = Tr("Connection is closed", "连接已关闭");
    return result;
  }

  detail::AdbcErrorGuard error;
  detail::AdbcStatementGuard stmt;
  AdbcStatusCode status = AdbcStatementNew(&session.connection, &stmt.statement, error.Ptr());
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.sqlstate = cap.sqlstate;
    result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
    return result;
  }
  stmt.created = true;

  status = AdbcStatementSetSqlQuery(&stmt.statement, sql.c_str(), error.Ptr());
  if (status != ADBC_STATUS_OK) {
    const auto cap = error.TakeDetailed();
    result.error = cap.message;
    result.sqlstate = cap.sqlstate;
    return result;
  }

  (void)AdbcStatementPrepare(&stmt.statement, error.Ptr());
  error.Reset();

  detail::AdbcSchemaHolder schema;
  status = AdbcStatementExecuteSchema(&stmt.statement, &schema.schema, error.Ptr());
  if (status == ADBC_STATUS_OK) {
    detail::AdbcPopulateColumnsFromArrowSchema(schema.schema, nullptr, result.columns);
    return result;
  }

  if (status == ADBC_STATUS_NOT_IMPLEMENTED) {
    error.Reset();
    if (IsSqlReadStatement(sql)) {
      std::string trimmed(detail::AdbcTrimAscii(sql));
      while (!trimmed.empty() && trimmed.back() == ';') {
        trimmed.pop_back();
        trimmed = std::string(detail::AdbcTrimAscii(trimmed));
      }
      const std::string lower = detail::AdbcLowerAscii(trimmed);
      const bool can_wrap = lower.starts_with("select") || lower.starts_with("with") ||
                            lower.starts_with("values") || lower.starts_with("table");
      detail::AdbcStatementGuard probe_stmt;
      if (AdbcStatementNew(&session.connection, &probe_stmt.statement, error.Ptr()) ==
          ADBC_STATUS_OK) {
        probe_stmt.created = true;
        const std::string probe_sql =
            can_wrap ? ("SELECT * FROM (" + trimmed + ") AS __dbpp_schema_probe LIMIT 0")
                     : trimmed;
        if (AdbcStatementSetSqlQuery(&probe_stmt.statement, probe_sql.c_str(), error.Ptr()) ==
            ADBC_STATUS_OK) {
          detail::AdbcStreamHolder probe_stream;
          if (AdbcStatementExecuteQuery(&probe_stmt.statement, &probe_stream.stream, nullptr,
                                        error.Ptr()) == ADBC_STATUS_OK) {
            detail::AdbcSchemaHolder probe_schema;
            if (probe_stream.stream.get_schema(&probe_stream.stream, &probe_schema.schema) == 0) {
              detail::AdbcPopulateColumnsFromArrowSchema(probe_schema.schema, nullptr,
                                                         result.columns);
              return result;
            }
          }
        }
      }
      const auto cap = error.TakeDetailed();
      result.error = cap.message;
      result.sqlstate = cap.sqlstate;
      return result;
    }
    result.error = Tr("Driver does not support dry-run schema inspection for non-read SQL",
                      "当前驱动不支持非只读 SQL 的 Schema 预检");
    return result;
  }

  const auto cap = error.TakeDetailed();
  result.error = cap.message;
  result.sqlstate = cap.sqlstate;
  result.error_details = cap.details;
  result.connection_lost = IsConnectionLostError(status, cap.message, cap.sqlstate);
  return result;
}

} // namespace dbpp
