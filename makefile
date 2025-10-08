all: game

game: game.c
	gcc -o game game.c -lncurses -lpthread -lrt

clean:
	rm -f game