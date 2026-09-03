#include "Dispatcher.h"
#include <sys/epoll.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include "EpollDispatcher.h"
#include <errno.h>

EpollDispatcher::EpollDispatcher(EventLoop* evloop) : Dispatcher(evloop)
{
    m_epfd = epoll_create(10);
    if (m_epfd == -1)
    {
        perror("epoll_create");
        exit(0);
    }
    m_events = new struct epoll_event[m_maxNode];
    m_name = "Epoll";
}

EpollDispatcher::~EpollDispatcher()
{
    close(m_epfd);
    delete[]m_events;
}

int EpollDispatcher::add()
{
    int ret = epollCtl(EPOLL_CTL_ADD);
    if (ret == -1)
    {
        perror("epoll_crl add");
        exit(0);
    }
    return ret;
}

int EpollDispatcher::remove()
{
    int ret = epollCtl(EPOLL_CTL_DEL);
    if (ret == -1)
    {
        perror("epoll_crl delete");
        exit(0);
    }
    // 通过 channel 释放对应的 TcpConnection 资源
    m_channel->destroyCallback(const_cast<void*>(m_channel->getArg()));

    return ret;
}

int EpollDispatcher::modify()
{
    int ret = epollCtl(EPOLL_CTL_MOD);
    if (ret == -1)
    {
        perror("epoll_crl modify");
        exit(0);
    }
    return ret;
}

int EpollDispatcher::dispatch(int timeout)
{
    int count = epoll_wait(m_epfd, m_events, m_maxNode, timeout * 1000);
    if (count == -1)
    {
        return errno == EINTR ? 0 : -1;
    }
    for (int i = 0; i < count; ++i)
    {
        int events = m_events[i].events;
        int fd = m_events[i].data.fd;
        if (events & EPOLLERR)
        {
            m_evLoop->removeFd(fd);
            continue;
        }
        if (events & EPOLLIN)
        {
            m_evLoop->eventActive(fd, (int)FDEvent::ReadEvent);
        }
        if (events & EPOLLOUT)
        {
            m_evLoop->eventActive(fd, (int)FDEvent::WriteEvent);
        }
        if ((events & (EPOLLHUP | EPOLLRDHUP)) && !(events & EPOLLIN))
        {
            m_evLoop->removeFd(fd);
        }
    }
    return 0;
}

int EpollDispatcher::epollCtl(int op)
{
    struct epoll_event ev;
    ev.data.fd = m_channel->getSocket();
    int events = 0;
    if (m_channel->getEvent() & (int)FDEvent::ReadEvent)
    {
        events |= EPOLLIN;
    }
    if (m_channel->getEvent() & (int)FDEvent::WriteEvent)
    {
        events |= EPOLLOUT;
    }
    ev.events = events | EPOLLRDHUP;
    int ret = epoll_ctl(m_epfd, op, m_channel->getSocket(), &ev);
    return ret;
}
