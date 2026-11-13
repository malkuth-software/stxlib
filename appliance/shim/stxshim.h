/*
 * stxshim.h -- the interface of the STXlib kernel shim (appliance/shim/),
 * shared by the kernel module (stxshim.c) and the supervisor
 * (runtime/supervisor/shim_client.cpp). C, usable from C++.
 *
 * The shim is a character device, /dev/stxshim (root, mode 0600), with two
 * requests:
 *
 *   STXSHIM_REGISTER   the caller becomes the registered supervisor. It
 *                      must be a child of PID 1; another live registered
 *                      supervisor makes it EBUSY, a caller that is not
 *                      PID 1's child EPERM. Registering again from the same
 *                      process succeeds. The shim marks the process
 *                      protected from the out-of-memory killer
 *                      (P_PROTECTED, the flag procctl(PROC_SPROTECT) sets),
 *                      for that process only, not its children. When the
 *                      registered process exits, by any cause, the
 *                      registration ends (the shim's death watch).
 *
 *   STXSHIM_TERMINATE  forcibly terminate userspace (owner, question 3,
 *                      option 1): every process except PID 1, the
 *                      registered supervisor and kernel processes receives
 *                      SIGTERM; after at most the grace period (1 s, ended
 *                      early when none is left) those still running receive
 *                      SIGKILL, repeated each poll until none is left or
 *                      the round limit; then file systems are synced as by
 *                      sync(2); nothing is unmounted. Only the registered
 *                      supervisor may ask (EPERM otherwise); one at a time
 *                      (EBUSY). The call returns when the sequence is done,
 *                      at most about 2 s plus the sync, with its counts.
 *                      Services the supervisor runs are among the processes
 *                      terminated; it restarts them by their registration
 *                      after the call returns.
 *
 * Read-only counters under the sysctl node kern.stxshim: supervisor_pid,
 * registrations, supervisor_exits, terminations.
 */
#ifndef STXSHIM_H
#define STXSHIM_H

#include <sys/types.h>
#include <sys/ioccom.h>

#define STXSHIM_DEVICE_PATH "/dev/stxshim"

/* What one terminate did. */
struct stxshim_terminate_result {
	uint32_t term_signalled;	/* processes sent SIGTERM */
	uint32_t kill_signalled;	/* processes sent SIGKILL in the first kill round */
	uint32_t kill_rounds;		/* SIGKILL rounds (0: none was needed) */
	uint32_t remaining;		/* still running after the last round */
	uint32_t elapsed_ms;		/* from the SIGTERM pass to the end of the sync */
};

#define STXSHIM_REGISTER	_IO('x', 1)
#define STXSHIM_TERMINATE	_IOR('x', 2, struct stxshim_terminate_result)

#endif /* STXSHIM_H */
