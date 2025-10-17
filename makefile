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
	@ipcs -m | grep $(shell id -u) | awk '{print $$2}' | xargs -r ipcrm -m 2>/dev/null || true
	@ipcs -s | grep $(shell id -u) | awk '{print $$2}' | xargs -r ipcrm -s 2>/dev/null || true
	@echo "Cleared IPC resources."

cleanall: clean

# Simple launch - each player enters only their own keys
run1:
	./$(TARGET) map.txt A w s a d f

run2:
	./$(TARGET) map.txt B i k j l space

# Alternative with different keys
runA:
	./$(TARGET) map.txt A w s a d space

runB:
	./$(TARGET) map.txt B 8 5 4 6 0

.PHONY: all clean cleanall run1 run2 runA runB
