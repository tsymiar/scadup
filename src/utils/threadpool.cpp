#include "threadpool.h"

threadpool::threadpool() : m_stopPool(false) { }

threadpool::~threadpool() { }

void threadpool::start(size_t threadsnum)
{
    for (size_t i = 0; i < threadsnum; ++i) {
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
                task();
            }
            });
    }
}

template<class F>
void threadpool::enqueue(F&& f)
{
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        m_tasks.emplace(std::forward<F>(f));
    }
    m_condition.notify_one();
}

void threadpool::stop()
{
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        m_stopPool = true;
    }
    m_condition.notify_all();
    for (std::thread& worker : m_workers) {
        worker.join();
    }
}
