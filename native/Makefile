CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra -fPIC
LDFLAGS  ?= -shared
LIBS     ?= -ljpeg

TARGET = embr-canvas.so

all: $(TARGET)

$(TARGET): embr-canvas.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
