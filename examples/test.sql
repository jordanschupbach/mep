-- database: test.db

CREATE TABLE IF NOT EXISTS users (
    id SERIAL PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    email VARCHAR(100) UNIQUE NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    asdf INTEGER NOT NULL
);

-- Add a few users
INSERT INTO users (name, email, asdf) VALUES 
    ('Alice Smith', 'alice@example.com', 1),
    ('Bob Johnson', 'bob@example.com', 2),
    ('Charlie Brown', 'charlie@example.com', 3),
    ('Diana Prince', 'diana@example.com', 4)
    ON CONFLICT (email) DO NOTHING;
    
select * from users;    


