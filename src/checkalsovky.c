#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LINE_MAX_LEN 4096
#define MOVE_MAX_LEN 16
#define MAX_CANDIDATES 32
#define MATE_SCORE 100000

typedef struct {
    char move[MOVE_MAX_LEN];
    int score;
    int depth;
} Candidate;

typedef struct {
    char bestmove[MOVE_MAX_LEN];
    char ponder[MOVE_MAX_LEN];
    int score;
    int depth;
    Candidate candidates[MAX_CANDIDATES];
    int candidate_count;
    bool has_score;
    bool failed;
} SearchResult;

typedef struct {
    const char *path;
    pid_t pid;
    int in_fd;
    int out_fd;
    char pending[LINE_MAX_LEN];
    size_t pending_len;
} Engine;

static Engine primary_engine = {"stockfish/src/stockfish", -1, -1, -1, {0}, 0};
static Engine secondary_engine = {"reckless/reckless", -1, -1, -1, {0}, 0};
static char current_position[LINE_MAX_LEN] = "position startpos";
static int veto_margin = 25;
static int multipv = 5;
static double movetime_scale = 6.0;

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void out_line(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    putchar('\n');
    fflush(stdout);
}

static bool starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int parse_int_after(const char *line, const char *needle, bool *ok) {
    const char *p = strstr(line, needle);
    if (!p) {
        *ok = false;
        return 0;
    }
    p += strlen(needle);
    while (*p == ' ') p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    *ok = end != p;
    return (int)v;
}

static int parse_movetime(const char *command) {
    bool ok = false;
    int value = parse_int_after(command, "movetime", &ok);
    return ok ? value : 0;
}

static void scale_go_command(const char *command, char *out, size_t out_size) {
    int movetime = parse_movetime(command);
    if (movetime <= 0 || movetime_scale <= 1.0) {
        snprintf(out, out_size, "%s", command);
        return;
    }

    const char *p = strstr(command, "movetime");
    size_t prefix_len = (size_t)(p - command);
    p += strlen("movetime");
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') p++;

    int scaled = (int)(movetime * movetime_scale);
    snprintf(out, out_size, "%.*smovetime %d%s", (int)prefix_len, command, scaled, p);
}

static bool parse_score(const char *line, int *score) {
    const char *p = strstr(line, " score ");
    if (!p) return false;
    p += 7;
    if (starts_with(p, "cp ")) {
        p += 3;
        *score = atoi(p);
        return true;
    }
    if (starts_with(p, "mate ")) {
        p += 5;
        int mate = atoi(p);
        *score = mate > 0 ? MATE_SCORE - mate : -MATE_SCORE - mate;
        return true;
    }
    return false;
}

static bool parse_pv_move(const char *line, char *move, size_t move_size) {
    const char *p = strstr(line, " pv ");
    if (!p) return false;
    p += 4;
    size_t i = 0;
    while (p[i] && p[i] != ' ' && i + 1 < move_size) {
        move[i] = p[i];
        i++;
    }
    move[i] = '\0';
    return i > 0;
}

static void add_candidate(SearchResult *result, const char *move, int score, int depth) {
    for (int i = 0; i < result->candidate_count; i++) {
        if (strcmp(result->candidates[i].move, move) == 0) {
            if (depth >= result->candidates[i].depth) {
                result->candidates[i].score = score;
                result->candidates[i].depth = depth;
            }
            return;
        }
    }
    if (result->candidate_count >= MAX_CANDIDATES) return;
    Candidate *candidate = &result->candidates[result->candidate_count++];
    snprintf(candidate->move, sizeof(candidate->move), "%s", move);
    candidate->score = score;
    candidate->depth = depth;
}

static bool candidate_score(const SearchResult *result, const char *move, int *score) {
    for (int i = 0; i < result->candidate_count; i++) {
        if (strcmp(result->candidates[i].move, move) == 0) {
            *score = result->candidates[i].score;
            return true;
        }
    }
    if (result->has_score && strcmp(result->bestmove, move) == 0) {
        *score = result->score;
        return true;
    }
    return false;
}

static bool spawn_engine(Engine *engine) {
    if (engine->pid > 0) return true;

    int to_child[2];
    int from_child[2];
    if (pipe(to_child) != 0 || pipe(from_child) != 0) return false;

    pid_t pid = fork();
    if (pid == 0) {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        dup2(from_child[1], STDERR_FILENO);
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        execl(engine->path, engine->path, (char *)NULL);
        _exit(127);
    }

    close(to_child[0]);
    close(from_child[1]);
    engine->pid = pid;
    engine->in_fd = to_child[1];
    engine->out_fd = from_child[0];
    fcntl(engine->out_fd, F_SETFL, fcntl(engine->out_fd, F_GETFL, 0) | O_NONBLOCK);
    engine->pending_len = 0;
    return pid > 0;
}

static void send_engine(Engine *engine, const char *command) {
    if (engine->pid <= 0 && !spawn_engine(engine)) return;
    dprintf(engine->in_fd, "%s\n", command);
}

static bool read_engine_line(Engine *engine, char *line, size_t line_size) {
    while (1) {
        for (size_t i = 0; i < engine->pending_len; i++) {
            if (engine->pending[i] == '\n') {
                size_t len = i;
                if (len && engine->pending[len - 1] == '\r') len--;
                if (len >= line_size) len = line_size - 1;
                memcpy(line, engine->pending, len);
                line[len] = '\0';
                memmove(engine->pending, engine->pending + i + 1, engine->pending_len - i - 1);
                engine->pending_len -= i + 1;
                return true;
            }
        }

        if (engine->pending_len >= sizeof(engine->pending) - 1) engine->pending_len = 0;
        ssize_t n = read(engine->out_fd, engine->pending + engine->pending_len,
                         sizeof(engine->pending) - engine->pending_len - 1);
        if (n <= 0) return false;
        engine->pending_len += (size_t)n;
        engine->pending[engine->pending_len] = '\0';
    }
}

static bool wait_token(Engine *engine, const char *token, int timeout_ms) {
    long deadline = now_ms() + timeout_ms;
    char line[LINE_MAX_LEN];
    while (now_ms() < deadline) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(engine->out_fd, &set);
        struct timeval tv = {0, 50000};
        int rc = select(engine->out_fd + 1, &set, NULL, NULL, &tv);
        if (rc > 0 && read_engine_line(engine, line, sizeof(line))) {
            if (strcmp(line, token) == 0) return true;
        }
    }
    return false;
}

static void ensure_ready(void) {
    Engine *engines[] = {&primary_engine, &secondary_engine};
    for (int i = 0; i < 2; i++) {
        if (!spawn_engine(engines[i])) continue;
        send_engine(engines[i], "uci");
        wait_token(engines[i], "uciok", 5000);
        send_engine(engines[i], "isready");
        wait_token(engines[i], "readyok", 5000);
    }
}

static void parse_info(SearchResult *result, const char *line) {
    int score = 0;
    int depth = 0;
    bool ok = false;
    depth = parse_int_after(line, "depth", &ok);
    if (!ok) depth = result->depth;
    if (parse_score(line, &score)) {
        result->score = score;
        result->depth = depth > result->depth ? depth : result->depth;
        result->has_score = true;
        char move[MOVE_MAX_LEN];
        if (parse_pv_move(line, move, sizeof(move))) add_candidate(result, move, score, depth);
    }
}

static void parse_bestmove(SearchResult *result, const char *line) {
    char best[MOVE_MAX_LEN] = {0};
    char ponder[MOVE_MAX_LEN] = {0};
    sscanf(line, "bestmove %15s ponder %15s", best, ponder);
    snprintf(result->bestmove, sizeof(result->bestmove), "%s", best);
    snprintf(result->ponder, sizeof(result->ponder), "%s", ponder);
}

static SearchResult search_engine(Engine *engine, const char *go_command, int timeout_ms) {
    SearchResult result;
    memset(&result, 0, sizeof(result));
    send_engine(engine, go_command);

    long deadline = now_ms() + timeout_ms;
    char line[LINE_MAX_LEN];
    while (now_ms() < deadline) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(engine->out_fd, &set);
        struct timeval tv = {0, 50000};
        int rc = select(engine->out_fd + 1, &set, NULL, NULL, &tv);
        if (rc > 0 && read_engine_line(engine, line, sizeof(line))) {
            if (starts_with(line, "info ")) parse_info(&result, line);
            if (starts_with(line, "bestmove ")) {
                parse_bestmove(&result, line);
                return result;
            }
        }
    }
    send_engine(engine, "stop");
    result.failed = true;
    return result;
}

static void choose_move(const SearchResult *primary, const SearchResult *secondary, char *move, size_t move_size) {
    if (primary->bestmove[0] == '\0') {
        snprintf(move, move_size, "%s", secondary->bestmove[0] ? secondary->bestmove : "0000");
        return;
    }
    if (secondary->bestmove[0] == '\0' || strcmp(primary->bestmove, secondary->bestmove) == 0) {
        snprintf(move, move_size, "%s", primary->bestmove);
        return;
    }

    int primary_score = 0;
    int secondary_score = 0;
    if (candidate_score(primary, primary->bestmove, &primary_score) &&
        candidate_score(primary, secondary->bestmove, &secondary_score) &&
        primary_score - secondary_score <= veto_margin) {
        snprintf(move, move_size, "%s", secondary->bestmove);
        return;
    }
    snprintf(move, move_size, "%s", primary->bestmove);
}

static void handle_go(const char *command) {
    ensure_ready();
    char child_go[LINE_MAX_LEN];
    scale_go_command(command, child_go, sizeof(child_go));
    int movetime = parse_movetime(child_go);
    int timeout = movetime > 0 ? movetime + 10000 : 30000;

    char option[128];
    snprintf(option, sizeof(option), "setoption name MultiPV value %d", multipv);
    send_engine(&primary_engine, option);
    send_engine(&primary_engine, "isready");
    wait_token(&primary_engine, "readyok", 5000);

    send_engine(&primary_engine, current_position);
    send_engine(&secondary_engine, current_position);

    SearchResult primary = search_engine(&primary_engine, child_go, timeout);
    SearchResult secondary = search_engine(&secondary_engine, child_go, timeout);

    char best[MOVE_MAX_LEN];
    choose_move(&primary, &secondary, best, sizeof(best));
    out_line("bestmove %s", best);
}

static const char *option_value(const char *command) {
    const char *p = strstr(command, " value ");
    return p ? p + 7 : "";
}

static void handle_setoption(const char *command) {
    if (strstr(command, "name Power") || strstr(command, "name FusedMoveTimeScale")) {
        double value = atof(option_value(command));
        if (value >= 1.0 && value <= 10.0) movetime_scale = value;
    } else if (strstr(command, "name CandidateLines") || strstr(command, "name StockfishMultiPV")) {
        int value = atoi(option_value(command));
        if (value >= 1 && value <= 10) multipv = value;
    } else if (strstr(command, "name SafetyMargin") || strstr(command, "name VetoMargin")) {
        int value = atoi(option_value(command));
        if (value >= 0) veto_margin = value;
    } else {
        ensure_ready();
        send_engine(&primary_engine, command);
        send_engine(&secondary_engine, command);
    }
}

static void shutdown_engines(void) {
    Engine *engines[] = {&primary_engine, &secondary_engine};
    for (int i = 0; i < 2; i++) {
        if (engines[i]->pid > 0) {
            send_engine(engines[i], "quit");
            close(engines[i]->in_fd);
            close(engines[i]->out_fd);
            waitpid(engines[i]->pid, NULL, 0);
            engines[i]->pid = -1;
        }
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    char line[LINE_MAX_LEN];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, "uci") == 0) {
            out_line("id name checkalsovky");
            out_line("id author checkalsovky");
            out_line("option name Power type spin default 6 min 1 max 10");
            out_line("option name CandidateLines type spin default 5 min 1 max 10");
            out_line("option name SafetyMargin type spin default 25 min 0 max 1000");
            out_line("uciok");
        } else if (strcmp(line, "isready") == 0) {
            ensure_ready();
            out_line("readyok");
        } else if (starts_with(line, "setoption ")) {
            handle_setoption(line);
        } else if (starts_with(line, "position ")) {
            snprintf(current_position, sizeof(current_position), "%s", line);
        } else if (starts_with(line, "go")) {
            handle_go(line);
        } else if (strcmp(line, "ucinewgame") == 0) {
            ensure_ready();
            send_engine(&primary_engine, "ucinewgame");
            send_engine(&secondary_engine, "ucinewgame");
        } else if (strcmp(line, "quit") == 0) {
            shutdown_engines();
            return 0;
        }
    }
    shutdown_engines();
    return 0;
}
