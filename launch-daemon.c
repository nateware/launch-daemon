/****************************************************************
 *
 * Copyright (c) Nate Wiger, http://nateware.com
 *
 * launch-daemon.c - Detach process from terminal and launch in
 * background. This is used to wrap processes that don't daemonize.
 *
 * Compile using:
 * gcc -o launch-daemon launch-daemon.c 
 *
 ****************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>

/* define where to put our output files */
#define STDERR  "stderr"
#define UMASK   0022

/* how many pending connections queue will hold */
#define BACKLOG 1

/* internal gook */
#define VERSION "1.20"
#define OPTIONS "[-h] [-s stderrfile] [-p port] [-c] [-v] [-R /chroot] [-L /ld/lib/path] [-E exename] [-u user] program [arguments ...]\n"

int CPID = -1;

/* signal catcher */
void trapper (int sig) {
    int pgrp;

    /* kill the child */
    if(CPID > 1) { 
        fprintf(stderr, "trapper child finishing for %d\n",CPID);
        kill(CPID, SIGTERM); 
        sleep(2);
        kill(CPID, SIGKILL); 
    }

    pgrp = getpgrp();
    pgrp = 0 - pgrp;
    kill(pgrp, SIGTERM);
    fprintf(stderr, "trapper received signal = %d\n", sig);
    fprintf(stderr, "trapper signalled process group = %d\n", pgrp);
    kill(pgrp, SIGKILL);
    exit(9);
}

/* open a port to listen on */
void listen_port (int port) {

    int sockfd, new_fd;  // listen on sock_fd, new connection on new_fd
    struct sockaddr_in my_addr;     // my address information
    struct sockaddr_in their_addr; // connector's address information
    socklen_t sin_size;
    struct sigaction sa;
    int yes=1;

    /* catch hangup, quit, etc when opening a port */
    signal(SIGINT,  trapper);   /* ctrl-c */
    signal(SIGHUP,  trapper);   /* kill -HUP */
    signal(SIGKILL, trapper);   /* kill -9 */
    signal(SIGQUIT, trapper);
    signal(SIGCHLD, trapper);

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(1);
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
        perror("setsockopt");
        exit(1);
    }

    my_addr.sin_family = AF_INET;            // host byte order
    my_addr.sin_port = htons(port);        // short, network byte order
    my_addr.sin_addr.s_addr = INADDR_ANY; // automatically fill with my IP
    memset(&(my_addr.sin_zero), '\0', 8); // zero the rest of the struct

    if (bind(sockfd, (struct sockaddr *)&my_addr, sizeof(struct sockaddr)) == -1) {
        perror("bind");
        exit(1);
    }
    if (listen(sockfd, BACKLOG) == -1) {
        perror("listen");
        exit(1);
    }
    while(1) {  // main accept() loop
        sin_size = sizeof(struct sockaddr_in);
        if ((new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size)) == -1) {
            perror("accept");
            continue;
        }
        printf("server: got connection from %s\n",inet_ntoa(their_addr.sin_addr));
        close(new_fd);  // parent doesn't need this
    }
}


/* daemon code is written inline */
int main (int argc, char *argv[]) {
    char *path;
    char *prog;
    char mesg[300];      /* message buf */
    int  pid, i, dofork, port, doport;

    /* option holders */
    char *rootdir    = NULL;    /* -R /rootdir   */
    char *ldpath     = NULL;    /* -L /lib/path */
    char *user       = NULL;    /* -u username  */
    char *dir        = NULL;    /* -d /base/dir */
    char *fake_exe   = NULL;    /* -E "fake_exe_name" */
    char *stderrfile = NULL;    /* -s stderr_file */

    /* passwd entry struct */
    struct passwd *pwent;

    /* options magic */
    char opt;
    extern int optind;
    struct stat lbuf;

    /* interesting tidbit */
    setenv("LAUNCH_DAEMON_VERSION", VERSION, 1);
    prog = argv[0];
    dofork = 1;
    doport = 0;
    port   = 0;
    doport = 0;

    /* options handling, but not if multiple args */
    while ((opt = getopt(argc, argv, "chp:vL:R:d:u:E:s:")) != -1) {
        switch (opt) {
        case 'R':
            rootdir = optarg;
            break;
        case 'L':
            ldpath = optarg;
            break;
        case 'E':
            fake_exe = optarg;
            break;
        case 's':
            stderrfile = optarg;
            break;
        case 'u':
            user = optarg;
            break;
        case 'd':
            dir = optarg;
            break;
        case 'c':
            dofork = 0;     /* no fork; run command ala su -c */
            break;
        case 'p':
            port = atoi(optarg);
            doport = 1;
            break;
        case '-':
            break;
        case 'h':
        case '?':
            fprintf(stderr, "Usage: %s " OPTIONS, prog);
            fprintf(stderr, "\nThis helper daemonizes a given program by wrapping up its\n"
                            "open files and input streams, setting its process group,\n"
                            "and forking into the background. Simply run it followed\n"
                            "by the command and its arguments you want to launch.\n\n");
            /* fall thru */
        case 'v':
            printf("launch-daemon version " VERSION " by Nate Wiger, http://nateware.com\n");
            exit(0);
        }
    }

    if (argc <= 1 || optind >= argc) {
        fprintf(stderr, "Usage: %s " OPTIONS, prog);
        return(2);
    }

    /* minor checks before launching */
    path = argv[optind];

    /* no longer check for path, since may be relative or in PATH
    if (lstat(path, &lbuf)) {
        sprintf(mesg, "launch-daemon: Couldn't find %s", path);
        perror(mesg);
        return(3);
    }
    */

    /* Try to find the user in /etc/passwd if -u was specified.
       Save this until *after* the chroot */
    if (user != NULL) {
        pwent = getpwnam(user);
        if (pwent == NULL) {
            sprintf(mesg, "launch-daemon: Invalid user %s", user);
            perror(mesg);
	        exit(1);
        }
    }

    /* port was passed in */
    if (port != 0 && port <= 1024) {
       sprintf(mesg, "launch-daemon: Invalid port specified (>1024)");
       perror(mesg);
       exit(1);
    }

    /* chroot if so requested */
    if (rootdir != NULL) {
        if (chdir(rootdir)) {
            sprintf(mesg, "launch-daemon: Couldn't cd to %s", rootdir);
            perror(mesg);
            exit(1);
        }
        if (chroot(rootdir)) {
            sprintf(mesg, "launch-daemon: Couldn't chroot %s", rootdir);
            perror(mesg);
            exit(1);
        }
        chdir("/");
    }

    /* Note that we must set the gid first, or else our uid has
       changed, and then it's impossible to do anything anymore! */
    if (user != NULL) {
        if (setgid(pwent->pw_gid)) {
            sprintf(mesg, "launch-daemon: Couldn't set group for %s (gid=%d)",
                    user, (int)pwent->pw_gid);
            perror(mesg);
	        exit(1);
        }
        if (setuid(pwent->pw_uid)) {
            sprintf(mesg, "launch-daemon: Couldn't set user to %s (uid=%d)",
                    user, (int)pwent->pw_uid);
            perror(mesg);
	        exit(1);
        }
    }

    /* and now change to any /base/dir */
    if (dir != NULL) {
        if (chdir(dir)) {
            sprintf(mesg, "launch-daemon: Couldn't cd to %s", dir);
            perror(mesg);
            exit(1);
        }
    }

    /* (child) fork to fall into background */
    if (dofork) {
        pid = (int)fork();
        if (pid > 0) {
            printf("%d\n", pid);
            CPID = pid;
            exit(0);    /* me */
        } else if (pid < 0) {
            sprintf(mesg, "launch-daemon: Couldn't fork %s", path);
            perror(mesg);
            return(4);
        }

        /* close all file descriptors, then reopen on /dev/null */
        if (getenv("DEBUG_LAUNCH_DAEMON") == NULL)
            for (i = getdtablesize(); i >= 0; i--) close(i);

        /* obtain a new process group */
        setsid();

        /* set umask */
        umask((mode_t)UMASK);

        /* stick both stdout and stderr into a file, in case the
         * programmer is erroneously printing things to the terminal
         */
        if(stderrfile == NULL) {
            stderrfile=STDERR;
        }
        unlink(stderrfile);
        if (getenv("DEBUG_LAUNCH_DAEMON") == NULL) {
            i = open("/dev/null", O_RDONLY);
            i = open(stderrfile, O_WRONLY|O_CREAT); dup(i);
        }
        chmod(stderrfile, (mode_t)0644);

        /* (parent) fork yet again and open a port (unprivileged) if one was specified */
        if (doport) {
            pid = (int)fork();
            if (pid > 0) {
                listen_port(port);
                waitpid((pid_t) pid, NULL, WUNTRACED);

                exit(0);
            } else if (pid < 0) {
                sprintf(mesg, "launch-daemon: Couldn't fork %s", path);
                perror(mesg);
                return(4);
            }
        }

        /* child daemon continues */
        /*
         * Ignore signals that will uselessly interrupt us.
         * Do NOT ignore SIGCHLD since doing so is undefined
         * and generally regarded as a "bad move".
         */
        signal(SIGPIPE, SIG_IGN);   /* pipe */
        signal(SIGTSTP, SIG_IGN);   /* tty */
        signal(SIGTTOU, SIG_IGN);   /* out */
        signal(SIGTTIN, SIG_IGN);   /* in */

        /* catch hangup, quit */
        signal(SIGINT,  trapper);   /* ctrl-c */
        signal(SIGHUP,  trapper);   /* kill -HUP */
        signal(SIGKILL, trapper);   /* kill -9 */
        signal(SIGQUIT, trapper);
        signal(SIGCHLD, trapper);

    }

    /* regardless of -c (fork or not) we continue here */

    /* LD_LIBRARY_PATH is stripped explicitly from some subshells, so set with -L /path */
    if (ldpath != NULL) {
        if (getenv("DEBUG_LAUNCH_DAEMON") != NULL)
            fprintf(stderr, "LD_LIBRARY_PATH=%s\n", ldpath);
        setenv("LD_LIBRARY_PATH", ldpath, 1);
    }

    /* use execvp to dup shell and exec shell scripts too */
    if (fake_exe != NULL) argv[optind] = fake_exe;
    if (execvp(path, &argv[optind])) {
        sprintf(mesg, "launch-daemon: Couldn't exec %s", path);
        perror(mesg);
        return(5);
    }

    return(0);
}

