#pragma once
#include "EventLoop.h"
#include <stdbool.h>
#include "WorkerThread.h"
#include <vector>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <functional>
using namespace std;

// 定义线程池
class ThreadPool
{
public:
    ThreadPool(EventLoop* mainLoop, int count);
    ThreadPool(EventLoop* mainLoop, int minThreads, int maxThreads);
    ~ThreadPool();
    // 启动线程池
    void run();
    void stop();
    // 取出线程池中的某个子线程的反应堆实例
    EventLoop* takeWorkerEventLoop();
    // 在调用线程依次对每个 Worker 的事件循环执行回调（停机排空时使用）
    void forEachWorkerEventLoop(const function<void(EventLoop*)>& fn);
    // 所有 Worker 当前连接数之和
    size_t totalConnections() const;
    size_t workerCount() const;
private:
    WorkerThread* addWorkerLocked();
    void managerLoop();
    void updateMetricsLocked() const;
    size_t availableWorkerCountLocked() const;

    // 主线程的反应堆模型
    EventLoop* m_mainLoop;
    bool m_isStart;
    int m_minThreads;
    int m_maxThreads;
    int m_nextWorkerId;
    int m_lowLoadSeconds;
    vector<WorkerThread*> m_workerThreads;
    mutable mutex m_mutex;
    condition_variable m_managerCondition;
    thread m_managerThread;
    atomic<bool> m_stopping;
};

