/*
          **********************************
              ********  GIDGET  ********
          **********************************

     Filesystem event triggered script executor
      Lou Goddard & Charlie Brooks  2011-02-15

   Fields in conf file:
     1) Path to file or directory to monitor
     2) bitmapped mask of events to trigger on
     3) Script or process to run when triggered
     4) user ID that will run the script or process
     5) email address to receive output

 Example:
 /home/gidget/xmas-list.txt:24:/usr/bin/call_santa.sh:nobody:gidget@example.com

    It is impossible to programmatically predict how 
    many related or unrelated events will occur at any
    given time.  We can detect events being discarded
    due to event queue overflow, but that's it.  So
    choose the minimum mask ALWAYS when setting up a
    trick for gidget, or play event loss russian roulette.

         See man inotify for all possible actions
     bitmapping for actions is defined in inotify.h

*/

#define GVERSION "1.01"
#define DEFAULT_CONFIG_FILE "/etc/gidget.conf"
#define MAX_CONFIG_NAME_LEN 256
#define DEFAULT_LOG_FILE "/var/log/gidget"
#define MAX_LOG_NAME_LEN 256
#define DEFAULT_PID_FILE "/var/run/gidget.pid"
#define MAX_PID_NAME_LEN 128

// It's important that MAX_ERR_TXT_LEN be large enough to hold
// error messages that may include the names of fully pathed
// files and significant amounts of diagnostic text.  Be aware
// that modern file systems allow incredibly long names!
#define MAX_ERR_TEXT_LEN 1640

#include "gidget.h"              // stdio and friends
#include "gidgetmail.h"          // define mailer here

// Gidget does tricks!  Each trick is defined by the
// content of a dynamically allocated data structure

  typedef struct {
      int32_t watchHandle;  // inotify watch descriptor
      uint32_t actions;     // bitmap of what to watch for
      char *fileName;       // file or directory to be watched
      char *script;         // executable object to run
      char *userid;         // user who will run script
      char *mail;           // email to recieve script output
  } trick_t;

// inotify_event is defined in sys/inotify.h

  typedef struct inotify_event event_t;

// global used by signal handler to pass signal back
 
  sig_atomic_t signalCaught;

// simple struct for command line options

  typedef struct {
      int daemon;
      int verbose;
      int log2file;
      int syslog;
      int sloglev;
      char config[MAX_CONFIG_NAME_LEN];
      char logfile[MAX_LOG_NAME_LEN];
      char pidfile[MAX_PID_NAME_LEN];
  } opts_t;

// function prototypes, actual functions are after main()

  void usage(FILE *fh);
  opts_t gig_opts(int argc, char **argv);
  static void signalTrap(int sig, siginfo_t * siginfo, void *context);
  static void reopenLogs(opts_t opt);
  void logx(int xstatus, opts_t opt, char logtxt[]);
  static void stringifyEventBits(uint32_t bitMap);

/*******  Hajime, let it begin *******/

int main(int argc, char **argv) {

// limit number of characters in a pathed script name
    const int maxScriptLen = 256;

// also limit length of an email address
// email will not be checked for syntax or existence
    const int maxEmailLen = 36;

// single ASCII characters used for path composition and munging
    const char space[] = { 32, 0 };
    const char apostrophe[] = { 39, 0 };
    const char slash[] = { 47, 0 };

// trickHeap is a pointer we will point at the first of a contiguously
// allocated series of anonymous pointers to trick data structures
// randomly allocated from available system heap memory.  Happily C
// will let us reference specific tricks as *trickHeap[trick_number]
    trick_t **trickHeap = NULL;

// to keep the code readable, use a one-trick pony to pass trick data
// in and out of the aforementioned configuration data structure 
    trick_t pony;

// avoid buffer overruns
// sysconf and pathconf let us use run-time values of system
// limits rather than compile-time values from limits.h
    int maxLineLen = sysconf(_SC_LINE_MAX);
    int maxUidLen = sysconf(_SC_LOGIN_NAME_MAX);
    int maxNameLen = 0;     // will be set later with pathconf

    char confLine[maxLineLen];
    char confToken[maxLineLen];
    char logtxt[MAX_ERR_TEXT_LEN];

// I do not know what the best programming style for C is because I rarely
// use the language at all.  I am considering declaring all variables at time
// of use, which would mean the following declarations would have to be moved
    int lineLen, recordLen, fieldNo, fieldLen;
    int tokenStart, tokenLen;
    int i, j, m; // dummies, mostly for string and loop indexing

// it's best to be paranoid about file creation
    umask(027);

// parse command line options
    opts_t opt = gig_opts(argc, argv);

// define a syslog socket if called for
    if (opt.syslog)
        openlog ("gidget", LOG_CONS | LOG_PID, LOG_DAEMON);

// open config file before daemonizing, to allow use of relative
// file names and to avoid creating log and pid files if the
// configuration file does not exist or can't be opened
    FILE *configFile;
    configFile = fopen(opt.config, "r");
    if (NULL == configFile) {
        sprintf(logtxt,"Error (%u) opening %s: %s", 
                 errno, opt.config, strerror(errno));
        logx(1, opt, logtxt);
    }

// redirect stdout and stderr if logging to file
    if (opt.log2file) reopenLogs(opt); 

// if -d option, then daemonize and create pidfile
    pid_t pid, ppid;
    if (opt.daemon) {
        fflush(stderr);
        fflush(stdout);
        pid = fork();
        if (pid < 0)
            logx(2, opt, "Unable to fork daemon process");
        if (pid > 0) {
            FILE *fp;
            if (NULL == (fp = fopen(opt.pidfile, "w"))) {
                kill (pid, SIGTERM);  // up with this we will not put
                logx(1, opt, "Could not create pid file, killing daemon");
            }
            fprintf(fp, "%d\n", pid);
            fclose(fp);
            exit(EXIT_SUCCESS); // parent exits normally
        }
        if (setsid() < 0)
            logx(2, opt, "Unable to set new process group");
        freopen ("/dev/null","r",stdin);  //daemon needs no kb
        if (chdir("/") < 0)
            logx(2, opt, "Unable to change working directory to root");
    }

// remember daemon process ID so that children can
// produce useful event and error messages if more
// than one gidget process is running
    if ((pid = getpid()) > 0) {
        ppid = pid;
    } else {
        logx(3, opt, "Unable to get daemon pid");
    }

// always log startup (logx does not exit if status 0)
    logx(0, opt, "daemon initialization");

// create another file handle, this one for an inotify instance.
// write operations to inotify must be done with specialised functions
// like inotify_add_watch, but reads are done with generic unix file
// read operations against the instance handle 
    int instanceHandle;
    instanceHandle = inotify_init();
    if (instanceHandle < 0)
        logx(4, opt, "Unable to initialize iNotify");


// in order to support comment lines in the configuration file
// we need to keep separate line and record counters
    int lineNo = 0;
    int trickCount = 0;

// if any field in a configuration line fails syntax checking
// badPony is set to something other than zero.  Using a flag
// instead of just jumping to the next line whenever a bogus
// field is encountered lets us give better diagnostics
// which users will almost certainly appreciate
    int badPony;


/************************************
           begin configuration parse / inotify setup loop
                                  *********************************/

// read a line from config file
// fgets error checking is outside the loop
    while (fgets(confLine, maxLineLen, configFile) != NULL) {
        lineLen = strlen(confLine) - 1;
        lineNo++;

        fieldNo = 0;    // no fields found yet, field count starts at one
        tokenStart = 0; // position of first character in current field
        badPony = 0;    // He rides across the nation, the Thoroughbred of Sin

// step through characters until EOL or comment delimiter found
        for (recordLen = 0;
             ((confLine[recordLen] != '\0')
              && (confLine[recordLen] != '#')); recordLen++) {

// you can use vim -b configfile to fix invisible characters
            if (isprint(confLine[recordLen]) == 0) {
                if (confLine[recordLen] != '\n') {
                    int charPos = recordLen + 1;
                    sprintf(logtxt, 
                         "invisible character in file %s line %d position %d",
                          opt.config, lineNo, (recordLen + 1));
                    logx(0, opt, logtxt);
                    badPony = 1;
                }
            } else {
                if (confLine[recordLen] == apostrophe[0]) {
                   sprintf(logtxt,
                         "illegal character in file %s line %d position %d",
                          opt.config, lineNo, (recordLen + 1));
                    logx(0, opt, logtxt);
                    badPony = 1;
                }
            }

// if field delimiter found, extract a token from the currently known field
            if ((confLine[recordLen] == ':')
                               || (confLine[recordLen] == '\n')) {

                fieldNo++;
                fieldLen = recordLen - tokenStart;
                confToken[fieldLen] = '\0';
                strncpy(confToken, confLine + tokenStart, fieldLen);

    // for each field, check token syntax while loading up the pony
                switch (fieldNo) {

                case 1:
                    m = pathconf(confToken, _PC_NAME_MAX);
                    if (m <= 0) {
                      // it is not possible in ISO standard C to test existence of a file
                      // however in our implementation pathconf gives us a reliable hint
                        sprintf(logtxt,
                            "Can't determine max file name length for filesystem hosting %s",
                            confToken);
                        logx(0, opt, logtxt);
                        badPony = 1;
                    } else {
                        if (m > maxNameLen) {
                            maxNameLen = m;
                            if (opt.verbose) {
                                sprintf(logtxt, "Maximum file name length set to %d...",m);
                                logx(0, opt, logtxt);
                            }
                        }
                        pony.fileName = malloc((fieldLen + 1) * sizeof(*pony.fileName));
                        if (pony.fileName == NULL) {
                            sprintf(logtxt,
                                 "Can't allocate memory for file name %s in line %d",
                                 confToken, lineNo);
                            logx(0, opt, logtxt);
                            badPony = 1;
                        } else {
                            strcpy(pony.fileName, confToken);
                        }
                    }
                    break;

                case 2:
                    for (i = 0; i < fieldLen; i++) {
                        if (!isdigit(confToken[i]))
                            break;
                    }
                    if (i == fieldLen) {
                        pony.actions = atoi(confToken);
                    } else {
                        sprintf(logtxt,
                             "ERROR: non-numeric event mask in %s line %d field 2",
                             opt.config, lineNo);
                        logx(0, opt, logtxt);
                        badPony = 2;
                    }
                    break;

                case 3:
                    if (fieldLen > maxScriptLen) {
                        sprintf(logtxt,
                             "ERROR: script name too long in %s line %d field 3",
                             opt.config, lineNo);
                        logx(0, opt, logtxt);
                        badPony = 3;
                    } else {
                        pony.script =
                            malloc((fieldLen + 1) * sizeof(*pony.script));
                        if (pony.script == NULL) {
                            sprintf(logtxt,
                                 "Can't allocate memory for script name %s in line %d",
                                 confToken, lineNo);
                            logx(0, opt, logtxt);
                            badPony = 3;
                        } else {
                            strcpy(pony.script, confToken);
                        }
                    }
                    break;

                case 4:
                    if (fieldLen > maxUidLen) {
                        sprintf(logtxt,
                             "ERROR: script name too long in %s line %d field 3",
                             opt.config, lineNo);
                        logx(0, opt, logtxt);
                        badPony = 4;
                        break;
                    }
                    pony.userid =
                        malloc((fieldLen + 1) * sizeof(*pony.userid));
                    if (pony.userid == NULL) {
                        sprintf(logtxt,
                             "Can't allocate memory for username %s in line %d",
                             confToken, lineNo);
                        logx(0, opt, logtxt);
                        badPony = 4;
                        break;
                    }
                    strcpy(pony.userid, confToken);
                    break;

                case 5:
                    if (fieldLen > maxEmailLen) {
                        sprintf(logtxt,
                             "Email address too long in %s line %d field 5",
                             opt.config, lineNo);
                        logx(0, opt, logtxt);
                        badPony = 7;
                    } else {
                        pony.mail = malloc((fieldLen + 1) * sizeof(*pony.mail));
                        if (pony.script == NULL) {
                            sprintf(logtxt,
                                 "Can't allocate memory for email address %s in line %d",
                                 confToken, lineNo);
                            logx(0, opt, logtxt);
                            badPony = 8;
                        } else {
                            strcpy(pony.mail, confToken);
                        }
                    }
                    break;

                default:
                    sprintf(logtxt,
                           "TOO MANY FIELDS IN LINE %d - DISCARDING %s!",
                           lineNo, confToken);
                    logx(0, opt, logtxt);
                    break;
                }    // end case token

                tokenStart = recordLen + 1;
            }     // end for each field

        }       // end for each record

// silently skip empty lines and full-line comments
        if (fieldNo != 0) {

// if all syntax checks were passed the pony is ready to be loaded into heap
            if ((badPony) || (fieldNo < 5)) {
                sprintf(logtxt, "ERROR: discarding %s line %d!", opt.config, lineNo);
                logx(0, opt, logtxt);

            } else {

// An inotify watch list will be built and passed to the kernel
// which will contain one inode watch for each gidget trick

                pony.watchHandle =
                    inotify_add_watch(instanceHandle, pony.fileName,
                                      pony.actions);
                if (pony.watchHandle < 0) {
                    sprintf(logtxt,
                         "ERROR %d: Unable to add watch for %s\t%s (%u)",
                         pony.watchHandle, pony.fileName, strerror(errno),
                         errno);
                    logx(0, opt, logtxt);
                    sprintf(logtxt, "ERROR: discarding %s line %d!", opt.config, lineNo);
                    logx(0, opt, logtxt);

                } else {
                    if (pony.watchHandle != (trickCount + 1)) {
                        sprintf(logtxt, "%s %s %s",
                               "FATAL ERROR!",
                               "Heap corrupt, non-sequential watch descriptor",
                               "returned from inotify!");
                        logx(2, opt, logtxt);
                    }
                    if (opt.verbose) {
                        sprintf(logtxt, "Added watch %s mask %#.8x handle %d.",
                           pony.fileName, pony.actions, pony.watchHandle);
                        logx(0, opt, logtxt);
                    }

    // extend the array of pointers to tricks with realloc
    //     (first use degrades gracefully to malloc)
                    trickHeap =
                        (trick_t **) realloc(trickHeap,
                                             (trickCount +
                                              1) * sizeof(trick_t *));
                    if (trickHeap == NULL) {
                        sprintf(logtxt, "%s %s at %s line %d!",
                               "FATAL ERROR!",
                               "Unable to allocate additional memory",
                               opt.config, lineNo);
                        logx(3, opt, logtxt);
                    }
    // allocate another block of memory for this trick with malloc
    // and point the newly created pointer at the newly created trick struct
                    trickHeap[trickCount] =
                        (trick_t *) malloc(sizeof(trick_t));
                    if (trickHeap[trickCount] == NULL) {
                        sprintf(logtxt, "%s %s at %s line %d!",
                               "FATAL ERROR!",
                               "Unable to allocate additional memory",
                               opt.config, lineNo);
                        logx(4, opt, logtxt);
                    }
    // unload pony into trick heap and increment number of tricks
                    *trickHeap[trickCount++] = pony;
                }
            }
        }
    } // elihw, loop back for next configuration record


/************************************
           end configuration parse / inotify setup loop
                                  *********************************/


// if we're not at end of file, the file read function blew up 
    if (!feof(configFile)) {
        sprintf(logtxt, "Error reading %s: %s (%u)",
                 opt.config, strerror(errno), errno);
        logx(5, opt, logtxt);
    }

// close that file, were you raised in a barn?
    fclose(configFile);  // no error check, we die soon anyway

// debuggery - dump the data structures in toto
    if (opt.verbose) {
        fflush(stdout);
        fflush(stderr);
        printf("\nMax userid length is %d\n", maxUidLen);
        printf("Max input line length is %d\n", maxLineLen);
        printf("Max file name length returnable by a watch is %d\n\n",
               maxNameLen);

        for (j = 0; j< trickCount; j++) {
            printf("\ntrick number%d\n",j);
            printf("real trick location is %zu\n",&(trickHeap[j]));
    // the following assumes we are 64 bit - which we indeed are
            unsigned long int l = (unsigned long int) &trickHeap[0] + (j * sizeof(trick_t *));
            printf("which should equal %zu\n",l);
            printf("thing to watch: %s\n",trickHeap[j]->fileName);
            printf("decimal event mask bitmap: %d\n",trickHeap[j]->actions);
            printf("hex event mask bitmap: %#.8x\n",trickHeap[j]->actions);
            printf("script to execute: %s\n",trickHeap[j]->script);
            printf("userid for script execution: %s\n",trickHeap[j]->userid);
            printf("email to receive output: %s\n",trickHeap[j]->mail);
            printf("watch descriptor assigned to trick: %zu\n",trickHeap[j]->watchHandle);
        }
    }

// prevent excessive output buffering
    fflush(stdout);
    fflush(stderr);

// we're going to be forking out responses to file system events and
// ignoring what happens to the children once they've forked off...
// so we need to set up a signal trap that will auto-reap the dying
// children to prevent an undesirable proliferation of zombies

    struct sigaction reapchildren, oldChldAct;
    memset(&reapchildren, 0, sizeof(reapchildren));
    memset(&oldChldAct, '\0', sizeof(oldChldAct));
    reapchildren.sa_flags = SA_NOCLDWAIT;
    if (sigaction(SIGCHLD, &reapchildren, 0) < 0) {
        logx(6, opt, "could not set up SIGCHLD auto reaper");
    }
// a couple more signal traps before starting read loop
// these let us terminate cleanly on various interupts
    struct sigaction newAction, oldTermAct, oldIntAct, oldHupAct;
    memset(&newAction, '\0', sizeof(newAction));
    memset(&oldIntAct, '\0', sizeof(oldIntAct));
    memset(&oldTermAct, '\0', sizeof(oldTermAct));
    memset(&oldHupAct, '\0', sizeof(oldHupAct));

// if the SA_SIGINFO flag is set, the signal handler is called as
// void handler(int signo, siginfo_t *info, void *context);
// instead of the usual void handler(int signo); so you get a
// pointer to a siginfo structure and a pointer to an identifier
// for the process context as well as the usual signal number
    newAction.sa_flags = SA_SIGINFO;
// must use sa_sigaction not sa_handler when using SA_SIGINFO
    newAction.sa_sigaction = &signalTrap;

// kill and killall will raise SIGTERM
    if (sigaction(SIGTERM, &newAction, &oldTermAct) < 0) {
        logx(6, opt, "could not set trap for SIGTERM");
    }

// control-c from the terminal raises SIGINT
    if (sigaction(SIGINT, &newAction, &oldIntAct) < 0) {
        logx(6, opt, "could not set control-c trap");
    }

// logrotate will use a SIGHUP to tell us to reopen logging
    if (sigaction(SIGHUP, &newAction, &oldHupAct) < 0) {
        logx(6, opt, "could not set trap for SIGHUP");
    }

/************************************
                   begin inotify read/wait loop
                                  *********************************/

// whenever any filesystem event(s) occur for which a watch
// exists, the kernel will generate corresponding inotify
// event object(s).  Each event object may or may not be 
// followed by a string containing additional information
// (usually a file name) and may or may not contain some
// additional information relating to memory overflow or
// other error conditions we probably need to know about 

// During the configuration parse we interrogated our filesystems
// to determine the longest possible file name that could be sent
// back by inotify.  That (plus overhead bytes) will determine 
// the size of our read buffer.  Program will block on the read
// until a valid event occurs.

    int len, eventID = 0;
    int maxEventBufSize = sizeof(struct inotify_event) + maxNameLen + 1;
    char buf[maxEventBufSize];


    while (pid > 0) {
        errno = 0;          // errno is not guaranteed clean so scrub it

        len = read(instanceHandle, buf, maxEventBufSize);
        //possible results are signal, event, or weird error

        if (errno == EINTR) {
            sprintf(logtxt, "Caught signal %d", signalCaught);
            switch (signalCaught) {

              case SIGHUP:
                if (opt.log2file) {
                    strcat(logtxt, ", reopening stdout/stderr");
                    logx(0, opt, logtxt);
                    reopenLogs(opt);
                } else {
                    strcat(logtxt, ", ignored.");
                    logx(0, opt, logtxt);
                }
                break;

              case SIGINT:
                strcat(logtxt, ", probably Control-C");
                logx(0, opt, logtxt);
                // do not break

              default:
                logx(0, opt, "gidget event wait terminated by signal, shutting down.");
                close(instanceHandle);
                if (opt.syslog) closelog();
                exit(EXIT_SUCCESS);          /*******  NORMAL DAEMON EXIT  *******/
                break;
            }

        } else {
            if (len > 0) {
                pid = fork();      // Clone off a child to handle the event
            } else {
                if (len == 0) {
                    sprintf(logtxt, "zero length string returned from inotify, daemon dead");
                } else {
                    sprintf(logtxt, "inotify returned %d, FAIL, daemon dead", len);
                }
                logx(7, opt, logtxt);   /******** INOTIFY FAILURE EXIT  *******/
            }
        }
    }

/************************************
                   end  inotify read/wait loop
                                  *********************************/


//  Escaping the while loop without exiting the program can
//  only happen on a sucessful fork or a fork error

    if (pid < 0) {
        logx(8, opt, "failed to fork script executor child process!");
    }


// If execution reaches this point we are the child

    if ((pid = getpid()) < 0) {
        logx(1, opt, "Unable to get event child pid");
    } else {
        if (opt.verbose) {
            sprintf(logtxt, "spawned event child process %d", pid);
            logx(0, opt, logtxt);
        }
    }

// children have no use for all those signal traps the daemon needed
    if (sigaction(SIGCHLD, &oldChldAct, NULL) < 0) {
        logx(10, opt, "Unable to release SIGCHLD trap");
    }
    if (sigaction(SIGTERM, &oldTermAct, NULL) < 0) {
        logx(10, opt, "Unable to release SIGTERM trap");
    }
    if (sigaction(SIGINT, &oldIntAct, NULL) < 0) {
        logx(10, opt, "Unable to release SIGINT trap");
    }
    if (sigaction(SIGHUP, &oldHupAct, NULL) < 0) {
        logx(10, opt, "Unable to release SIGHUP trap");
    }

// Only the parent should hold the watches open
    close(instanceHandle);

// cast the raw event buffer into an inotify event structure
// so we can figure out what the hell is going on here
    struct inotify_event *event;
    event = (struct inotify_event *) &buf[0];

// more debuggery
    if (opt.verbose) {
        printf("\n%s", trickHeap[((event->wd) - 1)]->fileName);
        if (event->len != 0) printf("/%s", event->name);
        printf(" watch=%d mask=%zu cookie=%zu len=%u\n",
                 event->wd, event->mask, event->cookie, event->len);
        uint32_t dummy = event->mask;
        stringifyEventBits(dummy);  //converts events to readable form
    }

// returned events are matched against known tricks by watch descriptor
// find the appropriate trick and load our faithful pony
    pony = *trickHeap[(event->wd) - 1];


/************************************
   build the fully pathed name of the triggering filesystem object
                                     *********************************/

/* Two issues to deal with:

      1) inotify often supplies multiple trailing nulls on the .name field
         so that additional events will be block aligned.
      2) there may be absurd meta-characters in the object name - people make
         files named Ph'nglui mglw'nafh Cthulhu R'lyeh wgah'nagl fhtagn!

   Three rules to accomodate the second issue:

    rule 1: script author is on his own - we cannot help
    rule 2: always single-quote the pathed fileOrFolder name
    rule 3: no internal single quotes allowed - munge and report
*/

    int terminus=0;
    char fileOrFolder[maxNameLen], *p, *q;
    char mungeChar[4] = "%27\0";    //  MS clickable version of apostrophe
//    char mungeChar[] = "&#39\0";  // standardized HTML encoding method

    p = &pony.fileName[0];
    q = &fileOrFolder[0];
    while ((*q++ = *p++) != '\0') terminus++;
    fileOrFolder[terminus++]=slash[0];
    for (i=0; ((i<=event->len) && (event->name[i]!='\0')); i++) {
        if (event->name[i] == apostrophe[0]) {
            p=&mungeChar[0];
            q=&fileOrFolder[terminus];
            while ((*q++ = *p++) != '\0') terminus++;
        } else {
            fileOrFolder[terminus++]=event->name[i];
        }
        if (terminus > maxNameLen) {
          logx(13, opt, "filesystem object name overflow!");
        }
    }

// test for backing filesystem unmount event
    if (event->mask & IN_UNMOUNT) {
        sprintf(logtxt,
               "GRIEVOUS ERROR: filesystem backing %s unmounted!",
                pony.fileName);
        logx(0, opt, logtxt);
        // this should set off as many alarms as possible!
        // at minimum alert sysadmins, operators, apps
    }

// test to see if events are being discarded
    if (event->mask & IN_Q_OVERFLOW) {
        logx(0, opt, "GRIEVOUS ERROR: inotify event queue overflow!");
        // this should set off as many alarms as possible!
        // at minimum alert sysadmins, operators, apps
    }

// test to see if a watch just got blown away
    if (event->mask & IN_IGNORED) {
        sprintf(logtxt,
               "WARNING: gidget watch on %s deleted!",
               pony.fileName);
        logx(0, opt, logtxt);
        // this should set off as many alarms as possible!
        // at minimum alert sysadmins, operators, apps
    }

// Doing the getpwnam() call down here after the fork
// insulates parent daemon from NSS memory leaks and allows
// user account changes to propagate without restarting gidget

    struct passwd *pwd = malloc(sizeof(struct passwd));
    if (pwd == NULL) {
        sprintf(logtxt,
                "Couldn't allocate memory to look up user %s",
                pony.userid);
        logx(17, opt, logtxt);
    }
    memset(pwd, 0, sizeof(struct passwd));

// newfangled getpwnam_r() takes more setup than getpwnam() did
    size_t pbuffer_len = sysconf(_SC_GETPW_R_SIZE_MAX) * sizeof(char);
    char *pbuffer = malloc(pbuffer_len);
    if (pbuffer == NULL) {
        sprintf(logtxt,
                "unable to allocate buffer for getpwnam_r, user %s",
                pony.userid);
        logx(18, opt, logtxt);
    }
// calling the Name Service Switch, come in NSS, do you copy
    getpwnam_r(pony.userid, pwd, pbuffer, pbuffer_len, &pwd);
    if (pwd == NULL) {
        sprintf(logtxt,
                "getpwnam_r failed to find user %s",
                pony.userid);
        logx(19, opt, logtxt);
    }
// get the current user's shell for executing script later
    size_t shell_len = strlen(pwd->pw_shell);
    if (shell_len <= 0) {
        sprintf(logtxt,
                "unable to determine shell for user %s",
                pony.userid);
        logx(20, opt, logtxt);
    }
    char *shell = malloc(shell_len);
    shell = pwd->pw_shell;

// build a command composed of script 'filename' eventmask
// script could already have trailing arguments, we don't care
    char command[maxLineLen];
    char eventMask[16];

    sprintf(eventMask, "%#.8x", event->mask);        // hex
//  sprintf(eventMask,"%d",event->mask);  //d-d-d-digital

//  there's a nasty buffer overflow potential building command
    if ((strlen(pony.script) + strlen(eventMask) +
         strlen(fileOrFolder) + 4) > maxLineLen) {
        logx(22, opt, "command too long for shell");
    }
        
    strcpy(command, pony.script);
    strcat(command, space);
    strcat(command, apostrophe);
    strcat(command, fileOrFolder);
    strcat(command, apostrophe);
    strcat(command, space);
    strcat(command, eventMask);

/******************************************************************
   We want to run the command or script associated with the event
   and email any output or error messages to the appropriate user.
   I stole the idea from Paul Vixie, but the implementation is mine
*******************************************************************/

// set up a pipe to manage script output
// Since Linux 2.6.11, pipe capacity is 65536 bytes.
// Linux pipes are NOT bidirectional, ARE POSIX-compliant

    int pipehandle[2];
    if (pipe(pipehandle) == -1) {
        logx(24, opt, "unable to create mail pipe");
    }

// ..Primary logging action..  This program should emit no other
// output except error messages and startup/shutdown unless verbose
// mode has been specifically selected by the user at run time

    if (opt. verbose) {
        sprintf(logtxt,
                "parentpid [%d] watch %d, mask %d, user %s, dir %s, shell %s, mail %s, %s",
                ppid, event->wd, event->mask, pony.userid, pwd->pw_shell, pwd->pw_dir, pony.mail, command);
    } else {
        sprintf(logtxt, "Executing %s using shell %s with output to %s", command, pwd->pw_shell, pony.mail);
    }
      logx(0, opt, logtxt);

// environment has been built, so it's time to fork
    pid = fork();

    if (pid == -1) {
        logx(25, opt, "unable to fork script executor");
    }

// (grand)child will execute the user script
// note, in valgrind the dup2 generates a spurious EBADF warning
    if (pid == 0) {
        close(pipehandle[0]);        // close read end (0) of pipe
        dup2(pipehandle[1], 1);        // make stdout (1) write end (1) of pipe
        dup2(1, 2);                // make stderr (2) same as stdout (1)
        close(pipehandle[1]);        // close redundant handle
        //  set current folder to home dir of executing userid
        if (chdir(pwd->pw_dir) < 0) {
            sprintf(logtxt, "unable to chdir to user %s home folder %s",
                   pony.userid, pwd->pw_dir);
            logx(26, opt, logtxt);
        }
        // set gid to primary group of executing user
        if (setgid(pwd->pw_gid) < 0) {
            sprintf(logtxt, "unable to set user %s primary group %d",
                   pony.userid, pwd->pw_gid);
            logx(27, opt, logtxt);
        }
        // set uid last because we lose root privileges
        if (setuid(pwd->pw_uid) < 0) {
            sprintf(logtxt, "unable to set user %s uid %d",
                   pony.userid, pwd->pw_uid);
            logx(28, opt, logtxt);
        }
        // Lay on, Macduff, and damn'd be he that first cries 'Hold, enough!'
        execl(shell, shell, "-c", command, (char *) NULL);
        logx(29, opt, "execl script FAILED"); // should never be reached
    }

// if the script outputs anything, it will need to be emailed, so
// build a timestamp instead of trusting the local email transport
// to be properly configured.  Use fundamentally stupid traditional
// Unix time format in order to be extremely SMTP friendly

    time_t unixEpochTime;  // only YOU can prevent the Y2.038K disaster
    char tmbuf[26], *mailTime;

    unixEpochTime = time(NULL);
    mailTime = ctime_r(&unixEpochTime, tmbuf);
    mailTime[24]=0; // strip unnneccccesssary newline

// parent (of grandchild) will receive output from script, if any
// this will block until something gets written or EOF
    if (pid > 0) {
        close(pipehandle[1]);        // close write end (1) of pipe

        int bytesMailed = 0;
        FILE *fromPipe = fdopen(pipehandle[0], "r");
        int ch = getc(fromPipe);
        if (ch != EOF) {        // we got fish on the hook!
            // fire up a mail process
            FILE *mailslot;
            // and stuff the mail into it
            if (mailslot = popen(MAILCOMMAND, "w")) {
                // boilerplate mail headers
                fprintf(mailslot, "From: %s (gidget)\n", pony.userid);
                fprintf(mailslot, "To: %s\n", pony.mail);
                fprintf(mailslot, "Subject: gidget event: %s\n",
                        fileOrFolder);
                fprintf(mailslot, "Date: %s\n", mailTime);
                // gidget is RFC3834 section 5.1 compliant
                // scalix autoresponder is non-compliant though
                fprintf(mailslot, "Auto-Submitted: auto-generated\n");
                // clues for the exceptionally clever or observant (hi there!)
                fprintf(mailslot, "X-gidget-object: %s\n", fileOrFolder);
                fprintf(mailslot, "X-gidget-watch: %d\n", event->wd);
                fprintf(mailslot, "X-gidget-mask: %d\n\n", event->mask);
                fprintf(mailslot, "%s -c %s:\n\n", shell, command);
                putc(ch, mailslot);        // first char
                while ((ch = getc(fromPipe)) != EOF) {
                    bytesMailed++;
                    putc(ch, mailslot);
                }
                fflush(mailslot);
                pclose(mailslot);
            }
        }

        if (bytesMailed != 0) {
            sprintf(logtxt, 
                    "parentpid [%d] mailed %d bytes of output to %s",
                    ppid, bytesMailed, MAIL_TRANSPORT);
            logx(0, opt, logtxt);
        }

        int cstatus;
        if (waitpid(pid, &cstatus, 0) == -1) {
            sprintf(logtxt, "unable to obtain exit status of grandchild [%d]%s",
                    pid, pony.script);
            logx(29, opt, "execl mail FAILED");
        }

// Returned status is not the bash exit value, and mucking about with the bits is not
// portable across architectures, so use the status evaluation macros from waitpid()
// WIFEXITED(i) evaluates to a non-zero value if child terminated normally & status available
// WEXITSTATUS(i) evaluates to the low-order 8 bits of the status returned by the child

        int shstatus=EXIT_FAILURE;       // always plan for boneheadedness

        if (WIFEXITED(cstatus) == 0) {
            sprintf(logtxt,
                    "FATAL ERROR: unable to determine exit status of script %s",
                    pony.script);
        } else {

            shstatus = WEXITSTATUS(cstatus); /* extract lower order 8 bits */

            switch (shstatus) {

            case 127:
              sprintf(logtxt, "Script %s returned ambiguous result", pony.script);
              logx(0, opt, logtxt);
              sprintf(logtxt, "scripts to be executed by %s should never be written to return status 127", argv[0]);
              break;

            case 0:
              if (opt.verbose) {
                  sprintf(logtxt, "child process successfully executed %s", command);
              } else {
                  sprintf(logtxt, "script executor grandchild process successful completion");
              }
              break;

            default:
              sprintf(logtxt, "script fail, %s returned returned status %d", command, shstatus);
              break;
            }
        }
/*
   This is where any process cleanup should occur.

   Some C programming gurus say not to free up dynamically allocated
   memory before exiting because it will be reclaimed during process
   cleanup and it's inefficient to do this costly operation twice.
   I am holding my tidiness instinct in check to follow this advice.
        free(pwd);
        free(pbuffer);
        free(trickHeap);
*/
        logx(shstatus, opt, logtxt);
        exit(shstatus);  // only reached if shstatus is zero
    }

    logx(255, opt, "The sky is falling!  The sky is falling!");  // should never happen
}

/*
               **********************************
           ************ GIDGET FUNCTIONS ************
               **********************************

*/

// Always be kind to your users, or they will not be kind to you
void usage(FILE *fh) {
    fprintf(fh,"\nRun programs when specific filesystem events occur\n");
    fprintf(fh,"\nUsage: gidget [OPTION]\n");
    fprintf(fh,"\t-c filename\toverride default configuration file\n");
    fprintf(fh,"\t-d         \trun as a system daemon, using pid & log files\n");
    fprintf(fh,"\t-l logfile \toverride default error and event logging\n");
    fprintf(fh,"\t-p pidfile \toverride default daemon process id file\n");
    fprintf(fh,"\t-s [n]     \tuse syslog to log events at level n\n");
    fprintf(fh,"\t-V         \tprint version string\n");
    fprintf(fh,"\t-v         \tbe exceptionally verbose\n");
    fprintf(fh,"\t-?         \tthese messages\n");
    fprintf(fh,"\nNOTE syslog levels are 0-7, higher number indicating lower priority\n\n");
    fprintf(fh,"Warnings and significant events will be logged to stdout unless\n");
    fprintf(fh,"a logfile is requested or gidget is running as a daemon.\n\n");
    exit(1);
}

// determine run-time options
opts_t gig_opts(int argc, char **argv) {

// default log level if syslog is invoked is 1 (LOG_ALERT)
    opts_t opt={0,0,0,0,0}; // no-verbose, no-daemon, no-logfile, no-syslog
    strcpy(opt.config, DEFAULT_CONFIG_FILE);
    strcpy(opt.logfile, DEFAULT_LOG_FILE);
    strcpy(opt.pidfile, DEFAULT_PID_FILE);

    char o;
    while ((o = getopt (argc, argv, ":dVvc:l:p:s:")) != -1) {
        switch (o) {

          case ':':
            if (optopt == 's') {
                opt.sloglev=3;   // default syslog level 3
            } else {
                fprintf(stderr, "Option -%c requires an argument.\n", optopt);
            }
            break;

          case 'd':
            opt.daemon = 1;
            opt.log2file = 1;
            break;

          case 'V':
            fprintf(stdout,"\nGidget v%s Goddard & Brooks 2011\n\n",GVERSION);
            exit(0);
            break;

          case 'v':
            opt.verbose = 1;
            break;

          case 'c':
            if (strlen(optarg) > MAX_CONFIG_NAME_LEN) {
                fprintf (stderr, "configuration file name too long!\n");
                exit(1);
            }
            strcpy(opt.config,optarg);
            break;

          case 'l':
            if (strlen(optarg) > MAX_LOG_NAME_LEN) {
                fprintf (stderr, "log file name too long!\n");
                exit(1);
            }
            strcpy(opt.logfile,optarg);
            opt.log2file = 1;
            break;

          case 'p':
            if (strlen(optarg) > MAX_PID_NAME_LEN) {
                fprintf (stderr, "Pid file name too long!\n");
                exit(1);
            }
            strcpy(opt.pidfile,optarg);
            break;

          case 's':
            if ((strlen(optarg) == 1) && (isdigit(*optarg))) {
                opt.sloglev = atoi(optarg);
            } else {
                opt.sloglev = -1;
            }
            if ((opt.sloglev > 7) || (opt.sloglev < 0)) {
                usage(stderr);
            }
            opt.syslog = 1;
            break;

          case '?':
            usage(stdout);
            break;

          default:
            usage(stderr);
            break;
        }
    }

// undocumented feature - config file from command line without -c switch
    if (optind < argc) {
        if (strlen(argv[optind]) > MAX_CONFIG_NAME_LEN) {
            fprintf (stderr,"configuration file name too long!\n");
            exit(1);
         }
         strcpy(opt.config,argv[optind++]);
    }

    if (optind < argc) usage(stderr);

    return opt;
}

/*  "If you'd told me in 1989 that unix would be the hope for
     the future, I'd have cut my throat" --Jamie Zwarinski?    */


// Signal management allows read() to provide EINTR results and
// permits forked children to be auto-reaped for zombie control

static void signalTrap(int sig, siginfo_t * siginfo, void *context) {
    long sigpid = siginfo->si_pid;
    long siguid = siginfo->si_uid;
    if (sigpid != 0 && siguid != 0) {
        printf("Signal %d received from process: %ld, UID: %ld\n", sig,
               sigpid, siguid);
    }
    signalCaught = (sig_atomic_t) sig;
    return;
}

// We re-open our output channels on request so that log files
// can be managed reasonably intelligently

static void reopenLogs(opts_t opt) {

    char logtxt[MAX_ERR_TEXT_LEN];

    if (NULL == freopen(opt.logfile,"a",stdout)) {
        sprintf(logtxt,"Error (%u) opening %s for stdout: %s",
             errno, opt.logfile, strerror(errno));
        logx(1, opt, logtxt);
    }
    if (NULL == freopen(opt.logfile,"a",stderr)) {
        sprintf(logtxt,"Error (%u) opening %s for stderr: %s",
             errno, opt.logfile, strerror(errno));
        logx(1, opt, logtxt);
    }
}

/*
  "The act of focusing our mightiest intellectual resources on
  the elusive goal of go to-less programs has helped us get our 
  minds off all those really tough and possibly unresolvable
  problems and issues with which today's professional programmer 
  would otherwise have to grapple."  --John R. Brown, as quoted
  by D. E. Knuth in "Structured Programming with go to Statements",
  ACM Computing Surveys vol. 6 no. 4, 1974
*/


// logx writes a line to the log file and optionally to syslog()
// it will terminate the process if status is non-zero

void logx(int xstatus, opts_t opt, char logtxt[]) {

//  ISO standard date/time representations
//  are used by all right-thinking people everywhere

    time_t unixEpochTime;
    struct tm fancyTime, *bigTime;
    char tmbuf[20], *Now;
    Now = &tmbuf[0];

    unixEpochTime = time(NULL);
    bigTime = localtime_r(&unixEpochTime, &fancyTime);
    strftime(tmbuf, sizeof(tmbuf), "%F %T", bigTime);

//  remember, kids, anyone who uses any time format other
//  than the ISO standard is an infidel and/or terrorist

    pid_t pid;
    char pidString[64];

    if ((pid = getpid()) < 0) {
        strcpy(pidString,"[unknown]");
    } else {
        sprintf(pidString, "[%d]", pid);
    }

    FILE* logstream;

    if (0 == xstatus) {
        fflush(stderr);
        logstream = stdout;
        if (strlen(logtxt) <= 0) {
            strcpy(logtxt,"Missing log string. This should not happen.");
        }
    } else {
        fflush(stdout);
        logstream = stderr;
        if (strlen(logtxt) <= 0) {
            strcpy(logtxt,"The sky is falling!  The sky is falling!");
        }
    }

    fprintf(logstream, "gidget%s: %s %s\n", pidString, Now, logtxt);

    if (opt.syslog == 1) {
         syslog(opt.sloglev, "gidget%s: %s %s\n", pidString, Now, logtxt);
    }
    
    fflush(logstream);
    if (0 == xstatus) return;
    exit(xstatus);
}

/*
    Bits in inotify event masks are numbered 0-31 from the least
    significant to the most significant under the current endian
    architecture.  Bitmap mnemonics are assigned in inotify.h;

    With the exception of IN_CLOSE, IN_MOVE, and IN_ALL_EVENTS
    which are synthetic you can obtain the bit values this way:
      gawk '/fine IN_/ {
               Bitmap=0;
               rawBits=strtonum($3);
               for (i=0; i<=32; i++) {
                 if (and(2^i,rawBits)!=0) {
                   Bitmap+=i
                 }
               }
               printf("  ezName[%d] = \"%s\";\n",Bitmap,$2)
            }'    /usr/include/linux/inotify.h
*/

static void stringifyEventBits(uint32_t bitMap) {
  uint32_t i,j,k=0;
  int hits=0;
  typedef char * string;  // never never never do this
  string ezName[32];  // subject to change without notice, see inotify.h

  ezName[0] = "IN_ACCESS";
  ezName[1] = "IN_MODIFY";
  ezName[2] = "IN_ATTRIB";
  ezName[3] = "IN_CLOSE_WRITE";
  ezName[4] = "IN_CLOSE_NOWRITE";
  ezName[5] = "IN_OPEN";
  ezName[6] = "IN_MOVED_FROM";
  ezName[7] = "IN_MOVED_TO";
  ezName[8] = "IN_CREATE";
  ezName[9] = "IN_DELETE";
  ezName[10] = "IN_DELETE_SELF";
  ezName[11] = "IN_MOVE_SELF";
  ezName[13] = "IN_UNMOUNT";
  ezName[14] = "IN_Q_OVERFLOW";
  ezName[15] = "IN_IGNORED";
  ezName[24] = "IN_ONLYDIR";
  ezName[25] = "IN_DONT_FOLLOW";
  ezName[29] = "IN_MASK_ADD";
  ezName[30] = "IN_ISDIR";
  ezName[31] = "IN_ONESHOT";

// C has no exponentiation operator, go figure 
  for (i=0; i<32; i++) {
    j=1<<i; // wheeeee!!! hardcore
    if ((j & bitMap) != 0) {
      if (hits++!=0) printf(" ");
      printf("%s(%#.8x)",ezName[i],j);
    }
  }

  if ((IN_CLOSE & bitMap) != 0) {
    if (hits++!=0) printf(" ");
    printf("IN_CLOSE(%#.8x)",IN_CLOSE);
  }

  if ((IN_MOVE & bitMap) != 0) {
    if (hits++!=0) printf(" ");
    printf("IN_MOVE(%#.8x)",IN_MOVE);
  }

// this should never ever happen - if it does, blame it on Robert Love
  k = bitMap & (~(IN_ALL_EVENTS|IN_ISDIR|IN_UNMOUNT|IN_Q_OVERFLOW|IN_IGNORED));
  if (k != 0) {
    if (hits++!=0) printf("\n");
    printf("WARNING! Unrecognized event flag %#.8x not mapped by IN_ALL_EVENTS!",k);
  }

// this will almost certainly happen one of these days
  if (hits==0) {
    printf("WARNING! No string representation of event mask %#.8x is available!\n",bitMap);
    printf("Most likely, this means that inotify.h has been enhanced to signal more events\n");
    printf("and stringifyEventBits() needs updating and recompilation with the new bitmaps\n");
    printf("Please consult the comments in the gidget source code for more information");
  }

  printf("\n");  // we always print something, so always dump the buffer
  return;
}

/* 
   MAX_Q_EVENTS=16384  from /proc/sys/fs/inotify/max_queued_events
   MAX_IN_PROC_PER_USER=128 from /proc/sys/fs/inotify/max_user_instances
   MAX_IN_WATCHES=8192 from /proc/sys/fs/inotify/max_user_watches

   These system-wide inotify limitations are not currently used by
   the gidget code.  They were taken from a Dell PowderedEgg M600
   blade running Red Hat Enterprise Linux 5 on 2011-01-24.

   I stuck them down here out of the way for hysterical porpoises
*/
