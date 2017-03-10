#pragma once
#include <cstd/std.h>
#include <cstd/unix.h>
#include <cstruct/binary_tree.h>

/*
 * Simple promise abstraction to track and to serialise asynchronous tasks
 *
 * Note: no timer is implemented, timeouts are triggered by calling
 * promise_timeout, which causes all promises with deadline before the given
 * time point to expire.
 */

enum promise_resolution
{
	awr_success = 0,
	awr_fail = 1,
	awr_cancel = 2,
	awr_timeout = 3
};

/* Handle used to identify a promise */
typedef int64_t promise;

/* Value is set at -1 to prevent wraparound */
const promise promise_fail = -1;

/*
 * TODO: Complete the tempus library so we can use that for time points instead
 * of this.
 */
struct promise_deadline
{
	int64_t s;
	uint32_t ns;
};

/*
 * TODO: I need to implement red/black for the binary_tree class, otherwise
 * these trees will basically behave as linked-lists for sequential keys.
 */
struct promises
{
	pthread_mutex_t mx;
	promise next;
	struct binary_tree cb;
	struct binary_tree deadline;
};

/*** Promise tracker lifetime ***/

void promise_init(struct promises *state);
void promise_destroy(struct promises *state);

/*** Promise creation: callback ***/

/* Register a task with callback, closure, deadline */
typedef void promise_callback(void *closure, enum promise_resolution result, void *data);
promise promise_open(struct promises *state, promise_callback *cb, void *closure, const struct promise_deadline *deadline);

/*** Promise creation: async/await ***/

/* Register a task (for use with promise_await) */
promise promise_async(struct promises *state, const struct promise_deadline *deadline);
/* Block until task (registered with promise_async) compeletes */
enum promise_resolution promise_await(struct promises *state, promise promise, void **data);

/*** Promise completion ***/

/* Complete a task */
void promise_resolve(struct promises *state, promise promise, void *result);
void promise_reject(struct promises *state, promise promise, void *error);
void promise_cancel(struct promises *state, promise promise, void *reason);
bool promise_complete(struct promises *state, promise promise, enum promise_resolution result, void *data);

/* Complete all tasks */
void promise_reject_all(struct promises *state, void *error);
void promise_cancel_all(struct promises *state, void *reason);

/* Check deadlines, trigger timeouts */
void promise_timeout(struct promises *state, const struct promise_deadline *now);
