#pragma once

#include "core/device/virtio/virtio_mmio.h"
#include <string>
#include <unordered_map>
#include <mutex>

#define NOMINMAX
#include <windows.h>

// VirtIO FS device ID
constexpr uint32_t VIRTIO_ID_FS = 26;

// Common VirtIO feature bits (guarded to avoid redefinition with virtio_blk.h)
#ifndef VIRTIO_F_VERSION_1_DEFINED
#define VIRTIO_F_VERSION_1_DEFINED
constexpr uint64_t VIRTIO_F_VERSION_1 = 1ULL << 32;
#endif

// VirtIO FS feature bits
constexpr uint64_t VIRTIO_FS_F_NOTIFICATION = 1ULL << 0;

// FUSE protocol version
constexpr uint32_t FUSE_KERNEL_VERSION = 7;
constexpr uint32_t FUSE_KERNEL_MINOR_VERSION = 31;

// FUSE opcodes
enum FuseOpcode : uint32_t {
    FUSE_LOOKUP      = 1,
    FUSE_FORGET      = 2,
    FUSE_GETATTR     = 3,
    FUSE_SETATTR     = 4,
    FUSE_READLINK    = 5,
    FUSE_SYMLINK     = 6,
    FUSE_MKNOD       = 8,
    FUSE_MKDIR       = 9,
    FUSE_UNLINK      = 10,
    FUSE_RMDIR       = 11,
    FUSE_RENAME      = 12,
    FUSE_LINK        = 13,
    FUSE_OPEN        = 14,
    FUSE_READ        = 15,
    FUSE_WRITE       = 16,
    FUSE_STATFS      = 17,
    FUSE_RELEASE     = 18,
    FUSE_FSYNC       = 20,
    FUSE_SETXATTR    = 21,
    FUSE_GETXATTR    = 22,
    FUSE_LISTXATTR   = 23,
    FUSE_REMOVEXATTR = 24,
    FUSE_FLUSH       = 25,
    FUSE_INIT        = 26,
    FUSE_OPENDIR     = 27,
    FUSE_READDIR     = 28,
    FUSE_RELEASEDIR  = 29,
    FUSE_FSYNCDIR    = 30,
    FUSE_GETLK       = 31,
    FUSE_SETLK       = 32,
    FUSE_SETLKW      = 33,
    FUSE_ACCESS      = 34,
    FUSE_CREATE      = 35,
    FUSE_INTERRUPT   = 36,
    FUSE_BMAP        = 37,
    FUSE_DESTROY     = 38,
    FUSE_IOCTL       = 39,
    FUSE_POLL        = 40,
    FUSE_NOTIFY_REPLY = 41,
    FUSE_BATCH_FORGET = 42,
    FUSE_FALLOCATE   = 43,
    FUSE_READDIRPLUS = 44,
    FUSE_RENAME2     = 45,
    FUSE_LSEEK       = 46,
    FUSE_COPY_FILE_RANGE = 47,
    FUSE_SETUPMAPPING = 48,
    FUSE_REMOVEMAPPING = 49,
};

// FUSE error codes (negative errno values)
constexpr int32_t FUSE_OK = 0;
constexpr int32_t FUSE_ENOENT = -2;
constexpr int32_t FUSE_EIO = -5;
constexpr int32_t FUSE_EACCES = -13;
constexpr int32_t FUSE_EEXIST = -17;
constexpr int32_t FUSE_ENOTDIR = -20;
constexpr int32_t FUSE_EISDIR = -21;
constexpr int32_t FUSE_EINVAL = -22;
constexpr int32_t FUSE_ENOSPC = -28;
constexpr int32_t FUSE_EROFS = -30;
constexpr int32_t FUSE_ENOTEMPTY = -39;
constexpr int32_t FUSE_ENOSYS = -38;
constexpr int32_t FUSE_ENODATA = -61;

// FUSE file types (for mode)
constexpr uint32_t FUSE_S_IFMT   = 0170000;
constexpr uint32_t FUSE_S_IFDIR  = 0040000;
constexpr uint32_t FUSE_S_IFREG  = 0100000;
constexpr uint32_t FUSE_S_IFLNK  = 0120000;

// FUSE init flags
constexpr uint32_t FUSE_ASYNC_READ       = 1 << 0;
constexpr uint32_t FUSE_POSIX_LOCKS      = 1 << 1;
constexpr uint32_t FUSE_FILE_OPS         = 1 << 2;
constexpr uint32_t FUSE_ATOMIC_O_TRUNC   = 1 << 3;
constexpr uint32_t FUSE_EXPORT_SUPPORT   = 1 << 4;
constexpr uint32_t FUSE_BIG_WRITES       = 1 << 5;
constexpr uint32_t FUSE_DONT_MASK        = 1 << 6;
constexpr uint32_t FUSE_WRITEBACK_CACHE  = 1 << 16;
constexpr uint32_t FUSE_NO_OPEN_SUPPORT  = 1 << 17;
constexpr uint32_t FUSE_PARALLEL_DIROPS  = 1 << 18;
constexpr uint32_t FUSE_HANDLE_KILLPRIV  = 1 << 19;
constexpr uint32_t FUSE_POSIX_ACL        = 1 << 20;
constexpr uint32_t FUSE_READDIRPLUS_AUTO = 1 << 29;

// FUSE setattr valid bits
constexpr uint32_t FATTR_MODE  = 1 << 0;
constexpr uint32_t FATTR_UID   = 1 << 1;
constexpr uint32_t FATTR_GID   = 1 << 2;
constexpr uint32_t FATTR_SIZE  = 1 << 3;
constexpr uint32_t FATTR_ATIME = 1 << 4;
constexpr uint32_t FATTR_MTIME = 1 << 5;

#pragma pack(push, 1)

struct FuseInHeader {
    uint32_t len;
    uint32_t opcode;
    uint64_t unique;
    uint64_t nodeid;
    uint32_t uid;
    uint32_t gid;
    uint32_t pid;
    uint32_t padding;
};

struct FuseOutHeader {
    uint32_t len;
    int32_t error;
    uint64_t unique;
};

struct FuseInitIn {
    uint32_t major;
    uint32_t minor;
    uint32_t max_readahead;
    uint32_t flags;
};

struct FuseInitOut {
    uint32_t major;
    uint32_t minor;
    uint32_t max_readahead;
    uint32_t flags;
    uint16_t max_background;
    uint16_t congestion_threshold;
    uint32_t max_write;
    uint32_t time_gran;
    uint16_t max_pages;
    uint16_t map_alignment;
    uint32_t unused[8];
};

struct FuseAttr {
    uint64_t ino;
    uint64_t size;
    uint64_t blocks;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t atimensec;
    uint32_t mtimensec;
    uint32_t ctimensec;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint32_t rdev;
    uint32_t blksize;
    uint32_t padding;
};

struct FuseEntryOut {
    uint64_t nodeid;
    uint64_t generation;
    uint64_t entry_valid;
    uint64_t attr_valid;
    uint32_t entry_valid_nsec;
    uint32_t attr_valid_nsec;
    FuseAttr attr;
};

struct FuseAttrOut {
    uint64_t attr_valid;
    uint32_t attr_valid_nsec;
    uint32_t dummy;
    FuseAttr attr;
};

struct FuseGetAttrIn {
    uint32_t getattr_flags;
    uint32_t dummy;
    uint64_t fh;
};

struct FuseSetAttrIn {
    uint32_t valid;
    uint32_t padding;
    uint64_t fh;
    uint64_t size;
    uint64_t lock_owner;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t atimensec;
    uint32_t mtimensec;
    uint32_t ctimensec;
    uint32_t mode;
    uint32_t unused4;
    uint32_t uid;
    uint32_t gid;
    uint32_t unused5;
};

struct FuseOpenIn {
    uint32_t flags;
    uint32_t unused;
};

struct FuseOpenOut {
    uint64_t fh;
    uint32_t open_flags;
    uint32_t padding;
};

struct FuseReadIn {
    uint64_t fh;
    uint64_t offset;
    uint32_t size;
    uint32_t read_flags;
    uint64_t lock_owner;
    uint32_t flags;
    uint32_t padding;
};

struct FuseWriteIn {
    uint64_t fh;
    uint64_t offset;
    uint32_t size;
    uint32_t write_flags;
    uint64_t lock_owner;
    uint32_t flags;
    uint32_t padding;
};

struct FuseWriteOut {
    uint32_t size;
    uint32_t padding;
};

struct FuseReleaseIn {
    uint64_t fh;
    uint32_t flags;
    uint32_t release_flags;
    uint64_t lock_owner;
};

struct FuseMkdirIn {
    uint32_t mode;
    uint32_t umask;
};

struct FuseCreateIn {
    uint32_t flags;
    uint32_t mode;
    uint32_t umask;
    uint32_t padding;
};

struct FuseStatfsOut {
    uint64_t blocks;
    uint64_t bfree;
    uint64_t bavail;
    uint64_t files;
    uint64_t ffree;
    uint32_t bsize;
    uint32_t namelen;
    uint32_t frsize;
    uint32_t padding;
    uint32_t spare[6];
};

struct FuseDirent {
    uint64_t ino;
    uint64_t off;
    uint32_t namelen;
    uint32_t type;
    // char name[] follows
};

struct FuseDirentplus {
    FuseEntryOut entry_out;
    FuseDirent dirent;
    // char name[] follows
};

struct FuseRenameIn {
    uint64_t newdir;
};

struct FuseRename2In {
    uint64_t newdir;
    uint32_t flags;
    uint32_t padding;
};

struct FuseForgetIn {
    uint64_t nlookup;
};

// VirtIO FS config space
struct VirtioFsConfig {
    char tag[36];
    uint32_t num_request_queues;
};

#pragma pack(pop)

// Shared folder info
struct ShareInfo {
    std::string tag;
    std::wstring host_path;
    bool readonly;
    uint64_t root_inode;  // inode of the share's root directory
};

// Inode info cached on the host
struct InodeInfo {
    uint64_t inode;
    std::wstring host_path;
    uint64_t nlookup;
    bool is_dir;
    std::string share_tag;  // which share this inode belongs to (empty for virtual root)
};

// Open file handle
struct FileHandle {
    HANDLE handle;
    bool is_dir;
    std::wstring path;
    std::string share_tag;
};

class VirtioFsDevice : public VirtioDeviceOps {
public:
    // Create a virtiofs device with a fixed tag (e.g., "shared")
    // The device starts empty; use AddShare/RemoveShare to manage shares
    explicit VirtioFsDevice(const std::string& mount_tag = "shared");
    ~VirtioFsDevice() override;

    void SetMmioDevice(VirtioMmioDevice* mmio) { mmio_ = mmio; }

    // Dynamic share management - can be called at runtime
    bool AddShare(const std::string& tag, const std::string& host_path, bool readonly = false);
    bool RemoveShare(const std::string& tag);
    std::vector<std::string> GetShareTags() const;
    bool HasShare(const std::string& tag) const;

    // VirtioDeviceOps interface
    uint32_t GetDeviceId() const override { return VIRTIO_ID_FS; }
    uint64_t GetDeviceFeatures() const override;
    uint32_t GetNumQueues() const override { return 2; }
    uint32_t GetQueueMaxSize(uint32_t queue_idx) const override { return 128; }
    void OnQueueNotify(uint32_t queue_idx, VirtQueue& vq) override;
    void ReadConfig(uint32_t offset, uint8_t size, uint32_t* value) override;
    void WriteConfig(uint32_t offset, uint8_t size, uint32_t value) override;
    void OnStatusChange(uint32_t new_status) override;

    // State query
    uint32_t GetOpenHandleCount() const;

private:
    void ProcessRequest(VirtQueue& vq, uint16_t head_idx);
    
    // FUSE request handlers
    void HandleInit(const FuseInHeader* in_hdr, const uint8_t* in_data,
                    std::vector<uint8_t>& out_buf);
    void HandleLookup(const FuseInHeader* in_hdr, const uint8_t* in_data, uint32_t in_len,
                      std::vector<uint8_t>& out_buf);
    void HandleForget(const FuseInHeader* in_hdr, const uint8_t* in_data);
    void HandleGetAttr(const FuseInHeader* in_hdr, const uint8_t* in_data,
                       std::vector<uint8_t>& out_buf);
    void HandleSetAttr(const FuseInHeader* in_hdr, const uint8_t* in_data,
                       std::vector<uint8_t>& out_buf);
    void HandleOpen(const FuseInHeader* in_hdr, const uint8_t* in_data,
                    std::vector<uint8_t>& out_buf);
    void HandleRead(const FuseInHeader* in_hdr, const uint8_t* in_data,
                    std::vector<uint8_t>& out_buf);
    void HandleWrite(const FuseInHeader* in_hdr, const uint8_t* in_data, uint32_t in_len,
                     std::vector<uint8_t>& out_buf);
    void HandleRelease(const FuseInHeader* in_hdr, const uint8_t* in_data);
    void HandleOpenDir(const FuseInHeader* in_hdr, const uint8_t* in_data,
                       std::vector<uint8_t>& out_buf);
    void HandleReadDir(const FuseInHeader* in_hdr, const uint8_t* in_data,
                       std::vector<uint8_t>& out_buf);
    void HandleReadDirPlus(const FuseInHeader* in_hdr, const uint8_t* in_data,
                           std::vector<uint8_t>& out_buf);
    void HandleReleaseDir(const FuseInHeader* in_hdr, const uint8_t* in_data);
    void HandleStatFs(const FuseInHeader* in_hdr, std::vector<uint8_t>& out_buf);
    void HandleCreate(const FuseInHeader* in_hdr, const uint8_t* in_data, uint32_t in_len,
                      std::vector<uint8_t>& out_buf);
    void HandleMkdir(const FuseInHeader* in_hdr, const uint8_t* in_data, uint32_t in_len,
                     std::vector<uint8_t>& out_buf);
    void HandleUnlink(const FuseInHeader* in_hdr, const uint8_t* in_data, uint32_t in_len,
                      std::vector<uint8_t>& out_buf);
    void HandleRmdir(const FuseInHeader* in_hdr, const uint8_t* in_data, uint32_t in_len,
                     std::vector<uint8_t>& out_buf);
    void HandleRename(const FuseInHeader* in_hdr, const uint8_t* in_data, uint32_t in_len,
                      std::vector<uint8_t>& out_buf);
    void HandleFlush(const FuseInHeader* in_hdr, const uint8_t* in_data);
    void HandleFsync(const FuseInHeader* in_hdr, const uint8_t* in_data);

    // Helper functions
    void WriteErrorResponse(std::vector<uint8_t>& out_buf, uint64_t unique, int32_t error);
    int32_t FillAttr(const std::wstring& path, FuseAttr* attr, uint64_t inode, bool share_readonly = false);
    int32_t FillVirtualRootAttr(FuseAttr* attr);
    int32_t FillShareRootAttr(const ShareInfo& share, FuseAttr* attr);
    int32_t WindowsErrorToFuse(DWORD error);
    uint64_t AllocInode();
    InodeInfo* GetInode(uint64_t inode);
    uint64_t GetOrCreateInode(const std::wstring& path, bool is_dir, const std::string& share_tag);
    void RemoveInode(uint64_t inode);
    uint64_t AllocFileHandle(HANDLE h, bool is_dir, const std::wstring& path, const std::string& share_tag);
    FileHandle* GetFileHandle(uint64_t fh);
    void CloseFileHandle(uint64_t fh);
    std::wstring NodeIdToPath(uint64_t nodeid);
    std::string NodeIdToShareTag(uint64_t nodeid);
    bool IsShareReadonly(const std::string& share_tag);
    std::wstring Utf8ToWide(const std::string& utf8);
    std::string WideToUtf8(const std::wstring& wide);

    VirtioMmioDevice* mmio_ = nullptr;
    std::string mount_tag_;  // virtiofs mount tag (e.g., "shared")
    VirtioFsConfig config_{};
    bool initialized_ = false;

    mutable std::mutex mutex_;
    uint64_t next_inode_ = 2;  // inode 1 is reserved for virtual root
    uint64_t shares_version_ = 0;  // bumped on AddShare/RemoveShare
    uint64_t virtual_root_mtime_ = 0;  // updated on share changes for cache invalidation
    uint64_t next_fh_ = 1;
    
    // Shares: tag -> ShareInfo
    std::unordered_map<std::string, ShareInfo> shares_;
    
    // Inode management
    std::unordered_map<uint64_t, InodeInfo> inodes_;
    std::unordered_map<std::wstring, uint64_t> path_to_inode_;
    std::unordered_map<uint64_t, FileHandle> file_handles_;
};
