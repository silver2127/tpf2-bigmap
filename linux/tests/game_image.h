#pragma once
#include <elf.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <fstream>
#include <vector>
#include <cassert>
#include <cstring>
#include <algorithm>
struct GameImage {
    char* data;size_t size=0;
    explicit GameImage(const char* file) {
        std::ifstream f(file,std::ios::binary);Elf64_Ehdr eh{};
        f.read(reinterpret_cast<char*>(&eh),sizeof(eh));
        assert(f && !memcmp(eh.e_ident,ELFMAG,SELFMAG) && eh.e_machine==EM_X86_64);
        std::vector<Elf64_Phdr> ph(eh.e_phnum);
        f.seekg(eh.e_phoff);f.read(reinterpret_cast<char*>(ph.data()),ph.size()*sizeof(ph[0]));
        for(auto& p:ph)if(p.p_type==PT_LOAD)size=std::max(size,size_t(p.p_vaddr+p.p_memsz));
        data=static_cast<char*>(mmap(nullptr,size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));assert(data!=MAP_FAILED);
        for(auto& p:ph)if(p.p_type==PT_LOAD){f.seekg(p.p_offset);f.read(data+p.p_vaddr,p.p_filesz);assert(f);}
        Elf64_Dyn* dyn=nullptr;for(auto& p:ph)if(p.p_type==PT_DYNAMIC)dyn=reinterpret_cast<Elf64_Dyn*>(data+p.p_vaddr);
        assert(dyn);
        auto value=[&](int64_t tag){for(auto* d=dyn;d->d_tag!=DT_NULL;++d)if(d->d_tag==tag)return d->d_un.d_val;return uint64_t(0);};
        auto* sym=reinterpret_cast<Elf64_Sym*>(data+value(DT_SYMTAB));auto* str=data+value(DT_STRTAB);
        auto relocate=[&](uint64_t start,uint64_t bytes){
            auto* rel=reinterpret_cast<Elf64_Rela*>(data+start);
            for(size_t i=0;i<bytes/sizeof(*rel);++i){
                const auto& r=rel[i];auto* out=reinterpret_cast<uint64_t*>(data+r.r_offset);
                auto type=ELF64_R_TYPE(r.r_info);
                if(type==R_X86_64_RELATIVE)*out=uint64_t(data)+r.r_addend;
                else if(type==R_X86_64_64 || type==R_X86_64_GLOB_DAT || type==R_X86_64_JUMP_SLOT){
                    auto& s=sym[ELF64_R_SYM(r.r_info)];void* address=dlsym(RTLD_DEFAULT,str+s.st_name);
                    if(!address && s.st_shndx!=SHN_UNDEF)address=data+s.st_value;
                    if(address)*out=uint64_t(address)+r.r_addend;
                }
            }
        };
        relocate(value(DT_RELA),value(DT_RELASZ));relocate(value(DT_JMPREL),value(DT_PLTRELSZ));
    }
    ~GameImage(){munmap(data,size);}
};
