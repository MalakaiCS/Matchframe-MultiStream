// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include "core/dispatcher.hpp"

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <map>

namespace mf {

/*
 * Dispatcher backed by the Qt UI thread. post() is thread-safe (queued invocation on a
 * context object living on the UI thread). When the context object is destroyed at
 * shutdown, Qt discards anything still queued, so no callback can run afterwards.
 */
class QtDispatcher final : public Dispatcher {
public:
	QtDispatcher();
	~QtDispatcher() override;

	void post(std::function<void()> fn) override;
	TimerId postDelayed(int delayMs, std::function<void()> fn) override;
	void cancel(TimerId id) override;
	bool isMainThread() const override;

private:
	QObject *context_;
	std::map<TimerId, QPointer<QTimer>> timers_;
	TimerId nextId_ = 0;
};

} // namespace mf
