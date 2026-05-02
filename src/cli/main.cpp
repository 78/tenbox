#include "client/client.h"
#include "version.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {

void PrintUsage(const char* prog) {
    std::cerr
        << "TenBox CLI v" TENBOX_VERSION "\n\n"
        << "Usage:\n"
        << "  " << prog << " doctor\n"
        << "  " << prog << " system info\n"
        << "  " << prog << " vm ls\n"
        << "  " << prog << " vm create --name NAME --kernel PATH [--initrd PATH] [--disk PATH] [--memory MB] [--cpus N]\n"
        << "  " << prog << " vm edit <id> [--name NAME] [--memory MB] [--cpus N] [--debug on|off] [--net on|off]\n"
        << "  " << prog << " vm start <id>\n"
        << "  " << prog << " vm stop <id>\n"
        << "  " << prog << " vm reboot <id>\n"
        << "  " << prog << " vm shutdown <id>\n"
        << "  " << prog << " vm rm <id>\n"
        << "  " << prog << " vm console <id>\n"
        << "  " << prog << " vm logs <id> [--lines N]\n";
}

int PrintResponse(const tenbox::client::Response& response) {
    if (!response.ok) {
        std::cerr << (response.error.empty() ? "request failed" : response.error) << "\n";
        if (!response.body.is_null()) {
            std::cerr << response.body.dump(2) << "\n";
        }
        return 1;
    }
    std::cout << response.body["payload"].dump(2) << "\n";
    return 0;
}

std::string RequireValue(int& i, int argc, char** argv, const std::string& flag) {
    if (i + 1 >= argc) {
        std::cerr << "missing value for " << flag << "\n";
        std::exit(2);
    }
    return argv[++i];
}

std::string AbsolutePath(const std::string& path) {
    return std::filesystem::absolute(path).lexically_normal().string();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 2;
    }

    tenbox::client::Client client;
    const std::string top = argv[1];

    if (top == "--version" || top == "version") {
        std::cout << TENBOX_VERSION << "\n";
        return 0;
    }
    if (top == "--help" || top == "help") {
        PrintUsage(argv[0]);
        return 0;
    }
    if (top == "doctor") {
        return PrintResponse(client.Request({{"type", "doctor"}}));
    }
    if (top == "system" && argc >= 3 && std::string(argv[2]) == "info") {
        return PrintResponse(client.Request({{"type", "system.info"}}));
    }
    if (top != "vm" || argc < 3) {
        PrintUsage(argv[0]);
        return 2;
    }

    const std::string cmd = argv[2];
    if (cmd == "ls") {
        return PrintResponse(client.Request({{"type", "vm.list"}}));
    }
    if (cmd == "create") {
        nlohmann::json payload = nlohmann::json::object();
        for (int i = 3; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--name") payload["name"] = RequireValue(i, argc, argv, arg);
            else if (arg == "--kernel") payload["kernel"] = AbsolutePath(RequireValue(i, argc, argv, arg));
            else if (arg == "--initrd") payload["initrd"] = AbsolutePath(RequireValue(i, argc, argv, arg));
            else if (arg == "--disk") payload["disk"] = AbsolutePath(RequireValue(i, argc, argv, arg));
            else if (arg == "--memory") payload["memory_mb"] = std::stoull(RequireValue(i, argc, argv, arg));
            else if (arg == "--cpus") payload["cpu_count"] = std::stoul(RequireValue(i, argc, argv, arg));
            else {
                std::cerr << "unknown option: " << arg << "\n";
                return 2;
            }
        }
        return PrintResponse(client.Request({{"type", "vm.create"}, {"payload", payload}}));
    }
    if (cmd == "edit" && argc >= 4) {
        nlohmann::json payload = nlohmann::json::object();
        for (int i = 4; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--name") payload["name"] = RequireValue(i, argc, argv, arg);
            else if (arg == "--memory") payload["memory_mb"] = std::stoull(RequireValue(i, argc, argv, arg));
            else if (arg == "--cpus") payload["cpu_count"] = std::stoul(RequireValue(i, argc, argv, arg));
            else if (arg == "--debug") {
                const auto value = RequireValue(i, argc, argv, arg);
                payload["debug_mode"] = value == "on" || value == "true" || value == "1";
            } else if (arg == "--net") {
                const auto value = RequireValue(i, argc, argv, arg);
                payload["net_enabled"] = value == "on" || value == "true" || value == "1";
            } else {
                std::cerr << "unknown option: " << arg << "\n";
                return 2;
            }
        }
        return PrintResponse(client.Request({{"type", "vm.edit"}, {"vm_id", argv[3]}, {"payload", payload}}));
    }
    if (cmd == "start" && argc >= 4) {
        return PrintResponse(client.Request({{"type", "vm.start"}, {"vm_id", argv[3]}}));
    }
    if (cmd == "stop" && argc >= 4) {
        return PrintResponse(client.Request({{"type", "vm.stop"}, {"vm_id", argv[3]}}));
    }
    if (cmd == "reboot" && argc >= 4) {
        return PrintResponse(client.Request({{"type", "vm.reboot"}, {"vm_id", argv[3]}}));
    }
    if (cmd == "shutdown" && argc >= 4) {
        return PrintResponse(client.Request({{"type", "vm.shutdown"}, {"vm_id", argv[3]}}));
    }
    if ((cmd == "rm" || cmd == "delete") && argc >= 4) {
        return PrintResponse(client.Request({{"type", "vm.delete"}, {"vm_id", argv[3]}}));
    }
    if (cmd == "console" && argc >= 4) {
        return client.AttachConsole(argv[3]);
    }
    if (cmd == "logs" && argc >= 4) {
        int lines = 200;
        bool follow = false;
        for (int i = 4; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--lines") lines = std::stoi(RequireValue(i, argc, argv, arg));
            else if (arg == "-f" || arg == "--follow") follow = true;
            else {
                std::cerr << "unknown option: " << arg << "\n";
                return 2;
            }
        }
        // First print the historical tail so `-f` users see context, then
        // (if requested) hand off to the streaming follower. The follower
        // takes over the same stdout, so the user sees an unbroken log.
        const auto rc = PrintResponse(client.Request({{"type", "vm.logs"}, {"vm_id", argv[3]}, {"lines", lines}}));
        if (!follow || rc != 0) return rc;
        return client.FollowLogs(argv[3]);
    }

    PrintUsage(argv[0]);
    return 2;
}
