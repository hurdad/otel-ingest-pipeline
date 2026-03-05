#include "otlp_decoder/decoder.h"

#include "opentelemetry/proto/collector/logs/v1/logs_service.pb.h"
#include "opentelemetry/proto/collector/metrics/v1/metrics_service.pb.h"
#include "opentelemetry/proto/collector/trace/v1/trace_service.pb.h"

namespace otlp_decoder {

std::vector<TraceRow> DecodeTraces(const std::string& payload) {
  opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest req;
  if (!req.ParseFromString(payload)) {
    return {};
  }
  std::vector<TraceRow> rows;
  for (const auto& rs : req.resource_spans()) {
    std::string service_name = "unknown";
    for (const auto& attr : rs.resource().attributes()) {
      if (attr.key() == "service.name") service_name = attr.value().string_value();
    }
    for (const auto& ss : rs.scope_spans()) {
      for (const auto& span : ss.spans()) {
        rows.push_back({
            static_cast<uint64_t>(span.start_time_unix_nano()),
            span.trace_id(),
            span.span_id(),
            span.parent_span_id(),
            service_name,
            span.name(),
            static_cast<uint64_t>(span.end_time_unix_nano() - span.start_time_unix_nano())});
      }
    }
  }
  return rows;
}

std::vector<MetricRow> DecodeMetrics(const std::string& payload) {
  opentelemetry::proto::collector::metrics::v1::ExportMetricsServiceRequest req;
  if (!req.ParseFromString(payload)) {
    return {};
  }
  using NDP = opentelemetry::proto::metrics::v1::NumberDataPoint;
  std::vector<MetricRow> rows;
  for (const auto& rm : req.resource_metrics()) {
    std::string service_name = "unknown";
    for (const auto& attr : rm.resource().attributes()) {
      if (attr.key() == "service.name") service_name = attr.value().string_value();
    }
    for (const auto& sm : rm.scope_metrics()) {
      for (const auto& metric : sm.metrics()) {
        const auto add = [&](uint64_t ts, double value) {
          rows.push_back({ts, service_name, metric.name(), value});
        };
        const auto ndp_value = [](const NDP& dp) -> double {
          return dp.value_case() == NDP::kAsInt ? static_cast<double>(dp.as_int())
                                                : dp.as_double();
        };
        if (metric.has_gauge()) {
          for (const auto& dp : metric.gauge().data_points()) {
            add(dp.time_unix_nano(), ndp_value(dp));
          }
        } else if (metric.has_sum()) {
          for (const auto& dp : metric.sum().data_points()) {
            add(dp.time_unix_nano(), ndp_value(dp));
          }
        } else if (metric.has_histogram()) {
          for (const auto& dp : metric.histogram().data_points()) {
            add(dp.time_unix_nano(), dp.sum());
          }
        } else if (metric.has_summary()) {
          for (const auto& dp : metric.summary().data_points()) {
            add(dp.time_unix_nano(), dp.sum());
          }
        }
      }
    }
  }
  return rows;
}

std::vector<LogRow> DecodeLogs(const std::string& payload) {
  opentelemetry::proto::collector::logs::v1::ExportLogsServiceRequest req;
  if (!req.ParseFromString(payload)) {
    return {};
  }
  std::vector<LogRow> rows;
  for (const auto& rl : req.resource_logs()) {
    std::string service_name = "unknown";
    for (const auto& attr : rl.resource().attributes()) {
      if (attr.key() == "service.name") service_name = attr.value().string_value();
    }
    for (const auto& sl : rl.scope_logs()) {
      for (const auto& rec : sl.log_records()) {
        rows.push_back({
            static_cast<uint64_t>(rec.time_unix_nano()),
            service_name,
            rec.severity_text(),
            rec.body().string_value()});
      }
    }
  }
  return rows;
}

}  // namespace otlp_decoder
