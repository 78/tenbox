#include "vmm/vm.h"
#include "version.h"
#include <cstdlib>
#include <cstring>

static void PrintVersion() {
    fprintf(stderr, "TenBox v" TENBOX_VERSION "\n");
}

static void PrintUsage(const char* prog) {
    PrintVersion();
    fprintf(stderr,
        "\nUsage: %s --kernel <path> [options]\n"
        "\n"
        "Options:\n"
        "  --kernel <path>    Path to vmlinuz (required)\n"
        "  --initrd <path>    Path to initramfs\n"
        "  --disk <path>      Path to raw / qcow2 disk image\n"
        "  --cmdline <str>    Kernel command line\n"
        "                     (default: \"console=ttyS0 earlyprintk=serial\")\n"
        "  --memory <MB>      Guest RAM in MB (default: 256)\n"
        "  --cpus <N>         Number of vCPUs (default: 1, max: 128)\n"
        "  --net              Enable virtio-net with NAT networking\n"
        "  --forward H:G      Port forward host:H -> guest:G (repeatable)\n"
        "  --version          Show version\n"
        "  --help             Show this help\n",
        prog);
}

int main(int argc, char* argv[]) {
    VmConfig config;

    for (int i = 1; i < argc; i++) {
        auto Arg = [&](const char* flag) {
            return strcmp(argv[i], flag) == 0;
        };
        auto NextArg = [&]() -> const char* {
            if (i + 1 < argc) return argv[++i];
            fprintf(stderr, "Missing value for %s\n", argv[i]);
            return nullptr;
        };

        if (Arg("--kernel")) {
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
            config.memory_mb = atoi(v);
        } else if (Arg("--cpus")) {
            auto v = NextArg(); if (!v) return 1;
            config.cpu_count = atoi(v);
        } else if (Arg("--net")) {
            config.net_enabled = true;
        } else if (Arg("--forward")) {
            auto v = NextArg(); if (!v) return 1;
            // Parse "hostPort:guestPort"
            unsigned hp = 0, gp = 0;
            if (sscanf(v, "%u:%u", &hp, &gp) == 2 && hp && gp) {
                config.port_forwards.push_back({
                    static_cast<uint16_t>(hp),
                    static_cast<uint16_t>(gp)});
            } else {
                fprintf(stderr, "Invalid --forward format: %s (expected H:G)\n", v);
                return 1;
            }
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

    LOG_INFO("TenBox VMM v" TENBOX_VERSION);
    LOG_INFO("Kernel: %s", config.kernel_path.c_str());
    if (!config.initrd_path.empty())
        LOG_INFO("Initrd: %s", config.initrd_path.c_str());
    if (!config.disk_path.empty())
        LOG_INFO("Disk: %s", config.disk_path.c_str());
    if (config.net_enabled)
        LOG_INFO("Network: enabled (NAT)");
    for (auto& f : config.port_forwards)
        LOG_INFO("Forward: host:%u -> guest:%u", f.host_port, f.guest_port);
    LOG_INFO("Cmdline: %s", config.cmdline.c_str());
    LOG_INFO("Memory: %llu MB", config.memory_mb);
    LOG_INFO("CPUs: %u", config.cpu_count);

    auto vm = Vm::Create(config);
    if (!vm) {
        LOG_ERROR("Failed to create VM");
        return 1;
    }

    return vm->Run();
}
