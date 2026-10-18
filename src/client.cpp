#include "esca/ai/client.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace esca::ai {

namespace {

size_t collect(void *data, size_t size, size_t count, void *user)
{
    auto *out = static_cast<std::string *>(user);
    out->append(static_cast<char *>(data), size * count);
    return size * count;
}

/// curl_global_init is not thread-safe and must happen once per process.
void ensure_curl()
{
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

} // namespace

struct AiClient::Impl {
    ServerConfig config;
    CURL *curl = nullptr;
    std::mutex mutex; ///< one handle, and callers on more than one thread

    void prepare(const std::string &url, std::string &body)
    {
        curl_easy_reset(curl);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(config.timeout_ms));
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                         static_cast<long>(config.connect_timeout_ms));
        // Without this a dropped network leaves the whole process blocked in
        // a signal handler on some libc builds.
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    }

    bool finish(CURLcode rc, const std::string &body, std::string &error)
    {
        if (rc != CURLE_OK) {
            error = curl_easy_strerror(rc);
            return false;
        }

        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (status < 200 || status >= 300) {
            // The server puts a readable reason in "detail"; surfacing it is
            // the difference between "HTTP 400" and "this model expects
            // 44100 Hz, got 16000".
            error = "HTTP " + std::to_string(status);
            try {
                const auto doc = nlohmann::json::parse(body);
                if (doc.contains("detail"))
                    error += ": " + doc["detail"].dump();
            } catch (const nlohmann::json::exception &) {
                if (!body.empty())
                    error += ": " + body.substr(0, 200);
            }
            return false;
        }
        return true;
    }
};

AiClient::AiClient(ServerConfig config) : impl_(std::make_unique<Impl>())
{
    ensure_curl();
    impl_->config = std::move(config);
    impl_->curl = curl_easy_init();
}

AiClient::~AiClient()
{
    if (impl_->curl)
        curl_easy_cleanup(impl_->curl);
}

const ServerConfig &AiClient::config() const { return impl_->config; }

bool AiClient::fetch_spec(ModelSpec &spec, std::string &error)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->curl) {
        error = "curl is not initialised";
        return false;
    }

    std::string body;
    impl_->prepare(impl_->config.base_url + "/v1/anomaly/model", body);
    const CURLcode rc = curl_easy_perform(impl_->curl);
    if (!impl_->finish(rc, body, error))
        return false;

    try {
        const auto doc = nlohmann::json::parse(body);
        spec.sample_rate = doc.value("sample_rate", 0u);
        spec.channels = doc.value("channels", 1u);
        spec.seconds = doc.value("seconds", 0.0);
        spec.samples = doc.value("samples", 0u);
        spec.threshold = doc.value("threshold", 0.0);
        spec.model = doc.value("model", "");
    } catch (const nlohmann::json::exception &e) {
        error = std::string("cannot parse the model spec: ") + e.what();
        return false;
    }
    return spec.valid();
}

bool AiClient::infer(const uint8_t *pcm, size_t bytes, uint32_t sample_rate, uint64_t sequence,
                     Verdict &out, std::string &error)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->curl) {
        error = "curl is not initialised";
        return false;
    }

    std::string body;
    impl_->prepare(impl_->config.base_url + "/v1/anomaly/infer", body);

    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    const std::string rate_header = "X-Sample-Rate: " + std::to_string(sample_rate);
    headers = curl_slist_append(headers, rate_header.c_str());
    const std::string seq_header = "X-Sequence: " + std::to_string(sequence);
    headers = curl_slist_append(headers, seq_header.c_str());
    curl_easy_setopt(impl_->curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(impl_->curl, CURLOPT_POST, 1L);
    curl_easy_setopt(impl_->curl, CURLOPT_POSTFIELDS, pcm);
    curl_easy_setopt(impl_->curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(bytes));

    const CURLcode rc = curl_easy_perform(impl_->curl);
    const bool ok = impl_->finish(rc, body, error);
    curl_slist_free_all(headers);
    if (!ok)
        return false;

    try {
        const auto doc = nlohmann::json::parse(body);
        out.anomaly = doc.value("anomaly", false);
        out.score = doc.value("score", 0.0);
        out.threshold = doc.value("threshold", 0.0);
        out.inference_ms = doc.value("inference_ms", 0.0);
        out.sequence = sequence;
    } catch (const nlohmann::json::exception &e) {
        error = std::string("cannot parse the verdict: ") + e.what();
        return false;
    }
    return true;
}

bool AiClient::upload(const std::string &wav_path, const ClipMeta &meta, std::string &error)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->curl) {
        error = "curl is not initialised";
        return false;
    }

    std::string body;
    impl_->prepare(impl_->config.base_url + "/v1/data/upload", body);

    // Multipart, because a clip without its label is of no use and one
    // request that cannot carry half the pair beats two that can disagree.
    curl_mime *form = curl_mime_init(impl_->curl);

    curl_mimepart *part = curl_mime_addpart(form);
    curl_mime_name(part, "file");
    curl_mime_filedata(part, wav_path.c_str());

    auto field = [&](const char *name, const std::string &value) {
        curl_mimepart *p = curl_mime_addpart(form);
        curl_mime_name(p, name);
        curl_mime_data(p, value.c_str(), CURL_ZERO_TERMINATED);
    };
    field("machine", meta.machine);
    field("label", meta.label);
    field("device", meta.device);
    field("started_ns", std::to_string(meta.started_ns));
    field("dropped", std::to_string(meta.dropped));

    curl_easy_setopt(impl_->curl, CURLOPT_MIMEPOST, form);
    const CURLcode rc = curl_easy_perform(impl_->curl);
    const bool ok = impl_->finish(rc, body, error);
    curl_mime_free(form);
    return ok;
}

bool AiClient::transcribe(const uint8_t *wav, size_t bytes, Transcript &out,
                          std::string &error)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->curl) {
        error = "curl is not initialised";
        return false;
    }

    std::string body;
    impl_->prepare(impl_->config.base_url + "/v1/stt", body);

    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: audio/wav");
    curl_easy_setopt(impl_->curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(impl_->curl, CURLOPT_POST, 1L);
    curl_easy_setopt(impl_->curl, CURLOPT_POSTFIELDS, wav);
    curl_easy_setopt(impl_->curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(bytes));

    const CURLcode rc = curl_easy_perform(impl_->curl);
    const bool ok = impl_->finish(rc, body, error);
    curl_slist_free_all(headers);
    if (!ok)
        return false;

    try {
        const auto doc = nlohmann::json::parse(body);
        out.text = doc.value("text", "");
        out.language = doc.value("language", "");
        out.confidence = doc.value("confidence", 0.0);
        out.ms = doc.value("ms", 0.0);
    } catch (const nlohmann::json::exception &e) {
        error = std::string("cannot parse the transcript: ") + e.what();
        return false;
    }
    return true;
}

bool AiClient::synthesise(const std::string &text, const std::string &wav_path,
                          std::string &error)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->curl) {
        error = "curl is not initialised";
        return false;
    }

    std::FILE *out = std::fopen(wav_path.c_str(), "wb");
    if (!out) {
        error = "cannot write " + wav_path;
        return false;
    }

    // Straight to the file: a spoken sentence is hundreds of kilobytes and
    // there is no reason for it to pass through a std::string first.
    curl_easy_reset(impl_->curl);
    const std::string url = impl_->config.base_url + "/v1/tts";
    curl_easy_setopt(impl_->curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(impl_->curl, CURLOPT_WRITEFUNCTION, nullptr);
    curl_easy_setopt(impl_->curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(impl_->curl, CURLOPT_TIMEOUT_MS,
                     static_cast<long>(impl_->config.timeout_ms));
    curl_easy_setopt(impl_->curl, CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(impl_->config.connect_timeout_ms));
    curl_easy_setopt(impl_->curl, CURLOPT_NOSIGNAL, 1L);

    const nlohmann::json request = { { "text", text } };
    const std::string payload = request.dump();

    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(impl_->curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(impl_->curl, CURLOPT_POST, 1L);
    curl_easy_setopt(impl_->curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(impl_->curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(payload.size()));

    const CURLcode rc = curl_easy_perform(impl_->curl);
    std::fclose(out);
    curl_slist_free_all(headers);

    if (rc != CURLE_OK) {
        error = curl_easy_strerror(rc);
        ::remove(wav_path.c_str());
        return false;
    }

    long status = 0;
    curl_easy_getinfo(impl_->curl, CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) {
        error = "HTTP " + std::to_string(status);
        // The body is an error message, not audio; leaving it on disk would
        // have esca-audiod try to play JSON.
        ::remove(wav_path.c_str());
        return false;
    }
    return true;
}

bool AiClient::healthy(std::string &error)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->curl) {
        error = "curl is not initialised";
        return false;
    }

    std::string body;
    impl_->prepare(impl_->config.base_url + "/v1/health", body);
    const CURLcode rc = curl_easy_perform(impl_->curl);
    return impl_->finish(rc, body, error);
}

} // namespace esca::ai
