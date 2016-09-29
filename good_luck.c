#include <time.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <pthread.h>

#define TRUE 1
#define FALSE 0

#define HASHMAP_SIZE 191

#define DRAW_MAX_ROUND 5

#define DRAW_LOOP_SLEEP_INTERVAL (50 * 1000) //50ms
#define DRAW_ROLLING_LOOP_MAX 8

#define RED_MAX 33
#define RED_MIN 1
#define BLUE_MAX 16
#define BLUE_MIN 1
#define RED_BALL_CNT 6
#define BLUE_BALL_CNT 1

typedef unsigned char bool;

enum {
    DRAW_STATE_PENDING = 0,
    DRAW_STATE_SLOW,
    DRAW_STATE_FAST,
    DRAW_STATE_TERMINATE,
};

enum {
    BALL_COLOR_RED = 0,
    BALL_COLOR_BLUE,
};

typedef struct random_request_s {
    int range_start;
    int range_end;
    int result_cnt;
    int *results;
} random_request_t;

typedef struct hashmap_entry_s {
    struct hashmap_entry_s *next;
    int key;
    int val;
} hashmap_entry_t;

typedef struct hashmap_s {
    int size;
    hashmap_entry_t **buf;
} hashmap_t;

typedef struct random_ctx_s {
    int offset;
    int random_max;
    int result_idx;
    int result_cnt;
    int *results;
    hashmap_t hashmap;
} random_ctx_t;

static int draw_state = DRAW_STATE_PENDING;
static pthread_mutex_t draw_state_mutex;

static int max_round = DRAW_MAX_ROUND;

static int
hashmap_init (hashmap_t *hashmap_p)
{
    hashmap_p->size = HASHMAP_SIZE;
    hashmap_p->buf = calloc(HASHMAP_SIZE, sizeof(hashmap_entry_t *));
    if (hashmap_p->buf == NULL) {
        return -1;
    }
    return 0;
}

static int
hashmap_fn (hashmap_t *hashmap_p, int key)
{
    return key % hashmap_p->size;
}

static hashmap_entry_t *
hashmap_get_entry (hashmap_t *hashmap_p, int key)
{
    int i;
    hashmap_entry_t *entry_p;

    i = hashmap_fn(hashmap_p, key);
    entry_p = hashmap_p->buf[i];
    while (entry_p) {
        if (entry_p->key == key) {
            return entry_p;
        } else if (entry_p->key > key) {
            break;
        }
        entry_p = entry_p->next;
    }

    return NULL;
}

static bool
hashmap_has_key (hashmap_t *hashmap_p, int key)
{
    hashmap_entry_t *entry_p;

    entry_p = hashmap_get_entry(hashmap_p, key);
    if (entry_p == NULL) {
        return FALSE;
    } else {
        return TRUE;
    }
}

static int
hashmap_get_val (hashmap_t *hashmap_p, int key)
{
    hashmap_entry_t *entry_p;

    entry_p = hashmap_get_entry(hashmap_p, key);
    if (entry_p == NULL) {
        return -1;
    } else {
        return entry_p->val;
    }
}

static void
hashmap_set_val (hashmap_t *hashmap_p, int key, int val)
{
    hashmap_entry_t *entry_p;

    entry_p = hashmap_get_entry(hashmap_p, key);
    if (entry_p) {
        entry_p->val = val;
    }
    return;
}

static hashmap_entry_t *
hashmap_new_entry (int key, int val)
{
    hashmap_entry_t *entry_p;

    entry_p = calloc(1, sizeof(hashmap_entry_t));
    if (entry_p == NULL) {
        return NULL;
    }

    entry_p->key = key;
    entry_p->val = val;
    return entry_p;
}

static int
hashmap_add_key (hashmap_t *hashmap_p, int key, int val)
{
    hashmap_entry_t **pprev, *p, *new;
    int i;

    new = hashmap_new_entry(key, val);
    if (new == NULL) {
        return -1;
    }

    i = hashmap_fn(hashmap_p, key);
    pprev = &hashmap_p->buf[i];

    for (p = *pprev; p; p = p->next) {
        if (p->key > key) {
            break;
        }
        pprev = &p->next;
    }

    new->next = *pprev;
    *pprev = new;
    return 0;
}

static void
hashmap_clean_slot (hashmap_entry_t *entry_p)
{
    hashmap_entry_t *p;

    p = entry_p;
    while (p) {
        entry_p = p->next;
        free(p);
        p = entry_p;
    }
    return;
}

static void
hashmap_clean (hashmap_t *hashmap_p)
{
    int i;

    for (i = 0; i < hashmap_p->size; i++) {
        hashmap_clean_slot(hashmap_p->buf[i]);
    }
    free(hashmap_p->buf);
    return;
}

static int
check_draw_state (void)
{
    int state;

    pthread_mutex_lock(&draw_state_mutex);
    state = draw_state;
    pthread_mutex_unlock(&draw_state_mutex);
    return state;
}

static void
update_draw_state_for_input (int state)
{
    pthread_mutex_lock(&draw_state_mutex);
    if (draw_state == DRAW_STATE_PENDING) {
        draw_state = state;
    }
    pthread_mutex_unlock(&draw_state_mutex);
    return;
}

static void
terminate_draw_state (void)
{
    pthread_mutex_lock(&draw_state_mutex);
    draw_state = DRAW_STATE_TERMINATE;
    pthread_mutex_unlock(&draw_state_mutex);
    return;
}

static void
reset_draw_state (void)
{
    pthread_mutex_lock(&draw_state_mutex);
    draw_state = DRAW_STATE_PENDING;
    pthread_mutex_unlock(&draw_state_mutex);
    return;
}

static int
init_draw_state_mutex (void)
{
    int rc;

    rc = pthread_mutex_init(&draw_state_mutex, NULL);
    if (rc != 0) {
        printf("Fail to init draw state mutex.\n");
    }
    return rc;
}

static int
init_random_ctx (random_ctx_t *ctx_p, random_request_t *request_p)
{
    int rc;

    memset(ctx_p, 0, sizeof(random_ctx_t));

    ctx_p->offset = request_p->range_start;
    ctx_p->random_max = request_p->range_end - request_p->range_start;
    ctx_p->result_idx = 0;
    ctx_p->result_cnt = request_p->result_cnt;
    ctx_p->results = request_p->results;

    rc = hashmap_init(&ctx_p->hashmap);
    return rc;
}

static void
clean_random_ctx (random_ctx_t *ctx_p)
{
    hashmap_clean(&ctx_p->hashmap);
    return;
}

static int
calc_random_from_idx (random_ctx_t *ctx_p, int idx)
{
    int x;

    if (hashmap_has_key(&ctx_p->hashmap, idx)) {
        x = hashmap_get_val(&ctx_p->hashmap, idx);
    } else {
        x = idx;
    }

    x += ctx_p->offset;
    return x;
}

static int
generate_one_random (random_ctx_t *ctx_p)
{
    int i;

    i = random();
    i = i % (ctx_p->random_max + 1);
    return i;
}

static void
display_random (random_ctx_t *ctx_p, int i)
{
    int j, x;

    printf("\r");

    for (j = 0; j < ctx_p->result_idx; j++) {
        printf("%02d ", ctx_p->results[j]);
    }

    x = calc_random_from_idx(ctx_p, i);
    printf("%02d", x);
    fflush(stdout);
    return;
}

static int
save_random (random_ctx_t *ctx_p, int i)
{
    int x, y, n, rc = 0;

    x = calc_random_from_idx(ctx_p, i);
    ctx_p->results[ctx_p->result_idx] = x;
    ctx_p->result_idx++;

    n = ctx_p->random_max;
    ctx_p->random_max--;
    if (i == n) {
        return 0;
    }

    if (hashmap_has_key(&ctx_p->hashmap, n)) {
        y = hashmap_get_val(&ctx_p->hashmap, n);
    } else {
        y = n;
    }

    if (hashmap_has_key(&ctx_p->hashmap, i)) {
        hashmap_set_val(&ctx_p->hashmap, i, y);
    } else {
        rc = hashmap_add_key(&ctx_p->hashmap, i, y);
    }
    return rc;
}

static int
generate_random_with_ctx (random_ctx_t *ctx_p)
{
    int i, rolling_loop = -1, rc, state;

    while (ctx_p->result_idx < ctx_p->result_cnt) {
        i = generate_one_random(ctx_p);
        display_random(ctx_p, i);

        state = check_draw_state();
        if (rolling_loop == -1 && state == DRAW_STATE_PENDING) {
            usleep(DRAW_LOOP_SLEEP_INTERVAL);
            continue;
        }
        if (state == DRAW_STATE_SLOW) {
            reset_draw_state();
        }
        if (rolling_loop == -1) {
            rolling_loop = DRAW_ROLLING_LOOP_MAX;
        } else if (rolling_loop == 0) {
            rc = save_random(ctx_p, i);
            if (rc != 0) {
                return rc;
            }
        }

        rolling_loop--;
        usleep(DRAW_LOOP_SLEEP_INTERVAL);
    }

    return 0;
}

static int
quicksort_split (int *a, int n)
{
    int pilot, i, j;

    pilot = a[0];
    for (i = 1, j = 0; i < n; i++) {
        if (a[i] < pilot) {
            a[j++] = a[i];
            a[i] = a[j];
        }
    }
    a[j] = pilot;
    return j;
}

static void
quicksort_internal (int *a, int n)
{
    int m;

    if (n <= 1) {
        return;
    }

    m = quicksort_split(a, n);

    quicksort_internal(a, m);
    quicksort_internal(a+m+1, n-m-1);
    return;
}

static void
quicksort (int *a, int n)
{
    if (a == NULL || n == 0) {
        return;
    }

    quicksort_internal(a, n);
    return;
}

static void
sort_result (random_ctx_t *ctx_p)
{
    quicksort(ctx_p->results, ctx_p->result_cnt);
    return;
}

static void
display_result (random_ctx_t *ctx_p, int color)
{
    int i;

    printf("\r");
    if (color == BALL_COLOR_RED) {
        printf("\033[1m\033[31m");
    } else {
        printf("\033[1m\033[34m");
    }

    for (i = 0; i < ctx_p->result_cnt; i++) {
        printf("%02d ", ctx_p->results[i]);
    }

    printf("\033[0m\n");
    return;
}

static int
generate_random (random_request_t *request_p, int color)
{
    random_ctx_t ctx;
    int rc;

    rc = init_random_ctx(&ctx, request_p);
    if (rc != 0) {
        return rc;
    }

    rc = generate_random_with_ctx(&ctx);
    if (rc != 0) {
        return rc;
    }

    sort_result(&ctx);
    display_result(&ctx, color);

    clean_random_ctx(&ctx);
    return 0;
}

static int
draw_red_balls (void)
{
    int red[RED_BALL_CNT] = {0,}, rc;
    random_request_t request;

    request.range_start = RED_MIN;
    request.range_end = RED_MAX;
    request.result_cnt = RED_BALL_CNT;
    request.results = red;
    rc = generate_random(&request, BALL_COLOR_RED);
    return rc;
}

static int
draw_blue_balls (void)
{
    int blue[BLUE_BALL_CNT] = {0,}, rc;
    random_request_t request;

    request.range_start = BLUE_MIN;
    request.range_end = BLUE_MAX;
    request.result_cnt = BLUE_BALL_CNT;
    request.results = blue;
    rc = generate_random(&request, BALL_COLOR_BLUE);
    return rc;
}

static void *
good_luck (void *args)
{
    int i, rc;

    for (i = 0; i < max_round; i++) {
        printf("***** %d *****\n", i+1);
        rc = draw_red_balls();
        if (rc != 0) {
            printf("System failure on drawing red balls.\n");
            exit(1);
        }
        rc = draw_blue_balls();
        if (rc != 0) {
            printf("System failure on drawing blue balls.\n");
            exit(1);
        }
        reset_draw_state();
    }
    terminate_draw_state();
    return NULL;
}

static int
start_good_luck (void)
{
    pthread_t thread;
    int rc;

    rc = pthread_create(&thread, NULL, good_luck, NULL);
    if (rc != 0) {
        printf("Fail to create good luck.\n");
        return rc;
    }
    return 0;
}

static void
handle_input (void)
{
    char ch;
    int state;

    for (;;) {
        state = check_draw_state();
        if (state == DRAW_STATE_TERMINATE) {
            break;
        }
        ch = getchar();
        if (ch == '\n') {
            update_draw_state_for_input(DRAW_STATE_SLOW);
        } else if (ch == ' ') {
            update_draw_state_for_input(DRAW_STATE_FAST);
        } else if (ch == 'q') {
            break;
        }
    }

    return;
}

static int
set_input_echo (bool is_echo)
{
    struct termios term;
    int echo_flags, rc;

    rc = tcgetattr(STDIN_FILENO, &term);
    if (rc == -1){
        return -1;
    }  

    echo_flags = ECHO | ECHOE | ECHOK | ECHONL;
    if (is_echo) {
        term.c_lflag |= echo_flags;  
    } else {
        term.c_lflag &=~ echo_flags;  
    }
    
    rc = tcsetattr(STDIN_FILENO, TCSAFLUSH, &term);  
    if(rc == -1) {  
        return -1;  
    }

    return 0;  
}

static int
set_input_icanon (int is_icanon)
{
    struct termios term;
    int rc;

    rc = tcgetattr(STDIN_FILENO, &term);
    if (rc == -1){
        return -1;
    }  

    if (is_icanon) {
        term.c_lflag |= ICANON;  
    } else {
        term.c_lflag &=~ ICANON;  
    }
    
    rc = tcsetattr(STDIN_FILENO, TCSAFLUSH, &term);  
    if(rc == -1) {  
        return -1;  
    }

    return 0;  
}

static void
init_random (void)
{
    time_t t;

    time(&t);
    srandom((int)t);
    return;
}

static int
do_init (void)
{
    int rc;

    rc = init_draw_state_mutex();
    if (rc != 0) {
        return rc;
    }

    init_random();
    (void)set_input_echo(FALSE);
    (void)set_input_icanon(FALSE);
    return 0;
}

static void
do_clean (void)
{
    (void)set_input_icanon(TRUE);
    (void)set_input_echo(TRUE);
    return;
}

int main (int argc, char **argv)
{
    int rc;

    if (argc > 1) {
        max_round = atoi(argv[1]);
    }
    if (max_round <= 0) {
        printf("Return due to max round <= 0.\n");
        return 0;
    }

    rc = do_init();
    if (rc != 0) {
        return -1;
    }

    rc = start_good_luck();
    if (rc != 0) {
        do_clean();
        return -1;
    }

    handle_input();

    do_clean();
    return 0;
}

