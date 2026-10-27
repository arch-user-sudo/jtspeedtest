/* jtspeedtestctl — send a command to a running jtspeedtest window.
 *
 * Usage:
 *   jtspeedtestctl accent '#RRGGBB'   change the accent live
 *   jtspeedtestctl '#RRGGBB'          shorthand for the same
 *   jtspeedtestctl quit               exit the window
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static void
ctl_path(char *buf, size_t n)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !rt[0]) rt = "/tmp";
    snprintf(buf, n, "%s/jtspeedtest-ctl.sock", rt);
}

static int
hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int
parse_color(const char *s, unsigned *out)
{
    if (!s)
        return 0;
    if (*s == '#')
        s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    if (strlen(s) != 6)
        return 0;

    unsigned value = 0;
    for (int i = 0; i < 6; i++) {
        int nibble = hex_nibble(s[i]);
        if (nibble < 0)
            return 0;
        value = (value << 4) | (unsigned)nibble;
    }
    *out = value;
    return 1;
}

static void
usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s accent '#RRGGBB'|quit\n"
            "       %s '#RRGGBB'\n"
            "  accent COLOR   change the window accent while it is running\n"
            "  quit           exit the window\n",
            prog, prog);
}

int
main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    char msg[64];
    unsigned rgb;
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage(argv[0]);
        return 0;
    } else if (!strcmp(argv[1], "accent")) {
        if (argc != 3 || !parse_color(argv[2], &rgb)) {
            fprintf(stderr, "%s: accent requires a color like '#378484'\n",
                    argv[0]);
            return 2;
        }
        snprintf(msg, sizeof(msg), "accent #%06X\n", rgb);
    } else if (argc == 2 && parse_color(argv[1], &rgb)) {
        snprintf(msg, sizeof(msg), "accent #%06X\n", rgb);
    } else if (!strcmp(argv[1], "quit")) {
        if (argc != 2) {
            usage(argv[0]);
            return 1;
        }
        snprintf(msg, sizeof(msg), "quit\n");
    } else {
        usage(argv[0]);
        return 1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("jtspeedtestctl: socket");
        return 1;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    ctl_path(addr.sun_path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
                "jtspeedtestctl: cannot connect to %s (is jtspeedtest running?)\n",
                addr.sun_path);
        close(fd);
        return 1;
    }

    size_t len = strlen(msg);
    ssize_t n = send(fd, msg, len, MSG_NOSIGNAL);
    close(fd);
    return (n >= 0 && (size_t)n == len) ? 0 : 1;
}
