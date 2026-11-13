/*
 * test_stxshim_terminate.c -- host test of the shim's target rule and
 * terminate sequence (appliance/shim/stxshim_terminate.h), the same code the
 * kernel runs, against a model of the processes.
 *
 * Target rule: exhaustive over pid in {-1, 0, 1, 2, 3, 99999}, the three
 * flags, and supervisor_pid in {0, 1, 2, 3}.
 *
 * Sequence: exhaustive over every set of 0 to 3 target processes, each with
 * one of 6 SIGTERM behaviours (exits at the signal, after 1, 99, 100 or 101
 * polls, or ignores SIGTERM) and one of 3 SIGKILL behaviours (exits at the
 * signal, after 1 poll, or never: a process the kernel cannot kill), and,
 * for each set, whether a process forks one new target the first time it
 * receives SIGTERM (a forked target ignores SIGTERM and dies at SIGKILL):
 * (1 + 18 + 18^2 + 18^3) x 2 = 12,350 runs. Checked for each run:
 *   - one SIGTERM pass, first, and its count is the starting targets;
 *   - no SIGKILL pass while the grace runs: the first comes after 100 polls,
 *     or when no target was left earlier there is none;
 *   - no poll is taken once no target is left (a second run of each set);
 *   - at most 100 SIGKILL rounds and 200 polls; elapsed = 10 ms x polls;
 *   - exactly one sync, after every signal, and nothing but the clock after;
 *   - remaining is the model's count of running targets at the end, and is
 *     0 unless some process is unkillable;
 *   - kill_signalled is the targets running at the first SIGKILL pass.
 * Build: cc -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Werror
 *        -fsanitize=address,undefined (run_host_tests.sh).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../stxshim.h"
#include "../stxshim_terminate.h"

static unsigned long checks;
static unsigned long failures;

static void
check(int condition, const char *what, unsigned long run)
{
	checks++;
	if (condition)
		return;
	failures++;
	if (failures <= 20)
		printf("  FAIL %s (run %lu)\n", what, run);
}

/* --- the target rule ------------------------------------------------- */

static void
test_target_rule(void)
{
	static const int pids[] = {-1, 0, 1, 2, 3, 99999};
	static const int supervisors[] = {0, 1, 2, 3};
	size_t i, j;
	int system, created, gone;
	unsigned long run = 0;

	for (i = 0; i < sizeof(pids) / sizeof(pids[0]); i++)
		for (j = 0; j < sizeof(supervisors) / sizeof(supervisors[0]); j++)
			for (system = 0; system < 2; system++)
				for (created = 0; created < 2; created++)
					for (gone = 0; gone < 2; gone++) {
						int pid = pids[i];
						int supervisor = supervisors[j];
						int expected = pid > 1 && system == 0 &&
						    created == 0 && gone == 0 &&
						    !(supervisor > 1 && pid == supervisor);
						int got = stxshim_is_target(pid, system,
						    created, gone, supervisor);
						check(got == expected, "target rule", run);
						run++;
					}
}

/* --- the process model ----------------------------------------------- */

#define TERM_IGNORES	(-1)
#define KILL_NEVER	(-1)
#define PROCESSES_MAX	6

static const int term_behaviours[] = {0, 1, 99, 100, 101, TERM_IGNORES};
static const int kill_behaviours[] = {0, 1, KILL_NEVER};

struct process {
	int running;
	int term_after;		/* polls from SIGTERM to exit, or TERM_IGNORES */
	int kill_after;		/* polls from SIGKILL to exit, or KILL_NEVER */
	int term_left;		/* -1: no SIGTERM pending an exit */
	int kill_left;		/* -1: no SIGKILL pending an exit */
	int forks_on_term;	/* forks one target at its first SIGTERM */
	int got_term;
};

enum event { EV_TERM, EV_KILL, EV_COUNT, EV_POLL, EV_NOW, EV_SYNC };
#define EVENTS_MAX	1024

struct model {
	struct process processes[PROCESSES_MAX];
	int count;
	uint64_t clock_ms;
	enum event events[EVENTS_MAX];
	int event_count;
	int overflow;
	uint32_t running_at_first_kill;
	int seen_kill;
};

static void
record(struct model *m, enum event e)
{
	if (m->event_count < EVENTS_MAX)
		m->events[m->event_count++] = e;
	else
		m->overflow = 1;
}

static uint32_t
running_count(const struct model *m)
{
	uint32_t n = 0;
	int i;

	for (i = 0; i < m->count; i++)
		if (m->processes[i].running)
			n++;
	return (n);
}

static void
add_process(struct model *m, int term_after, int kill_after, int forks)
{
	struct process *p;

	if (m->count >= PROCESSES_MAX)
		return;
	p = &m->processes[m->count++];
	memset(p, 0, sizeof(*p));
	p->running = 1;
	p->term_after = term_after;
	p->kill_after = kill_after;
	p->term_left = -1;
	p->kill_left = -1;
	p->forks_on_term = forks;
}

static uint32_t
model_signal(void *context, int hard)
{
	struct model *m = context;
	uint32_t sent = 0;
	int i;
	int count_before = m->count;

	record(m, hard ? EV_KILL : EV_TERM);
	if (hard && !m->seen_kill) {
		m->seen_kill = 1;
		m->running_at_first_kill = running_count(m);
	}
	for (i = 0; i < count_before; i++) {
		struct process *p = &m->processes[i];
		if (!p->running)
			continue;
		sent++;
		if (hard) {
			if (p->kill_after == 0)
				p->running = 0;
			else if (p->kill_after > 0 && p->kill_left < 0)
				p->kill_left = p->kill_after;
		} else {
			if (p->forks_on_term && !p->got_term)
				add_process(m, TERM_IGNORES, 0, 0);
			p->got_term = 1;
			if (p->term_after == 0)
				p->running = 0;
			else if (p->term_after > 0 && p->term_left < 0)
				p->term_left = p->term_after;
		}
	}
	return (sent);
}

static uint32_t
model_count(void *context)
{
	struct model *m = context;

	record(m, EV_COUNT);
	return (running_count(m));
}

static void
model_poll(void *context)
{
	struct model *m = context;
	int i;

	record(m, EV_POLL);
	m->clock_ms += STXSHIM_POLL_MS;
	for (i = 0; i < m->count; i++) {
		struct process *p = &m->processes[i];
		if (!p->running)
			continue;
		if (p->term_left > 0 && --p->term_left == 0)
			p->running = 0;
		if (p->running && p->kill_left > 0 && --p->kill_left == 0)
			p->running = 0;
	}
}

static uint64_t
model_now(void *context)
{
	struct model *m = context;

	record(m, EV_NOW);
	return (m->clock_ms);
}

static void
model_sync(void *context)
{
	record((struct model *)context, EV_SYNC);
}

/* --- the sequence ---------------------------------------------------- */

static void
check_run(struct model *m, uint32_t starting, int any_unkillable, unsigned long run)
{
	struct stxshim_terminate_ops ops;
	struct stxshim_terminate_result result;
	int i, terms = 0, kills = 0, polls = 0, syncs = 0;
	int first_kill = -1, sync_at = -1, last_signal = -1;
	int polls_before_first_kill = 0;

	ops.context = m;
	ops.signal_targets = model_signal;
	ops.count_targets = model_count;
	ops.sleep_poll = model_poll;
	ops.now_ms = model_now;
	ops.sync_filesystems = model_sync;
	memset(&result, 0xa5, sizeof(result));
	stxshim_terminate_sequence(&ops, &result);

	check(!m->overflow, "event log bounded", run);
	for (i = 0; i < m->event_count; i++) {
		switch (m->events[i]) {
		case EV_TERM:
			terms++;
			last_signal = i;
			break;
		case EV_KILL:
			kills++;
			last_signal = i;
			if (first_kill < 0) {
				first_kill = i;
				polls_before_first_kill = polls;
			}
			break;
		case EV_POLL:
			polls++;
			break;
		case EV_SYNC:
			syncs++;
			sync_at = i;
			break;
		case EV_COUNT:
		case EV_NOW:
			break;
		default:
			check(0, "known event", run);
			break;
		}
	}
	check(m->event_count >= 2 && m->events[0] == EV_NOW && m->events[1] == EV_TERM,
	    "clock then one SIGTERM pass first", run);
	check(terms == 1, "exactly one SIGTERM pass", run);
	check(result.term_signalled == starting, "SIGTERM count = starting targets", run);
	check(syncs == 1, "exactly one sync", run);
	check(sync_at > last_signal, "sync after every signal", run);
	for (i = sync_at + 1; i < m->event_count; i++)
		check(m->events[i] == EV_NOW, "only the clock after the sync", run);
	if (first_kill >= 0) {
		check(polls_before_first_kill == (int)STXSHIM_GRACE_POLLS,
		    "first SIGKILL only after the full grace", run);
		check(result.kill_signalled == m->running_at_first_kill,
		    "kill_signalled = running at the first SIGKILL", run);
		check(m->running_at_first_kill > 0, "SIGKILL only with targets left", run);
	} else {
		check(result.kill_signalled == 0 && result.kill_rounds == 0,
		    "no SIGKILL, no rounds", run);
	}
	check(kills == (int)result.kill_rounds, "rounds = SIGKILL passes", run);
	check(result.kill_rounds <= STXSHIM_KILL_ROUNDS_MAX, "rounds bounded", run);
	check(polls <= (int)(STXSHIM_GRACE_POLLS + STXSHIM_KILL_ROUNDS_MAX), "polls bounded", run);
	check(result.elapsed_ms == (uint32_t)polls * STXSHIM_POLL_MS, "elapsed = polls x 10 ms", run);
	check(result.remaining == running_count(m), "remaining = model's running", run);
	if (!any_unkillable)
		check(result.remaining == 0, "none remains unless unkillable", run);
}

/* A second run of the same set checks that no poll is taken once no
 * target is left: a poll is late when the model has none running. */
static int late_polls;

static void
late_poll(void *context)
{
	if (running_count((struct model *)context) == 0)
		late_polls++;
	model_poll(context);
}

static void
check_no_late_poll(struct model *m, unsigned long run)
{
	struct stxshim_terminate_ops ops;
	struct stxshim_terminate_result result;

	late_polls = 0;
	ops.context = m;
	ops.signal_targets = model_signal;
	ops.count_targets = model_count;
	ops.sleep_poll = late_poll;
	ops.now_ms = model_now;
	ops.sync_filesystems = model_sync;
	stxshim_terminate_sequence(&ops, &result);
	check(late_polls == 0, "no poll once no target is left", run);
}

static void
setup(struct model *m, int n, const int *term_index, const int *kill_index, int forks,
    int *any_unkillable)
{
	int i;

	memset(m, 0, sizeof(*m));
	*any_unkillable = 0;
	for (i = 0; i < n; i++) {
		int k = kill_behaviours[kill_index[i]];
		add_process(m, term_behaviours[term_index[i]], k, forks && i == 0);
		if (k == KILL_NEVER)
			*any_unkillable = 1;
	}
}

static unsigned long
test_sequence(void)
{
	const int term_n = (int)(sizeof(term_behaviours) / sizeof(term_behaviours[0]));
	const int kill_n = (int)(sizeof(kill_behaviours) / sizeof(kill_behaviours[0]));
	const int per_process = term_n * kill_n;
	unsigned long runs = 0;
	int n, code, codes, forks, i;

	for (n = 0; n <= 3; n++) {
		codes = 1;
		for (i = 0; i < n; i++)
			codes *= per_process;
		for (code = 0; code < codes; code++)
			for (forks = 0; forks < 2; forks++) {
				int term_index[3], kill_index[3];
				int rest = code, any_unkillable;
				struct model m;

				for (i = 0; i < n; i++) {
					int c = rest % per_process;
					rest /= per_process;
					term_index[i] = c % term_n;
					kill_index[i] = c / term_n;
				}
				setup(&m, n, term_index, kill_index, forks, &any_unkillable);
				check_run(&m, (uint32_t)n, any_unkillable, runs);
				setup(&m, n, term_index, kill_index, forks, &any_unkillable);
				check_no_late_poll(&m, runs);
				runs++;
			}
	}
	return (runs);
}

int
main(void)
{
	unsigned long runs;

	test_target_rule();
	runs = test_sequence();
	printf("exhaustive terminate: %lu runs (0-3 processes x 18 behaviours, forking or not)\n", runs);
	printf("test_stxshim_terminate: %lu checks, %lu failed\n", checks, failures);
	return (failures == 0 ? 0 : 1);
}
