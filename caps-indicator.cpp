#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <unistd.h>
#include <xcb/shape.h>
#include <xcb/shm.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <sys/types.h>
#include <sys/wait.h>

static void die(const char* fmt, ...) {
    va_list argp;
    va_start(argp, fmt);
    vfprintf(stderr, fmt, argp);
    va_end(argp);
    fputc('\n', stderr);
    exit(1);
}

static pid_t lock_exclusively(const int fd) {
    struct flock lock;
    int err = 0;

    if (fd == -1) {
        errno = EINVAL;
        return -1;
    }

    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    if (!fcntl(fd, F_SETLK, &lock))
        return 0;

    /* Remember the cause of the failure */
    err = errno;

    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    lock.l_pid = 0;
    if (fcntl(fd, F_GETLK, &lock) == 0 && lock.l_pid > 0)
        return lock.l_pid;

    errno = err;
    return -1;
}

#define BORDER_SIZE 16
typedef struct XCBGrabContext {
    xcb_connection_t* conn;
    xcb_screen_t* screen;
    xcb_window_t window;
    int x = BORDER_SIZE, y = BORDER_SIZE;
    int width, height;
    int region_border = BORDER_SIZE;
} XCBGrabContext;

static xcb_screen_t* get_screen(const xcb_setup_t* setup, int screen_num) {
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    xcb_screen_t* screen = NULL;

    for (; it.rem > 0; xcb_screen_next(&it)) {
        if (!screen_num) {
            screen = it.data;
            break;
        }

        screen_num--;
    }

    return screen;
}

static void draw_rectangle(XCBGrabContext* c) {
    xcb_gcontext_t gc = xcb_generate_id(c->conn);
    uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_LINE_WIDTH | XCB_GC_LINE_STYLE
        | XCB_GC_FILL_STYLE;
    uint32_t values[] = { 0x8EBF1F, c->screen->white_pixel, (uint32_t)c->region_border,
        XCB_LINE_STYLE_SOLID, XCB_FILL_STYLE_OPAQUE_STIPPLED };
    xcb_rectangle_t r = { 1, 1, (uint16_t)(c->width + c->region_border * 2 - 3),
        (uint16_t)(c->height + c->region_border * 2 - 3) };

    xcb_create_gc(c->conn, gc, c->window, mask, values);

    xcb_poly_rectangle(c->conn, c->window, gc, 1, &r);
}

static void setup_window(XCBGrabContext* c) {
    uint32_t mask = XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t values[] = { 1, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY };
    xcb_rectangle_t rect = { 0, 0, (uint16_t)(c->width), (uint16_t)(c->height) };

    c->window = xcb_generate_id(c->conn);

    xcb_create_window(c->conn, XCB_COPY_FROM_PARENT, c->window, c->screen->root,
        c->x - c->region_border, c->y - c->region_border, c->width + c->region_border * 2,
        c->height + c->region_border * 2, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
        mask, values);

    xcb_shape_rectangles(c->conn, XCB_SHAPE_SO_SUBTRACT, XCB_SHAPE_SK_BOUNDING,
        XCB_CLIP_ORDERING_UNSORTED, c->window, c->region_border, c->region_border, 1, &rect);
}

static int xcbgrab_read_header(XCBGrabContext* c, char* url) { return 0; }

Display* dpy = XOpenDisplay(NULL);

bool window_shown = false;
static int update(XCBGrabContext* c) {
    int unsigned state;
    int screen_num, caps, ret;

    /* get state */
    (void)XkbGetIndicatorState(dpy, XkbUseCoreKbd, &state);
    caps = (state & 0x01) == 1;

    if (caps) {
        c->conn = xcb_connect(NULL, &screen_num);
        if ((ret = xcb_connection_has_error(c->conn))) {
            die("Cannot open default display, error %d.\n", ret);
        }

        const xcb_setup_t* setup;
        setup = xcb_get_setup(c->conn);

        c->screen = get_screen(setup, screen_num);
        if (!c->screen) {
            die("The screen %d does not exist.\n", screen_num);
        }

        c->width = c->screen->width_in_pixels - c->region_border * 2;
        c->height = c->screen->height_in_pixels - c->region_border * 2;

        setup_window(c);

        const uint32_t args[]
            = { (uint32_t)c->x - c->region_border, (uint32_t)c->y - c->region_border };

        xcb_configure_window(c->conn, c->window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, args);

        xcb_map_window(c->conn, c->window);

        xcb_flush(c->conn);

        xcb_generic_event_t* event;

        window_shown = true;
        while ((event = xcb_wait_for_event(c->conn))) {
            switch (event->response_type) {
            case XCB_EXPOSE:
                draw_rectangle(c);
                xcb_flush(c->conn);
                free(event);
                return 0;
            default:
                break;
            }
            free(event);
        }
    } else {
        if (window_shown) {
            xcb_destroy_window(c->conn, c->window);
            xcb_flush(c->conn);
            xcb_disconnect(c->conn);
        }
    }
    return 0;
}

int main(int argc, char* argv[]) {
    pid_t pid1;
    pid_t pid2;
    int status;

    auto s = "/tmp/capslock.pid";
    int fd = open(s, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        fprintf(stderr, "Unable to open %s for read/write: %s\n", s, strerror(errno));
        return 1;
    }

    struct flock lock;

    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    lock.l_pid = 0;

    if (fcntl(fd, F_GETLK, &lock) == 0 && lock.l_pid > 0) {
        fprintf(stderr, "Cannot start capslock indicator. Another instance is running, pid = %d.\n",
            lock.l_pid);
        // kill(lock.l_pid, SIGTERM);
        return 1;
    }

    if (pid1 = fork()) {
        /* parent process A */
        waitpid(pid1, &status, 0);
        exit(0);
    } else {
        /* child process B */
        if (pid2 = fork()) {
            exit(0);
        }
    }

    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    fcntl(fd, F_SETLK, &lock);

    XCBGrabContext c;
    char* url;
    if (argc != 2)
        url = strdup("");
    else
        url = argv[1];
    xcbgrab_read_header(&c, url);

    int xfd = ConnectionNumber(dpy);
    int xkb_base_event_type;
    int err, reason;

    (void)XkbOpenDisplay(DisplayString(dpy), &xkb_base_event_type, &err, NULL, NULL, &reason);
    if (!XkbSelectEvents(
            dpy, XkbUseCoreKbd, XkbIndicatorStateNotifyMask, XkbIndicatorStateNotifyMask)) {
        die("could not select XKB events");
    }
    update(&c);

    int sel;
    fd_set fds;
    do {
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        sel = select(FD_SETSIZE, &fds, NULL, NULL, NULL);
        if (sel <= 0) {
            continue;
        }
        if (FD_ISSET(xfd, &fds))
            update(&c);
    } while (1);

    return 0;
}
