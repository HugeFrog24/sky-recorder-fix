#include <android/log.h>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "locator.h"
#include "cache_path.h"
#include "ownership.h"

#ifndef SKY_RECORDER_TRACE
#define SKY_RECORDER_TRACE 0
#endif
static constexpr bool tracing=SKY_RECORDER_TRACE!=0;

using U = uintptr_t;
template<class T> static T rd(U p) { T v; memcpy(&v, reinterpret_cast<void *>(p), sizeof(v)); return v; }
static U base;
static sky::Profile profile;
static uint64_t decoder_ns;
static const char *profile_status="bootloader_not_found";
static char observed_build_id[129]="unavailable";
static U rejected_address;
static std::atomic<bool> initialized{false};
static std::atomic<uint64_t> active_until{0};
static uint64_t now_ns() { if(!tracing) return 0; timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t); return uint64_t(t.tv_sec) * 1000000000ULL + t.tv_nsec; }
static bool active() { return tracing&&now_ns() < active_until.load(std::memory_order_relaxed); }
static U renderer() { return rd<U>(base + profile.renderer); }

struct Row { uint64_t time; int64_t tid; const char *kind; int64_t v[12]; };
struct Cell { std::atomic<size_t> sequence; Row row; };
static Cell cells[8192];
static std::atomic<size_t> head{0};
static size_t tail;
static std::atomic<uint64_t> lost{0};
static std::atomic<bool> writer_ready{false}, writer_ok{false};
static FILE *trace;
static void emit(const char *kind, uint64_t a=0, uint64_t b=0, uint64_t c=0, uint64_t d=0,
                 uint64_t e=0, uint64_t f=0, uint64_t g=0, uint64_t h=0, uint64_t i=0,
                 uint64_t j=0, uint64_t k=0, uint64_t l=0) {
    if(!tracing) return;
    Row row{now_ns(), syscall(SYS_gettid), kind, {int64_t(a),int64_t(b),int64_t(c),int64_t(d),
        int64_t(e),int64_t(f),int64_t(g),int64_t(h),int64_t(i),int64_t(j),int64_t(k),int64_t(l)}};
    size_t pos=head.load(std::memory_order_relaxed);
    for (;;) {
        Cell &cell=cells[pos%8192];
        intptr_t difference=intptr_t(cell.sequence.load(std::memory_order_acquire))-intptr_t(pos);
        if (difference==0) {
            if (!head.compare_exchange_weak(pos,pos+1,std::memory_order_relaxed)) continue;
            cell.row=row;
            cell.sequence.store(pos+1,std::memory_order_release);
            return;
        }
        if (difference<0) { lost.fetch_add(1); return; }
        pos=head.load(std::memory_order_relaxed);
    }
}
static void *write_loop(void *) {
    char process[128]{};
    FILE *cmd = fopen("/proc/self/cmdline", "rb");
    if (cmd) { fread(process, 1, sizeof(process)-1, cmd); fclose(cmd); }
    for (char *p=process; *p; ++p) if (!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='.'||*p=='_')) *p='_';
    Dl_info module{};
    char package[128]{};
    if (!dladdr(reinterpret_cast<void *>(write_loop),&module) || !module_package(module.dli_fname,package,sizeof(package))) {
        __android_log_print(ANDROID_LOG_ERROR,"SkyRecorderFix","Cannot resolve package from module path %s; no hooks installed",module.dli_fname?module.dli_fname:"unavailable");
        writer_ready.store(true,std::memory_order_release);
        return nullptr;
    }
    char dir[256], path[384];
    snprintf(dir, sizeof(dir), "/sdcard/Android/data/%s/cache", package);
    mkdir(dir, 0700);
    snprintf(path, sizeof(path), "%s/native-recorder-fix-%lld-%d.csv", dir, static_cast<long long>(time(nullptr)), getpid());
    trace = fopen(path, "wx");
    if (!trace) {
        __android_log_print(ANDROID_LOG_ERROR, "SkyRecorderFix", "Cannot create trace in %s; no hooks installed", dir);
        writer_ready.store(true, std::memory_order_release);
        return nullptr;
    }
    fprintf(trace, "# native-recorder-fix 0.2.0 copy-before-reuse\n# runtime_guard=relocation-normalized-functions+decoded-globals+dispatch-relationships\n# base=%" PRIuPTR "\n# process=%s\n# package=%s\n# module=%s\n# profile_status=%s\n# observed_build_id=%s\n# rejected_address=%" PRIuPTR "\ntime_ns,tid,event,a,b,c,d,e,f,g,h,i,j,k,l\n", base, process, package, module.dli_fname, profile_status, observed_build_id, rejected_address);
    fprintf(trace,"# decoder_ns=%" PRIu64 "\n# renderer=%" PRIu64 "\n# submit=%" PRIu64 "\n# wait=%" PRIu64 "\n",decoder_ns,profile.renderer,profile.submit,profile.wait);
    for(unsigned i=0;i<sky::RoleCount;++i) fprintf(trace,"# function_%u=%" PRIu64 "\n",i,profile.functions[i]);
    for(unsigned i=0;i<5;++i) fprintf(trace,"# slot_%u=%" PRIu64 "\n",i,profile.slots[i]);
    fflush(trace);
    __android_log_print(ANDROID_LOG_INFO, "SkyRecorderFix", "Trace: %s", path);
    writer_ok.store(true, std::memory_order_release);
    writer_ready.store(true, std::memory_order_release);
    uint64_t last_lost=UINT64_MAX;
    for (;;) {
        Row batch[128];
        size_t count=0;
        while(count<128) {
            Cell &cell=cells[tail%8192];
            if(cell.sequence.load(std::memory_order_acquire)!=tail+1) break;
            batch[count++]=cell.row;
            cell.sequence.store(tail+8192,std::memory_order_release);
            ++tail;
        }
        for (size_t n=0; n<count; ++n) {
            Row &r=batch[n];
            fprintf(trace, "%" PRIu64 ",%" PRId64 ",%s", r.time, r.tid, r.kind);
            for (auto v:r.v) fprintf(trace, ",%" PRId64, v);
            fputc('\n', trace);
        }
        uint64_t dropped=lost.load();
        if (dropped!=last_lost) { fprintf(trace,"# dropped=%" PRIu64 "\n",dropped); last_lost=dropped; }
        if (ferror(trace)) { __android_log_print(ANDROID_LOG_ERROR,"SkyRecorderFix","Trace write failed"); return nullptr; }
        fflush(trace);
        if (count<128) usleep(200000);
    }
}

using Callback=void (*)(void *);
using Register=void (*)(void *,Callback,void *);
using BeginPass=void *(*)(void *,uint32_t,uint64_t);
using RendererCall=void (*)(void *);
using Pixels=void (*)(void *,uint32_t,void *,uint64_t,uint64_t);
static Register original_register;
static BeginPass original_begin_pass;
static RendererCall original_end_frame, original_complete;
static Pixels original_pixels;
static PFN_vkQueueSubmit original_submit;
static PFN_vkWaitForFences original_wait;
static PFN_vkGetFenceStatus fence_status;
static PFN_vkGetDeviceProcAddr device_proc;
static thread_local U current_renderer;
struct Context {
    Ownership ownership;
    uint64_t id;
    void *data;
    U object;
    uint32_t frame, serial;
    uint8_t recording;
    U renderer, command, image, fence, device;
    uint32_t handle, width, height, slot;
    uint8_t format;
    bool capture_valid, frozen_delivery, consumed;
    uint8_t *buffer;
    size_t capacity, bytes;
    uint64_t signature;
};
static Context contexts[64];
static std::atomic<uint64_t> next_id{1};
static thread_local Context *current_context;
static pthread_mutex_t state_lock=PTHREAD_MUTEX_INITIALIZER;
static size_t allocated_bytes;
struct Capture {
    U renderer, encoder, command, image, result;
    uint32_t serial, slot, pass, handle, width, height;
    uint8_t format;
    bool valid, logged;
};
static Capture captures[64];
static size_t capture_head;
static bool backend_ready;
static void log_capture(Capture &c) {
    if(c.logged) return;
    emit("capture",c.renderer,c.serial,c.slot,c.encoder,c.command,c.pass,c.handle,c.image,c.width,c.height,c.result);
    c.logged=true;
}
static Capture *find_capture(U r,uint32_t serial,U command) {
    Capture *found=nullptr;
    for(auto &c:captures) if(c.valid&&c.renderer==r&&c.serial==serial&&c.command==command) {
        if(found) return nullptr;
        found=&c;
    }
    return found;
}
static bool rgba_format(uint8_t format) {
    return (format>=8&&format<=16)||format==18||format==20||format==22||format==24||format==27||format==28;
}

static U texture(U r,uint32_t handle) {
    if (!r || handle>=rd<uint32_t>(r+0xe90)) return 0;
    U mask=rd<U>(r+0xea0), pool=rd<U>(r+0xe98);
    if (!mask || !pool || !(rd<uint32_t>(mask+4*(handle/32)) & (1U<<(handle%32)))) return 0;
    return pool+U(handle)*0xb8;
}
static void sample_fences(U r,uint64_t id,uint32_t phase) {
    if (!tracing || !fence_status || !r) return;
    VkDevice device=reinterpret_cast<VkDevice>(rd<U>(r+0x1df8));
    if (!device) return;
    for (uint32_t slot=0;slot<3;++slot) {
        U s=r+U(slot)*0x50;
        int32_t serial=rd<int32_t>(s+0x1fa0);
        VkFence fence=reinterpret_cast<VkFence>(rd<U>(s+0x1f68));
        if (serial!=-1 && fence) emit("fence",id,phase,slot,uint32_t(serial),reinterpret_cast<U>(fence),fence_status(device,fence));
    }
}
static void callback(void *opaque) {
    auto *c=static_cast<Context *>(opaque);
    pthread_mutex_lock(&state_lock);
    Delivery delivery=c->ownership.claim();
    if(delivery==Delivery::None) {
        emit("duplicate_callback",c->id);
        pthread_mutex_unlock(&state_lock);
        return;
    }
    c->frozen_delivery=delivery==Delivery::Frozen;
    c->consumed=false;
    pthread_mutex_unlock(&state_lock);
    Context *previous=current_context;
    current_context=c;
    U actual=rd<U>(reinterpret_cast<U>(c->data));
    uint8_t recording=rd<uint8_t>(reinterpret_cast<U>(c->data)+8);
    uint64_t start=now_ns();
    emit("callback_begin",c->id,current_renderer,c->serial,c->frame,rd<uint64_t>(c->object+0x148),c->object,actual,recording,actual!=c->object||recording!=c->recording);
    struct Job { U object; uint8_t recording; uint8_t padding[7]; } job{c->object,c->recording,{}};
    reinterpret_cast<Callback>(base+profile.functions[sky::Callback])(&job);
    emit("callback_end",c->id,now_ns()-start,rd<uint64_t>(c->object+0x148));
    if(c->frozen_delivery&&!c->consumed) emit("snapshot_unused",c->id,c->image,c->bytes);
    current_context=previous;
    pthread_mutex_lock(&state_lock);
    c->ownership.retire();
    pthread_mutex_unlock(&state_lock);
}
static void register_callback(void *encoder,Callback fn,void *data) {
    if (reinterpret_cast<U>(fn)!=base+profile.functions[sky::Callback] || !data || !rd<uint8_t>(reinterpret_cast<U>(data)+8)) {
        original_register(encoder,fn,data);
        return;
    }
    active_until.store(now_ns()+3000000000ULL,std::memory_order_relaxed);
    U r=renderer(), object=rd<U>(reinterpret_cast<U>(data));
    U command=rd<U>(reinterpret_cast<U>(encoder)+0x18);
    uint32_t serial=rd<uint32_t>(r+0x1eec);
    pthread_mutex_lock(&state_lock);
    for (auto &c:contexts) {
        if(!c.ownership.reserve()) continue;
        c.id=next_id.fetch_add(1); c.data=data; c.object=object;
        c.frame=rd<uint32_t>(object+0x140); c.recording=rd<uint8_t>(reinterpret_cast<U>(data)+8);
        c.serial=serial; c.renderer=r; c.command=command;
        c.slot=rd<uint32_t>(r+0x1ef0); c.fence=0; c.device=0;
        c.capture_valid=false; c.frozen_delivery=false; c.consumed=false;
        c.image=0; c.handle=UINT32_MAX; c.width=0; c.height=0; c.format=0; c.bytes=0;
        Capture *capture=find_capture(r,serial,command);
        if(capture) {
            log_capture(*capture);
            c.capture_valid=true; c.image=capture->image; c.handle=capture->handle;
            c.width=capture->width; c.height=capture->height; c.format=capture->format;
        }
        emit("register",c.id,r,c.serial,c.slot,reinterpret_cast<U>(encoder),command,object,c.frame,rd<uint64_t>(object+0x148),reinterpret_cast<U>(data),c.recording);
        if(!c.capture_valid) emit("capture_missing",c.id,r,c.serial,command);
        pthread_mutex_unlock(&state_lock);
        original_register(encoder,callback,&c);
        return;
    }
    pthread_mutex_unlock(&state_lock);
    emit("context_overflow");
    original_register(encoder,fn,data);
}
static void *begin_pass(void *encoder,uint32_t handle,uint64_t flags) {
    U r=renderer();
    uint32_t tex=UINT32_MAX;
    if (r) {
        U p=r+0xc18;
        if (handle<rd<uint32_t>(p+0x60)) {
            U data=rd<U>(p+0x68), mask=rd<U>(p+0x70);
            if (data && mask && (rd<uint32_t>(mask+4*(handle/32)) & (1U<<(handle%32)))) {
                U desc=data+U(handle)*0xd8;
                if (memcmp(reinterpret_cast<void *>(desc),"Capture\0",8)==0) tex=rd<uint32_t>(desc+0x20);
            }
        }
    }
    void *result=original_begin_pass(encoder,handle,flags);
    if (tex!=UINT32_MAX && result) {
        U t=texture(r,tex);
        if(t) {
            pthread_mutex_lock(&state_lock);
            Capture &c=captures[capture_head++%64];
            c={r,reinterpret_cast<U>(encoder),rd<U>(reinterpret_cast<U>(encoder)+0x18),rd<U>(t+8),reinterpret_cast<U>(result),
                rd<uint32_t>(r+0x1eec),rd<uint32_t>(r+0x1ef0),handle,tex,rd<uint32_t>(t+0x64),rd<uint32_t>(t+0x68),rd<uint8_t>(t+0xb0),true,false};
            if(active()) log_capture(c);
            pthread_mutex_unlock(&state_lock);
        }
    }
    return result;
}
static uint64_t signature(const uint8_t *bytes,uint64_t pixels) {
    uint64_t hash=0xcbf29ce484222325ULL;
    for(uint64_t i=0;i<1024;++i) {
        const uint8_t *p=bytes+4*(i*pixels/1024);
        uint32_t word=(uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];
        hash=(hash^uint64_t(int64_t(int32_t(word))))*0x100000001b3ULL;
    }
    return hash;
}
static bool snapshot_failure(Context &c,uint32_t reason) {
    emit("snapshot_failed",c.id,reason,c.renderer,c.serial,c.handle,c.image);
    return false;
}
static bool valid_source(Context &c) {
    U t=texture(c.renderer,c.handle);
    return t&&rd<U>(t+8)==c.image&&rd<uint32_t>(t+0x64)==c.width&&rd<uint32_t>(t+0x68)==c.height&&
        rd<uint8_t>(t+0xb0)==c.format&&rd<uint32_t>(t+0x6c)==1&&rd<uint32_t>(t+0x7c)==1&&(rd<uint8_t>(t+0xb2)&1);
}
static bool snapshot(Context &c) {
    if(!backend_ready||!original_wait||!c.capture_valid||!c.fence||!c.device||c.slot>=3) return snapshot_failure(c,1);
    if(!c.width||!c.height||c.width>8192||c.height>8192||!rgba_format(c.format)) return snapshot_failure(c,2);
    size_t bytes=size_t(c.width)*c.height*4;
    if(bytes>64*1024*1024) return snapshot_failure(c,3);
    if(!valid_source(c)) return snapshot_failure(c,4);
    U slot=c.renderer+U(c.slot)*0x50;
    if(rd<U>(c.renderer+0x1df8)!=c.device||rd<U>(slot+0x1f68)!=c.fence||rd<uint32_t>(slot+0x1fa0)!=c.serial) return snapshot_failure(c,5);
    if(c.capacity<bytes) {
        for(auto &other:contexts) {
            if(allocated_bytes-c.capacity+bytes<=128*1024*1024) break;
            if(&other!=&c&&other.ownership.phase==Phase::Free&&other.buffer) {
                allocated_bytes-=other.capacity;
                free(other.buffer); other.buffer=nullptr; other.capacity=0;
            }
        }
        if(allocated_bytes-c.capacity+bytes>128*1024*1024) return snapshot_failure(c,6);
        auto *buffer=static_cast<uint8_t *>(realloc(c.buffer,bytes));
        if(!buffer) return snapshot_failure(c,6);
        allocated_bytes=allocated_bytes-c.capacity+bytes;
        c.buffer=buffer; c.capacity=bytes;
    }
    VkFence fence=reinterpret_cast<VkFence>(c.fence);
    uint64_t start=now_ns();
    VkResult result=original_wait(reinterpret_cast<VkDevice>(c.device),1,&fence,VK_TRUE,1000000000ULL);
    emit("snapshot_wait",c.id,c.serial,c.fence,result,now_ns()-start);
    if(result!=VK_SUCCESS) return snapshot_failure(c,7);
    if(!valid_source(c)) return snapshot_failure(c,4);
    emit("snapshot_begin",c.id,c.renderer,c.serial,c.handle,c.image,c.fence,bytes,c.slot);
    original_pixels(reinterpret_cast<void *>(c.renderer),c.handle,c.buffer,uint64_t(c.width)*4,0);
    c.bytes=bytes;
    c.signature=tracing?signature(c.buffer,uint64_t(c.width)*c.height):0;
    emit("snapshot_end",c.id,c.renderer,c.serial,c.image,c.bytes,now_ns()-start,c.signature,allocated_bytes);
    return true;
}
static void get_pixels(void *object,uint32_t handle,void *destination,uint64_t pitch,uint64_t flags) {
    U r=reinterpret_cast<U>(object);
    Context *c=current_context;
    uint64_t start=now_ns();
    U t=c?texture(r,handle):0;
    uint32_t w=t?rd<uint32_t>(t+0x64):0, h=t?rd<uint32_t>(t+0x68):0;
    uint8_t format=t?rd<uint8_t>(t+0xb0):0;
    if (c) {
        emit("read",c->id,r,handle,t?rd<U>(t+8):0,pitch,flags,w,h,format,reinterpret_cast<U>(destination));
        sample_fences(r,c->id,0);
    }
    uint64_t copy_start=now_ns();
    bool saved=c&&c->frozen_delivery&&!c->consumed&&r==c->renderer&&destination&&w==c->width&&h==c->height&&format==c->format&&
        (!pitch||pitch==uint64_t(w)*4)&&c->buffer&&c->bytes==size_t(w)*h*4;
    if(saved) {
        memcpy(destination,c->buffer,c->bytes);
        c->consumed=true;
    } else {
        if(c) emit("delivery_fallback",c->id,c->frozen_delivery?9:8,handle,t?rd<U>(t+8):0,w,h);
        original_pixels(object,handle,destination,pitch,flags);
    }
    if(c) emit("delivery_source",c->id,saved,c->handle,c->image,handle,t?rd<U>(t+8):0,saved?c->bytes:0,saved?c->signature:0);
    uint64_t copy_ns=now_ns()-copy_start;
    if (tracing&&c) {
        sample_fences(r,c->id,1);
        bool rgba=rgba_format(format);
        uint64_t hash=0xcbf29ce484222325ULL, pixels=uint64_t(w)*h;
        bool valid=rgba && destination && pixels && w<=8192 && h<=8192 && (!pitch||pitch==uint64_t(w)*4);
        if (valid) {
            hash=signature(static_cast<const uint8_t *>(destination),pixels);
        }
        emit("pixels",c->id,hash,valid,now_ns()-start,copy_ns);
    }
}
static bool contains_command(uint32_t count,const VkSubmitInfo *info,U command) {
    for(uint32_t i=0;i<count;++i) for(uint32_t j=0;j<info[i].commandBufferCount;++j)
        if(reinterpret_cast<U>(info[i].pCommandBuffers[j])==command) return true;
    return false;
}
static VkResult submit(VkQueue q,uint32_t count,const VkSubmitInfo *info,VkFence fence) {
    U r=current_renderer;
    bool log=active()&&r;
    uint32_t serial=r?rd<uint32_t>(r+0x1eec):0, slot=r?rd<uint32_t>(r+0x1ef0):0;
    if(r) {
        pthread_mutex_lock(&state_lock);
        for(auto &c:contexts) if(c.ownership.phase!=Phase::Free&&c.renderer==r) log=true;
        bool capture_known=false;
        for(auto &capture:captures) if(capture.valid&&capture.renderer==r&&capture.serial==serial&&contains_command(count,info,capture.command)) {
            capture_known=true;
            if(log) log_capture(capture);
        }
        for(auto &c:contexts) {
            if(c.ownership.phase!=Phase::Pending||c.renderer!=r) continue;
            bool reuse=!capture_known;
            for(auto &capture:captures) if(capture.valid&&capture.renderer==r&&capture.serial==serial&&capture.image==c.image&&contains_command(count,info,capture.command)) reuse=true;
            if(reuse) {
                emit("protect_reuse",c.id,serial,c.image,c.serial,!capture_known);
                c.ownership.protect([&c](){return snapshot(c);});
            }
        }
        pthread_mutex_unlock(&state_lock);
    }
    if (log) {
        for (uint32_t i=0;i<count;++i) for (uint32_t j=0;j<info[i].commandBufferCount;++j)
            emit("submit_begin",r,serial,slot,reinterpret_cast<U>(q),reinterpret_cast<U>(fence),reinterpret_cast<U>(info[i].pCommandBuffers[j]),i);
    }
    VkResult result=original_submit(q,count,info,fence);
    if (log) emit("submit_end",r,serial,slot,reinterpret_cast<U>(q),reinterpret_cast<U>(fence),result);
    if(r) {
        pthread_mutex_lock(&state_lock);
        for(auto &c:contexts) if(c.ownership.phase==Phase::Queued&&c.renderer==r&&c.serial==serial&&contains_command(count,info,c.command)) {
            if(result==VK_SUCCESS) {
                c.fence=reinterpret_cast<U>(fence); c.device=rd<U>(r+0x1df8); c.slot=slot;
                c.ownership.submitted();
                emit("producer_submit",c.id,c.serial,c.fence,c.slot,c.command,r);
            } else {
                c.ownership.phase=Phase::Failed;
                emit("producer_submit_failed",c.id,result);
            }
        }
        pthread_mutex_unlock(&state_lock);
    }
    return result;
}
static VkResult wait_fences(VkDevice device,uint32_t count,const VkFence *fences,VkBool32 all,uint64_t timeout) {
    U r=current_renderer;
    bool log=active()&&r;
    uint64_t start=now_ns();
    if (log) for(uint32_t i=0;i<count;++i) emit("wait_begin",r,reinterpret_cast<U>(fences[i]),all,timeout);
    VkResult result=original_wait(device,count,fences,all,timeout);
    if (log) for(uint32_t i=0;i<count;++i) emit("wait_end",r,reinterpret_cast<U>(fences[i]),result,now_ns()-start);
    return result;
}
static void complete(void *object) {
    U r=reinterpret_cast<U>(object), previous=current_renderer;
    current_renderer=r;
    uint32_t slot=rd<uint32_t>(r+0x1ef0);
    if (active()&&slot<3) {
        U s=r+U(slot)*0x50;
        emit("completion",r,slot,rd<uint32_t>(s+0x1fa0),rd<U>(s+0x1f68),rd<uint64_t>(s+0x1f98));
    }
    original_complete(object);
    current_renderer=previous;
}
static int memory_protection(U addr) {
    FILE *f=fopen("/proc/self/maps","r");
    if (!f) return -1;
    char line[512], mode[5];
    U lo,hi;
    int result=-1;
    while(fgets(line,sizeof(line),f)) {
        if(sscanf(line,"%" SCNxPTR "-%" SCNxPTR " %4s",&lo,&hi,mode)==3 && lo<=addr && addr<hi) {
            result=(mode[0]=='r'?PROT_READ:0)|(mode[1]=='w'?PROT_WRITE:0)|(mode[2]=='x'?PROT_EXEC:0);
            break;
        }
    }
    fclose(f); return result;
}
static bool replace(U address,U expected,U value) {
    long page_size=sysconf(_SC_PAGESIZE);
    if(page_size<=0 || address%sizeof(U)) return false;
    U page=address&~(U(page_size)-1);
    int prot=memory_protection(address);
    if(prot<0 || (prot&PROT_EXEC) || mprotect(reinterpret_cast<void *>(page),page_size,prot|PROT_WRITE)) return false;
    bool ok=__atomic_compare_exchange_n(reinterpret_cast<U *>(address),&expected,value,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
    if(mprotect(reinterpret_cast<void *>(page),page_size,prot)) __android_log_print(ANDROID_LOG_ERROR,"SkyRecorderFix","Failed restoring page protection");
    return ok;
}
static void end_frame(void *object) {
    U r=reinterpret_cast<U>(object), previous=current_renderer;
    current_renderer=r;
    static bool attempted=false;
    if (!attempted) {
        attempted=true;
        original_submit=reinterpret_cast<PFN_vkQueueSubmit>(rd<U>(base+profile.submit));
        original_wait=reinterpret_cast<PFN_vkWaitForFences>(rd<U>(base+profile.wait));
        bool qs=original_submit&&replace(base+profile.submit,reinterpret_cast<U>(original_submit),reinterpret_cast<U>(submit));
        bool wf=original_wait&&replace(base+profile.wait,reinterpret_cast<U>(original_wait),reinterpret_cast<U>(wait_fences));
        backend_ready=qs&&original_wait;
        VkDevice device=reinterpret_cast<VkDevice>(rd<U>(r+0x1df8));
        if(device_proc&&device) fence_status=reinterpret_cast<PFN_vkGetFenceStatus>(device_proc(device,"vkGetFenceStatus"));
        emit("backend",r,qs,wf,reinterpret_cast<U>(device),reinterpret_cast<U>(fence_status),rd<U>(r));
        __android_log_print(ANDROID_LOG_INFO,"SkyRecorderFix","Vulkan observed; submit=%d wait=%d fence-status=%d",qs,wf,fence_status!=nullptr);
    }
    original_end_frame(object);
    current_renderer=previous;
}

static int find_bootloader(dl_phdr_info *info,size_t,void *) {
    const char *leaf=strrchr(info->dlpi_name,'/');
    leaf=leaf?leaf+1:info->dlpi_name;
    if(strcmp(leaf,"libBootloader.so")) return 0;
    sky::Image image;
    image.bias=info->dlpi_addr;
    for(uint16_t i=0;i<info->dlpi_phnum;++i) {
        const auto &p=info->dlpi_phdr[i];
        if(p.p_type==PT_GNU_EH_FRAME) image.unwind=p.p_vaddr;
        if(p.p_type==PT_LOAD&&(p.p_flags&PF_R)) {
            if(image.count==16) { profile_status="too_many_segments"; return 1; }
            image.spans[image.count++]={p.p_vaddr,reinterpret_cast<const uint8_t *>(info->dlpi_addr+p.p_vaddr),size_t(p.p_memsz),p.p_flags};
        }
        if(p.p_type!=PT_NOTE) continue;
        U at=info->dlpi_addr+p.p_vaddr,end=at+p.p_memsz;
        while(at+sizeof(Elf64_Nhdr)<=end) {
            auto note=rd<Elf64_Nhdr>(at); at+=sizeof(Elf64_Nhdr);
            U name=at,desc=at+((note.n_namesz+3ULL)&~3ULL),next=desc+((note.n_descsz+3ULL)&~3ULL);
            if(next>end||next<at) break;
            if(note.n_type==NT_GNU_BUILD_ID&&note.n_namesz==4&&!memcmp(reinterpret_cast<void *>(name),"GNU",4)) {
                size_t size=note.n_descsz<64?note.n_descsz:64;
                for(size_t j=0;j<size;++j) snprintf(observed_build_id+2*j,3,"%02x",rd<uint8_t>(desc+j));
            }
            at=next;
        }
    }
    uint64_t start=now_ns();
    bool found=sky::locate(image,profile,profile_status);
    decoder_ns=now_ns()-start;
    if(!found) return 1;
    base=info->dlpi_addr;
    profile_status="accepted";
    return 1;
}
extern "C" __attribute__((visibility("default"))) void (*Start())() {
    if(initialized.exchange(true)) return nullptr;
    dl_iterate_phdr(find_bootloader,nullptr);
    if(tracing) {
        for(size_t i=0;i<8192;++i) cells[i].sequence.store(i,std::memory_order_relaxed);
        pthread_t thread;
        if(pthread_create(&thread,nullptr,write_loop,nullptr)) {
            __android_log_print(ANDROID_LOG_ERROR,"SkyRecorderFix","Cannot start trace writer; no hooks installed");
            return nullptr;
        }
        pthread_detach(thread);
        for(int i=0;i<200&&!writer_ready.load(std::memory_order_acquire);++i) usleep(10000);
        if(!writer_ready.load(std::memory_order_acquire)||!writer_ok.load(std::memory_order_acquire)) return nullptr;
    }
    if(!base) {
        emit("profile_rejected",rejected_address);
        __android_log_print(ANDROID_LOG_ERROR,"SkyRecorderFix","Unsupported or modified libBootloader; decoder rejected this code shape. No hooks installed.");
        return nullptr;
    }
    struct Hook { U slot,target,replacement; } hooks[]={
        {profile.slots[sky::Register],profile.functions[sky::Register],reinterpret_cast<U>(register_callback)},
        {profile.slots[sky::Begin],profile.functions[sky::Begin],reinterpret_cast<U>(begin_pass)},
        {profile.slots[sky::Pixels],profile.functions[sky::Pixels],reinterpret_cast<U>(get_pixels)},
        {profile.slots[sky::Complete],profile.functions[sky::Complete],reinterpret_cast<U>(complete)},
        {profile.slots[sky::End],profile.functions[sky::End],reinterpret_cast<U>(end_frame)}
    };
    for(auto &h:hooks) if(rd<U>(base+h.slot)!=base+h.target) {
        emit("dispatch_conflict",h.slot,base+h.target,rd<U>(base+h.slot));
        __android_log_print(ANDROID_LOG_ERROR,"SkyRecorderFix","Dispatch conflict; no hooks installed"); return nullptr;
    }
    original_register=reinterpret_cast<Register>(base+profile.functions[sky::Register]);
    original_begin_pass=reinterpret_cast<BeginPass>(base+profile.functions[sky::Begin]);
    original_pixels=reinterpret_cast<Pixels>(base+profile.functions[sky::Pixels]);
    original_complete=reinterpret_cast<RendererCall>(base+profile.functions[sky::Complete]);
    original_end_frame=reinterpret_cast<RendererCall>(base+profile.functions[sky::End]);
    void *vk=dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL);
    if(vk) device_proc=reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(vk,"vkGetDeviceProcAddr"));
    size_t installed=0;
    for(auto &h:hooks) {
        if(!replace(base+h.slot,base+h.target,h.replacement)) {
            for(size_t i=0;i<installed;++i) replace(base+hooks[i].slot,hooks[i].replacement,base+hooks[i].target);
            emit("install_failed",installed);
            __android_log_print(ANDROID_LOG_ERROR,"SkyRecorderFix","Hook installation failed; rolled back");
            return nullptr;
        }
        ++installed;
    }
    emit("installed",installed);
    __android_log_print(ANDROID_LOG_INFO,"SkyRecorderFix","Recorder fix installed; tracing=%d",tracing);
    return nullptr;
}
