#pragma once
#include "terrain_cache.h"
#include <array>
#include <mutex>
#include <unordered_map>

// Resident, exact-input cache of completed alignment work blocks. No engine
// pointers are retained and no file is opened on the worker threads.
namespace terrain_chunk {
using terrain_cache::Bytes;
class Store {
    struct Entry { Bytes key, output; };
    struct Shard { std::mutex mutex; std::unordered_multimap<uint64_t,Entry> entries; };
    std::array<Shard,16> shards;
public:
    uint64_t limit=0;
    std::atomic<uint64_t> used{0},hits{0},misses{0},writes{0},full{0};
    bool Get(const Bytes& key,Bytes& output) {
        const auto hash=terrain_cache::Hash(key.data(),key.size());
        auto& shard=shards[hash%shards.size()];
        std::lock_guard<std::mutex> lock(shard.mutex);
        const auto range=shard.entries.equal_range(hash);
        for(auto it=range.first;it!=range.second;++it)if(it->second.key==key) {
            output=it->second.output;++hits;return true;
        }
        ++misses;return false;
    }
    void Put(const Bytes& key,const Bytes& output) {
        if(key.size()>1024*1024 || output.size()>1024*1024)return;
        const auto hash=terrain_cache::Hash(key.data(),key.size());
        auto& shard=shards[hash%shards.size()];
        std::lock_guard<std::mutex> lock(shard.mutex);
        const auto range=shard.entries.equal_range(hash);
        for(auto it=range.first;it!=range.second;++it)if(it->second.key==key)return;
        // Includes a conservative allowance for the node, bucket and allocator.
        const uint64_t charge=key.size()+output.size()+256;
        auto old=used.load();
        do {if(old>limit || charge>limit-old){++full;return;}}
        while(!used.compare_exchange_weak(old,old+charge));
        try {shard.entries.emplace(hash,Entry{key,output});++writes;}
        catch(...) {used-=charge;throw;}
    }
};
inline Store& Cache(){static auto* value=new Store;return *value;}
}
