/* Offline AppContent provider for the configured base-game profile.
 * Add-ons (DLC) are folders already extracted from their packages (ADDCONT<label>/ with
 * sce_sys/param.sfo), listed by content_profile.py (--addcont-list); they are reported as
 * installed and mounted read-only on request. No downloads, entitlement or license emulation. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define INVALID_ID ((int32_t)0x805a1000)
#define NOT_LOADED ((int32_t)0x805a1001)
#define PARAMETER ((int32_t)0x80d90002)
#define BUSY ((int32_t)0x80d90003)
#define NOT_MOUNTED ((int32_t)0x80d90004)
#define NOT_FOUND ((int32_t)0x80d90005)
#define MOUNT_FULL ((int32_t)0x80d90006)
#define MAX_ADDONS 128
#define ADDON_INSTALLED 4 /* SCE_APP_CONTENT_ADDCONT_DOWNLOAD_STATUS_INSTALLED */
/* SceNpUnifiedEntitlementLabel (17 bytes + 3 padding), SceAppContentMountPoint (16 bytes). */
typedef struct { char label[17]; char padding[3]; } EntitlementLabel;
typedef struct { EntitlementLabel label; uint32_t status; } AddcontInfo;
typedef struct { char data[16]; } MountPoint;
static struct { char label[17]; char host[512]; char mount[16]; } addons[MAX_ADDONS];
static unsigned addon_count, addon_mounts;
static unsigned refs, initialized, configured;
static uint32_t parameters[5];
static unsigned queries, lists;
void runtime_content_configure(const uint32_t values[5]) {
    if (refs || initialized || (values[0]!=1 && values[0]!=3)) {
        fputs("ERROR: invalid or late content profile\n",stderr); exit(1);
    }
    memcpy(parameters,values,sizeof(parameters)); configured=1;
}
/* Lines "LABEL<tab>host folder" (content_profile.py). */
void runtime_content_load_addons(const char *path) {
    FILE *f=fopen(path,"r");
    if (!f) { fprintf(stderr,"ERROR: cannot read add-on list %s\n",path); exit(1); }
    char line[700];
    while (fgets(line,sizeof(line),f) && addon_count<MAX_ADDONS) {
        line[strcspn(line,"\r\n")]=0;
        char *tab=strchr(line,'\t');
        if (!tab || tab==line || tab-line>16 || !tab[1]) continue;
        *tab=0;
        snprintf(addons[addon_count].label,sizeof(addons[addon_count].label),"%s",line);
        snprintf(addons[addon_count].host,sizeof(addons[addon_count].host),"%s",tab+1);
        ++addon_count;
    }
    fclose(f);
    printf("AppContent: %u add-ons available\n",addon_count);
}
static int find_addon(const EntitlementLabel *label) {
    for (unsigned i=0;i<addon_count;++i)
        if (!strncmp(addons[i].label,label->label,16)) return (int)i;
    return -1;
}
static void require_provider(void) {
    if (!configured) { fputs("STOP: AppContent needs --content-profile\n",stderr); exit(21); }
}
/* System modules: every library this eboot imports is provided by the host
 * runtime, so loading one only tracks a reference count. AppContent (0xb4)
 * additionally requires the configured content profile. */
static unsigned module_refs[0x10000];
static ABI int32_t module_load(uint16_t id) {
    if (!id) return INVALID_ID;
    if (id==0xb4) require_provider();
    if (module_refs[id]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); exit(21); }
    ++module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; printf("Runtime: AppContent host provider loaded; references=%u\n",refs); }
    else printf("Runtime: system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
static ABI int32_t module_loaded(uint16_t id) {
    if (!id) return INVALID_ID;
    return module_refs[id] ? 0 : NOT_LOADED;
}
static ABI int32_t module_unload(uint16_t id) {
    int32_t result=module_loaded(id);
    if (result) return result;
    --module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; if (!refs) initialized=0; }
    return 0;
}
/* Internal loads (system modules such as Fios2 load their own dependencies) use the same
 * host implementations; the module's start result is 0. */
static ABI int32_t module_load_internal_with_arg(uint32_t id,int32_t argc,const void *argv,uint64_t flags,int32_t *result) {
    (void)argc; (void)argv; (void)flags;
    if (id>0xffff) { fprintf(stderr,"STOP: internal system module id 0x%x is not supported\n",id); exit(21); }
    int32_t r=module_load((uint16_t)id);
    if (!r && result) *result=0;
    return r;
}
static void require_loaded(void) {
    require_provider();
    if (!refs) { fputs("STOP: AppContent called before module load\n",stderr); exit(21); }
}
static ABI int32_t content_init(const unsigned char *init,unsigned char *boot) {
    require_loaded();
    if (!init || !boot) return PARAMETER;
    if (initialized) return BUSY;
    /* Only the reserved-zero init profile is implemented. */
    for (unsigned i=0;i<32;++i) if (init[i]) return PARAMETER;
    memset(boot,0,40); initialized=1;
    printf("Runtime: AppContent initialized; offline profile, %u add-ons installed\n",addon_count);
    return 0;
}
static void require_initialized(void) {
    require_loaded();
    if (!initialized) { fputs("STOP: AppContent query before initialization\n",stderr); exit(21); }
}
static ABI int32_t param_int(uint32_t id,int32_t *out) {
    require_initialized();
    if (id>4 || !out) return PARAMETER;
    memcpy(out,&parameters[id],4); ++queries;
    printf("Runtime: AppContent parameter %u = %d\n",id,*out);
    return 0;
}
static ABI int32_t addon_list(uint32_t service,void *list,uint32_t capacity,uint32_t *hits) {
    require_initialized();
    if (service) { fputs("STOP: nonzero AppContent service label unsupported\n",stderr); exit(21); }
    if ((!capacity || !list) && !hits) return PARAMETER;
    /* Every listed add-on is installed. Never touch unused list slots. */
    if (list && capacity) {
        AddcontInfo *out=list;
        unsigned n=capacity<addon_count ? capacity : addon_count;
        for (unsigned i=0;i<n;++i) {
            memset(&out[i],0,sizeof(out[i]));
            memcpy(out[i].label.label,addons[i].label,16);
            out[i].status=ADDON_INSTALLED;
        }
        if (hits) *hits=n;
    } else if (hits) *hits=addon_count;
    ++lists; return 0;
}
static ABI int32_t addon_mount(uint32_t service,const EntitlementLabel *label,MountPoint *point) {
    require_initialized();
    if (!label || !point) return PARAMETER;
    if (service) { fputs("STOP: nonzero AppContent service label unsupported\n",stderr); exit(21); }
    int i=find_addon(label);
    if (i<0) return NOT_FOUND;
    if (!addons[i].mount[0]) {
        /* /addcont0, /addcont1, ... in mount order, as the system assigns them */
        if (addon_mounts>=64) return MOUNT_FULL;
        snprintf(addons[i].mount,sizeof(addons[i].mount),"/addcont%u",addon_mounts);
        if (runtime_file_mount(addons[i].mount,addons[i].host)) { addons[i].mount[0]=0; return MOUNT_FULL; }
        ++addon_mounts;
        printf("AppContent: add-on %s mounted at %s\n",addons[i].label,addons[i].mount);
    }
    memset(point,0,sizeof(*point));
    memcpy(point->data,addons[i].mount,strlen(addons[i].mount));
    return 0;
}
static ABI int32_t addon_unmount(const MountPoint *point) {
    require_initialized();
    if (!point) return PARAMETER;
    for (unsigned i=0;i<addon_count;++i)
        if (addons[i].mount[0] && !strncmp(addons[i].mount,point->data,sizeof(point->data))) {
            runtime_file_unmount(addons[i].mount);
            addons[i].mount[0]=0;
            return 0;
        }
    return NOT_MOUNTED;
}
uintptr_t runtime_content_resolve(const char *name) {
    if (!strcmp(name,"VANhIWcqYak#libSceAppContent")) return (uintptr_t)addon_mount;
    if (!strcmp(name,"3rHWaV-1KC4#libSceAppContent")) return (uintptr_t)addon_unmount;
    if (!strcmp(name,"g8cM39EUZ6o#libSceSysmodule")) return (uintptr_t)module_load;
    if (!strcmp(name,"fMP5NHUOaMk#libSceSysmodule")) return (uintptr_t)module_loaded;
    if (!strcmp(name,"eR2bZFAAU0Q#libSceSysmodule")) return (uintptr_t)module_unload;
    if (!strcmp(name,"hHrGoGoNf+s#libSceSysmodule")) return (uintptr_t)module_load_internal_with_arg;
    if (!strcmp(name,"R9lA82OraNs#libSceAppContent")) return (uintptr_t)content_init;
    if (!strcmp(name,"99b82IKXpH4#libSceAppContent")) return (uintptr_t)param_int;
    if (!strcmp(name,"xnd8BJzAxmk#libSceAppContent")) return (uintptr_t)addon_list;
    return 0;
}
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u\n",refs,initialized,queries,lists);
}
