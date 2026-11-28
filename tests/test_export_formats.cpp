import std;
import dbpp.types;
import dbpp.adbc;

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    ++g_failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

struct TempDirGuard {
  fs::path path;
  TempDirGuard() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() / ("dbpp_test_export_" + std::to_string(stamp));
    fs::create_directories(path);
  }
  ~TempDirGuard() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

std::string ReadBinary(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

} // namespace

int main() {
  const TempDirGuard tmp;
  const fs::path db_path = tmp.path / "export_test.sqlite";

  const dbpp::AdbcOpenResult opened = dbpp::OpenAdbcBlocking(dbpp::ConnectionRequest{
      .driver_path = dbpp::DefaultSqliteDriverPath(),
      .uri = db_path.string(),
      .username = {},
      .password = {},
      .title = "export-test",
  });
  Expect(opened.error.empty() && opened.session != nullptr,
         "Open SQLite session failed: " + opened.error);
  if (!opened.session) {
    return 1;
  }
  auto& session = *opened.session;

  Expect(dbpp::ExecuteBlocking(
             session,
             "CREATE TABLE items ("
             "id INTEGER PRIMARY KEY, "
             "big_id INTEGER, "
             "name TEXT, "
             "score REAL, "
             "note TEXT"
             ");",
             false, 0)
             .error.empty(),
         "CREATE TABLE items failed");

  Expect(dbpp::ExecuteBlocking(
             session,
             "INSERT INTO items (id, big_id, name, score, note) VALUES "
             "(1, 9223372036854775800, 'Alpha, \"One\"', -12.5, 'a|b\nc\td'), "
             "(2, 42, '<script>&\"''tag', 3.25, NULL);",
             false, 0)
             .error.empty(),
         "INSERT rows failed");

  const std::string query =
      "SELECT id, big_id, name, score, note FROM items ORDER BY id";

  // 1. Format metadata & parser helpers
  Expect(dbpp::GetExportFormatInfo(dbpp::ExportFormat::Xlsx).extension == "xlsx",
         "XLSX extension info mismatch");
  Expect(dbpp::ParseExportFormatName("md") == dbpp::ExportFormat::Markdown,
         "ParseExportFormatName('md') failed");
  Expect(dbpp::ParseExportFormatName("SQL-INSERT") == dbpp::ExportFormat::SqlInsert,
         "ParseExportFormatName('SQL-INSERT') failed");
  Expect(dbpp::InferExportFormatFromExtension(".XLSX") == dbpp::ExportFormat::Xlsx,
         "InferExportFormatFromExtension('.XLSX') failed");

  // 2. CSV
  {
    const fs::path p = tmp.path / "out.csv";
    const auto res = dbpp::ExportQueryToFileBlocking(session, query, dbpp::ExportFormat::Csv, p);
    Expect(res.error.empty() && res.rows_written == 2, "CSV export failed: " + res.error);
    const std::string text = ReadBinary(p);
    Expect(text.find("id,big_id,name,score,note\n") == 0, "CSV header mismatch");
    Expect(text.find("\"Alpha, \"\"One\"\"\"") != std::string::npos, "CSV quoting mismatch");
    Expect(text.find("2,42,\"<script>&\"\"'tag\",3.25,\n") != std::string::npos,
           "CSV NULL/quote row mismatch");
  }

  // 3. TSV
  {
    const fs::path p = tmp.path / "out.tsv";
    const auto res = dbpp::ExportQueryToFileBlocking(session, query, dbpp::ExportFormat::Tsv, p);
    Expect(res.error.empty() && res.rows_written == 2, "TSV export failed: " + res.error);
    const std::string text = ReadBinary(p);
    Expect(text.find("id\tbig_id\tname\tscore\tnote\n") == 0, "TSV header mismatch");
    Expect(text.find("\"a|b\nc\td\"") != std::string::npos, "TSV tab/newline quoting mismatch");
    Expect(text.find("2\t42\t\"<script>&\"\"'tag\"\t3.25\t\n") != std::string::npos,
           "TSV second row mismatch");
  }

  // 4. JSON (non-empty and empty)
  {
    const fs::path p = tmp.path / "out.json";
    const auto res = dbpp::ExportQueryToFileBlocking(session, query, dbpp::ExportFormat::Json, p);
    Expect(res.error.empty() && res.rows_written == 2, "JSON export failed: " + res.error);
    const std::string text = ReadBinary(p);
    Expect(text.starts_with("[\n  {") && text.ends_with("\n]\n"), "JSON array wrapper mismatch");
    Expect(text.find("\"score\": -12.5") != std::string::npos &&
               text.find("\"note\": null") != std::string::npos,
           "JSON row content mismatch");

    std::ostringstream empty_oss;
    const auto empty_res = dbpp::ExportQueryStreamBlocking(
        session, "SELECT id FROM items WHERE 1 = 0", dbpp::ExportFormat::Json, empty_oss);
    Expect(empty_res.error.empty() && empty_res.rows_written == 0 && empty_oss.str() == "[]\n",
           "Empty JSON array mismatch");
  }

  // 5. JSONL
  {
    const fs::path p = tmp.path / "out.jsonl";
    const auto res = dbpp::ExportQueryToFileBlocking(session, query, dbpp::ExportFormat::Jsonl, p);
    Expect(res.error.empty() && res.rows_written == 2, "JSONL export failed: " + res.error);
    const std::string text = ReadBinary(p);
    Expect(text.find("\"id\": 1") != std::string::npos &&
               text.find("\\nc\\td") != std::string::npos,
           "JSONL content mismatch");
  }

  // 6. SQL INSERT + round-trip execution
  {
    const fs::path p = tmp.path / "out.sql";
    const auto res = dbpp::ExportQueryToFileBlocking(
        session, query, dbpp::ExportFormat::SqlInsert, p, 0, "items_copy");
    Expect(res.error.empty() && res.rows_written == 2, "SQL INSERT export failed: " + res.error);
    const std::string sql_dump = ReadBinary(p);
    Expect(sql_dump.find("INSERT INTO \"items_copy\" (\"id\", \"big_id\", \"name\", \"score\", \"note\") VALUES (") !=
               std::string::npos,
           "SQL INSERT target/column quoting mismatch");

    Expect(dbpp::ExecuteBlocking(
               session,
               "CREATE TABLE items_copy (id INTEGER PRIMARY KEY, big_id INTEGER, name TEXT, score REAL, note TEXT);",
               false, 0)
               .error.empty(),
           "CREATE TABLE items_copy failed");
    const auto imp = dbpp::ImportSqlScriptFileBlocking(session, p);
    Expect(imp.error.empty() && imp.statements_executed == 2,
           "SQL INSERT round-trip import failed: " + imp.error);

    const auto q_orig = dbpp::ExecuteBlocking(session, query, true, 10);
    const auto q_copy = dbpp::ExecuteBlocking(
        session, "SELECT id, big_id, name, score, note FROM items_copy ORDER BY id", true, 10);
    Expect(q_orig.rows.size() == 2 && q_copy.rows.size() == 2, "Round-trip row count mismatch");
    if (q_orig.rows.size() == 2 && q_copy.rows.size() == 2) {
      for (std::size_t r = 0; r < 2; ++r) {
        for (std::size_t c = 0; c < 5; ++c) {
          Expect(q_orig.rows[r].values[c].is_null == q_copy.rows[r].values[c].is_null &&
                     q_orig.rows[r].values[c].text == q_copy.rows[r].values[c].text,
                 "SQL INSERT round-trip cell mismatch");
        }
      }
    }

    // Default table name fallback when sql_table_name is omitted
    std::ostringstream default_sql;
    const auto def_res = dbpp::ExportQueryStreamBlocking(
        session, "SELECT id FROM items ORDER BY id", dbpp::ExportFormat::SqlInsert, default_sql, 1);
    Expect(def_res.error.empty() && def_res.rows_written == 1 && def_res.truncated &&
               default_sql.str().find("INSERT INTO \"query_result\" (\"id\") VALUES (1);") !=
                   std::string::npos,
           "SQL INSERT default table name or max_rows truncation mismatch");
  }

  // 7. Markdown
  {
    const fs::path p = tmp.path / "out.md";
    const auto res = dbpp::ExportQueryToFileBlocking(session, query, dbpp::ExportFormat::Markdown, p);
    Expect(res.error.empty() && res.rows_written == 2, "Markdown export failed: " + res.error);
    const std::string text = ReadBinary(p);
    Expect(text.find("| id | big_id | name | score | note |\n| --- | --- | --- | --- | --- |\n") == 0,
           "Markdown header mismatch");
    Expect(text.find("a\\|b<br>c\td") != std::string::npos, "Markdown pipe/newline escape mismatch");
  }

  // 8. HTML
  {
    const fs::path p = tmp.path / "out.html";
    const auto res = dbpp::ExportQueryToFileBlocking(session, query, dbpp::ExportFormat::Html, p);
    Expect(res.error.empty() && res.rows_written == 2, "HTML export failed: " + res.error);
    const std::string text = ReadBinary(p);
    Expect(text.find("<!DOCTYPE html>") == 0 &&
               text.find("&lt;script&gt;&amp;&quot;&#39;tag") != std::string::npos &&
               text.find("<td class=\"null\">NULL</td>") != std::string::npos &&
               text.find("</html>\n") != std::string::npos,
           "HTML content mismatch");
  }

  // 9. XML (including non-tag column alias like COUNT(*))
  {
    const fs::path p = tmp.path / "out.xml";
    const auto res = dbpp::ExportQueryToFileBlocking(
        session,
        "SELECT id, name, note, COUNT(*) AS \"COUNT(*)\" FROM items GROUP BY id, name, note ORDER BY id",
        dbpp::ExportFormat::Xml, p);
    Expect(res.error.empty() && res.rows_written == 2, "XML export failed: " + res.error);
    const std::string text = ReadBinary(p);
    Expect(text.find("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<rows>\n") == 0 &&
               text.find("<column name=\"COUNT(*)\">1</column>") != std::string::npos &&
               text.find("<note null=\"true\"/>") != std::string::npos &&
               text.find("&lt;script&gt;&amp;") != std::string::npos &&
               text.ends_with("</rows>\n"),
           "XML content mismatch");
  }

  // 10. XLSX (OOXML ZIP container, safe 64-bit integer handling, empty table)
  {
    const fs::path p = tmp.path / "out.xlsx";
    const auto res = dbpp::ExportQueryToFileBlocking(
        session, query, dbpp::ExportFormat::Xlsx, p, 0, "items");
    Expect(res.error.empty() && res.rows_written == 2, "XLSX export failed: " + res.error);
    const std::string bin = ReadBinary(p);
    Expect(bin.size() > 200 && bin.substr(0, 4) == "PK\x03\x04", "XLSX ZIP local header missing");
    Expect(bin.find("PK\x05\x06") != std::string::npos, "XLSX ZIP EOCD missing");
    for (const std::string_view part : {
             "[Content_Types].xml",
             "_rels/.rels",
             "xl/workbook.xml",
             "xl/_rels/workbook.xml.rels",
             "xl/styles.xml",
             "xl/worksheets/sheet1.xml",
         }) {
      Expect(bin.find(part) != std::string::npos,
             std::string("XLSX missing part: ") + std::string(part));
    }
    Expect(bin.find("<sheet name=\"items\"") != std::string::npos, "XLSX sheet name mismatch");
    // 9223372036854775800 exceeds 2^53-1 -> must be inlineStr, not lossy numeric t="n"
    Expect(bin.find("<c r=\"B2\" t=\"inlineStr\"><is><t xml:space=\"preserve\">9223372036854775800</t></is></c>") !=
               std::string::npos,
           "XLSX large integer precision preservation mismatch");
    Expect(bin.find("<c r=\"B3\" t=\"n\"><v>42</v></c>") != std::string::npos &&
               bin.find("<c r=\"D2\" t=\"n\"><v>-12.5</v></c>") != std::string::npos,
           "XLSX numeric cell mismatch");
    Expect(bin.find("&lt;script&gt;&amp;") != std::string::npos, "XLSX XML escaping mismatch");

    // Empty result set still creates a valid header-only XLSX
    const fs::path empty_xlsx = tmp.path / "empty.xlsx";
    const auto empty_res = dbpp::ExportQueryToFileBlocking(
        session, "SELECT id, name FROM items WHERE 1 = 0", dbpp::ExportFormat::Xlsx, empty_xlsx);
    Expect(empty_res.error.empty() && empty_res.rows_written == 0 &&
               ReadBinary(empty_xlsx).substr(0, 4) == "PK\x03\x04",
           "Empty XLSX export failed");
  }

  // 11. Full Export -> Import round-trip across all 9 formats
  {
    const auto orig = dbpp::ExecuteBlocking(session, query, true, 10);
    Expect(orig.error.empty() && orig.rows.size() == 2, "Baseline query failed");

    const struct RoundTripCase {
      dbpp::ExportFormat format;
      const char* table_name;
      const char* ext;
    } cases[] = {
        {dbpp::ExportFormat::Csv, "rt_csv", ".csv"},
        {dbpp::ExportFormat::Tsv, "rt_tsv", ".tsv"},
        {dbpp::ExportFormat::Json, "rt_json", ".json"},
        {dbpp::ExportFormat::Jsonl, "rt_jsonl", ".jsonl"},
        {dbpp::ExportFormat::SqlInsert, "rt_sql", ".sql"},
        {dbpp::ExportFormat::Markdown, "rt_md", ".md"},
        {dbpp::ExportFormat::Html, "rt_html", ".html"},
        {dbpp::ExportFormat::Xml, "rt_xml", ".xml"},
        {dbpp::ExportFormat::Xlsx, "rt_xlsx", ".xlsx"},
    };

    for (const auto& tc : cases) {
      const fs::path file = tmp.path / (std::string(tc.table_name) + tc.ext);
      const auto exp = dbpp::ExportQueryToFileBlocking(session, query, tc.format, file, 0,
                                                       tc.table_name);
      Expect(exp.error.empty() && exp.rows_written == 2,
             std::string("Round-trip export failed for ") + tc.table_name + ": " + exp.error);

      if (tc.format == dbpp::ExportFormat::SqlInsert) {
        Expect(dbpp::ExecuteBlocking(
                   session,
                   std::string("CREATE TABLE ") + tc.table_name +
                       " (id INTEGER PRIMARY KEY, big_id INTEGER, name TEXT, score REAL, note TEXT);",
                   false, 0)
                   .error.empty(),
               "CREATE TABLE rt_sql failed");
      }

      const auto ing = dbpp::IngestFileBlocking(session, file, tc.format, tc.table_name,
                                                dbpp::IngestMode::Create);
      Expect(ing.error.empty(),
             std::string("Round-trip import failed for ") + tc.table_name + ": " + ing.error);

      const auto back = dbpp::ExecuteBlocking(
          session,
          std::string("SELECT id, big_id, name, score, note FROM ") + tc.table_name + " ORDER BY id",
          true, 10);
      Expect(back.error.empty() && back.rows.size() == 2,
             std::string("Round-trip query failed for ") + tc.table_name + ": " + back.error);
      if (orig.rows.size() == 2 && back.rows.size() == 2) {
        for (std::size_t r = 0; r < 2; ++r) {
          for (std::size_t c = 0; c < 5; ++c) {
            Expect(orig.rows[r].values[c].is_null == back.rows[r].values[c].is_null &&
                       orig.rows[r].values[c].text == back.rows[r].values[c].text,
                   std::string("Round-trip mismatch at ") + tc.table_name + " row=" +
                       std::to_string(r) + " col=" + std::to_string(c) + " got='" +
                       back.rows[r].values[c].text + "' expected='" + orig.rows[r].values[c].text +
                       "'");
          }
        }
      }
    }

    // JSON \uXXXX + UTF-16 surrogate pair decoding
    const fs::path unicode_json = tmp.path / "unicode.json";
    {
      std::ofstream out(unicode_json, std::ios::binary);
      out << "[{\"id\": 1, \"txt\": \"\\u4e2d\\u6587\\uD83D\\uDE00\"}]";
    }
    const auto u_ing = dbpp::IngestFileBlocking(session, unicode_json, dbpp::ExportFormat::Json,
                                                "rt_unicode", dbpp::IngestMode::Create);
    Expect(u_ing.error.empty(), "Unicode JSON import failed: " + u_ing.error);
    const auto u_q = dbpp::ExecuteBlocking(session, "SELECT txt FROM rt_unicode", true, 5);
    Expect(u_q.rows.size() == 1 && u_q.rows[0].values[0].text == "中文😀",
           "JSON \\uXXXX surrogate decode mismatch");
  }

  if (g_failures > 0) {
    std::cerr << g_failures << " export test(s) failed.\n";
    return 1;
  }
  return 0;
}
