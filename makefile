CC = gcc
CFLAGS = -Wall -Wextra -g
LIBS = -lncurses -lpthread

TARGET = game
SOURCES = game.c
OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)
	@echo "Cleaning IPC resources..."
	@ipcs -m | grep $(shell id -u) | awk '{print $2}' | xargs -r ipcrm -m 2>/dev/null || true
	@ipcs -s | grep $(shell id -u) | awk '{print $2}' | xargs -r ipcrm -s 2>/dev/null || true
	@echo "Cleared IPC resources."

cleanall: clean

run1:
	./$(TARGET) harta.txt 1 w s a d f i k j l space

run2:
	./$(TARGET) harta.txt 2 w s a d f i k j l space

.PHONY: all clean run1 run2