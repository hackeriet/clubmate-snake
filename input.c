#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <errno.h>
#include <linux/limits.h>
#include <linux/joystick.h>
#include <libudev.h>

#include "matelight.h"

static struct joystick joysticks[MAX_JOYSTICKS] = { 0 };
static size_t num_joysticks = 0;

static struct udev *udev_ctx = NULL;
static struct udev_monitor *udev_monitor = NULL;

void input_reset(void)
{
    size_t i;

    for (i = 0; i < num_joysticks; i++) {
        if (joysticks[i].fd != -1) {
            close(joysticks[i].fd);
            joysticks[i].fd = -1;
        }
    }
    num_joysticks = 0;

    if (udev_ctx != NULL) {
        if (udev_monitor != NULL) {
            udev_monitor_unref(udev_monitor);
        }
        udev_monitor = NULL;

        udev_unref(udev_ctx);
        udev_ctx = NULL;
    }
}

struct joystick *get_free_joystick(void)
{
    size_t i;

    for (i = 0; i < num_joysticks; i++) {
        if (joysticks[i].fd == -1)
            return &joysticks[i];
    }

    if (num_joysticks < MAX_JOYSTICKS) {
        i = num_joysticks;
        joysticks[i].fd = -1;
        num_joysticks++;
        return &joysticks[i];
    }

    return NULL;
}

int get_available_player(void)
{
    size_t i;
    int player = 1;

    for (i = 0; i < num_joysticks; i++) {
        if (joysticks[i].fd == -1)
            continue;

        if (joysticks[i].player == player) {
            player++;
            i = 0;
        }
    }

    return player;
}

bool open_joystick(const char *devnode, struct stat *st, bool check_joydev)
{
    int fd = -1;
    int opt = 0;
    int version = 0;
    char axes = 0;
    char buttons = 0;
    char name[128] = { 0 };
    int save_errno = 0;
    struct joystick *joystick = NULL;
    int player = 1;

    fd = open(devnode, O_RDONLY);
    if (fd == -1) {
        return false;
    }

    opt = fcntl(fd, F_GETFL);
    if (opt >= 0) {
        opt |= O_NONBLOCK;
        fcntl(fd, F_SETFL, opt);
    }

    if (check_joydev) {
        if (ioctl(fd, JSIOCGVERSION, &version) < 0) {
            save_errno = errno;
            close(fd);
            errno = save_errno;
            return false;
        }
        if (version == 0) {
            errno = EINVAL;
            return false;
        }
    } else {
        if (ioctl(fd, JSIOCGVERSION, &version) < 0)
            version = 0;
    }

    if (ioctl(fd, JSIOCGAXES, &axes) < 0)
        axes = 0;
    if (ioctl(fd, JSIOCGBUTTONS, &buttons) < 0)
        buttons = 0;
    if (ioctl(fd, JSIOCGNAME(sizeof(name)), name) < 0)
        *name = '\0';

    joystick = get_free_joystick();
    if (! joystick) {
        close(fd);
        errno = ENOMEM;
        return false;
    }
    player = get_available_player();

    joystick->fd = fd;
    strncpy(joystick->devnode, devnode, sizeof(joystick->devnode));
    joystick->dev = st->st_rdev;

    joystick->axes = axes;
    joystick->buttons = buttons;
    memcpy(joystick->name, name, sizeof(joystick->name));

    joystick->player = player;

    joystick->key_state = 0;
    joystick->last_key_idx = KEYPAD_NONE;
    joystick->last_key_val = false;
    memset(joysticks[num_joysticks].key_history, '\0', sizeof(joysticks[num_joysticks].key_history));

    fprintf(stderr, "initialized joystick: %s, (player: %d, axes: %d, buttons: %d, name: %s)\n", devnode, player, axes, buttons, name);

    return true;
}

void init_joystick(const char *devnode)
{
    struct stat st;

    if (! devnode)
        return;

    fprintf(stderr, "initializing joystick: %s\n", devnode);

    if (stat(devnode, &st) != 0) {
        perror(devnode);
        exit(EXIT_FAILURE);
    }

    if (! open_joystick(devnode, &st, false)) {
        perror(devnode);
        exit(EXIT_FAILURE);
    }
}

static void add_udev_device(struct udev_device *dev)
{
    size_t i;
    const char *devnode;
    struct stat st;

    if (! dev)
        return;

    devnode = udev_device_get_devnode(dev);

    if (! devnode || !*devnode) {
        fprintf(stderr, "add_udev_device: unable to get joystick name\n");
        return;
    }

    if (stat(devnode, &st) != 0) {
        fprintf(stderr, "add_udev_device: unable to stat %s\n", devnode);
        return;
    }

    for (i = 0; i < num_joysticks; i++) {
        if (joysticks[i].fd == -1)
            continue;

        if (joysticks[i].dev == st.st_rdev) {
            fprintf(stderr, "add_udev_device: joystick %s allready opened\n", devnode);
            return;
        }
    }

    if (! open_joystick(devnode, &st, true)) {
        fprintf(stderr, "add_udev_device: unable to open %s\n", devnode);
        return;
    }
}

static void remove_udev_device(struct udev_device *dev)
{
    size_t i;
    const char *devnode;

    if (! dev)
        return;

    devnode = udev_device_get_devnode(dev);

    if (! devnode || !*devnode) {
        fprintf(stderr, "remove_udev_device: unable to get joystick name\n");
        return;
    }

    for (i = 0; i < num_joysticks; i++) {
        if (joysticks[i].fd == -1)
            continue;

        if (strcmp(joysticks[i].devnode, devnode) == 0) {
            fprintf(stderr, "remove_udev_device: removed joystick %s\n", devnode);
            close(joysticks[i].fd);
            joysticks[i].fd = -1;
        }
    }
}

void init_udev_hotplug(void)
{
    struct udev_enumerate *enumerate = NULL;
    struct udev_list_entry *devices = NULL;
    struct udev_list_entry *item = NULL;
    const char *name = NULL;
    struct udev_device *dev = NULL;

    if (udev_ctx != NULL) {
        if (udev_monitor != NULL) {
            udev_monitor_unref(udev_monitor);
        }
        udev_monitor = NULL;

        udev_unref(udev_ctx);
        udev_ctx = NULL;
    }

    udev_ctx = udev_new();

    if (! udev_ctx) {
        fprintf(stderr, "init_udev_hotplug: unable to initialize udev.\n");
        exit(EXIT_FAILURE);
    }

    udev_monitor = udev_monitor_new_from_netlink(udev_ctx, "udev");
    if (udev_monitor) {
        udev_monitor_filter_add_match_subsystem_devtype(udev_monitor, "input", NULL);
        udev_monitor_enable_receiving(udev_monitor);
    } else {
        fprintf(stderr, "init_udev_hotplug: unable to initialize udev monitor.\n");
    }

    enumerate = udev_enumerate_new(udev_ctx);
    if (! enumerate) {
        fprintf(stderr, "init_udev_hotplug: unable to initialize udev enumeration.\n");
        exit(EXIT_FAILURE);
    }

    udev_enumerate_add_match_property(enumerate, "ID_INPUT_JOYSTICK", "1");
    udev_enumerate_add_match_subsystem(enumerate, "input");
    udev_enumerate_scan_devices(enumerate);
    devices = udev_enumerate_get_list_entry(enumerate);

    udev_list_entry_foreach(item, devices) {
        name = udev_list_entry_get_name(item);
        if (name) {
            dev = udev_device_new_from_syspath(udev_ctx, name);
            if (dev) {
                add_udev_device(dev);
                udev_device_unref(dev);
            }
        }
    }

    udev_enumerate_unref(enumerate);
}

static void udev_monitor_poll(void)
{
    struct pollfd fds[1];
    int ret;
    struct udev_device *dev;
    const char *val;
    const char *action;

    if (udev_ctx == NULL || udev_monitor == NULL)
        return;

    for (;;) {
        fds[0].fd = udev_monitor_get_fd(udev_monitor);
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        ret = poll(fds, ARRAY_LENGTH(fds), 0);
        if (ret != 1)
            return;

        if ((fds[0].revents & POLLIN) == 0)
            return;

        dev = udev_monitor_receive_device(udev_monitor);
        if (dev) {
            val = udev_device_get_property_value(dev, "ID_INPUT_JOYSTICK");
            action = udev_device_get_action(dev);

            if (val && strcmp(val, "1") == 0 && action) {
                if (strcmp(action, "add") == 0) {
                    add_udev_device(dev);
                } else if (strcmp(action, "remove") == 0) {
                    remove_udev_device(dev);
                } else if (strcmp(action, "change") == 0) {
                    remove_udev_device(dev);
                    add_udev_device(dev);
                }
            }

            udev_device_unref(dev);
        }
    }
}

bool read_joystick(struct joystick **joystick_ptr)
{
    size_t i;
    struct joystick *joystick = NULL;
    struct js_event event = { 0 };
    int key_idx = KEYPAD_NONE;

    udev_monitor_poll();

    if (num_joysticks <= 0)
        return NULL;

    for (i = 0; i < num_joysticks; i++) {
        if (joysticks[i].fd == -1)
            continue;

        if (read(joysticks[i].fd, &event, sizeof(event)) == sizeof(event)) {
            joystick = &joysticks[i];
            break;
        }
    }

    if (! joystick)
        return NULL;

    switch (event.type & ~JS_EVENT_INIT) {
        case JS_EVENT_BUTTON:
            switch (event.number) {
                case 8:
                    key_idx = KEYPAD_SELECT;
                    break;
                case 9:
                    key_idx = KEYPAD_START;
                    break;
                case 0:
                    key_idx = KEYPAD_B;
                    break;
                case 1:
                    key_idx = KEYPAD_A;
                    break;
                default:
                    break;
            }
            if (key_idx != KEYPAD_NONE) {
                joystick->last_key_idx = key_idx;
                joystick->last_key_val = !!event.value;
                if (joystick->last_key_val) {
                    joystick->key_state |= key_idx;
                } else {
                    joystick->key_state &= ~key_idx;
                }
            } else {
                joystick->last_key_idx = KEYPAD_NONE;
                joystick->last_key_val = false;
            }
            break;
        case JS_EVENT_AXIS:
            switch (event.number) {
                case 0:
                    if (event.value <= -1024) {
                        joystick->last_key_idx = KEYPAD_LEFT;
                        joystick->last_key_val = true;
                        joystick->key_state |= KEYPAD_LEFT;
                        joystick->key_state &= ~KEYPAD_RIGHT;
                    } else if (event.value >= 1024) {
                        joystick->last_key_idx = KEYPAD_RIGHT;
                        joystick->last_key_val = true;
                        joystick->key_state &= ~KEYPAD_LEFT;
                        joystick->key_state |= KEYPAD_RIGHT;
                    } else {
                        if (joystick->key_state & KEYPAD_LEFT) {
                            joystick->last_key_idx = KEYPAD_LEFT;
                        } else if (joystick->key_state & KEYPAD_RIGHT) {
                            joystick->last_key_idx = KEYPAD_RIGHT;
                        } else {
                            joystick->last_key_idx = KEYPAD_NONE;
                        }
                        joystick->last_key_val = false;
                        joystick->key_state &= ~KEYPAD_LEFT;
                        joystick->key_state &= ~KEYPAD_RIGHT;
                    }
                    break;
                case 1:
                    if (event.value <= -1024) {
                        joystick->last_key_idx = KEYPAD_UP;
                        joystick->last_key_val = true;
                        joystick->key_state |= KEYPAD_UP;
                        joystick->key_state &= ~KEYPAD_DOWN;
                    } else if (event.value >= 1024) {
                        joystick->last_key_idx = KEYPAD_DOWN;
                        joystick->last_key_val = true;
                        joystick->key_state &= ~KEYPAD_UP;
                        joystick->key_state |= KEYPAD_DOWN;
                    } else {
                        if (joystick->key_state & KEYPAD_UP) {
                            joystick->last_key_idx = KEYPAD_UP;
                        } else if (joystick->key_state & KEYPAD_DOWN) {
                            joystick->last_key_idx = KEYPAD_DOWN;
                        } else {
                            joystick->last_key_idx = KEYPAD_NONE;
                        }
                        joystick->last_key_val = false;
                        joystick->key_state &= ~KEYPAD_UP;
                        joystick->key_state &= ~KEYPAD_DOWN;
                    }
                    break;
                default:
                    joystick->last_key_idx = KEYPAD_NONE;
                    joystick->last_key_val = false;
                    break;
            }
            break;
        default:
            joystick->last_key_idx = KEYPAD_NONE;
            joystick->last_key_val = false;
            break;
    }

    if (joystick->last_key_idx != KEYPAD_NONE && joystick->last_key_val) {
        memmove(&joystick->key_history[1], &joystick->key_history[0], sizeof(joystick->key_history) - sizeof(joystick->key_history[0]));
        joystick->key_history[0] = joystick->last_key_idx;
    }

    *joystick_ptr = joystick;
    return true;
}

extern int count_joysticks(void)
{
    size_t i;
    int cnt = 0;

    for (i = 0; i < num_joysticks; i++) {
        if (joysticks[i].fd != -1) {
            cnt++;
        }
    }

    return cnt;
}

bool joystick_is_key_seq(struct joystick *joystick, const int *seq, size_t seq_length)
{
    size_t hi, si;

    if (! joystick)
        return false;

    if (seq_length > KEY_HISTORY_SIZE)
        return false;

    for (hi = seq_length - 1, si = 0; si < seq_length; hi--, si++) {
        if (joystick->key_history[hi] != seq[si])
            return false;
    }

    return true;
}
