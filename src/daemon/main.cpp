#include "daemon/daemon_types.h"
#include "daemon/cloud_tunnel.h"
#include "daemon/kvm_doctor.h"
#include "daemon/rpc_server.h"
#include "daemon/runtime_manager.h"
#include "daemon/vm_store.h"
#include "common/image_source.h"
#include "version.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

// Production cloud tunnel; overridable via --cloud-url or TENBOX_CLOUD_URL.
// For local dev set TENBOX_CLOUD_URL=ws://127.0.0.1:18080/api/device-tunnel
// before launching tenboxd, or pass --cloud-url with the same value.
constexpr const char* kDefaultCloudUrl = "wss://my.tenbox.ai/api/device-tunnel";

void PrintUsage(const char* prog) {
    std::cerr
        << "TenBox daemon v" TENBOX_VERSION "\n\n"
        << "Usage: " << prog << " [options]\n\n"
        << "Options:\n"
        << "  --data-dir <path>      Override data directory for development/testing\n"
        << "  --socket <path>        Override Unix socket path for development/testing\n"
        << "  --runtime <path>       Override path to the tenbox-vm-runtime binary\n"
        << "  --cloud-url <url>      Cloud tunnel WS/WSS URL (default: " << kDefaultCloudUrl << ")\n"
        << "                         Also reads TENBOX_CLOUD_URL when --cloud-url is omitted.\n"
        << "                         Pass an empty value to disable cloud connectivity.\n"
        << "  --doctor              Run KVM support check and exit\n"
        << "  --version             Show version\n"
        << "  --help                Show help\n";
}

}  // namespace

int main(int argc, char** argv) {
    tenbox::daemon::DaemonConfig config;
    config.data_dir = tenbox::daemon::DefaultDataDir();
    config.socket_path = tenbox::daemon::DefaultSocketPath();
    config.runtime_path = (std::filesystem::absolute(argv[0]).parent_path() /
                           "tenbox-vm-runtime").string();
    if (const char* env = std::getenv("TENBOX_CLOUD_URL")) {
        config.cloud_url = env;
    } else {
        config.cloud_url = kDefaultCloudUrl;
    }

    bool doctor_only = false;
    bool cloud_url_explicit = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) return {};
            return argv[++i];
        };
        if (arg == "--data-dir") {
            config.data_dir = next();
        } else if (arg == "--socket") {
            config.socket_path = next();
        } else if (arg == "--runtime") {
            config.runtime_path = next();
        } else if (arg == "--cloud-url") {
            config.cloud_url = next();
            cloud_url_explicit = true;
        } else if (arg == "--doctor") {
            doctor_only = true;
        } else if (arg == "--version") {
            std::cout << TENBOX_VERSION << "\n";
            return 0;
        } else if (arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            PrintUsage(argv[0]);
            return 1;
        }
    }

    if (doctor_only) {
        const auto report = tenbox::daemon::RunKvmDoctor();
        std::cout << tenbox::daemon::ToJson(report).dump(2) << "\n";
        return report.supported ? 0 : 2;
    }

    tenbox::daemon::VmStore store(config.data_dir);
    std::string error;
    if (!store.Load(&error)) {
        std::cerr << "failed to load VM store: " << error << "\n";
        return 1;
    }

    // Sweep `images/` for half-finished downloads from a previous run that
    // was killed before the cache directory could be removed (SIGKILL,
    // power loss, etc.). Without this, hard-killed downloads accumulate
    // silently because the manifest never lands and IsImageCached/UI
    // both ignore the directory while disk usage keeps growing.
    const auto images_dir =
        (std::filesystem::path(config.data_dir) / "images").string();
    if (const size_t cleaned =
            image_source::CleanupStaleImageCache(images_dir);
        cleaned > 0) {
        std::cout << "cleaned up " << cleaned
                  << " stale image cache directory(ies) under " << images_dir
                  << "\n";
    }

    tenbox::daemon::RuntimeManager runtime_manager(config, store);
    tenbox::daemon::RpcServer rpc(config, store, runtime_manager);
    if (!rpc.Start(&error)) {
        std::cerr << "failed to start RPC server: " << error << "\n";
        return 1;
    }

    std::cout << "tenboxd listening on " << config.socket_path << "\n";
    if (!config.cloud_url.empty()) {
        std::cout << "cloud tunnel configured for " << config.cloud_url
                  << (cloud_url_explicit ? " (--cloud-url)" : " (default)") << "\n";
    } else {
        std::cout << "cloud tunnel disabled (cloud_url is empty)\n";
    }

    tenbox::daemon::CloudTunnel cloud_tunnel(config, store, runtime_manager);
    if (!cloud_tunnel.Start(&error)) {
        std::cerr << "failed to start cloud tunnel: " << error << "\n";
        return 1;
    }

    rpc.Run();
    return 0;
}
