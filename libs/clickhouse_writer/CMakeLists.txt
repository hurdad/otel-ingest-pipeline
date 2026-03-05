add_library(clickhouse_writer STATIC
  src/clickhouse_writer.cpp)

target_include_directories(clickhouse_writer PUBLIC include)
target_link_libraries(clickhouse_writer PUBLIC telemetry otlp_decoder)

if(TARGET clickhouse-cpp-lib)
  target_link_libraries(clickhouse_writer PRIVATE clickhouse-cpp-lib)
endif()
