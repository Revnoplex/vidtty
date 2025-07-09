AUTOMAKE_OPTIONS=foreign
bin_PROGRAMS=vidtty
vidtty_SOURCES=vidtty.c
vidtty_LDADD = $(SDL_LIBS) -lm -lcurses -lavformat -lavutil -lavcodec -lswresample
AM_CPPFLAGS = $(SDL_CFLAGS) -I/usr/include/ffmpeg/