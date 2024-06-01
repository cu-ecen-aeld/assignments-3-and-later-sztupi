TARGET=aesdsocket
CC=${CROSS_COMPILE}gcc
HEADERS=
OBJECTS=aesdsocket.o
LDFLAGS=-pthread

.PHONY: default all clean

default: $(TARGET)
all: default

%.o: %.c $(HEADERS)
	$(CC) -c $< -o $@

.PRECIOUS: $(TARGET) $(OBJECTS)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $(TARGET)

clean:
	-rm -f writer $(OBJECTS)
