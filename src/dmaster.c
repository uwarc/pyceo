#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <syslog.h>
#include <libgen.h>
#include <getopt.h>
#include <errno.h>
#include <netdb.h>
#include <alloca.h>
#include <fcntl.h>

#include "util.h"
#include "net.h"
#include "config.h"
#include "gss.h"
#include "daemon.h"
#include "ldap.h"
#include "kadm.h"
#include "krb5.h"
#include "ops.h"

static struct option opts[] = {
    { "detach", 0, NULL, 'd' },
    { "quiet", 0, NULL, 'q' },
    { NULL, 0, NULL, '\0' },
};

char *prog = NULL;

int terminate = 0;
int fatal_signal;

static int detach = 0;

static void usage() {
    fprintf(stderr, "Usage: %s [--detach]\n", prog);
    exit(2);
}

static void signal_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        const char *s = (sig == SIGTERM) ? "terminated" : "interrupt";
        notice("shutting down (%s)", s);
        terminate = 1;
        fatal_signal = sig;
        signal(sig, SIG_DFL);
    } else if (sig == SIGSEGV) {
        error("segmentation fault");
        signal(sig, SIG_DFL);
        raise(sig);
    } else if (sig != SIGCHLD) {
        fatal("unhandled signal %d", sig);
    }
}

static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = signal_handler;

    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
}

static void setup_pidfile(void) {
    int fd;
    size_t pidlen;
    char pidbuf[1024];
    const char *pidfile = "/var/run/ceod.pid";

    fd = open(pidfile, O_CREAT|O_RDWR, 0644);
    if (fd < 0)
        fatalpe("open: %s", pidfile);
    if (lockf(fd, F_TLOCK, 0))
        fatalpe("lockf: %s", pidfile);
    if (ftruncate(fd, 0))
        fatalpe("ftruncate: %s", pidfile);
    pidlen = snprintf(pidbuf, sizeof(pidbuf), "%d\n", getpid());
    if (pidlen >= sizeof(pidbuf))
        fatal("pid too long");
    if (full_write(fd, pidbuf, pidlen))
        fatalpe("write: %s", pidfile);
}

static void setup_daemon(void) {
    if (detach) {
        if (chdir("/"))
            fatalpe("chdir('/')");
        pid_t pid = fork();
        if (pid < 0)
            fatalpe("fork");
        if (pid)
            exit(0);
        if (setsid() < 0)
            fatalpe("setsid");

        setup_pidfile();

        if (!freopen("/dev/null", "r", stdin))
            fatalpe("freopen");
        if (!freopen("/dev/null", "w", stdout))
            fatalpe("freopen");
        if (!freopen("/dev/null", "w", stderr))
            fatalpe("freopen");
    }
}

static void setup_auth(void) {
    if (setenv("KRB5CCNAME", "MEMORY:ceod", 1))
        fatalpe("setenv");
    server_acquire_creds("ceod");
}

static void accept_one_client(int server) {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    memset(&addr, 0, addrlen);

    int client = accept(server, (sa *)&addr, &addrlen);
    if (client < 0) {
        if (errno == EINTR)
            return;
        fatalpe("accept");
    }

    pid_t pid = fork();
    if (!pid) {
        close(server);
        slave_main(client, (sa *)&addr);
        exit(0);
    }

    close(client);
}

static int master_main(void) {
    int sock, opt;
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9987);
    addr.sin_addr.s_addr = INADDR_ANY;

    sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0)
        fatalpe("socket");

    opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
        fatalpe("setsockopt");

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)))
        fatalpe("bind");

    if (listen(sock, 128))
        fatalpe("listen");

    setup_fqdn();
    setup_signals();
    setup_auth();
    setup_ops();
    setup_daemon();

    notice("now accepting connections");

    while (!terminate)
        accept_one_client(sock);

    free_gss();
    free_fqdn();
    free_ops();

    return 0;
}

int main(int argc, char *argv[]) {
    int opt;
    int ret;

    prog = xstrdup(basename(argv[0]));
    init_log(prog, LOG_PID, LOG_DAEMON, 0);

    while ((opt = getopt_long(argc, argv, "dq", opts, NULL)) != -1) {
        switch (opt) {
            case 'd':
                detach = 1;
                break;
            case 'q':
                log_set_maxprio(LOG_WARNING);
                break;
            case '?':
                usage();
                break;
            default:
                fatal("error parsing arguments");
        }
    }

    configure();

    if (argc != optind)
        usage();

    ret = master_main();

    free_config();
    free(prog);

    return ret;
}
