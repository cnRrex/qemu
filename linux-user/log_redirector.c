/*
 * log_redirector.c: redirect stdout and stderr to android log
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include "qemu/osdep.h"
#include "qemu.h"
#include "user-internals.h"

static int pfd[2];
static pthread_t thr;
static const char *tag = "qemu";
/* prio: debug-3, warn-5, error-6 */
static int (*__a_log_write)(int prio, const char *tag, const char *msg);
static int log_prio = 6;
void *log_handle = NULL;

static void thread_func_cleanup(void *arg)
{
  dlclose(log_handle);
}

/* from
 * https://codelab.wordpress.com/2014/11/03/how-to-use-standard-output-streams-for-logging-in-android-apps/
 */
static void *thread_func(void *) {
  pthread_cleanup_push(thread_func_cleanup, NULL);
  ssize_t rdsz;
  char buf[128];
  while ((rdsz = read(pfd[0], buf, sizeof buf - 1)) > 0) {
    buf[rdsz] = 0; /* add null-terminator */
    __a_log_write(log_prio, tag, buf);
  }
  pthread_testcancel();
  pthread_cleanup_pop(1);
  return 0;
}

int start_logger(const char *name) {
  tag = name;

  /* get android log write fcn */
  if (log_handle == NULL) {
    log_handle = dlopen("liblog.so", RTLD_LAZY);
    if (log_handle == NULL) {
      fprintf(stderr, "nb-qemu: dlopen liblog.so failed\n");
      return -1;
    }
    __a_log_write = dlsym(log_handle, "__android_log_write");
    if (__a_log_write == NULL) {
      fprintf(stderr, "nb-qemu: dlsym __android_log_write failed\n");
      dlclose(log_handle);
      return -1;
    }
  }

  /* make stdout line-buffered and stderr unbuffered */
  setvbuf(stdout, 0, _IOLBF, 0);
  setvbuf(stderr, 0, _IONBF, 0);

  /* create the pipe and redirect stdout and stderr */
  pipe(pfd);
  /* log stdout and lower the piro in debug mode */
  if (_nb_debug_) {
    log_prio = 3;
    dup2(pfd[1], 1);
  }
  dup2(pfd[1], 2);

  /* spawn the logging thread */
  if (pthread_create(&thr, 0, thread_func, 0) == -1){
    dlclose(log_handle);
    return -1;
  }
  pthread_detach(thr);
  return 0;
}
