#include "LoweringDatabase.h"
#include <algorithm>
#include <iostream>
#include <set>

namespace veval::result_validation {
void Database::checkSchema() {
    exec("CREATE TABLE IF NOT EXISTS check_runs("
         "run_id INTEGER PRIMARY KEY, started_utc TEXT NOT NULL, finished_utc TEXT, "
         "command TEXT NOT NULL, inputs_sha256 TEXT NOT NULL, tool_commit TEXT, "
         "status TEXT NOT NULL, active_s REAL NOT NULL DEFAULT 0, reason TEXT)");
    exec("CREATE TABLE IF NOT EXISTS problems("
         "job_id TEXT PRIMARY KEY, source_entry TEXT NOT NULL, instance TEXT NOT NULL, "
         "module TEXT NOT NULL, sentence_sha256 TEXT NOT NULL, context TEXT NOT NULL, "
         "engine TEXT NOT NULL, budget_s REAL, memory_gb REAL, status TEXT NOT NULL, "
         "first_run_id INTEGER NOT NULL, result_path TEXT, attempt_id TEXT, "
         "command_ref TEXT, first_result_s REAL)");
    exec("CREATE INDEX IF NOT EXISTS check_problems_by_source ON problems(source_entry,instance)");
    std::set<std::string> columns;
    for (const auto &row : rows("PRAGMA table_info(check_runs)")) columns.insert(text(row["name"]));
    // Strategy, placement and engine in force, with their origins.
    if (!columns.count("effective_config")) exec("ALTER TABLE check_runs ADD COLUMN effective_config TEXT");
}
static const char *schema = R"SQL(
PRAGMA user_version=1;
CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);
CREATE TABLE instances(instance TEXT PRIMARY KEY,module TEXT NOT NULL,depth INTEGER NOT NULL,
 parent TEXT,scope TEXT,environment_profile TEXT);
CREATE INDEX instance_depth ON instances(depth,instance);
CREATE TABLE node_premises(instance TEXT NOT NULL,ordinal INTEGER NOT NULL,entry TEXT NOT NULL,
 arrival TEXT,grade TEXT,PRIMARY KEY(instance,ordinal));
CREATE TABLE fixed_bindings(instance TEXT NOT NULL,ordinal INTEGER NOT NULL,typed_ir TEXT NOT NULL,
 PRIMARY KEY(instance,ordinal));
CREATE TABLE routes(row_id INTEGER PRIMARY KEY,entry TEXT NOT NULL,instance TEXT NOT NULL,result TEXT,stop_reason TEXT);
CREATE INDEX routes_by_entry ON routes(entry,instance,row_id);
CREATE INDEX routes_by_instance ON routes(instance,entry);
CREATE TABLE route_hops(route_id INTEGER NOT NULL,ordinal INTEGER NOT NULL,
 edge TEXT,kind TEXT,relation_id TEXT,composite INTEGER,span INTEGER,detail TEXT,
 PRIMARY KEY(route_id,ordinal));
CREATE TABLE correspondences(route_id INTEGER PRIMARY KEY,instance TEXT NOT NULL,status TEXT,direction TEXT,
 source_entry TEXT,target_sentence TEXT,target_clock TEXT,target_edge TEXT);
CREATE TABLE discharge_records(entry TEXT NOT NULL,instance TEXT NOT NULL,classification TEXT,
 granted_over_driven INTEGER,PRIMARY KEY(entry,instance));
CREATE TABLE discharge_sites(entry TEXT NOT NULL,consumer TEXT NOT NULL,ordinal INTEGER NOT NULL,
 instance TEXT NOT NULL,status TEXT,property TEXT,PRIMARY KEY(entry,consumer,ordinal));
CREATE INDEX owners_by_entry ON discharge_sites(entry,instance);
CREATE TABLE premise_tiers(entry TEXT NOT NULL,instance TEXT NOT NULL,tier TEXT,PRIMARY KEY(entry,instance));
CREATE TABLE transport_landings(entry TEXT NOT NULL,instance TEXT NOT NULL,PRIMARY KEY(entry,instance));
CREATE TABLE arrival_signals(entry TEXT NOT NULL,instance TEXT NOT NULL,ordinal INTEGER NOT NULL,
 signal TEXT NOT NULL,PRIMARY KEY(entry,instance,ordinal));
CREATE TABLE symeval_records(id TEXT PRIMARY KEY,schema TEXT,status TEXT,instance TEXT,module TEXT,
 source_property TEXT,source_text TEXT,target_text TEXT,obligation_status TEXT,
 antecedent_property TEXT,antecedent_instance TEXT,antecedent_role TEXT,antecedent_status TEXT,antecedent_tier TEXT,
 derived_entry TEXT,source_role TEXT,target_role TEXT);
CREATE INDEX field_by_landing ON symeval_records(instance,derived_entry);
-- Existing typed certificates are opaque evidence, fetched only for a used
-- field. They are not the index and never require a scan of symeval.json.
CREATE TABLE field_certificates(id TEXT PRIMARY KEY,certificate TEXT NOT NULL);
CREATE TABLE source_edges(instance TEXT PRIMARY KEY,module TEXT);
CREATE TABLE source_bindings(instance TEXT NOT NULL,ordinal INTEGER NOT NULL,port TEXT,parent_expr TEXT,
 direction TEXT,kind TEXT,PRIMARY KEY(instance,ordinal));
CREATE INDEX source_port ON source_bindings(instance,port);
CREATE TABLE attempts(arm TEXT NOT NULL,attempt_id TEXT NOT NULL,job_id TEXT NOT NULL,entry TEXT,
 instance TEXT,label TEXT,outcome TEXT,verdict TEXT,method TEXT,result_path TEXT NOT NULL,
 PRIMARY KEY(arm,attempt_id));
CREATE INDEX attempts_by_landing ON attempts(arm,entry,instance,verdict);
CREATE TABLE environment_dependencies(entry TEXT,instance TEXT,status TEXT,missing INTEGER,path_kind TEXT,ports TEXT,reason TEXT,PRIMARY KEY(entry,instance));
CREATE TABLE source_properties(entry TEXT PRIMARY KEY,kind TEXT NOT NULL,status TEXT NOT NULL,reason TEXT);
)SQL";

Database::Database(fs::path p, bool create, bool readOnly) : path(fs::absolute(p)) {
    if (create && readOnly) throw std::runtime_error("cannot create a read-only database");
    if (create && fs::exists(path))
        throw std::runtime_error("database already exists: " + path.string());
    if (create && !path.parent_path().empty())
        fs::create_directories(path.parent_path());
    auto flags = readOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0);
    if (sqlite3_open_v2(path.c_str(), &db, flags, nullptr) != SQLITE_OK) {
        std::string error = db ? sqlite3_errmsg(db) : "open failed";
        if (db)
            sqlite3_close(db);
        db = nullptr;
        throw std::runtime_error("SQLite: " + error + ": " + path.string());
    }
    try {
        sqlite3_busy_timeout(db, 5000);
        exec("PRAGMA foreign_keys=ON");
        if (!readOnly) {
            exec("PRAGMA journal_mode=WAL");
            exec("PRAGMA synchronous=NORMAL");
        }
        if (create) {
            char *message = nullptr;
            const std::string initialization =
                std::string("BEGIN IMMEDIATE;\n") + schema +
                "INSERT INTO metadata VALUES('schema','consflow-db/1');"
                "INSERT INTO metadata VALUES('complete','0');COMMIT;";
            if (sqlite3_exec(db, initialization.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
                std::string error = message ? message : sqlite3_errmsg(db);
                sqlite3_free(message);
                throw std::runtime_error(error);
            }
        } else if (meta("schema") != "consflow-db/1")
            throw std::runtime_error("unsupported ConsFlow database schema");
        if (!readOnly) { exec(
            "CREATE INDEX IF NOT EXISTS clock_by_port ON source_bindings(port) WHERE kind='whole'");
        exec("CREATE INDEX IF NOT EXISTS clock_by_parent ON source_bindings(parent_expr) WHERE "
             "kind='whole'"); }
    } catch (...) {
        for (auto &p : statements)
            sqlite3_finalize(p.second);
        sqlite3_close(db);
        db = nullptr;
        throw;
    }
}
Database::~Database() {
    for (auto &p : statements)
        sqlite3_finalize(p.second);
    if (db)
        sqlite3_close(db);
}
sqlite3_stmt *Database::statement(const std::string &sql) {
    if (budget)
        budget->check();
    auto &s = statements[sql];
    if (!s && sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) != SQLITE_OK)
        throw std::runtime_error("SQLite prepare: " + std::string(sqlite3_errmsg(db)));
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    return s;
}
sqlite3_stmt *Database::prepare(const std::string &sql, const std::vector<J> &args) {
    auto s = statement(sql);
    if (sqlite3_bind_parameter_count(s) != int(args.size()))
        throw std::runtime_error("SQLite argument count mismatch");
    for (size_t i = 0; i < args.size(); ++i) {
        auto &a = args[i];
        int rc;
        if (a.is_null())
            rc = sqlite3_bind_null(s, int(i + 1));
        else if (a.is_true() || a.is_false())
            rc = sqlite3_bind_int(s, int(i + 1), a.is_true());
        else if (a.is_number())
            rc = sqlite3_bind_double(s, int(i + 1), std::stod(a.value));
        else if (a.is_string())
            rc = sqlite3_bind_text(s, int(i + 1), a.value.data(), int(a.value.size()),
                                   SQLITE_TRANSIENT);
        else
            throw std::runtime_error("SQLite values must be scalar");
        if (rc != SQLITE_OK)
            throw std::runtime_error("SQLite bind failed");
    }
    return s;
}
int Database::execValues(const std::string &sql, std::initializer_list<SqlValue> args) {
    auto s = statement(sql);
    struct Reset {
        sqlite3_stmt *s;
        ~Reset() { sqlite3_reset(s); sqlite3_clear_bindings(s); }
    } reset{s};
    if (sqlite3_bind_parameter_count(s) != int(args.size()))
        throw std::runtime_error("SQLite argument count mismatch");
    int i = 0;
    for (const auto &value : args) {
        int rc = SQLITE_OK;
        ++i;
        switch (value.kind) {
        case SqlValue::Kind::Null: rc = sqlite3_bind_null(s, i); break;
        case SqlValue::Kind::Integer: rc = sqlite3_bind_int64(s, i, value.integer); break;
        case SqlValue::Kind::Text:
            rc = sqlite3_bind_text64(s, i, value.text.empty() ? "" : value.text.data(), value.text.size(), SQLITE_STATIC, SQLITE_UTF8);
            break;
        }
        if (rc != SQLITE_OK) throw std::runtime_error("SQLite bind failed");
    }
    const int rc = sqlite3_step(s);
    if (rc == SQLITE_INTERRUPT) throw Expired("lowering database deadline exhausted");
    if (rc != SQLITE_DONE) throw std::runtime_error("SQLite: " + std::string(sqlite3_errmsg(db)));
    return sqlite3_changes(db);
}
void Database::exec(const std::string &sql, const std::vector<J> &args) {
    auto s = prepare(sql, args);
    int rc = sqlite3_step(s);
    if (rc == SQLITE_INTERRUPT)
        throw Expired("lowering database deadline exhausted");
    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
        throw std::runtime_error("SQLite: " + std::string(sqlite3_errmsg(db)));
    sqlite3_reset(s);
}
std::vector<J> Database::rows(const std::string &sql, const std::vector<J> &args) {
    auto s = prepare(sql, args);
    std::vector<J> result;
    int rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        J row = object();
        for (int i = 0; i < sqlite3_column_count(s); ++i) {
            J v;
            switch (sqlite3_column_type(s, i)) {
            case SQLITE_INTEGER:
                v = json_numbert(std::to_string(sqlite3_column_int64(s, i)));
                break;
            case SQLITE_FLOAT:
                v = num(sqlite3_column_double(s, i));
                break;
            case SQLITE_TEXT:
                v = str(std::string(reinterpret_cast<const char *>(sqlite3_column_text(s, i)),
                                    sqlite3_column_bytes(s, i)));
                break;
            case SQLITE_NULL:
                break;
            default:
                throw std::runtime_error("unexpected SQLite column type");
            }
            put(row, sqlite3_column_name(s, i), v);
        }
        result.push_back(std::move(row));
    }
    if (rc == SQLITE_INTERRUPT)
        throw Expired("lowering database deadline exhausted");
    if (rc != SQLITE_DONE)
        throw std::runtime_error("SQLite query: " + std::string(sqlite3_errmsg(db)));
    sqlite3_reset(s);
    return result;
}
void Database::meta(const std::string &k, const std::string &v) {
    exec("INSERT OR REPLACE INTO metadata VALUES(?,?)", {str(k), str(v)});
}
std::string Database::meta(const std::string &k) {
    auto r = rows("SELECT value FROM metadata WHERE key=?", {str(k)});
    return r.empty() ? "" : text(r.front()["value"]);
}
void Database::begin() {
    exec("BEGIN IMMEDIATE");
}
std::vector<std::string> Database::deferIndexes() {
    if (sqlite3_get_autocommit(db))
        throw std::runtime_error("deferring indexes requires a transaction");
    const auto indexes = rows(
        "SELECT m.name,m.sql FROM sqlite_master m JOIN pragma_index_list(m.tbl_name) i ON i.name=m.name "
        "WHERE m.type='index' AND m.sql IS NOT NULL AND i.\"unique\"=0 ORDER BY m.rowid");
    std::vector<std::string> definitions;
    for (const auto &index : indexes) {
        std::string name = "\"";
        for (const char c : text(index["name"])) { if (c == '"') name += '"'; name += c; }
        name += '"';
        definitions.push_back(text(index["sql"]));
        exec("DROP INDEX " + name);
    }
    return definitions;
}
void Database::commit() {
    exec("COMMIT");
}
void Database::rollback() {
    auto previous = budget;
    watch(nullptr);
    if (!sqlite3_get_autocommit(db))
        exec("ROLLBACK");
    watch(previous);
}
void Database::watch(Budget *b) {
    budget = b;
    sqlite3_progress_handler(db,b?1000:0,b?+[](void *p)->int {
        auto budget=static_cast<Budget*>(p);return cancelled()||budget->left()==0;
    }:nullptr,b);
}

void Database::node(const J &n) {
    exec("INSERT INTO instances VALUES(?,?,?,?,?,?)",
         {n["instance"], n["module"], n["depth"], n["parent"], n["scope"],
          n["environment_profile"]});
    size_t i = 0;
    for (auto &p : items(n["premises"]))
        exec("INSERT INTO node_premises VALUES(?,?,?,?,?)",
             {n["instance"], num(i++), p["assume"], p["arrival"], p["grade"]});
    i = 0;
    for (auto &p : items(n["fixed_bindings"]))
        exec("INSERT INTO fixed_bindings VALUES(?,?,?)",
             {n["instance"], num(i++), str(canonical(p))});
}
void Database::route(const J &r) {
    exec("INSERT INTO routes(entry,instance,result,stop_reason) VALUES(?,?,?,?)",
         {r["entry"], r["instance"], r["result"], r["stop_reason"]});
    J id = json_numbert(std::to_string(sqlite3_last_insert_rowid(db)));
    size_t i = 0;
    for (auto &h : items(r["route"]))
        exec("INSERT INTO route_hops VALUES(?,?,?,?,?,?,?,?)",
             {id, num(i++), h["edge"], h["kind"], h["relation"], h["composite"], h["span"],
              h["detail"]});
    const auto &c = r["correspondence"];
    if (truth(c))
        exec("INSERT INTO correspondences VALUES(?,?,?,?,?,?,?,?)",
             {id, r["instance"], c["status"], c["direction"], c["source_entry"],
              c["target_sentence"], c["target_clock"], c["target_edge"]});
}
void Database::discharge(const J &r) {
    exec("INSERT OR REPLACE INTO discharge_records VALUES(?,?,?,?)",
         {r["property"], r["instance"], r["classification"], r["granted_over_driven"]});
    exec("DELETE FROM discharge_sites WHERE entry=? AND consumer=?",
         {r["property"], r["instance"]});
    size_t i = 0;
    for (auto &o : items(r["discharge"]))
        exec("INSERT INTO discharge_sites VALUES(?,?,?,?,?,?)",
             {r["property"], r["instance"], num(i++), o["instance"], o["status"], o["property"]});
}
void Database::history(const J &r) {
    exec("INSERT OR REPLACE INTO premise_tiers VALUES(?,?,?)",
         {r["property"], r["instance"], r["tier"]});
}
void Database::arrival(const J &r) {
    auto found = rows("SELECT 1 AS found FROM transport_landings WHERE entry=? AND instance=?",
                      {r["property"], r["instance"]});
    if (!found.empty())
        return; // Preserve the old first-record arrival rule.
    exec("INSERT INTO transport_landings VALUES(?,?)", {r["property"], r["instance"]});
    size_t i = 0;
    for (auto &s : items(r["arrived"]))
        exec("INSERT INTO arrival_signals VALUES(?,?,?,?)",
             {r["property"], r["instance"], num(i++), s});
}
void Database::relation(const J &r) {
    auto version = text(r["schema"], "consflow-symeval/2");
    if (version != "consflow-symeval/2" && version != "consflow-symeval/3")
        throw std::runtime_error("unsupported symeval schema: " + version);
    auto &a = r["antecedent"], &f = r["field"];
    exec("INSERT OR REPLACE INTO symeval_records VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
         {r["id"], r["schema"], r["status"], r["instance"], r["module"], r["source_property"],
          r["source_text"], r["target_text"], r["obligation"]["status"], a["property"],
          a["instance"], a["role"], a["status"], a["tier"], f["derived_entry"], f["source_role"],
          f["target_role"]});
    if (truth(f))
        exec("INSERT OR REPLACE INTO field_certificates VALUES(?,?)", {r["id"], str(canonical(f))});
}
void Database::binding(const J &e) {
    exec("INSERT OR REPLACE INTO source_edges VALUES(?,?)", {e["instance"], e["module"]});
    exec("DELETE FROM source_bindings WHERE instance=?", {e["instance"]});
    size_t i = 0;
    for (auto &b : items(e["bindings"]))
        exec("INSERT INTO source_bindings VALUES(?,?,?,?,?,?)",
             {e["instance"], num(i++), b["port"], b["parent_expr"], b["direction"], b["kind"]});
}
void Database::attempt(const J &r, const fs::path &p, const std::string &arm) {
    auto &t = r["target"];
    exec("INSERT OR REPLACE INTO attempts VALUES(?,?,?,?,?,?,?,?,?,?)",
         {str(arm), r["identity"]["attempt_id"], r["identity"]["job_id"], t["source_entry"],
          t["instance"], t["label"], r["outcome"], r["verdict"], r["evidence"]["method_token"],
          str(fs::absolute(p).string())});
}
} // namespace veval::result_validation
