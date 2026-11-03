#include "display.h"

#include <pspsdk.h>
#include <pspkernel.h>

#include "systemctrl.h"

// User exports of sceDisplay_Service; their NIDs are not randomized.
#define NID_sceDisplaySetFrameBuf      0x289D82FE
#define NID_sceDisplayGetFrameBuf      0xEEDA2E54
#define NID_sceDisplayWaitVblankStart  0x984C27E7

#define PSP_DISPLAY_SETBUF_NEXTFRAME 1

typedef int (*set_frame_buf_fn)(void *topaddr, int bufferwidth, int pixelformat, int sync);
typedef int (*get_frame_buf_fn)(void **topaddr, int *bufferwidth, int *pixelformat, int sync);
typedef int (*wait_vblank_fn)(void);

static get_frame_buf_fn g_get_frame_buf;
static wait_vblank_fn g_wait_vblank;

// Hook state. The latest frame is written by the game's thread and read by
// ours; both sides do it with interrupts off (single core: that is atomic).
static set_frame_buf_fn g_set_frame_buf; // the original
static void (*g_on_flip)(void);
static volatile int g_notify;
static display_frame g_latest;
static u32 g_flips;
static u64 g_last_hook_us;
static u64 g_last_poll_us;
static int g_saw_hook;
static int g_saw_poll;
static int g_polling;

#define HOOK_STALL_US 250000u
#ifndef PSPCAP_POLL_FPS
#define PSPCAP_POLL_FPS 30
#endif
#if PSPCAP_POLL_FPS < 1 || PSPCAP_POLL_FPS > 30
#error "PSPCAP_POLL_FPS must be between 1 and 30"
#endif
#define POLL_INTERVAL_US (1000000u / PSPCAP_POLL_FPS)
#define MAX_SERVICE_WAIT_US 16667u

int display_init(void)
{
    g_get_frame_buf = (get_frame_buf_fn)sctrlHENFindFunction(
        "sceDisplay_Service", "sceDisplay", NID_sceDisplayGetFrameBuf);
    g_wait_vblank = (wait_vblank_fn)sctrlHENFindFunction(
        "sceDisplay_Service", "sceDisplay", NID_sceDisplayWaitVblankStart);
    return g_get_frame_buf && g_wait_vblank ? 0 : -1;
}

int display_available(void)
{
    return g_get_frame_buf && g_wait_vblank;
}

int display_get_frame(display_frame *frame)
{
    if (!g_get_frame_buf)
        return -1;

    void *addr = 0;
    int stride = 0, format = 0;
    // NEXTFRAME: the buffer the game handed in last, i.e. its newest finished frame.
    int ret = g_get_frame_buf(&addr, &stride, &format, PSP_DISPLAY_SETBUF_NEXTFRAME);
    if (ret < 0)
        return ret;

    frame->addr = addr;
    frame->stride = stride;
    frame->pixel_format = format;
    return 0;
}

void display_wait_vblank(void)
{
    if (g_wait_vblank)
        g_wait_vblank();
}

// Runs on the game's thread inside its syscall. The original is called with
// the game's k1, so it still validates the game's arguments as user pointers.
static int hook_set_frame_buf(void *topaddr, int bufferwidth, int pixelformat, int sync)
{
    int ret = g_set_frame_buf(topaddr, bufferwidth, pixelformat, sync);
    if (ret < 0 || !topaddr)
        return ret;

    int intr = pspSdkDisableInterrupts();
    g_latest.addr = topaddr;
    g_latest.stride = bufferwidth;
    g_latest.pixel_format = pixelformat;
    g_flips++;
    g_last_hook_us = sceKernelGetSystemTimeWide();
    g_saw_hook = 1;
    g_polling = 0;
    pspSdkEnableInterrupts(intr);

    if (g_notify && g_on_flip) {
        // Our kernel objects reject callers marked as user mode by k1:
        // act as kernel for the callback, then restore the game's k1.
        u32 k1 = pspSdkSetK1(0);
        g_on_flip();
        pspSdkSetK1(k1);
    }
    return ret;
}

int display_hook_install(void (*on_flip)(void))
{
    u32 addr = sctrlHENFindFunction("sceDisplay_Service", "sceDisplay", NID_sceDisplaySetFrameBuf);
    if (!addr)
        return -1;
    g_set_frame_buf = (set_frame_buf_fn)addr;
    g_on_flip = on_flip;
    sctrlHENPatchSyscall((void *)addr, hook_set_frame_buf);
    return 0;
}

// A resolved export does not prove that a game's syscall reaches our hook.
// Some loaders link the game's imports after plugins have already started.
// Poll the working framebuffer getter if no hook has run recently. This also
// gives static screens an initial picture instead of an indefinitely black one.
int display_poll_frame(void)
{
    if (!g_notify || !display_available())
        return 0;
    u64 now = sceKernelGetSystemTimeWide();
    int intr = pspSdkDisableInterrupts();
    int recent = g_saw_hook && now - g_last_hook_us < HOOK_STALL_US;
    u32 observed = g_flips;
    pspSdkEnableInterrupts(intr);
    if (recent || (g_saw_poll && now - g_last_poll_us < POLL_INTERVAL_US))
        return 0;
    g_last_poll_us = now;
    g_saw_poll = 1;
    display_wait_vblank();
    display_frame frame;
    if (display_get_frame(&frame) < 0 || !frame.addr || frame.stride < 480 ||
        frame.stride > 2048 || frame.pixel_format < 0 || frame.pixel_format > 3)
        return 0;
    intr = pspSdkDisableInterrupts();
    // A real flip can arrive while waiting for vblank: prefer its newer frame.
    if (g_flips != observed) {
        pspSdkEnableInterrupts(intr);
        return 0;
    }
    g_latest = frame;
    g_flips++; // Keep frame IDs monotonic when real hooks resume.
    g_polling = 1;
    pspSdkEnableInterrupts(intr);
    return 1;
}

int display_polling(void)
{
    return g_polling;
}

u32 display_poll_timeout_us(void)
{
    if (!g_notify || !display_available())
        return MAX_SERVICE_WAIT_US;
    u64 now = sceKernelGetSystemTimeWide();
    int intr = pspSdkDisableInterrupts();
    int recent = g_saw_hook && now - g_last_hook_us < HOOK_STALL_US;
    pspSdkEnableInterrupts(intr);
    if (recent)
        return MAX_SERVICE_WAIT_US;
    if (!g_saw_poll)
        return 1;
    // Copying and USB transfers have already consumed part (or all) of the
    // interval. Do not add another fixed wait after every completed frame.
    u64 elapsed = now - g_last_poll_us;
    if (elapsed >= POLL_INTERVAL_US)
        return 1;
    u32 remaining = (u32)(POLL_INTERVAL_US - elapsed);
    return remaining < MAX_SERVICE_WAIT_US ? remaining : MAX_SERVICE_WAIT_US;
}

void display_hook_remove(void)
{
    if (g_set_frame_buf)
        sctrlHENPatchSyscall(hook_set_frame_buf, g_set_frame_buf);
    g_set_frame_buf = 0;
}

int display_hook_installed(void)
{
    return g_set_frame_buf != 0;
}

void display_set_notify(int on)
{
    g_notify = on;
    if (!on) {
        g_saw_poll = 0;
        g_polling = 0;
    }
}

u32 display_flip_count(void)
{
    return g_flips;
}

int display_latest(display_frame *frame, u32 *flip_id)
{
    int intr = pspSdkDisableInterrupts();
    *frame = g_latest;
    *flip_id = g_flips;
    pspSdkEnableInterrupts(intr);
    return *flip_id ? 0 : -1;
}
