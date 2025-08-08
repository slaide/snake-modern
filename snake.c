#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <time.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <string.h>

int override_width = 0;
int override_height = 0;
enum GameMode {
    MODE_REGULAR = 0,
    MODE_GREEDY = 1
};

typedef struct {
    int board_width;
    int board_height;
    int render_fps;
    int move_fps;
    long long render_interval;
    long long move_interval;
    int wraparound_mode;
    int emoji_mode;
    enum GameMode game_mode;
} Config;

Config config = {
    .board_width = 40,
    .board_height = 20,
    .render_fps = 30,
    .move_fps = 6,
    .render_interval = 33333,
    .move_interval = 166667,
    .wraparound_mode = 0,
    .emoji_mode = 0,
    .game_mode = MODE_REGULAR
};

#define EMOJI_SNAKE_HEAD "🐍"
#define EMOJI_SNAKE_BODY "🟢"
#define EMOJI_FOOD "🍎"
#define EMOJI_WALL "🧱"

#define MICROSECONDS_PER_SECOND 1000000
#define LOOP_SLEEP_US 1000
#define TERMINAL_WIDTH_MARGIN 4
#define TERMINAL_HEIGHT_MARGIN 6
#define POINTS_PER_FOOD 10
#define FOOD_PLACEMENT_MAX_ATTEMPTS_MULTIPLIER 2

typedef struct {
    int x, y;
} Point;

typedef struct {
    Point* body;
    int length;
    int max_length;
    int direction;
} Snake;

typedef struct {
    Point food;
    Snake snake;
    int score;
    int game_over;
    int paused;
} Game;

enum Direction {
    UP = 1,
    DOWN = 2,
    LEFT = 3,
    RIGHT = 4
};

struct termios orig_termios;

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int kbhit() {
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    
    int ch = getchar();
    
    fcntl(STDIN_FILENO, F_SETFL, flags);
    
    if (ch != EOF) {
        ungetc(ch, stdin);
        return 1;
    }
    return 0;
}

void clear_screen() {
    printf("\033[2J\033[H");
}

void hide_cursor() {
    printf("\033[?25l");
}

void show_cursor() {
    printf("\033[?25h");
}

void calculate_intervals(Config* cfg) {
    cfg->render_interval = MICROSECONDS_PER_SECOND / cfg->render_fps;
    cfg->move_interval = MICROSECONDS_PER_SECOND / cfg->move_fps;
}


void print_usage(const char* program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  -w WIDTH      Set board width (default: terminal width)\n");
    printf("  -h HEIGHT     Set board height (default: terminal height)\n");
    printf("  -r FPS        Set render frequency in FPS (default: 30)\n");
    printf("  -m FPS        Set move frequency in FPS (default: 6)\n");
    printf("  --mode MODE   Set game mode: regular, greedy (default: regular)\n");
    printf("  --wraparound  Enable wraparound mode (walls teleport to opposite side)\n");
    printf("  --emoji       Enable emoji mode (use emojis for game elements)\n");
    printf("  --help        Show this help message\n");
    printf("\nNote: For best visual experience, use a width:height ratio of approximately 2:1\n");
    printf("      (e.g., -w 40 -h 20 or -w 60 -h 30)\n");
    printf("      Higher render FPS makes input more responsive, higher move FPS makes game faster\n");
    printf("      Default mode: hitting walls causes death. Use --wraparound to pass through walls\n");
    printf("      Game modes: regular (classic snake), greedy (grows every move, find shortest path!)\n");
}

void get_terminal_size(Config* cfg) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        if (override_width == 0) {
            cfg->board_width = w.ws_col - TERMINAL_WIDTH_MARGIN;
            if (cfg->emoji_mode) {
                cfg->board_width = cfg->board_width / 2;
            }
        } else {
            cfg->board_width = override_width;
        }
        
        if (override_height == 0) {
            cfg->board_height = w.ws_row - TERMINAL_HEIGHT_MARGIN;
        } else {
            cfg->board_height = override_height;
        }
    } else {
        if (override_width > 0) cfg->board_width = override_width;
        if (override_height > 0) cfg->board_height = override_height;
    }
}

void init_game(Game *game, Config* cfg) {
    get_terminal_size(cfg);
    
    int max_possible_length = cfg->board_width * cfg->board_height;
    game->snake.body = malloc(max_possible_length * sizeof(Point));
    game->snake.max_length = max_possible_length;
    
    game->snake.length = 3;
    game->snake.direction = RIGHT;
    game->snake.body[0].x = cfg->board_width / 2;
    game->snake.body[0].y = cfg->board_height / 2;
    game->snake.body[1].x = game->snake.body[0].x - 1;
    game->snake.body[1].y = game->snake.body[0].y;
    game->snake.body[2].x = game->snake.body[0].x - 2;
    game->snake.body[2].y = game->snake.body[0].y;
    
    game->score = 0;
    game->game_over = 0;
    game->paused = 0;
    
    srand(time(NULL));
    game->food.x = rand() % cfg->board_width;
    game->food.y = rand() % cfg->board_height;
}

void cleanup_game(Game *game) {
    if (game->snake.body) {
        free(game->snake.body);
        game->snake.body = NULL;
    }
}

void draw_board(Game *game, Config* cfg) {
    printf("\033[H");
    
    if (game->paused) {
        printf("Score: %d - PAUSED (Press SPACE to resume)\n", game->score);
    } else {
        printf("Score: %d\n", game->score);
    }
    printf("\n");
    
    for (int y = -1; y <= cfg->board_height; y++) {
        for (int x = -1; x <= cfg->board_width; x++) {
            if (x == -1 || x == cfg->board_width || y == -1 || y == cfg->board_height) {
                printf("%s", cfg->emoji_mode ? EMOJI_WALL : "#");
            } else {
                int is_snake = 0;
                for (int i = 0; i < game->snake.length; i++) {
                    if (game->snake.body[i].x == x && game->snake.body[i].y == y) {
                        if (cfg->emoji_mode) {
                            printf("%s", (i == 0) ? EMOJI_SNAKE_HEAD : EMOJI_SNAKE_BODY);
                        } else {
                            printf("%s", (i == 0) ? "@" : "o");
                        }
                        is_snake = 1;
                        break;
                    }
                }
                
                if (!is_snake) {
                    if (game->food.x == x && game->food.y == y) {
                        printf("%s", cfg->emoji_mode ? EMOJI_FOOD : "*");
                    } else {
                        printf("%s", cfg->emoji_mode ? "  " : " ");
                    }
                }
            }
        }
        printf("\n");
    }
    
    printf("\nUse WASD or arrow keys to move, SPACE to pause, Q to quit\n");
    fflush(stdout);
}

void generate_food(Game *game, Config* cfg) {
    int total_cells = cfg->board_width * cfg->board_height;
    
    if (game->snake.length >= total_cells) {
        game->game_over = 1;
        return;
    }
    
    int valid = 0;
    int attempts = 0;
    while (!valid && attempts < total_cells * FOOD_PLACEMENT_MAX_ATTEMPTS_MULTIPLIER) {
        game->food.x = rand() % cfg->board_width;
        game->food.y = rand() % cfg->board_height;
        
        valid = 1;
        for (int i = 0; i < game->snake.length; i++) {
            if (game->snake.body[i].x == game->food.x && 
                game->snake.body[i].y == game->food.y) {
                valid = 0;
                break;
            }
        }
        attempts++;
    }
    
    if (!valid) {
        game->game_over = 1;
    }
}

void move_snake(Game *game, Config* cfg) {
    Point new_head = game->snake.body[0];
    
    switch (game->snake.direction) {
        case UP:
            new_head.y--;
            break;
        case DOWN:
            new_head.y++;
            break;
        case LEFT:
            new_head.x--;
            break;
        case RIGHT:
            new_head.x++;
            break;
    }
    
    if (cfg->wraparound_mode) {
        if (new_head.x < 0) {
            new_head.x = cfg->board_width - 1;
        } else if (new_head.x >= cfg->board_width) {
            new_head.x = 0;
        }
        
        if (new_head.y < 0) {
            new_head.y = cfg->board_height - 1;
        } else if (new_head.y >= cfg->board_height) {
            new_head.y = 0;
        }
    } else {
        if (new_head.x < 0 || new_head.x >= cfg->board_width || 
            new_head.y < 0 || new_head.y >= cfg->board_height) {
            game->game_over = 1;
            return;
        }
    }
    
    int ate_food = (new_head.x == game->food.x && new_head.y == game->food.y);
    
    for (int i = 1; i < game->snake.length; i++) {
        if (game->snake.body[i].x == new_head.x && 
            game->snake.body[i].y == new_head.y) {
            game->game_over = 1;
            return;
        }
    }
    
    if (cfg->game_mode == MODE_GREEDY) {
        if (game->snake.length >= game->snake.max_length) {
            game->game_over = 1;
            return;
        }
        
        for (int i = game->snake.length - 1; i >= 0; i--) {
            game->snake.body[i + 1] = game->snake.body[i];
        }
        game->snake.body[0] = new_head;
        game->snake.length++;
        
        if (ate_food) {
            game->score += POINTS_PER_FOOD;
            generate_food(game, cfg);
        }
    } else {
        for (int i = game->snake.length - 1; i > 0; i--) {
            game->snake.body[i] = game->snake.body[i - 1];
        }
        game->snake.body[0] = new_head;
        
        if (ate_food) {
            game->snake.length++;
            game->score += POINTS_PER_FOOD;
            generate_food(game, cfg);
        }
    }
}

void handle_input(Game *game) {
    if (kbhit()) {
        char c = getchar();
        if (c == 27) {
            if (kbhit()) {
                char seq1 = getchar();
                if (seq1 == '[' && kbhit()) {
                    char seq2 = getchar();
                    switch (seq2) {
                        case 'A':
                            if (game->snake.direction != DOWN) {
                                game->snake.direction = UP;
                            }
                            break;
                        case 'B':
                            if (game->snake.direction != UP) {
                                game->snake.direction = DOWN;
                            }
                            break;
                        case 'C':
                            if (game->snake.direction != LEFT) {
                                game->snake.direction = RIGHT;
                            }
                            break;
                        case 'D':
                            if (game->snake.direction != RIGHT) {
                                game->snake.direction = LEFT;
                            }
                            break;
                    }
                }
            }
        } else {
            switch (c) {
                case 'w':
                case 'W':
                    if (game->snake.direction != DOWN) {
                        game->snake.direction = UP;
                    }
                    break;
                case 's':
                case 'S':
                    if (game->snake.direction != UP) {
                        game->snake.direction = DOWN;
                    }
                    break;
                case 'a':
                case 'A':
                    if (game->snake.direction != RIGHT) {
                        game->snake.direction = LEFT;
                    }
                    break;
                case 'd':
                case 'D':
                    if (game->snake.direction != LEFT) {
                        game->snake.direction = RIGHT;
                    }
                    break;
                case 'q':
                case 'Q':
                    game->game_over = 1;
                    break;
                case ' ':
                    game->paused = !game->paused;
                    break;
            }
        }
    }
}

int parse_arguments(int argc, char *argv[], Config* cfg) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0) {
            if (i + 1 < argc) {
                override_width = atoi(argv[++i]);
                if (override_width <= 0) {
                    printf("Error: Width must be a positive integer\n");
                    return 1;
                }
            } else {
                printf("Error: -w requires a width value\n");
                print_usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0) {
            if (i + 1 < argc) {
                override_height = atoi(argv[++i]);
                if (override_height <= 0) {
                    printf("Error: Height must be a positive integer\n");
                    return 1;
                }
            } else {
                printf("Error: -h requires a height value\n");
                print_usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "-r") == 0) {
            if (i + 1 < argc) {
                cfg->render_fps = atoi(argv[++i]);
                if (cfg->render_fps <= 0) {
                    printf("Error: Render FPS must be a positive integer\n");
                    return 1;
                }
            } else {
                printf("Error: -r requires an FPS value\n");
                print_usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "-m") == 0) {
            if (i + 1 < argc) {
                cfg->move_fps = atoi(argv[++i]);
                if (cfg->move_fps <= 0) {
                    printf("Error: Move FPS must be a positive integer\n");
                    return 1;
                }
            } else {
                printf("Error: -m requires an FPS value\n");
                print_usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "--wraparound") == 0) {
            cfg->wraparound_mode = 1;
        } else if (strcmp(argv[i], "--emoji") == 0) {
            cfg->emoji_mode = 1;
        } else if (strcmp(argv[i], "--mode") == 0) {
            if (i + 1 < argc) {
                char* mode = argv[++i];
                if (strcmp(mode, "regular") == 0) {
                    cfg->game_mode = MODE_REGULAR;
                } else if (strcmp(mode, "greedy") == 0) {
                    cfg->game_mode = MODE_GREEDY;
                } else {
                    printf("Error: Unknown game mode '%s'. Available modes: regular, greedy\n", mode);
                    return 1;
                }
            } else {
                printf("Error: --mode requires a mode value\n");
                print_usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return -1;
        } else {
            printf("Error: Unknown option %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    int parse_result = parse_arguments(argc, argv, &config);
    if (parse_result != 0) {
        return (parse_result == -1) ? 0 : parse_result;
    }
    
    calculate_intervals(&config);
    
    Game game;
    
    enable_raw_mode();
    hide_cursor();
    clear_screen();
    
    init_game(&game, &config);
    
    struct timespec start_time, current_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    long long last_render = 0;
    long long last_move = 0;
    
    while (!game.game_over) {
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        long long elapsed_us = (current_time.tv_sec - start_time.tv_sec) * 1000000LL + 
                              (current_time.tv_nsec - start_time.tv_nsec) / 1000LL;
        
        handle_input(&game);
        
        if (elapsed_us - last_move >= config.move_interval && !game.paused) {
            move_snake(&game, &config);
            last_move = elapsed_us;
        }
        
        if (elapsed_us - last_render >= config.render_interval) {
            draw_board(&game, &config);
            last_render = elapsed_us;
        }
        
        usleep(LOOP_SLEEP_US);
    }
    
    cleanup_game(&game);
    clear_screen();
    show_cursor();
    
    printf("Game Over! Final Score: %d\n", game.score);
    printf("Press Enter to exit...");
    
    disable_raw_mode();
    getchar();
    
    return 0;
}