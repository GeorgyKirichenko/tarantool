/*
 * Copyright 2010-2018, Tarantool AUTHORS, please see AUTHORS file.
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
#include "vy_quota.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <tarantool_ev.h>

#include "diag.h"
#include "error.h"
#include "errcode.h"
#include "fiber.h"
#include "say.h"
#include "trivia/util.h"

/**
 * Quota timer period, in seconds.
 *
 * The timer is used for replenishing the rate limit value so
 * its period defines how long throttled transactions will wait.
 * Therefore use a relatively small period.
 */
static const double VY_QUOTA_TIMER_PERIOD = 0.1;

/**
 * Iterate over rate limit states that are enforced for a consumer
 * with the given priority.
 */
#define vy_quota_for_each_rate_limit(quota, rl, prio) \
	for (struct vy_rate_limit *rl = &(quota)->rate_limit[prio]; \
	     rl - (quota)->rate_limit < vy_quota_consumer_prio_MAX; rl++)

/**
 * Return true if the requested amount of memory may be consumed
 * right now, false if consumers have to wait.
 */
static inline bool
vy_quota_may_use(struct vy_quota *q, enum vy_quota_consumer_prio prio,
		 size_t size)
{
	if (!q->is_enabled)
		return true;
	if (q->used + size > q->limit)
		return false;
	vy_quota_for_each_rate_limit(q, rl, prio) {
		if (!vy_rate_limit_may_use(rl))
			return false;
	}
	return true;
}

/**
 * Consume the given amount of memory without checking the limit.
 */
static inline void
vy_quota_do_use(struct vy_quota *q, enum vy_quota_consumer_prio prio,
		size_t size)
{
	q->used += size;
	vy_quota_for_each_rate_limit(q, rl, prio)
		vy_rate_limit_use(rl, size);
}

/**
 * Return the given amount of memory without waking blocked fibers.
 * This function is an exact opposite of vy_quota_do_use().
 */
static inline void
vy_quota_do_unuse(struct vy_quota *q, enum vy_quota_consumer_prio prio,
		  size_t size)
{
	assert(q->used >= size);
	q->used -= size;
	vy_quota_for_each_rate_limit(q, rl, prio)
		vy_rate_limit_unuse(rl, size);
}

/**
 * Invoke the registered callback in case memory usage exceeds
 * the configured limit.
 */
static inline void
vy_quota_check_limit(struct vy_quota *q)
{
	if (q->is_enabled && q->used > q->limit)
		q->quota_exceeded_cb(q);
}

/**
 * Wake up the first consumer in the line waiting for quota.
 */
static void
vy_quota_signal(struct vy_quota *q)
{
	/*
	 * Wake up a consumer that has waited most no matter
	 * whether it's high or low priority. This assures that
	 * high priority consumers don't uncontrollably throttle
	 * low priority ones.
	 */
	struct vy_quota_wait_node *oldest = NULL;
	for (int i = 0; i < vy_quota_consumer_prio_MAX; i++) {
		struct rlist *wq = &q->wait_queue[i];
		if (rlist_empty(wq))
			continue;

		struct vy_quota_wait_node *n;
		n = rlist_first_entry(wq, struct vy_quota_wait_node,
				      in_wait_queue);
		/*
		 * No need in waking up a consumer if it will have
		 * to go back to sleep immediately.
		 */
		if (!vy_quota_may_use(q, i, n->size))
			continue;

		if (oldest == NULL || oldest->timestamp > n->timestamp)
			oldest = n;
	}
	if (oldest != NULL)
		fiber_wakeup(oldest->fiber);
}

static void
vy_quota_timer_cb(ev_loop *loop, ev_timer *timer, int events)
{
	(void)loop;
	(void)events;

	struct vy_quota *q = timer->data;

	for (int i = 0; i < vy_quota_consumer_prio_MAX; i++)
		vy_rate_limit_refill(&q->rate_limit[i], VY_QUOTA_TIMER_PERIOD);
	vy_quota_signal(q);
}

void
vy_quota_create(struct vy_quota *q, size_t limit,
		vy_quota_exceeded_f quota_exceeded_cb)
{
	q->is_enabled = false;
	q->limit = limit;
	q->used = 0;
	q->too_long_threshold = TIMEOUT_INFINITY;
	q->quota_exceeded_cb = quota_exceeded_cb;
	q->wait_timestamp = 0;
	for (int i = 0; i < vy_quota_consumer_prio_MAX; i++) {
		rlist_create(&q->wait_queue[i]);
		vy_rate_limit_create(&q->rate_limit[i]);
	}
	ev_timer_init(&q->timer, vy_quota_timer_cb, 0, VY_QUOTA_TIMER_PERIOD);
	q->timer.data = q;
}

void
vy_quota_enable(struct vy_quota *q)
{
	assert(!q->is_enabled);
	q->is_enabled = true;
	ev_timer_start(loop(), &q->timer);
	vy_quota_check_limit(q);
}

void
vy_quota_destroy(struct vy_quota *q)
{
	ev_timer_stop(loop(), &q->timer);
}

void
vy_quota_set_limit(struct vy_quota *q, size_t limit)
{
	q->limit = limit;
	vy_quota_check_limit(q);
	vy_quota_signal(q);
}

void
vy_quota_set_rate_limit(struct vy_quota *q, enum vy_quota_consumer_prio prio,
			size_t rate)
{
	vy_rate_limit_set(&q->rate_limit[prio], rate);
}

void
vy_quota_force_use(struct vy_quota *q, enum vy_quota_consumer_prio prio,
		   size_t size)
{
	vy_quota_do_use(q, prio, size);
	vy_quota_check_limit(q);
}

void
vy_quota_release(struct vy_quota *q, size_t size)
{
	/*
	 * Don't use vy_quota_do_unuse(), because it affects
	 * the rate limit state.
	 */
	assert(q->used >= size);
	q->used -= size;
	vy_quota_signal(q);
}

int
vy_quota_use(struct vy_quota *q, enum vy_quota_consumer_prio prio,
	     size_t size, double timeout)
{
	if (vy_quota_may_use(q, prio, size)) {
		vy_quota_do_use(q, prio, size);
		return 0;
	}

	/*
	 * Fail early if the configured memory limit never allows
	 * us to commit the transaction.
	 */
	if (size > q->limit) {
		diag_set(OutOfMemory, size, "lsregion", "vinyl transaction");
		return -1;
	}

	/* Wait for quota. */
	double wait_start = ev_monotonic_now(loop());
	double deadline = wait_start + timeout;

	struct vy_quota_wait_node wait_node = {
		.fiber = fiber(),
		.size = size,
		.timestamp = ++q->wait_timestamp,
	};
	rlist_add_tail_entry(&q->wait_queue[prio], &wait_node, in_wait_queue);

	do {
		/*
		 * If the requested amount of memory cannot be
		 * consumed due to the configured limit, notify
		 * the caller before going to sleep so that it
		 * can start memory reclaim immediately.
		 */
		if (q->used + size > q->limit)
			q->quota_exceeded_cb(q);

		double now = ev_monotonic_now(loop());
		fiber_yield_timeout(deadline - now);

		if (now >= deadline) {
			rlist_del_entry(&wait_node, in_wait_queue);
			diag_set(ClientError, ER_VY_QUOTA_TIMEOUT);
			return -1;
		}
	} while (!vy_quota_may_use(q, prio, size));

	rlist_del_entry(&wait_node, in_wait_queue);

	double wait_time = ev_monotonic_now(loop()) - wait_start;
	if (wait_time > q->too_long_threshold) {
		say_warn("waited for %zu bytes of vinyl memory quota "
			 "for too long: %.3f sec", size, wait_time);
	}

	vy_quota_do_use(q, prio, size);
	/*
	 * Blocked consumers are awaken one by one to preserve
	 * the order they were put to sleep. It's a responsibility
	 * of a consumer that managed to acquire the requested
	 * amount of quota to wake up the next one in the line.
	 */
	vy_quota_signal(q);
	return 0;
}

void
vy_quota_adjust(struct vy_quota *q, enum vy_quota_consumer_prio prio,
		size_t reserved, size_t used)
{
	if (reserved > used) {
		vy_quota_do_unuse(q, prio, reserved - used);
		vy_quota_signal(q);
	}
	if (reserved < used) {
		vy_quota_do_use(q, prio, used - reserved);
		vy_quota_check_limit(q);
	}
}
