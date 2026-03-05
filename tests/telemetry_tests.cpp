#include <gtest/gtest.h>

#include "telemetry/tracer.h"

namespace {

TEST(TelemetryTest, StartSpanReturnsObject) {
  auto span = telemetry::StartSpan("unit_test_span");
  EXPECT_NE(span, nullptr);
}

}  // namespace
