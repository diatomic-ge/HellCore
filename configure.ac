# This file is used to generate the configure script.
# To build it, run autogen.sh in this directory.

# Make changes in configure.ac, not configure.


# The minimum autoconf version required to rebuild the configure script.
# (This is only needed on developer machines, and should be set to the lowest
# possible version.)
AC_PREREQ([2.70])


# Package name, version, bug report address, tarball name (default), and URL.
# NOTE: The HellCore version should be set here, not elsewhere.
AC_INIT([HellCore],
        [1.10.4],
        [https://github.com/diatomic-ge/HellCore],
        [],
        [https://github.com/diatomic-ge/HellCore])


# Put extra build tools in the build-aux directory to keep things tidy.
AC_CONFIG_AUX_DIR([build-aux])


# Initialize automake.
# foreign:      Tell automake not to expect the standard GNU files (NEWS,
#               AUTHORS, etc.), and not to error when it doesn't find them.
AM_INIT_AUTOMAKE([foreign])


# Keep all our autoconf macros in the m4 directory.
AC_CONFIG_MACRO_DIRS([m4])


# By default, just show what's being compiled while building, rather than the
# whole command.
# (This can be overridden by passing --disable-silent-rules to configure, or by
# running 'make V=1 [target]'.)
AM_SILENT_RULES([yes])


# This should be the relative path to a unique file in the project.
# (It's just used as a sanity check for the configure script to make sure it's
# looking in the right place.)
# If the file is renamed, moved, or deleted, adjust this line to refer to
# another uniquely named file.
AC_CONFIG_SRCDIR([src/db_verbs.c])


# The name of the header file to generate which holds all the configuration
# options specified while building, or detected by autoconf.
AC_CONFIG_HEADERS([config.h])


# These are the files we want configure to substitute values.
# For example, Makefile.in (generated automatically by automake) becomes
# Makefile.
AC_CONFIG_FILES([Makefile
                 src/Makefile])


# Check for necessary programs.

# Make sure we have a working C compiler.
AC_PROG_CC

# Make sure we have a working yacc/bison/lex compiler.
# (If a compiler isn't available but the yacc/bison/lex files are already
# compiled (e.g. as in a source tarbal), this will fail safely and set the
# timestamps as if they were compiled.)
# (The first line is so automake knows to handle yacc/bison/lex, and the second
# makes autoconf look for a yacc/bison/lex compiler.)
AM_PROG_LEX([noyywrap])
AC_PROG_YACC

# Check if we have gperf.
# (This will follow the same fail-safe behavior as noted above for
# yacc/bison/lex.)
AM_MISSING_PROG([GPERF], [gperf])

# Find a valid sed.
AC_PROG_SED



# Check for necessary libraries and headers.

# Link the math library.
AC_SEARCH_LIBS([atan2], [m])


# Figure out what crypt we have.

# Find a crypt.h.
AC_CHECK_HEADERS([crypt.h])

# Link the crypt library.
# If we don't find a valid crypt() implementation, be loud and give up.
# We can technically use the no-op crypt that's just duplicating the string, but
# failing to an insecure mode without making the user explicitly request/allow
# it feels wrong.
AC_SEARCH_LIBS([crypt],
               [crypt xcrypt],
               [],
               [AC_MSG_ERROR([cannot find a crypt implementation])])

# Give a way to disable crypt_gensalt usage, in case someone wants to stick with
# the old DES crypt.
AC_ARG_ENABLE([crypt-gensalt],
              [AS_HELP_STRING([--disable-crypt-gensalt],
                              [disable support for newer crypt algorithms])],
              [],
              [enable_crypt_gensalt=default])

# Check if we have the more modern helper function to generate a salt.
# If we do, we can get the benefits of whatever the best algorithm our crypt
# implementation has, without needing to know what algorithm it is or how we
# should generate a salt.
# If we don't find it, we fall back on the less secure DES crypt().
AC_SEARCH_LIBS([crypt_gensalt],
               [crypt xcrypt],
               [crypt_gensalt_available=yes],
               [crypt_gensalt_available=no])

# Validate our crypt_gensalt setup.
if test "x$enable_crypt_gensalt" = "xdefault"; then
  # We should try to use crypt_gensalt, but it's only a warning if we can't.
  use_crypt_gensalt="$crypt_gensalt_available"
  if test "x$crypt_gensalt_available" = "xno"; then
    # Warn that we could be using more secure salts.
    AC_MSG_WARN([using insecure DES crypt salts as crypt_gensalt is unavailable])
  fi
elif test "x$enable_crypt_gensalt" = "xyes"; then
  # They explicitly requested support, so die if we didn't find it.
  if test "x$crypt_gensalt_available" = "xno"; then
    AC_MSG_ERROR([crypt_gensalt is unavailable but newer hash salts were requested])
  fi
  use_crypt_gensalt=yes
elif test "x$enable_crypt_gensalt" = "xno"; then
  # Stick with the legacy salts.
  # We don't need to warn about crypt_gensalt being unavailable, since we were
  # asked to exclude it anyway.
  use_crypt_gensalt=no
else
  AC_MSG_ERROR([unrecognized value '$enable_crypt_gensalt' for --enable-crypt-gensalt])
fi

# If we should use crypt_gensalt, let our code know.
if test "x$use_crypt_gensalt" = "xyes"; then
AC_DEFINE([USE_CRYPT_GENSALT],
          1,
          [Define to 1 if crypt_gensalt should be used.])
fi


AX_CHECK_OPENSSL([enable_openssl_support=yes],
                 [enable_openssl_support=no])

if test "x$enable_openssl_support" = "xyes"; then
  # Since we're only building one thing, just inject the flags into our
  # variables.
  LIBS="$OPENSSL_LIBS $LIBS"
  LDFLAGS="$OPENSSL_LDFLAGS $LDFLAGS"
  CPPFLAGS="$OPENSSL_INCLUDES $CPPFLAGS"
  AC_DEFINE([USE_OPENSSL],
            1,
            [Define to 1 if OpenSSL should be used for TLS support.])
else
  AC_MSG_WARN([building without SSL/TLS support as OpenSSL is unavailable])
fi


# Generate the configure script.
# (There probably shouldn't be any autoconf things after this.)
AC_OUTPUT
