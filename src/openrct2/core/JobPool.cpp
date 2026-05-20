/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "JobPool.h"

#include "Console.hpp"

#include <cassert>
#include <exception>

JobPool::TaskData::TaskData(std::function<void()> workFn, std::function<void()> completionFn)
    : WorkFn(std::move(workFn))
    , CompletionFn(std::move(completionFn))
{
}

JobPool::JobPool(size_t maxThreads)
{
    maxThreads = std::min<size_t>(maxThreads, std::max(1u, std::thread::hardware_concurrency()));
    for (size_t n = 0; n < maxThreads; n++)
    {
        _threads.emplace_back(&JobPool::ProcessQueue, this);
    }
}

JobPool::~JobPool()
{
    {
        std::lock_guard lock(_mutex);
        _shouldStop = true;
    }
    _condPending.notify_all();

    for (auto& th : _threads)
    {
        assert(th.joinable() != false);
        th.join();
    }
}

void JobPool::AddTask(std::function<void()> workFn, std::function<void()> completionFn)
{
    {
        std::lock_guard lock(_mutex);
        _pending.emplace_back(workFn, completionFn);
    }
    _condPending.notify_one();
}

void JobPool::Join(std::function<void()> reportFn)
{
    std::unique_lock lock(_mutex);
    while (true)
    {
        // Wait for the queue to become empty or having completed tasks.
        _condComplete.wait(lock, [this]() { return (_pending.empty() && _processing == 0) || !_completed.empty(); });

        // Dispatch all completion callbacks if there are any.
        while (!_completed.empty())
        {
            auto taskData = std::move(_completed.front());
            _completed.pop_front();

            if (taskData.CompletionFn)
            {
                lock.unlock();

                taskData.CompletionFn();

                lock.lock();
            }
        }

        if (reportFn)
        {
            lock.unlock();

            reportFn();

            lock.lock();
        }

        // If everything is empty and no more work has to be done we can stop waiting.
        if (_completed.empty() && _pending.empty() && _processing == 0)
        {
            break;
        }
    }
}

bool JobPool::IsBusy()
{
    std::lock_guard lock(_mutex);
    return _processing != 0 || !_pending.empty();
}

void JobPool::ProcessQueue()
{
    std::unique_lock lock(_mutex);
    do
    {
        // Wait for work or cancellation.
        _condPending.wait(lock, [this]() { return _shouldStop || !_pending.empty(); });

        if (!_pending.empty())
        {
            _processing++;

            auto taskData = std::move(_pending.front());
            _pending.pop_front();

            lock.unlock();

            // OPENRCT2MINI 2026-05-21: catch task exceptions so a single
            // failing job doesn't terminate the worker thread (which would
            // call std::terminate via the std::thread routine contract).
            // Originally surfaced on Windows builds: ObjectRepository::
            // LoadOrConstruct threw IOException on a malformed objects.idx,
            // the exception escaped this lambda, propagated up through
            // execute_native_thread_routine, and SIGABRT'd the whole
            // process. Log and swallow — the per-task error path is the
            // caller's responsibility, not the pool's.
            try
            {
                taskData.WorkFn();
            }
            catch (const std::exception& e)
            {
                OpenRCT2::Console::Error::WriteLine("JobPool task threw exception: %s", e.what());
            }
            catch (...)
            {
                OpenRCT2::Console::Error::WriteLine("JobPool task threw non-std::exception");
            }

            lock.lock();

            _completed.push_back(std::move(taskData));

            _processing--;
            _condComplete.notify_one();
        }
    } while (!_shouldStop);
}
