#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>

namespace tenbox::daemon {

struct RemoteSession {
    std::string session_id;
    std::string vm_id;
    std::string owner_user_id;
    int64_t created_at = 0;
};

class RemoteSessionRegistry {
public:
    std::optional<RemoteSession> Create(const std::string& vm_id,
                                        const std::string& owner_user_id,
                                        bool force);
    bool Close(const std::string& vm_id, const std::string& session_id);
    std::optional<RemoteSession> GetByVm(const std::string& vm_id) const;

private:
    std::unordered_map<std::string, RemoteSession> by_vm_;
};

nlohmann::json ToJson(const RemoteSession& session);

}  // namespace tenbox::daemon
