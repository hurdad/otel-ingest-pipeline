#include "otlp_decoder/decoder.h"

#include <sstream>

#include "opentelemetry/proto/collector/logs/v1/logs_service.pb.h"
#include "opentelemetry/proto/collector/metrics/v1/metrics_service.pb.h"
#include "opentelemetry/proto/collector/trace/v1/trace_service.pb.h"

namespace otlp_decoder {

namespace {

std::string BytesToHex(const std::string& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 0x0F]);
  }
  return out;
}

std::string AnyValueToString(const opentelemetry::proto::common::v1::AnyValue& value) {
  using AnyValue = opentelemetry::proto::common::v1::AnyValue;
  switch (value.value_case()) {
    case AnyValue::kStringValue:
      return value.string_value();
    case AnyValue::kBoolValue:
      return value.bool_value() ? "true" : "false";
    case AnyValue::kIntValue:
      return std::to_string(value.int_value());
    case AnyValue::kDoubleValue:
      return std::to_string(value.double_value());
    case AnyValue::kBytesValue:
      return BytesToHex(value.bytes_value());
    case AnyValue::kArrayValue: {
      std::ostringstream os;
      os << '[';
      bool first = true;
      for (const auto& item : value.array_value().values()) {
        if (!first) {
          os << ',';
        }
        first = false;
        os << AnyValueToString(item);
      }
      os << ']';
      return os.str();
    }
    case AnyValue::kKvlistValue: {
      std::ostringstream os;
      os << '{';
      bool first = true;
      for (const auto& kv : value.kvlist_value().values()) {
        if (!first) {
          os << ',';
        }
        first = false;
        os << kv.key() << ':' << AnyValueToString(kv.value());
      }
      os << '}';
      return os.str();
    }
    case AnyValue::VALUE_NOT_SET:
      return "";
  }
  return "";
}

std::map<std::string, std::string> AttributesToMap(
    const google::protobuf::RepeatedPtrField<opentelemetry::proto::common::v1::KeyValue>& attrs) {
  std::map<std::string, std::string> out;
  for (const auto& attr : attrs) {
    out[attr.key()] = AnyValueToString(attr.value());
  }
  return out;
}

}  // namespace

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
    auto resource_attributes = AttributesToMap(rl.resource().attributes());
    for (const auto& attr : rl.resource().attributes()) {
      if (attr.key() == "service.name") service_name = attr.value().string_value();
    }
    for (const auto& sl : rl.scope_logs()) {
      auto scope_attributes = AttributesToMap(sl.scope().attributes());
      for (const auto& rec : sl.log_records()) {
        rows.push_back({
            static_cast<uint64_t>(rec.time_unix_nano()),
            BytesToHex(rec.trace_id()),
            BytesToHex(rec.span_id()),
            static_cast<uint8_t>(rec.flags()),
            service_name,
            rec.severity_text(),
            static_cast<uint8_t>(rec.severity_number()),
            AnyValueToString(rec.body()),
            rl.schema_url(),
            resource_attributes,
            sl.schema_url(),
            sl.scope().name(),
            sl.scope().version(),
            scope_attributes,
            AttributesToMap(rec.attributes())});
      }
    }
  }
  return rows;
}

}  // namespace otlp_decoder
