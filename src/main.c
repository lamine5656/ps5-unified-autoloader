/*
 * ps5-autoloader — main entry point
 *
 * Execution flow:
 *   1. Handle WebKit browser (if running, return to Home screen)
 *   2. Kill entry app (YouTube / Artemis Lua / Yarpe / mast1c0re if running)
 *   3. Kill BD Disc Player (if running, with proper timing)
 *   4. Wait for elfldr to be ready on port 9021
 *   5. Scan USB0-7, then /data for autoload.txt
 *   6a. If found: iterate lines, launch each .elf from the config directory
 *   6b. If not found: send embedded pldmgr.elf to elfldr (fallback)
 */

#include "autoloader.h"
#include "app_killer.h"
#include "launcher.h"
#include "notification.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sync.h"

/*
 * pldmgr.elf is embedded at build time via:
 *   xxd -i pldmgr.elf > pldmgr_elf.c
 * which generates:
 *   unsigned char pldmgr_elf[] = { ... };
 *   unsigned int  pldmgr_elf_len = ...;
 */
#include "pldmgr_elf.c"

/*
 * NOTE (l92): hyperhen.elf is NO LONGER embedded. A 12+ MB autoloader ELF has to
 * be read fully into memory by the browser-side JS (autoload.js) which crashes
 * the exploit context (CE-108255-1). Instead the autoloader launches
 * hyperhen.elf FROM FILE (next to autoload.txt, or /data/ps5_autoloader/)
 * right after elfldr is ready, before the rest of the autoload sequence.
 */

/* -----------------------------------------------------------------------
 * find_autoload_config
 *
 * Searches for ps5_autoloader/autoload.txt in order:
 *   /data                   (internal storage — highest priority, per user preference)
 *   /mnt/usb0 .. /mnt/usb7  (fallback)
 *
 * Writes the found path into out_buf (size out_size).
 * Returns 0 on success (found), -1 if not found.
 * ----------------------------------------------------------------------- */
static int find_autoload_config(char *out_buf, size_t out_size) {
    struct stat st;

    /* Internal storage ( /data ) has PRIORITY over USB (user wants offline
       launches without needing the stick inserted) */
    if (g_entry_point_id[0] != '\0') {
        snprintf(out_buf, out_size, "%s/ps5_autoloader_%s/autoload.txt", DATA_BASE, g_entry_point_id);
        if (stat(out_buf, &st) == 0) {
            printf("[autoloader] Found config in /data: %s\n", out_buf);
            fflush(stdout);
            return 0;
        }
    }

    snprintf(out_buf, out_size, "%s/%s", DATA_BASE, AUTOLOAD_CONFIG_SUBPATH);
    if (stat(out_buf, &st) == 0) {
        printf("[autoloader] Found config in /data: %s\n", out_buf);
        fflush(stdout);
        return 0;
    }

    /* Fallback: app-specific USB dirs */
    if (g_entry_point_id[0] != '\0') {
        for (int i = 0; i < USB_COUNT; i++) {
            snprintf(out_buf, out_size, "%s/ps5_autoloader_%s/autoload.txt", USB_BASES[i], g_entry_point_id);
            if (stat(out_buf, &st) == 0) {
                printf("[autoloader] Found config on USB%d: %s\n", i, out_buf);
                fflush(stdout);
                return 0;
            }
        }
    }

    /* Fallback: generic USB dirs */
    for (int i = 0; i < USB_COUNT; i++) {
        snprintf(out_buf, out_size, "%s/%s", USB_BASES[i], AUTOLOAD_CONFIG_SUBPATH);
        if (stat(out_buf, &st) == 0) {
            printf("[autoloader] Found config on USB%d: %s\n", i, out_buf);
            fflush(stdout);
            return 0;
        }
    }

    out_buf[0] = '\0';
    return -1;
}


/* -----------------------------------------------------------------------
 * resolve_case_insensitive
 *
 * Tries to find `name` inside `dir` regardless of letter case (UFS is
 * case-sensitive; the PS5 file system is not forgiving). Returns 0 and
 * fills out_buf with full path "<dir><real name>" on match, -1 otherwise.
 * ----------------------------------------------------------------------- */
static int resolve_case_insensitive(const char *dir, const char *name, char *out_buf, size_t out_size) {
    DIR *d = opendir(dir);
    if (!d) {
        return -1;
    }

    struct dirent *entry;
    int found = -1;
    while ((entry = readdir(d)) != NULL) {
        if (strcasecmp(entry->d_name, name) == 0) {
            snprintf(out_buf, out_size, "%s%s", dir, entry->d_name);
            found = 0;
            break;
        }
    }

    closedir(d);
    return found;
}

/* -----------------------------------------------------------------------
 * run_autoload_sequence
 *
 * Opens config_path and processes each line:
 *   - Empty lines and '#' comments are skipped
 *   - Lines starting with '@' are directives (e.g. @sync)
 *   - Lines starting with '!' are sleep commands: !<ms>
 *   - Everything else is treated as a filename relative to config_dir
 * ----------------------------------------------------------------------- */
static void run_autoload_sequence(const char *config_path) {
    /* Derive the directory containing the config file */
    char config_dir[512];
    strncpy(config_dir, config_path, sizeof(config_dir) - 1);
    config_dir[sizeof(config_dir) - 1] = '\0';

    char *last_slash = strrchr(config_dir, '/');
    if (last_slash) {
        *(last_slash + 1) = '\0'; /* keep trailing slash */
    } else {
        config_dir[0] = '\0';
    }

    printf("[autoloader] Config dir: %s\n", config_dir);
    fflush(stdout);

    FILE *f = fopen(config_path, "r");
    if (!f) {
        printf("[autoloader] Failed to open config: %s\n", config_path);
        fflush(stdout);
        autoloader_notify("Autoload: failed to open config");
        return;
    }

    char line[512];
    int launched = 0;
    int errors = 0;
    int should_sync = 0;
    int bom_checked = 0;

    /* Dedupe: basenames already launched this run (case-insensitive) so a
       payload listed twice in autoload.txt is NOT launched twice */
    char seen[32][128];
    int seen_count = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Strip the UTF-8 BOM editors like Windows Notepad add at file start;
           it silently breaks the stat() of the FIRST listed payload */
        if (!bom_checked) {
            bom_checked = 1;
            if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) {
                memmove(line, line + 3, strlen(line + 3) + 1);
            }
        }

        /* Strip comments if any */
        char *comment = strchr(line, '#');
        if (comment) {
            *comment = '\0';
        }

        /* Strip trailing newline / carriage return */
        line[strcspn(line, "\r\n")] = '\0';

        /* Strip trailing whitespace */
        int len = strlen(line);
        while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
            line[--len] = '\0';
        }

        /* Skip empty lines */
        if (line[0] == '\0')
            continue;

        printf("[autoloader] Processing: %s\n", line);
        fflush(stdout);

        if (line[0] == '@') {
            /* Directive */
            if (strcmp(line, "@sync") == 0) {
                should_sync = 1;
            }
            continue;
        } else if (line[0] == '!') {
            /* Sleep command: !<ms> */
            int ms = atoi(line + 1);
            if (ms > 0) {
                printf("[autoloader] Sleeping %d ms\n", ms);
                fflush(stdout);
                usleep((useconds_t)ms * 1000);
            }
        } else {
            /* ELF/BIN payload — resolve relative to config directory */
            /* Skip the embedded payload: hyperhen.elf is auto-launched from memory
               earlier (Step 4b). Launching it twice crashes ("hypercore stopped"). */
            char base_name[256];
            snprintf(base_name, sizeof(base_name), "%s", line);
            char *base_slash = strrchr(base_name, '/');
            if (base_slash) {
                memmove(base_name, base_slash + 1, strlen(base_slash + 1) + 1);
            }
            if (strcasecmp(base_name, "hyperhen.elf") == 0) {
                /* Already auto-launched from file before this sequence */
                printf("[autoloader] Skipping %s: already auto-launched\n", line);
                fflush(stdout);
                continue;
            }

            /* Skip duplicates of the same payload within this run */
            int dup = 0;
            for (int s = 0; s < seen_count; s++) {
                if (strcasecmp(base_name, seen[s]) == 0) {
                    printf("[autoloader] Skipping duplicate: %s (already launched this run)\n", line);
                    fflush(stdout);
                    dup = 1;
                    break;
                }
            }
            if (dup) {
                continue;
            }

            char full_path[1024];
            if (line[0] == '/') {
                /* Absolute path — use as-is */
                snprintf(full_path, sizeof(full_path), "%s", line);
            } else {
                snprintf(full_path, sizeof(full_path), "%s%s", config_dir, line);
            }

            struct stat st;
            if (stat(full_path, &st) != 0) {
                /* Case-insensitive fallback: UFS is case-sensitive but users often
                   write "HyperHen.elf" on the USB / in autoload.txt mismatched */
                char resolved[1024];
                if (resolve_case_insensitive(config_dir, line, resolved, sizeof(resolved)) == 0) {
                    printf("[autoloader] Case-insensitive match: %s -> %s\n", full_path, resolved);
                    fflush(stdout);
                    snprintf(full_path, sizeof(full_path), "%s", resolved);
                }
            }

            if (stat(full_path, &st) == 0) {
                autoloader_notify("Launching: %s", line);
                if (launch_elf_from_file(full_path) != 0) {
                    autoloader_notify("Failed to launch: %s", line);
                    errors++;
                } else {
                    launched++;
                    if (seen_count < 32) {
                        snprintf(seen[seen_count], sizeof(seen[0]), "%s", base_name);
                        seen_count++;
                    }
                    /* Small delay to give elfldr time to receive and process */
                    usleep(500000); /* 500 ms */
                }
            } else {
                printf("[autoloader] Not found: %s\n", full_path);
                fflush(stdout);
                /* include the resolved path in the notification for easier debugging */
                autoloader_notify("Not found: %s (%s)", line, full_path);
                errors++;
            }
        }
    }

    fclose(f);
    printf("[autoloader] Autoload sequence complete. Launched %d payload(s).\n", launched);
    fflush(stdout);
    if (launched > 0) {
        autoloader_notify("Autoload complete (%d payload(s))", launched);
    }

    if (should_sync && errors == 0) {
        try_sync_usb_to_data(config_path);
    }
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(void) {
    printf("[autoloader] ps5-autoloader v" AUTOLOADER_VERSION " (" __DATE__ " " __TIME__ ") starting\n");
    fflush(stdout);

    /* Step 1: handle WebKit browser if running (navigate to Home) */
    handle_browser_app();

    /* Step 2: kill entry point app (YouTube or Artemis) if running (simple SIGKILL) */
    kill_entry_app();

    /* Step 3: kill BD Disc Player if running (suspend → wait → SIGKILL → LncKill) */
    kill_disc_player();

    /* Step 4: wait for elfldr to be ready on port 9021 */
    if (wait_for_elfldr() != 0) {
        autoloader_notify("ERROR: elfldr not available. Aborting.");
        printf("[autoloader] ERROR: elfldr did not become available. Aborting.\n");
        fflush(stdout);
        return -1;
    }

    /* Step 4b (helper): launch HyperHEN from `dir` if hyperhen.elf exists there.
       Returns 1 when launched, 0 otherwise. */
    /* Step 5: locate autoload.txt */
    char config_path[512];
    int found = (find_autoload_config(config_path, sizeof(config_path)) == 0);

    /* Detect/launch HyperHEN from file BEFORE running the autoload sequence.
       It sits in the same dir as autoload.txt (or in the generic dirs below). */
    char hen_path[1024];
    int hen_launched = 0;
    if (found) {
        /* dir containing the found autoload.txt */
        char config_dir[512];
        strncpy(config_dir, config_path, sizeof(config_dir) - 1);
        config_dir[sizeof(config_dir) - 1] = '\0';
        char *last_slash = strrchr(config_dir, '/');
        if (last_slash) {
            *(last_slash + 1) = '\0';
        } else {
            config_dir[0] = '\0';
        }
        if (resolve_case_insensitive(config_dir, "hyperhen.elf", hen_path, sizeof(hen_path)) == 0) {
            printf("[autoloader] Launching HyperHEN from file: %s\n", hen_path);
            fflush(stdout);
            autoloader_notify("Launching HyperHEN...");
            if (launch_elf_from_file(hen_path) == 0) {
                usleep(1500000); /* 1.5 s - give the kernel payload time to settle */
                autoloader_notify("HyperHEN loaded");
                hen_launched = 1;
            } else {
                printf("[autoloader] WARNING: HyperHEN file launch failed, continuing\n");
                fflush(stdout);
                autoloader_notify("HyperHEN launch failed (continuing)");
            }
        }
    }
    if (!hen_launched) {
        /* Fallback: generic autoload dirs (works even without autoload.txt) */
        const char *hen_search_bases[] = {
            "/data/ps5_autoloader/",
            "/mnt/usb0/ps5_autoloader/",
            "/mnt/usb1/ps5_autoloader/",
            "/mnt/usb2/ps5_autoloader/",
            "/mnt/usb3/ps5_autoloader/",
            "/mnt/usb4/ps5_autoloader/",
            "/mnt/usb5/ps5_autoloader/",
            "/mnt/usb6/ps5_autoloader/",
            "/mnt/usb7/ps5_autoloader/"
        };
        for (size_t k = 0; k < sizeof(hen_search_bases) / sizeof(hen_search_bases[0]); k++) {
            if (resolve_case_insensitive(hen_search_bases[k], "hyperhen.elf", hen_path, sizeof(hen_path)) == 0) {
                printf("[autoloader] Launching HyperHEN from file: %s\n", hen_path);
                fflush(stdout);
                autoloader_notify("Launching HyperHEN...");
                if (launch_elf_from_file(hen_path) == 0) {
                    usleep(1500000);
                    autoloader_notify("HyperHEN loaded");
                    hen_launched = 1;
                }
                break;
            }
        }
    }

    if (found) {
        /* Step 6a: run the autoload sequence from config */
        printf("[autoloader] Using autoload config: %s\n", config_path);
        fflush(stdout);
        run_autoload_sequence(config_path);
    } else {
        /* Step 6b: no config — fall back to embedded pldmgr */
        printf("[autoloader] No autoload config found. Starting Payload Manager...\n");
        fflush(stdout);
        if (launch_elf_from_memory(pldmgr_elf, pldmgr_elf_len) != 0) {
            autoloader_notify("ERROR: failed to launch Payload Manager");
            printf("[autoloader] ERROR: failed to launch pldmgr\n");
            fflush(stdout);
            return -1;
        }
    }

    return 0;
}
