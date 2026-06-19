#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Minimal Flattened Device Tree (FDT) builder that generates a DTB blob
// without depending on external libfdt.  Sufficient for the simple virt
// machine description needed by AgentSphere.
class FdtBuilder {
public:
    FdtBuilder();

    void BeginNode(const std::string& name);
    void EndNode();

    void AddPropertyU32(const std::string& name, uint32_t value);
    void AddPropertyU64(const std::string& name, uint64_t value);
    void AddPropertyString(const std::string& name, const std::string& value);
    void AddPropertyEmpty(const std::string& name);

    // Add a #address-cells / #size-cells pair of u32 property
    void AddPropertyCells(const std::string& name, const std::vector<uint32_t>& cells);

    // Add a raw byte array property
    void AddPropertyBytes(const std::string& name, const uint8_t* data, size_t len);

    // Finalize and return the complete DTB blob. Must not add more
    // nodes/properties after this call.
    std::vector<uint8_t> Finish();

    // Reserve a phandle and return the next available value.
    uint32_t AllocPhandle() { return next_phandle_++; }

private:
    void PadTo4(std::vector<uint8_t>& buf);
    uint32_t AddString(const std::string& s);

    std::vector<uint8_t> struct_buf_;
    std::vector<uint8_t> strings_buf_;
    uint32_t next_phandle_ = 1;
};
