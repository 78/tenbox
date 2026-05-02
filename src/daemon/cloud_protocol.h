#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace tenbox::daemon {

struct CloudEnvelope {
    std::string id;
    std::string type;
    std::string host_id;
    std::string vm_id;
    nlohmann::json payload = nlohmann::json::object();
};

nlohmann::json ToJson(const CloudEnvelope& envelope);
bool FromJson(const nlohmann::json& value, CloudEnvelope& envelope, std::string* error);

}  // namespace tenbox::daemon
