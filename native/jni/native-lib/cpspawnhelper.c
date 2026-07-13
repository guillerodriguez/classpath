/* cpspawnhelper.c - spawn helper for the vfork-based process launch path
   Copyright (C) 2026  Free Software Foundation, Inc.

This file is part of GNU Classpath.

GNU Classpath is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

GNU Classpath is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Classpath; see the file COPYING.  If not, write to the
Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
02110-1301 USA.

Linking this library statically or dynamically with other modules is
making a combined work based on this library.  Thus, the terms and
conditions of the GNU General Public License cover the whole
combination.

As a special exception, the copyright holders of this library give you
permission to link this library with independent modules to produce an
executable, regardless of the license terms of these independent
modules, and to copy and distribute the resulting executable under
terms of your choice, provided that you also meet, for each linked
independent module, the terms and conditions of the license of that
module.  An independent module is a module which is not derived from
or based on this library.  If you modify this library, you may extend
this exception to your version of the library, but you are not
obligated to do so.  If you do not wish to do so, delete this
exception statement from your version. */

/* This tiny standalone executable performs the child-side work of a
   process spawn (dup2 of the stdio pipe fds, closing them, an optional
   chdir, and the PATH search) and finally execs the target binary.

   It exists so that cpproc.c can call posix_spawn() with no file
   actions and no attributes: all the child-side setup is done here, in
   a fresh process after the exec, instead of through file actions
   executed between fork/vfork and exec. Keeping the file actions empty
   also lets posix_spawn use its vfork()/clone(CLONE_VFORK) fast path
   even on old glibc, where any file action would force a plain fork().

   It is invoked by cpproc_forkAndExec (via posix_spawn) with:

     argv[0]  helper path
     argv[1]  fail_fd     - write end of the parent's failure pipe
     argv[2]  pipe_count  - 2 (stderr redirected to stdout) or 3
     argv[3..]            - pipe_count*2 file descriptors (local_fds)
     argv[..] has_dir     - "1" if a working directory follows, else "0"
     argv[..] dir         - working directory (empty when has_dir is 0)
     argv[..] path        - PATH to search for the target
     argv[..] "--"        - separator
     argv[..]             - the target argv (NULL-terminated)

   The helper's own environment is the target's environment. On
   startup the helper first writes CP_HELPER_ALIVE to fail_fd, so
   that the parent can tell a helper that was never exec'd (EOF with
   no ping; posix_spawn implementations such as glibc before 2.24 do
   not report that failure themselves) from one that ran. After the
   ping, the protocol is the same one the parent uses for the plain
   fork() path: on any failure the helper writes its errno to fail_fd
   and _exit()s; on success the exec of the target closes fail_fd
   (FD_CLOEXEC) and the parent reads EOF. */

#include "config.h"
#include "cpexec.h"
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

int main(int argc, char **argv)
{
  int fail_fd;
  int pipe_count;
  int local_fds[6];
  int has_dir;
  const char *dir;
  const char *path;
  char **target_argv;
  char **sh_argv;
  int errnum = EINVAL;
  int alive = CP_HELPER_ALIVE;
  int i;
  int idx = 1;
  int min_argc;

  /* Need at least argv[0], fail_fd and pipe_count */
  if (argc < 3)
    _exit(127);

  fail_fd = atoi(argv[idx++]);

  /* Tell the parent we are alive before doing anything else; it needs
     the ping to know that the exec of the helper itself succeeded */
  while (write(fail_fd, &alive, sizeof(alive)) < 0 && errno == EINTR)
    ;

  pipe_count = atoi(argv[idx++]);

  if (pipe_count != 2 && pipe_count != 3)
    goto fail;

  /* helper fail_fd pipe_count <fds> has_dir dir path -- arg0 */
  min_argc = 1 + 1 + 1 + pipe_count * 2 + 1 + 1 + 1 + 1 + 1;
  if (argc < min_argc)
    goto fail;

  for (i = 0; i < pipe_count * 2; i++)
    local_fds[i] = atoi(argv[idx++]);

  has_dir = atoi(argv[idx++]);
  dir = argv[idx++];
  path = argv[idx++];

  if (argv[idx] == NULL || strcmp(argv[idx], "--") != 0)
    goto fail;
  idx++;

  target_argv = &argv[idx];
  if (target_argv[0] == NULL)
    {
      errnum = ENOENT;
      goto fail;
    }

  /* Preallocate the shell-fallback argv used by cp_execvpe */
  for (i = 0; target_argv[i] != NULL; i++)
    ;
  sh_argv = malloc((i + 2) * sizeof(char *));
  if (sh_argv == NULL)
    {
      errnum = ENOMEM;
      goto fail;
    }

  /* A failed exec must be reported, so keep fail_fd open across a
     failure but have a successful exec of the target close it. */
  if (fcntl(fail_fd, F_SETFD, FD_CLOEXEC) < 0)
    {
      errnum = errno;
      goto fail;
    }

  dup2(local_fds[0], 0);
  dup2(local_fds[3], 1);
  if (pipe_count == 3)
    dup2(local_fds[5], 2);
  else
    dup2(1, 2);

  cp_close_all_fds(local_fds, pipe_count * 2);

  if (!has_dir || chdir(dir) == 0)
    cp_execvpe(target_argv[0], target_argv, environ, path, sh_argv);

  errnum = errno;

fail:
  while (write(fail_fd, &errnum, sizeof(errnum)) < 0 && errno == EINTR)
    ;
  _exit(127);
}
