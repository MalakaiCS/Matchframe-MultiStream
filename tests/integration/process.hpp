// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors
//
// POSIX child-process helpers for local receivers (integration tests only).

#pragma once

#include <chrono>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace mftest {

class Process {
public:
	Process() = default;
	Process(const Process &) = delete;
	Process &operator=(const Process &) = delete;
	Process(Process &&other) noexcept : pid_(other.pid_), exitCode_(other.exitCode_) { other.pid_ = -1; }
	Process &operator=(Process &&other) noexcept
	{
		if (this != &other) {
			terminate();
			pid_ = other.pid_;
			exitCode_ = other.exitCode_;
			other.pid_ = -1;
		}
		return *this;
	}
	~Process() { terminate(); }

	bool spawn(const std::vector<std::string> &args, const std::string &logPath)
	{
		pid_ = fork();
		if (pid_ < 0)
			return false;
		if (pid_ == 0) {
			setpgid(0, 0);
			int fd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
			if (fd >= 0) {
				dup2(fd, 1);
				dup2(fd, 2);
				close(fd);
			}
			int devnull = open("/dev/null", O_RDONLY);
			if (devnull >= 0)
				dup2(devnull, 0);
			std::vector<char *> argv;
			for (const std::string &a : args)
				argv.push_back(const_cast<char *>(a.c_str()));
			argv.push_back(nullptr);
			execvp(argv[0], argv.data());
			_exit(127);
		}
		return true;
	}

	bool running()
	{
		if (pid_ <= 0)
			return false;
		int status = 0;
		pid_t r = waitpid(pid_, &status, WNOHANG);
		if (r == 0)
			return true;
		if (r == pid_)
			exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
		pid_ = -1;
		return false;
	}

	/* SIGINT lets ffmpeg finalise its output file; escalates to SIGKILL after waitMs. */
	void terminate(int sig = SIGINT, int waitMs = 3000)
	{
		if (pid_ <= 0)
			return;
		kill(pid_, sig);
		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
		while (running() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		if (pid_ > 0) {
			kill(-pid_, SIGKILL);
			kill(pid_, SIGKILL);
			int status;
			waitpid(pid_, &status, 0);
			pid_ = -1;
		}
	}

	/* Waits for a natural exit (e.g. ffmpeg after the publisher disconnected). */
	bool waitExit(int waitMs)
	{
		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
		while (running()) {
			if (std::chrono::steady_clock::now() >= deadline)
				return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		return true;
	}

	pid_t pid() const { return pid_; }
	int exitCode() const { return exitCode_; }

private:
	pid_t pid_ = -1;
	int exitCode_ = -1;
};

/* Runs a command and captures stdout+stderr. */
inline int run_capture(const std::string &command, std::string &output)
{
	output.clear();
	FILE *pipe = popen((command + " 2>&1").c_str(), "r");
	if (!pipe)
		return -1;
	char buffer[4096];
	size_t n;
	while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
		output.append(buffer, n);
	int status = pclose(pipe);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

inline std::string shell_quote(const std::string &value)
{
	std::string out = "'";
	for (char c : value) {
		if (c == '\'')
			out += "'\\''";
		else
			out += c;
	}
	return out + "'";
}

} // namespace mftest
