#include "generation_jobs.h"

#include "server_protocol.h"
#include "../common/base64.h"
#include "engine/framework/io/json.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace minitts::server {
namespace {
using engine::io::json::Value;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool valid_id(const std::string &id) {
    return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

std::string new_id() {
    std::random_device random;
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i) {
        out << std::setw(8) << static_cast<std::uint32_t>(random());
    }
    return out.str();
}

void write_atomic(const std::filesystem::path &path, std::string_view data) {
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("cannot open generation file: " + temporary);
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!file) throw std::runtime_error("cannot write generation file: " + temporary);
    }
#ifdef _WIN32
    // std::filesystem::rename cannot replace an existing file on Windows.
    // POSIX retains an atomic rename for readers and restart recovery.
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
#endif
    std::filesystem::rename(temporary, path);
}

std::string response_error(const HttpResponse &response) {
    try {
        const auto data = engine::io::json::parse(response.body);
        if (const auto *error = data.find("error")) {
            if (const auto *message = error->find("message")) {
                return message->as_string();
            }
        }
    } catch (...) {}
    return "Generation failed (HTTP " + std::to_string(response.status) + ")";
}

std::string audio_filename(const std::string &id, int index) {
    return id + "-" + std::to_string(index) + ".wav";
}
} // namespace

GenerationJobs::GenerationJobs(std::filesystem::path directory, Runner runner)
    : directory_(std::filesystem::absolute(std::move(directory)).lexically_normal()),
      runner_(std::move(runner)) {
    std::filesystem::create_directories(directory_);
    load();
    worker_ = std::thread(&GenerationJobs::run, this);
}

GenerationJobs::~GenerationJobs() {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        stopping_ = true;
    }
    changed_.notify_all();
    if (worker_.joinable()) worker_.join();
}

std::string GenerationJobs::to_json(const Job &job) {
    std::ostringstream out;
    out << "{\"id\":" << json_quote(job.id)
        << ",\"status\":" << json_quote(job.status)
        << ",\"model\":" << json_quote(job.model)
        << ",\"request_json\":" << json_quote(job.request_json)
        << ",\"error\":" << json_quote(job.error)
        << ",\"created_at_ms\":" << job.created_at_ms
        << ",\"started_at_ms\":" << job.started_at_ms
        << ",\"finished_at_ms\":" << job.finished_at_ms
        << ",\"output_count\":" << job.output_count
        << ",\"audio_urls\":[";
    for (int i = 0; i < job.output_count; ++i) {
        if (i) out << ",";
        out << json_quote("/v1/tasks/jobs/" + job.id + "/audio/" + std::to_string(i));
    }
    out << "]}";
    return out.str();
}

void GenerationJobs::persist(const Job &job) const {
    write_atomic(directory_ / (job.id + ".json"), to_json(job));
}

void GenerationJobs::load() {
    for (const auto &entry : std::filesystem::directory_iterator(directory_)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        const auto id = entry.path().stem().string();
        if (!valid_id(id)) continue;
        try {
            const auto value = engine::io::json::parse_file(entry.path());
            Job job;
            job.id = id;
            job.status = engine::io::json::optional_string(value, "status", "interrupted");
            job.model = engine::io::json::require_string(value, "model");
            job.request_json = engine::io::json::require_string(value, "request_json");
            job.error = engine::io::json::optional_string(value, "error", "");
            job.created_at_ms = engine::io::json::optional_i64(value, "created_at_ms", 0);
            job.started_at_ms = engine::io::json::optional_i64(value, "started_at_ms", 0);
            job.finished_at_ms = engine::io::json::optional_i64(value, "finished_at_ms", 0);
            job.output_count = engine::io::json::optional_i32(value, "output_count", 0);
            if (job.output_count < 0 || job.output_count > 64) continue;
            if (job.status == "queued" || job.status == "running") {
                // Model registrations and uploaded temporary inputs are not guaranteed
                // to survive a restart. Do not silently repeat an expensive job.
                job.status = "interrupted";
                job.error = "Server restarted. Retry once the model is available.";
                job.finished_at_ms = now_ms();
                persist(job);
            }
            jobs_.emplace(job.id, std::move(job));
        } catch (...) {
            // Do not let one damaged metadata file prevent the server starting.
        }
    }
}

HttpResponse GenerationJobs::submit(const std::string &request_json) {
    if (request_json.size() > 1024 * 1024) {
        return error_response(413, "generation request exceeds 1 MiB", "invalid_request_error");
    }
    Job job;
    try {
        const auto value = engine::io::json::parse(request_json);
        job.model = engine::io::json::require_string(value, "model");
        if (job.model.empty() || !value.find("request") || !value.require("request").is_object()) {
            return error_response(400, "model and request object are required", "invalid_request_error");
        }
        const auto &r = value.require("request");
        if (r.find("audio") || r.find("audio_base64")) {
            return error_response(400, "background generation currently supports text/lyrics requests only", "invalid_request_error");
        }
    } catch (const std::exception &e) {
        return error_response(400, e.what(), "invalid_request_error");
    }
    job.created_at_ms = now_ms();
    job.request_json = request_json;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= 50) {
            return error_response(429, "generation queue is full", "server_busy");
        }
        do { job.id = new_id(); } while (jobs_.count(job.id) != 0);
        try { persist(job); }
        catch (const std::exception &e) {
            return error_response(500, e.what(), "server_error");
        }
        jobs_.emplace(job.id, job);
        queue_.push_back(job.id);
    }
    changed_.notify_one();
    return json_response(to_json(job), 202);
}

HttpResponse GenerationJobs::list() {
    std::vector<Job> items;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &[id, job] : jobs_) items.push_back(job);
    }
    std::sort(items.begin(), items.end(), [](const Job &a, const Job &b) {
        if (a.created_at_ms != b.created_at_ms) return a.created_at_ms > b.created_at_ms;
        return a.id > b.id;
    });
    std::string result = "{\"data\":[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) result += ",";
        result += to_json(items[i]);
    }
    result += "]}";
    return json_response(std::move(result));
}

HttpResponse GenerationJobs::get(const std::string &id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(id);
    if (it == jobs_.end()) return error_response(404, "generation not found", "not_found");
    return json_response(to_json(it->second));
}

HttpResponse GenerationJobs::retry(const std::string &id) {
    std::string result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = jobs_.find(id);
        if (it == jobs_.end()) return error_response(404, "generation not found", "not_found");
        if (it->second.status != "failed" && it->second.status != "interrupted") {
            return error_response(409, "only failed or interrupted jobs can be retried", "invalid_request_error");
        }
        if (queue_.size() >= 50) return error_response(429, "generation queue is full", "server_busy");
        Job next = it->second;
        next.status = "queued";
        next.error.clear();
        next.started_at_ms = 0;
        next.finished_at_ms = 0;
        next.output_count = 0;
        try { persist(next); }
        catch (const std::exception &e) { return error_response(500, e.what(), "server_error"); }
        it->second = std::move(next);
        result = to_json(it->second);
        queue_.push_back(id);
    }
    changed_.notify_one();
    return json_response(std::move(result), 202);
}

HttpResponse GenerationJobs::audio(const std::string &id, unsigned int index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = jobs_.find(id);
        if (it == jobs_.end() || it->second.status != "completed" ||
            index >= static_cast<unsigned int>(it->second.output_count)) {
            return error_response(404, "audio not found", "not_found");
        }
    }
    const auto path = directory_ / audio_filename(id, static_cast<int>(index));
    std::ifstream file(path, std::ios::binary);
    if (!file) return error_response(404, "audio file is missing", "not_found");
    HttpResponse response;
    response.content_type = "audio/wav";
    response.body.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    response.headers["Cache-Control"] = "private, no-store";
    response.headers["X-Content-Type-Options"] = "nosniff";
    return response;
}

HttpResponse GenerationJobs::handle(const HttpRequest &request) {
    constexpr std::string_view prefix = "/v1/tasks/jobs";
    if (request.path == prefix) {
        if (request.method == "GET") return list();
        if (request.method == "POST") return submit(request.body);
        return error_response(405, "method not allowed", "invalid_request_error");
    }
    const std::string suffix = request.path.substr(prefix.size() + 1);
    const auto separator = suffix.find('/');
    const std::string id = suffix.substr(0, separator);
    if (!valid_id(id)) return error_response(404, "generation not found", "not_found");
    if (separator == std::string::npos && request.method == "GET") return get(id);
    const std::string tail = separator == std::string::npos ? "" : suffix.substr(separator + 1);
    if (tail == "retry" && request.method == "POST") return retry(id);
    if (tail.rfind("audio/", 0) == 0 && request.method == "GET") {
        const auto index_text = tail.substr(6);
        if (index_text.empty() || index_text.size() > 2 ||
            !std::all_of(index_text.begin(), index_text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            return error_response(404, "audio not found", "not_found");
        }
        return audio(id, static_cast<unsigned int>(std::stoul(index_text)));
    }
    return error_response(404, "unknown generation endpoint", "not_found");
}

void GenerationJobs::run() {
    for (;;) {
        std::string id;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_) break;
            id = queue_.front();
            queue_.pop_front();
            auto it = jobs_.find(id);
            if (it == jobs_.end() || it->second.status != "queued") continue;
            it->second.status = "running";
            it->second.started_at_ms = now_ms();
            try { persist(it->second); }
            catch (const std::exception &e) {
                it->second.status = "failed";
                it->second.error = e.what();
                continue;
            }
        }
        execute(id);
    }
}

void GenerationJobs::execute(const std::string &id) {
    try {
        std::string request_json;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            request_json = jobs_.at(id).request_json;
        }
        const auto response = runner_(request_json);
        if (response.status != 200) throw std::runtime_error(response_error(response));
        const auto body = engine::io::json::parse(response.body);
        std::vector<std::string> encoded;
        if (const auto *audio = body.find("audio"); audio && audio->is_string()) {
            encoded.push_back(audio->as_string());
        }
        if (const auto *named = body.find("named_audio_outputs"); named && named->is_array()) {
            encoded.clear();
            for (const auto &entry : named->as_array()) {
                encoded.push_back(engine::io::json::require_string(entry, "audio"));
            }
        }
        if (encoded.empty()) throw std::runtime_error("generation returned no audio");
        if (encoded.size() > 64) throw std::runtime_error("too many audio outputs");
        for (size_t i = 0; i < encoded.size(); ++i) {
            const auto wav = minitts::app::base64_decode(encoded[i]);
            if (wav.size() < 44) throw std::runtime_error("invalid WAV output");
            write_atomic(directory_ / audio_filename(id, static_cast<int>(i)),
                std::string_view(reinterpret_cast<const char *>(wav.data()), wav.size()));
        }
        std::lock_guard<std::mutex> lock(mutex_);
        Job next = jobs_.at(id);
        next.status = "completed";
        next.finished_at_ms = now_ms();
        next.output_count = static_cast<int>(encoded.size());
        persist(next);
        jobs_.at(id) = std::move(next);
    } catch (const std::exception &e) {
        std::lock_guard<std::mutex> lock(mutex_);
        Job next = jobs_.at(id);
        next.status = "failed";
        next.error = e.what();
        next.finished_at_ms = now_ms();
        next.output_count = 0;
        try { persist(next); } catch (...) {}
        jobs_.at(id) = std::move(next);
    }
}

} // namespace minitts::server
