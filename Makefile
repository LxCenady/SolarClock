CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
TARGET   = solartime

$(TARGET): main.c solar.c solar.h
	$(CC) $(CFLAGS) -o $@ main.c solar.c -lm

clean:
	rm -f $(TARGET) solar.cfg

.PHONY: clean
