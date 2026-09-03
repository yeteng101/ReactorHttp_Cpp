#pragma once
#include "Buffer.h"
#include <stdbool.h>
#include "HttpResponse.h"
#include <map>
using namespace std;

// 当前的解析状态
enum class PrecessState:char
{
    ParseReqLine,
    ParseReqHeaders,
    ParseReqBody,
    ParseReqDone
};
// 定义http请求结构体
class HttpRequest
{
public:
    HttpRequest();
    ~HttpRequest();
    // 重置
    void reset();
    // 添加请求头
    void addHeader(const string key, const string value);
    // 根据key得到请求头的value
    string getHeader(const string key);
    // 解析请求行
    bool parseRequestLine(Buffer* readBuf);
    // 解析请求头
    bool parseRequestHeader(Buffer* readBuf);
    // 解析请求体
    bool parseRequestBody(Buffer* readBuf);
    // 解析http请求协议
    bool parseHttpRequest(Buffer* readBuf, HttpResponse* response, Buffer* sendBuf, int socket);
    // 处理http请求协议
    bool processHttpRequest(HttpResponse* response);
    bool shouldKeepAlive() const;
    // 解码字符串
    bool decodeMsg(const string& from, string& decoded);
    // 获取请求体
    inline const string& getBody() const
    {
        return m_body;
    }
    const string getFileType(const string name);
    inline void setMethod(string method)
    {
        m_method = method;
    }
    inline void seturl(string url)
    {
        m_url = url;
    }
    inline void setVersion(string version)
    {
        m_version = version;
    }
    // 强制本次响应结束后关闭连接（连接数上限、每连接请求上限、停机排空时使用）
    inline void setForceClose(bool forceClose)
    {
        m_forceClose = forceClose;
    }
    // 获取处理状态
    inline PrecessState getState()
    {
        return m_curState;
    }
    inline void setState(PrecessState state)
    {
        m_curState = state;
    }

private:
    int hexToDec(char c);

private:
    string m_method;
    string m_url;
    string m_version;
    map<string, string> m_reqHeaders;
    string m_body;
    uint64_t m_bodyExpected = 0;
    bool m_bodyTooLarge = false;
    PrecessState m_curState;
    bool m_parseError;
    bool m_keepAlive;
    bool m_forceClose = false;
};

