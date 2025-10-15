#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <ncurses.h>
#include <signal.h>

#define SHM_KEY 0x1234
#define SEM_KEY 0x5678
#define MAX_HEIGHT 20
#define MAX_WIDTH 20
#define INITIAL_HP 5

// Structura unui proiectil
typedef struct {
    int x, y;
    int dir_x, dir_y;
    int active;
} Projectile;

// Starea jocului (in memoria partajata)
typedef struct {
    char map[MAX_HEIGHT][MAX_WIDTH];
    int height, width;
    int player1_hp, player1_x, player1_y, player1_dir_x, player1_dir_y;
    int player2_hp, player2_x, player2_y, player2_dir_x, player2_dir_y;
    int game_over, initialized;
    Projectile projectiles[10];
} GameState;

GameState *game_state = NULL;
int shm_id = -1;
int sem_id = -1;
char player_id;
char map_file[256];
int should_cleanup = 0;

char key_a_up, key_a_down, key_a_left, key_a_right, key_a_fire;
char key_b_up, key_b_down, key_b_left, key_b_right, key_b_fire;

// Calculeaza indexul semaforului pentru pozitia (y, x)
int get_sem_index(int y, int x) {
    return y * MAX_WIDTH + x;
}

// Blocheaza pozitia (y, x)
void lock_position(int y, int x) {
    if (y < 0 || y >= MAX_HEIGHT || x < 0 || x >= MAX_WIDTH)
        return;
    
    struct sembuf op;
    op.sem_num = get_sem_index(y, x);
    op.sem_op = -1;
    op.sem_flg = 0;
    semop(sem_id, &op, 1);
}

// Deblocheaza pozitia (y, x)
void unlock_position(int y, int x) {
    if (y < 0 || y >= MAX_HEIGHT || x < 0 || x >= MAX_WIDTH)
        return;
    
    struct sembuf op;
    op.sem_num = get_sem_index(y, x);
    op.sem_op = 1;
    op.sem_flg = 0;
    semop(sem_id, &op, 1);
}

void cleanup() {
    if (game_state != NULL) {
        endwin();
        shmdt(game_state);
        if (should_cleanup) {
            shmctl(shm_id, IPC_RMID, NULL);
            semctl(sem_id, 0, IPC_RMID);
        }
    }
}

void signal_handler(int signo) {
    cleanup();
    exit(0);
}

int load_map() {
    FILE *f = fopen(map_file, "r");
    if (!f) {
        perror("Eroare la deschiderea hartii");
        return 0;
    }

    game_state->height = 0;
    game_state->width = 0;
    
    char line[MAX_WIDTH + 2];
    while (fgets(line, sizeof(line), f) && game_state->height < MAX_HEIGHT) {
        int len = strlen(line);
        if (line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }
        strcpy(game_state->map[game_state->height], line);
        if (len > game_state->width)
            game_state->width = len;
        game_state->height++;
    }
    
    fclose(f);
    return 1;
}

void init_game() {
    if (!load_map()) {
        fprintf(stderr, "Eroare la incarcarea hartii\n");
        exit(1);
    }

    game_state->player1_hp = INITIAL_HP;
    game_state->player2_hp = INITIAL_HP;
    
    // Directia initiala: sus (dir_y = 1 inseamna sus acum)
    game_state->player1_dir_x = 0;
    game_state->player1_dir_y = 1;
    game_state->player2_dir_x = 0;
    game_state->player2_dir_y = 1;

    // Pozitii de spawn
    game_state->player1_x = 2;
    game_state->player1_y = 2;
    game_state->player2_x = 17;
    game_state->player2_y = 7;

    for (int i = 0; i < 10; i++)
        game_state->projectiles[i].active = 0;
    
    game_state->game_over = 0;
    game_state->initialized = 1;
}

void draw_game() {
    clear();

    for (int i = 0; i < game_state->height; i++) {
        for (int j = 0; j < game_state->width; j++) {
            char c = game_state->map[i][j];
            
            if (i == game_state->player1_y && j == game_state->player1_x)
                c = 'A';
            else if (i == game_state->player2_y && j == game_state->player2_x)
                c = 'B';
            else {
                for (int p = 0; p < 10; p++) {
                    if (game_state->projectiles[p].active && 
                        game_state->projectiles[p].y == i && 
                        game_state->projectiles[p].x == j) {
                        c = '.';
                        break;
                    }
                }
            }
            mvaddch(i, j, c);
        }
    }

    mvprintw(0, game_state->width + 2, "Player A: %d HP", game_state->player1_hp);
    mvprintw(1, game_state->width + 2, "Player B: %d HP", game_state->player2_hp);
    mvprintw(3, game_state->width + 2, "You are: Player %c", player_id);
    mvprintw(5, game_state->width + 2, "Controls:");
    mvprintw(6, game_state->width + 2, "A: %c/%c/%c/%c/%c", 
             key_a_up, key_a_down, key_a_left, key_a_right, 
             key_a_fire == ' ' ? 'S' : key_a_fire);
    mvprintw(7, game_state->width + 2, "B: %c/%c/%c/%c/%c", 
             key_b_up, key_b_down, key_b_left, key_b_right,
             key_b_fire == ' ' ? 'S' : key_b_fire);
    
    if (game_state->game_over) {
        char winner = (game_state->player1_hp > 0) ? 'A' : 'B';
        mvprintw(game_state->height / 2, game_state->width / 2 - 10, 
                 "GAME OVER! Player %c wins!", winner);
    }

    refresh();
}

void move_player(char which_player, int dx, int dy) {
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

    int old_x = *px;
    int old_y = *py;
    int new_x = old_x + dx;
    int new_y = old_y + dy;

    if (new_x < 0 || new_x >= game_state->width || 
        new_y < 0 || new_y >= game_state->height)
        return;

    lock_position(old_y, old_x);
    lock_position(new_y, new_x);
    
    char cell = game_state->map[new_y][new_x];
    
    // Verifica daca pozitia e libera
    if (cell == ' ' && 
        !(new_y == game_state->player1_y && new_x == game_state->player1_x && which_player != 'A') &&
        !(new_y == game_state->player2_y && new_x == game_state->player2_x && which_player != 'B')) {
        *px = new_x;
        *py = new_y;
        *dir_x = dx;
        *dir_y = dy;
    }
    
    unlock_position(new_y, new_x);
    unlock_position(old_y, old_x);
}

void fire_projectile(char which_player) {
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

    int proj_x = start_x + proj_dir_x;
    int proj_y = start_y + proj_dir_y;
    
    if (proj_x < 0 || proj_x >= game_state->width || 
        proj_y < 0 || proj_y >= game_state->height)
        return;
    
    lock_position(proj_y, proj_x);

    // Gaseste slot liber pentru proiectil
    int slot = -1;
    for (int i = 0; i < 10; i++) {
        if (!game_state->projectiles[i].active) {
            slot = i;
            break;
        }
    }
    
    if (slot != -1) {
        game_state->projectiles[slot].x = proj_x;
        game_state->projectiles[slot].y = proj_y;
        game_state->projectiles[slot].dir_x = proj_dir_x;
        game_state->projectiles[slot].dir_y = proj_dir_y;
        game_state->projectiles[slot].active = 1;
    }
    
    unlock_position(proj_y, proj_x);
}

void update_projectiles() {
    // Prima trecere: calculeaza noile pozitii
    int next_positions[10][2]; // [i][0] = next_x, [i][1] = next_y
    int to_deactivate[10] = {0}; // marcheaza proiectilele care trebuie dezactivate
    
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
        
        next_positions[i][0] = proj_x + dir_x;
        next_positions[i][1] = proj_y + dir_y;
    }
    
    // A doua trecere: verifica coliziuni intre proiectile
    for (int i = 0; i < 10; i++) {
        if (!game_state->projectiles[i].active || to_deactivate[i])
            continue;
            
        for (int j = i + 1; j < 10; j++) {
            if (!game_state->projectiles[j].active || to_deactivate[j])
                continue;
            
            // Cazul 1: Ambele proiectile ajung in aceeasi pozitie
            if (next_positions[i][0] == next_positions[j][0] && 
                next_positions[i][1] == next_positions[j][1]) {
                to_deactivate[i] = 1;
                to_deactivate[j] = 1;
                continue;
            }
            
            // Cazul 2: Proiectilele se intalnesc (se incruciseaza)
            if (next_positions[i][0] == game_state->projectiles[j].x &&
                next_positions[i][1] == game_state->projectiles[j].y &&
                next_positions[j][0] == game_state->projectiles[i].x &&
                next_positions[j][1] == game_state->projectiles[i].y) {
                to_deactivate[i] = 1;
                to_deactivate[j] = 1;
            }
        }
    }
    
    // A treia trecere: misca sau dezactiveaza proiectilele
    for (int i = 0; i < 10; i++) {
        if (!game_state->projectiles[i].active)
            continue;
        
        int proj_x = game_state->projectiles[i].x;
        int proj_y = game_state->projectiles[i].y;
        int next_x = next_positions[i][0];
        int next_y = next_positions[i][1];
        
        // Dezactiveaza daca a lovit alt proiectil
        if (to_deactivate[i]) {
            lock_position(proj_y, proj_x);
            game_state->projectiles[i].active = 0;
            unlock_position(proj_y, proj_x);
            continue;
        }
        
        // Verifica daca iese din harta
        if (next_x < 0 || next_x >= game_state->width || 
            next_y < 0 || next_y >= game_state->height) {
            lock_position(proj_y, proj_x);
            game_state->projectiles[i].active = 0;
            unlock_position(proj_y, proj_x);
            continue;
        }
        
        lock_position(proj_y, proj_x);
        lock_position(next_y, next_x);
        
        // Verifica coliziune cu zid
        if (game_state->map[next_y][next_x] == '#') {
            game_state->projectiles[i].active = 0;
            unlock_position(next_y, next_x);
            unlock_position(proj_y, proj_x);
            continue;
        }
        
        // Verifica coliziune cu jucator A
        if (next_y == game_state->player1_y && next_x == game_state->player1_x) {
            game_state->player1_hp--;
            if (game_state->player1_hp <= 0)
                game_state->game_over = 1;
            game_state->projectiles[i].active = 0;
            unlock_position(next_y, next_x);
            unlock_position(proj_y, proj_x);
            continue;
        }
        
        // Verifica coliziune cu jucator B
        if (next_y == game_state->player2_y && next_x == game_state->player2_x) {
            game_state->player2_hp--;
            if (game_state->player2_hp <= 0)
                game_state->game_over = 1;
            game_state->projectiles[i].active = 0;
            unlock_position(next_y, next_x);
            unlock_position(proj_y, proj_x);
            continue;
        }
        
        // Misca proiectilul
        game_state->projectiles[i].x = next_x;
        game_state->projectiles[i].y = next_y;
        
        unlock_position(next_y, next_x);
        unlock_position(proj_y, proj_x);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 13) {
        fprintf(stderr, "Utilizare: %s <harta.txt> <player_id> ", argv[0]);
        fprintf(stderr, "<A_up> <A_down> <A_left> <A_right> <A_fire> ");
        fprintf(stderr, "<B_up> <B_down> <B_left> <B_right> <B_fire>\n");
        return 1;
    }

    strcpy(map_file, argv[1]);
    player_id = argv[2][0];  // Ia prima litera din argument
    
    key_a_up = argv[3][0];
    key_a_down = argv[4][0];
    key_a_left = argv[5][0];
    key_a_right = argv[6][0];
    key_a_fire = (strcmp(argv[7], "space") == 0) ? ' ' : argv[7][0];
    
    key_b_up = argv[8][0];
    key_b_down = argv[9][0];
    key_b_left = argv[10][0];
    key_b_right = argv[11][0];
    key_b_fire = (strcmp(argv[12], "space") == 0) ? ' ' : argv[12][0];

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    atexit(cleanup);

    // Creeaza/ataseaza memoria partajata
    shm_id = shmget(SHM_KEY, sizeof(GameState), IPC_CREAT | 0666);
    if (shm_id < 0) {
        perror("shmget failed");
        return 1;
    }

    game_state = (GameState *)shmat(shm_id, NULL, 0);
    if (game_state == (void *)-1) {
        perror("shmat failed");
        return 1;
    }

    // Primul proces initializeaza jocul
    if (!game_state->initialized) {
        init_game();

        // Creeaza 400 semafoare (20x20)
        int num_sems = MAX_HEIGHT * MAX_WIDTH;
        sem_id = semget(SEM_KEY, num_sems, IPC_CREAT | 0666);
        if (sem_id < 0) {
            perror("semget failed");
            return 1;
        }

        // Initializeaza toate semafoarele cu 1
        for (int i = 0; i < num_sems; i++)
            semctl(sem_id, i, SETVAL, 1);
        
        printf("Joc initializat cu %d semafoare\n", num_sems);
        sleep(1);
    } else {
        sem_id = semget(SEM_KEY, MAX_HEIGHT * MAX_WIDTH, 0666);
        if (sem_id < 0) {
            perror("semget failed");
            return 1;
        }
    }

    // Initializeaza ncurses
    initscr();
    cbreak();
    noecho();
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    curs_set(0);

    int frame_counter = 0;
    while (!game_state->game_over) {
        int ch = getch();
        
        // Player A controls
        if (ch == key_a_up) {
            move_player('A', 0, -1);  // Sus: scadem y
        } else if (ch == key_a_down) {
            move_player('A', 0, 1);   // Jos: crestem y
        } else if (ch == key_a_left) {
            move_player('A', -1, 0);  // Stanga: scadem x
        } else if (ch == key_a_right) {
            move_player('A', 1, 0);   // Dreapta: crestem x
        } else if (ch == key_a_fire) {
            fire_projectile('A');
        }
        // Player B controls
        else if (ch == key_b_up) {
            move_player('B', 0, -1);  // Sus: scadem y
        } else if (ch == key_b_down) {
            move_player('B', 0, 1);   // Jos: crestem y
        } else if (ch == key_b_left) {
            move_player('B', -1, 0);  // Stanga: scadem x
        } else if (ch == key_b_right) {
            move_player('B', 1, 0);   // Dreapta: crestem x
        } else if (ch == key_b_fire) {
            fire_projectile('B');
        }
        // Quit game
        else if (ch == 'q' || ch == 'Q') {
            game_state->game_over = 1;
        }

        frame_counter++;
        if (frame_counter % 2 == 0)
            update_projectiles();

        draw_game();
        usleep(30000);
    }

    if (game_state->game_over) {
        draw_game();
        sleep(3);
        should_cleanup = 1;
    }

    return 0;
}