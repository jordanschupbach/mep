#!/usr/bin/env python3
"""The SQL oracle for grammar_diff.py: exit 0 when SQLite or PostgreSQL
accepts the file's syntax, 1 when neither does.

SQL has no single reference parser, so this asks two real ones:

* SQLite (the stdlib sqlite3 module): the file is split into complete
  statements (sqlite3.complete_statement) and each is executed in turn
  in a fresh :memory: database, so a CREATE TABLE makes the INSERT after
  it prepare. Only syntax errors count ("syntax error", "unrecognized
  token", "incomplete input"); "no such table" and the like are about
  the database, not the text.
* PostgreSQL's own parser through pglast, when it is importable (run
  grammar_diff under a python with pglast to use it).

A file valid only in another dialect (T-SQL, MySQL...) counts as
rejected, so for SQL the gate is `--old-clean`; this answers "is this
difference in code a real database reads?" for the two dialects with a
parser at hand.
"""

import re
import sqlite3
import sys

SYNTAX = re.compile(r'syntax error|unrecognized token|incomplete input')


def sqlite_accepts(text):
    db = sqlite3.connect(':memory:')
    statement = ''
    for line in text.splitlines(keepends=True):
        statement += line
        if not sqlite3.complete_statement(statement):
            continue
        try:
            db.execute(statement)
        except sqlite3.Warning:
            pass  # more than one statement on a line: fall back to executescript below
        except sqlite3.Error as e:
            if SYNTAX.search(str(e)):
                return False
        statement = ''
    if statement.strip():
        # A last statement without its `;`.
        try:
            db.execute(statement)
        except sqlite3.Error as e:
            if SYNTAX.search(str(e)):
                return False
    return True


def postgres_accepts(text):
    try:
        import pglast
    except ImportError:
        return False
    try:
        pglast.parse_sql(text)
        return True
    except Exception:
        return False


def main():
    text = open(sys.argv[1], 'rb').read().decode('utf-8', errors='replace')
    if sqlite_accepts(text) or postgres_accepts(text):
        return 0
    return 1


if __name__ == '__main__':
    sys.exit(main())
