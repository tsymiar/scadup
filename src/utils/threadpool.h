#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <thread>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>

class threadpool {
public:
    threadpool();
    ~threadpool();

    template<class F>
    void enqueue(F&& f);

    void start(size_t threadsnum);
    void stop();

private:
    std::vector<std::thread> m_workers;
    std::queue<std::function<void()>> m_tasks;
    std::mutex m_queueMutex;
    std::condition_variable m_condition;
    std::atomic<bool> m_stopPool;
};

#endif // THREADPOOL_H
