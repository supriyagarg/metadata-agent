#include "updater.h"

#include "json.h"
#include "local_stream_http.h"
#include "logging.h"

#include <boost/network/protocol/http/server.hpp>
#include <boost/network/protocol/http/client.hpp>
#include <chrono>

namespace http = boost::network::http;

namespace google {

PollingMetadataUpdater::PollingMetadataUpdater(
    double period_s, MetadataAgent* store,
    std::function<std::vector<Metadata>(void)> query_metadata)
    : period_(period_s),
      store_(store),
      query_metadata_(query_metadata),
      timer_(),
      reporter_thread_() {}

PollingMetadataUpdater::~PollingMetadataUpdater() {
  reporter_thread_.join();
}

void PollingMetadataUpdater::start() {
  timer_.lock();
  LOG(INFO) << "Timer locked";
  reporter_thread_ =
      std::thread(std::bind(&PollingMetadataUpdater::PollForMetadata, this));
}

void PollingMetadataUpdater::stop() {
  timer_.unlock();
  LOG(INFO) << "Timer unlocked";
}

void PollingMetadataUpdater::PollForMetadata() {
  // An unlocked timer means we should stop updating.
  LOG(INFO) << "Trying to unlock the timer";
  auto start = std::chrono::high_resolution_clock::now();
  while (!timer_.try_lock_for(period_)) {
    auto now = std::chrono::high_resolution_clock::now();
    // Detect spurious wakeups.
    if (now - start < period_) continue;
    LOG(INFO) << " Timer unlock timed out after " << std::chrono::duration_cast<seconds>(now - start).count() << "s (good)";
    start = now;
    std::vector<Metadata> result_vector = query_metadata_();
    for (const Metadata& result : result_vector) {
      store_->UpdateResource(result.id, result.resource, result.metadata);
    }
  }
  LOG(INFO) << "Timer unlocked (stop polling)";
}

std::string NumericProjectId() {
  // Query the metadata server.
  // TODO: Other sources.
  static std::string project_id("1234567890");
  if (project_id.empty()) {
    http::client client;
    http::client::request request("http://metadata.google.internal/computeMetadata/v1/project/numeric-project-id");
    request << boost::network::header("Metadata-Flavor", "Google");
    http::client::response response = client.get(request);
    project_id = body(response);
  }
  return project_id;
}

namespace {

std::string InstanceZone() {
  // Query the metadata server.
  // TODO: Other sources?
  static std::string zone("us-central1-a");
  if (zone.empty()) {
    http::client client;
    http::client::request request("http://metadata.google.internal/computeMetadata/v1/instance/zone");
    request << boost::network::header("Metadata-Flavor", "Google");
    http::client::response response = client.get(request);
    zone = body(response);
  }
  return zone;
}

}

std::vector<PollingMetadataUpdater::Metadata> DockerMetadataQuery() {
  // TODO
  LOG(INFO) << "Docker Query called";
  const std::string project_id = NumericProjectId();
  const std::string zone = InstanceZone();
  http::local_client client;
  http::local_client::request request("unix://%2Fvar%2Frun%2Fdocker.sock/v1.24/containers/json?all=true");
  http::local_client::response response = client.get(request);
  LOG(ERROR) << "Response: " << body(response);
  std::unique_ptr<json::Value> parsed = json::JSONParser::FromString(body(response));
  LOG(ERROR) << "Parsed response: " << *parsed;
  std::vector<PollingMetadataUpdater::Metadata> result;
  if (parsed->type() != json::ArrayType) {
    LOG(ERROR) << "Response is not an array!";
    return result;
  }
  const json::Array* parsed_array = parsed->As<json::Array>();
  //result.emplace_back("", MonitoredResource("", {}), "");
  for (const std::unique_ptr<json::Value>& element : *parsed_array) {
    if (element->type() != json::ObjectType) {
      LOG(ERROR) << "Element " << *element << " is not an object!";
      continue;
    }
    const json::Object* container = element->As<json::Object>();
    auto id_it = container->find("Id");
    if (id_it == container->end()) {
      LOG(ERROR) << "There is no container id in " << *container;
      continue;
    }
    if (id_it->second->type() != json::StringType) {
      LOG(ERROR) << "Container id " << *id_it->second << " is not a string";
      continue;
    }
    const std::string id = id_it->second->As<json::String>()->value();
    const MonitoredResource resource("docker_container", {
      {"project_id", project_id},
      {"location", zone},
      {"container_id", id},
    });
    result.emplace_back("container/" + id, resource, container->ToJSON());
  }
  return result;
}

}