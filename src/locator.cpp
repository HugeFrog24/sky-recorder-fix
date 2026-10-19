#include "locator.h"
#include "signatures.h"

namespace sky {
static bool unique(Address &value,Address next) {
    if(value&&value!=next) return false;
    value=next;
    return true;
}
static bool link(const Image &im,Address fn,Address target) {
    unsigned count=0;
    for(unsigned i=0;i<128;i+=4) {
        uint32_t w=im.read<uint32_t>(fn+i);
        if((w&0xfc000000)==0x94000000&&call_target(fn+i,w)==target) ++count;
    }
    return count==1;
}
bool locate(const Image &im,Profile &output,const char *&status) {
    Profile p;
    uint32_t sizes[RoleCount]{};
    status="unwind_missing_or_invalid";
    if(!im.unwind||im.read<uint32_t>(im.unwind)!=0x3b031b01) return false;
    uint32_t count=im.read<uint32_t>(im.unwind+8);
    if(count<2||count>2000000||!im.view(im.unwind+12,size_t(count)*8)) return false;
    Address prior=0;
    for(uint32_t i=0;i<count;++i) {
        Address start=im.unwind+int64_t(im.read<int32_t>(im.unwind+12+8ULL*i));
        status="unwind_missing_or_invalid";
        if(start<=prior||!im.view(start,4,5)) return false;
        prior=start;
        if(i+1==count) break;
        Address end=im.unwind+int64_t(im.read<int32_t>(im.unwind+20+8ULL*i));
        if(end<=start||(end-start)%4) return false;
        size_t size=size_t(end-start);
        bool candidate=false;
        for(const auto &s:signatures) if(s.size==size) candidate=true;
        if(!candidate) continue;
        auto bytes=im.view(start,size,5);
        if(!bytes) return false;
        auto h=fingerprint(bytes,size);
        for(const auto &s:signatures) if(s.size==size&&s.a==h.a&&s.b==h.b) {
            status="ambiguous_function";
            if(!unique(p.functions[s.role],start)) return false;
            sizes[s.role]=s.size;
        }
    }
    status="unsupported_code_shape";
    for(auto f:p.functions) if(!f) return false;
    status="helper_link_mismatch";
    if(!link(im,p.functions[Begin],p.functions[PassPool])) return false;
    status="dispatch_missing_or_ambiguous";
    for(size_t i=0;i<im.count;++i) {
        const auto &s=im.spans[i];
        if((s.flags&7)!=6) continue;
        for(Address address=(s.address+7)&~Address(7);address>=s.address&&address-s.address+8<=s.size;address+=8) {
            Address target=im.pointer(address);
            for(unsigned role=0;role<5;++role) if(target==p.functions[role]&&!unique(p.slots[role],address)) return false;
        }
    }
    for(auto s:p.slots) if(!s) return false;
    status="dispatch_relationship_mismatch";
    if(p.slots[End]!=p.slots[Complete]+16||p.slots[Register]!=p.slots[Begin]+48||p.slots[Pixels]!=p.slots[Complete]+392) return false;
    status="renderer_global_invalid";
    for(unsigned i=0;i<96;i+=4) {
        Address pc=p.functions[Begin]+i;
        Address global=global_load(im,pc,4);
        if(global&&im.read<uint32_t>(pc+8)==0xf94efd09&&!unique(p.renderer,global)) return false;
    }
    if(!p.renderer||!im.view(p.renderer,8,6)) return false;
    bool renderer_used=false;
    for(unsigned i=0;i+20<=sizes[End];i+=4) for(unsigned distance=4;distance<=16;distance+=4)
        if(global_load(im,p.functions[End]+i,distance)==p.renderer) renderer_used=true;
    if(!renderer_used) return false;
    status="wait_dispatch_invalid";
    for(unsigned i=0;i<128;i+=4) {
        Address got=global_load(im,p.functions[Complete]+i,12);
        if(!got) continue;
        uint32_t a=im.read<uint32_t>(p.functions[Complete]+i+16);
        if((a&~0x3e0U)!=0xf94efc00) continue;
        if(!unique(p.wait,im.pointer(got))) return false;
    }
    if(!p.wait||!im.view(p.wait,8,6)) return false;
    status="submit_dispatch_invalid";
    for(unsigned i=0;i+24<=sizes[End];i+=4) {
        Address pc=p.functions[End]+i;
        Address got=global_load(im,pc,4);
        if(!got) continue;
        if(im.read<uint32_t>(pc+8)!=0xf94f0ae0||im.read<uint32_t>(pc+12)!=0xf9400129||im.read<uint32_t>(pc+16)!=0xf94fb503||im.read<uint32_t>(pc+20)!=0xd63f0120) continue;
        if(!unique(p.submit,im.pointer(got))) return false;
    }
    if(!p.submit||!im.view(p.submit,8,6)||p.wait!=p.submit+80||p.submit==p.renderer||p.wait==p.renderer) return false;
    status="accepted";
    output=p;
    return true;
}
}
