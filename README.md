# libesca-aiclient

How the board talks to the AI server. One library, because there is one
server with one API, and every feature that needs a model links it rather
than carrying its own copy of the transport.

    esca-anomalyd ─┐
    esca-voiced   ─┼─► libesca-aiclient ──HTTP──► AI server
    (next feature)─┘

Covers `/v1/anomaly/model`, `/v1/anomaly/infer`, `/v1/data/upload`,
`/v1/stt`, `/v1/tts` and `/v1/health`.

## Why plain HTTP

A two-second clip of 44.1 kHz mono is 176 KB and a verdict is a line of
JSON. That is comfortable for HTTP with keep-alive, debuggable with `curl`,
and leaves a server that is a few dozen lines of FastAPI. A WebSocket would
save a handshake and is worth moving to when connection churn shows up in
measurements, not before; gRPC costs a heavy dependency on the board and
buys nothing at this rate.

## The two failure policies are the callers' to choose

The library reports failure and does not retry. That is deliberate, because
the features want opposite things:

- **Collected data must never be lost.** `esca-uploader` writes to disk
  first and records success before deleting anything.
- **A live verdict about sound from ten minutes ago is worthless.**
  `esca-anomalyd` drops the segment and counts it.

Putting a retry queue in here would force one of those policies on both.
