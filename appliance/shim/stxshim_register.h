/*
 * stxshim_register.h -- who may register as the supervisor, free of kernel
 * calls so that the same code runs in the kernel (stxshim.c) and in the host
 * test (test/test_stxshim_register.c). The includer provides uint32_t.
 *
 * C1 allowed any child of PID 1, which includes orphans PID 1 adopted (the
 * kernel records no difference, sys/kern/kern_exit.c:1657-1677), so an
 * orphan could register between a supervisor's death and its respawn. C2
 * closes that window with an object rather than a process id: PID 1 opens
 * /dev/stxshim before each supervisor start and hands the open file to that
 * child; the shim accepts a registration only on a file PID 1 opened, and
 * only once per file. An adopted orphan never held such a file (PID 1 closes
 * its own copy right after the fork, and the supervisor marks its copy
 * close-on-exec). Reasons for this remedy over the others:
 * appliance/C2_DESIGN.md, "Orphan registration".
 *
 * Inputs: whether the caller's parent is PID 1; whether PID 1 opened the
 * file the request came on, and whether that file has registered before;
 * the caller's pid; the registered supervisor's pid (0: none).
 * Result: 0 (register, or already registered: the same process asking
 * again on the same file), or an errno value: EPERM (1), EBUSY (16).
 */
#ifndef STXSHIM_REGISTER_H
#define STXSHIM_REGISTER_H

static inline int
stxshim_may_register(int parent_is_init, int opened_by_init, int file_registered_before,
    uint32_t caller_pid, uint32_t file_registered_pid, uint32_t supervisor_pid)
{
	if (parent_is_init == 0 || opened_by_init == 0)
		return (1);
	if (file_registered_before != 0) {
		/* The same process again on the same file: already registered. */
		if (file_registered_pid == caller_pid && supervisor_pid == caller_pid)
			return (0);
		return (1);
	}
	if (supervisor_pid != 0u && supervisor_pid != caller_pid)
		return (16);
	return (0);
}

#endif /* STXSHIM_REGISTER_H */
