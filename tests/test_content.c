#include "runtime.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir(p,0755)
#endif
/* runtime_file.c's guest errno and export lookup, which the full runtime provides. */
static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t n,const char *name) {
    for (size_t i=0;i<n;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
typedef int32_t (ABI *Module)(uint16_t);
typedef int32_t (ABI *Init)(const void *,void *);
typedef int32_t (ABI *Param)(uint32_t,int32_t *);
typedef int32_t (ABI *List)(uint32_t,void *,uint32_t,uint32_t *);
typedef struct { char label[17]; char padding[3]; } Label;
typedef struct { char data[16]; } Point;
typedef int32_t (ABI *Mount)(uint32_t,const Label *,Point *);
typedef int32_t (ABI *Unmount)(const Point *);
#define GET(t,n) ((t)runtime_content_resolve(n))
int main(int argc,char **argv) {
    Module load=GET(Module,"g8cM39EUZ6o#libSceSysmodule");
    Module loaded=GET(Module,"fMP5NHUOaMk#libSceSysmodule");
    Module unload=GET(Module,"eR2bZFAAU0Q#libSceSysmodule");
    Init init=GET(Init,"R9lA82OraNs#libSceAppContent");
    Param param=GET(Param,"99b82IKXpH4#libSceAppContent");
    List list=GET(List,"xnd8BJzAxmk#libSceAppContent");
    assert(load && loaded && unload && init && param && list);
    assert(!runtime_content_resolve("g8cM39EUZ6o#libkernel"));
    if (argc>1 && !strcmp(argv[1],"--missing")) { load(0xb4); return 99; }
    const uint32_t profile[]={1,13,0x80000000,0,7};
    runtime_content_configure(profile);
    if (argc>1 && !strcmp(argv[1],"--unknown")) {
        /* Other system modules are host-provided: reference counted, independent. */
        assert((uint32_t)loaded(0xb5)==0x805a1001);
        assert(load(0xb5)==0 && loaded(0xb5)==0 && (uint32_t)loaded(0xb4)==0x805a1001);
        assert(unload(0xb5)==0 && (uint32_t)loaded(0xb5)==0x805a1001);
        puts("PASS: independent module references"); return 0;
    }
    if (argc>1 && !strcmp(argv[1],"--mounts")) {
        /* A game that keeps all its DLC mounted: 59 add-ons at once beside the system mounts. */
        Mount mount=GET(Mount,"VANhIWcqYak#libSceAppContent");
        Unmount unmount=GET(Unmount,"3rHWaV-1KC4#libSceAppContent");
        assert(mount && unmount);
        char root[64]; snprintf(root,sizeof(root),"out/content-mounts-%d",(int)getpid());
        char path[256],list_path[256];
        make_dir(root);
        snprintf(path,sizeof(path),"%s/game",root); make_dir(path);
        snprintf(list_path,sizeof(list_path),"%s/user",root);
        runtime_file_configure(path,list_path);
        assert(!runtime_file_mount("/savedata0",list_path));
        enum { N=59 };
        snprintf(list_path,sizeof(list_path),"%s/addcont.txt",root);
        FILE *f=fopen(list_path,"w"); assert(f);
        for (int i=0;i<N;++i) {
            snprintf(path,sizeof(path),"%s/ADDCONTDLCD%05d",root,i); make_dir(path);
            fprintf(f,"ADDCONTDLCD%05d\t%s\n",i,path);
        }
        fclose(f);
        runtime_content_load_addons(list_path);
        unsigned char initial[32]={0},boot[40];
        assert(load(0xb4)==0 && init(initial,boot)==0);
        Label labels[N]; Point points[N];
        for (int i=0;i<N;++i) {
            memset(&labels[i],0,sizeof(labels[i]));
            snprintf(labels[i].label,sizeof(labels[i].label),"ADDCONTDLCD%05d",i);
            assert(mount(0,&labels[i],&points[i])==0);
            snprintf(path,sizeof(path),"/addcont%d",i);
            assert(!strcmp(points[i].data,path));
        }
        /* An unmounted add-on frees its number for the next mount. */
        assert(unmount(&points[7])==0);
        assert(mount(0,&labels[7],&points[7])==0 && !strcmp(points[7].data,"/addcont7"));
        /* Mounting and unmounting over and over never runs out. */
        for (int round=0;round<300;++round) {
            assert(unmount(&points[N-1])==0);
            assert(mount(0,&labels[N-1],&points[N-1])==0);
        }
        snprintf(path,sizeof(path),"/addcont%d",N-1);
        assert(!strcmp(points[N-1].data,path));
        puts("PASS: 59 add-ons mounted at once, numbers reused after unmount");
        return 0;
    }
    assert((uint32_t)loaded(0)==0x805a1000);
    assert((uint32_t)loaded(0xb4)==0x805a1001);
    assert((uint32_t)unload(0xb4)==0x805a1001);
    assert(load(0xb4)==0 && load(0xb4)==0 && loaded(0xb4)==0);
    unsigned char initial[32]={0};
    struct { unsigned char boot[40]; uint32_t canary; } b;
    memset(&b,0xa5,sizeof(b));
    assert((uint32_t)init(NULL,b.boot)==0x80d90002);
    initial[31]=1; assert((uint32_t)init(initial,b.boot)==0x80d90002); initial[31]=0;
    assert(b.boot[0]==0xa5 && init(initial,b.boot)==0);
    for (unsigned i=0;i<40;++i) assert(b.boot[i]==0);
    assert(b.canary==0xa5a5a5a5);
    b.boot[0]=0xcc; assert((uint32_t)init(initial,b.boot)==0x80d90003 && b.boot[0]==0xcc);
    struct { int32_t value; uint32_t canary; } result={-1,0x1234};
    for (unsigned i=0;i<5;++i) {
        assert(param(i,&result.value)==0 && (uint32_t)result.value==profile[i]);
        assert(result.canary==0x1234);
    }
    result.value=77;
    assert((uint32_t)param(5,&result.value)==0x80d90002 && result.value==77);
    assert((uint32_t)param(0,NULL)==0x80d90002);
    unsigned char entries[48]; memset(entries,0xaa,sizeof(entries));
    struct { uint32_t hits,canary; } h={99,0xabcdef};
    assert(list(0,NULL,0,&h.hits)==0 && h.hits==0 && h.canary==0xabcdef);
    assert(list(0,entries,2,&h.hits)==0 && h.hits==0);
    for (unsigned i=0;i<sizeof(entries);++i) assert(entries[i]==0xaa);
    assert((uint32_t)list(0,NULL,0,NULL)==0x80d90002);
    assert(unload(0xb4)==0 && loaded(0xb4)==0);
    assert((uint32_t)init(initial,b.boot)==0x80d90003);
    assert(unload(0xb4)==0 && (uint32_t)loaded(0xb4)==0x805a1001);
    assert(load(0xb4)==0 && init(initial,b.boot)==0 && unload(0xb4)==0);
    puts("PASS: AppContent lifecycle, profile and output boundaries");
    return 0;
}
