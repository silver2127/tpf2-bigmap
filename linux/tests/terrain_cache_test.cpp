#include "../terrain_cache.h"
#include <cassert>
#include <fstream>
#include <thread>
int main(){
    using namespace terrain_cache;
    char temp[]="/tmp/bigmap-cache.XXXXXX";assert(mkdtemp(temp));
    Store store;assert(store.Open(temp,1<<20));Bytes key{1,2,3},out{4,5,6,7},got;
    assert(!store.Get(key,got,4));store.Put(key,out);assert(store.Get(key,got,4) && got==out);
    Store reopened;assert(reopened.Open(temp,1<<20));assert(reopened.Get(key,got,4) && got==out);
    assert(!reopened.Get(key,got,3));
    Bytes other{1,2,4};assert(!store.Get(other,got,4));
    // Even a deliberate filename collision must compare the entire input key.
    assert(!link(store.Path(key).c_str(),store.Path(other).c_str()));
    assert(!store.Get(other,got,4));assert(!unlink(store.Path(other).c_str()));
    int fd=open(store.Path(key).c_str(),O_WRONLY);assert(fd>=0);
    uint8_t corrupt=0xff;assert(pwrite(fd,&corrupt,1,43)==1);close(fd);
    assert(!store.Get(key,got,4));assert(!unlink(store.Path(key).c_str()));
    // Concurrent misses publish one complete entry, with exact budget accounting.
    auto before=store.used.load();std::vector<std::thread> workers;
    for(int i=0;i<16;++i)workers.emplace_back([&]{store.Put(key,out);});
    for(auto& t:workers)t.join();assert(store.Get(key,got,4) && got==out);
    assert(store.used==before+47);assert(!unlink(store.Path(key).c_str()));
    store.limit=store.used.load();store.Put(other,out);assert(!store.Get(other,got,4));
    assert(!rmdir(temp));
}
