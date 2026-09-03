#include "Buffer.h"
#include "DriveServer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "ServerContext.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace
{
int failures = 0;

void fail(const std::string& message)
{
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string execute(ServerContext& context, const std::string& rawRequest)
{
    Buffer readBuffer(1024);
    Buffer writeBuffer(4096);
    HttpRequest parser;
    HttpResponse response;
    parser.setContext(&context);
    readBuffer.appendString(rawRequest.data(), static_cast<int>(rawRequest.size()));
    if (!parser.parseHttpRequest(&readBuffer, &response, &writeBuffer, -1))
    {
        fail("request did not complete: " + rawRequest.substr(0, 80));
        return std::string();
    }
    return std::string(writeBuffer.data(), writeBuffer.readableSize());
}

bool startsWith(const std::string& value, const std::string& prefix)
{
    return value.rfind(prefix, 0) == 0;
}

std::string headerValue(const std::string& response, const std::string& name)
{
    const std::string marker = name + ": ";
    const std::size_t pos = response.find(marker);
    if (pos == std::string::npos)
    {
        return "";
    }
    const std::size_t end = response.find("\r\n", pos + marker.size());
    return response.substr(pos + marker.size(), end - pos - marker.size());
}

void expectStatus(ServerContext& context, const std::string& raw, int status,
    const std::string& label)
{
    const std::string response = execute(context, raw);
    if (!startsWith(response, "HTTP/1.1 " + std::to_string(status) + " "))
    {
        fail(label + ": expected " + std::to_string(status) + " but got:\n" + response);
    }
}

std::string login(ServerContext& context, const std::string& username,
    const std::string& password)
{
    const std::string body = "username=" + username + "&password=" + password;
    const std::string raw =
        "POST /api/login HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    const std::string response = execute(context, raw);
    if (!startsWith(response, "HTTP/1.1 200 "))
    {
        fail("login " + username + " failed:\n" + response);
        return "";
    }
    const std::string marker = "Set-Cookie: sid=";
    const std::size_t pos = response.find(marker);
    if (pos == std::string::npos)
    {
        fail("login response missing Set-Cookie sid:\n" + response);
        return "";
    }
    const std::size_t start = pos + marker.size();
    const std::size_t end = response.find(';', start);
    return response.substr(start, end - start);
}

std::string authHeader(const std::string& token)
{
    return "Cookie: sid=" + token + "\r\n";
}

std::string fileContents(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

struct TempDir
{
    std::string path;

    TempDir()
    {
        char pattern[] = "/tmp/reactor-drive-test-XXXXXX";
        char* created = mkdtemp(pattern);
        if (created == nullptr)
        {
            std::cerr << "mkdtemp failed\n";
            std::exit(EXIT_FAILURE);
        }
        path = created;
    }
    ~TempDir()
    {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

void expectBodyContains(const std::string& response, const std::string& needle,
    const std::string& label)
{
    if (response.find(needle) == std::string::npos)
    {
        fail(label + ": response missing '" + needle + "':\n" + response);
    }
}
}

int main()
{
    TempDir temp;
    const std::string driveRoot = temp.path + "/drive";
    const std::string usersFile = temp.path + "/users.conf";
    fs::create_directories(driveRoot);
    {
        std::ofstream seed(usersFile);
    }

    ServerContext context;
    context.driveRoot = driveRoot;
    context.usersFile = usersFile;
    context.driveEnabled = true;
    std::string storeError;
    if (!context.users.load(usersFile, storeError) ||
        !context.users.create("author", "secret123", storeError) ||
        !context.users.create("reader", "reader123", storeError))
    {
        std::cerr << "user store setup failed: " << storeError << '\n';
        return EXIT_FAILURE;
    }

    // 未登录访问受保护接口 → 401
    expectStatus(context,
        "GET /api/me HTTP/1.1\r\nHost: localhost\r\n\r\n", 401, "me without login");
    expectStatus(context,
        "GET /api/drive/list HTTP/1.1\r\nHost: localhost\r\n\r\n", 401,
        "list without login");

    // 错误口令 → 401
    expectStatus(context,
        "POST /api/login HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: 34\r\n\r\nusername=author&password=wrongpass",
        401, "login with wrong password");

    const std::string token = login(context, "author", "secret123");
    if (token.empty())
    {
        return EXIT_FAILURE;
    }
    const std::string auth = authHeader(token);

    // /api/me
    {
        const std::string response = execute(context,
            "GET /api/me HTTP/1.1\r\nHost: localhost\r\n" + auth + "\r\n");
        expectBodyContains(response, "\"username\":\"author\"", "/api/me");
    }

    // 路径穿越必须被拒绝
    expectStatus(context,
        "GET /api/drive/list?path=..%2F.. HTTP/1.1\r\nHost: localhost\r\n" + auth + "\r\n",
        403, "path traversal");

    // 创建目录
    expectStatus(context,
        "POST /api/drive/mkdir?path=docs HTTP/1.1\r\nHost: localhost\r\n" + auth +
        "Content-Length: 0\r\n\r\n",
        200, "mkdir docs");

    // 分片上传：第 1 片
    expectBodyContains(execute(context,
        "PATCH /api/drive/file/docs/hello.txt HTTP/1.1\r\nHost: localhost\r\n" + auth +
        "Upload-Offset: 0\r\nContent-Length: 5\r\nContent-Type: application/octet-stream\r\n\r\nhello"),
        "\"size\":5", "upload chunk 1");
    // 第 2 片（续传 offset=5）
    expectBodyContains(execute(context,
        "PATCH /api/drive/file/docs/hello.txt HTTP/1.1\r\nHost: localhost\r\n" + auth +
        "Upload-Offset: 5\r\nContent-Length: 6\r\nContent-Type: application/octet-stream\r\n\r\n world"),
        "\"size\":11", "upload chunk 2");
    if (fileContents(driveRoot + "/author/docs/hello.txt") != "hello world")
    {
        fail("uploaded file content mismatch");
    }

    // stat
    {
        const std::string response = execute(context,
            "GET /api/drive/stat?path=docs/hello.txt HTTP/1.1\r\nHost: localhost\r\n" + auth + "\r\n");
        expectBodyContains(response, "\"size\":11", "stat size");
    }

    // list 根目录应包含 docs，docs 内应包含 hello.txt
    {
        const std::string rootList = execute(context,
            "GET /api/drive/list HTTP/1.1\r\nHost: localhost\r\n" + auth + "\r\n");
        expectBodyContains(rootList, "\"name\":\"docs\"", "root list docs");
        const std::string docsList = execute(context,
            "GET /api/drive/list?path=docs HTTP/1.1\r\nHost: localhost\r\n" + auth + "\r\n");
        expectBodyContains(docsList, "\"name\":\"hello.txt\"", "docs list hello.txt");
    }

    // Range 视频/流式响应
    {
        const std::string response = execute(context,
            "GET /api/drive/stream?path=docs/hello.txt HTTP/1.1\r\nHost: localhost\r\n" + auth +
            "Range: bytes=0-4\r\n\r\n");
        if (!startsWith(response, "HTTP/1.1 206 "))
        {
            fail("stream range expected 206:\n" + response);
        }
        if (headerValue(response, "Content-Range") != "bytes 0-4/11" ||
            headerValue(response, "Content-Length") != "5")
        {
            fail("stream range headers invalid:\n" + response);
        }
    }

    // 下载响应头
    {
        const std::string response = execute(context,
            "GET /api/drive/download?path=docs/hello.txt HTTP/1.1\r\nHost: localhost\r\n" + auth + "\r\n");
        if (!startsWith(response, "HTTP/1.1 200 ") ||
            headerValue(response, "Content-Disposition").find("attachment") == std::string::npos)
        {
            fail("download headers invalid:\n" + response);
        }
    }

    // 覆盖更新（同名字段内容变短，X-Overwrite 从 0 重传）
    expectBodyContains(execute(context,
        "PATCH /api/drive/file/docs/hello.txt HTTP/1.1\r\nHost: localhost\r\n" + auth +
        "Upload-Offset: 0\r\nX-Overwrite: 1\r\nContent-Length: 2\r\n\r\nhi"),
        "\"size\":2", "overwrite file");
    if (fileContents(driveRoot + "/author/docs/hello.txt") != "hi")
    {
        fail("overwritten file content mismatch");
    }

    // 重命名
    expectStatus(context,
        "POST /api/drive/rename?from=docs/hello.txt&to=docs/greeting.txt "
        "HTTP/1.1\r\nHost: localhost\r\n" + auth + "Content-Length: 0\r\n\r\n",
        200, "rename file");

    // 递归删除目录
    expectStatus(context,
        "PATCH /api/drive/file/docs/keep/inside/a.txt HTTP/1.1\r\nHost: localhost\r\n" + auth +
        "Upload-Offset: 0\r\nContent-Length: 3\r\n\r\nabc",
        200, "upload nested");
    expectStatus(context,
        "DELETE /api/drive/remove?path=docs/keep HTTP/1.1\r\nHost: localhost\r\n" + auth + "\r\n",
        200, "recursive delete");
    if (fs::exists(driveRoot + "/author/docs/keep"))
    {
        fail("directory was not removed recursively");
    }

    // 其他用户看不到 author 的文件（家目录隔离）
    const std::string readerToken = login(context, "reader", "reader123");
    if (readerToken.empty())
    {
        return EXIT_FAILURE;
    }
    const std::string readerList = execute(context,
        "GET /api/drive/list HTTP/1.1\r\nHost: localhost\r\nCookie: sid=" + readerToken + "\r\n\r\n");
    if (readerList.find("docs") != std::string::npos ||
        readerList.find("greeting.txt") != std::string::npos)
    {
        fail("reader must not see author files:\n" + readerList);
    }

    // 注销后 token 失效
    expectStatus(context,
        "POST /api/logout HTTP/1.1\r\nHost: localhost\r\n" + auth + "\r\n", 200,
        "logout");
    expectStatus(context,
        "GET /api/me HTTP/1.1\r\nHost: localhost\r\n" + auth + "\r\n", 401,
        "me after logout");

    if (failures != 0)
    {
        std::cerr << failures << " drive test failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "drive tests passed\n";
    return EXIT_SUCCESS;
}
