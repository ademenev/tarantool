--
-- gh-2677: box.session.push.
--

--
-- Usage.
--
box.session.push()
box.session.push(100)
box.session.push(100, {sync = -200})

--
-- Test text protocol.
--
test_run = require('test_run').new()
console = require('console')
netbox = require('net.box')
fiber = require('fiber')
s = console.listen(3434)
c = netbox.connect(3434, {console = true})
c:eval('100')
messages = {}
test_run:cmd("setopt delimiter ';'")
function on_push(message)
    table.insert(messages, message)
end;

function do_pushes()
    local sync = box.session.sync()
    for i = 1, 5 do
        box.session.push(i, {sync = sync})
        fiber.sleep(0.01)
    end
    return 300
end;
test_run:cmd("setopt delimiter ''");
-- Ensure a pushed message can ignored on a client.
c:eval('do_pushes()')
-- Now start catching pushes.
c:eval('do_pushes()', {on_push = on_push})
messages

c:close()
s:close()

--
-- Test binary protocol.
--
box.schema.user.grant('guest','read,write,execute','universe')

c = netbox.connect(box.cfg.listen)
c:ping()
messages = {}
c:call('do_pushes', {}, {on_push = on_push})
messages

-- Add a little stress: many pushes with different syncs, from
-- different fibers and DML/DQL requests.

catchers = {}
started = 0
finished = 0
s = box.schema.create_space('test', {format = {{'field1', 'integer'}}})
pk = s:create_index('pk')
c:reload_schema()
test_run:cmd("setopt delimiter ';'")
function dml_push_and_dml(key)
    local sync = box.session.sync()
    box.session.push('started dml', {sync = sync})
    s:replace{key}
    box.session.push('continued dml', {sync = sync})
    s:replace{-key}
    box.session.push('finished dml', {sync = sync})
    return key
end;
function do_pushes(val)
    local sync = box.session.sync()
    for i = 1, 5 do
        box.session.push(i, {sync = sync})
        fiber.yield()
    end
    return val
end;
function push_catcher_f()
    fiber.yield()
    started = started + 1
    local catcher = {messages = {}, retval = nil, is_dml = false}
    catcher.retval = c:call('do_pushes', {started}, {on_push = function(message)
        table.insert(catcher.messages, message)
    end})
    table.insert(catchers, catcher)
    finished = finished + 1
end;
function dml_push_and_dml_f()
    fiber.yield()
    started = started + 1
    local catcher = {messages = {}, retval = nil, is_dml = true}
    catcher.retval = c:call('dml_push_and_dml', {started}, {on_push = function(message)
        table.insert(catcher.messages, message)
    end})
    table.insert(catchers, catcher)
    finished = finished + 1
end;
-- At first check that a pushed message can be ignored in a binary
-- protocol too.
c:call('do_pushes', {300});
-- Then do stress.
for i = 1, 200 do
    fiber.create(dml_push_and_dml_f)
    fiber.create(push_catcher_f)
end;
while finished ~= 400 do fiber.sleep(0.1) end;

for _, c in pairs(catchers) do
    if c.is_dml then
        assert(#c.messages == 3)
        assert(c.messages[1] == 'started dml')
        assert(c.messages[2] == 'continued dml')
        assert(c.messages[3] == 'finished dml')
        assert(s:get{c.retval})
        assert(s:get{-c.retval})
    else
        assert(c.retval)
        assert(#c.messages == 5)
        for k, v in pairs(c.messages) do
            assert(k == v)
        end
    end
end;
test_run:cmd("setopt delimiter ''");

#s:select{}

--
-- Test binary pushes.
--
ibuf = require('buffer').ibuf()
msgpack = require('msgpack')
messages = {}
resp_len = c:call('do_pushes', {300}, {on_push = on_push, buffer = ibuf})
resp_len
messages
decoded = {}
r = nil
for i = 1, #messages do r, ibuf.rpos = msgpack.decode_unchecked(ibuf.rpos) table.insert(decoded, r) end
decoded
r, _ = msgpack.decode_unchecked(ibuf.rpos)
r

c:close()
s:drop()

box.schema.user.revoke('guest', 'read,write,execute', 'universe')

--
-- Ensure can not push in background.
--
ok = nil
err = nil
function back_push_f() ok, err = pcall(box.session.push, 100, {sync = 100}) end
f = fiber.create(back_push_f)
while f:status() ~= 'dead' do fiber.sleep(0.01) end
ok, err
