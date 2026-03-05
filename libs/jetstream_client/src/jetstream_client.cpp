#include "jetstream_client/jetstream_client.h"

#include <algorithm>
#include <iostream>

#include <natscpp/natscpp.hpp>
#include "telemetry/tracer.h"

namespace jetstream_client {

namespace {

// Creates the stream if it doesn't already exist.
void ensure_stream(natscpp::jetstream& js, const std::string& stream_name,
                   const std::vector<std::string>& subjects) {
  try {
    js.get_stream_info(stream_name);
  } catch (const natscpp::nats_error& e) {
    if (e.status() == NATS_NOT_FOUND) {
      js.create_stream({stream_name, subjects});
      std::clog << "JetStream stream created name=" << stream_name << '\n';
    } else {
      throw;
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// JetStreamPublisher
// ---------------------------------------------------------------------------

struct JetStreamPublisher::Impl {
  natscpp::connection conn;
  natscpp::jetstream js;

  Impl(std::string_view url, const std::string& stream_name,
       const std::vector<std::string>& subjects)
      : conn(natscpp::connection::connect_to(url)), js(conn) {
    ensure_stream(js, stream_name, subjects);
  }
};

JetStreamPublisher::JetStreamPublisher(std::string url, std::string stream_name,
                                       std::vector<std::string> subjects)
    : url_(std::move(url)) {
  try {
    impl_ = std::make_unique<Impl>(url_, stream_name, subjects);
  } catch (const natscpp::nats_error& e) {
    std::clog << "JetStream publisher connect failed url=" << url_ << ": " << e.what() << '\n';
  }
}

JetStreamPublisher::~JetStreamPublisher() = default;

bool JetStreamPublisher::Publish(const std::string& subject, const void* data, size_t size) {
  if (!impl_) {
    std::clog << "JetStream publisher not connected, dropping subject=" << subject << '\n';
    return false;
  }
  auto span = telemetry::StartSpan("jetstream_publish");
  try {
    (void)impl_->js.publish(subject, std::string_view(static_cast<const char*>(data), size));
    return true;
  } catch (const natscpp::nats_error& e) {
    std::clog << "JetStream publish error subject=" << subject << ": " << e.what() << '\n';
    return false;
  }
}

// ---------------------------------------------------------------------------
// JetStreamConsumer
// ---------------------------------------------------------------------------

struct JetStreamConsumer::Impl {
  natscpp::connection conn;
  natscpp::jetstream js;
  std::vector<natscpp::js_pull_consumer> consumers;
  std::vector<std::string> subjects;

  Impl(std::string_view url, const std::string& stream_name,
       const std::vector<std::string>& subs)
      : conn(natscpp::connection::connect_to(url)), js(conn) {
    ensure_stream(js, stream_name, subs);
    for (const auto& subject : subs) {
      // Derive a durable consumer name from the subject (dots -> dashes).
      std::string durable = subject;
      std::replace(durable.begin(), durable.end(), '.', '-');
      consumers.push_back(js.pull_subscribe(subject, durable));
      subjects.push_back(subject);
    }
  }
};

JetStreamConsumer::JetStreamConsumer(std::string url, std::string stream,
                                     std::vector<std::string> subjects)
    : url_(std::move(url)), stream_(std::move(stream)), subjects_(std::move(subjects)) {
  try {
    impl_ = std::make_unique<Impl>(url_, stream_, subjects_);
  } catch (const natscpp::nats_error& e) {
    std::clog << "JetStream consumer connect failed url=" << url_ << ": " << e.what() << '\n';
  }
}

JetStreamConsumer::~JetStreamConsumer() = default;

void JetStreamConsumer::Poll(const Handler& handler) {
  if (!impl_) {
    std::clog << "JetStream consumer not connected, skipping poll\n";
    return;
  }
  auto span = telemetry::StartSpan("jetstream_consume");
  for (size_t i = 0; i < impl_->consumers.size(); ++i) {
    // Drain all available messages for this subject.
    while (true) {
      try {
        auto msg = impl_->consumers[i].next(std::chrono::milliseconds(100));
        handler(Message{std::string(msg.subject()), std::string(msg.data())});
        msg.ack();
      } catch (const natscpp::nats_error& e) {
        if (e.status() != NATS_TIMEOUT) {
          std::clog << "JetStream consume error subject=" << impl_->subjects[i]
                    << ": " << e.what() << '\n';
        }
        break;
      }
    }
  }
}

}  // namespace jetstream_client
