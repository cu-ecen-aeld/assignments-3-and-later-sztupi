TARGET = aesdsocket
HEADERS =
OBJECTS = aesdsocket.o
LDFLAGS += -pthread
USE_AESD_CHAR_DEVICE ?= 1

ifeq ($(USE_AESD_CHAR_DEVICE),1)
CCFLAGS += -DUSE_AESD_CHAR_DEVICE
endif

.PHONY: default all clean

default: $(TARGET)
all: default

%.o: %.c $(HEADERS)
	$(CC) $(CCFLAGS) -c $< -o $@

.PRECIOUS: $(TARGET) $(OBJECTS)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $(TARGET)

clean:
	-rm -f writer $(OBJECTS)
