#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sky {
using Address=uint64_t;
struct Span { Address address; const uint8_t *data; size_t size; uint32_t flags; };
struct Image {
    Span spans[16]{};
    size_t count=0;
    Address unwind=0, bias=0;
    const uint8_t *view(Address address,size_t size,uint32_t flags=4) const {
        for(size_t i=0;i<count;++i) {
            const auto &s=spans[i];
            if((s.flags&flags)==flags&&address>=s.address&&size<=s.size&&address-s.address<=s.size-size) return s.data+(address-s.address);
        }
        return nullptr;
    }
    template<class T> T read(Address address) const {
        T value{};
        if(auto p=view(address,sizeof(T))) memcpy(&value,p,sizeof(T));
        return value;
    }
    Address pointer(Address address) const { auto v=read<Address>(address); return v>=bias?v-bias:0; }
};
enum Role { Pixels, End, Begin, Complete, Register, Callback, PassPool, RoleCount };
struct Profile {
    Address functions[RoleCount]{};
    Address slots[5]{};
    Address renderer=0, submit=0, wait=0;
};
struct Fingerprint { uint32_t size; uint64_t a,b; Role role; };
struct Digest { uint64_t a=0xcbf29ce484222325ULL,b=0x84222325cbf29ce4ULL; };
inline Digest fingerprint(const uint8_t *code,size_t size) {
    constexpr size_t limit=1024;
    if(size%4||size/4>limit) return {0,0};
    const size_t count=size/4;
    uint32_t words[limit]{},pages[limit]{};
    uint16_t queue[limit]{};
    bool reached[limit]{},queued[limit]{};
    size_t head=0,tail=0,pending=0;
    for(size_t i=0;i<count;++i) memcpy(words+i,code+4*i,4);
    auto merge=[&](int64_t index,uint32_t state) {
        if(index<0||uint64_t(index)>=count) return;
        size_t next=size_t(index);
        uint32_t combined=reached[next]?pages[next]&state:state;
        if(reached[next]&&pages[next]==combined) return;
        pages[next]=combined;
        reached[next]=true;
        if(!queued[next]) {
            queue[tail]=uint16_t(next);
            tail=(tail+1)%limit;
            ++pending;
            queued[next]=true;
        }
    };
    merge(0,0);
    while(pending) {
        size_t i=queue[head];
        head=(head+1)%limit;
        --pending;
        queued[i]=false;
        uint32_t w=words[i],state=pages[i],rd=w&31,rn=(w>>5)&31;
        auto kill=[&](unsigned reg) { state&=~(1U<<reg); };
        bool direct=(w&0xfc000000)==0x94000000;
        bool indirect=(w&0xfffffc1f)==0xd63f0000;
        if((w&0xfc000000)==0x14000000) {
            int64_t offset=w&0x3ffffff;
            if(offset&(1<<25)) offset-=1<<26;
            merge(int64_t(i)+offset,state);
            continue;
        }
        bool conditional=(w&0xff000010)==0x54000000||(w&0x7e000000)==0x34000000;
        bool test=(w&0x7e000000)==0x36000000;
        if(conditional||test) {
            unsigned bits=test?14:19;
            int64_t offset=(w>>5)&((1U<<bits)-1);
            if(offset&(1U<<(bits-1))) offset-=1U<<bits;
            merge(int64_t(i)+offset,state);
            merge(int64_t(i)+1,state);
            continue;
        }
        if((w&0xfffffc1f)==0xd65f0000||(w&0xfffffc1f)==0xd61f0000) continue;
        if(direct||indirect) state&=0x3ff80000U;
        else if((w&0x9f000000)==0x90000000) {
            if(rd!=31) state|=1U<<rd;
        } else if((w&0x1c000000)==0x10000000||(w&0x0e000000)==0x0a000000||(w&0x0e000000)==0x0e000000) kill(rd);
        else if((w&0x3a000000)==0x28000000) {
            if((w&0x04400000)==0x00400000) { kill(rd); kill((w>>10)&31); }
            if(w&0x00800000) kill(rn);
        } else if((w&0x3b000000)==0x39000000||(w&0x3b200000)==0x38000000||(w&0x3b200c00)==0x38200800) {
            if(!(w&0x04000000)&&(w&0x00c00000)) kill(rd);
            if((w&0x3b200000)==0x38000000&&(w&0x400)) kill(rn);
        } else if((w&0x3b000000)==0x18000000) {
            if(!(w&0x04000000)) kill(rd);
        } else if((w&0xfffff01f)==0xd503201f) kill(30);
        else if((w&0xfffff000)==0xd5033000) {}
        else if((w&0xfff00000)==0xd5300000) kill(rd);
        else state=0;
        merge(int64_t(i)+1,state&0x7fffffffU);
    }
    Digest h;
    for(size_t i=0;i<count;++i) {
        uint32_t w=words[i],n=w,rn=(w>>5)&31;
        if((w&0x1f000000)==0x10000000) n=w&0x9f00001f;
        if((w&0xfc000000)==0x94000000) n=w&0xfc000000;
        if(reached[i]&&(pages[i]&(1U<<rn))&&((w&0xffc00000)==0x91000000||(w&0x3b000000)==0x39000000)) n=w&~(4095U<<10);
        h.a=(h.a^n)*0x100000001b3ULL;
        h.b=(h.b+n)*0x9e3779b185ebca87ULL;
        h.b=(h.b<<13)|(h.b>>51);
    }
    return h;
}
inline Address page_address(Address pc,uint32_t w) {
    int64_t imm=int64_t(((w>>5)&0x7ffff)<<2|((w>>29)&3));
    if(imm&(1<<20)) imm-=1<<21;
    return (pc&~Address(4095))+imm*4096;
}
inline Address call_target(Address pc,uint32_t w) {
    int64_t imm=w&0x3ffffff;
    if(imm&(1<<25)) imm-=1<<26;
    return pc+imm*4;
}
inline Address global_load(const Image &im,Address pc,unsigned distance) {
    uint32_t a=im.read<uint32_t>(pc),b=im.read<uint32_t>(pc+distance);
    if((a&0x9f000000)!=0x90000000||(b&0xffc00000)!=0xf9400000||((b>>5)&31)!=(a&31)) return 0;
    Address result=page_address(pc,a)+((b>>10)&4095)*8;
    return result%8==0&&im.view(result,8)?result:0;
}
bool locate(const Image &,Profile &,const char *&);
}
