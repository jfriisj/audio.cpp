#pragma once

#include "http.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace minitts::server {

// A single background worker owns all queued generations. The callable executes
// the existing /v1/tasks/run handler without holding a client HTTP connection.
class GenerationJobs {
public:
    using Runner = std::function<HttpResponse(const std::string &)>;

    GenerationJobs(std::filesystem::path directory, Runner runner);
    ~GenerationJobs();

    GenerationJobs(const GenerationJobs &) = delete;
    GenerationJobs & operator=(const GenerationJobs &) = delete;

    HttpResponse handle(const HttpRequest & request);

private:
    struct Job {
        std::string id;
        std::string status = "queued";
        std::string model;
        std::string request_json;
        std::string error;
        std::int64_t created_at_ms = 0;
        std::int64_t started_at_ms = 0;
        std::int64_t finished_at_ms = 0;
        int output_count = 0;
    };

    std::filesystem::path directory_;
    Runner runner_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, Job> jobs_;
    std::deque<std::string> queue_;
    std::thread worker_;
    bool stopping_ = false;

    void load();
    void persist(const Job & job) const;
    void run();
    void execute(const std::string & id);
    static std::string to_json(const Job & job);
    HttpResponse list();
    HttpResponse get(const std::string & id);
    HttpResponse audio(const std::string & id, unsigned int index);
    HttpResponse submit(const std::string & request_json);
    HttpResponse retry(const std::string & id);
};

} // namespace minitts::server
