/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef P4DGPU_H
#define P4DGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} P4dGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void p4dgpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int p4dgpu_init(const P4dGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t p4dgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int p4dgpu_handle_fault(void *ucontext, void *address);
/* P4D_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void p4dgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int p4dgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int p4dgpu_text_input_poll(char *out_utf8, uint64_t size);
/* A list for the player to choose from, drawn over the game (save list). images: a PNG path per
 * item drawn beside it (NULL array or entry: none). begin: 0 when no window exists.
 * poll: -1 while open, -2 cancelled, else the chosen index. */
int p4dgpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus);
int p4dgpu_choice_poll(void);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int p4dgpu_overlay_captures_input(void);
/* Controller vibration strength set in the menu, 0 (off) to 100 (the game's own). */
int p4dgpu_vibration_percent(void);
/* R3 + L1 + R1 (pad) or F9: the next engine frame rate (60, 100, 120, 240), applied live. */
void p4dgpu_engine_fps_cycle(void);
/* The video decoder handed the game a picture: a cutscene is on screen (the presenter draws
 * black bars outside its 16:9 picture when the render resolution is not 16:9). */
void p4dgpu_movie_picture(void);
/* The pause key (touchpad right) was pressed: it lets go of the mouse lock, or takes it back. */
void p4dgpu_mouse_pause_key(void);
/* A trophy was unlocked (src/runtime_services.c): the overlay shows its notice with the icon
 * TROP<id>.PNG from P4D_TROPHY_ICONS. grade: 'p' platinum, 'g' gold, 's' silver, 'b' bronze. */
void p4dgpu_trophy_unlocked(int id, char grade, const char *name);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned p4dgpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif
