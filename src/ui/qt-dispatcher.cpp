// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "ui/qt-dispatcher.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

namespace mf {

QtDispatcher::QtDispatcher() : context_(new QObject())
{
	/* Created on the UI thread (obs_module_load); all queued work runs there. */
	context_->moveToThread(QCoreApplication::instance()->thread());
}

QtDispatcher::~QtDispatcher()
{
	for (auto &entry : timers_) {
		if (entry.second)
			delete entry.second.data();
	}
	timers_.clear();
	delete context_;
}

void QtDispatcher::post(std::function<void()> fn)
{
	QMetaObject::invokeMethod(context_, [fn = std::move(fn)]() { fn(); }, Qt::QueuedConnection);
}

TimerId QtDispatcher::postDelayed(int delayMs, std::function<void()> fn)
{
	TimerId id = ++nextId_;
	auto *timer = new QTimer(context_);
	timer->setSingleShot(true);
	QObject::connect(timer, &QTimer::timeout, context_, [this, id, timer, fn = std::move(fn)]() {
		timers_.erase(id);
		timer->deleteLater();
		fn();
	});
	timers_[id] = timer;
	timer->start(delayMs < 0 ? 0 : delayMs);
	return id;
}

void QtDispatcher::cancel(TimerId id)
{
	auto it = timers_.find(id);
	if (it == timers_.end())
		return;
	if (it->second) {
		it->second->stop();
		it->second->deleteLater();
	}
	timers_.erase(it);
}

bool QtDispatcher::isMainThread() const
{
	return QThread::currentThread() == context_->thread();
}

} // namespace mf
