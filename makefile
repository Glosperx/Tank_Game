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

# Lansare simplificată - fiecare jucător introduce doar propriile taste
run1:
	./$(TARGET) harta.txt A w s a d f

run2:
	./$(TARGET) harta.txt B i k j l space

# Alternative cu taste diferite
runA_arrows:
	./$(TARGET) harta.txt A w s a d space

runB_numpad:
	./$(TARGET) harta.txt B 8 5 4 6 0

# Test: lansează ambele în fundal (pentru debugging)
test:
	@echo "Lansez procesul A în fundal..."
	@xterm -e "./$(TARGET) harta.txt A w s a d f" &
	@sleep 1
	@echo "Lansez procesul B în fundal..."
	@xterm -e "./$(TARGET) harta.txt B i k j l space" &

# Informații despre IPC resources
ipc-status:
	@echo "=== Shared Memory Segments ==="
	@ipcs -m | grep $(shell id -u) || echo "None"
	@echo ""
	@echo "=== Semaphore Arrays ==="
	@ipcs -s | grep $(shell id -u) || echo "None"

.PHONY: all clean cleanall run1 run2 runA_arrows runB_numpad test ipc-status