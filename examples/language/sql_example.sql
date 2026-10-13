-- A small SQL tour, run with sqlite3 against a fresh in-memory database:
-- tables, inserts, a join, aggregates, a CTE and a window function.

.headers on
.mode column

CREATE TABLE fruit (
    id    INTEGER PRIMARY KEY,
    name  TEXT NOT NULL,
    color TEXT NOT NULL
);

CREATE TABLE sale (
    fruit_id INTEGER NOT NULL REFERENCES fruit (id),
    qty      INTEGER NOT NULL,
    price    REAL NOT NULL
);

INSERT INTO fruit (id, name, color) VALUES
    (1, 'apple', 'red'),
    (2, 'banana', 'yellow'),
    (3, 'avocado', 'green'),
    (4, 'cherry', 'red');

INSERT INTO sale (fruit_id, qty, price) VALUES
    (1, 3, 0.50), (1, 2, 0.55), (2, 6, 0.25),
    (3, 1, 1.80), (4, 20, 0.10), (4, 10, 0.12);

SELECT 'hello, world' AS greeting;

-- Revenue per fruit, best first.
SELECT f.name, SUM(s.qty) AS units, ROUND(SUM(s.qty * s.price), 2) AS revenue
FROM fruit AS f
JOIN sale AS s ON s.fruit_id = f.id
GROUP BY f.name
ORDER BY revenue DESC;

-- Revenue by color, with each color's share of the total.
WITH by_color AS (
    SELECT f.color, SUM(s.qty * s.price) AS revenue
    FROM fruit AS f
    JOIN sale AS s ON s.fruit_id = f.id
    GROUP BY f.color
)
SELECT color,
       ROUND(revenue, 2) AS revenue,
       ROUND(100.0 * revenue / SUM(revenue) OVER (), 1) AS pct
FROM by_color
ORDER BY revenue DESC;
