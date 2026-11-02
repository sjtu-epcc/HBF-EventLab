#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace hbflab {
// Single exclusive owner, versioned little-endian file. Only explicit complete
// checkpoints may be reopened. An interrupted mutation is rejected, not replayed.
class PageFile {
public:
    struct Entry {std::uint64_t owner=0,bytes=0;};
    PageFile(const std::filesystem::path&,std::size_t pages,bool create);
    ~PageFile();
    PageFile(const PageFile&)=delete;
    void read(std::size_t,std::array<unsigned char,4096>&);
    void write(std::size_t,const std::array<unsigned char,4096>&);
    void dirty();
    void checkpoint(const std::vector<Entry>&,std::uint64_t serial);
    const std::vector<Entry>& entries() const{return entries_;}
    std::uint64_t serial() const{return serial_;}
private:
    void transfer(std::uint64_t,void*,std::size_t,bool write);
    void sync();
    std::intptr_t handle_=-1;
    std::size_t pages_=0;
    bool clean_=false;
    std::uint64_t serial_=0;
    std::vector<Entry> entries_;
};
}
