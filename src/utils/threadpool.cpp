#include "threadpool.h"

threadpool::threadpool() : m_stopPool(false) { }

threadpool::~threadpool() { }

void threadpool::start(size_t threads)
{
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

        // don't allow enqueueing after stopping the pool
        if (m_stopPool)
            throw std::runtime_error("enqueue on stopped ThreadPool");

        m_tasks.emplace([task]() { (*task)(); });
    }
    m_condition.notify_one();
    return fres;
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
