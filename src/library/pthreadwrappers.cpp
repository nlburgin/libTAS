/*
    Copyright 2015-2018 Clément Gallet <clement.gallet@ens-lyon.org>

    This file is part of libTAS.

    libTAS is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    libTAS is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with libTAS.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pthreadwrappers.h"
#include "logging.h"
#include "checkpoint/ThreadInfo.h"
#include "checkpoint/ThreadManager.h"
#include "checkpoint/ThreadSync.h"
#include "DeterministicTimer.h"
#include "tlswrappers.h"
#include "backtrace.h"
#include "hook.h"

#include <errno.h>
#include <unistd.h>
#include <cstring>
#include <atomic>
#include <memory>
#include <exception>

namespace libtas {

DEFINE_ORIG_POINTER(pthread_create);
DEFINE_ORIG_POINTER(pthread_exit);
DEFINE_ORIG_POINTER(pthread_join);
DEFINE_ORIG_POINTER(pthread_detach);
DEFINE_ORIG_POINTER(pthread_tryjoin_np);
DEFINE_ORIG_POINTER(pthread_timedjoin_np);
DEFINE_ORIG_POINTER(pthread_cond_wait);
DEFINE_ORIG_POINTER(pthread_cond_timedwait);
DEFINE_ORIG_POINTER(pthread_cond_signal);
DEFINE_ORIG_POINTER(pthread_cond_broadcast);
DEFINE_ORIG_POINTER(pthread_setcancelstate);
DEFINE_ORIG_POINTER(pthread_setcanceltype);
DEFINE_ORIG_POINTER(pthread_cancel);
DEFINE_ORIG_POINTER(pthread_testcancel);
DEFINE_ORIG_POINTER(sem_timedwait);
DEFINE_ORIG_POINTER(sem_trywait);

/* We create a specific exception for thread exit calls */
class ThreadExitException {
    const char* what () const throw ()
    {
    	return "Thread exit";
    }
};

extern "C" {
  // Declare an extern reference to the internal, undocumented
  // function that glibc uses to set (or reset) thread-local storage
  // to its initial state.

  // This function (effectively) takes a pthread_t argument on
  // 32-bit and 64-bit x86 Linux, but for some other architectures
  // it does not.  If you search the glibc source code for TLS_TCB_AT_TP,
  // architectures that define it as 1 should work with this;
  // architectures that define it as 0 would require a different
  // code path.

  // Even though this function is internal and undocumented, I don't think
  // it's likely to change; it's part of the ABI between libpthread.so
  // and ld-linux.so.
  extern void _dl_allocate_tls_init(pthread_t pt);

  // Another internal, undocumented libc function, that calls C++
  // destructors on thread-local storage.
  extern void __call_tls_dtors();

  // Another internal, undocumented libc function, that cleans up
  // any libc internal state in thread-local storage.
  extern void __libc_thread_freeres();
}


static void *pthread_start(void *arg)
{
    ThreadInfo *thread = static_cast<ThreadInfo*>(arg);

    std::unique_lock<std::mutex> lock(thread->mutex);

    ThreadManager::initThreadFromChild(thread);

    do {
        /* Check if there is a function to execute */
        if (thread->state == ThreadInfo::ST_RUNNING) {
            ThreadManager::update(thread);
            ThreadSync::decrementUninitializedThreadCount();

            debuglog(LCF_THREAD, "Beginning of thread code ", thread->routine_id);

            /* We need to handle the case where the thread calls pthread_exit to
             * terminate. Because we recycle thread routines, we must continue
             * the execution past the routine, so we are using the exception
             * feature for that.
             */
            void *ret;
            try {
                /* Execute the function */
                ret = thread->start(thread->arg);
            }
            catch (const ThreadExitException& e) {}

            debuglog(LCF_THREAD, "End of thread code");

            /* Because we recycle this thread, we must unset all TLS values
             * and call destructors ourselves.  First, we unset the values
             * from the older, pthread_key_create()-based implementation
             * of TLS.
             */
            clear_pthread_keys();
            /* Next we deal with the newer linker-based TLS
             * implementation accessed via thread_local in C11/C++11
             * and later.  For that, first we need to run any C++
             * destructors for values in thread-local storage, using
             * an internal libc function.
             */
            __call_tls_dtors();
            /* Next, we clean up any libc state in thread-local storage,
             * using an internal libc function.
             */
            __libc_thread_freeres();
            /* Finally, we reset all thread-local storage back to its
             * initial value, using a third internal libc function.
             * This is architecture-specific; it works on 32-bit and
             * 64-bit x86, but not on all Linux architectures.  See
             * above, where this function is declared.
             */
            _dl_allocate_tls_init(thread->pthread_id);
            /* This has just reset any libTAS thread-local storage
             * for this thread.  Most libTAS TLS is either transient
             * anyway, or irrelevant while the thread is waiting
             * to be recycled.  But one value is important:
             * we need to fix ThreadManager::current_thread .
             */
            ThreadManager::setCurrentThread(thread);

            ThreadManager::threadExit(ret);

            /* Thread is now in zombie state until it is detached */
            // while (thread->state == ThreadInfo::ST_ZOMBIE) {
            //     struct timespec mssleep = {0, 1000*1000};
            //     NATIVECALL(nanosleep(&mssleep, NULL)); // Wait 1 ms before trying again
            // }
        }
        else {
             thread->cv.wait(lock);
        }
    } while (!thread->quit && shared_config.recycle_threads); /* Check if game is quitting */

    return nullptr;
}


/* Override */ int pthread_create (pthread_t * tid_p, const pthread_attr_t * attr, void * (* start_routine) (void *), void * arg) throw()
{
    debuglog(LCF_THREAD, "Thread is created with routine ", (void*)start_routine);
    LINK_NAMESPACE(pthread_create, "pthread");

    ThreadSync::wrapperExecutionLockLock();
    ThreadSync::incrementUninitializedThreadCount();

    /* Creating a new or recycled thread, and filling some parameters.
     * The rest (like thread->tid) will be filled by the child thread.
     */
    ThreadInfo* thread = ThreadManager::getNewThread();
    bool isRecycled = ThreadManager::initThreadFromParent(thread, start_routine, arg, __builtin_return_address(0));

    /* Threads can be created in detached state */
    int detachstate = PTHREAD_CREATE_JOINABLE; // default
    if (attr) {
        pthread_attr_getdetachstate(attr, &detachstate);
        debuglog(LCF_THREAD, "Detached state is ", detachstate);
        debuglog(LCF_THREAD, "Default state is ", PTHREAD_CREATE_JOINABLE);
    }
    thread->detached = (detachstate == PTHREAD_CREATE_DETACHED);

    int ret = 0;
    if (isRecycled) {
        debuglog(LCF_THREAD, "Recycling thread ", thread->tid);
        *tid_p = thread->pthread_id;
        /* Notify the thread that it has a function to execute */
        thread->cv.notify_all();
    }
    else {
        /* Call our wrapper function */
        ret = orig::pthread_create(tid_p, attr, pthread_start, thread);

        if (ret != 0) {
            /* Thread creation failed */
            ThreadSync::decrementUninitializedThreadCount();
            ThreadManager::threadIsDead(thread);
        }
    }

    ThreadSync::wrapperExecutionLockUnlock();
    return ret;
}

/* Override */ void pthread_exit (void *retval)
{
    LINK_NAMESPACE(pthread_exit, "pthread");
    debuglog(LCF_THREAD, "Thread has exited.");

    if (shared_config.recycle_threads) {
        /* We need to jump to code after the end of the original thread routine */
        throw ThreadExitException();

        // ThreadInfo* thread = ThreadManager::getThread(ThreadManager::getThreadId());
        // longjmp(thread->env, 1);
    }
    else {
        ThreadManager::threadExit(retval);
        orig::pthread_exit(retval);
    }
}

/* Override */ int pthread_join (pthread_t pthread_id, void **thread_return)
{
    LINK_NAMESPACE(pthread_join, "pthread");
    if (GlobalState::isNative()) {
        return orig::pthread_join(pthread_id, thread_return);
    }

    ThreadSync::wrapperExecutionLockLock();
    ThreadSync::waitForThreadsToFinishInitialization();

    debuglog(LCF_THREAD, "Joining thread ", ThreadManager::getThreadTid(pthread_id));

    ThreadInfo* thread = ThreadManager::getThread(pthread_id);

    if (!thread) {
        ThreadSync::wrapperExecutionLockUnlock();
        return ESRCH;
    }

    if (thread->detached) {
        ThreadSync::wrapperExecutionLockUnlock();
        return EINVAL;
    }

    int ret = 0;
    if (shared_config.recycle_threads) {
        /* Wait for the thread to become zombie */
        while (thread->state != ThreadInfo::ST_ZOMBIE) {
            struct timespec mssleep = {0, 1000*1000};
            NATIVECALL(nanosleep(&mssleep, NULL)); // Wait 1 ms before trying again
        }
    }
    else {
        ret = orig::pthread_join(pthread_id, thread_return);
    }

    ThreadManager::threadDetach(pthread_id);
    ThreadSync::wrapperExecutionLockUnlock();
    return ret;
}

/* Override */ int pthread_detach (pthread_t pthread_id) throw()
{
    LINK_NAMESPACE(pthread_detach, "pthread");
    if (GlobalState::isNative()) {
        return orig::pthread_detach(pthread_id);
    }

    ThreadSync::wrapperExecutionLockLock();
    ThreadSync::waitForThreadsToFinishInitialization();

    debuglog(LCF_THREAD, "Detaching thread ", ThreadManager::getThreadTid(pthread_id));
    ThreadInfo* thread = ThreadManager::getThread(pthread_id);

    if (!thread) {
        ThreadSync::wrapperExecutionLockUnlock();
        return ESRCH;
    }

    if (thread->detached) {
        ThreadSync::wrapperExecutionLockUnlock();
        return EINVAL;
    }

    int ret = 0;
    if (! shared_config.recycle_threads) {
        ret = orig::pthread_detach(pthread_id);
    }

    ThreadManager::threadDetach(pthread_id);
    ThreadSync::wrapperExecutionLockUnlock();
    return ret;
}

/* Override */ int pthread_tryjoin_np(pthread_t pthread_id, void **retval) throw()
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(pthread_tryjoin_np, "pthread");
        return orig::pthread_tryjoin_np(pthread_id, retval);
    }

    ThreadSync::wrapperExecutionLockLock();
    ThreadSync::waitForThreadsToFinishInitialization();

    debuglog(LCF_THREAD, "Try to join thread ", ThreadManager::getThreadTid(pthread_id));
    ThreadInfo* thread = ThreadManager::getThread(pthread_id);

    if (!thread) {
        ThreadSync::wrapperExecutionLockUnlock();
        return ESRCH;
    }

    if (thread->detached) {
        ThreadSync::wrapperExecutionLockUnlock();
        return EINVAL;
    }

    int ret = 0;
    if (shared_config.recycle_threads) {
        if (thread->state == ThreadInfo::ST_ZOMBIE) {
            if (retval) {
                *retval = thread->retval;
            }
            ThreadManager::threadDetach(pthread_id);
        }
        else {
            ret = EBUSY;
        }
    }
    else {
        ret = orig::pthread_tryjoin_np(pthread_id, retval);
        if (ret == 0) {
            ThreadManager::threadDetach(pthread_id);
        }
    }

    if (ret == 0)
        debuglog(LCF_THREAD, "Joining thread successfully.");
    else
        debuglog(LCF_THREAD, "Thread has not yet terminated.");
    ThreadSync::wrapperExecutionLockUnlock();
    return EBUSY;
}

/* Override */ int pthread_timedjoin_np(pthread_t pthread_id, void **retval, const struct timespec *abstime)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(pthread_timedjoin_np, "pthread");
        return orig::pthread_timedjoin_np(pthread_id, retval, abstime);
    }

    ThreadSync::wrapperExecutionLockLock();
    ThreadSync::waitForThreadsToFinishInitialization();

    debuglog(LCF_THREAD | LCF_TODO, "Try to join thread in ", 1000*abstime->tv_sec + abstime->tv_nsec/1000000," ms.");

    if (abstime->tv_sec < 0 || abstime->tv_nsec >= 1000000000) {
        ThreadSync::wrapperExecutionLockUnlock();
        return EINVAL;
    }

    ThreadInfo* thread = ThreadManager::getThread(pthread_id);

    if (!thread) {
        ThreadSync::wrapperExecutionLockUnlock();
        return ESRCH;
    }

    if (thread->detached) {
        ThreadSync::wrapperExecutionLockUnlock();
        return EINVAL;
    }

    int ret = 0;
    if (shared_config.recycle_threads) {
        /* For now I'm lazy, so we just wait the amount of time and check joining */
        NATIVECALL(nanosleep(abstime, NULL));

        if (thread->state == ThreadInfo::ST_ZOMBIE) {
            if (retval) {
                *retval = thread->retval;
            }
            ThreadManager::threadDetach(pthread_id);
        }
        else {
            ret = ETIMEDOUT;
        }
    }
    else {
        ret = orig::pthread_timedjoin_np(pthread_id, retval, abstime);
        if (ret == 0) {
            ThreadManager::threadDetach(pthread_id);
        }
    }

    if (ret == 0)
        debuglog(LCF_THREAD, "Joining thread successfully.");
    else
        debuglog(LCF_THREAD, "Call timed out before thread terminated.");

    ThreadSync::wrapperExecutionLockUnlock();
    return ETIMEDOUT;
}

/* Override */ int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    LINK_NAMESPACE_VERSION(pthread_cond_wait, "pthread", "GLIBC_2.3.2");
    if (GlobalState::isNative())
        return orig::pthread_cond_wait(cond, mutex);

    debuglog(LCF_WAIT | LCF_TODO, __func__, " call with cond ", static_cast<void*>(cond), " and mutex ", static_cast<void*>(mutex));
    return orig::pthread_cond_wait(cond, mutex);
}

/* Override */ int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime)
{
    LINK_NAMESPACE_VERSION(pthread_cond_timedwait, "pthread", "GLIBC_2.3.2");
    if (GlobalState::isNative())
        return orig::pthread_cond_timedwait(cond, mutex, abstime);

    debuglog(LCF_WAIT | LCF_TODO, __func__, " call with cond ", static_cast<void*>(cond), " and mutex ", static_cast<void*>(mutex));

    /* If not main thread, do not change the behavior */
    if (!ThreadManager::isMainThread())
        return orig::pthread_cond_timedwait(cond, mutex, abstime);

    if (shared_config.wait_timeout == SharedConfig::WAIT_NATIVE)
        return orig::pthread_cond_timedwait(cond, mutex, abstime);

    TimeHolder now = detTimer.getTicks();

    if (shared_config.wait_timeout == SharedConfig::WAIT_FINITE) {
        /* Wait for 0.1 sec, arbitrary */
        TimeHolder delta_time;
        delta_time.tv_sec = 0;
        delta_time.tv_nsec = 100*1000*1000;
        TimeHolder new_end_time = now + delta_time;
        int ret = orig::pthread_cond_timedwait(cond, mutex, &new_end_time);
        if (ret == 0)
            return ret;
    }

    if ((shared_config.wait_timeout == SharedConfig::WAIT_FULL_INFINITE) ||
        (shared_config.wait_timeout == SharedConfig::WAIT_FINITE)) {
        /* Transfer time to our deterministic timer */
        TimeHolder end = *abstime;
        // end.tv_sec = end_time / (1000*1000);
        // end.tv_nsec = (end_time % (1000*1000)) * 1000;
        TimeHolder delay = end - now;
        detTimer.addDelay(delay);
    }

    if (shared_config.wait_timeout == SharedConfig::WAIT_FINITE) {
        /* Wait again for 0.1 sec, arbitrary */
        now = detTimer.getTicks();
        TimeHolder delta_time;
        delta_time.tv_sec = 0;
        delta_time.tv_nsec = 100*1000*1000;
        TimeHolder new_end_time = now + delta_time;

        // gint64 new_end_time = (static_cast<gint64>(now.tv_sec) * 1000000) + (now.tv_nsec / 1000) + 100*1000;
        return orig::pthread_cond_timedwait(cond, mutex, &new_end_time);
    }

    /* Infinite wait */
    LINK_NAMESPACE_VERSION(pthread_cond_wait, "pthread", "GLIBC_2.3.2");
    return orig::pthread_cond_wait(cond, mutex);
}

/* Override */ int pthread_cond_signal(pthread_cond_t *cond) throw()
{
    LINK_NAMESPACE_VERSION(pthread_cond_signal, "pthread", "GLIBC_2.3.2");
    if (GlobalState::isNative())
        return orig::pthread_cond_signal(cond);

    debuglog(LCF_WAIT | LCF_TODO, __func__, " call with cond ", static_cast<void*>(cond));
    return orig::pthread_cond_signal(cond);
}

/* Override */ int pthread_cond_broadcast(pthread_cond_t *cond) throw()
{
    LINK_NAMESPACE_VERSION(pthread_cond_broadcast, "pthread", "GLIBC_2.3.2");
    if (GlobalState::isNative())
        return orig::pthread_cond_broadcast(cond);

    debuglog(LCF_WAIT | LCF_TODO, __func__, " call with cond ", static_cast<void*>(cond));
    return orig::pthread_cond_broadcast(cond);
}

/* Override */ int pthread_setcancelstate (int state, int *oldstate)
{
    LINK_NAMESPACE(pthread_setcancelstate, "pthread");
    DEBUGLOGCALL(LCF_THREAD | LCF_TODO);
    return orig::pthread_setcancelstate(state, oldstate);
}

/* Override */ int pthread_setcanceltype (int type, int *oldtype)
{
    LINK_NAMESPACE(pthread_setcanceltype, "pthread");
    DEBUGLOGCALL(LCF_THREAD | LCF_TODO);
    return orig::pthread_setcanceltype(type, oldtype);
}

/* Override */ int pthread_cancel (pthread_t pthread_id)
{
    LINK_NAMESPACE(pthread_cancel, "pthread");
    debuglog(LCF_THREAD | LCF_TODO, "Cancel thread ", ThreadManager::getThreadTid(pthread_id));
    return orig::pthread_cancel(pthread_id);
}

/* Override */ void pthread_testcancel (void)
{
    LINK_NAMESPACE(pthread_testcancel, "pthread");
    DEBUGLOGCALL(LCF_THREAD | LCF_TODO);
    return orig::pthread_testcancel();
}

int sem_timedwait (sem_t * sem, const struct timespec *abstime)
{
    LINK_NAMESPACE(sem_timedwait, "pthread");
    if (GlobalState::isNative())
        return orig::sem_timedwait(sem, abstime);

    DEBUGLOGCALL(LCF_THREAD | LCF_TODO);
    return orig::sem_timedwait(sem, abstime);
}

int sem_trywait (sem_t *sem) throw()
{
    LINK_NAMESPACE(sem_trywait, "pthread");
    if (GlobalState::isNative())
        return orig::sem_trywait(sem);

    DEBUGLOGCALL(LCF_THREAD | LCF_TODO);
    return orig::sem_trywait(sem);
}

}
