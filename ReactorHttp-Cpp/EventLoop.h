#pragma once
#include "Dispatcher.h"
#include "Channel.h"
#include <thread>
#include <string>
#include <queue>
#include <map>
#include <mutex>
#include <atomic>
#include <functional>
using namespace std;

// 处理该节点中的channel的方式
enum class ElemType:char{ADD, DELETE, MODIFY};
// 定义任务队列的节点
struct ChannelElement
{
    ElemType type;   // 如何处理该节点中的channel
    Channel* channel;
};
class Dispatcher;

class EventLoop
{
public:
    EventLoop();
    EventLoop(const string threadName);
    ~EventLoop();
    // 启动反应堆模型
    int run();
    void requestStop();
    void connectionOpened();
    void connectionClosed();
    size_t connectionCount() const;
    int processTimeouts();
    // 处理别激活的文件fd
    int eventActive(int fd, int event);
    // 添加任务到任务队列
    int addTask(Channel* channel, ElemType type);
    // 把回调投递到事件循环线程执行（线程安全，muduo runInLoop 的简化版）
    int queueInLoop(function<void()> fn);
    // 每轮事件循环结束后调用一次（用于停机监控等周期任务）
    void setIdleCallback(function<void()> fn);
    // 处理任务队列中的任务
    int processTaskQ();
    // 处理dispatcher中的节点
    int add(Channel* channel);
    int remove(Channel* channel);
    int modify(Channel* channel);
    int removeFd(int fd);
    // 释放channel
    int freeChannel(Channel* channel);
    int readMessage();
    // 返回线程ID
    inline thread::id getThreadID()
    {
        return m_threadID;
    }
    inline string getThreadName()
    {
        return m_threadName;
    }
    static int readLocalMessage(void* arg);

private:
    void taskWakeup();

private:
    atomic<bool> m_isQuit;
    atomic<size_t> m_connectionCount;
    // 该指针指向子类的实例 epoll, poll, select
    Dispatcher* m_dispatcher;
    // 任务队列
    queue<ChannelElement*> m_taskQ;
    queue<function<void()>> m_taskFnQ;
    function<void()> m_idleCallback;
    // map
    map<int, Channel*> m_channelMap;
    // 线程id, name, mutex
    thread::id m_threadID;
    string m_threadName;
    mutex m_mutex;
    int m_socketPair[2];  // 存储本地通信的fd 通过socketpair 初始化
};


