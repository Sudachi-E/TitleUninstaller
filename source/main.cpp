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
#include <coreinit/filesystem.h>
#include <coreinit/filesystem_fsa.h>
#include <sysapp/launch.h>
#include <mocha/mocha.h>
#include <nsysnet/netconfig.h>
#include <sndcore2/core.h>

static FSAClientHandle sInstallFsaClient = 0;

static void SetupSdBindMount() {
    if (FSAInit() != FS_ERROR_OK) {
        LOG("FSAInit failed — SD installs unavailable");
        return;
    }
    sInstallFsaClient = FSAAddClient(nullptr);
    if (sInstallFsaClient == 0) {
        LOG("FSAAddClient failed — SD installs unavailable");
        return;
    }
    if (Mocha_UnlockFSClientEx(sInstallFsaClient) != MOCHA_RESULT_SUCCESS) {
        LOG("Mocha_UnlockFSClientEx failed — SD installs unavailable");
        return;
    }
    FSError e = FSAMount(sInstallFsaClient, "/vol/external01", "/vol/app_sd",
                         FSA_MOUNT_FLAG_BIND_MOUNT, nullptr, 0);
    LOG("FSAMount /vol/external01 -> /vol/app_sd result=%d", (int)e);
}

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

    SetupSdBindMount();

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
    }

    LOG("Calling Gfx::Shutdown");
    Gfx::Shutdown();
    LOG("Gfx::Shutdown done");

    Mocha_UnmountFS("storage_usb01");
    Mocha_UnmountFS("storage_mlc01");
    if (sInstallFsaClient != 0) {
        FSAUnmount(sInstallFsaClient, "/vol/app_sd", FSA_UNMOUNT_FLAG_BIND_MOUNT);
        FSADelClient(sInstallFsaClient);
        FSAShutdown();
        sInstallFsaClient = 0;
    }
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
