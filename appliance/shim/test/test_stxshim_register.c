/*
 * test_stxshim_register.c -- host test of who may register as the
 * supervisor (appliance/shim/stxshim_register.h), the same code the kernel
 * runs.
 *
 * 1. The rule: exhaustive over parent is PID 1 or not, file opened by PID 1
 *    or not, file registered before or not, caller pid 1..3, the pid that
 *    registered on the file 0..3 and the registered supervisor 0..3: 384
 *    cases against a decision table written as the C2 design states it.
 * 2. The window: a model of PID 1, its supervisor children, an orphan it
 *    adopted, and a child the supervisor forked. Exhaustive over every
 *    sequence of 0 to 8 steps from an alphabet of 7: PID 1 starts a
 *    supervisor when none is alive (opens a file and hands it to the child
 *    alone, as stxinit does); the newest supervisor registers; it registers
 *    again; it dies (the shim's death watch clears the registration); the
 *    orphan registers on a file it opened itself; the orphan registers on
 *    the newest supervisor's file once that file has been used; the
 *    supervisor's forked child, holding the inherited file, registers (its
 *    parent is PID 1 once the supervisor died and it was adopted).
 *    7^0 + ... + 7^8 = 6,725,601 sequences. Checked at every step: the
 *    supervisor PID 1 started always registers; the orphan and the forked
 *    child never do; a registered supervisor stays registered until it
 *    dies; no file ever registered the orphan.
 *    What the model assumes, and the kernel and stxinit provide: a file
 *    PID 1 opened for a supervisor reaches only that child (stxinit
 *    dup2s it into the child and closes its own copy; the supervisor marks
 *    it close-on-exec), and the supervisor registers before it forks
 *    anything, so a file any other process can hold has been used.
 * Build: run_host_tests.sh.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../stxshim.h"
#include "../stxshim_register.h"

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

/* --- 1. the rule ------------------------------------------------------- */

static int
table(int parent_is_init, int opened_by_init, int before, uint32_t caller, uint32_t file_pid, uint32_t supervisor)
{
	/* Only a child of PID 1, only on a file PID 1 opened. */
	if (!(parent_is_init && opened_by_init))
		return (1);
	/* A file used once: only its own registrant may repeat, while registered. */
	if (before)
		return (file_pid == caller && supervisor == caller) ? 0 : 1;
	/* A fresh file: refused while another supervisor is registered. */
	if (supervisor == 0u || supervisor == caller)
		return (0);
	return (16);
}

static unsigned long
test_rule(void)
{
	unsigned long run;
	int p, o, b;
	uint32_t c, f, s;

	run = 0;
	for (p = 0; p < 2; p++)
		for (o = 0; o < 2; o++)
			for (b = 0; b < 2; b++)
				for (c = 1; c <= 3; c++)
					for (f = 0; f <= 3; f++)
						for (s = 0; s <= 3; s++) {
							check(stxshim_may_register(p, o, b, c, f, s) == table(p, o, b, c, f, s),
							    "rule equals the table", run);
							run++;
						}
	return (run);
}

/* --- 2. the window ----------------------------------------------------- */

#define FILES_MAX	16
#define PIDS_MAX	16
#define ORPHAN_PID	2u
#define LENGTH_MAX	8
#define ALPHABET	7

struct file {
	int opened_by_init;
	int registered_before;
	uint32_t registered_pid;
};

struct world {
	struct file files[FILES_MAX];
	int file_count;
	uint32_t supervisor;		/* the shim's registered pid, 0: none */
	uint32_t newest_pid;		/* the newest supervisor child, 0: none */
	int newest_file;
	int newest_alive;
	uint32_t next_pid;
	int orphan_file;		/* a file the orphan opened itself */
};

static int
attempt(struct world *w, uint32_t pid, int parent_is_init, int file_index)
{
	struct file *f = &w->files[file_index];
	int error;

	error = stxshim_may_register(parent_is_init, f->opened_by_init, f->registered_before, pid,
	    f->registered_pid, w->supervisor);
	if (error == 0) {
		w->supervisor = pid;
		f->registered_before = 1;
		f->registered_pid = pid;
	}
	return (error);
}

static void
step(struct world *w, int s, unsigned long run)
{
	uint32_t supervisor_before;
	int error;

	supervisor_before = w->supervisor;
	switch (s) {
	case 0: /* PID 1 starts a supervisor (only when none is alive, as stxinit does). */
		if (w->newest_alive || w->file_count >= FILES_MAX - 1)
			break;
		w->files[w->file_count].opened_by_init = 1;
		w->newest_file = w->file_count++;
		w->newest_pid = w->next_pid++;
		w->newest_alive = 1;
		break;
	case 1: /* the newest registers */
	case 2: /* and again */
		if (!w->newest_alive)
			break;
		error = attempt(w, w->newest_pid, 1, w->newest_file);
		check(error == 0, "the supervisor PID 1 started registers", run);
		break;
	case 3: /* the newest dies (the shim's death watch clears it) */
		if (!w->newest_alive)
			break;
		w->newest_alive = 0;
		if (w->supervisor == w->newest_pid)
			w->supervisor = 0;
		break;
	case 4: /* the orphan on its own file */
		error = attempt(w, ORPHAN_PID, 1, w->orphan_file);
		check(error != 0, "the orphan is refused on its own file", run);
		break;
	case 5: /* the orphan on a file PID 1 opened for a supervisor that has used it */
		if (w->newest_pid == 0u || !w->files[w->newest_file].registered_before)
			break;
		error = attempt(w, ORPHAN_PID, 1, w->newest_file);
		check(error != 0, "the orphan is refused on a used file", run);
		break;
	case 6: /* the supervisor's forked child, on the inherited file */
		if (w->newest_pid == 0u || !w->files[w->newest_file].registered_before)
			break;
		error = attempt(w, w->newest_pid + 1000u, w->newest_alive ? 0 : 1, w->newest_file);
		check(error != 0, "the supervisor's child is refused", run);
		break;
	default:
		check(0, "step in range", run);
		break;
	}
	check(w->supervisor != ORPHAN_PID, "the orphan never registers", run);
	check(w->supervisor == 0u || w->supervisor == w->newest_pid, "only the newest supervisor registers", run);
	if (supervisor_before != 0u && supervisor_before != w->supervisor)
		check(s == 3, "a registered supervisor stays until it dies", run);
}

static unsigned long
test_window(void)
{
	static struct world w;
	int steps[LENGTH_MAX];
	int length, i, j;
	unsigned long run, total, index, rest;

	run = 0;
	for (length = 0; length <= LENGTH_MAX; length++) {
		total = 1;
		for (i = 0; i < length; i++)
			total *= ALPHABET;
		for (index = 0; index < total; index++) {
			rest = index;
			for (i = 0; i < length; i++) {
				steps[i] = (int)(rest % ALPHABET);
				rest /= ALPHABET;
			}
			memset(&w, 0, sizeof(w));
			w.next_pid = 10;
			w.orphan_file = 0;
			w.files[0].opened_by_init = 0;
			w.file_count = 1;
			for (i = 0; i < length; i++)
				step(&w, steps[i], run);
			for (j = 0; j < w.file_count; j++)
				check(w.files[j].registered_before == 0 || w.files[j].registered_pid != ORPHAN_PID,
				    "no file registered the orphan", run);
			run++;
		}
	}
	return (run);
}

int
main(void)
{
	unsigned long rule, window;

	rule = test_rule();
	window = test_window();
	printf("exhaustive register rule: %lu cases\n", rule);
	printf("exhaustive registration window: %lu sequences (0-8 steps of 7)\n", window);
	printf("test_stxshim_register: %lu checks, %lu failed\n", checks, failures);
	return (failures == 0 && rule == 384ul && window == 6725601ul ? 0 : 1);
}
