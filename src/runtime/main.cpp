#include "runtime/runtime_service.h"
#include "version.h"
#include "core/vmm/vm.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

static void PrintVersion() {
    fprintf(stderr, "TenBox vm-runtime v" TENBOX_VERSION "\n");
}

static void PrintUsage(const char* prog) {
    PrintVersion();
    fprintf(stderr,
        "\nUsage: %s --kernel <path> [options]\n"
        "\n"
        "Options:\n"
        "  --vm-id <id>         Runtime vm id (default: default)\n"
        "  --control-endpoint <name>\n"
        "                       Named pipe endpoint (without \\\\.\\pipe\\ prefix)\n"
        "  --interactive on|off Console mode (default: on)\n"
        "  --kernel <path>      Path to vmlinuz (required)\n"
        "  --initrd <path>      Path to initramfs\n"
        "  --disk <path>        Path to raw / qcow2 disk image\n"
        "  --cmdline <str>      Kernel command line\n"
        "  --memory <MB>        Guest RAM in MB (default: 256)\n"
        "  --cpus <N>           Number of vCPUs (default: 1, max: 128)\n"
        "  --net                Start with network link up (default: link down)\n"
        "  --forward H:G        Port forward host:H -> guest:G (repeatable)\n"
        "  --share TAG:PATH[:ro] Share host directory (repeatable)\n"
        "  --version            Show version\n"
        "  --help               Show this help\n",
        prog);
}

int main(int argc, char* argv[]) {
    // Set line buffering for stdout/stderr so logs flush on newline
    setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
    setvbuf(stderr, nullptr, _IOLBF, BUFSIZ);

    VmConfig config;
    std::string vm_id = "default";
    std::string control_endpoint;

    for (int i = 1; i < argc; i++) {
        auto Arg = [&](const char* flag) {
            return std::strcmp(argv[i], flag) == 0;
        };
        auto NextArg = [&]() -> const char* {
            if (i + 1 < argc) return argv[++i];
            fprintf(stderr, "Missing value for %s\n", argv[i]);
            return nullptr;
        };

        if (Arg("--vm-id")) {
            auto v = NextArg(); if (!v) return 1;
            vm_id = v;
        } else if (Arg("--control-endpoint")) {
            auto v = NextArg(); if (!v) return 1;
            control_endpoint = v;
        } else if (Arg("--interactive")) {
            auto v = NextArg(); if (!v) return 1;
            if (std::strcmp(v, "on") == 0) {
                config.interactive = true;
            } else if (std::strcmp(v, "off") == 0) {
                config.interactive = false;
            } else {
                fprintf(stderr, "Invalid --interactive value: %s\n", v);
                return 1;
            }
        } else if (Arg("--kernel")) {
            auto v = NextArg(); if (!v) return 1;
            config.kernel_path = v;
        } else if (Arg("--initrd")) {
            auto v = NextArg(); if (!v) return 1;
            config.initrd_path = v;
        } else if (Arg("--disk")) {
            auto v = NextArg(); if (!v) return 1;
            config.disk_path = v;
        } else if (Arg("--cmdline")) {
            auto v = NextArg(); if (!v) return 1;
            config.cmdline = v;
        } else if (Arg("--memory")) {
            auto v = NextArg(); if (!v) return 1;
            config.memory_mb = std::atoi(v);
        } else if (Arg("--cpus")) {
            auto v = NextArg(); if (!v) return 1;
            config.cpu_count = std::atoi(v);
        } else if (Arg("--net")) {
            config.net_link_up = true;
        } else if (Arg("--forward")) {
            auto v = NextArg(); if (!v) return 1;
            unsigned hp = 0, gp = 0;
            if (std::sscanf(v, "%u:%u", &hp, &gp) == 2 && hp && gp) {
                config.port_forwards.push_back({
                    static_cast<uint16_t>(hp),
                    static_cast<uint16_t>(gp)});
            } else {
                fprintf(stderr, "Invalid --forward format: %s (expected H:G)\n", v);
                return 1;
            }
        } else if (Arg("--share")) {
            auto v = NextArg(); if (!v) return 1;
            std::string arg(v);
            VmSharedFolder sf;
            sf.readonly = false;
            
            size_t first_colon = arg.find(':');
            if (first_colon == std::string::npos) {
                fprintf(stderr, "Invalid --share format: %s (expected TAG:PATH[:ro])\n", v);
                return 1;
            }
            sf.tag = arg.substr(0, first_colon);
            std::string rest = arg.substr(first_colon + 1);
            
            if (rest.size() >= 3 && rest.substr(rest.size() - 3) == ":ro") {
                sf.readonly = true;
                rest = rest.substr(0, rest.size() - 3);
            }
            sf.host_path = rest;
            
            if (sf.tag.empty() || sf.host_path.empty()) {
                fprintf(stderr, "Invalid --share format: %s (empty tag or path)\n", v);
                return 1;
            }
            config.shared_folders.push_back(sf);
        } else if (Arg("--version") || Arg("-v")) {
            PrintVersion();
            return 0;
        } else if (Arg("--help") || Arg("-h")) {
            PrintUsage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            PrintUsage(argv[0]);
            return 1;
        }
    }

    if (config.kernel_path.empty()) {
        fprintf(stderr, "Error: --kernel is required\n\n");
        PrintUsage(argv[0]);
        return 1;
    }
    if (config.memory_mb < 16) {
        fprintf(stderr, "Error: minimum memory is 16 MB\n");
        return 1;
    }
    if (config.cpu_count < 1 || config.cpu_count > 128) {
        fprintf(stderr, "Error: --cpus must be between 1 and 128\n");
        return 1;
    }

    std::unique_ptr<RuntimeControlService> control;
    if (!control_endpoint.empty()) {
        control = std::make_unique<RuntimeControlService>(vm_id, control_endpoint);
        if (!control->Start()) {
            fprintf(stderr, "Failed to start runtime control service\n");
            return 1;
        }
        control->PublishState("starting");
        config.console_port = control->ConsolePort();
        config.input_port = control->GetInputPort();
        config.display_port = control->GetDisplayPort();
        config.clipboard_port = control->GetClipboardPort();
        config.audio_port = control->GetAudioPort();
    }

    auto vm = Vm::Create(config);
    if (!vm) {
        if (control) control->PublishState("crashed", 1);
        fprintf(stderr, "Failed to create VM\n");
        return 1;
    }
    if (control) control->AttachVm(vm.get());
    if (control) control->PublishState("running");

    int exit_code = vm->Run();
    bool wants_reboot = vm->RebootRequested();
    if (control) {
        if (wants_reboot) {
            control->PublishState("rebooting", 0);
        } else {
            control->PublishState(exit_code == 0 ? "stopped" : "crashed", exit_code);
        }
        control->Stop();
    }
    return wants_reboot ? 128 : exit_code;
}
