#ifndef INCLUDES_TARANTOOL_BOX_VY_QUOTA_H
#define INCLUDES_TARANTOOL_BOX_VY_QUOTA_H
/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <small/rlist.h>
#include <tarantool_ev.h>

#include "trivia/util.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct fiber;
struct vy_quota;

/** Rate limit state. */
struct vy_rate_limit {
	/** Max allowed rate, per second. */
	size_t rate;
	/** Current quota. */
	ssize_t value;
};

/** Initialize a rate limit state. */
static inline void
vy_rate_limit_create(struct vy_rate_limit *rl)
{
	rl->rate = SIZE_MAX;
	rl->value = SSIZE_MAX;
}

/** Set rate limit. */
static inline void
vy_rate_limit_set(struct vy_rate_limit *rl, size_t rate)
{
	rl->rate = rate;
}

/**
 * Return true if quota may be consumed without exceeding
 * the configured rate limit.
 */
static inline bool
vy_rate_limit_may_use(struct vy_rate_limit *rl)
{
	return rl->value > 0;
}

/** Consume the given amount of quota. */
static inline void
vy_rate_limit_use(struct vy_rate_limit *rl, size_t size)
{
	rl->value -= size;
}

/** Release the given amount of quota. */
static inline void
vy_rate_limit_unuse(struct vy_rate_limit *rl, size_t size)
{
	rl->value += size;
}

/**
 * Replenish quota by the amount accumulated for the given
 * time interval.
 */
static inline void
vy_rate_limit_refill(struct vy_rate_limit *rl, double time)
{
	double size = rl->rate * time;
	double value = rl->value + size;
	/* Allow bursts up to 2x rate. */
	value = MIN(value, size * 2);
	rl->value = MIN(value, SSIZE_MAX);
}

typedef void
(*vy_quota_exceeded_f)(struct vy_quota *quota);

/**
 * Quota consumer priority. Determines how a consumer will be
 * rate limited. See also vy_quota::rate_limit.
 */
enum vy_quota_consumer_prio {
	/**
	 * Transaction processor priority.
	 *
	 * Transaction throttling pursues two goals. First, it is
	 * capping memory consumption rate so that the hard memory
	 * limit will not be hit before memory dump has completed
	 * (memory-based throttling). Second, we must make sure
	 * that compaction jobs keep up with dumps to keep the read
	 * amplification within bounds (disk-based throttling).
	 * Transactions ought to respect them both.
	 */
	VY_QUOTA_CONSUMER_TX = 0,
	/**
	 * Compaction job priority.
	 *
	 * Compaction jobs may need some quota too, because they
	 * may generate deferred DELETEs for secondary indexes.
	 * Apparently, we must not impose the rate limit that
	 * is supposed to speed up compaction on them, however
	 * they still have to respect memory-based throttling to
	 * avoid long stalls.
	 */
	VY_QUOTA_CONSUMER_COMPACTION = 1,
	/**
	 * A convenience shortcut for setting the rate limit for
	 * all kinds of consumers.
	 */
	VY_QUOTA_CONSUMER_ALL = VY_QUOTA_CONSUMER_COMPACTION,

	vy_quota_consumer_prio_MAX,
};

struct vy_quota_wait_node {
	/** Link in vy_quota::wait_queue. */
	struct rlist in_wait_queue;
	/** Fiber waiting for quota. */
	struct fiber *fiber;
	/** Amount of requested memory. */
	size_t size;
	/**
	 * Timestamp assigned to this fiber when it was put to
	 * sleep, see vy_quota::wait_timestamp for more details.
	 */
	int64_t timestamp;
};

/**
 * Quota used for accounting and limiting memory consumption
 * in the vinyl engine. It is NOT multi-threading safe.
 */
struct vy_quota {
	/** Set if the quota was enabled. */
	bool is_enabled;
	/**
	 * Memory limit. Once hit, new transactions are
	 * throttled until memory is reclaimed.
	 */
	size_t limit;
	/** Current memory consumption. */
	size_t used;
	/**
	 * If vy_quota_use() takes longer than the given
	 * value, warn about it in the log.
	 */
	double too_long_threshold;
	/**
	 * Called if the limit is hit when quota is consumed.
	 * It is supposed to trigger memory reclaim.
	 */
	vy_quota_exceeded_f quota_exceeded_cb;
	/**
	 * Monotonically growing timestamp assigned to consumers
	 * waiting for quota. It is used for balancing wakeups
	 * among wait queues: if two fibers from different wait
	 * queues may proceed, the one with the lowest timestamp
	 * will be picked.
	 *
	 * See also vy_quota_wait_node::timestamp.
	 */
	int64_t wait_timestamp;
	/**
	 * Queue of consumers waiting for quota, one per each
	 * consumer priority, linked by vy_quota_wait_node::state.
	 * Newcomers are added to the tail.
	 */
	struct rlist wait_queue[vy_quota_consumer_prio_MAX];
	/**
	 * Rate limit state, one per each consumer priority.
	 * A rate limit is enforced if and only if the consumer
	 * priority is less than or equal to its index.
	 */
	struct vy_rate_limit rate_limit[vy_quota_consumer_prio_MAX];
	/**
	 * Periodic timer that is used for refilling the rate
	 * limit value.
	 */
	ev_timer timer;
};

/**
 * Initialize a quota object.
 *
 * Note, the limit won't be imposed until vy_quota_enable()
 * is called.
 */
void
vy_quota_create(struct vy_quota *q, size_t limit,
		vy_quota_exceeded_f quota_exceeded_cb);

/**
 * Enable the configured limit for a quota object.
 */
void
vy_quota_enable(struct vy_quota *q);

/**
 * Destroy a quota object.
 */
void
vy_quota_destroy(struct vy_quota *q);

/**
 * Set memory limit. If current memory usage exceeds
 * the new limit, invoke the callback.
 */
void
vy_quota_set_limit(struct vy_quota *q, size_t limit);

/**
 * Set the rate limit for consumers with priority less than or
 * equal to @prio, in bytes per second.
 */
void
vy_quota_set_rate_limit(struct vy_quota *q, enum vy_quota_consumer_prio prio,
			size_t rate);

/**
 * Consume @size bytes of memory. In contrast to vy_quota_use()
 * this function does not throttle the caller.
 */
void
vy_quota_force_use(struct vy_quota *q, enum vy_quota_consumer_prio prio,
		   size_t size);

/**
 * Release @size bytes of memory.
 */
void
vy_quota_release(struct vy_quota *q, size_t size);

/**
 * Try to consume @size bytes of memory, throttle the caller
 * if the limit is exceeded. @timeout specifies the maximal
 * time to wait. Return 0 on success, -1 on timeout.
 *
 * Usage pattern:
 *
 *   size_t reserved = <estimate>;
 *   if (vy_quota_use(q, reserved, timeout) != 0)
 *           return -1;
 *   <allocate memory>
 *   size_t used = <actually allocated>;
 *   vy_quota_adjust(q, reserved, used);
 *
 * We use two-step quota allocation strategy (reserve-consume),
 * because we may not yield after we start inserting statements
 * into a space so we estimate the allocation size and wait for
 * quota before committing statements. At the same time, we
 * cannot precisely estimate the size of memory we are going to
 * consume so we adjust the quota after the allocation.
 *
 * The size of memory allocated while committing a transaction
 * may be greater than an estimate, because insertion of a
 * statement into an in-memory index can trigger allocation
 * of a new index extent. This should not normally result in a
 * noticeable breach in the memory limit, because most memory
 * is occupied by statements, but we need to adjust the quota
 * accordingly after the allocation in this case.
 *
 * The actual memory allocation size may also be less than an
 * estimate if the space has multiple indexes, because statements
 * are stored in the common memory level, which isn't taken into
 * account while estimating the size of a memory allocation.
 */
int
vy_quota_use(struct vy_quota *q, enum vy_quota_consumer_prio prio,
	     size_t size, double timeout);

/**
 * Adjust quota after allocating memory.
 *
 * @reserved: size of quota reserved by vy_quota_use().
 * @used: size of memory actually allocated.
 *
 * See also vy_quota_use().
 */
void
vy_quota_adjust(struct vy_quota *q, enum vy_quota_consumer_prio prio,
		size_t reserved, size_t used);

/**
 * Block the caller until the quota is not exceeded.
 */
static inline void
vy_quota_wait(struct vy_quota *q, enum vy_quota_consumer_prio prio)
{
	vy_quota_use(q, prio, 0, TIMEOUT_INFINITY);
}

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* INCLUDES_TARANTOOL_BOX_VY_QUOTA_H */
