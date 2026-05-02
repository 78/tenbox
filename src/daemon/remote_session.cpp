#include "daemon/remote_session.h"

#include "daemon/daemon_types.h"

namespace tenbox::daemon {

std::optional<RemoteSession> RemoteSessionRegistry::Create(
    const std::string& vm_id,
    const std::string& owner_user_id,
    bool force) {
    if (!force && by_vm_.count(vm_id)) return std::nullopt;

    RemoteSession session;
    session.session_id = GenerateUuid();
    session.vm_id = vm_id;
    session.owner_user_id = owner_user_id;
    session.created_at = UnixNow();
    by_vm_[vm_id] = session;
    return session;
}

bool RemoteSessionRegistry::Close(const std::string& vm_id, const std::string& session_id) {
    auto it = by_vm_.find(vm_id);
    if (it == by_vm_.end()) return false;
    if (it->second.session_id != session_id) return false;
    by_vm_.erase(it);
    return true;
}

std::optional<RemoteSession> RemoteSessionRegistry::GetByVm(const std::string& vm_id) const {
    auto it = by_vm_.find(vm_id);
    if (it == by_vm_.end()) return std::nullopt;
    return it->second;
}

nlohmann::json ToJson(const RemoteSession& session) {
    return {
        {"session_id", session.session_id},
        {"vm_id", session.vm_id},
        {"owner_user_id", session.owner_user_id},
        {"created_at", session.created_at},
    };
}

}  // namespace tenbox::daemon
