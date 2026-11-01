-- database: bookstore.db
--
-- A small bookstore in SQLite: open this file and press <leader>uu.
--
-- The line above ties this file to examples/sql/bookstore.db. The SQL UI
-- mode opens a sqlite3 console on it, and the language server reads its
-- schema for completion, hover and the lints. Press <leader>rr to (re)build
-- the database from this file, then <leader>re on any statement below to
-- run just that one; SELECT results land in the Results panel.
--
-- Running the whole file again is safe: it starts by dropping what it makes.

PRAGMA foreign_keys = ON;

DROP VIEW IF EXISTS author_revenue;
DROP TRIGGER IF EXISTS order_line_stock;
DROP TABLE IF EXISTS order_line;
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS customer;
DROP TABLE IF EXISTS book;
DROP TABLE IF EXISTS genre;
DROP TABLE IF EXISTS author;

------------------------------------------------------------------------
-- Schema
------------------------------------------------------------------------

CREATE TABLE author (
    id      INTEGER PRIMARY KEY,
    name    TEXT NOT NULL UNIQUE,
    born    INTEGER,
    country TEXT
) STRICT;

-- Genres form a tree: "Science fiction" sits under "Fiction".
CREATE TABLE genre (
    id        INTEGER PRIMARY KEY,
    name      TEXT NOT NULL,
    parent_id INTEGER REFERENCES genre (id)
) STRICT;

CREATE TABLE book (
    id          INTEGER PRIMARY KEY,
    title       TEXT NOT NULL,
    author_id   INTEGER NOT NULL REFERENCES author (id),
    genre_id    INTEGER REFERENCES genre (id),
    published   INTEGER,
    price_cents INTEGER NOT NULL CHECK (price_cents > 0),
    stock       INTEGER NOT NULL DEFAULT 0 CHECK (stock >= 0),
    -- A generated column: computed from price_cents, never stored by hand.
    price       REAL GENERATED ALWAYS AS (price_cents / 100.0) VIRTUAL,
    tags        TEXT NOT NULL DEFAULT '[]'  -- a JSON array
) STRICT;

CREATE INDEX book_author ON book (author_id);

CREATE TABLE customer (
    id    INTEGER PRIMARY KEY,
    name  TEXT NOT NULL,
    email TEXT NOT NULL UNIQUE COLLATE NOCASE
) STRICT;

CREATE TABLE orders (
    id          INTEGER PRIMARY KEY,
    customer_id INTEGER NOT NULL REFERENCES customer (id),
    placed_at   TEXT NOT NULL DEFAULT (datetime('now'))
) STRICT;

CREATE TABLE order_line (
    order_id INTEGER NOT NULL REFERENCES orders (id),
    book_id  INTEGER NOT NULL REFERENCES book (id),
    qty      INTEGER NOT NULL CHECK (qty > 0),
    PRIMARY KEY (order_id, book_id)
) STRICT, WITHOUT ROWID;

-- Selling a book takes it out of stock; the CHECK on book.stock refuses
-- an order for more copies than the shop has.
CREATE TRIGGER order_line_stock
AFTER INSERT ON order_line
BEGIN
    UPDATE book SET stock = stock - NEW.qty WHERE id = NEW.book_id;
END;

CREATE VIEW author_revenue AS
SELECT a.name AS author,
       COUNT(DISTINCT b.id) AS titles,
       SUM(ol.qty) AS copies,
       ROUND(SUM(ol.qty * b.price), 2) AS revenue
FROM author AS a
JOIN book AS b ON b.author_id = a.id
JOIN order_line AS ol ON ol.book_id = b.id
GROUP BY a.id;

------------------------------------------------------------------------
-- Data
------------------------------------------------------------------------

INSERT INTO author (id, name, born, country) VALUES
    (1, 'Ursula K. Le Guin', 1929, 'US'),
    (2, 'Stanisław Lem', 1921, 'PL'),
    (3, 'Octavia E. Butler', 1947, 'US'),
    (4, 'Italo Calvino', 1923, 'IT'),
    (5, 'Ted Chiang', 1967, 'US');

INSERT INTO genre (id, name, parent_id) VALUES
    (1, 'Fiction', NULL),
    (2, 'Science fiction', 1),
    (3, 'Fantasy', 1),
    (4, 'Short stories', 2),
    (5, 'Literary fiction', 1);

INSERT INTO book (id, title, author_id, genre_id, published, price_cents, stock, tags) VALUES
    (1, 'The Dispossessed', 1, 2, 1974, 1599, 12, '["utopia", "physics"]'),
    (2, 'A Wizard of Earthsea', 1, 3, 1968, 1199, 20, '["magic", "coming of age"]'),
    (3, 'Solaris', 2, 2, 1961, 1450, 7, '["first contact", "ocean"]'),
    (4, 'The Cyberiad', 2, 2, 1965, 1300, 3, '["robots", "humour"]'),
    (5, 'Kindred', 3, 2, 1979, 1250, 15, '["time travel"]'),
    (6, 'Parable of the Sower', 3, 2, 1993, 1399, 9, '["dystopia"]'),
    (7, 'Invisible Cities', 4, 5, 1972, 1100, 6, '["cities", "travel"]'),
    (8, 'Stories of Your Life and Others', 5, 4, 2002, 1699, 11, '["language", "first contact"]'),
    (9, 'Exhalation', 5, 4, 2019, 1799, 4, '["robots", "free will"]');

INSERT INTO customer (id, name, email) VALUES
    (1, 'Ada', 'ada@example.org'),
    (2, 'Brian', 'brian@example.org'),
    (3, 'Chidi', 'chidi@example.org');

INSERT INTO orders (id, customer_id, placed_at) VALUES
    (1, 1, '2026-10-01 09:12:00'),
    (2, 2, '2026-10-03 17:40:00'),
    (3, 1, '2026-10-15 12:05:00'),
    (4, 3, '2026-10-20 08:30:00');

INSERT INTO order_line (order_id, book_id, qty) VALUES
    (1, 1, 1), (1, 3, 1),
    (2, 8, 2), (2, 9, 1),
    (3, 5, 1), (3, 6, 1), (3, 7, 1),
    (4, 1, 2), (4, 8, 1);

------------------------------------------------------------------------
-- Queries: put the cursor on one and press <leader>re
------------------------------------------------------------------------

-- Best-selling authors (the view).
SELECT * FROM author_revenue ORDER BY revenue DESC;

-- Each book's rank by price within its genre (a window function).
SELECT g.name AS genre,
       b.title,
       b.price,
       RANK() OVER (PARTITION BY g.id ORDER BY b.price DESC) AS price_rank
FROM book AS b
JOIN genre AS g ON g.id = b.genre_id
ORDER BY g.name, price_rank;

-- The genre tree, walked with a recursive CTE.
WITH RECURSIVE tree (id, name, depth, path) AS (
    SELECT id, name, 0, name FROM genre WHERE parent_id IS NULL
    UNION ALL
    SELECT g.id, g.name, t.depth + 1, t.path || ' > ' || g.name
    FROM genre AS g
    JOIN tree AS t ON g.parent_id = t.id
)
SELECT printf('%.*c%s', depth * 2, ' ', name) AS genre, path
FROM tree
ORDER BY path;

-- Books by tag, unpacking the JSON arrays.
SELECT tag.value AS tag, group_concat(b.title, '; ') AS books
FROM book AS b, json_each(b.tags) AS tag
GROUP BY tag.value
HAVING COUNT(*) > 1
ORDER BY tag;

-- What each customer spent, with a running total over time.
SELECT c.name,
       date(o.placed_at) AS day,
       ROUND(SUM(ol.qty * b.price), 2) AS spent,
       ROUND(SUM(SUM(ol.qty * b.price)) OVER (
           PARTITION BY c.id ORDER BY o.placed_at
       ), 2) AS running_total
FROM customer AS c
JOIN orders AS o ON o.customer_id = c.id
JOIN order_line AS ol ON ol.order_id = o.id
JOIN book AS b ON b.id = ol.book_id
GROUP BY o.id
ORDER BY c.name, day;

-- Low stock (the trigger has been taking sold copies off).
SELECT title, stock FROM book WHERE stock < 5 ORDER BY stock;

-- An upsert: restock a book, or add it if it is new. RETURNING shows the row.
INSERT INTO book (id, title, author_id, genre_id, published, price_cents, stock)
VALUES (4, 'The Cyberiad', 2, 2, 1965, 1300, 10)
ON CONFLICT (id) DO UPDATE SET stock = stock + excluded.stock
RETURNING id, title, stock;

------------------------------------------------------------------------
-- Things to try
------------------------------------------------------------------------
--
-- * Type `b.` inside a query that has `book AS b` in its FROM clause:
--   completion offers book's columns. K (hover) on a table shows its
--   columns; on a function, its signature and documentation.
-- * gh on a function name (printf, json_each, RANK) or a table name opens
--   it in the Help panel.
-- * The Schema panel lists every table, view, index and trigger. Click a
--   table to preview its rows.
-- * The statements below have mistakes the language server points out.
--   Uncomment them one at a time to see the diagnostics:
--
-- SELECT titel FROM book;                        -- no such column
-- SELECT name FROM author JOIN customer USING (id); -- ambiguous column
-- SELECT * FROM book WHERE genre_id = NULL;      -- always unknown: use IS NULL
-- SELECT substr(title) FROM book;                -- wrong number of arguments
-- INSERT INTO customer (name, email) VALUES ('Dee');  -- 2 columns, 1 value
