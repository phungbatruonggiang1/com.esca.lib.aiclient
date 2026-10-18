// Talking to the AI server.
//
// The Raspberry Pi cannot run the models, so everything that needs one goes
// over the network. A two-second clip of 44.1 kHz mono is 176 kB and a
// verdict is a line of JSON, so plain HTTP is comfortable here and leaves
// something that can be debugged with curl.
//
// The two callers want opposite things when the link goes down, and the
// difference is deliberate:
//
//   - Collected data must never be lost. The recorder writes to disk first,
//     and the uploader deletes only once the server has acknowledged it.
//   - A live verdict about sound from ten minutes ago is worthless, so the
//     detector drops segments and says so rather than building a backlog it
//     would deliver far too late.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace esca::ai {

struct ServerConfig {
    std::string base_url = "http://192.168.1.10:8099";
    int timeout_ms = 5000;
    int connect_timeout_ms = 2000;
};

/// What the server says it expects. Fetched rather than hard-coded on both
/// sides: the training repository disagrees with itself about the sample
/// rate, and a mismatch there produces confident nonsense instead of an
/// error.
struct ModelSpec {
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    double seconds = 0.0;
    uint32_t samples = 0;
    double threshold = 0.0;
    std::string model;

    bool valid() const { return sample_rate != 0 && samples != 0; }
};

struct Verdict {
    bool anomaly = false;
    double score = 0.0;
    double threshold = 0.0;
    double inference_ms = 0.0;
    uint64_t sequence = 0;
};

/// Metadata that travels with an uploaded clip. A filename cannot carry
/// which microphone recorded it or whether samples were lost.
struct ClipMeta {
    std::string machine = "unknown";
    std::string label = "unlabelled";
    std::string device;
    uint64_t started_ns = 0;
    uint64_t dropped = 0;
};

/// What the speech server heard.
struct Transcript {
    std::string text;
    std::string language;
    double confidence = 0.0;
    double ms = 0.0;
};

class AiClient {
public:
    explicit AiClient(ServerConfig config);
    ~AiClient();

    AiClient(const AiClient &) = delete;
    AiClient &operator=(const AiClient &) = delete;

    const ServerConfig &config() const;

    /// GET /v1/anomaly/model. Call once at startup and record at whatever
    /// it asks for.
    bool fetch_spec(ModelSpec &spec, std::string &error);

    /// POST /v1/anomaly/infer with raw s16le. The sequence number is echoed
    /// back so a caller can tell which chunk a verdict belongs to.
    bool infer(const uint8_t *pcm, size_t bytes, uint32_t sample_rate, uint64_t sequence,
               Verdict &out, std::string &error);

    /// POST /v1/data/upload with a WAV file. Returns true only once the
    /// server has stored it, which is the signal to delete the local copy.
    bool upload(const std::string &wav_path, const ClipMeta &meta, std::string &error);

    /// POST /v1/stt with a WAV. Any sample rate: the server resamples,
    /// which it was doing internally anyway, so the board does not carry a
    /// resampler just to satisfy a model that wants 16 kHz while the
    /// anomaly model pins the microphone to 44100.
    bool transcribe(const uint8_t *wav, size_t bytes, Transcript &out, std::string &error);

    /// POST /v1/tts and write the spoken reply to a file, which is what
    /// esca-audiod plays. WAV, because the board has no mp3 decoder and
    /// adding one to say a sentence is not worth it.
    bool synthesise(const std::string &text, const std::string &wav_path,
                    std::string &error);

    /// GET /v1/health. Cheap enough to use as a reachability check.
    bool healthy(std::string &error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace esca::ai
