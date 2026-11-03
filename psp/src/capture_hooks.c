#include "capture_hooks.h"

#include <pspkernel.h>
#include <pspsdk.h>

#include "audio.h"
#include "display.h"
#include "systemctrl.h"
#include "usb.h"

static pspcap_module_handler g_previous;
static int g_registered;

void capture_hooks_refresh(void)
{
    display_init();
    display_hook_install(usb_wake);
    audio_hooks_install(usb_wake);
    sceKernelDcacheWritebackInvalidateAll();
}

static int module_started(SceModule *module)
{
    // Preserve the CFW/plugin chain, then patch the imports it just linked.
    // Do not log or wait here: this runs on the module-start path.
    int result = g_previous ? g_previous(module) : 0;
    capture_hooks_refresh();
    return result;
}

void capture_hooks_start(void)
{
    if (!g_registered) {
        g_previous = sctrlHENSetStartModuleHandler(module_started);
        g_registered = 1;
    }
    capture_hooks_refresh();
}

void capture_hooks_stop(void)
{
    if (g_registered) {
        sctrlHENSetStartModuleHandler(g_previous);
        g_registered = 0;
    }
    audio_hooks_remove();
    display_hook_remove();
}
