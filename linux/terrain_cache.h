// Persistent, exact-input terrain kernel cache. Independent of save names and
// mutable terrain ownership. A hit restores only the kernel's output samples.
#pragma once
#include <atomic>
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
namespace terrain_cache {
using Bytes=std::vector<uint8_t>;
inline uint64_t Hash(const void* ptr,size_t n,uint64_t h=14695981039346656037ull) {
    auto* p=static_cast<const uint8_t*>(ptr);
    // Exact key comparison handles hash collisions. Word loads reduce checksum
    // overhead on the thousands of small blocks in a native terrain rebuild.
    while(n>=8){uint64_t word;memcpy(&word,p,8);h=(h^word)*1099511628211ull;p+=8;n-=8;}
    while(n--){h=(h^*p++)*1099511628211ull;}return h;
}
inline void Add(Bytes& key,const void* data,size_t bytes) {
    const auto* p=static_cast<const uint8_t*>(data);if(bytes)key.insert(key.end(),p,p+bytes);
}
struct Store {
    std::string directory;uint64_t limit=0;
    std::atomic<uint64_t> used{0},hits{0},misses{0},writes{0},rejected{0},serial{0};
    bool Open(const std::string& path,uint64_t budget) {
        if(mkdir(path.c_str(),0700) && errno!=EEXIST)return false;
        struct stat st{};if(lstat(path.c_str(),&st) || !S_ISDIR(st.st_mode))return false;
        DIR* dir=opendir(path.c_str());if(!dir)return false;
        uint64_t bytes=0;
        while(auto* e=readdir(dir)){std::string name=e->d_name;
            if(name.size()>6 && name.substr(name.size()-6)==".cache" && !lstat((path+"/"+name).c_str(),&st) && S_ISREG(st.st_mode))bytes+=st.st_size;
        }
        closedir(dir);directory=path;limit=budget;used=bytes;return true;
    }
    std::string Path(const Bytes& key)const {
        char name[48];snprintf(name,sizeof(name),"/%016llx%016llx.cache",
            (unsigned long long)Hash(key.data(),key.size()),
            (unsigned long long)Hash(key.data(),key.size(),0x9e3779b97f4a7c15ull));return directory+name;
    }
    static bool Transfer(int fd,void* ptr,size_t n,bool writing) {
        auto* p=static_cast<uint8_t*>(ptr);
        while(n){ssize_t got=writing?write(fd,p,n):read(fd,p,n);if(got<0 && errno==EINTR)continue;if(got<=0)return false;p+=got;n-=size_t(got);}return true;
    }
    bool Get(const Bytes& key,Bytes& out,size_t expected) {
        if(directory.empty())return false;
        int fd=open(Path(key).c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0){++misses;return false;}
        // Fixed-width header; v1 and kernel revision are included in the key.
        uint64_t h[5]{};struct stat st{};bool ok=!fstat(fd,&st) && S_ISREG(st.st_mode) && Transfer(fd,h,sizeof(h),false)
            && h[0]==0x3145484341435442ull && h[1]==key.size() && h[2]==expected
            && expected<=8*1024*1024 && key.size()<=8*1024*1024 && uint64_t(st.st_size)==sizeof(h)+h[1]+h[2];
        Bytes actual;if(ok){actual.resize(key.size());out.resize(expected);ok=Transfer(fd,actual.data(),actual.size(),false) && actual==key
            && Transfer(fd,out.data(),out.size(),false) && Hash(out.data(),out.size())==h[3] && Hash(actual.data(),actual.size())==h[4];}
        close(fd);if(ok){++hits;return true;}out.clear();++misses;++rejected;return false;
    }
    void Put(const Bytes& key,const Bytes& out) {
        if(directory.empty() || key.size()>8*1024*1024 || out.size()>8*1024*1024)return;
        const uint64_t bytes=40+key.size()+out.size();uint64_t old=used.load();
        do{if(old>limit || bytes>limit-old)return;}while(!used.compare_exchange_weak(old,old+bytes));
        auto path=Path(key);auto temp=path+".tmp."+std::to_string(getpid())+"."+std::to_string(serial++);
        int fd=open(temp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        uint64_t h[]={0x3145484341435442ull,key.size(),out.size(),Hash(out.data(),out.size()),Hash(key.data(),key.size())};
        bool ok=fd>=0 && Transfer(fd,h,sizeof(h),true) && Transfer(fd,const_cast<uint8_t*>(key.data()),key.size(),true)
            && Transfer(fd,const_cast<uint8_t*>(out.data()),out.size(),true);
        if(fd>=0 && close(fd))ok=false;
        // link publishes only complete entries and never replaces another writer.
        if(ok && !link(temp.c_str(),path.c_str()))++writes;else used-=bytes;
        unlink(temp.c_str());
    }
};
inline Store& Cache(){static auto* cache=new Store;return *cache;}
inline bool Overlap(const void* a,size_t n,const void* b,size_t m) {
    auto x=uintptr_t(a),y=uintptr_t(b);return x<y?y-x<n:x-y<m;
}
}
