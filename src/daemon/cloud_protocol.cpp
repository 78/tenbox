#include "daemon/cloud_protocol.h"

namespace tenbox::daemon {

nlohmann::json ToJson(const CloudEnvelope& envelope) {
    nlohmann::json json = {
        {"id", envelope.id},
        {"type", envelope.type},
        {"host_id", envelope.host_id},
        {"payload", envelope.payload},
    };
    if (!envelope.vm_id.empty()) json["vm_id"] = envelope.vm_id;
    return json;
}

bool FromJson(const nlohmann::json& value, CloudEnvelope& envelope, std::string* error) {
    if (!value.is_object()) {
        if (error) *error = "cloud envelope must be an object";
        return false;
    }
    envelope.id = value.value("id", "");
    envelope.type = value.value("type", "");
    envelope.host_id = value.value("host_id", "");
    envelope.vm_id = value.value("vm_id", "");
    envelope.payload = value.value("payload", nlohmann::json::object());
    if (envelope.type.empty()) {
        if (error) *error = "cloud envelope type is required";
        return false;
    }
    return true;
}

}  // namespace tenbox::daemon
