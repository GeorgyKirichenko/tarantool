test_run = require('test_run').new()
fiber = require('fiber')

SERVERS = {'prune_dead1', 'prune_dead2', 'prune_dead3'}

-- Deploy cluster
test_run:create_cluster(SERVERS, "replication")
test_run:wait_fullmesh(SERVERS)

-- check that we can monitor replica set and all replicas are alive
test_run:cmd('switch prune_dead1')
alive = box.replication.get_alive_replicas(TIMEOUT)
table.getn(alive) == box.space._cluster:count()
box.info.replication[1].uuid == alive[1]
box.info.replication[2].uuid == alive[2]
box.info.replication[3].uuid == alive[3]

-- check if we turn off replication replica is considered as alive
test_run:cmd('switch prune_dead2')
replication = box.cfg.replication
box.cfg{replication = ''}
test_run:cmd('switch prune_dead1')
alive = box.replication.get_alive_replicas(TIMEOUT)
table.getn(alive) == box.space._cluster:count()
test_run:cmd('switch prune_dead2')
box.cfg{replication = replication}
test_run:cmd('switch default')
test_run:wait_fullmesh(SERVERS)

-- stop replica to see that is not in alive list
test_run:cmd('stop server prune_dead2')
test_run:cmd('switch prune_dead1')
alive = box.replication.get_alive_replicas(TIMEOUT)
table.getn(alive) < box.space._cluster:count()
all = {box.info.replication[1].uuid, box.info.replication[2].uuid, box.info.replication[3].uuid}
box.info.replication[find_excess(all, alive)].upstream.status == "disconnected"
box.info.replication[find_excess(all, alive)].downstream.status == "stopped"

-- prune  dead replica
box.replication.prune_replicas(alive)
table.getn(alive) == box.space._cluster:count()

-- Cleanup
test_run:cmd("switch default")
test_run:cmd('start server prune_dead2')
test_run:drop_cluster(SERVERS)
