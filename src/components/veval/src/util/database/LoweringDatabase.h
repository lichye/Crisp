#pragma once
#include "Json.h"
#include <sqlite3.h>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <unordered_map>

namespace veval::result_validation {
struct SqlValue {
    enum class Kind { Null, Integer, Text } kind;
    std::string_view text;
    std::int64_t integer = 0;
    SqlValue(std::nullptr_t) : kind(Kind::Null) {}
    SqlValue(std::int64_t value) : kind(Kind::Integer), integer(value) {}
    SqlValue(std::string_view value) : kind(Kind::Text), text(value) {}
    SqlValue(const std::string &value) : SqlValue(std::string_view(value)) {}
    SqlValue(const char *value) : SqlValue(std::string_view(value)) {}
};
// One database belongs to one lowering invocation; checks retain their own identities.
class Database {
    sqlite3 *db = nullptr;
    std::unordered_map<std::string, sqlite3_stmt *> statements;
    sqlite3_stmt *statement(const std::string &);
    sqlite3_stmt *prepare(const std::string &, const std::vector<J> &);
    Budget *budget = nullptr;

  public:
    fs::path path;
    explicit Database(fs::path path, bool create = false, bool readOnly = false);
    ~Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
    void exec(const std::string &sql, const std::vector<J> &args = {});
    // Borrow column values only for this call. Return the number of changed rows.
    int execValues(const std::string &sql, std::initializer_list<SqlValue> args);
    std::vector<J> rows(const std::string &sql, const std::vector<J> &args = {});
    void meta(const std::string &key, const std::string &value);
    std::string meta(const std::string &key);
    void begin();
    // Within a transaction, postpone non-unique indexes until bulk insertion
    // finishes. Execute the returned definitions before committing.
    std::vector<std::string> deferIndexes();
    void commit();
    void rollback();
    void watch(Budget *);
    void node(const J &);
    void route(const J &);
    void discharge(const J &);
    void history(const J &);
    void arrival(const J &);
    void relation(const J &);
    void binding(const J &edge);
    void attempt(const J &result, const fs::path &path, const std::string &arm);
    // Optional S1 driver bookkeeping; existing producers/readers retain schema 1.
    void checkSchema();
};
} // namespace veval::result_validation
