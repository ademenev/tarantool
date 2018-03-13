env = require('test_run')
test_run = env.new()

SERVERS = { 'master1', 'master2' }
-- Start servers
test_run:create_cluster(SERVERS)
-- Wait for full mesh
test_run:wait_fullmesh(SERVERS)

test_run:cmd("switch master1")
box.space._schema:insert({'1'})
box.space._schema:select('1')

fiber = require('fiber')
fiber.sleep(0.1)

test_run:cmd("switch master2")
box.space._schema:select('1')
test_run:cmd("stop server master1")
fio = require('fio')
fio.unlink(fio.pathjoin(fio.abspath("."), string.format('master1/%020d.xlog', 8)))
test_run:cmd("start server master1")

test_run:cmd("switch master1")
box.space._schema:select('1')

test_run:cmd("switch default")
test_run:cmd("stop server master1")
test_run:cmd("stop server master2")

