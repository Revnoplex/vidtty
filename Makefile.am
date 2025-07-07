AUTOMAKE_OPTIONS=foreign
bin_PROGRAMS=vidtty
vidtty_SOURCES=vidtty.c
vidtty_LDADD = $(SDL_LIBS) -lm -lncurses -lSDL3 -lavformat -lavutil -lavcodec -lswresample
AM_CPPFLAGS = $(SDL_CFLAGS)