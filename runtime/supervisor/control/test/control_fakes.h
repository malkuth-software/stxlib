// control_fakes.h: fake service actions and a fake supervisor system for
// the control channel's host tests (test_control_server.cpp) and the web
// interface's (runtime/web/test/test_api.cpp, which runs a real
// ControlServer behind its fake backend).

#ifndef STXLIB_RUNTIME_SUPERVISOR_CONTROL_TEST_CONTROL_FAKES_H
#define STXLIB_RUNTIME_SUPERVISOR_CONTROL_TEST_CONTROL_FAKES_H

#include <cstdint>
#include <cstring>
#include <span>

#include "../../config/service_registry.h"
#include "../control_protocol.h"
#include "../control_server.h"

namespace stxcontrol::test {

using stxconfig::ControlAccess;
using stxconfig::ServiceSignal;
using stxconfig::SpawnOutcome;

class FakeActions final : public stxconfig::ServiceActions {
public:
    FakeActions() noexcept : next_pid_(100), signals_(0) {}
    [[nodiscard]] SpawnOutcome spawn(const char* const* arguments, const char* const* environment,
                                     const char* directory, ControlAccess control) noexcept override {
        (void)arguments;
        (void)environment;
        (void)directory;
        (void)control;
        const int pid = next_pid_;
        next_pid_ += 1;
        return SpawnOutcome::spawned(pid);
    }
    void signal(int pid, ServiceSignal which) noexcept override {
        (void)pid;
        (void)which;
        signals_ += 1;
    }
    void report(const stxconfig::ServiceEvent& event) noexcept override {
        (void)event;
    }
    [[nodiscard]] int signals() const noexcept {
        return signals_;
    }

private:
    int next_pid_;
    int signals_;
};

class FakeSystem final : public ControlSystem {
public:
    FakeSystem() noexcept
        : text_{}, size_(0), sequence_(0), fail_writes_(false), reboots_(0), program_(program_answer(Status::unsupported)),
          operation_(operation_none()) {}

    [[nodiscard]] BootLogRead read_boot_log_end(std::span<char> destination) noexcept override {
        const char* const log = "first line\nsecond line\n";
        const std::size_t size = std::strlen(log) <= destination.size() ? std::strlen(log) : destination.size();
        std::memcpy(destination.data(), log, size);
        return BootLogRead::text(size, false);
    }
    [[nodiscard]] UserConfigRead read_user_config(std::span<char> destination) noexcept override {
        if (sequence_ == 0u) {
            return UserConfigRead::absent();
        }
        std::memcpy(destination.data(), text_, size_);
        return UserConfigRead::text(sequence_ % 2u == 1u ? ConfigSource::file_0 : ConfigSource::file_1, sequence_,
                                    size_);
    }
    [[nodiscard]] Status write_user_config(std::span<const char> text, ConfigWritten& written) noexcept override {
        if (fail_writes_ || text.size() > sizeof(text_)) {
            return Status::store_failed;
        }
        std::memcpy(text_, text.data(), text.size());
        size_ = text.size();
        sequence_ += 1;
        written.sequence = sequence_;
        written.file_index = static_cast<std::uint8_t>((sequence_ + 1u) % 2u);
        return Status::ok;
    }
    void request_reboot() noexcept override {
        reboots_ += 1;
    }
    [[nodiscard]] ProgramAnswer program_state(const ServiceName& target) noexcept override {
        (void)target;
        return program_;
    }
    [[nodiscard]] ProgramAnswer program_install(const ServiceName& target, const InstallSource& source) noexcept override {
        (void)target;
        (void)source;
        return program_;
    }
    [[nodiscard]] ProgramAnswer program_rollback(const ServiceName& target) noexcept override {
        (void)target;
        return program_;
    }
    [[nodiscard]] ProgramAnswer program_check(const ServiceName& target) noexcept override {
        (void)target;
        return program_;
    }
    [[nodiscard]] Status system_update() noexcept override {
        return Status::unsupported;
    }
    [[nodiscard]] OperationRecord operation_state() noexcept override {
        return operation_;
    }

    void set_fail_writes(bool fail) noexcept {
        fail_writes_ = fail;
    }
    void set_program(const ProgramAnswer& answer) noexcept {
        program_ = answer;
    }
    void set_operation(const OperationRecord& record) noexcept {
        operation_ = record;
    }
    [[nodiscard]] int reboots() const noexcept {
        return reboots_;
    }

private:
    char text_[config_text_max];
    std::size_t size_;
    std::uint32_t sequence_;
    bool fail_writes_;
    int reboots_;
    ProgramAnswer program_;
    OperationRecord operation_;
};

}  // namespace stxcontrol::test

#endif  // STXLIB_RUNTIME_SUPERVISOR_CONTROL_TEST_CONTROL_FAKES_H
