test_run = require('test_run').new()
engine = test_run:get_cfg('engine')
box.sql.execute('pragma sql_default_engine=\''..engine..'\'')

-- box.cfg()

-- create space
box.sql.execute("CREATE TABLE zzzoobar (c1, c2 PRIMARY KEY, c3, c4)")

-- Debug
-- box.sql.execute("PRAGMA vdbe_debug=ON ; INSERT INTO zzzoobar VALUES (111, 222, 'c3', 444)")

box.sql.execute("CREATE INDEX zb ON zzzoobar(c1, c3)")

-- Dummy entry
box.sql.execute("INSERT INTO zzzoobar VALUES (111, 222, 'c3', 444)")

box.sql.execute("DROP TABLE zzzoobar")

-- Table does not exist anymore. Should error here.
box.sql.execute("INSERT INTO zzzoobar VALUES (111, 222, 'c3', 444)")

-- Cleanup
-- DROP TABLE should do the job

-- Debug
-- require("console").start()

--
-- gh-3592: segmentation fault when table with error during
-- creation is dropped.
-- We should grant user enough rights to create space, but not
-- enough to create index.
--
box.schema.user.create('tmp')
box.schema.user.grant('tmp', 'create', 'universe')
box.schema.user.grant('tmp', 'write', 'space', '_space')
box.schema.user.grant('tmp', 'write', 'space', '_schema')
box.session.su('tmp')
--
-- Error: user do not have rights to write in box.space._index.
-- Space that was already created should be automatically dropped.
--
box.sql.execute('create table t1 (id int primary key, a int)')
-- Error: no such table.
box.sql.execute('drop table t1')
box.session.su('admin')
box.schema.user.drop('tmp')
