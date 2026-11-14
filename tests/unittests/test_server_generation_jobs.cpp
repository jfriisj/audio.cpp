#include "generation_jobs.h"
#include "server_protocol.h"
#include "../../app/common/base64.h"
#include "engine/framework/io/json.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace minitts::server {
// Test-only protocol helpers: the production versions live in http.cpp and
// server_protocol.cpp. Keep the queue test independent of CUDA and model loading.
std::string json_quote(std::string_view text) {
    return engine::io::json::stringify_string(text);
}
HttpResponse json_response(std::string body, int status) {
    HttpResponse result;
    result.status = status;
    result.body = std::move(body);
    return result;
}
HttpResponse error_response(int status, const std::string &message, const std::string &type) {
    return json_response("{\"error\":{\"message\":" + json_quote(message) +
                         ",\"type\":" + json_quote(type) + "}}", status);
}
} // namespace minitts::server

using namespace minitts::server;
using engine::io::json::parse;

namespace {
HttpRequest request(std::string method, std::string path, std::string body = {}) {
    HttpRequest result;
    result.method = std::move(method);
    result.path = std::move(path);
    result.body = std::move(body);
    return result;
}

std::string sample_result() {
    std::vector<uint8_t> wav(44, 0);
    wav[0] = 'R'; wav[1] = 'I'; wav[2] = 'F'; wav[3] = 'F';
    wav[8] = 'W'; wav[9] = 'A'; wav[10] = 'V'; wav[11] = 'E';
    return "{\"audio\":" + json_quote(minitts::app::base64_encode(wav)) + "}";
}

std::string job_status(GenerationJobs &jobs, const std::string &id) {
    return parse(jobs.handle(request("GET", "/v1/tasks/jobs/" + id)).body)
        .require("status").as_string();
}

void wait_for(GenerationJobs &jobs, const std::string &id, const std::string &status) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (job_status(jobs, id) == status) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(false && "job did not reach expected status");
}
}

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("audiocpp-jobtest-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    std::string id;

    {
        GenerationJobs jobs(root, [](const std::string &) {
            return json_response(sample_result());
        });
        const auto submitted = jobs.handle(request("POST", "/v1/tasks/jobs",
            R"({"model":"fixture","request":{"text":"Nordic music","seed":42}})"));
        assert(submitted.status == 202);
        id = parse(submitted.body).require("id").as_string();
        wait_for(jobs, id, "completed");

        assert(std::filesystem::exists(root / (id + "-0.wav")));
        const auto wav = jobs.handle(request("GET", "/v1/tasks/jobs/" + id + "/audio/0"));
        assert(wav.status == 200);
        assert(wav.content_type == "audio/wav");
        assert(wav.body.size() == 44);
        assert(jobs.handle(request("GET", "/v1/tasks/jobs/../../etc/passwd")).status == 404);
        assert(jobs.handle(request("POST", "/v1/tasks/jobs",
            R"({"model":"fixture","request":{"audio":"/tmp/input.wav"}})")).status == 400);
        assert(parse(jobs.handle(request("GET", "/v1/tasks/jobs")).body)
            .require("data").as_array().size() == 1);
    }

    // Finished audio remains usable after process restart.
    {
        GenerationJobs jobs(root, [](const std::string &) {
            return error_response(503, "device busy", "server_busy");
        });
        assert(job_status(jobs, id) == "completed");
        assert(jobs.handle(request("GET", "/v1/tasks/jobs/" + id + "/audio/0")).status == 200);
    }

    // Simulate a process killed during inference; no automatic duplicate run.
    const auto metadata = root / (id + ".json");
    {
        auto data = parse(engine::io::json::parse_file(metadata).require("request_json").as_string());
        (void)data;
    }
    {
        const auto old = engine::io::json::parse_file(metadata);
        engine::io::json::Value::Object values = old.as_object();
        values["status"] = engine::io::json::Value::make_string("running");
        std::ofstream output(metadata, std::ios::binary | std::ios::trunc);
        output << engine::io::json::stringify(engine::io::json::Value::make_object(std::move(values)));
    }
    {
        GenerationJobs jobs(root, [](const std::string &) {
            return json_response(sample_result());
        });
        assert(job_status(jobs, id) == "interrupted");
        const auto retried = jobs.handle(request("POST", "/v1/tasks/jobs/" + id + "/retry"));
        assert(retried.status == 202);
        wait_for(jobs, id, "completed");
    }

    std::filesystem::remove_all(root);
    return 0;
}
