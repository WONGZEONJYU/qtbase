// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2016 Intel Corporation.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qplatformdefs.h"
#include "qwaitcondition.h"
#include "qmutex.h"
#include "qreadwritelock.h"
#include "qatomic.h"
#include "qstring.h"
#include "qdeadlinetimer.h"
#include "private/qdeadlinetimer_p.h"
#include "qelapsedtimer.h"
#include "private/qcore_unix_p.h"

#include "qmutex_p.h"
#include "qreadwritelock_p.h"

#include <errno.h>
#include <sys/time.h>
#include <time.h>

QT_BEGIN_NAMESPACE

#ifdef Q_OS_ANDROID
// pthread_condattr_setclock is available only since Android 5.0. On older versions, there's
// a private function for relative waits (hidden in 5.0).
// Use weakref so we can determine at runtime whether each of them is present.
static int local_condattr_setclock(pthread_condattr_t*, clockid_t)
__attribute__((weakref("pthread_condattr_setclock")));

static int local_cond_timedwait_relative(pthread_cond_t*, pthread_mutex_t *, const timespec *)
__attribute__((weakref("__pthread_cond_timedwait_relative")));
#endif

static void report_error(int code, const char *where, const char *what)
{
    if (code != 0)
        qErrnoWarning(code, "%s: %s failure", where, what);
}

void qt_initialize_pthread_cond(pthread_cond_t *cond, const char *where)
{
    pthread_condattr_t condattr;

    pthread_condattr_init(&condattr);
#ifdef CLOCK_MONOTONIC
#if defined(Q_OS_ANDROID)
    if (local_condattr_setclock)
        local_condattr_setclock(&condattr, CLOCK_MONOTONIC);
#elif !defined(Q_OS_DARWIN)
    // Darwin doesn't have this function
    pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC);
#endif
#endif
    report_error(pthread_cond_init(cond, &condattr), where, "cv init");
    pthread_condattr_destroy(&condattr);
}

void qt_abstime_for_timeout(timespec *ts, QDeadlineTimer deadline)
{
#ifdef Q_OS_DARWIN
    // on Mac, qt_gettime() (on qelapsedtimer_mac.cpp) returns ticks related to the Mach absolute time
    // that doesn't work with pthread
    // Mac also doesn't have clock_gettime
    struct timeval tv;
    qint64 nsec = deadline.remainingTimeNSecs();
    gettimeofday(&tv, 0);
    ts->tv_sec = tv.tv_sec + nsec / (1000 * 1000 * 1000);
    ts->tv_nsec = tv.tv_usec * 1000 + nsec % (1000 * 1000 * 1000);

    normalizedTimespec(*ts);
#else
    // depends on QDeadlineTimer's internals!!
    static_assert(QDeadlineTimerNanosecondsInT2);
    ts->tv_sec = deadline._q_data().first;
    ts->tv_nsec = deadline._q_data().second;
#endif
}

class QWaitConditionPrivate
{
public:
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int waiters;
    int wakeups;

    int wait_relative(QDeadlineTimer deadline)
    {
        timespec ti;
#ifdef Q_OS_ANDROID
        if (!local_condattr_setclock && local_cond_timedwait_relative) {
            qint64 nsec = deadline.remainingTimeNSecs();
            ti.tv_sec = nsec / (1000 * 1000 * 1000);
            ti.tv_nsec = nsec - ti.tv_sec * 1000 * 1000 * 1000;
            return local_cond_timedwait_relative(&cond, &mutex, &ti);
        }
#endif
        qt_abstime_for_timeout(&ti, deadline);
        return pthread_cond_timedwait(&cond, &mutex, &ti);
    }

    bool wait(QDeadlineTimer deadline)
    {
        int code;
        forever {
            if (!deadline.isForever()) {
                code = wait_relative(deadline);
            } else {
                code = pthread_cond_wait(&cond, &mutex);
            }
            if (code == 0 && wakeups == 0) {
                // spurious wakeup
                continue;
            }
            break;
        }

        Q_ASSERT_X(waiters > 0, "QWaitCondition::wait", "internal error (waiters)");
        --waiters;
        if (code == 0) {
            Q_ASSERT_X(wakeups > 0, "QWaitCondition::wait", "internal error (wakeups)");
            --wakeups;
        }
        report_error(pthread_mutex_unlock(&mutex), "QWaitCondition::wait()", "mutex unlock");

        if (code && code != ETIMEDOUT)
            report_error(code, "QWaitCondition::wait()", "cv wait");

        return (code == 0);
    }
};

QWaitCondition::QWaitCondition()
{
    d = new QWaitConditionPrivate;
    report_error(pthread_mutex_init(&d->mutex, nullptr), "QWaitCondition", "mutex init");
    qt_initialize_pthread_cond(&d->cond, "QWaitCondition");
    d->waiters = d->wakeups = 0;
}

QWaitCondition::~QWaitCondition()
{
    report_error(pthread_cond_destroy(&d->cond), "QWaitCondition", "cv destroy");
    report_error(pthread_mutex_destroy(&d->mutex), "QWaitCondition", "mutex destroy");
    delete d;
}

void QWaitCondition::wakeOne()
{
    report_error(pthread_mutex_lock(&d->mutex), "QWaitCondition::wakeOne()", "mutex lock");
    d->wakeups = qMin(d->wakeups + 1, d->waiters);
    report_error(pthread_cond_signal(&d->cond), "QWaitCondition::wakeOne()", "cv signal");
    report_error(pthread_mutex_unlock(&d->mutex), "QWaitCondition::wakeOne()", "mutex unlock");
}

void QWaitCondition::wakeAll()
{
    report_error(pthread_mutex_lock(&d->mutex), "QWaitCondition::wakeAll()", "mutex lock");
    d->wakeups = d->waiters;
    report_error(pthread_cond_broadcast(&d->cond), "QWaitCondition::wakeAll()", "cv broadcast");
    report_error(pthread_mutex_unlock(&d->mutex), "QWaitCondition::wakeAll()", "mutex unlock");
}

bool QWaitCondition::wait(QMutex *mutex, unsigned long time)
{
    if (time == std::numeric_limits<unsigned long>::max())
        return wait(mutex, QDeadlineTimer(QDeadlineTimer::Forever));
    return wait(mutex, QDeadlineTimer(time));
}

bool QWaitCondition::wait(QMutex *mutex, QDeadlineTimer deadline)
{
    if (!mutex)
        return false;

    report_error(pthread_mutex_lock(&d->mutex), "QWaitCondition::wait()", "mutex lock");
    ++d->waiters;
    mutex->unlock();

    bool returnValue = d->wait(deadline);

    mutex->lock();

    return returnValue;
}

bool QWaitCondition::wait(QReadWriteLock *readWriteLock, unsigned long time)
{
    if (time == std::numeric_limits<unsigned long>::max())
        return wait(readWriteLock, QDeadlineTimer(QDeadlineTimer::Forever));
    return wait(readWriteLock, QDeadlineTimer(time));
}

bool QWaitCondition::wait(QReadWriteLock *readWriteLock, QDeadlineTimer deadline)
{
    if (!readWriteLock)
        return false;
    auto previousState = readWriteLock->stateForWaitCondition();
    if (previousState == QReadWriteLock::Unlocked)
        return false;
    if (previousState == QReadWriteLock::RecursivelyLocked) {
        qWarning("QWaitCondition: cannot wait on QReadWriteLocks with recursive lockForWrite()");
        return false;
    }

    report_error(pthread_mutex_lock(&d->mutex), "QWaitCondition::wait()", "mutex lock");
    ++d->waiters;

    readWriteLock->unlock();

    bool returnValue = d->wait(deadline);

    if (previousState == QReadWriteLock::LockedForWrite)
        readWriteLock->lockForWrite();
    else
        readWriteLock->lockForRead();

    return returnValue;
}

QT_END_NAMESPACE
