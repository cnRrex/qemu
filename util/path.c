/* Code to mangle pathnames into those matching a given prefix.
   eg. open("/lib/foo.so") => open("/usr/gnemul/i386-linux/lib/foo.so");

   The assumption is that this area does not change.
*/
#include "qemu/osdep.h"
#include <sys/param.h>
#include <dirent.h>
#include "qemu/cutils.h"
#include "qemu/path.h"
#include "qemu/thread.h"

static const char *base_nb;
static const char *base;
static GHashTable *hash;
static QemuMutex lock;

void init_paths(const char *prefix)
{
    if (prefix[0] == '\0' || !strcmp(prefix, "/")) {
        return;
    }

    if (prefix[0] == '/') {
        base = g_strdup(prefix);
    } else {
        char *cwd = g_get_current_dir();
        base = g_build_filename(cwd, prefix, NULL);
        g_free(cwd);
    }

    hash = g_hash_table_new(g_str_hash, g_str_equal);
    qemu_mutex_init(&lock);
}

void init_paths_nb(const char *prefix)
{
    if (prefix[0] == '\0' || !strcmp(prefix, "/")) {
        return;
    }

    if (prefix[0] == '/') {
        base_nb = g_strdup(prefix);
    } else {
        char *cwd = g_get_current_dir();
        base_nb = g_build_filename(cwd, prefix, NULL);
        g_free(cwd);
    }
}

/* Look for path in emulation dir, otherwise return name. */
const char *path(const char *name)
{
    gpointer key, value;
    const char *ret;

    /* Only do absolute paths: quick and dirty, but should mostly be OK.  */
    if (!base || !name || name[0] != '/') {
        return name;
    }

    qemu_mutex_lock(&lock);

    /* Have we looked up this file before?  */
    if (g_hash_table_lookup_extended(hash, name, &key, &value)) {
        ret = value ? value : name;
    } else {
        char *save = g_strdup(name);

        if (bash_nb) {
            char *full_nb = g_build_filename(base_nb, name, NULL);

            /* Look for the path; record the result, pass or fail.  */
            if (access(full_nb, F_OK) == 0) {
                /* Exists.  */
                g_hash_table_insert(hash, save, full_nb);
                ret = full_nb;
                goto out;
            } else {
                /* Does not exist.  */
                g_free(full_nb);
            }
        }
        char *full = g_build_filename(base, name, NULL);

        /* Look for the path; record the result, pass or fail.  */
        if (access(full, F_OK) == 0) {
            /* Exists.  */
            g_hash_table_insert(hash, save, full);
            ret = full;
        } else {
            /* Does not exist.  */
            g_free(full);
            g_hash_table_insert(hash, save, NULL);
            ret = name;
        }
     out:
    }

    qemu_mutex_unlock(&lock);
    return ret;
}
