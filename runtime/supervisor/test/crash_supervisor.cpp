// crash_supervisor: a test stand-in for /sbin/stxsupervisor that exits with
// status 1 as soon as it has written its line. Installed in place of the
// supervisor in a test VM only, to exercise stxinit's respawn and back-off
// (appliance/vm/c1_s1.sh). Never part of an appliance image.
//
//   STXSUPERVISOR crash-test restart_count=<n|unknown>

#include <cstdlib>

#include <unistd.h>

#include "../../common/console_line.h"
#include "../restart_argument.h"

int main(int argument_count, char** argument_vector) {
    const char* argument = argument_count >= 2 ? argument_vector[1] : nullptr;
    const stxsupervisor::RestartCount restart_count = stxsupervisor::parse_restart_count(argument);
    stxruntime::LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXSUPERVISOR crash-test restart_count=");
    if (restart_count.has_value()) {
        stxruntime::append_decimal(line, *restart_count);
    } else {
        stxruntime::append_text(line, "unknown");
    }
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(STDOUT_FILENO, line);
    return EXIT_FAILURE;
}
