// Exercise the real PSP capture sources against a small firmware/syscall mock.
// No PSP, USB device, SDK, graphics driver, or audio hardware is used here.
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "capture_hooks.h"
#include "display.h"
#include "systemctrl.h"

#define SET_FRAME 0x289D82FEu
#define GET_FRAME 0xEEDA2E54u
#define WAIT_BLANK 0x984C27E7u
#define RESERVE 0x5EC81C55u
#define PANNED 0x13F592BCu
#define OUTPUT2_RESERVE 0x01562BA3u

static u64 clock_us;
static int interrupts;
static int wakeups;
static int linked = 1;
static int no_audio;
static int no_set;
static u32 missing;
static int getter_error;
static int race_flip;
static int original_calls;
static int previous_calls;
static pspcap_module_handler module_handler;
static display_frame framebuffer = { (void *)0x04000000, 512, 3 };

typedef struct { u32 nid; void *original; void *syscall; } entry;
static entry exports[16];
static int export_count;

u64 sceKernelGetSystemTimeWide(void) { return clock_us; }
void sceKernelDcacheWritebackInvalidateAll(void) {}
int pspSdkDisableInterrupts(void) { return interrupts++; }
void pspSdkEnableInterrupts(int state) { assert(interrupts > 0); interrupts = state; }
u32 pspSdkSetK1(u32 value) { (void)value; return 0; }
void usb_wake(void) { wakeups++; }

static entry *find(u32 nid)
{
    for (int i = 0; i < export_count; i++)
        if (exports[i].nid == nid)
            return &exports[i];
    assert(!"Unknown mock NID");
    return 0;
}

static int original_set(void *address, int stride, int format, int sync)
{
    (void)sync;
    original_calls++;
    framebuffer = (display_frame){address, stride, format};
    return 0;
}

static int original_get(void **address, int *stride, int *format, int sync)
{
    (void)sync;
    *address = framebuffer.addr;
    *stride = framebuffer.stride;
    *format = framebuffer.pixel_format;
    return getter_error;
}

static void flip(void *address)
{
    int (*call)(void *, int, int, int) = (int (*)(void *, int, int, int))find(SET_FRAME)->syscall;
    assert(call(address, 512, 3, 1) == 0);
}

static int original_wait(void)
{
    clock_us += 16667;
    if (race_flip)
        flip((void *)0x04088000);
    return 0;
}

static int original_reserve(int channel, int samples, int format)
{
    (void)samples; (void)format;
    return channel;
}

static int original_config(int channel, int setting)
{
    (void)channel; (void)setting; return 0;
}

static int original_output(int channel, int volume, void *buffer)
{
    (void)channel; (void)volume; (void)buffer;
    original_calls++;
    return 0;
}

static int original_panned(int channel, int left, int right, void *buffer)
{
    (void)channel; (void)left; (void)right; (void)buffer;
    original_calls++;
    return 0;
}

static int original_src_reserve(int samples, int rate, int channels)
{
    (void)samples; (void)rate; (void)channels; return 0;
}

static int original_src_output(int volume, void *buffer)
{
    (void)volume; (void)buffer; return 0;
}

static int original_output2_reserve(int samples) { (void)samples; return 0; }

static void add(u32 nid, void *function)
{
    exports[export_count++] = (entry){nid, function, function};
}

static void setup(void)
{
    add(SET_FRAME, original_set);
    add(GET_FRAME, original_get);
    add(WAIT_BLANK, original_wait);
    add(RESERVE, original_reserve);
    add(0xCB2E439E, original_config);
    add(0x95FD0C2D, original_config);
    add(0x8C1009B2, original_output);
    add(0x136CAF51, original_output);
    add(0xE2D56B2D, original_panned);
    add(PANNED, original_panned);
    add(0x38553111, original_src_reserve);
    add(0xE0727056, original_src_output);
    add(OUTPUT2_RESERVE, original_output2_reserve);
    add(0x63F2889C, original_output2_reserve);
    add(0x2D53F36E, original_src_output);
}

u32 sctrlHENFindFunction(const char *module, const char *library, u32 nid)
{
    (void)library;
    if (nid == missing || (no_set && nid == SET_FRAME) ||
        (no_audio && strcmp(module, "sceAudio_Driver") == 0))
        return 0;
    return (u32)find(nid)->original;
}

void sctrlHENPatchSyscall(void *old, void *replacement)
{
    if (!linked)
        return; // Export exists, but the game has not linked its syscall yet.
    for (int i = 0; i < export_count; i++)
        if (exports[i].syscall == old)
            exports[i].syscall = replacement;
}

pspcap_module_handler sctrlHENSetStartModuleHandler(pspcap_module_handler handler)
{
    pspcap_module_handler previous = module_handler;
    module_handler = handler;
    return previous;
}

static int previous_handler(SceModule *module)
{
    (void)module;
    previous_calls++;
    return 17;
}

static void reserve(int samples)
{
    int (*call)(int, int, int) = (int (*)(int, int, int))find(RESERVE)->syscall;
    assert(call(0, samples, 0) == 0);
}

static void output(void *pcm)
{
    int (*call)(int, int, int, void *) = (int (*)(int, int, int, void *))find(PANNED)->syscall;
    assert(call(0, 0x7000, 0x6000, pcm) == 0);
}

static void test_audio_block(void)
{
    short pcm[64], received[64];
    for (int i = 0; i < 64; i++) pcm[i] = (short)(i * 31 - 1000);
    reserve(32);
    audio_set_capture(1);
    output(pcm);
    pspcap_audio_info info;
    assert(audio_pop(&info, received, sizeof(received)) == sizeof(pcm));
    assert(info.samples == 32 && info.channels == 2 && info.rate == 44100);
    assert(info.vol_left == 0x7000 && info.vol_right == 0x6000);
    assert(memcmp(pcm, received, sizeof(pcm)) == 0);
    assert(audio_pop(&info, received, sizeof(received)) == 0);
}

int main(int argc, char **argv)
{
    assert(sizeof(void *) == 4);
    assert(argc == 2);
    setup();
    const char *scenario = argv[1];
    if (strcmp(scenario, "polling") == 0) {
        no_set = 1;
        assert(display_init() == 0 && display_available());
        assert(display_hook_install(usb_wake) == -1);
        display_set_notify(1);
        assert(display_poll_frame() == 1 && display_polling());
        display_frame frame; u32 id;
        assert(display_latest(&frame, &id) == 0 && id == 1);
        assert(frame.addr == framebuffer.addr);
    } else if (strcmp(scenario, "pacing") == 0) {
        display_init(); display_set_notify(1);
        assert(display_poll_frame() == 1);
        assert(display_poll_frame() == 0);
        clock_us += 33333;
        assert(display_poll_frame() == 1 && display_flip_count() == 2);
    } else if (strcmp(scenario, "deadlines") == 0) {
        display_init();
        assert(display_poll_timeout_us() == 16667);
        display_set_notify(1);
        assert(display_poll_timeout_us() == 1);
        assert(display_poll_frame() == 1); // fake vblank consumes 16.667 ms
        assert(display_poll_timeout_us() == 16666);
        clock_us += 15000; // part of a frame copy/transfer
        assert(display_poll_timeout_us() == 1666);
        clock_us += 1666;
        assert(display_poll_timeout_us() == 1); // deadline is due, no extra sleep
        assert(display_poll_frame() == 1);
        clock_us += 20000; // sending plus vblank exceeded the 33.333 ms interval
        assert(display_poll_timeout_us() == 1);
        display_set_notify(0);
        assert(display_poll_timeout_us() == 16667);
    } else if (strcmp(scenario, "resume") == 0) {
        display_init(); display_hook_install(usb_wake); display_set_notify(1);
        flip(framebuffer.addr);
        assert(display_poll_frame() == 0 && !display_polling());
        clock_us += 250000;
        assert(display_poll_frame() == 1 && display_polling());
        u32 before = display_flip_count();
        flip(framebuffer.addr);
        assert(display_flip_count() == before + 1 && !display_polling());
        assert(display_poll_frame() == 0);
    } else if (strcmp(scenario, "race") == 0) {
        display_init(); display_hook_install(usb_wake); display_set_notify(1);
        race_flip = 1;
        assert(display_poll_frame() == 0);
        display_frame frame; u32 id;
        assert(display_latest(&frame, &id) == 0 && id == 1);
        assert(frame.addr == (void *)0x04088000 && !display_polling());
    } else if (strcmp(scenario, "disabled") == 0) {
        display_init();
        assert(display_poll_frame() == 0);
        display_set_notify(1);
        assert(display_poll_frame() == 1);
        display_set_notify(0);
        clock_us += 1000000;
        assert(display_poll_frame() == 0 && !display_polling());
    } else if (strcmp(scenario, "invalid_getter") == 0) {
        display_init(); display_set_notify(1);
        getter_error = -1;
        assert(display_poll_frame() == 0 && display_flip_count() == 0);
        clock_us += 1000000; getter_error = 0; framebuffer.stride = 1;
        assert(display_poll_frame() == 0 && display_flip_count() == 0);
    } else if (strcmp(scenario, "partial_audio") == 0) {
        missing = OUTPUT2_RESERVE;
        assert(audio_hooks_install(usb_wake) == 11 && audio_hooks_installed());
        test_audio_block();
        audio_hooks_remove();
        assert(!audio_hooks_installed());
        assert(find(PANNED)->syscall == find(PANNED)->original);
    } else if (strcmp(scenario, "audio_retry") == 0) {
        missing = OUTPUT2_RESERVE;
        assert(audio_hooks_install(usb_wake) == 11);
        test_audio_block();
        missing = 0;
        assert(audio_hooks_install(usb_wake) == 12);
        test_audio_block();
    } else if (strcmp(scenario, "audio_overflow") == 0) {
        audio_hooks_install(usb_wake); reserve(INT_MAX); audio_set_capture(1);
        output((void *)1); // Must reject the length before dereferencing this.
        pspcap_audio_info info; short pcm[2];
        assert(audio_dropped() == 1 && audio_pop(&info, pcm, sizeof(pcm)) == 0);
    } else if (strcmp(scenario, "audio_unknown") == 0) {
        audio_hooks_install(usb_wake); audio_set_capture(1);
        output((void *)1);
        assert(audio_dropped() == 1);
    } else if (strcmp(scenario, "audio_ring") == 0) {
        audio_hooks_install(usb_wake);
        for (int i = 0; i < 1500; i++) test_audio_block();
        assert(audio_dropped() == 0);
    } else if (strcmp(scenario, "no_audio") == 0) {
        no_audio = 1;
        assert(audio_hooks_install(usb_wake) == 0 && !audio_hooks_installed());
        audio_hooks_remove();
    } else if (strcmp(scenario, "late_imports") == 0) {
        linked = 0; module_handler = previous_handler;
        capture_hooks_start();
        pspcap_module_handler installed = module_handler;
        assert(installed != previous_handler);
        linked = 1;
        assert(installed(0) == 17 && previous_calls == 1);
        display_set_notify(1); flip(framebuffer.addr);
        assert(display_flip_count() == 1 && wakeups == 1);
        test_audio_block();
        capture_hooks_start();
        assert(module_handler == installed); // No recursive registration.
        capture_hooks_stop();
        assert(module_handler == previous_handler);
        assert(find(SET_FRAME)->syscall == find(SET_FRAME)->original);
        assert(find(PANNED)->syscall == find(PANNED)->original);
    } else {
        assert(!"Unknown scenario");
    }
    assert(interrupts == 0);
    printf("PASS %s\n", scenario);
    return 0;
}
