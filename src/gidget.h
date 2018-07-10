/*

  Include standard routines here to avoid cluttering up gidget.c
            Charlie Brooks 2011-03-10

*/

#include <stdio.h>       /* printf */
#include <ctype.h>
#include <limits.h>
#include <errno.h>       /* perror, errno, strerror */
#include <stdlib.h>      /* exit and many others */
#include <syslog.h>      /* syslog & friends */
#include <pwd.h>         /* getpwnam */
#include <unistd.h>      /* getopt, exec */
#include <string.h>
#include <sys/types.h>   /* pid_t */
#include <sys/inotify.h>
#include <signal.h>      /* sigaction */
#include <sys/wait.h>    /* wait and wait status fns */
#include <time.h>        /* time, localtime, asctime */
#include <fcntl.h>       /* open() & friends */
//#include <sys/stat.h>	 /* for open() CREAT modes */
