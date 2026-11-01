// Coverage for sql_format.h/.cpp: mep's own SQL formatter. Each case is an
// input and the exact layout expected; every case is also checked to be
// idempotent (formatting the output again changes nothing).
//
// `mep-sql-format-test --format [--width N] [--tabs|--spaces N] [--keep-case] FILE...`
// prints each file formatted (or the refusal on stderr, exit 1), for running
// the formatter over a corpus.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "sql_format.h"

namespace {

int checks = 0;

void Check(bool ok, const char *expr, int line) {
    checks++;
    if (ok) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, __FILE__, line);
    std::abort();
}
#define CHECK(c) Check((c), #c, __LINE__)

void Same(const char *name, const std::string &in, const std::string &want, const sqlfmt::Options &o = {}) {
    const sqlfmt::Result r = sqlfmt::Format(in, o);
    if (!r.ok) {
        std::fprintf(stderr, "[%s] refused: %s (line %d)\n", name, r.error.c_str(), r.error_line);
        CHECK(r.ok);
    }
    if (r.text != want) {
        std::fprintf(stderr, "[%s] got:\n%s\n--- want:\n%s\n---\n", name, r.text.c_str(), want.c_str());
        CHECK(r.text == want);
    }
    const sqlfmt::Result again = sqlfmt::Format(r.text, o);
    CHECK(again.ok);
    if (again.text != r.text) {
        std::fprintf(stderr, "[%s] not idempotent:\n%s\n", name, again.text.c_str());
        CHECK(again.text == r.text);
    }
}

int FormatFiles(int argc, char **argv) {
    sqlfmt::Options o;
    int status = 0;
    for (int i = 2; i < argc; i++) {
        if (std::strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            o.width = std::atoi(argv[++i]);
            continue;
        }
        if (std::strcmp(argv[i], "--tabs") == 0) {
            o.use_tabs = true;
            continue;
        }
        if (std::strcmp(argv[i], "--spaces") == 0 && i + 1 < argc) {
            o.indent_width = std::atoi(argv[++i]);
            continue;
        }
        if (std::strcmp(argv[i], "--keep-case") == 0) {
            o.upper_keywords = false;
            continue;
        }
        std::ifstream in(argv[i], std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        const sqlfmt::Result r = sqlfmt::Format(ss.str(), o);
        if (!r.ok) {
            std::fprintf(stderr, "%s:%d: %s\n", argv[i], r.error_line + 1, r.error.c_str());
            status = 1;
            continue;
        }
        std::fwrite(r.text.data(), 1, r.text.size(), stdout);
    }
    return status;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--format") == 0) return FormatFiles(argc, argv);

    Same("short select", "select a,b from t where x=1;\n", "SELECT a, b\nFROM t\nWHERE x = 1;\n");
    Same("trailing comment on the last column",
         "create table t (\n    a int,\n    b text  -- a note\n);\n",
         "CREATE TABLE t (\n    a int,\n    b text -- a note\n);\n");
    Same("blank line after a heading comment",
         "-- Schema\n\ncreate table t (a);\n\n-- Data\n\ninsert into t values (1);\n",
         "-- Schema\n\nCREATE TABLE t (\n    a\n);\n\n-- Data\n\nINSERT INTO t\nVALUES (1);\n");
    Same("comment directly above a statement",
         "-- the rows\nselect a from t;\n", "-- the rows\nSELECT a\nFROM t;\n");
    Same("trailing comment on the last select item",
         "select customer_id, first_name, last_name, email_address, phone_number, created_at -- when\nfrom customers;\n",
         "SELECT\n    customer_id,\n    first_name,\n    last_name,\n    email_address,\n    phone_number,\n"
         "    created_at -- when\nFROM customers;\n");
    Same("keywords keep case when asked", "select a from t;\n", "select a\nfrom t;\n",
         [] {
             sqlfmt::Options o;
             o.upper_keywords = false;
             return o;
         }());
    Same("long select list",
         "select customer_id, first_name, last_name, email_address, phone_number, created_at from customers;\n",
         "SELECT\n    customer_id,\n    first_name,\n    last_name,\n    email_address,\n    phone_number,\n"
         "    created_at\nFROM customers;\n");
    Same("joins", "select o.id, c.name from orders o join customers c on c.id = o.customer_id left join x on x.a=o.a;",
         "SELECT o.id, c.name\nFROM orders o\nJOIN customers c ON c.id = o.customer_id\nLEFT JOIN x ON x.a = o.a;");
    Same("long where",
         "select * from t where alpha_value = 1 and beta_value = 2 and gamma_value between 3 and 4 or delta_value is null;",
         "SELECT *\nFROM t\nWHERE alpha_value = 1\n    AND beta_value = 2\n    AND gamma_value BETWEEN 3 AND 4\n"
         "    OR delta_value IS NULL;");
    Same("group order limit", "select a, count(*) from t group by a having count(*) > 1 order by 2 desc limit 10 offset 5;",
         "SELECT a, count(*)\nFROM t\nGROUP BY a\nHAVING count(*) > 1\nORDER BY 2 DESC\nLIMIT 10 OFFSET 5;");
    Same("subquery",
         "select name from users where id in (select user_id from orders where total > 100 and status = 'paid' and created_at > date('now', '-30 days'));",
         "SELECT name\nFROM users\nWHERE id IN (\n    SELECT user_id\n    FROM orders\n    WHERE total > 100\n"
         "        AND status = 'paid'\n        AND created_at > date('now', '-30 days')\n);");
    Same("cte",
         "with recent as (select * from orders where created_at > '2024-01-01'), big as (select * from recent where total > 10) select count(*) from big;",
         "WITH recent AS (\n    SELECT *\n    FROM orders\n    WHERE created_at > '2024-01-01'\n),\nbig AS (\n"
         "    SELECT *\n    FROM recent\n    WHERE total > 10\n)\nSELECT count(*)\nFROM big;");
    // Type names are identifiers to SQLite: they keep the case they were written in.
    Same("create table aligned",
         "create table users (id integer primary key, name text not null, email_address text unique, created_at text default (datetime('now')), foreign key (id) references other(id));",
         "CREATE TABLE users (\n    id            integer PRIMARY KEY,\n    name          text NOT NULL,\n"
         "    email_address text UNIQUE,\n    created_at    text DEFAULT (datetime('now')),\n"
         "    FOREIGN KEY (id) REFERENCES other (id)\n);");
    Same("insert values", "insert into t (a, b) values (1, 'x'), (2, 'y');",
         "INSERT INTO t (a, b)\nVALUES (1, 'x'), (2, 'y');");
    Same("update", "update t set a = 1, b = b + 1 where id = 3;", "UPDATE t\nSET a = 1, b = b + 1\nWHERE id = 3;");
    Same("delete", "delete from t where id = 3;", "DELETE FROM t\nWHERE id = 3;");
    Same("comments kept", "-- header\nselect a, -- the a\n  b from t; -- trailing\n\n\n\nselect 1;\n",
         "-- header\nSELECT\n    a, -- the a\n    b\nFROM t; -- trailing\n\nSELECT 1;\n");
    Same("trigger body",
         "create trigger trg after insert on t begin update u set n = n + 1 where id = new.id; delete from v; end;",
         "CREATE TRIGGER trg AFTER INSERT ON t BEGIN\n    UPDATE u\n    SET n = n + 1\n    WHERE id = new.id;\n"
         "    DELETE FROM v;\nEND;");
    Same("dot commands pass through", ".headers on\n.mode column\nselect 1;\n",
         ".headers on\n.mode column\nSELECT 1;\n");
    Same("database header untouched", "--   database:  ./x.db\nselect 1;\n", "--   database:  ./x.db\nSELECT 1;\n");
    Same("names that are keywords keep their case", "select key, replace(a, 'x', 'y') from t;\n",
         "SELECT key, replace(a, 'x', 'y')\nFROM t;\n");
    Same("unary minus", "select -1, - -2, a - 1 from t;", "SELECT -1, - -2, a - 1\nFROM t;");
    Same("view", "create view v as select a from t;", "CREATE VIEW v AS\nSELECT a\nFROM t;");
    Same("pragma", "pragma foreign_keys=on;", "PRAGMA foreign_keys = ON;");
    Same("upsert", "insert into t(a,b) values(1,2) on conflict(a) do update set b=excluded.b;",
         "INSERT INTO t (a, b)\nVALUES (1, 2)\nON CONFLICT (a) DO UPDATE\nSET b = excluded.b;");
    Same("compound", "select a from t union all select b from u order by 1;",
         "SELECT a\nFROM t\nUNION ALL\nSELECT b\nFROM u\nORDER BY 1;");
    Same("tabs", "select aaaaaaaaaaaaaaaaaaaa, bbbbbbbbbbbbbbbbbbbbbb, cccccccccccccccccccccccc, dddddddddd from t;",
         "SELECT\n\taaaaaaaaaaaaaaaaaaaa,\n\tbbbbbbbbbbbbbbbbbbbbbb,\n\tcccccccccccccccccccccccc,\n\tdddddddddd\nFROM t;",
         [] {
             sqlfmt::Options o;
             o.use_tabs = true;
             return o;
         }());

    // Refusals: a syntax error, with SQLite's message and its line.
    {
        const sqlfmt::Result r = sqlfmt::Format("select 1;\nselect from where;\n");
        CHECK(!r.ok);
        CHECK(r.error.find("syntax error") != std::string::npos);
        CHECK(r.error_line == 1);
        const sqlfmt::Result inc = sqlfmt::Format("select (1\n");
        CHECK(!inc.ok);
        CHECK(inc.error == "incomplete input");
    }
    std::printf("mep-sql-format-test: %d checks passed\n", checks);
    return 0;
}
