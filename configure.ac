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


# Initialize automake.
# foreign:      Tell automake not to expect the standard GNU files (NEWS,
#               AUTHORS, etc.), and not to error when it doesn't find them.
AM_INIT_AUTOMAKE([foreign])

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

# Check if we have the more modern helper function to generate a salt.
# If we do, we can get the benefits of whatever the best algorithm our crypt
# implementation has, without needing to know what algorithm it is or how we
# should generate a salt.
# If we don't find it, we fall back on the less secure DES crypt().
AC_SEARCH_LIBS([crypt_gensalt],
               [crypt xcrypt],
               [AC_DEFINE([HAVE_CRYPT_GENSALT],
                          1,
                          [Define to 1 if crypt_gensalt is available.])],
               [AC_MSG_WARN([using insecure DES crypt salts as crypt_gensalt is unavailable])])


# Generate the configure script.
# (There probably shouldn't be any autoconf things after this.)
AC_OUTPUT
