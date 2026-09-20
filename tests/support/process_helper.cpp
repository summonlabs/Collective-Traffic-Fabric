// Collective Traffic Fabric - independent OS process management.
// Copyright 2026 Summon Software Labs.
#include "support/process_helper.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "ctf/transport.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace ctf::test {

void emit_line(const std::string& line) {
  std::fputs(line.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

std::string participant_executable() {
#ifdef CTF_TEST_PARTICIPANT_PATH
  return CTF_TEST_PARTICIPANT_PATH;
#else
  return std::string();
#endif
}

std::string coordinator_executable() {
#ifdef CTF_TEST_COORDINATOR_PATH
  return CTF_TEST_COORDINATOR_PATH;
#else
  return std::string();
#endif
}

#if defined(_WIN32)
Status spawn_process(const std::vector<std::string>& arguments, bool inherit_console, ChildProcess& out) {
  if (arguments.empty()) {
    return Status(ErrorCode::kInternalInvariant, "no program was given to spawn");
  }
  // Every argument is quoted; the helper processes never receive an argument
  // containing a quote, so this is exact rather than approximate.
  std::string command_line;
  for (const std::string& argument : arguments) {
    if (!command_line.empty()) command_line.push_back(' ');
    command_line.push_back('"');
    command_line += argument;
    command_line.push_back('"');
  }
  std::vector<char> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back('\0');
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION information{};
  const BOOL created = ::CreateProcessA(arguments.front().c_str(), mutable_command.data(), nullptr,
                                        nullptr, FALSE, 0, nullptr, nullptr, &startup, &information);
  if (created == FALSE) {
    return Status(ErrorCode::kInternalInvariant, "cannot start a child process");
  }
  ::CloseHandle(information.hThread);
  out.handle = reinterpret_cast<long long>(information.hProcess);
  out.process_id = static_cast<int>(information.dwProcessId);
  out.running = true;
  static_cast<void>(inherit_console);
  return Status::ok();
}

Status kill_process(const ChildProcess& process) {
  if (process.handle == 0) {
    return Status(ErrorCode::kInternalInvariant, "no process handle to terminate");
  }
  HANDLE handle = reinterpret_cast<HANDLE>(process.handle);
  if (::TerminateProcess(handle, 137) == FALSE) {
    return Status(ErrorCode::kInternalInvariant, "cannot terminate the child process");
  }
  return Status::ok();
}

Status wait_process(const ChildProcess& process, int& exit_code) {
  if (process.handle == 0) {
    return Status(ErrorCode::kInternalInvariant, "no process handle to wait for");
  }
  HANDLE handle = reinterpret_cast<HANDLE>(process.handle);
  const DWORD waited = ::WaitForSingleObject(handle, INFINITE);
  if (waited != WAIT_OBJECT_0) {
    return Status(ErrorCode::kInternalInvariant, "waiting for the child process failed");
  }
  DWORD code = 0;
  if (::GetExitCodeProcess(handle, &code) == FALSE) {
    return Status(ErrorCode::kInternalInvariant, "cannot read the child exit code");
  }
  exit_code = static_cast<int>(code);
  ::CloseHandle(handle);
  return Status::ok();
}

bool process_is_running(const ChildProcess& process) {
  if (process.handle == 0) return false;
  HANDLE handle = reinterpret_cast<HANDLE>(process.handle);
  return ::WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
}
#else
Status spawn_process(const std::vector<std::string>& arguments, bool inherit_console, ChildProcess& out) {
  if (arguments.empty()) {
    return Status(ErrorCode::kInternalInvariant, "no program was given to spawn");
  }
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (const std::string& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);
  ::pid_t child = 0;
  const int result = ::posix_spawn(&child, arguments.front().c_str(), nullptr, nullptr, argv.data(), environ);
  if (result != 0) {
    return Status(ErrorCode::kInternalInvariant, "cannot start a child process");
  }
  out.handle = static_cast<long long>(child);
  out.process_id = static_cast<int>(child);
  out.running = true;
  static_cast<void>(inherit_console);
  return Status::ok();
}

Status kill_process(const ChildProcess& process) {
  if (process.handle == 0) {
    return Status(ErrorCode::kInternalInvariant, "no process to terminate");
  }
  if (::kill(static_cast<::pid_t>(process.handle), SIGKILL) != 0) {
    return Status(ErrorCode::kInternalInvariant, "cannot terminate the child process");
  }
  return Status::ok();
}

Status wait_process(const ChildProcess& process, int& exit_code) {
  int status = 0;
  if (::waitpid(static_cast<::pid_t>(process.handle), &status, 0) < 0) {
    return Status(ErrorCode::kInternalInvariant, "waiting for the child process failed");
  }
  exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return Status::ok();
}

bool process_is_running(const ChildProcess& process) {
  if (process.handle == 0) return false;
  return ::kill(static_cast<::pid_t>(process.handle), 0) == 0;
}
#endif

}  // namespace ctf::test
