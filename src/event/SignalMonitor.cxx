/*
 * Copyright (C) 2003-2013 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "SignalMonitor.hxx"

#ifndef WIN32

#include "WakeFD.hxx"
#include "SocketMonitor.hxx"
#include "util/Manual.hxx"
#include "system/FatalError.hxx"

#ifdef USE_SIGNALFD
#include "system/SignalFD.hxx"
#else
#include "WakeFD.hxx"
#endif

#ifndef USE_SIGNALFD
#include <atomic>
#endif

#include <algorithm>

class SignalMonitor final : private SocketMonitor {
#ifdef USE_SIGNALFD
	SignalFD fd;
#else
	WakeFD fd;
#endif

public:
	SignalMonitor(EventLoop &_loop)
		:SocketMonitor(_loop) {
#ifndef USE_SIGNALFD
		SocketMonitor::Open(fd.Get());
		SocketMonitor::ScheduleRead();
#endif
	}

	~SignalMonitor() {
		/* prevent the descriptor to be closed twice */
#ifdef USE_SIGNALFD
		if (SocketMonitor::IsDefined())
#endif
			SocketMonitor::Steal();
	}

	using SocketMonitor::GetEventLoop;

#ifdef USE_SIGNALFD
	void Update(sigset_t &mask) {
		const bool was_open = SocketMonitor::IsDefined();

		fd.Create(mask);

		if (!was_open) {
			SocketMonitor::Open(fd.Get());
			SocketMonitor::ScheduleRead();
		}
	}
#else
	void WakeUp() {
		fd.Write();
	}
#endif

private:
	virtual bool OnSocketReady(unsigned flags) override;
};

/* this should be enough - is it? */
static constexpr unsigned MAX_SIGNAL = 64;

static SignalHandler signal_handlers[MAX_SIGNAL];

#ifdef USE_SIGNALFD
static sigset_t signal_mask;
#else
static std::atomic_bool signal_pending[MAX_SIGNAL];
#endif

static Manual<SignalMonitor> monitor;

#ifndef USE_SIGNALFD
static void
SignalCallback(int signo)
{
	assert(signal_handlers[signo] != nullptr);

	if (!signal_pending[signo].exchange(true))
		monitor->WakeUp();
}
#endif

void
SignalMonitorInit(EventLoop &loop)
{
#ifdef USE_SIGNALFD
	sigemptyset(&signal_mask);
#endif

	monitor.Construct(loop);
}

#ifndef USE_SIGNALFD

static void
x_sigaction(int signum, const struct sigaction &act)
{
	if (sigaction(signum, &act, nullptr) < 0)
		FatalSystemError("sigaction() failed");
}

#endif

void
SignalMonitorFinish()
{
#ifdef USE_SIGNALFD
	std::fill_n(signal_handlers, MAX_SIGNAL, nullptr);
#else
	struct sigaction sa;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = SIG_DFL;

	for (unsigned i = 0; i < MAX_SIGNAL; ++i) {
		if (signal_handlers[i] != nullptr) {
			x_sigaction(i, sa);
			signal_handlers[i] = nullptr;
		}
	}

	std::fill_n(signal_pending, MAX_SIGNAL, false);
#endif

	monitor.Destruct();
}

EventLoop &
SignalMonitorGetEventLoop()
{
	return monitor->GetEventLoop();
}

void
SignalMonitorRegister(int signo, SignalHandler handler)
{
	assert(signal_handlers[signo] == nullptr);
#ifndef USE_SIGNALFD
	assert(!signal_pending[signo]);
#endif

	signal_handlers[signo] = handler;

#ifdef USE_SIGNALFD
	sigaddset(&signal_mask, signo);

	if (sigprocmask(SIG_BLOCK, &signal_mask, nullptr) < 0)
		FatalSystemError("sigprocmask() failed");

	monitor->Update(signal_mask);
#else
	struct sigaction sa;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = SignalCallback;
	x_sigaction(signo, sa);
#endif
}

bool
SignalMonitor::OnSocketReady(unsigned)
{
#ifdef USE_SIGNALFD
	int signo;
	while ((signo = fd.Read()) >= 0) {
		assert(unsigned(signo) < MAX_SIGNAL);
		assert(signal_handlers[signo] != nullptr);

		signal_handlers[signo]();
	}
#else
	fd.Read();

	for (unsigned i = 0; i < MAX_SIGNAL; ++i)
		if (signal_pending[i].exchange(false))
			signal_handlers[i]();
#endif

	return true;
}

#endif
