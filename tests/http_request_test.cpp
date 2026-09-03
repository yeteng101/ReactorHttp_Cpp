#include "Buffer.h"
#include "Config.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "ThreadPool.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace
{
std::string executeRequest(const std::string& request)
{
    Buffer readBuffer(256);
    Buffer writeBuffer(256);
    HttpRequest parser;
    HttpResponse response;
    readBuffer.appendString(request.data(), static_cast<int>(request.size()));
    if (!parser.parseHttpRequest(&readBuffer, &response, &writeBuffer, -1))
    {
        std::cerr << "request was not completed: " << request << '\n';
        std::exit(EXIT_FAILURE);
    }
    return std::string(writeBuffer.data(), writeBuffer.readableSize());
}

void expectStatus(const std::string& request, int status)
{
    const std::string response = executeRequest(request);
    const std::string expected = "HTTP/1.1 " + std::to_string(status) + " ";
    if (response.rfind(expected, 0) != 0)
    {
        std::cerr << "expected status " << status << ", got:\n" << response << '\n';
        std::exit(EXIT_FAILURE);
    }
    if (response.find("Content-Length:") == std::string::npos ||
        response.find("Connection:") == std::string::npos)
    {
        std::cerr << "required response headers are missing:\n" << response << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void expectContains(const std::string& request, const std::string& value)
{
    const std::string response = executeRequest(request);
    if (response.find(value) == std::string::npos)
    {
        std::cerr << "expected response to contain " << value << ":\n" << response << '\n';
        std::exit(EXIT_FAILURE);
    }
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

void expectRange(const std::string& range, int status, const std::string& contentRange,
    const std::string& expectedLength)
{
    const std::string response = executeRequest(
        "GET /index.html HTTP/1.1\r\nHost: localhost\r\nRange: " + range + "\r\n\r\n");
    const std::string expected = "HTTP/1.1 " + std::to_string(status) + " ";
    if (response.rfind(expected, 0) != 0)
    {
        std::cerr << "expected status " << status << " for Range: " << range << ", got:\n"
                  << response << '\n';
        std::exit(EXIT_FAILURE);
    }
    if (!contentRange.empty() && headerValue(response, "Content-Range") != contentRange)
    {
        std::cerr << "wrong Content-Range for Range: " << range << ", got:\n"
                  << response << '\n';
        std::exit(EXIT_FAILURE);
    }
    if (!expectedLength.empty() &&
        headerValue(response, "Content-Length") != expectedLength)
    {
        std::cerr << "wrong Content-Length for Range: " << range << ", got:\n"
                  << response << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct TempFileGuard
{
    const char* path;

    TempFileGuard(const char* p) : path(p)
    {
        unlink(path);
    }
    ~TempFileGuard()
    {
        unlink(path);
    }
};
}

int main()
{
    if (chdir("public") != 0)
    {
        perror("chdir public");
        return EXIT_FAILURE;
    }

    expectStatus("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n", 200);
    expectStatus("GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n", 200);
    expectStatus("GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n", 200);
    expectStatus("GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n", 200);
    expectStatus("GET /api/files HTTP/1.1\r\nHost: localhost\r\n\r\n", 200);
    expectStatus("GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n", 404);
    expectStatus("POST /health HTTP/1.1\r\nHost: localhost\r\n\r\n", 405);
    expectStatus("OPTIONS /health HTTP/1.1\r\nHost: localhost\r\n\r\n", 204);
    expectStatus("GET /../../etc/passwd HTTP/1.1\r\nHost: localhost\r\n\r\n", 403);
    expectStatus("GET /%2e%2e/%2e%2e/etc/passwd HTTP/1.1\r\nHost: localhost\r\n\r\n", 403);
    expectStatus("GET /bad%zz HTTP/1.1\r\nHost: localhost\r\n\r\n", 400);
    expectStatus("not-http\r\n\r\n", 400);
    expectContains("GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n",
        "{\"status\":\"ok\"}");
    expectContains("GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n",
        "\"active_connections\":");
    expectContains("GET /api/files HTTP/1.1\r\nHost: localhost\r\n\r\n",
        "\"name\":\"index.html\"");

    // OPTIONS：204 + Allow + CORS 预检头
    {
        const std::string optionsResponse = executeRequest(
            "OPTIONS /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
        if (optionsResponse.rfind("HTTP/1.1 204 ", 0) != 0 ||
            headerValue(optionsResponse, "Content-Length") != "0" ||
            headerValue(optionsResponse, "Allow") != "GET, HEAD, OPTIONS, PATCH, DELETE" ||
            headerValue(optionsResponse, "Access-Control-Allow-Origin") != "*" ||
            headerValue(optionsResponse, "Access-Control-Allow-Methods") !=
                "GET, HEAD, OPTIONS, PATCH, DELETE")
        {
            std::cerr << "OPTIONS preflight response is invalid:\n"
                      << optionsResponse << '\n';
            return EXIT_FAILURE;
        }

        const std::string reflected = executeRequest(
            "OPTIONS /health HTTP/1.1\r\nHost: localhost\r\n"
            "Access-Control-Request-Headers: X-Test, Range\r\n\r\n");
        if (headerValue(reflected, "Access-Control-Allow-Headers") != "X-Test, Range")
        {
            std::cerr << "OPTIONS should echo Access-Control-Request-Headers:\n"
                      << reflected << '\n';
            return EXIT_FAILURE;
        }
    }

    expectContains("GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n",
        "Connection: keep-alive");
    expectContains("GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n",
        "Access-Control-Allow-Origin: *");
    expectContains("GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
        "Connection: close");
    expectContains("GET /health HTTP/1.0\r\nHost: localhost\r\n\r\n",
        "Connection: close");
    expectContains("GET /health HTTP/1.0\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n",
        "Connection: keep-alive");

    // Date 头（RFC 7231 要求）
    {
        const std::string response = executeRequest(
            "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
        const std::string dateMarker = "Date: ";
        const std::size_t datePos = response.find(dateMarker);
        if (datePos == std::string::npos)
        {
            std::cerr << "response is missing Date header:\n" << response << '\n';
            return EXIT_FAILURE;
        }
        if (response.find(" GMT\r\n", datePos + dateMarker.size()) == std::string::npos)
        {
            std::cerr << "Date header must end with GMT:\n" << response << '\n';
            return EXIT_FAILURE;
        }
    }

    // HEAD：状态码和 Content-Length 与 GET 相同，但没有正文
    {
        const std::string headResponse = executeRequest(
            "HEAD /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
        if (headResponse.rfind("HTTP/1.1 200 ", 0) != 0)
        {
            std::cerr << "HEAD /health should return 200:\n" << headResponse << '\n';
            return EXIT_FAILURE;
        }
        if (headResponse.find("Content-Length:") == std::string::npos ||
            headResponse.find("{\"status\":\"ok\"}") != std::string::npos)
        {
            std::cerr << "HEAD must have Content-Length but no body:\n"
                      << headResponse << '\n';
            return EXIT_FAILURE;
        }

        const std::string fileHead = executeRequest(
            "HEAD /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n");
        const std::string fileGet = executeRequest(
            "GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n");
        const std::string lengthMarker = "Content-Length: ";
        const std::size_t fileHeadLength = fileHead.find(lengthMarker);
        const std::size_t fileGetLength = fileGet.find(lengthMarker);
        if (fileHeadLength == std::string::npos || fileGetLength == std::string::npos ||
            fileHead.substr(fileHeadLength + lengthMarker.size()) !=
                fileGet.substr(fileGetLength + lengthMarker.size()))
        {
            std::cerr << "HEAD and GET must report the same Content-Length\n"
                      << "HEAD:\n" << fileHead << "\nGET:\n" << fileGet << '\n';
            return EXIT_FAILURE;
        }
        if (fileHead.find("<html>") != std::string::npos)
        {
            std::cerr << "HEAD must not send a body:\n" << fileHead << '\n';
            return EXIT_FAILURE;
        }
    }

    // MIME：扩展名映射与未知类型回退
    {
        HttpRequest parser;
        if (parser.getFileType("photo.PNG") != "image/png" ||
            parser.getFileType("archive.tar.gz") != "application/gzip" ||
            parser.getFileType("script.js") != "text/javascript; charset=utf-8" ||
            parser.getFileType("sample.docx") !=
                "application/vnd.openxmlformats-officedocument.wordprocessingml.document" ||
            parser.getFileType("archive.unknown") != "application/octet-stream")
        {
            std::cerr << "MIME extension mapping failed\n";
            return EXIT_FAILURE;
        }
    }

    // Range 断点续传：单段 bytes 范围、后缀范围、越界 416
    {
        const std::string fullResponse = executeRequest(
            "GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n");
        const std::string totalLength = headerValue(fullResponse, "Content-Length");
        if (totalLength.empty() || headerValue(fullResponse, "Accept-Ranges") != "bytes")
        {
            std::cerr << "static file must advertise Accept-Ranges: bytes\n"
                      << fullResponse << '\n';
            return EXIT_FAILURE;
        }
        const uint64_t total = std::stoull(totalLength);
        if (total < 11)
        {
            std::cerr << "index.html is too small for range tests\n";
            return EXIT_FAILURE;
        }
        const std::string lastStart = std::to_string(total - 4);
        const std::string lastEnd = std::to_string(total - 1);
        const std::string suffixStart = std::to_string(total - 10);

        expectRange("bytes=0-9", 206, "bytes 0-9/" + totalLength, "10");
        expectRange("bytes=" + lastStart + "-", 206,
            "bytes " + lastStart + "-" + lastEnd + "/" + totalLength, "4");
        expectRange("bytes=-10", 206,
            "bytes " + suffixStart + "-" + lastEnd + "/" + totalLength, "10");
        expectRange("bytes=999999999-", 416, "bytes */" + totalLength, "");

        const std::string multiRange = executeRequest(
            "GET /index.html HTTP/1.1\r\nHost: localhost\r\n"
            "Range: bytes=0-1,5-6\r\n\r\n");
        if (multiRange.rfind("HTTP/1.1 200 ", 0) != 0 ||
            headerValue(multiRange, "Content-Length") != totalLength)
        {
            std::cerr << "multi-range should fall back to full 200 response:\n"
                      << multiRange << '\n';
            return EXIT_FAILURE;
        }

        const std::string headRange = executeRequest(
            "HEAD /index.html HTTP/1.1\r\nHost: localhost\r\n"
            "Range: bytes=0-9\r\n\r\n");
        if (headRange.rfind("HTTP/1.1 206 ", 0) != 0 ||
            headerValue(headRange, "Content-Range") != "bytes 0-9/" + totalLength ||
            headerValue(headRange, "Content-Length") != "10")
        {
            std::cerr << "HEAD with Range must report 206 and partial length:\n"
                      << headRange << '\n';
            return EXIT_FAILURE;
        }
    }

    // PATCH/DELETE 文件 API：分片续传写文件 + 删除文件
    {
        const char* uploadName = ".test-upload.bin";
        TempFileGuard guard(uploadName);

        const std::string missingPatch =
            "PATCH /api/files/.test-upload.bin HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Upload-Offset: 5\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "hello";
        const std::string missingResponse = executeRequest(missingPatch);
        if (missingResponse.rfind("HTTP/1.1 404 ", 0) != 0)
        {
            std::cerr << "PATCH missing file with nonzero offset should be 404:\n"
                      << missingResponse << '\n';
            return EXIT_FAILURE;
        }

        expectStatus(
            "PATCH /api/files/.test-upload.bin HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Upload-Offset: 0\r\n"
            "Content-Length: 67108865\r\n\r\n",
            413);

        const std::string patchFirst =
            "PATCH /api/files/.test-upload.bin HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Upload-Offset: 0\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "hello";
        const std::string firstResponse = executeRequest(patchFirst);
        if (firstResponse.rfind("HTTP/1.1 200 ", 0) != 0 ||
            firstResponse.find("\"size\":5") == std::string::npos ||
            headerValue(firstResponse, "Upload-Offset") != "5")
        {
            std::cerr << "PATCH first chunk failed:\n" << firstResponse << '\n';
            return EXIT_FAILURE;
        }
        {
            std::ifstream input(uploadName, std::ios::binary);
            const std::string content((std::istreambuf_iterator<char>(input)),
                std::istreambuf_iterator<char>());
            if (content != "hello")
            {
                std::cerr << "PATCH first chunk wrote wrong content\n";
                return EXIT_FAILURE;
            }
        }

        const std::string patchSecond =
            "PATCH /api/files/.test-upload.bin HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Upload-Offset: 5\r\n"
            "Content-Length: 6\r\n"
            "\r\n"
            " world";
        const std::string secondResponse = executeRequest(patchSecond);
        if (secondResponse.rfind("HTTP/1.1 200 ", 0) != 0 ||
            secondResponse.find("\"size\":11") == std::string::npos ||
            headerValue(secondResponse, "Upload-Offset") != "11")
        {
            std::cerr << "PATCH second chunk failed:\n" << secondResponse << '\n';
            return EXIT_FAILURE;
        }
        {
            std::ifstream input(uploadName, std::ios::binary);
            const std::string content((std::istreambuf_iterator<char>(input)),
                std::istreambuf_iterator<char>());
            if (content != "hello world")
            {
                std::cerr << "PATCH second chunk wrote wrong content\n";
                return EXIT_FAILURE;
            }
        }

        const std::string duplicateResponse = executeRequest(patchSecond);
        if (duplicateResponse.rfind("HTTP/1.1 200 ", 0) != 0 ||
            duplicateResponse.find("\"size\":11") == std::string::npos)
        {
            std::cerr << "PATCH duplicate chunk should be idempotent:\n"
                      << duplicateResponse << '\n';
            return EXIT_FAILURE;
        }

        const std::string conflictPatch =
            "PATCH /api/files/.test-upload.bin HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Upload-Offset: 99\r\n"
            "Content-Length: 1\r\n"
            "\r\n"
            "x";
        const std::string conflictResponse = executeRequest(conflictPatch);
        if (conflictResponse.rfind("HTTP/1.1 409 ", 0) != 0)
        {
            std::cerr << "PATCH out-of-range offset should be 409:\n"
                      << conflictResponse << '\n';
            return EXIT_FAILURE;
        }

        const std::string deleteResponse = executeRequest(
            "DELETE /api/files/.test-upload.bin HTTP/1.1\r\nHost: localhost\r\n\r\n");
        if (deleteResponse.rfind("HTTP/1.1 204 ", 0) != 0)
        {
            std::cerr << "DELETE existing file should return 204:\n"
                      << deleteResponse << '\n';
            return EXIT_FAILURE;
        }
        {
            std::ifstream input(uploadName, std::ios::binary);
            if (input)
            {
                std::cerr << "DELETE did not remove the file\n";
                return EXIT_FAILURE;
            }
        }

        const std::string deleteMissing = executeRequest(
            "DELETE /api/files/.test-upload.bin HTTP/1.1\r\nHost: localhost\r\n\r\n");
        if (deleteMissing.rfind("HTTP/1.1 404 ", 0) != 0)
        {
            std::cerr << "DELETE missing file should return 404:\n"
                      << deleteMissing << '\n';
            return EXIT_FAILURE;
        }
    }

    // 强制关闭（连接数上限 / 每连接请求上限 / 停机排空时使用）
    {
        Buffer readBuffer(256);
        Buffer writeBuffer(256);
        HttpRequest parser;
        HttpResponse response;
        readBuffer.appendString("GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
        parser.setForceClose(true);
        if (!parser.parseHttpRequest(&readBuffer, &response, &writeBuffer, -1))
        {
            std::cerr << "forced-close request was not completed\n";
            return EXIT_FAILURE;
        }
        const std::string responseData(writeBuffer.data(), writeBuffer.readableSize());
        if (responseData.find("Connection: close") == std::string::npos)
        {
            std::cerr << "forced-close response must say Connection: close:\n"
                      << responseData << '\n';
            return EXIT_FAILURE;
        }
    }

    // 配置解析：旧式位置参数与 --flags 两种形式
    {
        ServerConfig legacy;
        char* legacyArgs[] = {const_cast<char*>("reactor-http"),
            const_cast<char*>("18080"), const_cast<char*>("./public"),
            const_cast<char*>("2"), const_cast<char*>("8")};
        if (!parseConfig(5, legacyArgs, legacy) ||
            legacy.port != 18080 || legacy.minWorkers != 2 || legacy.maxWorkers != 8 ||
            legacy.webRoot != "./public")
        {
            std::cerr << "legacy positional config parse failed\n";
            return EXIT_FAILURE;
        }

        ServerConfig flags;
        char* flagArgs[] = {const_cast<char*>("reactor-http"),
            const_cast<char*>("--port"), const_cast<char*>("9090"),
            const_cast<char*>("--root"), const_cast<char*>("/srv/www"),
            const_cast<char*>("--min-workers"), const_cast<char*>("1"),
            const_cast<char*>("--max-workers"), const_cast<char*>("4"),
            const_cast<char*>("--max-connections"), const_cast<char*>("500"),
            const_cast<char*>("--idle-timeout"), const_cast<char*>("45"),
            const_cast<char*>("--access-log"), const_cast<char*>("-"),
            const_cast<char*>("--error-log"), const_cast<char*>("/var/log/http.err")};
        if (!parseConfig(17, flagArgs, flags) ||
            flags.port != 9090 || flags.webRoot != "/srv/www" ||
            flags.minWorkers != 1 || flags.maxWorkers != 4 ||
            flags.maxConnections != 500 || flags.idleTimeoutSeconds != 45 ||
            flags.errorLogPath != "/var/log/http.err")
        {
            std::cerr << "--flag config parse failed\n";
            return EXIT_FAILURE;
        }
    }

    Buffer pipelineRead(512);
    Buffer pipelineWrite(512);
    HttpRequest pipelineParser;
    HttpResponse pipelineResponse;
    const std::string pipeline =
        "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    pipelineRead.appendString(pipeline.data(), static_cast<int>(pipeline.size()));
    const bool firstPipelineResponse = pipelineParser.parseHttpRequest(
        &pipelineRead, &pipelineResponse, &pipelineWrite, -1);
    const bool secondPipelineResponse = pipelineParser.parseHttpRequest(
        &pipelineRead, &pipelineResponse, &pipelineWrite, -1);
    const std::string pipelineResponseData(pipelineWrite.data(), pipelineWrite.readableSize());
    if (!firstPipelineResponse || !secondPipelineResponse ||
        pipelineResponseData.find("HTTP/1.1") == std::string::npos ||
        pipelineResponseData.find("HTTP/1.1", pipelineResponseData.find("HTTP/1.1") + 1) == std::string::npos)
    {
        std::cerr << "HTTP pipelining test failed\n";
        return EXIT_FAILURE;
    }

    EventLoop mainLoop;
    ThreadPool pool(&mainLoop, 2, 3);
    pool.run();
    if (pool.workerCount() != 2)
    {
        std::cerr << "expected two initial workers\n";
        return EXIT_FAILURE;
    }
    EventLoop* busyLoop = pool.takeWorkerEventLoop();
    for (int i = 0; i < 64; ++i)
    {
        busyLoop->connectionOpened();
    }
    EventLoop* secondBusyLoop = pool.takeWorkerEventLoop();
    for (int i = 0; i < 64; ++i)
    {
        secondBusyLoop->connectionOpened();
    }
    pool.takeWorkerEventLoop();
    if (pool.workerCount() != 3)
    {
        std::cerr << "expected dynamic worker scale-up to three workers, got "
                  << pool.workerCount() << " (" << busyLoop->connectionCount() << ", "
                  << secondBusyLoop->connectionCount() << ")\n";
        return EXIT_FAILURE;
    }
    for (int i = 0; i < 64; ++i)
    {
        busyLoop->connectionClosed();
    }
    pool.stop();

    std::cout << "HTTP request tests passed\n";
    return EXIT_SUCCESS;
}
