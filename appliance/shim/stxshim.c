/*
 * stxshim.c -- the STXlib kernel shim, first form (C1, step S5).
 *
 * Compiled into the appliance kernel, not loaded as a module: the kernel
 * configuration names it with config(8)'s "files" directive
 * (appliance/kernel/build_kernel.sh writes the files list; no FreeBSD source
 * is patched). The interface, the target rule and the terminate sequence are
 * in stxshim.h and stxshim_terminate.h; this file adds only the kernel's
 * side: the device, the registration, the death watch, the process passes,
 * the sleep, the clock and the sync.
 *
 * In this form the shim
 *   - registers the supervisor and protects it from the out-of-memory
 *     killer (P_PROTECTED, sys/sys/proc.h:847, the flag that
 *     procctl(PROC_SPROTECT) sets, sys/kern/kern_procctl.c:56-71);
 *   - watches it: the process_exit event (sys/kern/kern_exit.c:410) ends the
 *     registration and prints "stxshim: supervisor exited pid=<n>"; PID 1
 *     respawns the supervisor, which registers again;
 *   - forcibly terminates userspace on the supervisor's request (question 3,
 *     option 1), printing "stxshim: terminate term=<n> kill=<n> rounds=<n>
 *     remaining=<n> ms=<n>".
 * The reserved chord (Ctrl+Alt+F4), the display and input ownership, the
 * reserved memory, and killing a supervisor that does not answer after the
 * chord come with the framebuffer and input host (C2): each needs the
 * keyboard and framebuffer paths, which the C1 VM's serial console lacks.
 *
 * The process passes follow kill(2)'s broadcast (sys/kern/kern_sig.c:1795-
 * 1828): allproc shared-locked, each process locked while it is examined and
 * signalled; kernel processes (P_SYSTEM) and PID 1 are never targets, nor
 * processes still being created (PRS_NEW). The sync is sync(2)'s
 * (kern_sync, sys/kern/vfs_syscalls.c:134): it starts the writes of every
 * writable file system and does not wait for them, as sync(2) does not.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/eventhandler.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/signalvar.h>
#include <sys/sx.h>
#include <sys/syscallsubr.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include "stxshim.h"
#include "stxshim_terminate.h"

/*
 * State. stxshim_mtx protects all of it; stxshim_supervisor_pid is also
 * read without the lock, atomically, as the exit handler's fast path.
 */
static struct mtx stxshim_mtx;
static u_int stxshim_supervisor_pid;	/* 0: none registered */
static u_int stxshim_registrations;
static u_int stxshim_supervisor_exits;
static u_int stxshim_terminations;
static int stxshim_terminating;
static struct cdev *stxshim_dev;
static eventhandler_tag stxshim_exit_tag;

static SYSCTL_NODE(_kern, OID_AUTO, stxshim, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "STXlib kernel shim");
SYSCTL_UINT(_kern_stxshim, OID_AUTO, supervisor_pid, CTLFLAG_RD,
    &stxshim_supervisor_pid, 0, "Registered supervisor (0: none)");
SYSCTL_UINT(_kern_stxshim, OID_AUTO, registrations, CTLFLAG_RD,
    &stxshim_registrations, 0, "Supervisor registrations");
SYSCTL_UINT(_kern_stxshim, OID_AUTO, supervisor_exits, CTLFLAG_RD,
    &stxshim_supervisor_exits, 0, "Exits of the registered supervisor");
SYSCTL_UINT(_kern_stxshim, OID_AUTO, terminations, CTLFLAG_RD,
    &stxshim_terminations, 0, "Completed terminates of userspace");

/* The death watch. */
static void
stxshim_process_exit(void *arg __unused, struct proc *p)
{
	u_int pid;
	int exited;

	pid = (u_int)p->p_pid;
	if (atomic_load_int(&stxshim_supervisor_pid) != pid)
		return;
	exited = 0;
	mtx_lock(&stxshim_mtx);
	if (stxshim_supervisor_pid == pid) {
		atomic_store_int(&stxshim_supervisor_pid, 0);
		stxshim_supervisor_exits++;
		exited = 1;
	}
	mtx_unlock(&stxshim_mtx);
	if (exited != 0)
		printf("stxshim: supervisor exited pid=%u\n", pid);
}

static int
stxshim_register(struct thread *td)
{
	struct proc *p;
	u_int current;
	int parent_is_init;

	p = td->td_proc;
	sx_slock(&proctree_lock);
	parent_is_init = p->p_pptr == initproc;
	sx_sunlock(&proctree_lock);
	if (parent_is_init == 0)
		return (EPERM);

	mtx_lock(&stxshim_mtx);
	current = stxshim_supervisor_pid;
	if (current != 0 && current != (u_int)p->p_pid) {
		mtx_unlock(&stxshim_mtx);
		return (EBUSY);
	}
	if (current == 0) {
		atomic_store_int(&stxshim_supervisor_pid, (u_int)p->p_pid);
		stxshim_registrations++;
	}
	mtx_unlock(&stxshim_mtx);

	PROC_LOCK(p);
	p->p_flag |= P_PROTECTED;
	PROC_UNLOCK(p);
	return (0);
}

/* The terminate's surroundings in the kernel. */
struct stxshim_context {
	int supervisor_pid;
};

static int
stxshim_proc_is_target(struct proc *p, int supervisor_pid)
{
	PROC_LOCK_ASSERT(p, MA_OWNED);
	return (stxshim_is_target(p->p_pid, (p->p_flag & P_SYSTEM) != 0,
	    p->p_state == PRS_NEW,
	    (p->p_flag & P_WEXIT) != 0 || p->p_state == PRS_ZOMBIE,
	    supervisor_pid));
}

static uint32_t
stxshim_pass(const struct stxshim_context *context, int signal_number)
{
	struct proc *p;
	uint32_t count;

	count = 0;
	sx_slock(&allproc_lock);
	FOREACH_PROC_IN_SYSTEM(p) {
		PROC_LOCK(p);
		if (stxshim_proc_is_target(p, context->supervisor_pid) != 0) {
			if (signal_number != 0)
				kern_psignal(p, signal_number);
			count++;
		}
		PROC_UNLOCK(p);
	}
	sx_sunlock(&allproc_lock);
	return (count);
}

static uint32_t
stxshim_signal_targets(void *context, int hard)
{
	return (stxshim_pass((const struct stxshim_context *)context,
	    hard != 0 ? SIGKILL : SIGTERM));
}

static uint32_t
stxshim_count_targets(void *context)
{
	return (stxshim_pass((const struct stxshim_context *)context, 0));
}

static void
stxshim_sleep_poll(void *context __unused)
{
	(void)pause_sbt("stxtrm", SBT_1MS * (sbintime_t)STXSHIM_POLL_MS, 0, 0);
}

static uint64_t
stxshim_now_ms(void *context __unused)
{
	return ((uint64_t)(sbinuptime() / SBT_1MS));
}

static void
stxshim_sync_filesystems(void *context __unused)
{
	(void)kern_sync(curthread);
}

static int
stxshim_terminate(struct thread *td, struct stxshim_terminate_result *result)
{
	struct stxshim_context context;
	struct stxshim_terminate_ops ops;

	mtx_lock(&stxshim_mtx);
	if (stxshim_supervisor_pid == 0 ||
	    stxshim_supervisor_pid != (u_int)td->td_proc->p_pid) {
		mtx_unlock(&stxshim_mtx);
		return (EPERM);
	}
	if (stxshim_terminating != 0) {
		mtx_unlock(&stxshim_mtx);
		return (EBUSY);
	}
	stxshim_terminating = 1;
	context.supervisor_pid = (int)stxshim_supervisor_pid;
	mtx_unlock(&stxshim_mtx);

	ops.context = &context;
	ops.signal_targets = stxshim_signal_targets;
	ops.count_targets = stxshim_count_targets;
	ops.sleep_poll = stxshim_sleep_poll;
	ops.now_ms = stxshim_now_ms;
	ops.sync_filesystems = stxshim_sync_filesystems;
	stxshim_terminate_sequence(&ops, result);

	mtx_lock(&stxshim_mtx);
	stxshim_terminating = 0;
	stxshim_terminations++;
	mtx_unlock(&stxshim_mtx);
	printf("stxshim: terminate term=%u kill=%u rounds=%u remaining=%u ms=%u\n",
	    result->term_signalled, result->kill_signalled, result->kill_rounds,
	    result->remaining, result->elapsed_ms);
	return (0);
}

static int
stxshim_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data,
    int fflag __unused, struct thread *td)
{
	int error;

	error = priv_check(td, PRIV_DRIVER);
	if (error != 0)
		return (error);
	switch (cmd) {
	case STXSHIM_REGISTER:
		return (stxshim_register(td));
	case STXSHIM_TERMINATE:
		return (stxshim_terminate(td,
		    (struct stxshim_terminate_result *)(void *)data));
	default:
		return (ENOTTY);
	}
}

static struct cdevsw stxshim_cdevsw = {
	.d_version =	D_VERSION,
	.d_ioctl =	stxshim_ioctl,
	.d_name =	"stxshim",
};

static int
stxshim_modevent(module_t mod __unused, int type, void *data __unused)
{
	struct make_dev_args args;
	int error;

	switch (type) {
	case MOD_LOAD:
		mtx_init(&stxshim_mtx, "stxshim", NULL, MTX_DEF);
		stxshim_exit_tag = EVENTHANDLER_REGISTER(process_exit,
		    stxshim_process_exit, NULL, EVENTHANDLER_PRI_ANY);
		make_dev_args_init(&args);
		args.mda_devsw = &stxshim_cdevsw;
		args.mda_uid = UID_ROOT;
		args.mda_gid = GID_WHEEL;
		args.mda_mode = 0600;
		error = make_dev_s(&args, &stxshim_dev, "stxshim");
		if (error != 0) {
			EVENTHANDLER_DEREGISTER(process_exit, stxshim_exit_tag);
			mtx_destroy(&stxshim_mtx);
			return (error);
		}
		printf("stxshim: ready\n");
		return (0);
	case MOD_UNLOAD:
		/* The shim stays for the life of the system. */
		return (EBUSY);
	case MOD_SHUTDOWN:
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

DEV_MODULE(stxshim, stxshim_modevent, NULL);
MODULE_VERSION(stxshim, 1);
