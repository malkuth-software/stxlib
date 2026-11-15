/*
 * stxshim_terminate.h -- the shim's terminate sequence and target rule,
 * free of kernel calls so that the same code runs in the kernel (stxshim.c)
 * and in the host test (test/test_stxshim_terminate.c), which checks it
 * exhaustively over a bounded model.
 *
 * The includer provides uint32_t and uint64_t (the kernel through
 * <sys/param.h>, the host test through <stdint.h>) before including this
 * header after stxshim.h.
 *
 * The sequence (owner, question 3, option 1; grace 1 s as proposed):
 *   1. one SIGTERM pass over the targets;
 *   2. polls of STXSHIM_POLL_MS while any target is left, at most
 *      STXSHIM_GRACE_POLLS (1 s);
 *   3. while any target is left, a SIGKILL pass and, if any is still
 *      left, one poll, at most STXSHIM_KILL_ROUNDS_MAX rounds (1 s);
 *      repeating the pass also catches processes a target forked after an
 *      earlier pass;
 *   4. one sync, also when targets remain (a process the kernel cannot
 *      kill, for instance one waiting on a device, must not block it).
 * No poll is taken when the count just before it found no target. Every
 * loop is bounded by a constant; the whole sequence makes at most 1 + 100
 * signal passes, 100 + 1 + 2 x 100 + 1 counts and 100 + 100 polls.
 */
#ifndef STXSHIM_TERMINATE_H
#define STXSHIM_TERMINATE_H

#define STXSHIM_POLL_MS		10u
#define STXSHIM_GRACE_POLLS	100u
#define STXSHIM_KILL_ROUNDS_MAX	100u

/*
 * Whether a process is a target of the terminate. pid is the process's id;
 * is_system: a kernel process (P_SYSTEM); is_new: still being created
 * (PRS_NEW, not yet signallable, as kill(2) treats it); is_gone: exiting or
 * a zombie (P_WEXIT or PRS_ZOMBIE); supervisor_pid: the registered
 * supervisor, or 0 when none is registered.
 */
static inline int
stxshim_is_target(int pid, int is_system, int is_new, int is_gone, int supervisor_pid)
{
	if (pid <= 1)
		return (0);
	if (is_system != 0 || is_new != 0 || is_gone != 0)
		return (0);
	if (supervisor_pid > 1 && pid == supervisor_pid)
		return (0);
	return (1);
}

/* What the sequence needs from its surroundings. */
struct stxshim_terminate_ops {
	void *context;
	/* Signals every target (hard: SIGKILL, else SIGTERM); returns how many. */
	uint32_t (*signal_targets)(void *context, int hard);
	/* Counts the targets left. */
	uint32_t (*count_targets)(void *context);
	/* Sleeps STXSHIM_POLL_MS. */
	void (*sleep_poll)(void *context);
	/* A monotonic clock in milliseconds. */
	uint64_t (*now_ms)(void *context);
	/* Syncs every writable file system once. */
	void (*sync_filesystems)(void *context);
};

static inline void
stxshim_terminate_sequence(const struct stxshim_terminate_ops *ops,
    struct stxshim_terminate_result *result)
{
	uint64_t started_ms;
	uint64_t ended_ms;
	uint64_t elapsed_ms;
	uint32_t poll;
	uint32_t kill_round;
	uint32_t sent;

	started_ms = (*ops->now_ms)(ops->context);
	result->term_signalled = (*ops->signal_targets)(ops->context, 0);
	result->kill_signalled = 0;
	result->kill_rounds = 0;
	result->remaining = 0;
	result->elapsed_ms = 0;

	for (poll = 0; poll < STXSHIM_GRACE_POLLS; poll++) {
		if ((*ops->count_targets)(ops->context) == 0)
			break;
		(*ops->sleep_poll)(ops->context);
	}
	for (kill_round = 0; kill_round < STXSHIM_KILL_ROUNDS_MAX; kill_round++) {
		if ((*ops->count_targets)(ops->context) == 0)
			break;
		sent = (*ops->signal_targets)(ops->context, 1);
		if (kill_round == 0)
			result->kill_signalled = sent;
		result->kill_rounds = kill_round + 1u;
		if ((*ops->count_targets)(ops->context) == 0)
			break;
		(*ops->sleep_poll)(ops->context);
	}
	result->remaining = (*ops->count_targets)(ops->context);
	(*ops->sync_filesystems)(ops->context);

	ended_ms = (*ops->now_ms)(ops->context);
	elapsed_ms = ended_ms >= started_ms ? ended_ms - started_ms : 0u;
	result->elapsed_ms = elapsed_ms > 0xffffffffu ? 0xffffffffu : (uint32_t)elapsed_ms;
}

#endif /* STXSHIM_TERMINATE_H */
