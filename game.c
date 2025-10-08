#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncurses.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>
#include <sys/types.h>

#define MAP_ROWS 10
#define MAP_COLS 20
#define SHM_NAME "/tank_game"
#define SEM_PREFIX "/tank_sem_"

typedef struct {
    char map[MAP_ROWS][MAP_COLS];
    int player1_x;
    int player1_y;
    int player2_x;
    int player2_y;
} GameState;

sem_t *grid_semaphores[MAP_ROWS][MAP_COLS];
GameState *shared_state;
int shm_fd;

void init_semaphores() {
    char sem_name[50];
    for (int i = 0; i < MAP_ROWS; i++) {
        for (int j = 0; j < MAP_COLS; j++) {
            sprintf(sem_name, "%s%d_%d", SEM_PREFIX, i, j);
            grid_semaphores[i][j] = sem_open(sem_name, O_CREAT, 0644, 1);
            if (grid_semaphores[i][j] == SEM_FAILED) {
                perror("sem_open failed");
                exit(1);
            }
        }
    }
}

void cleanup_semaphores() {
    char sem_name[50];
    for (int i = 0; i < MAP_ROWS; i++) {
        for (int j = 0; j < MAP_COLS; j++) {
            sprintf(sem_name, "%s%d_%d", SEM_PREFIX, i, j);
            sem_close(grid_semaphores[i][j]);
            sem_unlink(sem_name);
        }
    }
}

void init_shared_memory() {
    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0644);
    if (shm_fd == -1) {
        perror("shm_open failed");
        exit(1);
    }

    if (ftruncate(shm_fd, sizeof(GameState)) == -1) {
        perror("ftruncate failed");
        exit(1);
    }

    shared_state = mmap(NULL, sizeof(GameState), PROT_READ | PROT_WRITE, 
                       MAP_SHARED, shm_fd, 0);
    if (shared_state == MAP_FAILED) {
        perror("mmap failed");
        exit(1);
    }
}

void read_map() {
    FILE *fp = fopen("harta.txt", "r");
    if (fp == NULL) {
        perror("Failed to open map file");
        exit(1);
    }

    char line[MAP_COLS + 2]; // +2 for newline and null terminator
    for (int i = 0; i < MAP_ROWS; i++) {
        if (fgets(line, sizeof(line), fp)) {
            strncpy(shared_state->map[i], line, MAP_COLS);
        }
    }

    fclose(fp);
}

void init_players() {
    // Hardcoded initial positions for players
    shared_state->player1_x = 1;
    shared_state->player1_y = 1;
    shared_state->player2_x = MAP_COLS - 2;
    shared_state->player2_y = MAP_ROWS - 2;
}

void display_game() {
    clear();
    
    for (int i = 0; i < MAP_ROWS; i++) {
        for (int j = 0; j < MAP_COLS; j++) {
            if (i == shared_state->player1_y && j == shared_state->player1_x) {
                mvaddch(i, j, 'A');
            } else if (i == shared_state->player2_y && j == shared_state->player2_x) {
                mvaddch(i, j, 'B');
            } else {
                mvaddch(i, j, shared_state->map[i][j]);
            }
        }
    }
    
    refresh();
}

int main(int argc, char *argv[]) {
    // Initialize ncurses
    initscr();
    noecho();
    curs_set(0);

    // Initialize game components
    init_shared_memory();
    init_semaphores();
    read_map();
    init_players();

    // Display initial game state
    display_game();
    
    // Wait for a key press before exiting
    getch();

    // Cleanup
    endwin();
    cleanup_semaphores();
    munmap(shared_state, sizeof(GameState));
    shm_unlink(SHM_NAME);
    close(shm_fd);

    return 0;
}