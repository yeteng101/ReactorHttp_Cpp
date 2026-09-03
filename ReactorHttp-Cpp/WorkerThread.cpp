#include "WorkerThread.h"
#include <stdio.h>

// 子线程的回调函数
void WorkerThread::running()
{
    EventLoop* loop = new EventLoop(m_name);
    {
        lock_guard<mutex> lock(m_mutex);
        m_evLoop = loop;
    }
    m_cond.notify_one();
    loop->run();
}

WorkerThread::WorkerThread(int index) : m_evLoop(nullptr), m_draining(false)
{
    m_name =  "SubThread-" + to_string(index);
}

WorkerThread::~WorkerThread()
{
    stop();
}

void WorkerThread::run()
{
    // 创建子线程
    m_thread = thread(&WorkerThread::running, this);
    // 阻塞主线程, 让当前函数不会直接结束
    unique_lock<mutex> locker(m_mutex);
    m_cond.wait(locker, [this] { return m_evLoop != nullptr; });
}

void WorkerThread::stop()
{
    EventLoop* loop = nullptr;
    {
        lock_guard<mutex> lock(m_mutex);
        loop = m_evLoop;
    }
    if (loop != nullptr)
    {
        loop->requestStop();
    }
    if (m_thread.joinable())
    {
        m_thread.join();
    }
    {
        lock_guard<mutex> lock(m_mutex);
        delete m_evLoop;
        m_evLoop = nullptr;
    }
}

EventLoop* WorkerThread::getEventLoop() const
{
    lock_guard<mutex> lock(m_mutex);
    return m_evLoop;
}

size_t WorkerThread::connectionCount() const
{
    EventLoop* loop = getEventLoop();
    return loop == nullptr ? 0 : loop->connectionCount();
}

bool WorkerThread::isDraining() const
{
    return m_draining.load(memory_order_relaxed);
}

void WorkerThread::setDraining(bool draining)
{
    m_draining.store(draining, memory_order_relaxed);
}
