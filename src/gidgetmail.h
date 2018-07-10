/*

    The gidget filesystem event monitor executes
    user-defined scripts when it recieves notification
    of (also user-defined) activity on a file system.

    If a script generates output of any sort that
    output will be e-mailed to a specified user.

    This file defines how gidget needs to call the
    local email utility in order to send that mail.

    The program was originally written for Red Hat
    Enterprise Linux and so the mailer definition
    pretty much conforms to "the Red Hat way" which
    will probably be compatible with nearly all
    systems based on sendmail or postfix.

    Using a sendmail-compatible transport provides
    transparent access to aliases and LDAP etc.

    --Charlie Brooks 2011-03-10

*/

// simple inclusion guard
#ifndef _GIG_MAIL

# define _GIG_MAIL

# ifndef _PATH_SENDMAIL
#  define _PATH_SENDMAIL "/usr/lib/sendmail"
# endif

# define MAIL_TRANSPORT _PATH_SENDMAIL

# define MAIL_OPTIONS " -Fgidget -odi -oem -oi -t "
          /* -Fx   = Set full-name of sender
           * -odi  = Option Deliverymode Interactive
           * -oem  = Option Errors Mailedtosender
           * -oi   = Ignore "." alone on a line
           * -t    = Get recipient from headers
           */

# define MAILCOMMAND MAIL_TRANSPORT MAIL_OPTIONS

#endif
