#include "grpc_server.h"

#include "telemetry/tracer.h"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <pthread.h>
#include <string>
#include <thread>

namespace {

std::string GetEnvOrDefault(const char* key, const char* fallback) {
  const char* value = std::getenv(key);
  if (value == nullptr || value[0] == '\0') {
    return fallback;
  }
  return value;
}

int WaitForTerminationSignal() {
  sigset_t signal_set;
  sigemptyset(&signal_set);
  sigaddset(&signal_set, SIGINT);
  sigaddset(&signal_set, SIGTERM);

  const int mask_result = pthread_sigmask(SIG_BLOCK, &signal_set, nullptr);
  if (mask_result != 0) {
    std::clog << "Failed to block termination signals, exiting without graceful shutdown\n";
    return SIGTERM;
  }

  int received_signal = 0;
  const int wait_result = sigwait(&signal_set, &received_signal);
  if (wait_result != 0) {
    std::clog << "sigwait failed, forcing shutdown\n";
    return SIGTERM;
  }

  return received_signal;
}

}  // namespace

int main() {
  telemetry::InitTelemetry();

  OtlpGrpcServer server(GetEnvOrDefault("GATEWAY_LISTEN_ADDR", "0.0.0.0:4317"),
                        GetEnvOrDefault("NATS_URL", "nats://localhost:4222"));

  std::thread server_thread([&server]() { server.Run(); });

  const int signal = WaitForTerminationSignal();
  std::clog << "Received signal " << signal << ", shutting down OTLP gateway\n";

  server.Shutdown();

  if (server_thread.joinable()) {
    server_thread.join();
  }

  return 0;
}
