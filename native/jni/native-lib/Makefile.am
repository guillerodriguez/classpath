noinst_LTLIBRARIES = libclasspathnative.la
libclasspathnative_la_SOURCES = cpnet.c \
				cpnet.h \
				cpio.c \
				cpio.h \
				cpnative.h \
				cpproc.h \
				cpproc.c \
				cpexec.h \
				cpexec.c

# Helper executable used by the posix_spawn-based process spawn path
# in cpproc.c. Installed next to the native libraries; cpproc.c gets
# its location baked in via -DCP_SPAWN_HELPER. It is a plain
# standalone binary (libc only, no JNI/libtool), so it overrides the
# libtool convenience LDFLAGS used by the convenience library.
nativeexeclib_PROGRAMS = gcpspawnhelper
gcpspawnhelper_SOURCES = cpspawnhelper.c \
			 cpexec.h \
			 cpexec.c
gcpspawnhelper_CPPFLAGS = @CLASSPATH_INCLUDES@
gcpspawnhelper_CFLAGS = @WARNING_CFLAGS@ @STRICT_WARNING_CFLAGS@ @ERROR_CFLAGS@ \
			@EXTRA_CFLAGS@
gcpspawnhelper_LDFLAGS =

AM_LDFLAGS = @CLASSPATH_CONVENIENCE@
AM_CPPFLAGS = @CLASSPATH_INCLUDES@ \
	-DCP_SPAWN_HELPER="\"$(nativeexeclibdir)/`echo gcpspawnhelper | sed '$(program_transform_name)'`\""
AM_CFLAGS = @WARNING_CFLAGS@ @STRICT_WARNING_CFLAGS@ @ERROR_CFLAGS@ \
            @EXTRA_CFLAGS@
