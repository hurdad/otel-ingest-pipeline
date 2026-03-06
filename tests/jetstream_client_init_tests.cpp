#include "jetstream_client/jetstream_client.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace {

TEST(JetStreamPublisherInitTest, InitializesWhenForcedSuccess) {
  jetstream_client::testing::ForceInitializationSuccessForTests();
  EXPECT_NO_THROW({
    jetstream_client::JetStreamPublisher publisher("nats://invalid:4222",
                                                   "otel", {"otel.traces"});
  });
  jetstream_client::testing::ResetInitializationBehaviorForTests();
}

TEST(JetStreamPublisherInitTest, ThrowsWhenInitialConnectFails) {
  jetstream_client::testing::ForceInitializationFailureForTests();
  EXPECT_THROW(
      {
        jetstream_client::JetStreamPublisher publisher("nats://invalid:4222",
                                                       "otel", {"otel.traces"});
      },
      std::runtime_error);
  jetstream_client::testing::ResetInitializationBehaviorForTests();
}

TEST(JetStreamConsumerInitTest, InitializesWhenForcedSuccess) {
  jetstream_client::testing::ForceInitializationSuccessForTests();
  EXPECT_NO_THROW({
    jetstream_client::JetStreamConsumer consumer(
        "nats://invalid:4222", "otel",
        {"otel.traces", "otel.metrics", "otel.logs"});
  });
  jetstream_client::testing::ResetInitializationBehaviorForTests();
}

TEST(JetStreamConsumerInitTest, ThrowsWhenInitialConnectFails) {
  jetstream_client::testing::ForceInitializationFailureForTests();
  EXPECT_THROW(
      {
        jetstream_client::JetStreamConsumer consumer(
            "nats://invalid:4222", "otel",
            {"otel.traces", "otel.metrics", "otel.logs"});
      },
      std::runtime_error);
  jetstream_client::testing::ResetInitializationBehaviorForTests();
}


TEST(JetStreamConsumerBehaviorTest, ContainsHandlerExceptions) {
  const jetstream_client::Message message{"otel.traces", "payload"};

  EXPECT_FALSE(jetstream_client::testing::InvokeConsumerHandlerForTests(
      message, [](const jetstream_client::Message &) {
        throw std::runtime_error("handler failure");
      }));

  EXPECT_TRUE(jetstream_client::testing::InvokeConsumerHandlerForTests(
      message, [](const jetstream_client::Message &msg) {
        EXPECT_EQ(msg.subject, "otel.traces");
        EXPECT_EQ(msg.payload, "payload");
      }));
}

} // namespace
