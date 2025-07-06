LDFLAGS=-lm -lncurses -lSDL3 -lavformat -lavutil -lavcodec -lswresample
DEBUG_FLAGS=-g -fsanitize=address -fsanitize=undefined
TEST_FLAGS=-c -o /dev/null $(LDFLAGS) -O -Wall -Wextra

vidtty:
	mkdir -p build
	cc -o build/vidtty $(LDFLAGS) vidtty.c

vidtty_debug:
	mkdir -p build
	cc -o build/vidtty-debug $(LDFLAGS) $(DEBUG_FLAGS) vidtty.c

vidtty_test:
	cc $(TEST_FLAGS) vidtty.c

vidtty_debug_test: vidtty_test vidtty_debug
