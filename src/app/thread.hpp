// PShare - A thread with a stack size of our choosing.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Console threads start with a small stack and are never time-sliced, so
// work that runs TLS or decodes JPEGs gets a thread of its own with 1 MiB
// (ps5-native-app-boilerplate docs/CURL.md, ps5-homebrew-ui ADOPTING.md).
// std::thread cannot set the stack size; pthread can, on the console and on
// the PC alike.

#pragma once

#include <cstddef>
#include <functional>
#include <pthread.h>

namespace pshare::app
{

class Thread
{
  public:
    Thread() = default;
    Thread(const Thread &) = delete;
    Thread &operator=(const Thread &) = delete;
    ~Thread()
    {
        join();
    }

    bool start(std::function<void()> body, std::size_t stack = 1u << 20)
    {
        if (running_)
            return false;
        body_ = std::move(body);
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, stack);
        running_ = pthread_create(&thread_, &attributes, &Thread::entry, this) == 0;
        pthread_attr_destroy(&attributes);
        return running_;
    }
    void join()
    {
        if (!running_)
            return;
        pthread_join(thread_, nullptr);
        running_ = false;
    }
    bool running() const
    {
        return running_;
    }

  private:
    static void *entry(void *self)
    {
        static_cast<Thread *>(self)->body_();
        return nullptr;
    }
    std::function<void()> body_;
    pthread_t thread_{};
    bool running_ = false;
};

// A mutex, a scoped lock and a condition, straight on pthread. libc++'s
// std::mutex and std::unique_lock report misuse by throwing, which drags the
// exception runtime and the unwinder into a native title that has neither.
class Mutex
{
  public:
    Mutex()
    {
        pthread_mutex_init(&mutex_, nullptr);
    }
    ~Mutex()
    {
        pthread_mutex_destroy(&mutex_);
    }
    Mutex(const Mutex &) = delete;
    Mutex &operator=(const Mutex &) = delete;
    void lock()
    {
        pthread_mutex_lock(&mutex_);
    }
    void unlock()
    {
        pthread_mutex_unlock(&mutex_);
    }
    pthread_mutex_t *native()
    {
        return &mutex_;
    }

  private:
    pthread_mutex_t mutex_;
};

class Lock
{
  public:
    explicit Lock(Mutex &mutex) : mutex_(mutex)
    {
        mutex_.lock();
    }
    ~Lock()
    {
        mutex_.unlock();
    }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
    Mutex &mutex()
    {
        return mutex_;
    }

  private:
    Mutex &mutex_;
};

class Condition
{
  public:
    Condition()
    {
        pthread_cond_init(&condition_, nullptr);
    }
    ~Condition()
    {
        pthread_cond_destroy(&condition_);
    }
    Condition(const Condition &) = delete;
    Condition &operator=(const Condition &) = delete;
    // Waits until `ready()` holds; the lock is held whenever it is asked.
    template <typename Ready> void wait(Lock &lock, Ready ready)
    {
        while (!ready())
            pthread_cond_wait(&condition_, lock.mutex().native());
    }
    void notify_one()
    {
        pthread_cond_signal(&condition_);
    }
    void notify_all()
    {
        pthread_cond_broadcast(&condition_);
    }

  private:
    pthread_cond_t condition_;
};

} // namespace pshare::app
