#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <thread>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>
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
    std::vector<std::thread> m_workers = {};
    std::queue<std::function<void()>> m_tasks = {};
    std::mutex m_queueMutex{};
    std::condition_variable m_condition{};
    std::atomic<bool> m_stopPool;
};

#endif // THREADPOOL_H
