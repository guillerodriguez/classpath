/* cpproc.c -
   Copyright (C) 2003, 2004, 2005, 2006  Free Software Foundation, Inc.

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

#include "config.h"
#include <jni.h>
#include "cpproc.h"
#include "cpexec.h"
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#if HAVE_VFORK_H
# include <vfork.h>
#endif

extern char **environ;

#ifdef HAVE_WORKING_VFORK

/* Return the path to the spawn helper executable, or NULL if none is
   available. The GNU_CLASSPATH_SPAWN_HELPER environment variable
   overrides the compiled-in location; this is mainly useful for
   testing against an uninstalled build tree. */
static const char *cp_spawn_helper_path(void)
{
  const char *p = getenv("GNU_CLASSPATH_SPAWN_HELPER");

  if (p != NULL && *p != '\0')
    return p;
#ifdef CP_SPAWN_HELPER
  return CP_SPAWN_HELPER;
#else
  return NULL;
#endif
}

/* vfork() the spawn helper. This is kept in its own (non-inlined)
   function on purpose: the vfork() child shares the parent's address
   space and stack, so it may do nothing but exec or _exit. Confining
   vfork() here guarantees no other local variable of the caller can be
   clobbered by the child, and keeps the child path to the bare minimum
   (execve of the helper, _exit on failure). All the "dangerous"
   child-side work (dup2, close, chdir, PATH search) is done by the
   helper *after* it has exec'd into a fresh address space. */
#if defined(__GNUC__)
__attribute__((__noinline__))
#endif
static pid_t cp_vfork_exec(const char *helper, char * const *helper_argv,
			   char * const *child_env)
{
  pid_t pid = vfork();

  if (pid == 0)
    {
      execve(helper, helper_argv, child_env);
      _exit(127);
    }

  return pid;
}

#endif /* HAVE_WORKING_VFORK */

int cpproc_forkAndExec (char * const *commandLine, char * const * newEnviron,
			int *fds, int pipe_count, pid_t *out_pid,
			const char *wd, int use_vfork)
{
  int local_fds[6];
  int fail_fds[2];
  const char *path;
  char **sh_argv = NULL;
  int errnum;
  ssize_t n;
  int argc;
  int i;
  pid_t pid;
#ifdef HAVE_WORKING_VFORK
  const char *helper = NULL;
  char **helper_argv = NULL;
  int do_vfork = 0;
#endif

  /* Initialize the output fds so that the caller sees no garbage in
     them if we return with an error, or in the unused stderr entry
     when redirection is requested */
  for (i = 0; i < CPIO_EXEC_NUM_PIPES; i++)
    fds[i] = -1;

  path = getenv("PATH");
  if (path == NULL)
    path = "/bin:/usr/bin";
  for (argc = 0; commandLine[argc] != NULL; argc++)
    ;

#ifdef HAVE_WORKING_VFORK
  /* Use the vfork path only when requested and a helper is actually
     available; otherwise fall back to the plain fork path below. */
  if (use_vfork)
    {
      helper = cp_spawn_helper_path();
      if (helper != NULL && access(helper, X_OK) == 0)
	do_vfork = 1;
    }
#else
  (void) use_vfork;
#endif

  /* The fork path executes cp_execvpe in the child, where after the
     fork of a multi-threaded process only async-signal-safe operations
     may run, so no malloc there: preallocate its buffer now. The vfork
     path does not need this (the helper allocates as usual). */
#ifdef HAVE_WORKING_VFORK
  if (!do_vfork)
#endif
    {
      sh_argv = malloc((argc + 2) * sizeof(char *));
      if (sh_argv == NULL)
	return ENOMEM;
    }

  for (i = 0; i < (pipe_count * 2); i += 2)
    {
      if (pipe(&local_fds[i]) < 0)
	{
	  int err = errno;

	  cp_close_all_fds(local_fds, i);
	  free(sh_argv);

	  return err;
	}
    }

  /* Extra pipe used by the child (or the helper) to report a failed
     chdir or exec to the parent. On success the final exec closes the
     write end (FD_CLOEXEC) and the parent reads EOF. */
  if (pipe(fail_fds) < 0)
    {
      int err = errno;

      cp_close_all_fds(local_fds, pipe_count * 2);
      free(sh_argv);

      return err;
    }

#ifdef HAVE_WORKING_VFORK
  if (do_vfork)
    {
      char numbuf[8][16];
      char * const *child_env = (newEnviron != NULL) ? newEnviron : environ;
      int k = 0;
      int nb = 0;

      /* The parent's read end must not leak into the helper or the
	 target; the write end must survive the exec of the helper (the
	 helper re-applies FD_CLOEXEC to it) so that its own exec of the
	 target closes it and the parent sees EOF on success. */
      if (fcntl(fail_fds[0], F_SETFD, FD_CLOEXEC) < 0)
	{
	  int err = errno;

	  cp_close_all_fds(local_fds, pipe_count * 2);
	  close(fail_fds[0]);
	  close(fail_fds[1]);
	  return err;
	}

      /* All marshalling happens here, in the parent, before vfork().
	 The helper argv is:
	   helper fail_fd pipe_count fd0..fdN hasDir dir path -- argv... */
      helper_argv = malloc((1 + 1 + 1 + pipe_count * 2 + 1 + 1 + 1 + 1
			    + argc + 1) * sizeof(char *));
      if (helper_argv == NULL)
	{
	  cp_close_all_fds(local_fds, pipe_count * 2);
	  close(fail_fds[0]);
	  close(fail_fds[1]);
	  return ENOMEM;
	}

      helper_argv[k++] = (char *) helper;
      snprintf(numbuf[nb], sizeof(numbuf[nb]), "%d", fail_fds[1]);
      helper_argv[k++] = numbuf[nb++];
      snprintf(numbuf[nb], sizeof(numbuf[nb]), "%d", pipe_count);
      helper_argv[k++] = numbuf[nb++];
      for (i = 0; i < pipe_count * 2; i++)
	{
	  snprintf(numbuf[nb], sizeof(numbuf[nb]), "%d", local_fds[i]);
	  helper_argv[k++] = numbuf[nb++];
	}
      helper_argv[k++] = (char *) (wd != NULL ? "1" : "0");
      helper_argv[k++] = (char *) (wd != NULL ? wd : "");
      helper_argv[k++] = (char *) path;
      helper_argv[k++] = (char *) "--";
      for (i = 0; i < argc; i++)
	helper_argv[k++] = commandLine[i];
      helper_argv[k] = NULL;

      pid = cp_vfork_exec(helper, helper_argv, child_env);
    }
  else
#endif /* HAVE_WORKING_VFORK */
    {
      pid = fork();

      if (pid == 0)
	{
	  close(fail_fds[0]);
	  if (fcntl(fail_fds[1], F_SETFD, FD_CLOEXEC) < 0)
	    goto child_error;

	  dup2(local_fds[0], 0);
	  dup2(local_fds[3], 1);
	  if (pipe_count == 3)
	    dup2(local_fds[5], 2);
	  else
	    dup2(1, 2);

	  cp_close_all_fds(local_fds, pipe_count * 2);

	  if (wd == NULL || chdir(wd) == 0)
	    cp_execvpe(commandLine[0], commandLine, newEnviron, path, sh_argv);

	child_error:
	  /* The fcntl, chdir or exec failed; send our errno to the parent */
	  errnum = errno;
	  while (write(fail_fds[1], &errnum, sizeof(errnum)) < 0
		 && errno == EINTR)
	    ;
	  _exit(127);
	}
    }

  /* Parent (and fork/vfork error) handling from here on. */

  if (pid == -1)
    {
      int err = errno;

      cp_close_all_fds(local_fds, pipe_count * 2);
      close(fail_fds[0]);
      close(fail_fds[1]);
      free(sh_argv);
#ifdef HAVE_WORKING_VFORK
      free(helper_argv);
#endif
      return err;
    }

  free(sh_argv);
#ifdef HAVE_WORKING_VFORK
  free(helper_argv);
#endif
  close(fail_fds[1]);

  /* Wait for the outcome of the exec: EOF if it succeeded, the
     child's errno if not */
  do
    {
      n = read(fail_fds[0], &errnum, sizeof(errnum));
    }
  while (n < 0 && errno == EINTR);
  close(fail_fds[0]);

  if (n != 0)
    {
      int status;

      if (n != (ssize_t) sizeof(errnum))
	errnum = EIO;

      /* The child exited without exec'ing; reap it */
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
	;

      cp_close_all_fds(local_fds, pipe_count * 2);
      return errnum;
    }

  close(local_fds[0]);
  close(local_fds[3]);
  if (pipe_count == 3)
    close(local_fds[5]);

  fds[0] = local_fds[1];
  fds[1] = local_fds[2];
  if (pipe_count == 3)
    fds[2] = local_fds[4];
  *out_pid = pid;
  return 0;
}

int cpproc_waitpid (pid_t pid, int *status, pid_t *outpid, int options)
{
  pid_t wp = waitpid(pid, status, options);

  if (wp < 0)
    return errno;

  *outpid = wp;
  return 0;
}

int cpproc_kill (pid_t pid, int signal)
{
  if (kill(pid, signal) < 0)
    return errno;

  return 0;
}
