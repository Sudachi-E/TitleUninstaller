#include "Gfx.hpp"
#include "Input.hpp"
#include "TitleUninstaller.hpp"
#include <SDL.h>
#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_udp.h>
#include <vpad/input.h>
#include <padscore/kpad.h>
#include <coreinit/title.h>
#include <coreinit/debug.h>
#include <sysapp/launch.h>
#include <mocha/mocha.h>
#include <nsysnet/netconfig.h>
#include <sndcore2/core.h>

#define LOG(fmt, ...) do { \
    OSReport("[UNINSTALLER] " fmt "\n", ##__VA_ARGS__); \
    WHBLogPrintf("[UNINSTALLER] " fmt, ##__VA_ARGS__); \
} while(0)

int main(int argc, char const* argv[]) {
    WHBProcInit();
    WHBLogUdpInit();

    LOG("=== STARTUP ===");

    AXInit();
    AXQuit();

    VPADInit();
    KPADInit();
    LOG("VPAD/KPAD init done");

    netconf_init();
    LOG("netconf_init done");

    MochaUtilsStatus mochaResult = Mocha_InitLibrary();
    LOG("Mocha_InitLibrary result=%d", (int)mochaResult);
    if (mochaResult != MOCHA_RESULT_SUCCESS) {
        LOG("ERROR: Mocha init failed");
        WHBProcShutdown();
        WHBLogUdpDeinit();
        return -1;
    }

    // Mount USB and NAND storage to get title directories and metadata
    Mocha_MountFS("storage_usb01", nullptr, "/vol/storage_usb01");
    Mocha_MountFS("storage_mlc01", nullptr, "/vol/storage_mlc01");

    if (!Gfx::Init()) {
        LOG("ERROR: Gfx::Init failed");
        Mocha_DeInitLibrary();
        WHBProcShutdown();
        WHBLogUdpDeinit();
        return -1;
    }
    LOG("Gfx::Init done");

    {
        Input input;
        TitleUninstaller app;
        LOG("Entering main loop");

        while (true) {
            if (!WHBProcIsRunning()) break;

            SDL_PumpEvents();
            input.Update();
            app.Update(input);

            if (!WHBProcIsRunning()) break;

            app.Draw();

            if (!WHBProcIsRunning()) break;

            Gfx::Render();
        }

        LOG("Main loop exited — ProcUI signalled exit");
        app.StopIconThread();
    }

    LOG("Calling Gfx::Shutdown");
    Gfx::Shutdown();
    LOG("Gfx::Shutdown done");

    Mocha_UnmountFS("storage_usb01");
    Mocha_UnmountFS("storage_mlc01");
    Mocha_DeInitLibrary();
    KPADShutdown();
    netconf_close();
    VPADShutdown();

    LOG("Calling WHBProcShutdown");
    WHBProcShutdown();
    LOG("Done");
    WHBLogUdpDeinit();
    return 0;
}
