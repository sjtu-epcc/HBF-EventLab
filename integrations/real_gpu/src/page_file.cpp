#include "page_file.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace hbflab {
namespace {
constexpr std::uint64_t header_size=64, entry_size=16;
void put(unsigned char* p,std::uint64_t v){for(unsigned i=0;i<8;++i)p[i]=static_cast<unsigned char>(v>>(i*8));}
std::uint64_t get(const unsigned char* p){std::uint64_t v=0;for(unsigned i=0;i<8;++i)v|=std::uint64_t(p[i])<<(i*8);return v;}
void close_file(std::intptr_t h) {
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(h));
#else
    close(static_cast<int>(h));
#endif
}
}
PageFile::PageFile(const std::filesystem::path& path,std::size_t pages,bool create):pages_(pages),entries_(pages) {
#ifdef _WIN32
    auto h=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,create?CREATE_NEW:OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot exclusively open capacity backing file");
    handle_=reinterpret_cast<std::intptr_t>(h);
#else
    handle_=open(path.c_str(),O_RDWR|(create?(O_CREAT|O_EXCL):0),0600);
    if(handle_<0)throw std::runtime_error("cannot open capacity backing file");
    if(flock(static_cast<int>(handle_),LOCK_EX|LOCK_NB)!=0){close_file(handle_);handle_=-1;throw std::runtime_error("backing file already owned");}
#endif
    try {
        std::array<unsigned char,64> header{};
        if(create) {
            std::memcpy(header.data(),"HBFPAGE1",8);put(header.data()+8,1);put(header.data()+16,pages);
            transfer(0,header.data(),header.size(),true);
            std::vector<unsigned char> catalog(pages*entry_size,0);transfer(header_size,catalog.data(),catalog.size(),true);
            std::array<unsigned char,4096> zeros{};
            for(std::size_t p=0;p<pages;++p)write(p,zeros);
            sync(); // Starts unclean until CapacityStore::flush_all().
        } else {
            transfer(0,header.data(),header.size(),false);
            if(std::memcmp(header.data(),"HBFPAGE1",8) || get(header.data()+8)!=1 || get(header.data()+16)!=pages || get(header.data()+24)!=1)
                throw std::runtime_error("invalid, incompatible or uncheckpointed backing file");
            serial_=get(header.data()+32);clean_=true;
            std::vector<unsigned char> catalog(pages*entry_size);transfer(header_size,catalog.data(),catalog.size(),false);
            for(std::size_t p=0;p<pages;++p)entries_[p]={get(catalog.data()+p*16),get(catalog.data()+p*16+8)};
            // Check exact length before a store can expose restored allocations.
#ifdef _WIN32
            LARGE_INTEGER size{};if(!GetFileSizeEx(reinterpret_cast<HANDLE>(handle_),&size) || static_cast<std::uint64_t>(size.QuadPart)!=header_size+pages*(entry_size+4096))
#else
            if(static_cast<std::uint64_t>(lseek(static_cast<int>(handle_),0,SEEK_END))!=header_size+pages*(entry_size+4096))
#endif
                throw std::runtime_error("backing file length mismatch");
        }
    }catch(...){close_file(handle_);handle_=-1;throw;}
}
PageFile::~PageFile(){if(handle_!=-1)close_file(handle_);}
void PageFile::transfer(std::uint64_t offset,void* data,std::size_t bytes,bool writing) {
#ifdef _WIN32
    LARGE_INTEGER at{};at.QuadPart=static_cast<LONGLONG>(offset);
    if(!SetFilePointerEx(reinterpret_cast<HANDLE>(handle_),at,nullptr,FILE_BEGIN))throw std::runtime_error("backing seek failed");
    DWORD done=0;
    const auto ok=writing?WriteFile(reinterpret_cast<HANDLE>(handle_),data,static_cast<DWORD>(bytes),&done,nullptr):ReadFile(reinterpret_cast<HANDLE>(handle_),data,static_cast<DWORD>(bytes),&done,nullptr);
    if(!ok || done!=bytes)throw std::runtime_error("backing I/O failed or truncated");
#else
    auto* p=static_cast<unsigned char*>(data);
    while(bytes){const auto n=writing?pwrite(static_cast<int>(handle_),p,bytes,offset):pread(static_cast<int>(handle_),p,bytes,offset);
        if(n<=0)throw std::runtime_error("backing I/O failed or truncated");p+=n;bytes-=n;offset+=n;}
#endif
}
void PageFile::sync(){
#ifdef _WIN32
    if(!FlushFileBuffers(reinterpret_cast<HANDLE>(handle_)))throw std::runtime_error("backing flush failed");
#else
    if(fsync(static_cast<int>(handle_))!=0)throw std::runtime_error("backing flush failed");
#endif
}
void PageFile::dirty(){if(clean_){std::array<unsigned char,8> flag{};transfer(24,flag.data(),8,true);sync();clean_=false;}}
void PageFile::read(std::size_t page,std::array<unsigned char,4096>& data){if(page>=pages_)throw std::out_of_range("backing page");transfer(header_size+pages_*entry_size+page*4096,data.data(),4096,false);}
void PageFile::write(std::size_t page,const std::array<unsigned char,4096>& data){if(page>=pages_)throw std::out_of_range("backing page");dirty();transfer(header_size+pages_*entry_size+page*4096,const_cast<unsigned char*>(data.data()),4096,true);}
void PageFile::checkpoint(const std::vector<Entry>& entries,std::uint64_t serial) {
    if(entries.size()!=pages_)throw std::invalid_argument("backing catalog size");dirty();
    std::vector<unsigned char> catalog(pages_*entry_size);
    for(std::size_t p=0;p<pages_;++p){put(catalog.data()+p*16,entries[p].owner);put(catalog.data()+p*16+8,entries[p].bytes);}
    transfer(header_size,catalog.data(),catalog.size(),true);
    std::array<unsigned char,8> value{};put(value.data(),serial);transfer(32,value.data(),8,true);sync();
    put(value.data(),1);transfer(24,value.data(),8,true);sync();clean_=true;
}
}
