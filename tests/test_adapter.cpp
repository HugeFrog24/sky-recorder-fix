#include "cache_path.h"
#include <cassert>
#include <cstdio>

static void test_cache_path() {
    char package[128];
    const char *valid[]={
        "/data/data/git.artdeell.skymodloader/files/mods/libsky_native_probe.so",
        "/data/user/0/git.artdeell.skymodloader/files/mods/libsky_native_probe.so",
        "/data/user/10/git.artdeell.skymodloader/files/mods/libsky_native_probe.so",
        "/data/user_de/0/git.artdeell.skymodloader/files/mods/libsky_native_probe.so"
    };
    for (auto path:valid) {
        assert(module_package(path,package,sizeof(package)));
        assert(!strcmp(package,"git.artdeell.skymodloader"));
    }
    assert(module_package("/data/user/0/example.clone/files/mods/probe.so",package,sizeof(package)));
    assert(!strcmp(package,"example.clone"));
    const char *invalid[]={nullptr,"com.tgc.sky.android.sml","/sdcard/Download/probe.so",
        "/data/user/x/a.b/files/mods/probe.so","/data/data/../files/mods/probe.so",
        "/data/data/a..b/files/mods/probe.so","/data/data/a:b/files/mods/probe.so",
        "/data/data/a.b/files/mods/","/data/data/a.b/files/modsX/probe.so",
        "/data/data/a.b/cache/probe.so","/data/data//files/mods/probe.so"};
    for (auto path:invalid) assert(!module_package(path,package,sizeof(package)));
    assert(!module_package(valid[0],package,4));
    assert(!module_package(valid[0],nullptr,128));
    assert(!module_package(valid[0],package,0));
    puts("19 cache-path regression cases passed");
}

#include "ownership.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <random>

struct ModelFrame {
    Ownership owner;
    int image=-1, producer=-1, saved=-1;
};

static void model(unsigned seed) {
    std::mt19937 random(seed);
    std::array<ModelFrame,4> contexts{};
    int images[2]={-1,-1};
    int produced=0,delivered=0,stopped=0,counter=0;
    for(int n=0;n<2000;++n) {
        ModelFrame &current=contexts[n%4];
        assert(current.owner.reserve());
        current.image=n%2; current.producer=n;
        for(auto &old:contexts) if(old.owner.phase==Phase::Pending&&old.image==current.image) {
            assert(old.owner.protect([&](){
                images[old.image]=old.producer;
                old.saved=images[old.image];
                return true;
            }));
        }
        assert(current.owner.submitted());
        ++produced;
        if(random()%2) images[n%2]=n;
        if(n>=2) {
            ModelFrame &old=contexts[(n-2)%4];
            assert(old.owner.claim()==Delivery::Frozen);
            assert(old.saved==old.producer);
            if(random()%23) ++delivered; else ++stopped;
            assert(old.owner.claim()==Delivery::None);
            assert(old.owner.retire());
            assert(!old.owner.retire());
            ++counter;
        }
    }
    for(auto &frame:contexts) if(frame.owner.phase==Phase::Pending) {
        assert(frame.owner.claim()==Delivery::Original);
        ++delivered; ++counter;
        assert(frame.owner.retire());
    }
    assert(produced==delivered+stopped&&counter==produced);
}

static void test_ownership() {
    for(unsigned seed=0;seed<100;++seed) model(seed);
    Ownership state;
    assert(state.claim()==Delivery::None);
    assert(!state.submitted());
    assert(state.reserve());
    assert(!state.reserve());
    int attempts=0;
    assert(!state.protect([&](){++attempts; return true;}));
    assert(attempts==0);
    assert(state.submitted());
    assert(!state.protect([&](){++attempts; return false;}));
    assert(!state.protect([&](){++attempts; return true;}));
    assert(attempts==1);
    assert(state.claim()==Delivery::Original);
    assert(state.retire());
    assert(state.reserve()&&state.submitted());
    assert(state.protect([](){return true;}));
    assert(!state.protect([](){assert(false); return true;}));
    assert(state.claim()==Delivery::Frozen);
    assert(state.retire());
    puts("200000 frame schedules passed: protected image reuse, monotone producer delivery, stop omissions, callback retirement and failed snapshot non-retry");
}

#include <array>
#include <atomic>
#include <cassert>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "ownership.h"
#include "locator.h"
static sky::Profile profile;
#ifndef TEST_TRACE
#define TEST_TRACE 1
#endif
static constexpr bool tracing=TEST_TRACE!=0;

using U=uintptr_t;
using VkDevice=struct DeviceTag *;
using VkQueue=struct QueueTag *;
using VkFence=struct FenceTag *;
using VkCommandBuffer=struct CommandTag *;
using VkResult=int32_t;
using VkBool32=uint32_t;
constexpr VkResult VK_SUCCESS=0;
constexpr VkBool32 VK_TRUE=1;
struct VkSubmitInfo { uint32_t commandBufferCount; const VkCommandBuffer *pCommandBuffers; };
using PFN_vkQueueSubmit=VkResult (*)(VkQueue,uint32_t,const VkSubmitInfo *,VkFence);
using PFN_vkWaitForFences=VkResult (*)(VkDevice,uint32_t,const VkFence *,VkBool32,uint64_t);
using PFN_vkGetFenceStatus=VkResult (*)(VkDevice,VkFence);
using PFN_vkGetDeviceProcAddr=void (*(*)(VkDevice,const char *))();
#define pthread_mutex_t TestMutex
#undef PTHREAD_MUTEX_INITIALIZER
#define PTHREAD_MUTEX_INITIALIZER 0
#define pthread_mutex_lock test_lock
#define pthread_mutex_unlock test_unlock
using TestMutex=int;
static void test_lock(TestMutex *m) { assert(*m==0); *m=1; }
static void test_unlock(TestMutex *m) { assert(*m==1); *m=0; }
template<class T> static T rd(U p) { T v; memcpy(&v,reinterpret_cast<void *>(p),sizeof(v)); return v; }
template<class T> static void wr(U p,T value) { memcpy(reinterpret_cast<void *>(p),&value,sizeof(value)); }
static U base, test_renderer;
static uint64_t tick;
static std::atomic<uint64_t> active_until{0};
static uint64_t now_ns() { return tracing?++tick:0; }
static bool active() { return tracing&&now_ns()<active_until.load(); }
static U renderer() { return test_renderer; }
struct Event { uint64_t time; const char *kind; uint64_t fields[12]; };
static std::vector<Event> events;
static void emit(const char *kind,uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0,uint64_t e=0,uint64_t f=0,
                 uint64_t g=0,uint64_t h=0,uint64_t i=0,uint64_t j=0,uint64_t k=0,uint64_t l=0) {
    if(!tracing) return;
    events.push_back({now_ns(),kind,{a,b,c,d,e,f,g,h,i,j,k,l}});
}
#include "adapter-under-test.inc"

static std::array<uint8_t,0x2200> renderer_bytes;
static std::array<uint8_t,0x200> object_bytes;
static std::array<uint8_t,0x170> textures;
static std::array<uint8_t,0xd8> descriptor;
static std::array<uint8_t,0x40> encoder_bytes;
static uint32_t texture_mask=3, pass_mask=1;
static std::array<std::array<uint32_t,128>,2> gpu_images;
static std::array<uint32_t,128> destination;
static int current_serial,completed;
static bool timeout_wait, stopped, mutate_job, recreate;
static uint32_t recycled_generation;
struct Job { U object; uint8_t recording; uint8_t pad[7]; };
static std::array<Job,3> jobs;
struct Pending { Callback fn; void *data; int producer; };
static std::vector<Pending> pending;
static std::vector<int> delivered;

static void complete_gpu(int serial) {
    while(completed<serial) {
        ++completed;
        gpu_images[completed%2].fill(uint32_t(completed));
    }
}
static VkResult fake_wait(VkDevice,uint32_t,const VkFence *fences,VkBool32,uint64_t) {
    if(timeout_wait) return 2;
    int slot=int(reinterpret_cast<U>(*fences))-100;
    assert(slot>=0&&slot<3);
    complete_gpu(int(rd<uint32_t>(test_renderer+0x1fa0+slot*0x50)));
    return VK_SUCCESS;
}
static VkResult fake_status(VkDevice,VkFence fence) {
    int serial=int(rd<uint32_t>(test_renderer+0x1fa0+(reinterpret_cast<U>(fence)-100)*0x50));
    return serial<=completed?0:1;
}
static void fake_pixels(void *,uint32_t handle,void *dest,uint64_t,uint64_t flags) {
    assert(handle<2&&flags==0);
    memcpy(dest,gpu_images[handle].data(),512);
}
static VkResult fake_submit(VkQueue,uint32_t,const VkSubmitInfo *,VkFence) {
    if(current_serial%3!=0) complete_gpu(current_serial);
    return VK_SUCCESS;
}
static void *fake_pass(void *encoder,uint32_t,uint64_t) { return encoder; }
static void fake_register(void *,Callback cb,void *data) { pending.push_back({cb,data,current_serial}); }
static void fake_native_callback(void *data) {
    auto *job=static_cast<Job *>(data);
    uint64_t counter=rd<uint64_t>(job->object+0x148);
    if(job->recording&&!stopped) {
        get_pixels(reinterpret_cast<void *>(test_renderer),uint32_t(counter%2),destination.data(),0,0);
        delivered.push_back(int(destination[0]));
    }
    wr<uint64_t>(job->object+0x148,counter+1);
}

static void reset() {
    for(auto &c:contexts) { free(c.buffer); c=Context{}; }
    for(auto &c:captures) c=Capture{};
    allocated_bytes=0; capture_head=0; next_id.store(1); state_lock=0;
    events.clear(); pending.clear(); delivered.clear(); tick=0;
    renderer_bytes.fill(0); object_bytes.fill(0); textures.fill(0); descriptor.fill(0); encoder_bytes.fill(0);
    test_renderer=reinterpret_cast<U>(renderer_bytes.data());
    U r=test_renderer;
    wr<uint32_t>(r+0xe90,2); wr<U>(r+0xe98,reinterpret_cast<U>(textures.data())); wr<U>(r+0xea0,reinterpret_cast<U>(&texture_mask));
    wr<uint32_t>(r+0xc18+0x60,1); wr<U>(r+0xc18+0x68,reinterpret_cast<U>(descriptor.data())); wr<U>(r+0xc18+0x70,reinterpret_cast<U>(&pass_mask));
    memcpy(descriptor.data(),"Capture",8);
    wr<U>(r+0x1df8,123);
    for(int i=0;i<2;++i) {
        U t=reinterpret_cast<U>(textures.data())+i*0xb8;
        wr<U>(t+8,1000+i); wr<uint32_t>(t+0x64,16); wr<uint32_t>(t+0x68,8);
        wr<uint32_t>(t+0x6c,1); wr<uint32_t>(t+0x7c,1); wr<uint8_t>(t+0xb0,8); wr<uint8_t>(t+0xb2,1);
        gpu_images[i].fill(UINT32_MAX);
    }
    base=0; profile.functions[sky::Callback]=reinterpret_cast<U>(fake_native_callback);
    original_pixels=fake_pixels; original_wait=fake_wait; original_submit=fake_submit;
    original_register=fake_register; original_begin_pass=fake_pass; fence_status=fake_status;
    backend_ready=true; timeout_wait=false; stopped=false; mutate_job=false; recreate=false;
    current_renderer=test_renderer; current_context=nullptr; current_serial=0; completed=-1; recycled_generation=0;
}
static void frame(int n,bool record=true) {
    current_serial=n;
    U r=test_renderer;
    wr<uint32_t>(r+0x1eec,uint32_t(n)); wr<uint32_t>(r+0x1ef0,uint32_t(n%3));
    wr<uint32_t>(r+0x1fa0+(n%3)*0x50,uint32_t(n)); wr<U>(r+0x1f68+(n%3)*0x50,U(100+n%3));
    wr<uint32_t>(reinterpret_cast<U>(descriptor.data())+0x20,uint32_t(n%2));
    U command=200+n%3;
    wr<U>(reinterpret_cast<U>(encoder_bytes.data())+0x18,command);
    begin_pass(encoder_bytes.data(),0,0);
    U object=reinterpret_cast<U>(object_bytes.data());
    wr<uint32_t>(object+0x140,uint32_t(n));
    jobs[n%3]={object,uint8_t(record),{}};
    register_callback(encoder_bytes.data(),fake_native_callback,&jobs[n%3]);
    if(mutate_job) jobs[n%3].recording=0;
    if(recreate&&n==2) wr<U>(reinterpret_cast<U>(textures.data())+8,2000);
    VkCommandBuffer cb=reinterpret_cast<VkCommandBuffer>(command);
    VkSubmitInfo info{1,&cb};
    submit(reinterpret_cast<VkQueue>(1),1,&info,reinterpret_cast<VkFence>(100+n%3));
    if(n>=2) {
        Pending job=pending[n-2];
        complete_gpu(n-2);
        job.fn(job.data);
    }
}
static size_t count(const char *kind) {
    size_t n=0; for(auto &e:events) if(!strcmp(e.kind,kind)) ++n; return n;
}
int main(int argc,char **argv) {
    test_ownership();
    test_cache_path();
    reset();
    emit("installed",5);
    emit("backend",test_renderer,1,1,123,456,789);
    for(int n=0;n<90;++n) frame(n);
    assert(delivered.size()==88);
    for(size_t i=0;i<delivered.size();++i) assert(delivered[i]==int(i));
    if(tracing) assert(count("snapshot_end")==88&&count("delivery_fallback")==0&&count("capture_missing")==0);
    else assert(events.empty()&&tick==0);
    assert(rd<uint64_t>(reinterpret_cast<U>(object_bytes.data())+0x148)==88);
    if(argc>1) {
        FILE *file=fopen(argv[1],"w");
        assert(file);
        fputs("# Host simulation of production adapter, not a device trace\ntime_ns,tid,event,a,b,c,d,e,f,g,h,i,j,k,l\n",file);
        for(auto &event:events) {
            fprintf(file,"%" PRIu64 ",1,%s",event.time,event.kind);
            for(auto field:event.fields) fprintf(file,",%" PRId64,int64_t(field));
            fputc('\n',file);
        }
        fclose(file);
    }
    reset(); mutate_job=true;
    for(int n=0;n<9;++n) frame(n);
    assert(delivered.size()==7);
    if(tracing) assert(count("snapshot_end")==7);
    for(size_t i=0;i<delivered.size();++i) assert(delivered[i]==int(i));
    reset();
    for(int n=0;n<6;++n) frame(n);
    stopped=true;
    frame(6,false); frame(7,false);
    assert(rd<uint64_t>(reinterpret_cast<U>(object_bytes.data())+0x148)==6);
    if(tracing) assert(count("snapshot_unused")==2);
    stopped=false;
    for(int n=8;n<14;++n) frame(n);
    assert(count("delivery_fallback")==0);
    assert(delivered.back()==11);
    reset(); timeout_wait=true;
    for(int n=0;n<5;++n) frame(n);
    assert(delivered.size()==3);
    if(tracing) assert(count("snapshot_failed")==3&&count("delivery_fallback")==3&&count("snapshot_end")==0);
    reset(); recreate=true;
    for(int n=0;n<5;++n) frame(n);
    assert(delivered.size()==3);
    if(tracing) assert(count("snapshot_failed")>=1&&count("delivery_fallback")>=1);
    reset();
    frame(0); frame(1);
    contexts[0].ownership.protect([](){return snapshot(contexts[0]);});
    wr<uint32_t>(reinterpret_cast<U>(textures.data())+0x64,8);
    pending[0].fn(pending[0].data);
    assert(delivered.size()==1);
    if(tracing) assert(count("delivery_fallback")==1);
    else assert(events.empty()&&tick==0);
    reset();
    puts("Production adapter tests passed: 88 ordered captures, stable job snapshot, stop/restart, wait failure, image replacement, dimension mismatch and original callback counters");
}
