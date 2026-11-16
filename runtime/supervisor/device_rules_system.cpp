// device_rules_system.cpp: see device_rules_system.h.

#include "device_rules_system.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/types.h>
#include <unistd.h>

// devfs.h uses TAILQ_ENTRY without including sys/queue.h itself.
#include <sys/queue.h>
#include <fs/devfs/devfs.h>

#include "../common/console_line.h"

namespace stxdevices {

static_assert(pattern_size == DEVFS_MAXPTRNLEN, "device_rules.h spells devfs's pattern size");

namespace {

void report(int console, const char* step, int error_number, std::size_t rules) noexcept {
    stxruntime::LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXSUPERVISOR devices ruleset=");
    stxruntime::append_decimal(line, device_ruleset);
    stxruntime::append_text(line, " rules=");
    stxruntime::append_decimal(line, rules);
    if (step == nullptr) {
        stxruntime::append_text(line, " applied\n");
    } else {
        stxruntime::append_text(line, " failed step=");
        stxruntime::append_text(line, step);
        stxruntime::append_text(line, " errno=");
        stxruntime::append_decimal(line, static_cast<std::uint64_t>(static_cast<unsigned int>(error_number)));
        stxruntime::append_text(line, "\n");
    }
    stxruntime::write_line(console, line);
}

}  // namespace

bool apply_device_plan(int console, const DevicePlan& plan) noexcept {
    const int devices = open("/dev", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (devices < 0) {
        report(console, "open", errno, plan.count);
        return false;
    }
    // Old rules of the ruleset (a restarted supervisor applies it again):
    // DEVFSIO_RDEL each number the plan uses. An absent rule is ENOENT
    // (sys/fs/devfs/devfs_rule.c:241-248, releng/15.2); ESRCH is taken as
    // absent too. Taking ENOENT as a failure left every boot without the
    // ruleset (c2_m4's surface check).
    for (std::size_t index = 0; index < plan.count; index += 1) {
        devfs_rid id = mkrid(device_ruleset, static_cast<devfs_rnum>(index + 1u));
        if (ioctl(devices, DEVFSIO_RDEL, &id) != 0 && errno != ENOENT && errno != ESRCH) {
            report(console, "delete", errno, plan.count);
            (void)close(devices);
            return false;
        }
    }
    for (std::size_t index = 0; index < plan.count; index += 1) {
        const DeviceRule& planned = plan.rules[index];
        struct devfs_rule rule{};
        rule.dr_magic = DEVFS_MAGIC;
        rule.dr_id = mkrid(device_ruleset, static_cast<devfs_rnum>(index + 1u));
        rule.dr_iacts = DRA_BACTS;
        switch (planned.action) {
        case RuleAction::hide_all:
            rule.dr_icond = 0;
            rule.dr_bacts = DRB_HIDE;
            break;
        case RuleAction::show:
            rule.dr_icond = DRC_PATHPTRN;
            std::memcpy(rule.dr_pathptrn, planned.pattern, pattern_size);
            rule.dr_bacts = DRB_UNHIDE;
            break;
        case RuleAction::show_to_group:
            rule.dr_icond = DRC_PATHPTRN;
            std::memcpy(rule.dr_pathptrn, planned.pattern, pattern_size);
            rule.dr_bacts = DRB_UNHIDE;
            rule.dr_iacts = DRA_BACTS | DRA_GID | DRA_MODE;
            rule.dr_gid = static_cast<gid_t>(planned.group);
            rule.dr_mode = static_cast<mode_t>(planned.mode);
            break;
        default:
            // Fallback: an action outside the enumeration is not added.
            continue;
        }
        if (ioctl(devices, DEVFSIO_RADD, &rule) != 0) {
            report(console, "add", errno, plan.count);
            (void)close(devices);
            return false;
        }
    }
    // SUSE makes the mount apply the ruleset to nodes created from now on;
    // SAPPLY applies it to the nodes already there (devfs(8)'s "ruleset"
    // and "rule applyset").
    devfs_rsnum ruleset = device_ruleset;
    if (ioctl(devices, DEVFSIO_SUSE, &ruleset) != 0) {
        report(console, "use", errno, plan.count);
        (void)close(devices);
        return false;
    }
    if (ioctl(devices, DEVFSIO_SAPPLY, &ruleset) != 0) {
        report(console, "apply", errno, plan.count);
        (void)close(devices);
        return false;
    }
    (void)close(devices);
    report(console, nullptr, 0, plan.count);
    return true;
}

}  // namespace stxdevices
