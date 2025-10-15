#include <stdio.h>      
#include <stdlib.h>     // exit, malloc
#include <string.h>     // strcmp, strcpy, strlen
#include <unistd.h>     // sleep, usleep

#include <sys/ipc.h>    // IPC_CREAT (constante pentru IPC)
#include <sys/shm.h>    // shmget, shmat, shmdt (memorie partajata)
#include <sys/sem.h>    // semget, semop, semctl (semafoare)
#include <signal.h>     // signal handling (SIGINT, SIGTERM)

#include <ncurses.h>    // ncurses

#define SHM_KEY 0x1234           // Cheia pentru memoria partajata
#define SEM_KEY 0x5678           // Cheia pentru semafoare

#define MAX_HEIGHT 20            // Inaltimea maxima a hartii
#define MAX_WIDTH 20             // Latimea maxima a hartii

#define INITIAL_HP 5             // Viata initiala pentru fiecare jucator
#define SEM_PROJECTILE_UPDATE 400 // Indexul semaforului special pentru proiectile

typedef struct {
    int x, y;
    int dir_x, dir_y;
    int active; // 1 daca proiectilul este activ, 0 altfel
} Projectile;

typedef struct {
    char map[MAX_HEIGHT][MAX_WIDTH];
    int height, width;

    // Jucator A (Player 1)
    int player1_hp;                   // Viata
    int player1_x, player1_y;         // Pozitia
    int player1_dir_x, player1_dir_y; // Directia in care se uita

    // Jucator B (Player 2)
    int player2_hp;                   // Viata
    int player2_x, player2_y;         // Pozitia
    int player2_dir_x, player2_dir_y; // Directia in care se uita

    int game_over;         // 1 = jocul s-a terminat
    int initialized;       // 1 = jocul a fost initializat de primul proces

    Projectile projectiles[10];  // Array cu 10 sloturi pentru proiectile
    
    // Tastele fiecarui jucator (stocate in memorie partajata)
    char player1_keys[5];       // [up, down, left, right, fire]
    char player2_keys[5];
    int player1_registered;     // 1 daca A si-a inregistrat tastele
    int player2_registered;     // 1 daca B si-a inregistrat tastele

} GameState;

// Variabile globale

GameState *game_state = NULL;  // Pointer catre memoria partajata
int shm_id = -1;               // ID-ul segmentului de memorie partajata
int sem_id = -1;               // ID-ul array-ului de semafoare
char player_id;                // 'A' sau 'B' (ID-ul acestui proces)
char map_file[256];            // Calea catre fisierul cu harta
int should_cleanup = 0;        // 1 = acest proces curata resursele IPC


// Calcularea indexului semaforului pentru o pozitie (y, x)
// ex:
// Pozitia (5, 7) -> Semafor 107 (5 * 20 + 7)
int get_sem_index(int y, int x) {
    return y * MAX_WIDTH + x;
}

// Blocheaza pozitia (y, x)
void lock_position(int y, int x) {
    // Verifica limitele
    if (y < 0 || y >= MAX_HEIGHT || x < 0 || x >= MAX_WIDTH)
        return;
    
    struct sembuf op;
    op.sem_num = get_sem_index(y, x);  // Care semafor?
    op.sem_op = -1;                    // Operatie: scade cu 1
    op.sem_flg = 0;                    // Flag-uri: 0 = asteapta daca e blocat
    semop(sem_id, &op, 1);             // Executa operatia
}

void unlock_position(int y, int x) {
    if (y < 0 || y >= MAX_HEIGHT || x < 0 || x >= MAX_WIDTH)
        return;
    
    struct sembuf op;
    op.sem_num = get_sem_index(y, x);
    // Similar cu lock_position, dar:
    op.sem_op = 1;  // Creste cu 1 (elibereaza)
    op.sem_flg = 0;
    semop(sem_id, &op, 1);
}

int try_lock_projectile_update() {
    struct sembuf op;
    op.sem_num = SEM_PROJECTILE_UPDATE;
    op.sem_op = -1;
    op.sem_flg = IPC_NOWAIT; // Returneaza imediat
    // Returneaza 1 daca a reusit
    return (semop(sem_id, &op, 1) == 0);
}

void unlock_projectile_update() {
    struct sembuf op;
    op.sem_num = SEM_PROJECTILE_UPDATE;
    op.sem_op = 1;
    op.sem_flg = 0;
    semop(sem_id, &op, 1);
}

void cleanup() {
    if (game_state != NULL) {
        endwin();  // Inchide ncurses
        shmdt(game_state);  // Detaseaza memoria partajata

        if (should_cleanup) {
            // DOAR daca jocul s-a terminat, sterge resursele
            shmctl(shm_id, IPC_RMID, NULL);  // Sterge memoria partajata
            semctl(sem_id, 0, IPC_RMID);    // Sterge semafoarele
        }
    }
}

// Gestioneaza Ctrl+C
void signal_handler(int signo) {
    cleanup();  // Curata
    exit(0);    // Iesi
}

// Incarca harta din fisier
int load_map() {

    FILE *f = fopen(map_file, "r");
    if (!f) {
        perror("Eroare la deschiderea hartii");
        return 0;
    }

    game_state->height = 0;
    game_state->width = 0;

    char line[MAX_WIDTH + 2];  // Buffer pentru o linie
    while (fgets(line, sizeof(line), f) && game_state->height < MAX_HEIGHT) {
        int len = strlen(line);

        // Elimina '\n' de la final
        if (line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }
        strcpy(game_state->map[game_state->height], line);

        // Actualizeaza latimea maxima
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

    // Seteaza HP-urile initiale
    game_state->player1_hp = INITIAL_HP;
    game_state->player2_hp = INITIAL_HP;
    
     // Directia initiala: sus
    game_state->player1_dir_x = 0;
    game_state->player1_dir_y = 1;
    game_state->player2_dir_x = 0;
    game_state->player2_dir_y = 1;

    // Pozitiile de start
    game_state->player1_x = 2;
    game_state->player1_y = 2;
    game_state->player2_x = 17;
    game_state->player2_y = 7;

    // Dezactiveaza toate proiectilele
    for (int i = 0; i < 10; i++)
        game_state->projectiles[i].active = 0;
    
    // Reseteaza flag-urile
    game_state->player1_registered = 0;
    game_state->player2_registered = 0;
    game_state->game_over = 0;
    game_state->initialized = 1;  // Marcheaza ca jocul e initializat
}

void draw_game() {
    clear();  // Sterge ecranul

    // Deseneaza harta
    for (int i = 0; i < game_state->height; i++) {
        for (int j = 0; j < game_state->width; j++) {
            char c = game_state->map[i][j];  // Caracterul din harta
            
            // Suprascrie cu jucatori
            if (i == game_state->player1_y && j == game_state->player1_x)
                c = 'A';
            else if (i == game_state->player2_y && j == game_state->player2_x)
                c = 'B';

            else {
                 // Verifica daca e un proiectil

                for (int p = 0; p < 10; p++) {
                    if (game_state->projectiles[p].active && 
                        game_state->projectiles[p].y == i && 
                        game_state->projectiles[p].x == j) {
                        c = '.';
                        break;
                    }
                }
            }
            // Deseneaza caracterul la pozitia (i, j)
            mvaddch(i, j, c);
        }
    }

    // Statistici pe dreapta hartii
    mvprintw(0, game_state->width + 2, "Player A: %d HP", game_state->player1_hp);
    mvprintw(1, game_state->width + 2, "Player B: %d HP", game_state->player2_hp);
    mvprintw(3, game_state->width + 2, "You are: Player %c", player_id);
    mvprintw(5, game_state->width + 2, "Controls:");
    
    // Afiseaza tastele din memoria partajata
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
                 game_state->player1_keys[0],  // up
                 game_state->player1_keys[1],  // down
                 game_state->player1_keys[2],  // left
                 game_state->player1_keys[3],  // right
                 game_state->player2_keys[4] == ' ' ? 'S' : game_state->player2_keys[4]);
    } else {
        mvprintw(7, game_state->width + 2, "B: waiting...");
    }

    // Game Over
    if (game_state->game_over) {
        char winner = (game_state->player1_hp > 0) ? 'A' : 'B';
        mvprintw(game_state->height / 2, game_state->width / 2 - 10, 
                 "GAME OVER! Player %c wins!", winner);
    }

    // Afiseaza pe ecran
    refresh();
}

void move_player(char which_player, int dx, int dy) {

    // Determina pointeri catre datele jucatorului
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

    // Calculeaza noua pozitie
    int old_x = *px;
    int old_y = *py;
    int new_x = old_x + dx;
    int new_y = old_y + dy;

    // Verifica limitele hartii
    if (new_x < 0 || new_x >= game_state->width || 
        new_y < 0 || new_y >= game_state->height)
        return;

    // Blocheaza ambele pozitii
    lock_position(old_y, old_x);
    lock_position(new_y, new_x);
    
    // Verifica daca pozitia e libera
    char cell = game_state->map[new_y][new_x];

    if (cell == ' ' && // Spatiu liber

        // NU e ocupat de celalalt jucator
        !(new_y == game_state->player1_y && new_x == game_state->player1_x && which_player != 'A') &&
        !(new_y == game_state->player2_y && new_x == game_state->player2_x && which_player != 'B')) {

            // Move player
        *px = new_x;
        *py = new_y;
        *dir_x = dx;    // Update direction
        *dir_y = dy;
    }
    
    // Deblocheaza pozitiile
    unlock_position(new_y, new_x);
    unlock_position(old_y, old_x);
}

void fire_projectile(char which_player) {
    // Determina coordonatele de start si directia proiectilului
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

    // Pozitia initiala a proiectilului (in fata jucatorului)
    int proj_x = start_x + proj_dir_x;
    int proj_y = start_y + proj_dir_y;
    
    // Verifica limitele hartii
    if (proj_x < 0 || proj_x >= game_state->width || 
        proj_y < 0 || proj_y >= game_state->height)
        return;
    

    lock_position(proj_y, proj_x);

    // Verifica daca pozitia e libera
    int slot = -1;
    for (int i = 0; i < 10; i++) {
        if (!game_state->projectiles[i].active) {
            slot = i;
            break;
        }
    }
    
    // Daca exista un slot liber, initializeaza proiectilul
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
    int next_positions[10][2]; // next_positions[i] = {next_x, next_y}
    int to_deactivate[10] = {0}; // 1 daca proiectilul i trebuie dezactivat
    
    // Calculeaza pozitiile urmatoare
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

        // Calculeaza noua pozitie
        next_positions[i][0] = proj_x + dir_x;
        next_positions[i][1] = proj_y + dir_y;
    }
    
    // Coliziuni intre proiectile

    // Verifica toate perechile de proiectile
    for (int i = 0; i < 10; i++) {
        if (!game_state->projectiles[i].active || to_deactivate[i])
        continue;
            
        for (int j = i + 1; j < 10; j++) {
            if (!game_state->projectiles[j].active || to_deactivate[j])
                continue;
            
            // Coliziune directa - Ambele proiectile ajung in aceeasi pozitie
            if (next_positions[i][0] == next_positions[j][0] && 
                next_positions[i][1] == next_positions[j][1]) {

                to_deactivate[i] = 1;
                to_deactivate[j] = 1;
                continue;
            }
            
            // Coliziune indirecta - Proiectilele se intersecteaza
            if (next_positions[i][0] == game_state->projectiles[j].x &&
                next_positions[i][1] == game_state->projectiles[j].y &&
                next_positions[j][0] == game_state->projectiles[i].x &&
                next_positions[j][1] == game_state->projectiles[i].y) {
                to_deactivate[i] = 1;
                to_deactivate[j] = 1;
            }
        }
    }
    

    // Actualizeaza pozitiile proiectilelor
    for (int i = 0; i < 10; i++) {
        if (!game_state->projectiles[i].active)
            continue;
        
            // Pozitia curenta si urmatoarea
        int proj_x = game_state->projectiles[i].x;
        int proj_y = game_state->projectiles[i].y;
        int next_x = next_positions[i][0];
        int next_y = next_positions[i][1];
        
        // Daca proiectilul trebuie dezactivat (coliziune)

        if (to_deactivate[i]) {
            // Dezactiveaza proiectilul
            lock_position(proj_y, proj_x);
            game_state->projectiles[i].active = 0;
            unlock_position(proj_y, proj_x);
            continue;
        }
        
        // Verifica limitele hartii
        if (next_x < 0 || next_x >= game_state->width || 
            next_y < 0 || next_y >= game_state->height) {
            lock_position(proj_y, proj_x);
            game_state->projectiles[i].active = 0;
            unlock_position(proj_y, proj_x);
            continue;
        }
        
        lock_position(proj_y, proj_x);
        lock_position(next_y, next_x);

        // Verifica coliziunea cu ziduri
        
        if (game_state->map[next_y][next_x] == '#') {
            game_state->projectiles[i].active = 0;
            unlock_position(next_y, next_x);
            unlock_position(proj_y, proj_x);
            continue;
        }

        // Verifica coliziunea cu jucatorii

        // Coliziune cu Player 1
        if (next_y == game_state->player1_y && next_x == game_state->player1_x) {
            game_state->player1_hp--;
            if (game_state->player1_hp <= 0)
                game_state->game_over = 1;
            game_state->projectiles[i].active = 0;
            unlock_position(next_y, next_x);
            unlock_position(proj_y, proj_x);
            continue;
        }
        
        // Coliziune cu Player 2
        if (next_y == game_state->player2_y && next_x == game_state->player2_x) {
            game_state->player2_hp--;
            if (game_state->player2_hp <= 0)
                game_state->game_over = 1;
            game_state->projectiles[i].active = 0;
            unlock_position(next_y, next_x);
            unlock_position(proj_y, proj_x);
            continue;
        }
        
        // Muta proiectilul in noua pozitie
        game_state->projectiles[i].x = next_x;
        game_state->projectiles[i].y = next_y;
        
        unlock_position(next_y, next_x);
        unlock_position(proj_y, proj_x);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 8) {
        fprintf(stderr, "Utilizare: %s <harta.txt> <player_id> ", argv[0]);
        fprintf(stderr, "<up> <down> <left> <right> <fire>\n");
        fprintf(stderr, "Exemplu A: %s harta.txt A w s a d f\n", argv[0]);
        fprintf(stderr, "Exemplu B: %s harta.txt B i k j l space\n", argv[0]);
        return 1;
    }

    strcpy(map_file, argv[1]);  // "harta.txt"
    player_id = argv[2][0]; // 'A' sau 'B'
    
    // Citeste tastele din linia de comanda
    char my_keys[5];
    my_keys[0] = argv[3][0];  // up
    my_keys[1] = argv[4][0];  // down
    my_keys[2] = argv[5][0];  // left
    my_keys[3] = argv[6][0];  // right
    my_keys[4] = (strcmp(argv[7], "space") == 0) ? ' ' : argv[7][0];  // fire

    signal(SIGINT, signal_handler); // Ctrl+C
    signal(SIGTERM, signal_handler);    // kill
    atexit(cleanup); // cleanup() la exit()

    // Creaza/ataseaza memoria partajata
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

        // Creaza 401 semafoare: 400 pentru pozitii + 1 semafor pentru proiectile
        int num_sems = MAX_HEIGHT * MAX_WIDTH + 1;
        sem_id = semget(SEM_KEY, num_sems, IPC_CREAT | 0666);
        if (sem_id < 0) {
            perror("semget failed");
            return 1;
        }

        // Initializeaza toate semafoarele la 1 (liber)
        for (int i = 0; i < num_sems; i++)
            semctl(sem_id, i, SETVAL, 1);
        
        printf("Joc initializat cu %d semafoare\n", num_sems);
        sleep(1);
    } else {
        // Al doilea proces doar se ataseaza la semafoare
        // Nu mai creaza, doar se ataseaza la semafoarele existente
        sem_id = semget(SEM_KEY, MAX_HEIGHT * MAX_WIDTH + 1, 0666);
        if (sem_id < 0) {
            perror("semget failed");
            return 1;
        }
    }

    // Inregistreaza tastele in memoria partajata
    if (player_id == 'A') {
        for (int i = 0; i < 5; i++)
            game_state->player1_keys[i] = my_keys[i];
        game_state->player1_registered = 1;
    } else if (player_id == 'B') {
        for (int i = 0; i < 5; i++)
            game_state->player2_keys[i] = my_keys[i];
        game_state->player2_registered = 1;
    }

     // Initializeaza ncurses
    initscr();              // Porneste modul ncurses
    cbreak();               // Dezactiveaza buffering-ul liniei
    noecho();               // Nu afisa tastele apasate
    nodelay(stdscr, TRUE);  // getch() non-blocking (returneaza imediat)
    keypad(stdscr, TRUE);   // Activeaza taste speciale (sageti, etc.)
    curs_set(0);            // Ascunde cursorul

    int frame_counter = 0;
    while (!game_state->game_over) {
        // Citeste o tasta
        int ch = getch(); // Returneaza codul tastei sau -1
        
        // Verifica tastele pentru Player A (daca sunt inregistrate)
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
        
        // Verifica tastele pentru Player B (daca sunt inregistrate)
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

        // Actualizeaza proiectilele cu semafor global
        frame_counter++;
        if (frame_counter % 2 == 0) { // La fiecare 2 frame-uri

            // Doar unul dintre procese poate actualiza proiectilele
            if (try_lock_projectile_update()) {
                update_projectiles();
                unlock_projectile_update();
            }
            // Daca acest proces nu reuseste, celalalt o face
        }

        // Deseneaza totul
        draw_game();
        // Delay (30ms = ~33 FPS)
        usleep(30000);
    }

    if (game_state->game_over) {
        draw_game(); // Afiseaza ecranul final
        sleep(3);   // Asteapta 3 secunde
        should_cleanup = 1; // Marcheaza ca trebuie sa stearga resursele IPC
    }

    return 0; // cleanup() e apelat automat (atexit)
}