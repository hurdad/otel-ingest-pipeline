# otel-ingest-pipeline

## Architecture

```text
┌─────────────────────────────────────────────────────────────────┐
│                        Applications                             │
│        (FastAPI, services, any OTel-instrumented app)           │
└────────────────────────────┬────────────────────────────────────┘
                             │ OTLP gRPC  :4317
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│                   OpenTelemetry Collector                        │
│  receivers: OTLP gRPC (:4317)                                   │
│  processors: batch                                              │
│  exporters: OTLP gRPC → otlp-gateway (:4320)                   │
└────────────────────────────┬────────────────────────────────────┘
                             │ OTLP gRPC  :4320
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│                       otlp-gateway  (C++)                        │
│  gRPC server (:4320)                                            │
│  Deserializes ExportTraces/Metrics/LogsServiceRequest protobufs │
│  Publishes raw protobuf bytes to NATS JetStream subjects        │
└──────────┬───────────────────────┬──────────────────────────────┘
           │ otel.traces           │ otel.metrics / otel.logs
           ▼                       ▼
┌─────────────────────────────────────────────────────────────────┐
│                      NATS JetStream                             │
│  Stream: OTEL_TELEMETRY                                         │
│  Subjects: otel.traces | otel.metrics | otel.logs               │
│  Retention: limits, max-age 24h, file storage                   │
└────────────────────────────┬────────────────────────────────────┘
                             │ consume
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│              jetstream-clickhouse-loader  (C++)                  │
│  Decodes OTLP protobuf → TraceRow / MetricRow / LogRow structs  │
│  Batches rows (50k rows or 2s flush interval)                   │
│  Inserts via ClickHouse native protocol                         │
└──────────┬───────────────────────────────────────────────────── ┘
           │ native protocol  :9000
           ▼
┌─────────────────────────────────────────────────────────────────┐
│                         ClickHouse                              │
│  otel.otel_traces                                               │
│  otel.otel_metrics_{gauge,sum,histogram,exphistogram,summary}   │
│  otel.otel_logs                                                 │
└─────────────────────────────────────────────────────────────────┘

Self-telemetry (gateway + loader → otel-collector → otlp-gateway):
  OTEL_EXPORTER_OTLP_ENDPOINT=otel-collector:4317  (insecure gRPC)

Observability UI:
  HyperDX  :8090  (separate MongoDB service on :27017)
  Grafana   :3000  (ClickHouse datasource, pre-provisioned dashboards)
```

The repository contains two C++20 services:
- `otlp-gateway`: OTLP gRPC ingest service implementing traces/metrics/logs collector APIs.
- `jetstream-clickhouse-loader`: JetStream consumer that decodes OTLP protobuf payloads and batches inserts into ClickHouse.

## Design benefits

**Durability and backpressure**

NATS JetStream is a persistent queue, not just a pub/sub bus. If ClickHouse is slow, restarting, or under load, messages accumulate in JetStream (up to 24h retention) rather than being dropped. The gateway never blocks waiting for a database write.

**Decoupled write path**

The gateway and loader are completely independent processes. You can restart, redeploy, or scale either one without affecting the other. The loader resumes from its durable consumer position after a crash rather than losing data.

**Batching at the loader, not the gateway**

The gateway does one thing: receive OTLP, serialize the protobuf, publish to JetStream. All batching logic (50k rows or 2s flush interval) lives in the loader. This keeps the gateway lean and low-latency, and batch tuning requires no changes to the ingest path.

**Schema isolation**

ClickHouse schema changes (adding columns, altering indexes, TTL changes) only affect the loader. The gateway is shielded from the storage layer entirely.

**Fan-out ready**

Because data lives in named JetStream subjects (`otel.traces`, `otel.metrics`, `otel.logs`), additional consumers can be added — a second loader writing to a different database, an alerting consumer, a stream processor — without modifying the gateway.

**Horizontal scalability**

Multiple loader instances can consume from the same JetStream stream using durable consumers. JetStream handles message distribution; each loader processes its share independently.

**Self-telemetry without a bootstrap problem**

The gateway and loader export their own OTLP telemetry to the OTel Collector rather than directly back into themselves. Internal spans and metrics flow through the same pipeline as application data, giving full observability of the pipeline without circular dependencies.

**Protobuf preserved end-to-end**

The gateway publishes raw protobuf bytes — the exact OTLP wire format — directly to JetStream with no transcoding. The loader decodes them natively. There is no format conversion in the hot path and no information loss.

## Quick start (Docker Compose)

If you want to run the full pipeline locally with minimal setup, use Docker Compose:

```bash
docker compose up --build
```

Then send OTLP data to the collector:
- gRPC: `localhost:4317`

The compose stack starts Collector → Gateway → NATS JetStream → Loader → ClickHouse, and initializes both the JetStream stream and ClickHouse schema automatically.

## Sample FastAPI app (sends telemetry to collector)

A Python sample REST app is available at `examples/fastapi-rest-otel`.

It uses:
- `fastapi` + `uvicorn`
- `opentelemetry-sdk`
- OTLP gRPC exporter to send traces, metrics, and logs to OpenTelemetry Collector

Quick run:

```bash
cd examples/fastapi-rest-otel
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
uvicorn app.main:app --host 0.0.0.0 --port 8000
```

Default exporter endpoint: `localhost:4317` (insecure gRPC; no `http://` prefix)

See `examples/fastapi-rest-otel/README.md` for Docker usage, Locust load generation, and test requests.

## Repository layout

- `services/otlp-gateway`: OTLP ingest gateway implementation.
- `services/jetstream-clickhouse-loader`: JetStream-to-ClickHouse loader implementation.
- `libs/`: shared first-party libraries (serialization, telemetry, common utilities).
- `configs/`: example runtime configuration for gateway, loader, and collector.
- `scripts/`: JetStream stream bootstrap and ClickHouse schema SQL.
- `charts/otel-telemetry-pipeline`: Helm chart for Kubernetes deployment.

## Build instructions

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Binaries:
- `build/services/otlp-gateway/otlp-gateway`
- `build/services/jetstream-clickhouse-loader/jetstream-clickhouse-loader`

## Testing

Configure with tests enabled, build, and run with CTest:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOTEL_PIPELINE_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

This wires unit-test executables for first-party libraries under `libs/` (and optional `telemetry`) and excludes third-party test suites.

## Running locally

1. Start NATS with JetStream and ClickHouse.
2. Apply stream and table schema scripts.
3. Run gateway and loader binaries.
4. Configure OpenTelemetry Collector to export OTLP to `otlp-gateway:4320`.

Configuration examples are in `configs/gateway.yaml` and `configs/loader.yaml`.

### Telemetry env vars

- `OTEL_EXPORTER_OTLP_ENDPOINT`
- `OTEL_SERVICE_NAME`
- `OTEL_RESOURCE_ATTRIBUTES`

Both gateway and loader use `libs/telemetry` self-instrumentation built on `opentelemetry-cpp` SDK:
- traces: internal operation spans (gRPC handling, JetStream operations, ClickHouse writes)
- metrics: counters and histograms for internal operations (see `libs/telemetry` for exact metric names)
- logs: structured SDK logs emitted by the telemetry runtime

When `OTEL_EXPORTER_OTLP_ENDPOINT` is empty, self-telemetry exporters are disabled.

## JetStream setup

```bash
./scripts/create_jetstream_stream.sh
```

Creates stream `OTEL_TELEMETRY` with subjects:
- `otel.traces`
- `otel.metrics`
- `otel.logs`

Retention is configured with max age 24h.

## ClickHouse schema

```bash
clickhouse-client --multiquery < scripts/clickhouse_schema.sql
```

The schema script creates (if needed) and uses the `otel` database before creating tables.

Tables:
- `otel.otel_traces`
- `otel.otel_metrics_gauge`
- `otel.otel_metrics_sum`
- `otel.otel_metrics_histogram`
- `otel.otel_metrics_exponentialhistogram`
- `otel.otel_metrics_summary`
- `otel.otel_logs`

## Example pipeline

1. OpenTelemetry Collector receives app telemetry.
2. Collector exports OTLP gRPC to `otlp-gateway`.
3. Gateway serializes `Export*ServiceRequest` protobufs and publishes payloads to JetStream subjects.
4. Loader consumes JetStream records, decodes protobuf payloads, batches rows (50k or 2s), and writes using ClickHouse native protocol.

## Single Docker image build (two-stage)

The repository root `Dockerfile` performs a multi-stage build that:
- builds both services (with static first-party libraries),
- runs the CTest suite,
- installs both binaries,
- copies only the two installed binaries into the final image and installs only runtime packages (no build/dev packages and no manual shared-library collection step).

Build it with:

```bash
docker build -t otel-ingest-pipeline .
```

Included binaries in the final image:
- `/usr/local/bin/otlp-gateway`
- `/usr/local/bin/jetstream-clickhouse-loader`

By default, the container starts `otlp-gateway`.


## Docker Compose full pipeline

A ready-to-run compose stack is provided in `docker-compose.yml` with:
- OpenTelemetry Collector (`otel-collector`) — OTLP gRPC receiver on `:4317`
- OTLP Gateway (`otlp-gateway`) — gRPC ingest on `:4320` (internal)
- NATS JetStream (`nats`)
- JetStream stream bootstrap job (`jetstream-init`)
- ClickHouse (`clickhouse`)
- JetStream loader (`jetstream-clickhouse-loader`)
- FastAPI sample app (`fastapi-rest-sample`) — exposed on `:8080`
- Locust headless load generator (`fastapi-rest-sample-loadgen`)
- HyperDX (`hyperdx`) — observability UI on `:8090`
- MongoDB (`db`) — backing store for HyperDX
- Grafana (`grafana`) — metrics/traces/logs dashboards on `:3000` (admin/admin)

Start everything:

```bash
docker compose up --build
```

Send OTLP data to the collector at:
- gRPC: `localhost:4317`

The collector forwards telemetry to the gateway on `:4320`, which publishes to JetStream subjects (`otel.traces`, `otel.metrics`, `otel.logs`).

ClickHouse tables are auto-created from `scripts/clickhouse_schema.sql` at container startup.

## Grafana

Grafana is available at `http://localhost:3000` (credentials: `admin` / `admin`).

The ClickHouse datasource is provisioned automatically via `configs/grafana/provisioning/datasources/clickhouse.yml`. Three dashboards are pre-loaded from `configs/grafana/dashboards/`:

| Dashboard | UID | Description |
| --- | --- | --- |
| OTel Logs Overview | `otel-logs` | Log volume, severity distribution, error rate, recent error logs |
| OTel Traces Overview | `otel-traces` | Span volume, error rate, latency percentiles (P50/P95/P99), top operations, slowest spans |
| OTel Metrics Overview | `otel-metrics` | Gauge/sum/histogram ingestion rates, top metric names, HTTP request rate and duration, system CPU and memory |

All dashboards query ClickHouse directly using the `grafana-clickhouse-datasource` plugin and default to a 1-hour time window with 10-second auto-refresh.

## Helm chart

A Helm chart is available at `charts/otel-telemetry-pipeline` for deploying:
- `otlp-gateway`
- `jetstream-clickhouse-loader`

The chart expects reachable NATS and ClickHouse endpoints (defaults match the compose service hostnames).

Install example:

```bash
helm upgrade --install otel-pipeline ./charts/otel-telemetry-pipeline \
  --namespace observability \
  --create-namespace \
  --set gateway.image.repository=<your-registry>/otlp-gateway \
  --set gateway.image.tag=<tag> \
  --set loader.image.repository=<your-registry>/jetstream-clickhouse-loader \
  --set loader.image.tag=<tag>
```

Override connectivity if needed:

```bash
helm upgrade --install otel-pipeline ./charts/otel-telemetry-pipeline \
  --set nats.url=nats://nats.my-namespace.svc.cluster.local:4222 \
  --set loader.clickhouse.host=clickhouse.my-namespace.svc.cluster.local
```
