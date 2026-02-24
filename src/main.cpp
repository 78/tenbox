#include "vmm/vm.h"
#include <cstdlib>
#include <cstring>

static void PrintUsage(const char* prog) {
    fprintf(stderr,
        "Usage: %s --kernel <path> [options]\n"
        "\n"
        "Options:\n"
        "  --kernel <path>    Path to vmlinuz (required)\n"
        "  --initrd <path>    Path to initramfs\n"
        "  --disk <path>      Path to raw / qcow2 disk image\n"
        "  --cmdline <str>    Kernel command line\n"
        "                     (default: \"console=ttyS0 earlyprintk=serial\")\n"
        "  --memory <MB>      Guest RAM in MB (default: 256)\n"
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

    LOG_INFO("TenClaw VMM v0.1");
    LOG_INFO("Kernel: %s", config.kernel_path.c_str());
    if (!config.initrd_path.empty())
        LOG_INFO("Initrd: %s", config.initrd_path.c_str());
    if (!config.disk_path.empty())
        LOG_INFO("Disk: %s", config.disk_path.c_str());
    LOG_INFO("Cmdline: %s", config.cmdline.c_str());
    LOG_INFO("Memory: %llu MB", config.memory_mb);

    auto vm = Vm::Create(config);
    if (!vm) {
        LOG_ERROR("Failed to create VM");
        return 1;
    }

    return vm->Run();
}
