#pragma once
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include "EventLoop.h"
using namespace std;

// 定义子线程对应的结构体
class WorkerThread
{
public:
    explicit WorkerThread(int index);
    ~WorkerThread();
    // 启动线程
    void run();
    void stop();
    EventLoop* getEventLoop() const;
    size_t connectionCount() const;
    bool isDraining() const;
    void setDraining(bool draining);

private:
    void running();

private:
    thread m_thread;
    string m_name;
    mutable mutex m_mutex;
    condition_variable m_cond;    // 条件变量
    EventLoop* m_evLoop;   // 反应堆模型
    atomic<bool> m_draining;
};

