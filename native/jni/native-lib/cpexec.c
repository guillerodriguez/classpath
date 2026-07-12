/* cpexec.c -
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

#include "config.h"
#include "cpexec.h"
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

/* PATH_MAX is not guaranteed to be defined (e.g. on GNU Hurd) */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

void cp_close_all_fds(int *fds, int numFds)
{
  int i;

  for (i = 0; i < numFds; i++)
    close(fds[i]);
}

/* Like execve, but also implementing execvp's "shell fallback"
   behaviour: if execve fails with ENOEXEC, try to execute as a
   script via /bin/sh. The shell receives the script path (file)
   followed by the original arguments minus argv[0], which is
   dropped. If envp is NULL the environment is inherited (execv is
   used instead of execve). */
void cp_execve_sh(const char *file, char * const *argv,
		  char * const *envp, char **sh_argv)
{
  if (envp != NULL)
    execve(file, argv, envp);
  else
    execv(file, argv);

  if (errno == ENOEXEC)
    {
      int i;

      sh_argv[0] = (char *) "/bin/sh";
      sh_argv[1] = (char *) file;
      for (i = 1; argv[i] != NULL; i++)
	sh_argv[i + 1] = argv[i];
      sh_argv[i + 1] = NULL;

      if (envp != NULL)
	execve("/bin/sh", sh_argv, envp);
      else
	execv("/bin/sh", sh_argv);
    }
}

/* Replacement for execvpe, which is a GNU extension and not available
   everywhere. If envp is NULL the environment is inherited. The
   supplied preallocated sh_argv array must have room for one entry
   more than argv, including its terminating NULL. */
void cp_execvpe(const char *file, char * const *argv,
		char * const *envp, const char *path,
		char **sh_argv)
{
  /* - In fork mode this runs in the child of a fork of a
       multi-threaded process, so it may only execute
       async-signal-safe operations. (In vfork mode it runs in the
       spawn helper, which is a fresh single-threaded process, so the
       constraint does not apply there.)
     - If execve fails with ENOEXEC, we assume it is a script with +x
       permission (otherwise we would have seen EACCES) but without a
       shebang line, and execute it via /bin/sh, as execvp would do.
       The fallback is implemented explicitly because execve does not
       provide it, and execvp (which does) is not async-signal-safe.
     - OpenJDK implements a similar execvpe replacement, except that
       they do use execvp in fork mode (see childproc.c). */
  char buffer[PATH_MAX];
  const char *p, *next;
  size_t filelen = strlen(file);
  int got_eacces = 0;

  /* An empty command name fails with ENOENT */
  if (*file == '\0')
    {
      errno = ENOENT;
      return;
    }

  /* Command names containing a slash are not looked up in the PATH */
  if (strchr(file, '/') != NULL)
    {
      cp_execve_sh(file, argv, envp, sh_argv);
      return;
    }

  for (p = path; p != NULL; p = next)
    {
      const char *candidate;
      const char *sep;
      size_t len;

      sep = strchr(p, ':');
      next = (sep != NULL) ? sep + 1 : NULL;
      len = (sep != NULL) ? (size_t) (sep - p) : strlen(p);
      if (len == 0)
	{
	  /* An empty PATH element means the current directory */
	  candidate = file;
	}
      else if (len + filelen + 2 <= sizeof(buffer))
	{
	  memcpy(buffer, p, len);
	  buffer[len] = '/';
	  strcpy(buffer + len + 1, file);
	  candidate = buffer;
	}
      else
	{
	  errno = ENAMETOOLONG;
	  continue;
	}

      cp_execve_sh(candidate, argv, envp, sh_argv);
      switch (errno)
	{
	case EACCES:
	  /* Keep searching, but report EACCES if nothing is found */
	  got_eacces = 1;
	  break;
	case ENOENT:
	case ENOTDIR:
#ifdef ELOOP
	case ELOOP:
#endif
#ifdef ESTALE
	case ESTALE:
#endif
#ifdef ENODEV
	case ENODEV:
#endif
#ifdef ETIMEDOUT
	case ETIMEDOUT:
#endif
	  break;
	default:
	  return;
	}
    }

  if (got_eacces)
    errno = EACCES;
}
