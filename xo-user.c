#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/timerfd.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "game.h"

#define XO_STATUS_FILE "/sys/module/kxo/initstate"
#define XO_DEVICE_FILE "/dev/kxo"
#define XO_DEVICE_ATTR_FILE "/sys/class/kxo/kxo/kxo_state"

#define handle_error(msg)   \
    do {                    \
        perror(msg);        \
        exit(EXIT_FAILURE); \
    } while (0)

static bool status_check(void)
{
    FILE *fp = fopen(XO_STATUS_FILE, "r");
    if (!fp) {
        printf("kxo status : not loaded\n");
        return false;
    }

    char read_buf[20];
    fgets(read_buf, 20, fp);
    read_buf[strcspn(read_buf, "\n")] = 0;
    if (strcmp("live", read_buf)) {
        printf("kxo status : %s\n", read_buf);
        fclose(fp);
        return false;
    }
    fclose(fp);
    return true;
}

static struct termios orig_termios;

static void raw_mode_disable(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void raw_mode_enable(void)
{
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(raw_mode_disable);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static bool read_attr, end_attr;

static void listen_keyboard_handler(void)
{
    int attr_fd = open(XO_DEVICE_ATTR_FILE, O_RDWR);
    char input;

    if (read(STDIN_FILENO, &input, 1) == 1) {
        char buf[20];
        switch (input) {
        case 16: /* Ctrl-P */
            read(attr_fd, buf, 6);
            buf[0] = (buf[0] - '0') ? '0' : '1';
            read_attr ^= 1;
            write(attr_fd, buf, 6);
            if (!read_attr)
                printf("Stopping to display the chess board...\n");
            break;
        case 17: /* Ctrl-Q */
            read(attr_fd, buf, 6);
            buf[4] = '1';
            read_attr = false;
            end_attr = true;
            write(attr_fd, buf, 6);
            printf("Stopping the kernel space tic-tac-toe game...\n");
            break;
        }
    }
    close(attr_fd);
}
static char draw_buffer[DRAWBUFFER_SIZE];
/* Draw the board into draw_buffer */
static int draw_board(char *table)
{
    int i = 0, k = 0;
    draw_buffer[i++] = '\n';
    // smp_wmb();
    draw_buffer[i++] = '\n';
    // smp_wmb();

    while (i < DRAWBUFFER_SIZE) {
        for (int j = 0; j < (BOARD_SIZE << 1) - 1 && k < N_GRIDS; j++) {
            draw_buffer[i++] = j & 1 ? '|' : table[k++];
            // smp_wmb();
        }
        draw_buffer[i++] = '\n';
        // smp_wmb();
        for (int j = 0; j < (BOARD_SIZE << 1) - 1; j++) {
            draw_buffer[i++] = '-';
            // smp_wmb();
        }
        draw_buffer[i++] = '\n';
        // smp_wmb();
    }


    return 0;
}
static void print(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    const struct tm *t = localtime(&ts.tv_sec);

    printf("\033[H\033[J"); /* ASCII escape code to clear the screen */
    printf("%s\n", draw_buffer);
    printf("%02d:%02d:%02d.%03d\n", t->tm_hour, t->tm_min, t->tm_sec,
           (int) (ts.tv_nsec / 1000000));
}
int main(int argc, char *argv[])
{
    if (!status_check())
        exit(1);

    raw_mode_enable();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    struct itimerspec timer_spec = {
        .it_value = {.tv_sec = 0, .tv_nsec = 200000000},
        .it_interval = {.tv_sec = 0, .tv_nsec = 200000000}};

    // char display_buf[DRAWBUFFER_SIZE];
    char table[N_GRIDS];
    fd_set readset;
    int device_fd = open(XO_DEVICE_FILE, O_RDONLY);
    int timerfd = timerfd_create(CLOCK_REALTIME, 0);
    int max_fd = device_fd > STDIN_FILENO ? device_fd : STDIN_FILENO;
    if (timerfd > max_fd)
        max_fd = timerfd;
    read_attr = true;
    end_attr = false;

    if (timerfd_settime(timerfd, TFD_TIMER_ABSTIME, &timer_spec, NULL) == -1) {
        handle_error("timerfd_settime");
    }

    uint64_t exp;
    while (!end_attr) {
        FD_ZERO(&readset);
        FD_SET(STDIN_FILENO, &readset);
        FD_SET(device_fd, &readset);
        FD_SET(timerfd, &readset);

        int result = select(max_fd + 1, &readset, NULL, NULL, NULL);
        if (result < 0) {
            printf("Error with select system call\n");
            exit(1);
        }

        if (FD_ISSET(STDIN_FILENO, &readset)) {
            FD_CLR(STDIN_FILENO, &readset);
            listen_keyboard_handler();
        } else if (read_attr && FD_ISSET(device_fd, &readset)) {
            FD_CLR(device_fd, &readset);
            // printf("\033[H\033[J"); /* ASCII escape code to clear the screen
            // */
            //  read(device_fd, display_buf, DRAWBUFFER_SIZE);
            //  draw_board(display_buf);
            read(device_fd, table, N_GRIDS);
            draw_board(table);
            print();
            // printf("%s", draw_buffer);
            //  printf("%s", display_buf);
        } else if (FD_ISSET(timerfd, &readset)) {
            ssize_t s = read(timerfd, &exp, sizeof(exp));
            if (s != sizeof(uint64_t))
                handle_error("read timer");
            print();
            printf("STDIN_FILENO: %d, devicefd: %d, timerfd: %d\n",
                   STDIN_FILENO, device_fd, timerfd);
        }
    }

    raw_mode_disable();
    fcntl(STDIN_FILENO, F_SETFL, flags);

    close(device_fd);

    return 0;
}
