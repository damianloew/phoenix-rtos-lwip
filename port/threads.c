/*
 * Phoenix-RTOS --- LwIP port
 *
 * LwIP OS mode layer - thread support
 *
 * Copyright 2018 Phoenix Systems
 * Author: Michał Mirosław
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "arch/cc.h"
#include "arch/sys_arch.h"

#include <sys/rb.h>
#include <sys/wait.h>
#include <sys/threads.h>
#include <stdlib.h>
#include <errno.h>


typedef struct {
	rbnode_t linkage;
	handle_t tid;
	void *stack;
	void (*work)(void *arg);
	void *arg;

	struct __errno_t err;
} thread_data_t;


static struct {
	char collector_stack[512] __attribute__((aligned(8)));
	rbtree_t threads;
	handle_t lock;
	handle_t join_lock;
	handle_t join_cond;
} global;


static int thread_cmp(rbnode_t *n1, rbnode_t *n2)
{
	thread_data_t *s1 = lib_treeof(thread_data_t, linkage, n1);
	thread_data_t *s2 = lib_treeof(thread_data_t, linkage, n2);

	if (s1->tid == s2->tid)
		return 0;

	else if (s1->tid > s2->tid)
		return 1;

	else
		return -1;
}


static void thread_register(thread_data_t *ts)
{
	thread_data_t *old, s;

	s.tid = ts->tid;

	mutexLock(global.lock);
	if ((old = lib_treeof(thread_data_t, linkage, lib_rbFind(&global.threads, &s.linkage))) != NULL) {
		_errno_remove(&old->err);
		lib_rbRemove(&global.threads, &old->linkage);
	}

	lib_rbInsert(&global.threads, &ts->linkage);
	mutexUnlock(global.lock);
	_errno_new(&ts->err);

	if (old != NULL) {
		free(old->stack);
		free(old);
	}
}


static void thread_waittid_thr(void *arg)
{
	thread_data_t *data, s;
	priority(3);

	for (;;) {
		while ((s.tid = threadJoin(0)) == -EINTR)
			;

		mutexLock(global.lock);
		data = lib_treeof(thread_data_t, linkage, lib_rbFind(&global.threads, &s.linkage));
		if (data != NULL) {
			lib_rbRemove(&global.threads, &data->linkage);
			mutexUnlock(global.lock);

			_errno_remove(&data->err);
			free(data->stack);
			free(data);

			mutexLock(global.join_lock);
			condSignal(global.join_cond);
			mutexUnlock(global.join_lock);
		}
		else {
			mutexUnlock(global.lock);
		}
	}
}


static void thread_main(void *arg)
{
	thread_data_t *t = arg;
	thread_register(t);
	t->work(t->arg);
	endthread();
}


int sys_thread_opt_new(const char *name, void (* thread)(void *arg), void *arg, int stacksize, int prio, handle_t *id)
{
	void *stack;
	int err;
	thread_data_t *ts;

	stack = malloc(stacksize);
	if (!stack)
		bail("no memory for thread: %s\n", name);

	ts = malloc(sizeof(*ts));
	if (!ts) {
		free(stack);
		bail("no memory for thread: %s\n", name);
	}

	ts->work = thread;
	ts->stack = stack;
	ts->arg = arg;

	mutexLock(global.lock);
	err = beginthreadex(thread_main, prio, stack, stacksize, ts, &ts->tid);

	if (err) {
		free(stack);
		free(ts);
	}
	else if (id != NULL) {
		*id = ts->tid;
	}

	mutexUnlock(global.lock);

	return err;
}


sys_thread_t sys_thread_new(const char *name, void (* thread)(void *arg), void *arg, int stacksize, int prio)
{
	handle_t id;
	int err;

	err = sys_thread_opt_new(name, thread, arg, stacksize, prio, &id);
	if (err)
		errout(err, "beginthread(%s)", name);

	return id;
}


int sys_thread_join(handle_t id)
{
	thread_data_t *data, s;

	if (id == gettid())
		return -1;

	s.tid = id;

	for (;;) {
		mutexLock(global.lock);
		data = lib_treeof(thread_data_t, linkage, lib_rbFind(&global.threads, &s.linkage));
		mutexUnlock(global.lock);

		if (data == NULL) {
			return 0;
		}

		mutexLock(global.join_lock);
		condWait(global.join_cond, global.join_lock, 0);
		mutexUnlock(global.join_lock);
	}
}


void init_lwip_threads(void)
{
	int err;

	err = mutexCreate(&global.lock);
	if (err)
		errout(err, "mutexCreate(lock)");

	err = mutexCreate(&global.join_lock);
	if (err) {
		resourceDestroy(global.lock);
		errout(err, "mutexCreate(join_lock)");
	}

	err = condCreate(&global.join_cond);
	if (err) {
		resourceDestroy(global.lock);
		resourceDestroy(global.join_lock);
		errout(err, "condCreate(join_cond)");
	}

	lib_rbInit(&global.threads, thread_cmp, NULL);
	beginthreadex(thread_waittid_thr, 4, global.collector_stack, sizeof(global.collector_stack), NULL, NULL);
}
