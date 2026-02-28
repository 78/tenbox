#include "core/device/virtio/virtio_fs.h"
#include "core/vmm/types.h"
#include <algorithm>
#include <cstring>

// Virtual root inode number
constexpr uint64_t VIRTUAL_ROOT_INODE = 1;

VirtioFsDevice::VirtioFsDevice(const std::string& mount_tag)
    : mount_tag_(mount_tag) {
    memset(&config_, 0, sizeof(config_));
    size_t tag_len = std::min(mount_tag_.size(), sizeof(config_.tag) - 1);
    memcpy(config_.tag, mount_tag_.c_str(), tag_len);
    config_.num_request_queues = 1;
    virtual_root_mtime_ = static_cast<uint64_t>(time(nullptr));

    // Create virtual root inode (inode 1) - this is the virtual directory containing all shares
    InodeInfo root;
    root.inode = VIRTUAL_ROOT_INODE;
    root.host_path = L"";  // virtual, no host path
    root.nlookup = 1;
    root.is_dir = true;
    root.share_tag = "";  // empty means virtual root
    inodes_[VIRTUAL_ROOT_INODE] = root;

    LOG_INFO("VirtIO FS: mount_tag=%s (virtual root with dynamic shares)", mount_tag_.c_str());
}

VirtioFsDevice::~VirtioFsDevice() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [fh, handle] : file_handles_) {
        if (handle.handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle.handle);
        }
    }
    file_handles_.clear();
}

bool VirtioFsDevice::AddShare(const std::string& tag, const std::string& host_path, bool readonly) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Check for duplicate tag
    if (shares_.find(tag) != shares_.end()) {
        LOG_ERROR("VirtIO FS: share tag '%s' already exists", tag.c_str());
        return false;
    }

    // Validate host path exists
    std::wstring wide_path = Utf8ToWide(host_path);
    DWORD attrs = GetFileAttributesW(wide_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        LOG_ERROR("VirtIO FS: host path '%s' is not a valid directory", host_path.c_str());
        return false;
    }

    // Normalize path (remove trailing backslash)
    while (wide_path.size() > 3 && wide_path.back() == L'\\') {
        wide_path.pop_back();
    }

    // Allocate inode for this share's root
    uint64_t share_root_inode = next_inode_++;
    
    ShareInfo share;
    share.tag = tag;
    share.host_path = wide_path;
    share.readonly = readonly;
    share.root_inode = share_root_inode;
    shares_[tag] = share;

    // Create inode for share root
    InodeInfo share_root;
    share_root.inode = share_root_inode;
    share_root.host_path = wide_path;
    share_root.nlookup = 1;
    share_root.is_dir = true;
    share_root.share_tag = tag;
    inodes_[share_root_inode] = share_root;
    path_to_inode_[wide_path] = share_root_inode;

    shares_version_++;
    virtual_root_mtime_ = static_cast<uint64_t>(time(nullptr));
    LOG_INFO("VirtIO FS: added share '%s' -> '%s' (readonly=%s, inode=%llu)", 
             tag.c_str(), host_path.c_str(), readonly ? "true" : "false", share_root_inode);
    return true;
}

bool VirtioFsDevice::RemoveShare(const std::string& tag) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = shares_.find(tag);
    if (it == shares_.end()) {
        LOG_ERROR("VirtIO FS: share tag '%s' not found", tag.c_str());
        return false;
    }

    // Close all file handles belonging to this share
    for (auto fh_it = file_handles_.begin(); fh_it != file_handles_.end(); ) {
        if (fh_it->second.share_tag == tag) {
            if (fh_it->second.handle != INVALID_HANDLE_VALUE) {
                CloseHandle(fh_it->second.handle);
            }
            fh_it = file_handles_.erase(fh_it);
        } else {
            ++fh_it;
        }
    }

    // Remove all inodes belonging to this share
    for (auto inode_it = inodes_.begin(); inode_it != inodes_.end(); ) {
        if (inode_it->second.share_tag == tag) {
            path_to_inode_.erase(inode_it->second.host_path);
            inode_it = inodes_.erase(inode_it);
        } else {
            ++inode_it;
        }
    }

    shares_.erase(it);
    shares_version_++;
    virtual_root_mtime_ = static_cast<uint64_t>(time(nullptr));
    LOG_INFO("VirtIO FS: removed share '%s'", tag.c_str());
    return true;
}

std::vector<std::string> VirtioFsDevice::GetShareTags() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> tags;
    for (const auto& [tag, _] : shares_) {
        tags.push_back(tag);
    }
    return tags;
}

bool VirtioFsDevice::HasShare(const std::string& tag) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shares_.find(tag) != shares_.end();
}

uint64_t VirtioFsDevice::GetDeviceFeatures() const {
    return VIRTIO_F_VERSION_1;
}

void VirtioFsDevice::ReadConfig(uint32_t offset, uint8_t size, uint32_t* value) {
    const auto* cfg = reinterpret_cast<const uint8_t*>(&config_);
    uint32_t cfg_size = sizeof(config_);

    if (offset >= cfg_size) {
        *value = 0;
        return;
    }

    uint32_t avail = cfg_size - offset;
    uint32_t read_size = std::min(static_cast<uint32_t>(size), avail);
    *value = 0;
    memcpy(value, cfg + offset, read_size);
}

void VirtioFsDevice::WriteConfig(uint32_t, uint8_t, uint32_t) {
}

void VirtioFsDevice::OnStatusChange(uint32_t new_status) {
    if (new_status == 0) {
        LOG_INFO("VirtIO FS: device reset");
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [fh, handle] : file_handles_) {
            if (handle.handle != INVALID_HANDLE_VALUE) {
                CloseHandle(handle.handle);
            }
        }
        file_handles_.clear();
        initialized_ = false;
    }
}

void VirtioFsDevice::OnQueueNotify(uint32_t queue_idx, VirtQueue& vq) {
    // Virtiofs queues:
    //   Queue 0: hiprio queue (for FUSE_INTERRUPT, etc.)
    //   Queue 1: request queue (for FUSE_INIT, FUSE_LOOKUP, etc.)
    // We handle both queues the same way
    if (queue_idx > 1) return;

    uint16_t head;
    while (vq.PopAvail(&head)) {
        ProcessRequest(vq, head);
    }

    if (mmio_) mmio_->NotifyUsedBuffer();
}

void VirtioFsDevice::ProcessRequest(VirtQueue& vq, uint16_t head_idx) {
    std::vector<VirtqChainElem> chain;
    if (!vq.WalkChain(head_idx, &chain)) {
        LOG_ERROR("VirtIO FS: failed to walk descriptor chain");
        return;
    }

    if (chain.empty()) {
        LOG_ERROR("VirtIO FS: empty descriptor chain");
        return;
    }

    std::vector<uint8_t> in_buf;
    for (const auto& elem : chain) {
        if (!elem.writable) {
            in_buf.insert(in_buf.end(), elem.addr, elem.addr + elem.len);
        }
    }

    if (in_buf.size() < sizeof(FuseInHeader)) {
        LOG_ERROR("VirtIO FS: request too small (%zu bytes)", in_buf.size());
        return;
    }

    auto* in_hdr = reinterpret_cast<const FuseInHeader*>(in_buf.data());
    const uint8_t* in_data = in_buf.data() + sizeof(FuseInHeader);
    uint32_t in_len = static_cast<uint32_t>(in_buf.size() - sizeof(FuseInHeader));

    std::vector<uint8_t> out_buf;

    switch (in_hdr->opcode) {
    case FUSE_INIT:
        HandleInit(in_hdr, in_data, out_buf);
        break;
    case FUSE_LOOKUP:
        HandleLookup(in_hdr, in_data, in_len, out_buf);
        break;
    case FUSE_FORGET:
        HandleForget(in_hdr, in_data);
        break;
    case FUSE_GETATTR:
        HandleGetAttr(in_hdr, in_data, out_buf);
        break;
    case FUSE_SETATTR:
        HandleSetAttr(in_hdr, in_data, out_buf);
        break;
    case FUSE_OPEN:
        HandleOpen(in_hdr, in_data, out_buf);
        break;
    case FUSE_READ:
        HandleRead(in_hdr, in_data, out_buf);
        break;
    case FUSE_WRITE:
        HandleWrite(in_hdr, in_data, in_len, out_buf);
        break;
    case FUSE_RELEASE:
        HandleRelease(in_hdr, in_data);
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_OK);
        break;
    case FUSE_OPENDIR:
        HandleOpenDir(in_hdr, in_data, out_buf);
        break;
    case FUSE_READDIR:
        HandleReadDir(in_hdr, in_data, out_buf);
        break;
    case FUSE_READDIRPLUS:
        HandleReadDirPlus(in_hdr, in_data, out_buf);
        break;
    case FUSE_RELEASEDIR:
        HandleReleaseDir(in_hdr, in_data);
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_OK);
        break;
    case FUSE_STATFS:
        HandleStatFs(in_hdr, out_buf);
        break;
    case FUSE_CREATE:
        HandleCreate(in_hdr, in_data, in_len, out_buf);
        break;
    case FUSE_MKDIR:
        HandleMkdir(in_hdr, in_data, in_len, out_buf);
        break;
    case FUSE_UNLINK:
        HandleUnlink(in_hdr, in_data, in_len, out_buf);
        break;
    case FUSE_RMDIR:
        HandleRmdir(in_hdr, in_data, in_len, out_buf);
        break;
    case FUSE_RENAME:
    case FUSE_RENAME2:
        HandleRename(in_hdr, in_data, in_len, out_buf);
        break;
    case FUSE_FLUSH:
        HandleFlush(in_hdr, in_data);
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_OK);
        break;
    case FUSE_FSYNC:
    case FUSE_FSYNCDIR:
        HandleFsync(in_hdr, in_data);
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_OK);
        break;
    case FUSE_ACCESS:
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_OK);
        break;
    case FUSE_DESTROY:
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_OK);
        break;
    default:
        LOG_WARN("VirtIO FS: unsupported opcode %u", in_hdr->opcode);
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_ENOSYS);
        break;
    }

    size_t out_offset = 0;
    for (const auto& elem : chain) {
        if (elem.writable && out_offset < out_buf.size()) {
            size_t copy_len = std::min(static_cast<size_t>(elem.len), 
                                       out_buf.size() - out_offset);
            memcpy(elem.addr, out_buf.data() + out_offset, copy_len);
            out_offset += copy_len;
        }
    }

    vq.PushUsed(head_idx, static_cast<uint32_t>(out_buf.size()));
}

void VirtioFsDevice::WriteErrorResponse(std::vector<uint8_t>& out_buf, 
                                         uint64_t unique, int32_t error) {
    FuseOutHeader out_hdr;
    out_hdr.len = sizeof(FuseOutHeader);
    out_hdr.error = error;
    out_hdr.unique = unique;
    
    out_buf.resize(sizeof(FuseOutHeader));
    memcpy(out_buf.data(), &out_hdr, sizeof(out_hdr));
}

void VirtioFsDevice::HandleInit(const FuseInHeader* in_hdr, const uint8_t* in_data,
                                 std::vector<uint8_t>& out_buf) {
    auto* init_in = reinterpret_cast<const FuseInitIn*>(in_data);
    
    LOG_INFO("VirtIO FS: INIT major=%u minor=%u", init_in->major, init_in->minor);

    FuseOutHeader out_hdr;
    FuseInitOut init_out;
    memset(&init_out, 0, sizeof(init_out));

    init_out.major = FUSE_KERNEL_VERSION;
    init_out.minor = FUSE_KERNEL_MINOR_VERSION;
    init_out.max_readahead = init_in->max_readahead;
    init_out.flags = FUSE_BIG_WRITES | FUSE_PARALLEL_DIROPS;
    init_out.max_write = 1024 * 1024;
    init_out.max_background = 16;
    init_out.congestion_threshold = 12;
    init_out.time_gran = 1;
    init_out.max_pages = 256;

    out_hdr.len = sizeof(FuseOutHeader) + sizeof(FuseInitOut);
    out_hdr.error = 0;
    out_hdr.unique = in_hdr->unique;

    out_buf.resize(out_hdr.len);
    memcpy(out_buf.data(), &out_hdr, sizeof(out_hdr));
    memcpy(out_buf.data() + sizeof(out_hdr), &init_out, sizeof(init_out));

    initialized_ = true;
}

void VirtioFsDevice::HandleLookup(const FuseInHeader* in_hdr, const uint8_t* in_data,
                                   uint32_t in_len, std::vector<uint8_t>& out_buf) {
    std::string name(reinterpret_cast<const char*>(in_data), 
                     strnlen(reinterpret_cast<const char*>(in_data), in_len));
    
    std::lock_guard<std::mutex> lock(mutex_);

    // Special handling for virtual root directory
    if (in_hdr->nodeid == VIRTUAL_ROOT_INODE) {
        // Looking up a share tag in the virtual root
        auto it = shares_.find(name);
        if (it == shares_.end()) {
            WriteErrorResponse(out_buf, in_hdr->unique, FUSE_ENOENT);
            return;
        }

        const ShareInfo& share = it->second;
        
        FuseOutHeader out_hdr;
        FuseEntryOut entry_out;
        memset(&entry_out, 0, sizeof(entry_out));

        entry_out.nodeid = share.root_inode;
        entry_out.generation = 1;
        entry_out.entry_valid = 0;
        entry_out.attr_valid = 0;

        int32_t err = FillShareRootAttr(share, &entry_out.attr);
        if (err != FUSE_OK) {
            WriteErrorResponse(out_buf, in_hdr->unique, err);
            return;
        }

        out_hdr.len = sizeof(FuseOutHeader) + sizeof(FuseEntryOut);
        out_hdr.error = 0;
        out_hdr.unique = in_hdr->unique;

        out_buf.resize(out_hdr.len);
        memcpy(out_buf.data(), &out_hdr, sizeof(out_hdr));
        memcpy(out_buf.data() + sizeof(out_hdr), &entry_out, sizeof(entry_out));
        return;
    }

    // Normal lookup in a real directory
    auto inode_it = inodes_.find(in_hdr->nodeid);
    if (inode_it == inodes_.end()) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_ENOENT);
        return;
    }

    const InodeInfo& parent = inode_it->second;
    std::wstring child_path = parent.host_path + L"\\" + Utf8ToWide(name);
    
    DWORD attrs = GetFileAttributesW(child_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        WriteErrorResponse(out_buf, in_hdr->unique, WindowsErrorToFuse(GetLastError()));
        return;
    }

    bool is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    
    // Create or get inode
    auto path_it = path_to_inode_.find(child_path);
    uint64_t inode;
    if (path_it != path_to_inode_.end()) {
        inode = path_it->second;
        inodes_[inode].nlookup++;
    } else {
        inode = next_inode_++;
        InodeInfo info;
        info.inode = inode;
        info.host_path = child_path;
        info.nlookup = 1;
        info.is_dir = is_dir;
        info.share_tag = parent.share_tag;
        inodes_[inode] = info;
        path_to_inode_[child_path] = inode;
    }

    FuseOutHeader out_hdr;
    FuseEntryOut entry_out;
    memset(&entry_out, 0, sizeof(entry_out));

    entry_out.nodeid = inode;
    entry_out.generation = 1;
    entry_out.entry_valid = 1;
    entry_out.attr_valid = 1;

    bool readonly = false;
    {
        auto sit = shares_.find(parent.share_tag);
        if (sit != shares_.end()) readonly = sit->second.readonly;
    }
    int32_t err = FillAttr(child_path, &entry_out.attr, inode, readonly);
    if (err != FUSE_OK) {
        WriteErrorResponse(out_buf, in_hdr->unique, err);
        return;
    }

    out_hdr.len = sizeof(FuseOutHeader) + sizeof(FuseEntryOut);
    out_hdr.error = 0;
    out_hdr.unique = in_hdr->unique;

    out_buf.resize(out_hdr.len);
    memcpy(out_buf.data(), &out_hdr, sizeof(out_hdr));
    memcpy(out_buf.data() + sizeof(out_hdr), &entry_out, sizeof(entry_out));
}

void VirtioFsDevice::HandleForget(const FuseInHeader* in_hdr, const uint8_t* in_data) {
    auto* forget_in = reinterpret_cast<const FuseForgetIn*>(in_data);
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Don't forget virtual root or share roots
    if (in_hdr->nodeid == VIRTUAL_ROOT_INODE) return;
    for (const auto& [tag, share] : shares_) {
        if (share.root_inode == in_hdr->nodeid) return;
    }

    auto it = inodes_.find(in_hdr->nodeid);
    if (it != inodes_.end()) {
        if (it->second.nlookup > forget_in->nlookup) {
            it->second.nlookup -= forget_in->nlookup;
        } else {
            path_to_inode_.erase(it->second.host_path);
            inodes_.erase(it);
        }
    }
}

void VirtioFsDevice::HandleGetAttr(const FuseInHeader* in_hdr, const uint8_t*,
                                    std::vector<uint8_t>& out_buf) {
    std::lock_guard<std::mutex> lock(mutex_);

    FuseOutHeader out_hdr;
    FuseAttrOut attr_out;
    memset(&attr_out, 0, sizeof(attr_out));
    attr_out.attr_valid = 1;

    // Special handling for virtual root
    if (in_hdr->nodeid == VIRTUAL_ROOT_INODE) {
        attr_out.attr_valid = 0;
        int32_t err = FillVirtualRootAttr(&attr_out.attr);
        if (err != FUSE_OK) {
            WriteErrorResponse(out_buf, in_hdr->unique, err);
            return;
        }
    } else {
        // Check if it's a share root
        for (const auto& [tag, share] : shares_) {
            if (share.root_inode == in_hdr->nodeid) {
                attr_out.attr_valid = 0;
                int32_t err = FillShareRootAttr(share, &attr_out.attr);
                if (err != FUSE_OK) {
                    WriteErrorResponse(out_buf, in_hdr->unique, err);
                    return;
                }
                goto send_response;
            }
        }

        // Normal file/directory
        auto it = inodes_.find(in_hdr->nodeid);
        if (it == inodes_.end()) {
            WriteErrorResponse(out_buf, in_hdr->unique, FUSE_ENOENT);
            return;
        }

        bool readonly = false;
        {
            auto sit = shares_.find(it->second.share_tag);
            if (sit != shares_.end()) readonly = sit->second.readonly;
        }
        int32_t err = FillAttr(it->second.host_path, &attr_out.attr, in_hdr->nodeid, readonly);
        if (err != FUSE_OK) {
            WriteErrorResponse(out_buf, in_hdr->unique, err);
            return;
        }
    }

send_response:
    out_hdr.len = sizeof(FuseOutHeader) + sizeof(FuseAttrOut);
    out_hdr.error = 0;
    out_hdr.unique = in_hdr->unique;

    out_buf.resize(out_hdr.len);
    memcpy(out_buf.data(), &out_hdr, sizeof(out_hdr));
    memcpy(out_buf.data() + sizeof(out_hdr), &attr_out, sizeof(attr_out));
}

void VirtioFsDevice::HandleSetAttr(const FuseInHeader* in_hdr, const uint8_t* in_data,
                                    std::vector<uint8_t>& out_buf) {
    // Cannot modify virtual root
    if (in_hdr->nodeid == VIRTUAL_ROOT_INODE) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_EACCES);
        return;
    }

    std::string share_tag = NodeIdToShareTag(in_hdr->nodeid);
    if (IsShareReadonly(share_tag)) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_EROFS);
        return;
    }

    auto* setattr_in = reinterpret_cast<const FuseSetAttrIn*>(in_data);
    
    std::wstring path = NodeIdToPath(in_hdr->nodeid);
    if (path.empty()) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_ENOENT);
        return;
    }

    // Handle size truncation
    if (setattr_in->valid & FATTR_SIZE) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER li;
            li.QuadPart = static_cast<LONGLONG>(setattr_in->size);
            SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
            SetEndOfFile(h);
            CloseHandle(h);
        }
    }

    // Handle time updates
    if ((setattr_in->valid & FATTR_ATIME) || (setattr_in->valid & FATTR_MTIME)) {
        HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, 
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            FILETIME atime, mtime;
            FILETIME* patime = nullptr;
            FILETIME* pmtime = nullptr;
            
            if (setattr_in->valid & FATTR_ATIME) {
                ULARGE_INTEGER uli;
                uli.QuadPart = (setattr_in->atime + 11644473600ULL) * 10000000ULL;
                atime.dwLowDateTime = uli.LowPart;
                atime.dwHighDateTime = uli.HighPart;
                patime = &atime;
            }
            if (setattr_in->valid & FATTR_MTIME) {
                ULARGE_INTEGER uli;
                uli.QuadPart = (setattr_in->mtime + 11644473600ULL) * 10000000ULL;
                mtime.dwLowDateTime = uli.LowPart;
                mtime.dwHighDateTime = uli.HighPart;
                pmtime = &mtime;
            }
            SetFileTime(h, nullptr, patime, pmtime);
            CloseHandle(h);
        }
    }

    HandleGetAttr(in_hdr, nullptr, out_buf);
}

void VirtioFsDevice::HandleOpen(const FuseInHeader* in_hdr, const uint8_t* in_data,
                                 std::vector<uint8_t>& out_buf) {
    auto* open_in = reinterpret_cast<const FuseOpenIn*>(in_data);
    
    std::wstring path = NodeIdToPath(in_hdr->nodeid);
    if (path.empty()) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_ENOENT);
        return;
    }

    std::string share_tag = NodeIdToShareTag(in_hdr->nodeid);
    
    DWORD access = 0;
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    DWORD disposition = OPEN_EXISTING;

    uint32_t flags = open_in->flags;
    bool write_access = (flags & 0x3) != 0;
    
    if (IsShareReadonly(share_tag) && write_access) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_EROFS);
        return;
    }

    if ((flags & 0x3) == 0) {
        access = GENERIC_READ;
    } else if ((flags & 0x3) == 1) {
        access = GENERIC_WRITE;
    } else {
        access = GENERIC_READ | GENERIC_WRITE;
    }

    HANDLE h = CreateFileW(path.c_str(), access, share, nullptr, disposition,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        WriteErrorResponse(out_buf, in_hdr->unique, WindowsErrorToFuse(GetLastError()));
        return;
    }

    uint64_t fh = AllocFileHandle(h, false, path, share_tag);

    FuseOutHeader out_hdr;
    FuseOpenOut open_out;
    memset(&open_out, 0, sizeof(open_out));
    open_out.fh = fh;

    out_hdr.len = sizeof(FuseOutHeader) + sizeof(FuseOpenOut);
    out_hdr.error = 0;
    out_hdr.unique = in_hdr->unique;

    out_buf.resize(out_hdr.len);
    memcpy(out_buf.data(), &out_hdr, sizeof(out_hdr));
    memcpy(out_buf.data() + sizeof(out_hdr), &open_out, sizeof(open_out));
}

void VirtioFsDevice::HandleRead(const FuseInHeader* in_hdr, const uint8_t* in_data,
                                 std::vector<uint8_t>& out_buf) {
    auto* read_in = reinterpret_cast<const FuseReadIn*>(in_data);
    
    FileHandle* fh = GetFileHandle(read_in->fh);
    if (!fh || fh->handle == INVALID_HANDLE_VALUE) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_ENOENT);
        return;
    }

    std::vector<uint8_t> data(read_in->size);
    
    LARGE_INTEGER offset;
    offset.QuadPart = static_cast<LONGLONG>(read_in->offset);
    
    OVERLAPPED ov = {};
    ov.Offset = offset.LowPart;
    ov.OffsetHigh = offset.HighPart;
    
    DWORD bytes_read = 0;
    if (!ReadFile(fh->handle, data.data(), read_in->size, &bytes_read, &ov)) {
        DWORD err = GetLastError();
        if (err != ERROR_HANDLE_EOF) {
            WriteErrorResponse(out_buf, in_hdr->unique, WindowsErrorToFuse(err));
            return;
        }
    }

    FuseOutHeader out_hdr;
    out_hdr.len = sizeof(FuseOutHeader) + bytes_read;
    out_hdr.error = 0;
    out_hdr.unique = in_hdr->unique;

    out_buf.resize(out_hdr.len);
    memcpy(out_buf.data(), &out_hdr, sizeof(out_hdr));
    if (bytes_read > 0) {
        memcpy(out_buf.data() + sizeof(out_hdr), data.data(), bytes_read);
    }
}

void VirtioFsDevice::HandleWrite(const FuseInHeader* in_hdr, const uint8_t* in_data,
                                  uint32_t in_len, std::vector<uint8_t>& out_buf) {
    auto* write_in = reinterpret_cast<const FuseWriteIn*>(in_data);
    
    FileHandle* fh = GetFileHandle(write_in->fh);
    if (!fh || fh->handle == INVALID_HANDLE_VALUE) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_ENOENT);
        return;
    }

    if (IsShareReadonly(fh->share_tag)) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_EROFS);
        return;
    }

    const uint8_t* write_data = in_data + sizeof(FuseWriteIn);
    uint32_t data_len = in_len - sizeof(FuseWriteIn);
    
    if (data_len < write_in->size) {
        data_len = write_in->size;
    }

    LARGE_INTEGER offset;
    offset.QuadPart = static_cast<LONGLONG>(write_in->offset);
    
    OVERLAPPED ov = {};
    ov.Offset = offset.LowPart;
    ov.OffsetHigh = offset.HighPart;
    
    DWORD bytes_written = 0;
    if (!WriteFile(fh->handle, write_data, write_in->size, &bytes_written, &ov)) {
        WriteErrorResponse(out_buf, in_hdr->unique, WindowsErrorToFuse(GetLastError()));
        return;
    }

    FuseOutHeader out_hdr;
    FuseWriteOut write_out;
    memset(&write_out, 0, sizeof(write_out));
    write_out.size = bytes_written;

    out_hdr.len = sizeof(FuseOutHeader) + sizeof(FuseWriteOut);
    out_hdr.error = 0;
    out_hdr.unique = in_hdr->unique;

    out_buf.resize(out_hdr.len);
    memcpy(out_buf.data(), &out_hdr, sizeof(out_hdr));
    memcpy(out_buf.data() + sizeof(out_hdr), &write_out, sizeof(write_out));
}

void VirtioFsDevice::HandleRelease(const FuseInHeader*, const uint8_t* in_data) {
    auto* release_in = reinterpret_cast<const FuseReleaseIn*>(in_data);
    CloseFileHandle(release_in->fh);
}

void VirtioFsDevice::HandleOpenDir(const FuseInHeader* in_hdr, const uint8_t*,
                                    std::vector<uint8_t>& out_buf) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::wstring path;
    std::string share_tag;

    // Virtual root is always openable
    if (in_hdr->nodeid == VIRTUAL_ROOT_INODE) {
        path = L"";
        share_tag = "";
    } else {
        auto it = inodes_.find(in_hdr->nodeid);
        if (it == inodes_.end()) {
            WriteErrorResponse(out_buf, in_hdr->unique, FUSE_ENOENT);
            return;
        }
        path = it->second.host_path;
        share_tag = it->second.share_tag;

        DWORD attrs = GetFileAttributesW(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            WriteErrorResponse(out_buf, in_hdr->unique, WindowsErrorToFuse(GetLastError()));
            return;
        }
        if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            WriteErrorResponse(out_buf, in_hdr->unique, FUSE_ENOTDIR);
            return;
        }
    }

    uint64_t fh = next_fh_++;
    file_handles_[fh] = {INVALID_HANDLE_VALUE, true, path, share_tag};

    FuseOutHeader out_hdr;
    FuseOpenOut open_out;
    memset(&open_out, 0, sizeof(open_out));
    open_out.fh = fh;

    out_hdr.len = sizeof(FuseOutHeader) + sizeof(FuseOpenOut);
    out_hdr.error = 0;
    out_hdr.unique = in_hdr->unique;

    out_buf.resize(out_hdr.len);
    memcpy(out_buf.data(), &out_hdr, sizeof(out_hdr));
    memcpy(out_buf.data() + sizeof(out_hdr), &open_out, sizeof(open_out));
}

void VirtioFsDevice::HandleReadDir(const FuseInHeader* in_hdr, const uint8_t* in_data,
                                    std::vector<uint8_t>& out_buf) {
    auto* read_in = reinterpret_cast<const FuseReadIn*>(in_data);
    
    std::lock_guard<std::mutex> lock(mutex_);

    FileHandle* fh = GetFileHandle(read_in->fh);
    if (!fh) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_ENOENT);
        return;
    }

    std::vector<uint8_t> dir_buf;
    uint64_t entry_offset = 0;

    // Virtual root directory - list all shares
    if (fh->path.empty() && fh->share_tag.empty()) {
        for (const auto& [tag, share] : shares_) {
            if (entry_offset < read_in->offset) {
                entry_offset++;
                continue;
            }

            uint32_t name_len = static_cast<uint32_t>(tag.size());
            uint32_t entry_size = sizeof(FuseDirent) + name_len;
            entry_size = (entry_size + 7) & ~7;

            if (dir_buf.size() + entry_size > read_in->size) {
                break;
            }

            FuseDirent dirent;
            memset(&dirent, 0, sizeof(dirent));
            dirent.ino = share.root_inode;
            dirent.off = entry_offset + 1;
            dirent.namelen = name_len;
            dirent.type = FUSE_S_IFDIR >> 12;

            size_t old_size = dir_buf.size();
            dir_buf.resize(old_size + entry_size);
            memcpy(dir_buf.data() + old_size, &dirent, sizeof(dirent));
            memcpy(dir_buf.data() + old_size + sizeof(dirent), tag.c_str(), name_len);

            entry_offset++;
        }
    } else {
        // Real directory
        std::wstring search_path = fh->path + L"\\*";
        
        WIN32_FIND_DATAW fdata;
        HANDLE hFind = FindFirstFileW(search_path.c_str(), &fdata);
        if (hFind == INVALID_HANDLE_VALUE) {
            WriteErrorResponse(out_buf, in_hdr->unique, WindowsErrorToFuse(GetLastError()));
            return;
        }

        do {
            std::wstring name_w = fdata.cFileName;
            std::string name = WideToUtf8(name_w);
            
            if (entry_offset < read_in->offset) {
                entry_offset++;
                continue;
            }

            uint32_t name_len = static_cast<uint32_t>(name.size());
            uint32_t entry_size = sizeof(FuseDirent) + name_len;
            entry_size = (entry_size + 7) & ~7;

            if (dir_buf.size() + entry_size > read_in->size) {
                break;
            }

            FuseDirent dirent;
            memset(&dirent, 0, sizeof(dirent));
            
            std::wstring full_path = fh->path + L"\\" + name_w;
            bool is_dir = (fdata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            
            // Get or create inode
            auto path_it = path_to_inode_.find(full_path);
            uint64_t inode;
            if (path_it != path_to_inode_.end()) {
                inode = path_it->second;
            } else {
                inode = next_inode_++;
                InodeInfo info;
                info.inode = inode;
                info.host_path = full_path;
                info.nlookup = 0;
                info.is_dir = is_dir;
                info.share_tag = fh->share_tag;
                inodes_[inode] = info;
                path_to_inode_[full_path] = inode;
            }

            dirent.ino = inode;
            dirent.off = entry_offset + 1;
            dirent.namelen = name_len;
            dirent.type = is_dir ? (FUSE_S_IFDIR >> 12) : (FUSE_S_IFREG >> 12);

            size_t old_size = dir_buf.size();
            dir_buf.resize(old_size + entry_size);
            memcpy(dir_buf.data() + old_size, &dirent, sizeof(dirent));
            memcpy(dir_buf.data() + old_size + sizeof(dirent), name.c_str(), name_len);

            entry_offset++;
        } while (FindNextFileW(hFind, &fdata));

        FindClose(hFind);
    }

    FuseOutHeader out_hdr;
    out_hdr.len = sizeof(FuseOutHeader) + static_cast<uint32_t>(dir_buf.size());
    out_hdr.error = 0;
    out_hdr.unique = in_hdr->unique;

    out_buf.resize(out_hdr.len);
    memcpy(out_buf.data(), &out_hdr, sizeof(out_hdr));
    if (!dir_buf.empty()) {
        memcpy(out_buf.data() + sizeof(out_hdr), dir_buf.data(), dir_buf.size());
    }
}

void VirtioFsDevice::HandleReadDirPlus(const FuseInHeader* in_hdr, const uint8_t* in_data,
                                        std::vector<uint8_t>& out_buf) {
    auto* read_in = reinterpret_cast<const FuseReadIn*>(in_data);
    
    std::lock_guard<std::mutex> lock(mutex_);

    FileHandle* fh = GetFileHandle(read_in->fh);
    if (!fh) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_ENOENT);
        return;
    }

    std::vector<uint8_t> dir_buf;
    uint64_t entry_offset = 0;

    // Virtual root directory - list all shares
    if (fh->path.empty() && fh->share_tag.empty()) {
        for (const auto& [tag, share] : shares_) {
            if (entry_offset < read_in->offset) {
                entry_offset++;
                continue;
            }

            uint32_t name_len = static_cast<uint32_t>(tag.size());
            uint32_t entry_size = sizeof(FuseDirentplus) + name_len;
            entry_size = (entry_size + 7) & ~7;

            if (dir_buf.size() + entry_size > read_in->size) {
                break;
            }

            FuseDirentplus direntplus;
            memset(&direntplus, 0, sizeof(direntplus));
            
            direntplus.entry_out.nodeid = share.root_inode;
            direntplus.entry_out.generation = 1;
            direntplus.entry_out.entry_valid = 0;
            direntplus.entry_out.attr_valid = 0;
            FillShareRootAttr(share, &direntplus.entry_out.attr);

            direntplus.dirent.ino = share.root_inode;
            direntplus.dirent.off = entry_offset + 1;
            direntplus.dirent.namelen = name_len;
            direntplus.dirent.type = FUSE_S_IFDIR >> 12;

            size_t old_size = dir_buf.size();
            dir_buf.resize(old_size + entry_size);
            memcpy(dir_buf.data() + old_size, &direntplus, sizeof(direntplus));
            memcpy(dir_buf.data() + old_size + sizeof(direntplus), tag.c_str(), name_len);

            entry_offset++;
        }
    } else {
        // Real directory
        std::wstring search_path = fh->path + L"\\*";

        bool share_readonly = false;
        {
            auto sit = shares_.find(fh->share_tag);
            if (sit != shares_.end()) share_readonly = sit->second.readonly;
        }
        
        WIN32_FIND_DATAW fdata;
        HANDLE hFind = FindFirstFileW(search_path.c_str(), &fdata);
        if (hFind == INVALID_HANDLE_VALUE) {
            WriteErrorResponse(out_buf, in_hdr->unique, WindowsErrorToFuse(GetLastError()));
            return;
        }

        do {
            std::wstring name_w = fdata.cFileName;
            std::string name = WideToUtf8(name_w);
            
            if (entry_offset < read_in->offset) {
                entry_offset++;
                continue;
            }

            uint32_t name_len = static_cast<uint32_t>(name.size());
            uint32_t entry_size = sizeof(FuseDirentplus) + name_len;
            entry_size = (entry_size + 7) & ~7;

            if (dir_buf.size() + entry_size > read_in->size) {
                break;
            }

            std::wstring full_path = fh->path + L"\\" + name_w;
            bool is_dir = (fdata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            
            // Get or create inode
            auto path_it = path_to_inode_.find(full_path);
            uint64_t inode;
            if (path_it != path_to_inode_.end()) {
                inode = path_it->second;
            } else {
                inode = next_inode_++;
                InodeInfo info;
                info.inode = inode;
                info.host_path = full_path;
                info.nlookup = 0;
                info.is_dir = is_dir;
                info.share_tag = fh->share_tag;
                inodes_[inode] = info;
                path_to_inode_[full_path] = inode;
            }

            FuseDirentplus direntplus;
            memset(&direntplus, 0, sizeof(direntplus));
            
            direntplus.entry_out.nodeid = inode;
            direntplus.entry_out.generation = 1;
            direntplus.entry_out.entry_valid = 1;
            direntplus.entry_out.attr_valid = 1;
            FillAttr(full_path, &direntplus.entry_out.attr, inode, share_readonly);

            direntplus.dirent.ino = inode;
            direntplus.dirent.off = entry_offset + 1;
            direntplus.dirent.namelen = name_len;
            direntplus.dirent.type = is_dir ? (FUSE_S_IFDIR >> 12) : (FUSE_S_IFREG >> 12);

            size_t old_size = dir_buf.size();
            dir_buf.resize(old_size + entry_size);
            memcpy(dir_buf.data() + old_size, &direntplus, sizeof(direntplus));
            memcpy(dir_buf.data() + old_size + sizeof(direntplus), name.c_str(), name_len);

            entry_offset++;
        } while (FindNextFileW(hFind, &fdata));

        FindClose(hFind);
    }

    FuseOutHeader out_hdr;
    out_hdr.len = sizeof(FuseOutHeader) + static_cast<uint32_t>(dir_buf.size());
    out_hdr.error = 0;
    out_hdr.unique = in_hdr->unique;

    out_buf.resize(out_hdr.len);
    memcpy(out_buf.data(), &out_hdr, sizeof(out_hdr));
    if (!dir_buf.empty()) {
        memcpy(out_buf.data() + sizeof(out_hdr), dir_buf.data(), dir_buf.size());
    }
}

void VirtioFsDevice::HandleReleaseDir(const FuseInHeader*, const uint8_t* in_data) {
    auto* release_in = reinterpret_cast<const FuseReleaseIn*>(in_data);
    CloseFileHandle(release_in->fh);
}

void VirtioFsDevice::HandleStatFs(const FuseInHeader* in_hdr, std::vector<uint8_t>& out_buf) {
    FuseOutHeader out_hdr;
    FuseStatfsOut statfs_out;
    memset(&statfs_out, 0, sizeof(statfs_out));

    // For virtual root, return aggregated stats or reasonable defaults
    std::lock_guard<std::mutex> lock(mutex_);
    
    uint64_t total_blocks = 0;
    uint64_t free_blocks = 0;
    uint64_t avail_blocks = 0;
    uint64_t block_size = 4096;
    bool got_stats = false;

    // Try to get stats from the first share, or aggregate
    for (const auto& [tag, share] : shares_) {
        ULARGE_INTEGER free_bytes, total_bytes, total_free;
        if (GetDiskFreeSpaceExW(share.host_path.c_str(), &free_bytes, &total_bytes, &total_free)) {
            if (!got_stats) {
                total_blocks = total_bytes.QuadPart / block_size;
                free_blocks = total_free.QuadPart / block_size;
                avail_blocks = free_bytes.QuadPart / block_size;
                got_stats = true;
            }
            break;  // Use first share's disk stats
        }
    }

    if (!got_stats) {
        // Default values if no shares
        total_blocks = 1000000;
        free_blocks = 1000000;
        avail_blocks = 1000000;
    }

    statfs_out.bsize = static_cast<uint32_t>(block_size);
    statfs_out.frsize = static_cast<uint32_t>(block_size);
    statfs_out.blocks = total_blocks;
    statfs_out.bfree = free_blocks;
    statfs_out.bavail = avail_blocks;
    statfs_out.files = 1000000;
    statfs_out.ffree = 1000000;
    statfs_out.namelen = 255;

    out_hdr.len = sizeof(FuseOutHeader) + sizeof(FuseStatfsOut);
    out_hdr.error = 0;
    out_hdr.unique = in_hdr->unique;

    out_buf.resize(out_hdr.len);
    memcpy(out_buf.data(), &out_hdr, sizeof(out_hdr));
    memcpy(out_buf.data() + sizeof(out_hdr), &statfs_out, sizeof(statfs_out));
}

void VirtioFsDevice::HandleCreate(const FuseInHeader* in_hdr, const uint8_t* in_data,
                                   uint32_t in_len, std::vector<uint8_t>& out_buf) {
    // Cannot create in virtual root
    if (in_hdr->nodeid == VIRTUAL_ROOT_INODE) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_EACCES);
        return;
    }

    std::string share_tag = NodeIdToShareTag(in_hdr->nodeid);
    if (IsShareReadonly(share_tag)) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_EROFS);
        return;
    }

    auto* create_in = reinterpret_cast<const FuseCreateIn*>(in_data);
    const char* name = reinterpret_cast<const char*>(in_data + sizeof(FuseCreateIn));
    size_t name_len = strnlen(name, in_len - sizeof(FuseCreateIn));

    std::wstring parent_path = NodeIdToPath(in_hdr->nodeid);
    if (parent_path.empty()) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_ENOENT);
        return;
    }

    std::wstring file_path = parent_path + L"\\" + Utf8ToWide(std::string(name, name_len));

    DWORD access = GENERIC_READ | GENERIC_WRITE;
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    
    HANDLE h = CreateFileW(file_path.c_str(), access, share, nullptr, 
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        WriteErrorResponse(out_buf, in_hdr->unique, WindowsErrorToFuse(GetLastError()));
        return;
    }

    uint64_t inode = GetOrCreateInode(file_path, false, share_tag);
    uint64_t fh = AllocFileHandle(h, false, file_path, share_tag);

    FuseOutHeader out_hdr;
    FuseEntryOut entry_out;
    FuseOpenOut open_out;
    memset(&entry_out, 0, sizeof(entry_out));
    memset(&open_out, 0, sizeof(open_out));

    entry_out.nodeid = inode;
    entry_out.generation = 1;
    entry_out.entry_valid = 1;
    entry_out.attr_valid = 1;
    FillAttr(file_path, &entry_out.attr, inode);

    open_out.fh = fh;

    out_hdr.len = sizeof(FuseOutHeader) + sizeof(FuseEntryOut) + sizeof(FuseOpenOut);
    out_hdr.error = 0;
    out_hdr.unique = in_hdr->unique;

    out_buf.resize(out_hdr.len);
    memcpy(out_buf.data(), &out_hdr, sizeof(out_hdr));
    memcpy(out_buf.data() + sizeof(out_hdr), &entry_out, sizeof(entry_out));
    memcpy(out_buf.data() + sizeof(out_hdr) + sizeof(entry_out), &open_out, sizeof(open_out));
}

void VirtioFsDevice::HandleMkdir(const FuseInHeader* in_hdr, const uint8_t* in_data,
                                  uint32_t in_len, std::vector<uint8_t>& out_buf) {
    // Cannot mkdir in virtual root
    if (in_hdr->nodeid == VIRTUAL_ROOT_INODE) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_EACCES);
        return;
    }

    std::string share_tag = NodeIdToShareTag(in_hdr->nodeid);
    if (IsShareReadonly(share_tag)) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_EROFS);
        return;
    }

    const char* name = reinterpret_cast<const char*>(in_data + sizeof(FuseMkdirIn));
    size_t name_len = strnlen(name, in_len - sizeof(FuseMkdirIn));

    std::wstring parent_path = NodeIdToPath(in_hdr->nodeid);
    if (parent_path.empty()) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_ENOENT);
        return;
    }

    std::wstring dir_path = parent_path + L"\\" + Utf8ToWide(std::string(name, name_len));

    if (!CreateDirectoryW(dir_path.c_str(), nullptr)) {
        WriteErrorResponse(out_buf, in_hdr->unique, WindowsErrorToFuse(GetLastError()));
        return;
    }

    uint64_t inode = GetOrCreateInode(dir_path, true, share_tag);

    FuseOutHeader out_hdr;
    FuseEntryOut entry_out;
    memset(&entry_out, 0, sizeof(entry_out));

    entry_out.nodeid = inode;
    entry_out.generation = 1;
    entry_out.entry_valid = 1;
    entry_out.attr_valid = 1;
    FillAttr(dir_path, &entry_out.attr, inode);

    out_hdr.len = sizeof(FuseOutHeader) + sizeof(FuseEntryOut);
    out_hdr.error = 0;
    out_hdr.unique = in_hdr->unique;

    out_buf.resize(out_hdr.len);
    memcpy(out_buf.data(), &out_hdr, sizeof(out_hdr));
    memcpy(out_buf.data() + sizeof(out_hdr), &entry_out, sizeof(entry_out));
}

void VirtioFsDevice::HandleUnlink(const FuseInHeader* in_hdr, const uint8_t* in_data,
                                   uint32_t in_len, std::vector<uint8_t>& out_buf) {
    // Cannot unlink in virtual root
    if (in_hdr->nodeid == VIRTUAL_ROOT_INODE) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_EACCES);
        return;
    }

    std::string share_tag = NodeIdToShareTag(in_hdr->nodeid);
    if (IsShareReadonly(share_tag)) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_EROFS);
        return;
    }

    std::string name(reinterpret_cast<const char*>(in_data), 
                     strnlen(reinterpret_cast<const char*>(in_data), in_len));

    std::wstring parent_path = NodeIdToPath(in_hdr->nodeid);
    if (parent_path.empty()) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_ENOENT);
        return;
    }

    std::wstring file_path = parent_path + L"\\" + Utf8ToWide(name);

    if (!DeleteFileW(file_path.c_str())) {
        WriteErrorResponse(out_buf, in_hdr->unique, WindowsErrorToFuse(GetLastError()));
        return;
    }

    RemoveInode(0);
    WriteErrorResponse(out_buf, in_hdr->unique, FUSE_OK);
}

void VirtioFsDevice::HandleRmdir(const FuseInHeader* in_hdr, const uint8_t* in_data,
                                  uint32_t in_len, std::vector<uint8_t>& out_buf) {
    // Cannot rmdir in virtual root (would remove a share)
    if (in_hdr->nodeid == VIRTUAL_ROOT_INODE) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_EACCES);
        return;
    }

    std::string share_tag = NodeIdToShareTag(in_hdr->nodeid);
    if (IsShareReadonly(share_tag)) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_EROFS);
        return;
    }

    std::string name(reinterpret_cast<const char*>(in_data), 
                     strnlen(reinterpret_cast<const char*>(in_data), in_len));

    std::wstring parent_path = NodeIdToPath(in_hdr->nodeid);
    if (parent_path.empty()) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_ENOENT);
        return;
    }

    std::wstring dir_path = parent_path + L"\\" + Utf8ToWide(name);

    if (!RemoveDirectoryW(dir_path.c_str())) {
        WriteErrorResponse(out_buf, in_hdr->unique, WindowsErrorToFuse(GetLastError()));
        return;
    }

    WriteErrorResponse(out_buf, in_hdr->unique, FUSE_OK);
}

void VirtioFsDevice::HandleRename(const FuseInHeader* in_hdr, const uint8_t* in_data,
                                   uint32_t in_len, std::vector<uint8_t>& out_buf) {
    // Cannot rename in virtual root
    if (in_hdr->nodeid == VIRTUAL_ROOT_INODE) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_EACCES);
        return;
    }

    std::string share_tag = NodeIdToShareTag(in_hdr->nodeid);
    if (IsShareReadonly(share_tag)) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_EROFS);
        return;
    }

    auto* rename_in = reinterpret_cast<const FuseRenameIn*>(in_data);
    const char* names = reinterpret_cast<const char*>(in_data + sizeof(FuseRenameIn));
    size_t names_len = in_len - sizeof(FuseRenameIn);

    size_t old_name_len = strnlen(names, names_len);
    std::string old_name(names, old_name_len);
    
    const char* new_name_ptr = names + old_name_len + 1;
    size_t remaining = names_len - old_name_len - 1;
    std::string new_name(new_name_ptr, strnlen(new_name_ptr, remaining));

    std::wstring old_parent_path = NodeIdToPath(in_hdr->nodeid);
    std::wstring new_parent_path = NodeIdToPath(rename_in->newdir);
    
    if (old_parent_path.empty() || new_parent_path.empty()) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_ENOENT);
        return;
    }

    // Check if target is also writable
    std::string new_share_tag = NodeIdToShareTag(rename_in->newdir);
    if (IsShareReadonly(new_share_tag)) {
        WriteErrorResponse(out_buf, in_hdr->unique, FUSE_EROFS);
        return;
    }

    std::wstring old_path = old_parent_path + L"\\" + Utf8ToWide(old_name);
    std::wstring new_path = new_parent_path + L"\\" + Utf8ToWide(new_name);

    if (!MoveFileExW(old_path.c_str(), new_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        WriteErrorResponse(out_buf, in_hdr->unique, WindowsErrorToFuse(GetLastError()));
        return;
    }

    WriteErrorResponse(out_buf, in_hdr->unique, FUSE_OK);
}

void VirtioFsDevice::HandleFlush(const FuseInHeader*, const uint8_t* in_data) {
    auto* release_in = reinterpret_cast<const FuseReleaseIn*>(in_data);
    FileHandle* fh = GetFileHandle(release_in->fh);
    if (fh && fh->handle != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(fh->handle);
    }
}

void VirtioFsDevice::HandleFsync(const FuseInHeader*, const uint8_t* in_data) {
    auto* release_in = reinterpret_cast<const FuseReleaseIn*>(in_data);
    FileHandle* fh = GetFileHandle(release_in->fh);
    if (fh && fh->handle != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(fh->handle);
    }
}

int32_t VirtioFsDevice::FillAttr(const std::wstring& path, FuseAttr* attr, uint64_t inode, bool share_readonly) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        return WindowsErrorToFuse(GetLastError());
    }

    memset(attr, 0, sizeof(*attr));
    attr->ino = inode;
    
    ULARGE_INTEGER size;
    size.HighPart = fad.nFileSizeHigh;
    size.LowPart = fad.nFileSizeLow;
    attr->size = size.QuadPart;
    attr->blocks = (attr->size + 511) / 512;

    auto FiletimeToUnix = [](const FILETIME& ft) -> uint64_t {
        ULARGE_INTEGER uli;
        uli.LowPart = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        return (uli.QuadPart / 10000000ULL) - 11644473600ULL;
    };

    attr->atime = FiletimeToUnix(fad.ftLastAccessTime);
    attr->mtime = FiletimeToUnix(fad.ftLastWriteTime);
    attr->ctime = FiletimeToUnix(fad.ftCreationTime);

    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        attr->mode = FUSE_S_IFDIR | 0777;
        attr->nlink = 2;
    } else {
        attr->mode = FUSE_S_IFREG | 0666;
        attr->nlink = 1;
    }

    if (share_readonly || (fad.dwFileAttributes & FILE_ATTRIBUTE_READONLY)) {
        attr->mode &= ~0222;
    }

    attr->uid = 0;
    attr->gid = 0;
    attr->blksize = 4096;

    return FUSE_OK;
}

int32_t VirtioFsDevice::FillVirtualRootAttr(FuseAttr* attr) {
    memset(attr, 0, sizeof(*attr));
    attr->ino = VIRTUAL_ROOT_INODE;
    attr->mode = FUSE_S_IFDIR | 0755;
    attr->nlink = 2 + static_cast<uint32_t>(shares_.size());
    attr->uid = 0;
    attr->gid = 0;
    attr->blksize = 4096;

    attr->atime = virtual_root_mtime_;
    attr->mtime = virtual_root_mtime_;
    attr->ctime = virtual_root_mtime_;

    return FUSE_OK;
}

int32_t VirtioFsDevice::FillShareRootAttr(const ShareInfo& share, FuseAttr* attr) {
    return FillAttr(share.host_path, attr, share.root_inode, share.readonly);
}

int32_t VirtioFsDevice::WindowsErrorToFuse(DWORD error) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return FUSE_ENOENT;
    case ERROR_ACCESS_DENIED:
        return FUSE_EACCES;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return FUSE_EEXIST;
    case ERROR_DIRECTORY:
        return FUSE_ENOTDIR;
    case ERROR_DIR_NOT_EMPTY:
        return FUSE_ENOTEMPTY;
    case ERROR_DISK_FULL:
        return FUSE_ENOSPC;
    case ERROR_WRITE_PROTECT:
        return FUSE_EROFS;
    case ERROR_INVALID_PARAMETER:
        return FUSE_EINVAL;
    default:
        return FUSE_EIO;
    }
}

uint64_t VirtioFsDevice::AllocInode() {
    // Note: caller should already hold mutex_
    return next_inode_++;
}

InodeInfo* VirtioFsDevice::GetInode(uint64_t inode) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = inodes_.find(inode);
    return it != inodes_.end() ? &it->second : nullptr;
}

uint64_t VirtioFsDevice::GetOrCreateInode(const std::wstring& path, bool is_dir, const std::string& share_tag) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = path_to_inode_.find(path);
    if (it != path_to_inode_.end()) {
        inodes_[it->second].nlookup++;
        return it->second;
    }

    uint64_t inode = next_inode_++;
    InodeInfo info;
    info.inode = inode;
    info.host_path = path;
    info.nlookup = 1;
    info.is_dir = is_dir;
    info.share_tag = share_tag;
    
    inodes_[inode] = info;
    path_to_inode_[path] = inode;
    
    return inode;
}

void VirtioFsDevice::RemoveInode(uint64_t inode) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = inodes_.find(inode);
    if (it != inodes_.end()) {
        path_to_inode_.erase(it->second.host_path);
        inodes_.erase(it);
    }
}

uint64_t VirtioFsDevice::AllocFileHandle(HANDLE h, bool is_dir, const std::wstring& path, const std::string& share_tag) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t fh = next_fh_++;
    file_handles_[fh] = {h, is_dir, path, share_tag};
    return fh;
}

FileHandle* VirtioFsDevice::GetFileHandle(uint64_t fh) {
    // Note: caller should handle locking if needed
    auto it = file_handles_.find(fh);
    return it != file_handles_.end() ? &it->second : nullptr;
}

void VirtioFsDevice::CloseFileHandle(uint64_t fh) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = file_handles_.find(fh);
    if (it != file_handles_.end()) {
        if (it->second.handle != INVALID_HANDLE_VALUE) {
            CloseHandle(it->second.handle);
        }
        file_handles_.erase(it);
    }
}

std::wstring VirtioFsDevice::NodeIdToPath(uint64_t nodeid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = inodes_.find(nodeid);
    return it != inodes_.end() ? it->second.host_path : L"";
}

std::string VirtioFsDevice::NodeIdToShareTag(uint64_t nodeid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = inodes_.find(nodeid);
    return it != inodes_.end() ? it->second.share_tag : "";
}

bool VirtioFsDevice::IsShareReadonly(const std::string& share_tag) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Empty share_tag means something is wrong, but don't block writes
    if (share_tag.empty()) {
        LOG_WARN("VirtIO FS: IsShareReadonly called with empty share_tag");
        return false;
    }
    
    auto it = shares_.find(share_tag);
    if (it == shares_.end()) {
        LOG_WARN("VirtIO FS: share '%s' not found in IsShareReadonly", share_tag.c_str());
        return false;  // Default to writable if share not found
    }
    
    return it->second.readonly;
}

uint32_t VirtioFsDevice::GetOpenHandleCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint32_t>(file_handles_.size());
}

std::wstring VirtioFsDevice::Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), 
                                   static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), 
                        static_cast<int>(utf8.size()), &wide[0], len);
    return wide;
}

std::string VirtioFsDevice::WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), 
                                   static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), 
                        static_cast<int>(wide.size()), &utf8[0], len, nullptr, nullptr);
    return utf8;
}
