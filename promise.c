#if 0
(
set -eu
declare -r tmp="$(mktemp)"
gcc -O3 -DSIMPLE_LOGGING -DTEST_AWAIT -std=gnu11 -Ic_modules -Wall -Wextra -Wno-strict-aliasing -Werror -o "$tmp" "$0" $(find c_modules -name \*.c) -lpthread
exec "$tmp"
)
exit $?
#endif
#include "promise.h"
#include <cstruct/binary_tree_iterator.h>

/*
 * Underscore-prefixed functions must be called from within lock.
 * All others must be called from outside lock.
 */

static bool expired(const struct promise_deadline *now, const struct promise_deadline *deadline)
{
	return now->s > deadline->s ||
			(now->s == deadline->s && now->ns > deadline->ns);
}

struct cb_node
{
	promise promise;
	promise_callback *cb;
	void *closure;
	struct binary_tree_node **deadline;
};

static int promise_compare(const void *a, size_t al, const void *b, size_t bl, void *arg)
{
	(void) al;
	(void) bl;
	(void) arg;
	return ((const struct cb_node *) a)->promise - ((const struct cb_node *) b)->promise;
}

struct deadline_node
{
	struct promise_deadline deadline;
	promise promise;
};

static int deadline_compare(const void *a, size_t al, const void *b, size_t bl, void *arg)
{
	(void) arg;
	const size_t len = al < bl ? al : bl;
	return memcmp(a, b, len);
}

static void do_complete(const struct cb_node *acb, enum promise_resolution result, void *data)
{
	acb->cb(acb->closure, result, data);
}

static promise _put(struct promises *state, promise_callback *cb, void *closure, const struct promise_deadline *deadline)
{
	const promise id = state->next++;
	struct binary_tree_node **dl = NULL;
	if (deadline != NULL) {
		struct deadline_node dln = {
			.deadline = *deadline,
			.promise = id
		};
		bool isnew;
		dl = binary_tree_insert(&state->deadline, &dln, sizeof(dln), &isnew);
		if (!isnew) {
			return promise_fail;
		}
	}
	struct cb_node acb = {
		.promise = id,
		.cb = cb,
		.closure = closure,
		.deadline = dl
	};
	if (!binary_tree_insert_new(&state->cb, &acb, sizeof(acb))) {
		if (dl) {
			binary_tree_delete(&state->deadline, dl);
		}
		return promise_fail;
	}
	return id;
}

static const struct cb_node *_get(struct promises *state, promise promise)
{
	struct binary_tree_node **it = binary_tree_find(&state->cb, &promise, sizeof(promise));
	if (!it) {
		return NULL;
	}
	return (const struct cb_node *) (*it)->data;
}

static bool _take(struct promises *state, promise promise, struct cb_node *acb)
{
	struct binary_tree_node **it = binary_tree_find(&state->cb, &promise, sizeof(promise));
	if (!it) {
		return false;
	}
	*acb = *(const struct cb_node *) (*it)->data;
	binary_tree_delete(&state->cb, it);
	if (acb->deadline) {
		binary_tree_delete(&state->deadline, acb->deadline);
	}
	return true;
}

void promise_init(struct promises *state)
{
	pthread_mutex_init(&state->mx, NULL);
	binary_tree_init(&state->cb, promise_compare, NULL, NULL);
	binary_tree_init(&state->deadline, deadline_compare, NULL, NULL);
}

void promise_destroy(struct promises *state)
{
	promise_cancel_all(state, NULL);
	binary_tree_destroy(&state->cb);
	binary_tree_destroy(&state->deadline);
	pthread_mutex_destroy(&state->mx);
}

struct cb_node *_take_all(struct promises *state)
{
	const size_t count = binary_tree_size(&state->cb);
	struct cb_node * const buf = malloc(sizeof(*buf) * (count + 1));
	struct cb_node *oit = buf;
	/* Iterate over tree, building list */
	struct binary_tree_iterator it;
	binary_tree_iter_init(&it, &state->cb, true);
	struct cb_node *acb;
	while ((acb = binary_tree_iter_next(&it, NULL))) {
		*oit++ = *acb;
	}
	binary_tree_iter_destroy(&it);
	/* Add fail value to mark end of list */
	(oit++)->promise = promise_fail;
	/* Clear trees */
	binary_tree_clear(&state->cb);
	binary_tree_clear(&state->deadline);
	return buf;
}

struct cb_node *_take_expired(struct promises *state, const struct promise_deadline *now)
{
	struct binary_tree_iterator it;
	size_t count = 0;
	struct binary_tree_node **freshest;
	/* Count expired nodes and find oldest not-expired node */
	binary_tree_iter_init(&it, &state->deadline, true);
	while ((freshest = binary_tree_iter_next_node(&it))) {
		const struct deadline_node *dl = (const void *) (*freshest)->data;
		if (!expired(now, &dl->deadline)) {
			break;
		}
		count++;
	}
	binary_tree_iter_destroy(&it);
	struct cb_node * const buf = malloc(sizeof(*buf) * (count + 1));
	struct cb_node *oit = buf;
	/*
	 * Iterate over tree, building list of callbacks to return and list of
	 * nodes to delete from both trees
	 */
	struct binary_tree_node *** const del_begin = malloc(sizeof(*del_begin) * count * 2);
	struct binary_tree_node *** const del_end = del_begin + count * 2;
	struct binary_tree_node ***del_it = del_begin;
	binary_tree_iter_init(&it, &state->deadline, true);
	struct binary_tree_node **pos;
	while ((pos = binary_tree_iter_next_node(&it)) != freshest) {
		const promise id = ((const struct deadline_node *) (*pos)->data)->promise;
		struct binary_tree_node **cb = binary_tree_find(&state->cb, &id, sizeof(id));
		*oit++ = *(const struct cb_node *) (*cb)->data;
		*del_it++ = cb;
		*del_it++ = pos;
	}
	binary_tree_iter_destroy(&it);
	/* Add fail value to mark end of list */
	(oit++)->promise = promise_fail;
	/* Clear trees */
	del_it = del_begin;
	while (del_it != del_end) {
		binary_tree_delete(&state->cb, *del_it++);
		binary_tree_delete(&state->deadline, *del_it++);
	}
	free(del_it);
	return buf;
}


void promise_timeout(struct promises *state, const struct promise_deadline *now)
{
	/* Move tree to dynamic array */
	pthread_mutex_lock(&state->mx);
	struct cb_node *promises = _take_expired(state, now);
	pthread_mutex_unlock(&state->mx);
	/* Iterate over array, calling _complete for each in turn  */
	for (struct cb_node *it = promises; it->promise != promise_fail; ++it) {
		do_complete(it, awr_timeout, NULL);
	}
	free(promises);
}

static void promise_complete_all(struct promises *state, enum promise_resolution result, void *data)
{
	/* Move tree to dynamic array */
	pthread_mutex_lock(&state->mx);
	struct cb_node *promises = _take_all(state);
	pthread_mutex_unlock(&state->mx);
	/* Iterate over array, calling _complete for each in turn  */
	for (struct cb_node *it = promises; it->promise != promise_fail; ++it) {
		do_complete(it, result, data);
	}
	free(promises);
}

void promise_reject_all(struct promises *state, void *error)
{
	promise_complete_all(state, awr_fail, error);
}

void promise_cancel_all(struct promises *state, void *reason)
{
	promise_complete_all(state, awr_cancel, reason);
}

promise promise_open(struct promises *state, promise_callback *cb, void *closure, const struct promise_deadline *deadline)
{
	pthread_mutex_lock(&state->mx);
	promise res = _put(state, cb, closure, deadline);
	pthread_mutex_unlock(&state->mx);
	return res;
}

struct wait
{
	bool completed;
	pthread_mutex_t mx;
	pthread_cond_t cv;
	enum promise_resolution res;
	void *data;
};

static void wait_cb(void *closure, enum promise_resolution result, void *data)
{
	struct wait *w = closure;
	pthread_mutex_lock(&w->mx);
	w->res = result;
	w->data = data;
	w->completed = true;
	pthread_cond_signal(&w->cv);
	pthread_mutex_unlock(&w->mx);
}

promise promise_async(struct promises *state, const struct promise_deadline *deadline)
{
	struct wait *w = malloc(sizeof(*w));
	w->completed = false;
	pthread_mutex_init(&w->mx, NULL);
	pthread_cond_init(&w->cv, NULL);
	return promise_open(state, wait_cb, w, deadline);
}

enum promise_resolution promise_await(struct promises *state, promise promise, void **data)
{
	/* Get state */
	pthread_mutex_lock(&state->mx);
	struct wait * const w = (struct wait *) _get(state, promise)->closure;
	pthread_mutex_unlock(&state->mx);
	/* Wait for completion */
	pthread_mutex_lock(&w->mx);
	while (!w->completed) {
		pthread_cond_wait(&w->cv, &w->mx);
	}
	pthread_mutex_unlock(&w->mx);
	/* Store result */
	enum promise_resolution res = w->res;
	if (*data) {
		*data = w->data;
	}
	/* Clean up */
	pthread_cond_destroy(&w->cv);
	pthread_mutex_destroy(&w->mx);
	free(w);
	return res;
}

void promise_resolve(struct promises *state, promise promise, void *result)
{
	promise_complete(state, promise, awr_success, result);
}

void promise_reject(struct promises *state, promise promise, void *error)
{
	promise_complete(state, promise, awr_fail, error);
}

void promise_cancel(struct promises *state, promise promise, void *reason)
{
	promise_complete(state, promise, awr_cancel, reason);
}

bool promise_complete(struct promises *state, promise promise, enum promise_resolution result, void *data)
{
	struct cb_node acb;
	pthread_mutex_lock(&state->mx);
	bool res = _take(state, promise, &acb);
	pthread_mutex_unlock(&state->mx);
	if (res) {
		do_complete(&acb, result, data);
	}
	return res;
}

#if defined TEST_AWAIT
#include <ctype.h>

static struct promises aw;

struct req
{
	enum promise_resolution desired;
	const char *in;
	promise id;
};

static void *producer(void *arg)
{
	struct req *req = arg;
	char *data = NULL;
	if (req->in) {
		size_t size = strlen(req->in) + 1;
		data = malloc(size);
		for (size_t i = 0; i < size; ++i) {
			data[i] = toupper(req->in[i]);
		}
	}
	for (int i = 0; i < 10; i++) {
		fprintf(stderr, ".");
		usleep(20000);
	}
	if (!promise_complete(&aw, req->id, req->desired, data)) {
		log_error("promise_complete failed");
	}
	return NULL;
}

static enum promise_resolution request(enum promise_resolution desired, const char *in, void **out)
{
	struct req req = {
		.desired = desired,
		.in = in,
		.id = promise_async(&aw, NULL)
	};
	if (req.id == promise_fail) {
		log_error("promise_async failed");
	}
	pthread_t tid;
	pthread_create(&tid, NULL, producer, &req);
	return promise_await(&aw, req.id, out);
}

#define txtin "Mashed potato and PIEs"
#define txtout "MASHED POTATO AND PIES"

static bool test_req(enum promise_resolution expect)
{
	bool fail = false;
	char *out;
	enum promise_resolution actual = request(expect, txtin, (void **) &out);
	if (actual != expect) {
		log_error("Incorrect resolution");
		fail = true;
	}
	for (size_t i = 0; i < strlen(txtin); ++i) {
		if (toupper(txtin[i]) != out[i]) {
			log_error("Incorrect data");
			fail = true;
		}
	}
	free(out);
	fprintf(stderr, "%s\n", fail ? "FAIL" : "PASS");
	return fail;
}

int main(int argc, char *argv[])
{
	(void) argc;
	(void) argv;
	int fail = 0;
	promise_init(&aw);
	fail += test_req(awr_success);
	fail += test_req(awr_fail);
	fail += test_req(awr_cancel);
	fail += test_req(awr_timeout);
	promise_destroy(&aw);
	return fail;
}
#endif
