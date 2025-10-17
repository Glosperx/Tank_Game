#include <stdio.h>
#include <stdlib.h>     // For exit, malloc
#include <string.h>     // For strcmp, strcpy, strlen
#include <unistd.h>     // For sleep, usleep

#include <sys/ipc.h>    // For IPC_CREAT (IPC constants)
#include <sys/shm.h>    // For shmget, shmat, shmdt (shared memory)
#include <sys/sem.h>    // For semget, semop, semctl (semaphores)
#include <signal.h>     // For signal handling (SIGINT, SIGTERM)

#include <ncurses.h>    // For ncurses library

#define SHM_KEY 0x1234           // Key for shared memory
#define SEM_KEY 0x5678           // Key for semaphores

#define MAX_HEIGHT 20            // Maximum height of the game map
#define MAX_WIDTH 20             // Maximum width of the game map

#define INITIAL_HP 5             // Initial health points for each player
#define SEM_PROJECTILE_UPDATE 400 // Special semaphore index for projectile updates

typedef struct {
    int x, y;                    // Projectile position
    int dir_x, dir_y;            // Projectile direction
    int active;                  // 1 if projectile is active, 0 otherwise
} Projectile;

typedef struct {
    char map[MAX_HEIGHT][MAX_WIDTH]; // Game map
    int height, width;               // Map dimensions

    // Player A (Player 1)
    int player1_hp;                   // Health points
    int player1_x, player1_y;         // Position
    int player1_dir_x, player1_dir_y; // Facing direction

    // Player B (Player 2)
    int player2_hp;                   // Health points
    int player2_x, player2_y;         // Position
    int player2_dir_x, player2_dir_y; // Facing direction

    int game_over;         // 1 = game has ended
    int initialized;       // 1 = game initialized by the first process

    Projectile projectiles[10];  // Array of 10 projectile slots

    // Key bindings for each player (stored in shared memory)
    char player1_keys[5];       // [up, down, left, right, fire]
    char player2_keys[5];
    int player1_registered;     // 1 if Player A has registered keys
    int player2_registered;     // 1 if Player B has registered keys

    int player1_active;    // Active status for Player 1
    int player2_active;    // Active status for Player 2
} GameState;

// Global variables
GameState *game_state = NULL;  // Pointer to shared memory
int shm_id = -1;               // Shared memory segment ID
int sem_id = -1;               // Semaphore array ID
char player_id;                // 'A' or 'B' (ID of this process)
char map_file[256];            // Path to the map file
int should_cleanup = 0;        // 1 = this process should clean up IPC resources

// Calculate semaphore index for a position (y, x)
// e.g., Position (5, 7) -> Semaphore 107 (5 * 20 + 7)
int get_sem_index(int y, int x) {
    return y * MAX_WIDTH + x;
}

// Lock a position (y, x)
void lock_position(int y, int x) {
    // Check bounds
    if (y < 0 || y >= MAX_HEIGHT || x < 0 || x >= MAX_WIDTH)
        return;

    struct sembuf op;
    op.sem_num = get_sem_index(y, x);  // Which semaphore?
    op.sem_op = -1;                    // Operation: decrement by 1
    op.sem_flg = 0;                    // Flags: 0 = wait if locked
    semop(sem_id, &op, 1);             // Perform the operation
}

// Unlock a position (y, x)
void unlock_position(int y, int x) {
    if (y < 0 || y >= MAX_HEIGHT || x < 0 || x >= MAX_WIDTH)
        return;

    struct sembuf op;
    op.sem_num = get_sem_index(y, x);
    // Similar to lock_position, but:
    op.sem_op = 1;  // Increment by 1 (release)
    op.sem_flg = 0;
    semop(sem_id, &op, 1);
}

// Attempt to lock the projectile update semaphore
int try_lock_projectile_update() {
    struct sembuf op;
    op.sem_num = SEM_PROJECTILE_UPDATE;
    op.sem_op = -1;
    op.sem_flg = IPC_NOWAIT; // Return immediately
    // Returns 1 if successful
    return (semop(sem_id, &op, 1) == 0);
}

// Unlock the projectile update semaphore
void unlock_projectile_update() {
    struct sembuf op;
    op.sem_num = SEM_PROJECTILE_UPDATE;
    op.sem_op = 1;
    op.sem_flg = 0;
    semop(sem_id, &op, 1);
}

// Clean up shared memory
void cleanup_shared_memory() {
    if (game_state != NULL) {
        shmdt(game_state);              // Detach shared memory
        if (should_cleanup) {
            shmctl(shm_id, IPC_RMID, NULL);  // Delete the segment
        }
    }
}

// Clean up semaphores
void cleanup_semaphores() {
    if (sem_id >= 0 && should_cleanup) {
        semctl(sem_id, 0, IPC_RMID);    // Delete the semaphore array
    }
}

// General cleanup function
void cleanup() {
    endwin();  // Close ncurses
    cleanup_shared_memory();
    cleanup_semaphores();
}

// Handle Ctrl+C
void signal_handler(int signo) {
    if (player_id == 'A') {
        game_state->player1_active = 0;
        game_state->game_over = 1;  // Mark the game as over
    } else if (player_id == 'B') {
        game_state->player2_active = 0;
        game_state->game_over = 1;  // Mark the game as over
    }

    // Set cleanup only if both players are inactive
    if (!game_state->player1_active && !game_state->player2_active) {
        should_cleanup = 1;
    }

    cleanup();
    exit(0);
}

// Load the map from a file
int load_map() {
    FILE *f = fopen(map_file, "r");
    if (!f) {
        perror(map_file);
        return 0;
    }

    game_state->height = 0;
    game_state->width = 0;

    char line[MAX_WIDTH + 2];
    while (fgets(line, sizeof(line), f) && game_state->height < MAX_HEIGHT) {
        int len = strlen(line);

        // Remove trailing '\n'
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }

        // Pad the line with spaces up to MAX_WIDTH
        for (int i = len; i < MAX_WIDTH; i++) {
            line[i] = ' ';
        }
        line[MAX_WIDTH] = '\0';  // Ensure proper string termination

        // Copy the line to the map
        memcpy(game_state->map[game_state->height], line, MAX_WIDTH);

        // Update maximum width
        if (len > game_state->width)
            game_state->width = len;

        game_state->height++;
    }

    fclose(f);
    return 1;
}

// Initialize the game
void init_game() {
    if (!load_map()) {
        fprintf(stderr, "Error loading map\n");
        exit(1);
    }

    // Set initial health points
    game_state->player1_hp = INITIAL_HP;
    game_state->player2_hp = INITIAL_HP;

    // Initial direction: up
    game_state->player1_dir_x = 0;
    game_state->player1_dir_y = 1;
    game_state->player2_dir_x = 0;
    game_state->player2_dir_y = 1;

    // Starting positions
    game_state->player1_x = 2;
    game_state->player1_y = 2;
    game_state->player2_x = 17;
    game_state->player2_y = 7;

    // Deactivate all projectiles
    for (int i = 0; i < 10; i++)
        game_state->projectiles[i].active = 0;

    // Reset flags
    game_state->player1_registered = 0;
    game_state->player2_registered = 0;
    game_state->game_over = 0;
    game_state->initialized = 1;  // Mark the game as initialized

    game_state->player1_active = 0;
    game_state->player2_active = 0;
}

// Draw the game state
void draw_game() {
    clear();  // Clear the screen

    // Draw the map
    for (int i = 0; i < game_state->height; i++) {
        for (int j = 0; j < game_state->width; j++) {
            char c = game_state->map[i][j];  // Map character

            // Override with players
            if (i == game_state->player1_y && j == game_state->player1_x)
                c = 'A';
            else if (i == game_state->player2_y && j == game_state->player2_x)
                c = 'B';
            else {
                // Check for projectiles
                for (int p = 0; p < 10; p++) {
                    if (game_state->projectiles[p].active &&
                        game_state->projectiles[p].y == i &&
                        game_state->projectiles[p].x == j) {
                        c = '.';
                        break;
                    }
                }
            }
            // Draw the character at position (i, j)
            mvaddch(i, j, c);
        }
    }

    // Display stats on the right side of the map
    mvprintw(0, game_state->width + 2, "Player A: %d HP", game_state->player1_hp);
    mvprintw(1, game_state->width + 2, "Player B: %d HP", game_state->player2_hp);
    mvprintw(3, game_state->width + 2, "You are: Player %c", player_id);
    mvprintw(5, game_state->width + 2, "Controls:");

    // Display key bindings from shared memory
    if (game_state->player1_registered) {
        mvprintw(6, game_state->width + 2, "A: %c/%c/%c/%c/%c",
                 game_state->player1_keys[0],
                 game_state->player1_keys[1],
                 game_state->player1_keys[2],
                 game_state->player1_keys[3],
                 game_state->player1_keys[4] == ' ' ? 'S' : game_state->player1_keys[4]);
    } else {
        mvprintw(6, game_state->width + 2, "A: waiting...");
    }

    if (game_state->player2_registered) {
        mvprintw(7, game_state->width + 2, "B: %c/%c/%c/%c/%c",
                 game_state->player2_keys[0],  // up
                 game_state->player2_keys[1],  // down
                 game_state->player2_keys[2],  // left
                 game_state->player2_keys[3],  // right
                 game_state->player2_keys[4] == ' ' ? 'S' : game_state->player2_keys[4]);
    } else {
        mvprintw(7, game_state->width + 2, "B: waiting...");
    }

    // Display Game Over message
    if (game_state->game_over) {
        char winner = (game_state->player1_hp > 0) ? 'A' : 'B';
        mvprintw(game_state->height / 2, game_state->width / 2 - 10,
                 "GAME OVER! Player %c wins!", winner);
    }

    // Refresh the screen
    refresh();
}

// Move a player
void move_player(char which_player, int dx, int dy) {
    // Determine pointers to player data
    int *px, *py, *dir_x, *dir_y;

    if (which_player == 'A') {
        px = &game_state->player1_x;
        py = &game_state->player1_y;
        dir_x = &game_state->player1_dir_x;
        dir_y = &game_state->player1_dir_y;
    } else {
        px = &game_state->player2_x;
        py = &game_state->player2_y;
        dir_x = &game_state->player2_dir_x;
        dir_y = &game_state->player2_dir_y;
    }

    // Calculate new position
    int old_x = *px;
    int old_y = *py;
    int new_x = old_x + dx;
    int new_y = old_y + dy;

    // Check map bounds
    if (new_x < 0 || new_x >= game_state->width ||
        new_y < 0 || new_y >= game_state->height)
        return;

    // Lock both positions
    lock_position(old_y, old_x);
    lock_position(new_y, new_x);

    // Check if the position is free
    char cell = game_state->map[new_y][new_x];

    if (cell == ' ' && // Free space
        // Not occupied by the other player
        !(new_y == game_state->player1_y && new_x == game_state->player1_x && which_player != 'A') &&
        !(new_y == game_state->player2_y && new_x == game_state->player2_x && which_player != 'B')) {
        // Move player
        *px = new_x;
        *py = new_y;
        *dir_x = dx;    // Update direction
        *dir_y = dy;
    }

    // Unlock positions
    unlock_position(new_y, new_x);
    unlock_position(old_y, old_x);
}

// Fire a projectile
void fire_projectile(char which_player) {
    // Determine starting coordinates and direction of the projectile
    int start_x, start_y, proj_dir_x, proj_dir_y;

    if (which_player == 'A') {
        start_x = game_state->player1_x;
        start_y = game_state->player1_y;
        proj_dir_x = game_state->player1_dir_x;
        proj_dir_y = game_state->player1_dir_y;
    } else {
        start_x = game_state->player2_x;
        start_y = game_state->player2_y;
        proj_dir_x = game_state->player2_dir_x;
        proj_dir_y = game_state->player2_dir_y;
    }

    // Initial projectile position (in front of the player)
    int proj_x = start_x + proj_dir_x;
    int proj_y = start_y + proj_dir_y;

    // Check map bounds
    if (proj_x < 0 || proj_x >= game_state->width ||
        proj_y < 0 || proj_y >= game_state->height)
        return;

    lock_position(proj_y, proj_x);

    // Check for an available projectile slot
    int slot = -1;
    for (int i = 0; i < 10; i++) {
        if (!game_state->projectiles[i].active) {
            slot = i;
            break;
        }
    }

    // If a slot is available, initialize the projectile
    if (slot != -1) {
        game_state->projectiles[slot].x = proj_x;
        game_state->projectiles[slot].y = proj_y;
        game_state->projectiles[slot].dir_x = proj_dir_x;
        game_state->projectiles[slot].dir_y = proj_dir_y;
        game_state->projectiles[slot].active = 1;
    }

    unlock_position(proj_y, proj_x);
}

// Update projectile positions
void update_projectiles() {
    int next_positions[10][2]; // next_positions[i] = {next_x, next_y}
    int to_deactivate[10] = {0}; // 1 if projectile i should be deactivated

    // Calculate next positions
    for (int i = 0; i < 10; i++) {
        if (!game_state->projectiles[i].active) {
            next_positions[i][0] = -1;
            next_positions[i][1] = -1;
            continue;
        }

        int proj_x = game_state->projectiles[i].x;
        int proj_y = game_state->projectiles[i].y;
        int dir_x = game_state->projectiles[i].dir_x;
        int dir_y = game_state->projectiles[i].dir_y;

        // Calculate new position
        next_positions[i][0] = proj_x + dir_x;
        next_positions[i][1] = proj_y + dir_y;
    }

    // Check for projectile collisions
    // Check all pairs of projectiles
    for (int i = 0; i < 10; i++) {
        if (!game_state->projectiles[i].active || to_deactivate[i])
            continue;

        for (int j = i + 1; j < 10; j++) {
            if (!game_state->projectiles[j].active || to_deactivate[j])
                continue;

            // Direct collision - Both projectiles reach the same position
            if (next_positions[i][0] == next_positions[j][0] &&
                next_positions[i][1] == next_positions[j][1]) {
                to_deactivate[i] = 1;
                to_deactivate[j] = 1;
                continue;
            }

            // Indirect collision - Projectiles cross paths
            if (next_positions[i][0] == game_state->projectiles[j].x &&
                next_positions[i][1] == game_state->projectiles[j].y &&
                next_positions[j][0] == game_state->projectiles[i].x &&
                next_positions[j][1] == game_state->projectiles[i].y) {
                to_deactivate[i] = 1;
                to_deactivate[j] = 1;
            }
        }
    }

    // Update projectile positions
    for (int i = 0; i < 10; i++) {
        if (!game_state->projectiles[i].active)
            continue;

        // Current and next positions
        int proj_x = game_state->projectiles[i].x;
        int proj_y = game_state->projectiles[i].y;
        int next_x = next_positions[i][0];
        int next_y = next_positions[i][1];

        // If projectile should be deactivated (collision)
        if (to_deactivate[i]) {
            // Deactivate the projectile
            lock_position(proj_y, proj_x);
            game_state->projectiles[i].active = 0;
            unlock_position(proj_y, proj_x);
            continue;
        }

        // Check map bounds
        if (next_x < 0 || next_x >= game_state->width ||
            next_y < 0 || next_y >= game_state->height) {
            lock_position(proj_y, proj_x);
            game_state->projectiles[i].active = 0;
            unlock_position(proj_y, proj_x);
            continue;
        }

        lock_position(proj_y, proj_x);
        lock_position(next_y, next_x);

        // Check collision with walls
        if (game_state->map[next_y][next_x] == '#') {
            game_state->projectiles[i].active = 0;
            unlock_position(next_y, next_x);
            unlock_position(proj_y, proj_x);
            continue;
        }

        // Check collision with players
        // Collision with Player 1
        if (next_y == game_state->player1_y && next_x == game_state->player1_x) {
            game_state->player1_hp--;
            if (game_state->player1_hp <= 0)
                game_state->game_over = 1;
            game_state->projectiles[i].active = 0;
            unlock_position(next_y, next_x);
            unlock_position(proj_y, proj_x);
            continue;
        }

        // Collision with Player 2
        if (next_y == game_state->player2_y && next_x == game_state->player2_x) {
            game_state->player2_hp--;
            if (game_state->player2_hp <= 0)
                game_state->game_over = 1;
            game_state->projectiles[i].active = 0;
            unlock_position(next_y, next_x);
            unlock_position(proj_y, proj_x);
            continue;
        }

        // Move the projectile to the new position
        game_state->projectiles[i].x = next_x;
        game_state->projectiles[i].y = next_y;

        unlock_position(next_y, next_x);
        unlock_position(proj_y, proj_x);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 8) {
        fprintf(stderr, "Usage: %s <map_file> <player_id> ", argv[0]);
        fprintf(stderr, "<up> <down> <left> <right> <fire>\n");
        fprintf(stderr, "Example A: %s map.txt A w s a d f\n", argv[0]);
        fprintf(stderr, "Example B: %s map.txt B i k j l space\n", argv[0]);
        return 1;
    }

    strcpy(map_file, argv[1]);  // Copy map file path
    player_id = argv[2][0];     // 'A' or 'B'

    // Read key bindings from command line
    char my_keys[5];
    my_keys[0] = argv[3][0];  // up
    my_keys[1] = argv[4][0];  // down
    my_keys[2] = argv[5][0];  // left
    my_keys[3] = argv[6][0];  // right
    my_keys[4] = (strcmp(argv[7], "space") == 0) ? ' ' : argv[7][0];  // fire

    signal(SIGINT, signal_handler);  // Handle Ctrl+C
    signal(SIGTERM, signal_handler); // Handle kill
    atexit(cleanup);                // Call cleanup() on exit

    // Create/attach shared memory
    shm_id = shmget(SHM_KEY, sizeof(GameState), IPC_CREAT | 0666);
    if (shm_id < 0) {
        perror("shmget failed");
        return 1;
    }

    // Attach shared memory
    game_state = (GameState *)shmat(shm_id, NULL, 0);
    if (game_state == (void *)-1) {
        perror("shmat failed");
        return 1;
    }

    // Check number of attachments AFTER attaching
    struct shmid_ds shm_info;
    if (shmctl(shm_id, IPC_STAT, &shm_info) == -1) {
        perror("shmctl failed");
        shmdt(game_state);
        return 1;
    }

    // Check if this is the first process (shm_nattch == 1)
    if (shm_info.shm_nattch == 1) {
        init_game();

        // Create semaphores
        int num_sems = MAX_HEIGHT * MAX_WIDTH + 1;
        sem_id = semget(SEM_KEY, num_sems, IPC_CREAT | 0666);
        if (sem_id < 0) {
            perror("semget failed");
            should_cleanup = 1;
            cleanup_shared_memory();
            return 1;
        }

        // Initialize semaphores
        for (int i = 0; i < num_sems; i++) {
            semctl(sem_id, i, SETVAL, 1);
        }

        printf("Game initialized with %d semaphores\n", num_sems);
    } else {
        // Second process attaches to existing semaphores
        sem_id = semget(SEM_KEY, MAX_HEIGHT * MAX_WIDTH + 1, 0666);
        if (sem_id < 0) {
            perror("semget failed");
            shmdt(game_state);
            return 1;
        }
    }

    // Register key bindings in shared memory
    if (player_id == 'A') {
        for (int i = 0; i < 5; i++)
            game_state->player1_keys[i] = my_keys[i];
        game_state->player1_registered = 1;
        game_state->player1_active = 1;
    } else if (player_id == 'B') {
        for (int i = 0; i < 5; i++)
            game_state->player2_keys[i] = my_keys[i];
        game_state->player2_registered = 1;
        game_state->player2_active = 1;
    }

    // Initialize ncurses
    initscr();              // Start ncurses mode
    cbreak();               // Disable line buffering
    noecho();               // Don't echo keypresses
    nodelay(stdscr, TRUE);  // Make getch() non-blocking
    keypad(stdscr, TRUE);   // Enable special keys (arrows, etc.)
    curs_set(0);            // Hide the cursor

    int frame_counter = 0;
    while (!game_state->game_over) {
        // Read a key
        int ch = getch(); // Returns key code or -1

        // Check keys for Player A (if registered)
        if (game_state->player1_registered) {
            if (ch == game_state->player1_keys[0]) {
                move_player('A', 0, -1);  // up
            } else if (ch == game_state->player1_keys[1]) {
                move_player('A', 0, 1);   // down
            } else if (ch == game_state->player1_keys[2]) {
                move_player('A', -1, 0);  // left
            } else if (ch == game_state->player1_keys[3]) {
                move_player('A', 1, 0);   // right
            } else if (ch == game_state->player1_keys[4]) {
                fire_projectile('A');     // fire
            }
        }

        // Check keys for Player B (if registered)
        if (game_state->player2_registered) {
            if (ch == game_state->player2_keys[0]) {
                move_player('B', 0, -1);  // up
            } else if (ch == game_state->player2_keys[1]) {
                move_player('B', 0, 1);   // down
            } else if (ch == game_state->player2_keys[2]) {
                move_player('B', -1, 0);  // left
            } else if (ch == game_state->player2_keys[3]) {
                move_player('B', 1, 0);   // right
            } else if (ch == game_state->player2_keys[4]) {
                fire_projectile('B');     // fire
            }
        }

        // Quit game
        if (ch == 'q' || ch == 'Q') {
            game_state->game_over = 1;
        }

        // Update projectiles with global semaphore
        frame_counter++;
        if (frame_counter % 2 == 0) { // Every 2 frames
            // Only one process can update projectiles
            if (try_lock_projectile_update()) {
                update_projectiles();
                unlock_projectile_update();
            }
            // If this process fails, the other will handle it
        }

        // Draw everything
        draw_game();
        // Delay (30ms = ~33 FPS)
        usleep(30000);
    }

    if (game_state->game_over) {
        draw_game(); // Display final screen
        sleep(3);   // Wait 3 seconds
        should_cleanup = 1; // Mark for IPC resource cleanup
    }

    return 0; // cleanup() is called automatically (atexit)
}