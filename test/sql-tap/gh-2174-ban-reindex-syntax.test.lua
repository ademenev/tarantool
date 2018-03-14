#!/usr/bin/env tarantool

-- this test will be deleted in scope of #3195
test = require("sqltester")
test:plan(1)

test:execsql("DROP TABLE IF EXISTS t1");
test:execsql("CREATE TABLE t1(a INT PRIMARY KEY)");
test:execsql("CREATE INDEX i1 on t1(a)");

local ok = pcall(test.execsql, test, [[
    REINDEX i1 ON t1
]])

test:ok(not ok, 'reindex syntax must be banned')

test:finish_test()
