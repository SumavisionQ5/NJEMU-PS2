#include <dirent.h>
#include <stdio.h>
#include <stdarg.h>
#include "emumain.h"

#include <kernel.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <ps2_filesystem_driver.h>
#include <ps2_usb_driver.h>
#include <ps2_mx4sio_driver.h>
#include <ps2_audio_driver.h>

/* BOOT LOG (diagnostic). Writes to CWD (USB root) first, then device paths. */
void boot_log(const char *msg)
{
	/* DEBUG LOG DISABLED (v16 cleanup): no njemu_boot.txt output. */
	(void)msg;
}

typedef struct ps2_platform {
} ps2_platform_t;

static void reset_IOP()
{
    SifInitRpc(0);
    while (!SifIopReset(NULL, 0)) {}
    while (!SifIopSync()) {}
}

static void prepare_IOP()
{
    reset_IOP();
    SifInitRpc(0);
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    sbv_patch_fileio();
}

static void init_drivers()
{
	init_only_boot_ps2_filesystem_driver();
	/* FIX: explicitly mount USB + MX4SIO so mass:/ works even when the
	 * loader CWD makes getBootDeviceID() mis-detect the boot device
	 * (this left NO filesystem mounted -> roms/ unreadable + no logs).
	 * IRX modules are embedded in the library, no filesystem needed. */
	init_usb_driver(true);
	init_mx4sio_driver(true);
	init_audio_driver();
}

static void deinit_drivers()
{
	deinit_audio_driver();
	deinit_only_boot_ps2_filesystem_driver();
}

static void *ps2_init(void) {
	ps2_platform_t *ps2 = (ps2_platform_t*)calloc(1, sizeof(ps2_platform_t));

    prepare_IOP();
	boot_log("[S0] after prepare_IOP");
    init_drivers();
	boot_log("[S1] after init_drivers");

#if 0 /* DEBUG LOG DISABLED (v16 cleanup): no njemu_diag.txt output */
    {
        char cwd[PATH_MAX];
        FILE *f = fopen("njemu_diag.txt", "w");
        if (!f) f = fopen("mass:/njemu_diag.txt", "w");
        if (!f) f = fopen("mass0:/njemu_diag.txt", "w");
        if (f) {
            if (getcwd(cwd, sizeof(cwd)) == NULL) strcpy(cwd, "(getcwd FAILED)");
            fprintf(f, "CWD: %s\n", cwd);
            DIR *d;
            d = opendir("roms"); fprintf(f, "roms: %s\n", d?"OPEN":"fail"); if(d) closedir(d);
            d = opendir("mass:/roms"); fprintf(f, "mass:/roms: %s\n", d?"OPEN":"fail"); if(d) closedir(d);
            d = opendir("mass0:/roms"); fprintf(f, "mass0:/roms: %s\n", d?"OPEN":"fail"); if(d) closedir(d);
            {
                extern uint64_t GetTimerSystemTime(void);
                uint64_t t0, t1, t2;
                volatile uint32_t *t1c = (volatile uint32_t *)0x10000800;
                uint32_t s0, s1;
                t0 = GetTimerSystemTime();
                s0 = *t1c;
                usleep(100000);
                t1 = GetTimerSystemTime();
                s1 = *t1c;
                usleep(100000);
                t2 = GetTimerSystemTime();
                fprintf(f, "TIMER us_d1=%u us_d2=%u t1cnt_delta=%u base=%u%c",
                    (unsigned)(t1 - t0), (unsigned)(t2 - t1),
                    (unsigned)(s1 - s0), (unsigned)(t0 & 0xffffffff), 10);
            }
            fclose(f);
        }
    }
#endif /* DEBUG LOG DISABLED */

	boot_log("[S2] ps2_init end");
	return ps2;
}

static void ps2_free(void *data) {
	ps2_platform_t *ps2 = (ps2_platform_t*)data;

    deinit_drivers();

	free(ps2);
}

void dbg_printf(const char *fmt, ...)
{
	/* DEBUG LOG DISABLED (v16 cleanup): no njemu_dbg.txt output. */
	(void)fmt;
}

static void ps2_main(void *data, int argc, char *argv[]) {
	ps2_platform_t *ps2 = (ps2_platform_t*)data;

    getcwd(screenshotDir, sizeof(screenshotDir));
    strcat(screenshotDir, "/PICTURE");
    mkdir(screenshotDir, 0777);
#if	(EMU_SYSTEM == CPS1)
	strcat(screenshotDir, "/CPS1");
#endif
#if	(EMU_SYSTEM == CPS2)
	strcat(screenshotDir, "/CPS2");
#endif
#if	(EMU_SYSTEM == MVS)
	strcat(screenshotDir, "/MVS");
#endif
#if	(EMU_SYSTEM == NCDZ)
	strcat(screenshotDir, "/NCDZ");
#endif
}

static bool ps2_startSystemButtons(void *data) {
return false;
}

static int32_t ps2_getDevkitVersion(void *data) {
	return 0;
}

platform_driver_t platform_ps2 = {
	"ps2",
	ps2_init,
	ps2_free,
	ps2_main,
	ps2_startSystemButtons,
	ps2_getDevkitVersion,
};
