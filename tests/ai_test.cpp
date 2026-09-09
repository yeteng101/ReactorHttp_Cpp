#include "AiClient.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <sys/stat.h>
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

struct TempDir
{
    std::string path;
    TempDir()
    {
        char pattern[] = "/tmp/reactor-ai-test-XXXXXX";
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
}

int main()
{
    TempDir temp;
    const std::string configPath = temp.path + "/ai-config.json";

    // 1) 文件不存在时使用默认值
    AiService service;
    std::string error;
    if (!service.load(configPath, error))
    {
        fail("load missing config failed: " + error);
        return EXIT_FAILURE;
    }
    AiConfig config = service.snapshot();
    if (!config.apiKey.empty() || config.model != "gpt-4o-mini")
    {
        fail("unexpected default config");
    }

    // 2) 更新并持久化，Key 只落盘不下发
    if (!service.update("http://127.0.0.1:1/v1", "test-model", "sk-abc123", true,
            false, false, error))
    {
        fail("update failed: " + error);
        return EXIT_FAILURE;
    }
    config = service.snapshot();
    if (config.model != "test-model" || config.apiKey != "sk-abc123")
    {
        fail("config not updated in memory");
    }
    struct stat fileStat;
    if (stat(configPath.c_str(), &fileStat) != 0 || (fileStat.st_mode & 0777) != 0600)
    {
        fail("config file permission is not 0600");
    }

    // 3) 重新加载后配置仍在（不再被 sidecar 环境变量覆盖）
    AiService reloaded;
    if (!reloaded.load(configPath, error))
    {
        fail("reload failed: " + error);
    }
    const AiConfig reloadedConfig = reloaded.snapshot();
    if (reloadedConfig.apiKey != "sk-abc123" || reloadedConfig.model != "test-model" ||
        reloadedConfig.baseUrl != "http://127.0.0.1:1/v1")
    {
        fail("persisted config mismatch");
    }

    // 4) 后台任务：连接被拒绝时任务应结束并带上错误
    std::string jobError;
    const std::string jobId = service.startJob("{}", jobError);
    if (jobId.empty())
    {
        fail("startJob failed: " + jobError);
    }
    else
    {
        bool done = false;
        // 连接超时上限是 10s（ConnectTimeoutMs），这里给 15s 预算
        for (int i = 0; i < 600 && !done; ++i)
        {
            const std::shared_ptr<AiJob> job = service.findJob(jobId);
            if (!job)
            {
                fail("job disappeared");
                break;
            }
            {
                std::lock_guard<std::mutex> lock(job->mutex);
                done = job->done;
                if (done)
                {
                    if (job->ok)
                    {
                        fail("job unexpectedly succeeded against unreachable host");
                    }
                    if (job->error.empty())
                    {
                        fail("job finished without error message");
                    }
                }
            }
            if (!done)
            {
                // 不要持锁睡眠：否则后台线程可能一直抢不到锁，导致误报超时
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        }
        if (!done)
        {
            fail("job did not finish in time");
        }
    }

    if (failures != 0)
    {
        std::cerr << failures << " ai test failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "ai tests passed\n";
    return EXIT_SUCCESS;
}
