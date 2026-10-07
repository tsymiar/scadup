
#ifndef THREADPOOL_HPP
#define THREADPOOL_HPP

#include <vector>
#include <queue>
#include <thread>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdio>
#include <future>
#include <stdexcept>

class threadpool {
public:
    threadpool();
    ~threadpool();

    template<class F>
    void enqueue(F&& f);

    template<class F, class... Args>
    std::future<typename std::result_of<F(Args...)>::type> enqueue(F&& f, Args&&... args);

    void start(size_t threads);
    void stop();

private:
    std::vector<std::thread> m_workers = { };
    std::queue<std::function<void()>> m_tasks = { };
    std::mutex m_queueMutex{ };
    /** Serializes stop(): quit() on the subscribing thread and exit() on the JNI thread
        may stop the pool at the same time; two threads joining the same std::thread
        objects throws system_error -> terminate */
    std::mutex m_thrMutex{ };
    std::condition_variable m_condition{ };
    std::atomic<bool> m_stopPool;
};

threadpool::threadpool() : m_stopPool(false) {}

threadpool::~threadpool() {}

void threadpool::start(size_t threads)
{
    // Serialized with stop(): if this cleared m_stopPool back to false while stop() is
    // joining the workers, they would keep taking tasks and never return, so the join
    // would block forever (ANR).
    std::unique_lock<std::mutex> guard(m_thrMutex);
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        // Do not emplace_back again once started: after stop() the std::thread objects
        // stay in m_workers, so appending would add new threads while the next stop()
        // joins objects that were already joined (std::system_error -> terminate).
        // Restarting only needs the stop flag cleared.
        if (!m_workers.empty()) {
            m_stopPool = false;
            return;
        }
        m_stopPool = false;
    }
    for (size_t i = 0; i < threads; ++i) {
        m_workers.emplace_back([this] {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->m_queueMutex);
                    this->m_condition.wait(lock, [this] { return this->m_stopPool || !this->m_tasks.empty(); });
                    if (this->m_stopPool && this->m_tasks.empty()) {
                        return;
                    }
                    task = std::move(this->m_tasks.front());
                    this->m_tasks.pop();
                }
                // An exception escaping a task reaches the top of the thread and calls
                // std::terminate -- which shows up as "the app dies on any click after
                // subscribing once".
                try {
                    task();
                } catch (const std::exception& e) {
                    fprintf(stderr, "threadpool task exception: %s\n", e.what());
                } catch (...) {
                    fprintf(stderr, "threadpool task unknown exception\n");
                }
            }
            });
    }
}

template<class F>
void threadpool::enqueue(F&& f)
{
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        // Drop the task when the pool is stopped: no worker will ever pick it up
        // (the threads are gone), so everything it captured just leaks.
        if (m_stopPool) return;
        m_tasks.emplace(std::forward<F>(f));
    }
    m_condition.notify_one();
}

template<class F, class... Args>
auto threadpool::enqueue(F&& f, Args&&... args)
-> std::future<typename std::result_of<F(Args...)>::type>
{
    using return_type = typename std::result_of<F(Args...)>::type;
    auto task = std::make_shared< std::packaged_task<return_type()> >(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );
    std::future<return_type> fres = task->get_future();
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);

        // Pool stopped (the previous subscription quit / the user closed): drop the task
        // and return an invalid future. Throwing here is not an option -- an exception
        // travelling from SubscribeTask to the top of a std::thread is std::terminate.
        if (m_stopPool) {
            task.reset();
            return std::future<return_type>();
        }

        m_tasks.emplace([task]() { (*task)(); });
    }
    m_condition.notify_one();
    return fres;
}

void threadpool::stop()
{
    std::unique_lock<std::mutex> guard(m_thrMutex);
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        // A second stop() would join already-joined threads and throw std::system_error
        // -> terminate. Return early once everything is already stopped.
        if (m_stopPool && m_workers.empty()) return;
        m_stopPool = true;
    }
    m_condition.notify_all();
    for (std::thread& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    // Clear: start() uses "is m_workers empty" to decide whether to spawn threads.
    // Keeping terminated objects would make the next start/stop hit the double join again.
    m_workers.clear();
    std::unique_lock<std::mutex> lock(m_queueMutex);
    std::queue<std::function<void()>> empty;
    std::swap(m_tasks, empty);
}

#endif // THREADPOOL_HPP
