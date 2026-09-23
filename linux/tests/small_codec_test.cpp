#include "../../src/small_codec.h"
#include <cassert>
#include <memory>
#include <vector>
#include <cstdio>
int main(){
    auto enc=std::make_unique<BlockCodec::EncodeScratch>();
    auto dec=std::make_unique<BlockCodec::DecodeScratch>();
    std::vector<uint16_t> source(BlockCodec::MaxSamples),output(BlockCodec::MaxSamples);
    std::vector<uint8_t> packed(BlockCodec::MaxSamples*3);
    uint32_t rng=123;
    for(int pattern=0;pattern<3;++pattern){
        for(size_t i=0;i<source.size();++i){rng=rng*1664525+1013904223;source[i]=pattern==0?0:pattern==1?uint16_t(i):uint16_t(rng>>16);}
        for(size_t n:{size_t(1),size_t(2048),size_t(66049),BlockCodec::MaxSamples}){
            auto bytes=BlockCodec::Encode(source.data(),n,packed.data(),packed.size(),*enc);assert(bytes);
            assert(BlockCodec::Decode(packed.data(),bytes,output.data(),n,*dec));
            assert(!memcmp(source.data(),output.data(),n*sizeof(uint16_t)));
            assert(!BlockCodec::Decode(packed.data(),bytes-1,output.data(),n,*dec));
            // Corrupt the stored content hash, preserving the stream layout.
            packed[5]^=1;assert(!BlockCodec::Decode(packed.data(),bytes,output.data(),n,*dec));packed[5]^=1;
            assert(!BlockCodec::Encode(source.data(),n,packed.data(),1,*enc));
        }
    }
    assert(!BlockCodec::Encode(source.data(),0,packed.data(),packed.size(),*enc));
    assert(!BlockCodec::Encode(source.data(),BlockCodec::MaxSamples+1,packed.data(),packed.size(),*enc));
    puts("PASS: shared variable block codec, bounds, truncation and hash corruption");
}
