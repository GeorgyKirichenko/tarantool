#!/usr/bin/env tarantool

-- get instance name from filename (prune_dead.lua => prune_dead1)
local INSTANCE_ID = string.match(arg[0], "%d")

local SOCKET_DIR = require('fio').cwd()

local function instance_uri(instance_id)
    --return 'localhost:'..(3310 + instance_id)
    return SOCKET_DIR..'/prune_dead'..instance_id..'.sock';
end

-- start console first
require('console').listen(os.getenv('ADMIN'))

box.cfg({
    listen = instance_uri(INSTANCE_ID);
    replication = {
        instance_uri(1);
        instance_uri(2);
        instance_uri(3);
    };
})

TIMEOUT = 0.01

box.once("bootstrap", function()
    local test_run = require('test_run').new()
    box.schema.user.grant("guest", 'replication')
    box.schema.space.create('test', {engine = test_run:get_cfg('engine')})
    box.space.test:create_index('primary')
end)

-- helper functions
function contains (uuid_table, value)
    for i = 1, table.getn(uuid_table) do
        if (uuid_table[i] == value) then return true end
    end
    return false
end

function find_excess (uuid_all, uuid_alive)
    local i = 1
    while (i <= table.getn(uuid_alive)) do
        if (not contains(uuid_alive, uuid_all[i])) then return i  end
        i = i + 1
    end
    return i
end
