#include "EventLoop.h"
#include <assert.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <utility>
#include <vector>
#include "SelectDispatcher.h"
#include "PollDispatcher.h"
#ifdef __linux__
#include "EpollDispatcher.h"
#endif

EventLoop::EventLoop() : EventLoop(string())
{
}

EventLoop::EventLoop(const string threadName)
{
    m_isQuit.store(true);    // 默认没有启动
    m_connectionCount.store(0);
    m_threadID = this_thread::get_id();
    m_threadName = threadName == string() ? "MainThread" : threadName;
#ifdef __linux__
    m_dispatcher = new EpollDispatcher(this);
#else
    m_dispatcher = new SelectDispatcher(this);
#endif
    // map
    m_channelMap.clear();
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, m_socketPair);
    if (ret == -1)
    {
        perror("socketpair");
        exit(0);
    }
#if 0
    // 指定规则: evLoop->socketPair[0] 发送数据, evLoop->socketPair[1] 接收数据
    Channel* channel = new Channel(m_socketPair[1], FDEvent::ReadEvent,
        readLocalMessage, nullptr, nullptr, this);
#else
    // 绑定 - bind
    auto obj = bind(&EventLoop::readMessage, this);
    Channel* channel = new Channel(m_socketPair[1], FDEvent::ReadEvent,
        obj, nullptr, nullptr, this);
#endif
    // channel 添加到任务队列
    addTask(channel, ElemType::ADD);
}

EventLoop::~EventLoop()
{
    for (auto& item : m_channelMap)
    {
        close(item.first);
        delete item.second;
    }
    m_channelMap.clear();
    close(m_socketPair[0]);
    close(m_socketPair[1]);
    delete m_dispatcher;
}

int EventLoop::run()
{
    m_isQuit.store(false);
    // 比较线程ID是否正常
    if (m_threadID != this_thread::get_id())
    {
        return -1;
    }
    // 循环进行事件处理
    while (!m_isQuit.load())
    {
        m_dispatcher->dispatch();    // 超时时长 2s
        processTaskQ();
        processTimeouts();
        if (m_idleCallback)
        {
            m_idleCallback();
        }
    }
    return 0;
}

void EventLoop::requestStop()
{
    m_isQuit.store(true);
    taskWakeup();
}

void EventLoop::connectionOpened()
{
    m_connectionCount.fetch_add(1, memory_order_relaxed);
}

void EventLoop::connectionClosed()
{
    size_t count = m_connectionCount.load(memory_order_relaxed);
    while (count > 0 && !m_connectionCount.compare_exchange_weak(
        count, count - 1, memory_order_relaxed))
    {
    }
}

size_t EventLoop::connectionCount() const
{
    return m_connectionCount.load(memory_order_relaxed);
}

int EventLoop::processTimeouts()
{
    vector<Channel*> expired;
    for (const auto& item : m_channelMap)
    {
        Channel* channel = item.second;
        if (channel->timeoutCallback && channel->timeoutCallback(
            const_cast<void*>(channel->getArg())) != 0)
        {
            expired.push_back(channel);
        }
    }
    for (Channel* channel : expired)
    {
        addTask(channel, ElemType::DELETE);
    }
    return static_cast<int>(expired.size());
}

int EventLoop::eventActive(int fd, int event)
{
    if (fd < 0)
    {
        return -1;
    }
    // 取出channel
    auto item = m_channelMap.find(fd);
    if (item == m_channelMap.end())
    {
        return -1;
    }
    Channel* channel = item->second;
    if (event & (int)FDEvent::ReadEvent && channel->readCallback)
    {
        auto callback = channel->readCallback;
        void* arg = const_cast<void*>(channel->getArg());
        callback(arg);
    }

    // The read callback may close and destroy the connection. Look it up
    // again before dispatching a write event from the same readiness record.
    item = m_channelMap.find(fd);
    if (item != m_channelMap.end() && event & (int)FDEvent::WriteEvent &&
        item->second->writeCallback)
    {
        channel = item->second;
        auto callback = channel->writeCallback;
        void* arg = const_cast<void*>(channel->getArg());
        callback(arg);
    }
    return 0;
}

int EventLoop::addTask(Channel* channel, ElemType type)
{
    // 加锁, 保护共享资源
    m_mutex.lock();
    // 创建新节点
    ChannelElement* node = new ChannelElement;
    node->channel = channel;
    node->type = type;
    m_taskQ.push(node);
    m_mutex.unlock();
    // 处理节点
    /*
    * 细节:
    *   1. 对于链表节点的添加: 可能是当前线程也可能是其他线程(主线程)
    *       1). 修改fd的事件, 当前子线程发起, 当前子线程处理
    *       2). 添加新的fd, 添加任务节点的操作是由主线程发起的
    *   2. 不能让主线程处理任务队列, 需要由当前的子线程取处理
    */
    if (m_threadID == this_thread::get_id())
    {
        // 当前子线程(基于子线程的角度分析)
        processTaskQ();
    }
    else
    {
        // 主线程 -- 告诉子线程处理任务队列中的任务
        // 1. 子线程在工作 2. 子线程被阻塞了:select, poll, epoll
        taskWakeup();
    }
    return 0;
}

int EventLoop::queueInLoop(function<void()> fn)
{
    m_mutex.lock();
    m_taskFnQ.push(std::move(fn));
    m_mutex.unlock();
    if (m_threadID == this_thread::get_id())
    {
        processTaskQ();
    }
    else
    {
        taskWakeup();
    }
    return 0;
}

void EventLoop::setIdleCallback(function<void()> fn)
{
    m_idleCallback = std::move(fn);
}

int EventLoop::processTaskQ()
{
    while (true)
    {
        ChannelElement* node = nullptr;
        function<void()> fn;
        {
            lock_guard<mutex> lock(m_mutex);
            if (m_taskQ.empty() && m_taskFnQ.empty())
            {
                break;
            }
            if (!m_taskQ.empty())
            {
                node = m_taskQ.front();
                m_taskQ.pop();
            }
            else
            {
                fn = std::move(m_taskFnQ.front());
                m_taskFnQ.pop();
            }
        }
        if (node != nullptr)
        {
            Channel* channel = node->channel;
            if (node->type == ElemType::ADD)
            {
                // 添加
                add(channel);
            }
            else if (node->type == ElemType::DELETE)
            {
                // 删除
                remove(channel);
            }
            else if (node->type == ElemType::MODIFY)
            {
                // 修改
                modify(channel);
            }
            delete node;
        }
        else
        {
            fn();
        }
    }
    return 0;
}

int EventLoop::removeFd(int fd)
{
    auto item = m_channelMap.find(fd);
    if (item == m_channelMap.end())
    {
        return -1;
    }
    return remove(item->second);
}

int EventLoop::add(Channel* channel)
{
    int fd = channel->getSocket();
    // 找到fd对应的数组元素位置, 并存储
    if (m_channelMap.find(fd) == m_channelMap.end())
    {
        m_channelMap.insert(make_pair(fd, channel));
        m_dispatcher->setChannel(channel);
        int ret = m_dispatcher->add();
        return ret;
    }
    return -1;
}

int EventLoop::remove(Channel* channel)
{
    int fd = channel->getSocket();
    if (m_channelMap.find(fd) == m_channelMap.end())
    {
        return -1;
    }
    m_dispatcher->setChannel(channel);
    int ret = m_dispatcher->remove();
    return ret;
}

int EventLoop::modify(Channel* channel)
{
    int fd = channel->getSocket();
    if (m_channelMap.find(fd) == m_channelMap.end())
    {
        return -1;
    }
    m_dispatcher->setChannel(channel);
    int ret = m_dispatcher->modify();
    return ret;
}

int EventLoop::readLocalMessage(void* arg)
{
    EventLoop* evLoop = static_cast<EventLoop*>(arg);
    char buf[256];
    read(evLoop->m_socketPair[1], buf, sizeof(buf));
    return 0;
}

void EventLoop::taskWakeup()
{
    const char* msg = "我是要成为海贼王的男人!!!";
    write(m_socketPair[0], msg, strlen(msg));
}

int EventLoop::freeChannel(Channel* channel)
{
    // 删除 channel 和 fd 的对应关系
    auto it = m_channelMap.find(channel->getSocket());
    if (it != m_channelMap.end())
    {
        m_channelMap.erase(it);
        close(channel->getSocket());
        delete channel;
    }
    return 0;
}

int EventLoop::readMessage()
{
    char buf[256];
    read(m_socketPair[1], buf, sizeof(buf));
    return 0;
}
