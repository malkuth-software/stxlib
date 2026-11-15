/*
 * stxshim.h -- the interface of the STXlib kernel shim (appliance/shim/),
 * shared by the kernel module (stxshim.c), PID 1 (runtime/init/stxinit.cpp)
 * and the supervisor (runtime/supervisor/shim_client.cpp). C, usable from
 * C++.
 *
 * The shim is a character device, /dev/stxshim (root, mode 0600). The
 * kernel remembers, per open file, whether PID 1 opened it. Two requests:
 *
 *   STXSHIM_REGISTER   the caller becomes the registered supervisor. The
 *                      request must come on an open file that PID 1 opened
 *                      and handed to the caller (stxinit opens one per
 *                      supervisor start and passes it as descriptor
 *                      STXSHIM_SUPERVISOR_DESCRIPTOR), at most once per such
 *                      file, from a child of PID 1 (EPERM otherwise);
 *                      another live registered supervisor makes it EBUSY.
 *                      The same process asking again on the same file
 *                      succeeds. C1 accepted any child of PID 1, which let
 *                      an orphan PID 1 adopted register between a
 *                      supervisor's death and its restart; an orphan holds
 *                      no such file (stxshim_register.h). The shim marks the
 *                      process protected from the out-of-memory killer
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
/* The descriptor on which the supervisor finds the file PID 1 opened. */
#define STXSHIM_SUPERVISOR_DESCRIPTOR 3

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
