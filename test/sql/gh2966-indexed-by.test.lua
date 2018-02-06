box.cfg{}

-- Check that 'INDEXED BY' clause works correctly without
-- raising syntax error, which happened earlier after keyword patch
-- (ban keywords reserved by standard)

box.sql.execute("CREATE TABLE a (id PRIMARY KEY, a INTEGER);");
box.sql.execute("CREATE INDEX i on a(a);");
box.sql.execute("INSERT INTO a VALUES (1,2), (3,4), (5,6);");
box.sql.execute("SELECT id FROM a INDEXED BY i");
