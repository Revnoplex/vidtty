AC_INIT([vidtty], [2.0.0b1], [revnoplex.business@protonmail.com])
AM_INIT_AUTOMAKE
AC_PROG_CC

PKG_PROG_PKG_CONFIG

PKG_CHECK_MODULES([SDL], [sdl3], [
    AC_DEFINE([HAVE_SDL3], [1], [Define if using SDL3])
], [
    PKG_CHECK_MODULES([SDL], [sdl2], [
        AC_DEFINE([HAVE_SDL2], [1], [Define if using SDL2])
    ], [
        AC_MSG_ERROR([Neither SDL3 nor SDL2 could be found])
    ])
])

CFLAGS="$CFLAGS $SDL_CFLAGS"
LIBS="$LIBS $SDL_LIBS"

AC_CONFIG_FILES([Makefile])
AC_OUTPUT