/* waydroid.c — Waydroid-style Android runtime for CodeOS.
 *
 * `waydroid` is a kernel shell command (registered in shell.c) that
 * orchestrates an Android guest container in the existing appvm engine:
 *
 *   waydroid init             seed android-stock image + install apps
 *   waydroid session start    boot the Android container (PID 1 = init)
 *   waydroid session stop     stop it
 *   waydroid session pause    pause / resume
 *   waydroid app list         installed Android apps
 *   waydroid app launch <a>   run an app (desktop window or console demo)
 *   waydroid shell            boot-role shell demo inside the guest
 *   waydroid status           overall status
 *
 * The container-side runtime is the existing run-as/init shim; apps are the
 * android-* ELFs installed at /system/app/<name>/<name>. When the desktop
 * compositor is present the app gets a real window through the user_wm
 * bridge (fds 3/4); otherwise it falls back to a console demo, so headless
 * verification stays deterministic.
 */

#include "waydroid.h"
#include "kprintf.h"
#include "string.h"
#include "fs.h"
#include "container.h"
#include "rootfs.h"
#include "user_wm.h"

/* ─── bundled Android apps (names installed from /bin/android-<name>) ───
 * Keep in sync with ANDROID_PROGS in kernel/userspace/Makefile. */
static const char *wd_app_names[WAYDROID_MAX_APPS] = {
    "android-launcher", "android-clock", "android-calculator",
    "android-settings", "android-dialer", "android-music",
    "android-browser",  "android-camera", "android-calendar",
    "android-keyboard", 0
};

struct wd_display {
    const char *elf;
    const char *label;
};

static const struct wd_display wd_displays[] = {
    { "android-launcher",   "Launcher"   },
    { "android-clock",      "Clock"      },
    { "android-calculator", "Calculator" },
    { "android-settings",   "Settings"   },
    { "android-dialer",     "Dialer"     },
    { "android-music",      "Music"      },
    { "android-browser",    "Browser"    },
    { "android-camera",     "Camera"     },
    { "android-calendar",   "Calendar"   },
    { "android-keyboard",   "Keyboard"   },
    { 0, 0 }
};

static const char *wd_label_for(const char *elf) {
    for (int i = 0; wd_displays[i].elf; i++)
        if (strcmp(wd_displays[i].elf, elf) == 0)
            return wd_displays[i].label;
    return elf;
}

static void wd_app_dir(char *out, size_t outsz, const char *app) {
    snprintf(out, outsz, "%s/system/app/%s", WAYDROID_IMAGE_ROOT, app);
}

static void wd_app_path(char *out, size_t outsz, const char *app) {
    snprintf(out, outsz, "%s/system/app/%s/%s", WAYDROID_IMAGE_ROOT, app, app);
}

static int wd_install_one(const char *name) {
    char src[FS_PATH_MAX];
    char dir[FS_PATH_MAX];
    char dst[FS_PATH_MAX];
    char buf[FS_CONTENT_MAX];
    int is_dir;

    snprintf(src, sizeof(src), "/bin/%s", name);
    if (fs_resolve(src, &is_dir) < 0) {
        kprintf("waydroid: note: /bin/%s not present, skipping install\n", name);
        return 0;
    }

    wd_app_dir(dir, sizeof(dir), name);
    wd_app_path(dst, sizeof(dst), name);

    if (fs_resolve(dst, &is_dir) >= 0) {
        kprintf("waydroid: %s already installed\n", name);
        return 0;
    }

    int n = fs_read(src, buf, FS_CONTENT_MAX);
    if (n <= 0) {
        kprintf("waydroid: failed to read %s\n", src);
        return -1;
    }

    /* ensure /system/app exists, then the app dir */
    fs_mkdir(WAYDROID_IMAGE_ROOT "/system");
    fs_mkdir(WAYDROID_IMAGE_ROOT "/system/app");
    fs_mkdir(dir);
    if (fs_mkfile(dst) < 0) {
        kprintf("waydroid: failed to create %s\n", dst);
        return -1;
    }
    fs_write(dst, buf, n);
    kprintf("waydroid: installed %s (%d bytes)\n", name, n);
    return 0;
}

int waydroid_init(void) {
    int ok = 0;
    int have_image;

    /* Seed the guest image only once: boot already materializes it, and
     * re-seeding would churn the fs tree (the seed is not idempotent). */
    if (fs_resolve(WAYDROID_IMAGE_ROOT "/init", &have_image) < 0) {
        if (rootfs_seed_android_stock() < 0) {
            kprintf("waydroid: init failed: could not seed %s image\n", WAYDROID_IMAGE_NAME);
            return -1;
        }
    }

    /* Write a small build.prop so the guest has Android metadata. */
    {
        char prop[FS_PATH_MAX];
        char content[512];
        snprintf(prop, sizeof(prop), "%s/system/build.prop", WAYDROID_IMAGE_ROOT);
        fs_mkdir(WAYDROID_IMAGE_ROOT "/system");
        int n = snprintf(content, sizeof(content),
            "# Waydroid build fingerprint (CodeOS)\n"
            "ro.build.fingerprint=CodeOS/waydroid/x86_64:12/SKQ1.211006.001/waydroid:user/release-keys\n"
            "ro.build.version.sdk=31\n"
            "ro.build.version.release=12\n"
            "ro.product.model=CodeOS Android\n"
            "ro.product.device=waydroid\n"
            "ro.hardware=codeos\n"
            "ro.secure=0\n"
            "ro.debuggable=1\n"
            "persist.sys.dalvik.vm.lib.2=libart.so\n");
        if (n < 0) n = 0;
        if (n > (int)sizeof(content) - 1) n = (int)sizeof(content) - 1;
        fs_mkfile(prop);
        fs_write(prop, content, n);
        kprintf("waydroid: wrote %s/system/build.prop\n", WAYDROID_IMAGE_ROOT);
    }

    for (int i = 0; wd_app_names[i]; i++)
        if (wd_install_one(wd_app_names[i]) == 0)
            ok++;

    kprintf("waydroid: init complete (%d apps ready)\n", ok);
    return 0;
}

static int wd_ensure_container(void) {
    container_t *c = container_find(WAYDROID_CONT_NAME);
    if (c) return c->id;
    int id = container_create(WAYDROID_CONT_NAME, WAYDROID_IMAGE_NAME);
    if (id < 0) {
        kprintf("waydroid: could not create container '%s'\n", WAYDROID_CONT_NAME);
        return -1;
    }
    kprintf("waydroid: container '%s' created (id=%d)\n", WAYDROID_CONT_NAME, id);
    return id;
}

int waydroid_session_start(void) {
    int id = wd_ensure_container();
    if (id < 0) return -1;

    container_t *c = container_get(id);
    if (!c) return -1;
    if (c->state == CONTAINER_RUNNING) {
        kprintf("waydroid: Android session already running\n");
        return 0;
    }
    if (container_start(id) < 0) {
        kprintf("waydroid: session start failed\n");
        return -1;
    }
    kprintf("waydroid: Android session running (id=%d)\n", id);
    return 0;
}

int waydroid_session_stop(void) {
    container_t *c = container_find(WAYDROID_CONT_NAME);
    if (!c || (c->state != CONTAINER_RUNNING && c->state != CONTAINER_PAUSED)) {
        kprintf("waydroid: no running Android session\n");
        return -1;
    }
    if (container_stop(c->id) < 0) return -1;
    kprintf("waydroid: Android session stopped\n");
    return 0;
}

int waydroid_session_pause(void) {
    container_t *c = container_find(WAYDROID_CONT_NAME);
    if (!c) return -1;
    container_set_state(c->id, CONTAINER_PAUSED);
    kprintf("waydroid: Android session paused\n");
    return 0;
}

int waydroid_session_resume(void) {
    container_t *c = container_find(WAYDROID_CONT_NAME);
    if (!c) return -1;
    container_set_state(c->id, CONTAINER_RUNNING);
    kprintf("waydroid: Android session resumed\n");
    return 0;
}

int waydroid_session_active(void) {
    container_t *c = container_find(WAYDROID_CONT_NAME);
    if (!c) return 0;
    return c->state == CONTAINER_RUNNING;
}

int waydroid_app_installed(const char *app) {
    char dst[FS_PATH_MAX];
    int is_dir;
    wd_app_path(dst, sizeof(dst), app);
    return fs_resolve(dst, &is_dir) >= 0 && !is_dir;
}

int waydroid_app_launch(const char *app) {
    int id;
    char path[FS_PATH_MAX];
    container_t *c;

    if (!app || !app[0]) {
        kprintf("waydroid: app launch: missing app name\n");
        return -1;
    }
    if (!waydroid_app_installed(app)) {
        kprintf("waydroid: app '%s' is not installed\n", app);
        return -1;
    }

    id = wd_ensure_container();
    if (id < 0) return -1;
    c = container_get(id);
    if (!c) return -1;
    if (c->state != CONTAINER_RUNNING) {
        kprintf("waydroid: starting Android session for '%s'...\n", app);
        if (container_start(id) < 0) return -1;
    }

    kprintf("waydroid: launching '%s' in Android guest\n", app);
    snprintf(path, sizeof(path), "/system/app/%s/%s", app, app);
    return container_exec(id, path, 0, 0, 0);
}

int waydroid_app_list(char *buf, int max) {
    int n = 0;
    for (int i = 0; wd_app_names[i]; i++) {
        const char *name = wd_app_names[i];
        const char *label = wd_label_for(name);
        int installed = waydroid_app_installed(name);
        /* NB: the kernel snprintf has no '-' flag, so pad right-aligned. */
        int w = snprintf(buf + n, max > n ? (size_t)(max - n) : 0,
                         "  %18s %4s %s\n", label,
                         installed ? "[ok]" : "[--]", name);
        if (w < 0) break;
        n += w;
        if (n >= max) break;
    }
    if (n == 0)
        n = snprintf(buf, (size_t)max, "  (no apps - run 'waydroid init')\n");
    return n;
}

int waydroid_shell(void) {
    int id = wd_ensure_container();
    container_t *c;
    if (id < 0) return -1;
    c = container_get(id);
    if (!c) return -1;
    if (c->state != CONTAINER_RUNNING) {
        if (container_start(id) < 0) return -1;
    }
    kprintf("waydroid: opening Android container shell\n");
    return container_exec(id, "/bin/sh", 0, 0, 0);
}

int waydroid_status(char *buf, int max) {
    int n = 0;
    container_t *c = container_find(WAYDROID_CONT_NAME);
    const char *state = "not created";

    if (c) {
        switch (c->state) {
        case CONTAINER_RUNNING: state = "running"; break;
        case CONTAINER_PAUSED:  state = "paused";  break;
        case CONTAINER_STOPPED: state = "stopped"; break;
        default:                state = "created"; break;
        }
    }

    n += snprintf(buf + n, max > n ? (size_t)(max - n) : 0,
                  "Waydroid for CodeOS\n");
    n += snprintf(buf + n, max > n ? (size_t)(max - n) : 0,
                  "  image   : %s\n", WAYDROID_IMAGE_NAME);
    n += snprintf(buf + n, max > n ? (size_t)(max - n) : 0,
                  "  root    : %s\n", WAYDROID_IMAGE_ROOT);
    n += snprintf(buf + n, max > n ? (size_t)(max - n) : 0,
                  "  session : %s\n", state);
    n += snprintf(buf + n, max > n ? (size_t)(max - n) : 0,
                  "  gui     : %s (user-window bridge)\n",
                  user_wm_ready() ? "on" : "off");
    return n;
}

/* ─── shell command ─── */

static void wd_usage(void) {
    kprintf("usage: waydroid init\n");
    kprintf("       waydroid session start|stop|pause|resume\n");
    kprintf("       waydroid app list\n");
    kprintf("       waydroid app launch <app>\n");
    kprintf("       waydroid shell\n");
    kprintf("       waydroid status\n");
}

void cmd_waydroid(int argc, char **argv) {
    char buf[1024];

    if (argc < 2) { wd_usage(); return; }

    if (strcmp(argv[1], "init") == 0) {
        waydroid_init();
    } else if (strcmp(argv[1], "session") == 0 && argc >= 3) {
        if (strcmp(argv[2], "start") == 0)      waydroid_session_start();
        else if (strcmp(argv[2], "stop") == 0)  waydroid_session_stop();
        else if (strcmp(argv[2], "pause") == 0) waydroid_session_pause();
        else if (strcmp(argv[2], "resume") == 0) waydroid_session_resume();
        else kprintf("waydroid: unknown session subcommand '%s'\n", argv[2]);
    } else if (strcmp(argv[1], "app") == 0 && argc >= 3) {
        if (strcmp(argv[2], "list") == 0) {
            waydroid_app_list(buf, sizeof(buf));
            kprintf("%s", buf);
        } else if (strcmp(argv[2], "launch") == 0 && argc >= 4) {
            if (waydroid_app_launch(argv[3]) < 0)
                kprintf("waydroid: app launch failed\n");
        } else {
            kprintf("waydroid: usage: waydroid app list|launch <app>\n");
        }
    } else if (strcmp(argv[1], "shell") == 0) {
        waydroid_shell();
    } else if (strcmp(argv[1], "status") == 0) {
        waydroid_status(buf, sizeof(buf));
        kprintf("%s", buf);
    } else {
        wd_usage();
    }
}