#include <gtest/gtest.h>

#include <string>

#include "otlp_decoder/decoder.h"

namespace {

TEST(OtlpDecoderTest, InvalidTracePayloadReturnsEmptyRows) {
  const std::string invalid_payload = "not-a-protobuf";
  EXPECT_TRUE(otlp_decoder::DecodeTraces(invalid_payload).empty());
}

TEST(OtlpDecoderTest, InvalidMetricPayloadReturnsEmptyRows) {
  const std::string invalid_payload = "not-a-protobuf";
  EXPECT_TRUE(otlp_decoder::DecodeMetrics(invalid_payload).empty());
}

TEST(OtlpDecoderTest, InvalidLogPayloadReturnsEmptyRows) {
  const std::string invalid_payload = "not-a-protobuf";
  EXPECT_TRUE(otlp_decoder::DecodeLogs(invalid_payload).empty());
}

}  // namespace
