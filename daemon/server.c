/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * MDM - The MDM Display Manager
 * Copyright (C) 1999, 2000 Martin K. Petersen <mkp@mkp.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/* This file contains functions for controlling local X servers */

#include "config.h"

#include <glib/gi18n.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <strings.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <X11/Xlib.h>

#include "mdm.h"
#include "server.h"
#include "misc.h"
#include "xdmcp.h"
#include "display.h"
#include "auth.h"
#include "slave.h"
#include "getvt.h"

#include "mdm-common.h"
#include "mdm-log.h"
#include "mdm-daemon-config.h"

#include "mdm-socket-protocol.h"

#if __sun
#define MDM_PRIO_DEFAULT NZERO
#else
#define MDM_PRIO_DEFAULT 0
#endif

/* Local prototypes */
static void mdm_server_spawn (MdmDisplay *d, const char *vtarg);
static void mdm_server_usr1_handler (gint);
static void mdm_server_child_handler (gint);
static char * get_font_path (const char *display);

/* Global vars */
static int server_signal_pipe[2];
static MdmDisplay *d                   = NULL;
static gboolean server_signal_notified = FALSE;
static int mdm_in_signal               = 0;

static void do_server_wait (MdmDisplay *d);
static gboolean setup_server_wait (MdmDisplay *d);

void
mdm_server_whack_lockfile (MdmDisplay *disp)
{
	    char buf[256];

	    /* X seems to be sometimes broken with its lock files and
	       doesn't seem to remove them always, and if you manage
	       to get into the weird state where the old pid now
	       corresponds to some new pid, X will just die with
	       a stupid error. */

	    /* Yes there could be a race here if another X server starts
	       at this exact instant.  Oh well such is life.  Very unlikely
	       to happen though as we should really be the only ones
	       trying to start X servers, and we aren't starting an
	       X server on this display yet. */

	    /* if lock file exists and it is our process, whack it! */
	    g_snprintf (buf, sizeof (buf), "/tmp/.X%d-lock", disp->dispnum);
	    VE_IGNORE_EINTR (g_unlink (buf));

	    /* whack the unix socket as well */
	    g_snprintf (buf, sizeof (buf),
			"/tmp/.X11-unix/X%d", disp->dispnum);
	    VE_IGNORE_EINTR (g_unlink (buf));
}


/* Wipe cookie files */
void
mdm_server_wipe_cookies (MdmDisplay *disp)
{
	if ( ! ve_string_empty (disp->authfile)) {
		VE_IGNORE_EINTR (g_unlink (disp->authfile));
	}
	g_free (disp->authfile);
	disp->authfile = NULL;
	if ( ! ve_string_empty (disp->authfile_mdm)) {
		VE_IGNORE_EINTR (g_unlink (disp->authfile_mdm));
	}
	g_free (disp->authfile_mdm);
	disp->authfile_mdm = NULL;
}

static Jmp_buf reinitjmp;

/* ignore handlers */
static int
ignore_xerror_handler (Display *disp, XErrorEvent *evt)
{
	return 0;
}

static int
jumpback_xioerror_handler (Display *disp)
{
	Longjmp (reinitjmp, 1);
}

#ifdef HAVE_FBCONSOLE
#define FBCONSOLE "/usr/openwin/bin/fbconsole"

static void
mdm_exec_fbconsole (MdmDisplay *disp)
{
        char *argv[6];

        argv[0] = FBCONSOLE;
        argv[1] = "-n";
        argv[2] = "-d";
        argv[3] = disp->name;
        argv[4] = NULL;

	mdm_debug ("Forking fbconsole");

        d->fbconsolepid = fork ();
        if (d->fbconsolepid == 0) {
                mdm_close_all_descriptors (0 /* from */, -1 /* except */, -1 /* except2 */)
;
                VE_IGNORE_EINTR (execv (argv[0], argv));

		mdm_error ("Can not start fallback console: %s",
			   strerror (errno));
		_exit (0);
        }
        if (d->fbconsolepid == -1) {
                mdm_error (_("Can not start fallback console"));
        }
}
#endif

/**
 * mdm_server_stop:
 * @disp: Pointer to a MdmDisplay structure
 *
 * Stops a local X server, but only if it exists
 */

void
mdm_server_stop (MdmDisplay *disp)
{
    static gboolean waiting_for_server = FALSE;
    int old_servstat;

    if (disp == NULL)
	return;

    /* Kill our connection if one existed */
    if (disp->dsp != NULL) {
	    /* on XDMCP servers first kill everything in sight */
	    if (disp->type == TYPE_XDMCP)
		    mdm_server_whack_clients (disp->dsp);
	    XCloseDisplay (disp->dsp);
	    disp->dsp = NULL;
    }

    /* Kill our parent connection if one existed */
    if (disp->parent_dsp != NULL) {
	    /* on XDMCP servers first kill everything in sight */
	    if (disp->type == TYPE_XDMCP_PROXY)
		    mdm_server_whack_clients (disp->parent_dsp);
	    XCloseDisplay (disp->parent_dsp);
	    disp->parent_dsp = NULL;
    }

    if (disp->servpid <= 0)
	    return;

    mdm_debug ("mdm_server_stop: Server for %s going down!", disp->name);

    old_servstat = disp->servstat;
    disp->servstat = SERVER_DEAD;

    if (disp->servpid > 0) {
	    pid_t servpid;

	    mdm_debug ("mdm_server_stop: Killing server pid %d",
		       (int)disp->servpid);

	    /* avoid SIGCHLD race */
	    mdm_sigchld_block_push ();
	    servpid = disp->servpid;

	    if (waiting_for_server) {
		    mdm_error ("mdm_server_stop: Some problem killing server, whacking with SIGKILL");
		    if (disp->servpid > 1)
			    kill (disp->servpid, SIGKILL);

	    } else {
		    if (disp->servpid > 1 &&
			kill (disp->servpid, SIGTERM) == 0) {
			    waiting_for_server = TRUE;
			    ve_waitpid_no_signal (disp->servpid, NULL, 0);
			    waiting_for_server = FALSE;
		    }
	    }
	    disp->servpid = 0;

	    mdm_sigchld_block_pop ();

	    if (old_servstat == SERVER_RUNNING)
		    mdm_server_whack_lockfile (disp);

	    mdm_debug ("mdm_server_stop: Server pid %d dead", (int)servpid);

	    /* just in case we restart again wait at least
	       one sec to avoid races */
	    if (d->sleep_before_run < 1)
		    d->sleep_before_run = 1;
    }

    mdm_server_wipe_cookies (disp);

#ifdef HAVE_FBCONSOLE
    /* Kill fbconsole if it is running */
    if (d->fbconsolepid > 0)
        kill (d->fbconsolepid, SIGTERM);
    d->fbconsolepid = 0;
#endif

    mdm_slave_whack_temp_auth_file ();
}

static gboolean
busy_ask_user (MdmDisplay *disp)
{
    /* if we have "open" we can talk to the user */
    if (g_access (LIBEXECDIR "/mdmopen", X_OK) == 0) {
	    char *error = g_strdup_printf
		    (C_(N_("There already appears to be an X server "
			   "running on display %s.  Should another "
			   "display number by tried?  Answering no will "
			   "cause MDM to attempt starting the server "
			   "on %s again.%s")),
		     disp->name,
		     disp->name,
#ifdef __linux__
		     C_(N_("  (You can change consoles by pressing Ctrl-Alt "
			   "plus a function key, such as Ctrl-Alt-F7 to go "
			   "to console 7.  X servers usually run on consoles "
			   "7 and higher.)"))
#else /* ! __linux__ */
		     /* no info for non linux users */
		     ""
#endif /* __linux__ */
		     );
	    gboolean ret = TRUE;
	    /* default ret to TRUE */
	    if ( ! mdm_text_yesno_dialog (error, &ret))
		    ret = TRUE;
	    g_free (error);
	    return ret;
    } else {
	    /* Well we'll just try another display number */
	    return TRUE;
    }
}

/* Checks only output, no XFree86 v4 logfile */
static gboolean
display_parent_no_connect (MdmDisplay *disp)
{
	char *logname = mdm_make_filename (mdm_daemon_config_get_value_string (MDM_KEY_LOG_DIR), d->name, ".log");
	FILE *fp;
	char buf[256];
	char *getsret;

	VE_IGNORE_EINTR (fp = fopen (logname, "r"));
	g_free (logname);

	if (fp == NULL)
		return FALSE;

	for (;;) {
		VE_IGNORE_EINTR (getsret = fgets (buf, sizeof (buf), fp));
		if (getsret == NULL) {
			VE_IGNORE_EINTR (fclose (fp));
			return FALSE;
		}
		/* Note: this is probably XFree86 specific, and perhaps even
		 * version 3 specific (I don't have xfree v4 to test this),
		 * of course additions are welcome to make this more robust */
		if (strstr (buf, "Unable to open display \"") == buf) {
			mdm_error (_("Display '%s' cannot be opened by nested display"),
				   ve_sure_string (disp->parent_disp));
			VE_IGNORE_EINTR (fclose (fp));
			return TRUE;
		}
	}
}

static gboolean
display_busy (MdmDisplay *disp)
{
	char *logname = mdm_make_filename (mdm_daemon_config_get_value_string (MDM_KEY_LOG_DIR), d->name, ".log");
	FILE *fp;
	char buf[256];
	char *getsret;

	VE_IGNORE_EINTR (fp = fopen (logname, "r"));
	g_free (logname);

	if (fp == NULL)
		return FALSE;

	for (;;) {
		VE_IGNORE_EINTR (getsret = fgets (buf, sizeof (buf), fp));
		if (getsret == NULL) {
			VE_IGNORE_EINTR (fclose (fp));
			return FALSE;
		}
		/* Note: this is probably XFree86 specific */
		if (strstr (buf, "Server is already active for display")
		    == buf) {
			mdm_error (_("Display %s is busy. There is another "
				     "X server running already."),
				   disp->name);
			VE_IGNORE_EINTR (fclose (fp));
			return TRUE;
		}
	}
}

/* if we find 'Log file: "foo"' switch fp to foo and
   return TRUE */
/* Note: assumes buf is of size 256 and is writable */ 
static gboolean
open_another_logfile (char buf[256], FILE **fp)
{
	if (strncmp (&buf[5], "Log file: \"", strlen ("Log file: \"")) == 0) {
		FILE *ffp;
		char *fname = &buf[5+strlen ("Log file: \"")];
		char *p = strchr (fname, '"');
		if (p == NULL)
			return FALSE;
		*p = '\0';
		VE_IGNORE_EINTR (ffp = fopen (fname, "r"));
		if (ffp == NULL)
			return FALSE;
		VE_IGNORE_EINTR (fclose (*fp));
		*fp = ffp;
		return TRUE;
	}
	return FALSE;
}

static int
display_vt (MdmDisplay *disp)
{
	char *logname = mdm_make_filename (mdm_daemon_config_get_value_string (MDM_KEY_LOG_DIR), d->name, ".log");
	FILE *fp;
	char buf[256];
	gboolean switched = FALSE;
	char *getsret;

	VE_IGNORE_EINTR (fp = fopen (logname, "r"));
	g_free (logname);

	if (fp == NULL)
		return FALSE;

	for (;;) {
		int vt;
		char *p;

		VE_IGNORE_EINTR (getsret = fgets (buf, sizeof (buf), fp));
		if (getsret == NULL) {
			VE_IGNORE_EINTR (fclose (fp));
			return -1;
		}

		if ( ! switched &&
		     /* this is XFree v4 specific */
		    open_another_logfile (buf, &fp)) {
			switched = TRUE;
			continue;
		} 
		/* Note: this is probably XFree86 specific (works with
		 * both v3 and v4 though */
		p = strstr (buf, "using VT number ");
		if (p != NULL &&
		    sscanf (p, "using VT number %d", &vt) == 1) {
			VE_IGNORE_EINTR (fclose (fp));
			return vt;
		}
	}
}

static struct sigaction old_svr_wait_chld;
static sigset_t old_svr_wait_mask;

static gboolean
setup_server_wait (MdmDisplay *d)
{
    struct sigaction usr1, chld;
    sigset_t mask;

    if (pipe (server_signal_pipe) != 0) {
	    mdm_error (_("%s: Error opening a pipe: %s"),
		       "setup_server_wait", strerror (errno));
	    return FALSE; 
    }
    server_signal_notified = FALSE;

    /* Catch USR1 from X server */
    usr1.sa_handler = mdm_server_usr1_handler;
    usr1.sa_flags = SA_RESTART;
    sigemptyset (&usr1.sa_mask);

    if (sigaction (SIGUSR1, &usr1, NULL) < 0) {
	    mdm_error (_("%s: Error setting up %s signal handler: %s"),
		       "mdm_server_start", "USR1", strerror (errno));
	    VE_IGNORE_EINTR (close (server_signal_pipe[0]));
	    VE_IGNORE_EINTR (close (server_signal_pipe[1]));
	    return FALSE;
    }

    /* Catch CHLD from X server */
    chld.sa_handler = mdm_server_child_handler;
    chld.sa_flags = SA_RESTART|SA_NOCLDSTOP;
    sigemptyset (&chld.sa_mask);

    if (sigaction (SIGCHLD, &chld, &old_svr_wait_chld) < 0) {
	    mdm_error (_("%s: Error setting up %s signal handler: %s"),
		       "mdm_server_start", "CHLD", strerror (errno));
	    mdm_signal_ignore (SIGUSR1);
	    VE_IGNORE_EINTR (close (server_signal_pipe[0]));
	    VE_IGNORE_EINTR (close (server_signal_pipe[1]));
	    return FALSE;
    }

    /* Set signal mask */
    sigemptyset (&mask);
    sigaddset (&mask, SIGUSR1);
    sigaddset (&mask, SIGCHLD);
    sigprocmask (SIG_UNBLOCK, &mask, &old_svr_wait_mask);

    return TRUE;
}

static void
do_server_wait (MdmDisplay *d)
{
    /* Wait for X server to send ready signal */
    if (d->servstat == SERVER_PENDING) {
	    if (d->server_uid != 0 && ! d->handled && ! d->chosen_hostname) {
		    /* FIXME: If not handled, we just don't know, so
		     * just wait a few seconds and hope things just work,
		     * fortunately there is no such case yet and probably
		     * never will, but just for code anality's sake */
		    mdm_sleep_no_signal (mdm_daemon_config_get_value_int(MDM_KEY_XSERVER_TIMEOUT));
	    } else if (d->server_uid != 0) {
		    int i;

		    /* FIXME: This is not likely to work in reinit,
		       but we never reinit Nested servers nowdays,
		       so that's fine */

		    /* if we're running the server as a non-root, we can't
		     * use USR1 of course, so try openning the display 
		     * as a test, but the */

		    /* just in case it's set */
		    g_unsetenv ("XAUTHORITY");

		    mdm_auth_set_local_auth (d);

		    for (i = 0;
			 d->dsp == NULL &&
			 d->servstat == SERVER_PENDING &&
			 i < mdm_daemon_config_get_value_int(MDM_KEY_XSERVER_TIMEOUT);
			 i++) {
			    d->dsp = XOpenDisplay (d->name);
			    if (d->dsp == NULL)
				    mdm_sleep_no_signal (1);
			    else
				    d->servstat = SERVER_RUNNING;
		    }
		    if (d->dsp == NULL &&
			/* Note: we could have still gotten a SIGCHLD */
			d->servstat == SERVER_PENDING) {
			    d->servstat = SERVER_TIMEOUT;
		    }
	    } else {
		    time_t t = time (NULL);

		    mdm_debug ("do_server_wait: Before mainloop waiting for server");

		    do {
			    fd_set rfds;
			    struct timeval tv;

			    /* Wait up to MDM_KEY_XSERVER_TIMEOUT seconds. */
			    tv.tv_sec = MAX (1, mdm_daemon_config_get_value_int(MDM_KEY_XSERVER_TIMEOUT) 
			    	- (time (NULL) - t));
			    tv.tv_usec = 0;

			    FD_ZERO (&rfds);
			    FD_SET (server_signal_pipe[0], &rfds);

			    if (select (server_signal_pipe[0]+1, &rfds, NULL, NULL, &tv) > 0) {
				    char buf[4];
				    /* read the Yay! */
				    VE_IGNORE_EINTR (read (server_signal_pipe[0], buf, 4));
			    }
			    if ( ! server_signal_notified &&
				t + mdm_daemon_config_get_value_int(MDM_KEY_XSERVER_TIMEOUT) < time (NULL)) {
				    mdm_debug ("do_server_wait: Server timeout");
				    d->servstat = SERVER_TIMEOUT;
				    server_signal_notified = TRUE;
			    }
			    if (d->servpid <= 1) {
				    d->servstat = SERVER_ABORT;
				    server_signal_notified = TRUE;
			    }
		    } while ( ! server_signal_notified);

		    mdm_debug ("mdm_server_start: After mainloop waiting for server");
	    }
    }

    /* restore default handlers */
    mdm_signal_ignore (SIGUSR1);
    sigaction (SIGCHLD, &old_svr_wait_chld, NULL);
    sigprocmask (SIG_SETMASK, &old_svr_wait_mask, NULL);

    VE_IGNORE_EINTR (close (server_signal_pipe[0]));
    VE_IGNORE_EINTR (close (server_signal_pipe[1]));

    if (d->servpid <= 1) {
	    d->servstat = SERVER_ABORT;
    }

    if (d->servstat != SERVER_RUNNING) {
	    /* bad things are happening */
	    if (d->servpid > 0) {
		    pid_t pid;

		    d->dsp = NULL;

		    mdm_sigchld_block_push ();
		    pid = d->servpid;
		    d->servpid = 0;
		    if (pid > 1 &&
			kill (pid, SIGTERM) == 0)
			    ve_waitpid_no_signal (pid, NULL, 0);
		    mdm_sigchld_block_pop ();
	    }

	    /* We will rebake cookies anyway, so wipe these */
	    mdm_server_wipe_cookies (d);
    }
}

/* We keep a connection (parent_dsp) open with the parent X server
 * before running a proxy on it to prevent the X server resetting
 * as we open and close other connections.
 * Note that XDMCP servers, by default, reset when the seed X
 * connection closes whereas usually the X server only quits when
 * all X connections have closed.
 */
static gboolean
connect_to_parent (MdmDisplay *d)
{
	int maxtries;
	int openretries;

	mdm_debug ("mdm_server_start: Connecting to parent display \'%s\'",
		   d->parent_disp);

	d->parent_dsp = NULL;

	maxtries = SERVER_IS_XDMCP (d) ? 10 : 2;

	openretries = 0;
	while (openretries < maxtries &&
	       d->parent_dsp == NULL) {
		d->parent_dsp = XOpenDisplay (d->parent_disp);

		if G_UNLIKELY (d->parent_dsp == NULL) {
			mdm_debug ("mdm_server_start: Sleeping %d on a retry", 1+openretries*2);
			mdm_sleep_no_signal (1+openretries*2);
			openretries++;
		}
	}

	if (d->parent_dsp == NULL)
		mdm_error (_("%s: failed to connect to parent display \'%s\'"),
			   "mdm_server_start", d->parent_disp);

	return d->parent_dsp != NULL;
}

/**
 * mdm_server_start:
 * @disp: Pointer to a MdmDisplay structure
 *
 * Starts a local X server. Handles retries and fatal errors properly.
 */

gboolean
mdm_server_start (MdmDisplay *disp,
		  gboolean try_again_if_busy /* only affects non-flexi servers */,
		  gboolean treat_as_flexi,
		  int min_flexi_disp,
		  int flexi_retries)
{
    int flexi_disp = 20;
    char *vtarg = NULL;
    int vtfd = -1, vt = -1;
    
    if (disp == NULL)
	    return FALSE;

    d = disp;

#ifdef HAVE_FBCONSOLE
    d->fbconsolepid = 0;
#endif

    /* if an X server exists, wipe it */
    mdm_server_stop (d);

    /* First clear the VT number */
    if (d->type == TYPE_STATIC ||
	d->type == TYPE_FLEXI) {
	    d->vt = -1;
	    mdm_slave_send_num (MDM_SOP_VT_NUM, -1);
    }

    if (SERVER_IS_FLEXI (d) ||
	treat_as_flexi) {
	    flexi_disp = mdm_get_free_display
		    (MAX (mdm_daemon_config_get_high_display_num () + 1, min_flexi_disp) /* start */,
		     d->server_uid /* server uid */);

	    g_free (d->name);
	    d->name = g_strdup_printf (":%d", flexi_disp);
	    d->dispnum = flexi_disp;

	    mdm_slave_send_num (MDM_SOP_DISP_NUM, flexi_disp);
    }

    if (d->type == TYPE_XDMCP_PROXY &&
	! connect_to_parent (d))
	    return FALSE;

    mdm_debug ("mdm_server_start: %s", d->name);

    /* Create new cookie */
    if ( ! mdm_auth_secure_display (d)) 
	    return FALSE;
    mdm_slave_send_string (MDM_SOP_COOKIE, d->cookie);
    mdm_slave_send_string (MDM_SOP_AUTHFILE, d->authfile);
    g_setenv ("DISPLAY", d->name, TRUE);

    if ( ! setup_server_wait (d))
	    return FALSE;

    d->servstat = SERVER_DEAD;

    if (d->type == TYPE_STATIC ||
	d->type == TYPE_FLEXI) {
	    vtarg = mdm_get_empty_vt_argument (&vtfd, &vt);
    }

    /* fork X server process */
    mdm_server_spawn (d, vtarg);

    g_free (vtarg);

    /* we can now use d->handled since that's set up above */
    do_server_wait (d);

    /* If we were holding a vt open for the server, close it now as it has
     * already taken the bait. */
    if (vtfd > 0) {
	    VE_IGNORE_EINTR (close (vtfd));
    }

    switch (d->servstat) {

    case SERVER_TIMEOUT:
	    mdm_debug ("mdm_server_start: Temporary server failure (%s)", d->name);
	    break;

    case SERVER_ABORT:
	    mdm_debug ("mdm_server_start: Server %s died during startup!", d->name);
	    break;

    case SERVER_RUNNING:
	    mdm_debug ("mdm_server_start: Completed %s!", d->name);

	    if (SERVER_IS_FLEXI (d))
		    mdm_slave_send_num (MDM_SOP_FLEXI_OK, 0 /* bogus */);
	    if (d->type == TYPE_STATIC ||
		d->type == TYPE_FLEXI) {
		    if (vt >= 0)
			    d->vt = vt;

		    if (d->vt < 0)
			    d->vt = display_vt (d);
		    if (d->vt >= 0)
			    mdm_slave_send_num (MDM_SOP_VT_NUM, d->vt);
	    }

#ifdef HAVE_FBCONSOLE
            mdm_exec_fbconsole (d);
#endif

	    return TRUE;
    default:
	    break;
    }

    if (SERVER_IS_PROXY (disp) &&
	display_parent_no_connect (disp)) {
	    mdm_slave_send_num (MDM_SOP_FLEXI_ERR,
				5 /* proxy can't connect */);
	    _exit (DISPLAY_REMANAGE);
    }

    /* if this was a busy fail, that is, there is already
     * a server on that display, we'll display an error and after
     * this we'll exit with DISPLAY_REMANAGE to try again if the
     * user wants to, or abort this display */
    if (display_busy (disp)) {
	    if (SERVER_IS_FLEXI (disp) ||
		treat_as_flexi) {
		    /* for flexi displays, try again a few times with different
		     * display numbers */
		    if (flexi_retries <= 0) {
			    /* Send X too busy */
			    mdm_error (_("%s: Cannot find a free "
					 "display number"),
				       "mdm_server_start");
			    if (SERVER_IS_FLEXI (disp)) {
				    mdm_slave_send_num (MDM_SOP_FLEXI_ERR,
							4 /* X too busy */);
			    }
			    /* eki eki */
			    _exit (DISPLAY_REMANAGE);
		    }
		    return mdm_server_start (d, FALSE /*try_again_if_busy */,
					     treat_as_flexi,
					     flexi_disp + 1,
					     flexi_retries - 1);
	    } else {
		    if (try_again_if_busy) {
			    mdm_debug ("%s: Display %s busy.  Trying once again "
				       "(after 2 sec delay)",
				       "mdm_server_start", d->name);
			    mdm_sleep_no_signal (2);
			    return mdm_server_start (d,
						     FALSE /* try_again_if_busy */,
						     treat_as_flexi,
						     flexi_disp,
						     flexi_retries);
		    }
		    if (busy_ask_user (disp)) {
			    mdm_error (_("%s: Display %s busy.  Trying "
					 "another display number."),
				       "mdm_server_start",
				       d->name);
			    d->busy_display = TRUE;
			    return mdm_server_start (d,
						     FALSE /*try_again_if_busy */,
						     TRUE /* treat as flexi */,
						     mdm_daemon_config_get_high_display_num () + 1,
						     flexi_retries - 1);
		    }
		    _exit (DISPLAY_REMANAGE);
	    }
    }

    _exit (DISPLAY_XFAILED);

    return FALSE;
}

/* Do things that require checking the log,
 * we really do need to get called a bit later, after all init is done
 * as things aren't written to disk before that */
void
mdm_server_checklog (MdmDisplay *disp)
{
	if (d->vt < 0 &&
	    (d->type == TYPE_STATIC ||
	     d->type == TYPE_FLEXI)) {
		d->vt = display_vt (d);
		if (d->vt >= 0)
			mdm_slave_send_num (MDM_SOP_VT_NUM, d->vt);
	}
}

/* somewhat safer rename (safer if the log dir is unsafe), may in fact
   lose the file though, it guarantees that a is gone, but not that
   b exists */
static void
safer_rename (const char *a, const char *b)
{
	errno = 0;
	if (link (a, b) < 0) {
		if (errno == EEXIST) {
			VE_IGNORE_EINTR (g_unlink (a));
			return;
		} 
		VE_IGNORE_EINTR (g_unlink (b));
		/* likely this system doesn't support hard links */
		g_rename (a, b);
		VE_IGNORE_EINTR (g_unlink (a));
		return;
	}
	VE_IGNORE_EINTR (g_unlink (a));
}

static void
rotate_logs (const char *dname)
{
	const gchar *logdir = mdm_daemon_config_get_value_string (MDM_KEY_LOG_DIR);

	/* I'm too lazy to write a loop */
	char *fname4 = mdm_make_filename (logdir, dname, ".log.4");
	char *fname3 = mdm_make_filename (logdir, dname, ".log.3");
	char *fname2 = mdm_make_filename (logdir, dname, ".log.2");
	char *fname1 = mdm_make_filename (logdir, dname, ".log.1");
	char *fname = mdm_make_filename (logdir, dname, ".log");

	/* Rotate the logs (keep 4 last) */
	VE_IGNORE_EINTR (g_unlink (fname4));
	safer_rename (fname3, fname4);
	safer_rename (fname2, fname3);
	safer_rename (fname1, fname2);
	safer_rename (fname, fname1);

	g_free (fname4);
	g_free (fname3);
	g_free (fname2);
	g_free (fname1);
	g_free (fname);
}

static void
mdm_server_add_xserver_args (MdmDisplay *d, char **argv)
{
	int count;
	char **args;
	int len;
	int i;

	len = mdm_vector_len (argv);
	g_shell_parse_argv (d->xserver_session_args, &count, &args, NULL);
	argv = g_renew (char *, argv, len + count + 1);

	for (i=0; i < count;i++) {
		argv[len++] = g_strdup(args[i]);
	}

	argv[len] = NULL;
	g_strfreev (args);
}

MdmXserver *
mdm_server_resolve (MdmDisplay *disp)
{
	char *bin;
	MdmXserver *svr = NULL;

	bin = ve_first_word (disp->command);
	if (bin != NULL && bin[0] != '/') {
		svr = mdm_daemon_config_find_xserver (bin);
	}
	g_free (bin);
	return svr;
}


static char **
vector_merge (char * const *v1,
	      int           len1,
	      char * const *v2,
	      int           len2)
{
	int argc, i;
	char **argv;

	if (v1 == NULL && v2 == NULL)
		return NULL;

	argc = len1 + len2;

	argv = g_new (char *, argc + 1);
	for (i = 0; i < len1; i++)
		argv[i] = g_strdup (v1[i]);
	for (; i < argc; i++)
		argv[i] = g_strdup (v2[i - len1]);
	argv[i] = NULL;

	return argv;
}

gboolean
mdm_server_resolve_command_line (MdmDisplay *disp,
				 gboolean    resolve_flags,
				 const char *vtarg,
				 int        *argcp,
				 char     ***argvp)
{
	char *bin;
	int    argc;
	char **argv;
	int len;
	int i;
	gboolean gotvtarg = FALSE;
	gboolean query_in_arglist = FALSE;

        argv = NULL;

	bin = ve_first_word (disp->command);
	if (bin == NULL) {
		const char *str;

		mdm_error (_("Invalid server command '%s'"), disp->command);
		str = mdm_daemon_config_get_value_string (MDM_KEY_STANDARD_XSERVER);
       		g_shell_parse_argv (str, &argc, &argv, NULL);
	} else if (bin[0] != '/') {
		MdmXserver *svr = mdm_daemon_config_find_xserver (bin);
		if (svr == NULL) {
			const char *str;

			mdm_error (_("Server name '%s' not found; "
				     "using standard server"), bin);
			str = mdm_daemon_config_get_value_string (MDM_KEY_STANDARD_XSERVER);
			g_shell_parse_argv (str, &argc, &argv, NULL);

		} else {
			GError* error_p;
			char **svr_command;
			const char *str;
			int svr_argc;

			str = ve_sure_string (svr->command);
			svr_command = NULL;

			g_shell_parse_argv (str, &svr_argc,
				&svr_command, &error_p);

			g_shell_parse_argv (disp->command, &argc,
				&argv, &error_p);

			if (argv == NULL) {
				mdm_debug ("Problem parsing server command <%s>",
					disp->command ? disp->command : "(null)");
				return FALSE;
			}

			if (argv[0] == NULL || argv[1] == NULL) {
				g_strfreev (argv);
				argv = svr_command;
				argc = svr_argc;
			} else {
				char **old_argv = argv;
				argv = vector_merge (svr_command,
						     svr_argc,
						     &old_argv[1],
						     argc);
				g_strfreev (svr_command);
				g_strfreev (old_argv);

				argc += svr_argc;
			}

			if (resolve_flags) {
				/* Setup the handled function */
				disp->handled = svr->handled;
				/* never make use_chooser FALSE,
				   it may have been set temporarily for
				   us by the master */
				if (svr->chooser)
					disp->use_chooser = TRUE;
				disp->priority = svr->priority;
			}
		}
	} else {
		g_shell_parse_argv (disp->command, &argc, &argv, NULL);
	}

	for (len = 0; argv != NULL && argv[len] != NULL; len++) {
		char *arg = argv[len];
		/* HACK! Not to add vt argument to servers that already force
		 * allocation.  Mostly for backwards compat only */
		if (strncmp (arg, "vt", 2) == 0 &&
		    isdigit (arg[2]) &&
		    (arg[3] == '\0' ||
		     (isdigit (arg[3]) && arg[4] == '\0')))
			gotvtarg = TRUE;
		if (strcmp (arg, "-query") == 0 ||
		    strcmp (arg, "-indirect") == 0)
			query_in_arglist = TRUE;
	}

	argv = g_renew (char *, argv, len + 10);
	/* shift args down one */
	for (i = len - 1; i >= 1; i--) {
		argv[i+1] = argv[i];
	}
	/* server number is the FIRST argument, before any others */
	argv[1] = g_strdup (disp->name);
	len++;

	if (disp->authfile != NULL) {
		argv[len++] = g_strdup ("-auth");
		argv[len++] = g_strdup (disp->authfile);
	}

	if (resolve_flags && disp->chosen_hostname) {
		/* this display is NOT handled */
		disp->handled = FALSE;
		/* never ever ever use chooser here */
		disp->use_chooser = FALSE;
		disp->priority = MDM_PRIO_DEFAULT;
		/* run just one session */
		argv[len++] = g_strdup ("-terminate");
		argv[len++] = g_strdup ("-query");
		argv[len++] = g_strdup (disp->chosen_hostname);
		query_in_arglist = TRUE;
	}

	if (resolve_flags && mdm_daemon_config_get_value_bool (MDM_KEY_DISALLOW_TCP) && ! query_in_arglist) {
		argv[len++] = g_strdup ("-nolisten");
		argv[len++] = g_strdup ("tcp");
		d->tcp_disallowed = TRUE;
	}

	if (vtarg != NULL &&
	    ! gotvtarg) {
		argv[len++] = g_strdup (vtarg);
	}

	argv[len++] = NULL;

	*argvp = argv;
	*argcp = len;

	g_free (bin);

	return TRUE;
}

/**
 * mdm_server_spawn:
 * @disp: Pointer to a MdmDisplay structure
 *
 * forks an actual X server process
 *
 * Note that we can only use d->handled once we call this function
 * since otherwise the server might not yet be looked up yet.
 */

static void
mdm_server_spawn (MdmDisplay *d, const char *vtarg)
{
    struct sigaction ign_signal;
    sigset_t mask;
    int argc;
    gchar **argv = NULL;
    char *logfile;
    int logfd;
    char *command;
    pid_t pid;
    gboolean rc;

    if (d == NULL ||
	ve_string_empty (d->command)) {
	    return;
    }

    d->servstat = SERVER_PENDING;

    mdm_sigchld_block_push ();

    /* eek, some previous copy, just wipe it */
    if (d->servpid > 0) {
	    pid_t pid = d->servpid;
	    d->servpid = 0;
	    if (pid > 1 &&
		kill (pid, SIGTERM) == 0)
		    ve_waitpid_no_signal (pid, NULL, 0);
    }

    /* Figure out the server command */
    argv = NULL;
    argc = 0;
    rc = mdm_server_resolve_command_line (d,
				         TRUE /* resolve flags */,
				         vtarg,
				         &argc,
				         &argv);
    if (rc == FALSE)
       return;

    /* Do not support additional session arguments with Xnest. */
    if (d->type != TYPE_FLEXI_XNEST) {
	    if (d->xserver_session_args)
		    mdm_server_add_xserver_args (d, argv);
    }

    command = g_strjoinv (" ", argv);

    /* Fork into two processes. Parent remains the mdm process. Child
     * becomes the X server. */

    mdm_debug ("Forking X server process");

    mdm_sigterm_block_push ();
    pid = d->servpid = fork ();
    if (pid == 0)
	    mdm_unset_signals ();
    mdm_sigterm_block_pop ();
    mdm_sigchld_block_pop ();
    
    switch (pid) {
	
    case 0:
	/* the pops whacked mask again */
        mdm_unset_signals ();

	mdm_log_shutdown ();

	/* close things */
	mdm_close_all_descriptors (0 /* from */, -1 /* except */, -1 /* except2 */);

	/* No error checking here - if it's messed the best response
         * is to ignore & try to continue */
	mdm_open_dev_null (O_RDONLY); /* open stdin - fd 0 */
	mdm_open_dev_null (O_RDWR); /* open stdout - fd 1 */
	mdm_open_dev_null (O_RDWR); /* open stderr - fd 2 */

	mdm_log_init ();

	/* Rotate the X server logs */
	rotate_logs (d->name);

        /* Log all output from spawned programs to a file */
	logfile = mdm_make_filename (mdm_daemon_config_get_value_string (MDM_KEY_LOG_DIR), d->name, ".log");
	VE_IGNORE_EINTR (g_unlink (logfile));
	VE_IGNORE_EINTR (logfd = open (logfile, O_CREAT|O_TRUNC|O_WRONLY|O_EXCL, 0644));

	if (logfd != -1) {
		VE_IGNORE_EINTR (dup2 (logfd, 1));
		VE_IGNORE_EINTR (dup2 (logfd, 2));
		close (logfd);
        } else {
		mdm_error (_("%s: Could not open logfile for display %s!"),
			   "mdm_server_spawn", d->name);
	}

	g_free (logfile);

	/* The X server expects USR1/TTIN/TTOU to be SIG_IGN */
	ign_signal.sa_handler = SIG_IGN;
	ign_signal.sa_flags = SA_RESTART;
	sigemptyset (&ign_signal.sa_mask);

	if (d->server_uid == 0) {
		/* only set this if we can actually listen */
		if (sigaction (SIGUSR1, &ign_signal, NULL) < 0) {
			mdm_error (_("%s: Error setting %s to %s"),
				   "mdm_server_spawn", "USR1", "SIG_IGN");
			_exit (SERVER_ABORT);
		}
	}
	if (sigaction (SIGTTIN, &ign_signal, NULL) < 0) {
		mdm_error (_("%s: Error setting %s to %s"),
			   "mdm_server_spawn", "TTIN", "SIG_IGN");
		_exit (SERVER_ABORT);
	}
	if (sigaction (SIGTTOU, &ign_signal, NULL) < 0) {
		mdm_error (_("%s: Error setting %s to %s"),
			   "mdm_server_spawn", "TTOU", "SIG_IGN");
		_exit (SERVER_ABORT);
	}

	/* And HUP and TERM are at SIG_DFL from mdm_unset_signals,
	   we also have an empty mask and all that fun stuff */

	/* unblock signals (especially HUP/TERM/USR1) so that we
	 * can control the X server */
	sigemptyset (&mask);
	sigprocmask (SIG_SETMASK, &mask, NULL);

	if (SERVER_IS_PROXY (d)) {
		gboolean add_display = TRUE;

		g_unsetenv ("DISPLAY");
		if (d->parent_auth_file != NULL)
			g_setenv ("XAUTHORITY", d->parent_auth_file, TRUE);
		else
			g_unsetenv ("XAUTHORITY");

		if (d->type == TYPE_FLEXI_XNEST) {
			char *font_path = NULL;
			/* Add -fp with the current font path, but only if not
			 * already among the arguments */
			if (strstr (command, "-fp") == NULL)
				font_path = get_font_path (d->parent_disp);
			if (font_path != NULL) {
				argv = g_renew (char *, argv, argc + 2);
				argv[argc++] = "-fp";
				argv[argc++] = font_path;
				command = g_strconcat (command, " -fp ",
						       font_path, NULL);
			}
			add_display = FALSE;
		}

		/*
		 * Set the DISPLAY environment variable when calling
		 * nested server since some Xnest commands like Xephyr 
		 * do not support the -display argument.
		 */
		if (add_display == TRUE) {
			argv = g_renew (char *, argv, argc + 3);
			argv[argc++] = "-display";
			argv[argc++] = d->parent_disp;
			argv[argc++] = NULL;
			command = g_strconcat (command, " -display ",
				       d->parent_disp, NULL);
		} else {
			argv = g_renew (char *, argv, argc + 1);
			argv[argc++] = NULL;
			g_setenv ("DISPLAY", d->parent_disp, TRUE);
		}
	}

	if (argv[0] == NULL) {
		mdm_error (_("%s: Empty server command for display %s"),
			   "mdm_server_spawn",
			   d->name);
		_exit (SERVER_ABORT);
	}

	mdm_debug ("mdm_server_spawn: '%s'", command);
	
	if (d->priority != MDM_PRIO_DEFAULT) {
		if (setpriority (PRIO_PROCESS, 0, d->priority)) {
			mdm_error (_("%s: Server priority couldn't be set to %d: %s"),
				   "mdm_server_spawn", d->priority,
				   strerror (errno));
		}
	}

	setpgid (0, 0);

	if (d->server_uid != 0) {
		struct passwd *pwent;
		pwent = getpwuid (d->server_uid);
		if (pwent == NULL) {
			mdm_error (_("%s: Server was to be spawned by uid %d but "
				     "that user doesn't exist"),
				   "mdm_server_spawn",
				   (int)d->server_uid);
			_exit (SERVER_ABORT);
		}
	/*
	 * When started as root, X will read $HOME/xorg.conf before files in /etc.  As this isn't what users
	 * expect from mdm starting up, we set $HOME to be /etc/X11.  (an unset $HOME will cause the X server
	 * to bail)  The Debian X server will have this removed soon, and hopefully the change will go
	 * upstream, at which point the original code will not cause user confusion.
	 */
#if 0
		if (pwent->pw_dir != NULL &&
		    g_file_test (pwent->pw_dir, G_FILE_TEST_EXISTS))
			g_setenv ("HOME", pwent->pw_dir, TRUE);
		else
			g_setenv ("HOME", "/", TRUE); /* Hack */
#endif
		g_setenv ("HOME", "/etc/X11", TRUE);
		g_setenv ("SHELL", pwent->pw_shell, TRUE);
		g_unsetenv ("MAIL");

		if (setgid (pwent->pw_gid) < 0)  {
			mdm_error (_("%s: Couldn't set groupid to %d"), 
				   "mdm_server_spawn", (int)pwent->pw_gid);
			_exit (SERVER_ABORT);
		}

		if (initgroups (pwent->pw_name, pwent->pw_gid) < 0) {
			mdm_error (_("%s: initgroups () failed for %s"),
				   "mdm_server_spawn", pwent->pw_name);
			_exit (SERVER_ABORT);
		}

		if (setuid (d->server_uid) < 0)  {
			mdm_error (_("%s: Couldn't set userid to %d"),
				   "mdm_server_spawn", (int)d->server_uid);
			_exit (SERVER_ABORT);
		}
	} else {
		gid_t groups[1] = { 0 };
		if (setgid (0) < 0)  {
			mdm_error (_("%s: Couldn't set groupid to 0"), 
				   "mdm_server_spawn");
			/* Don't error out, it's not fatal, if it fails we'll
			 * just still be */
		}
		/* this will get rid of any suplementary groups etc... */
		setgroups (1, groups);
	}

#if __sun
    {
        /* Remove old communication pipe, if present */
        char old_pipe[MAXPATHLEN];

        sprintf (old_pipe, "%s/%d", MDM_SDTLOGIN_DIR, d->name);
        g_unlink (old_pipe);
    }
#endif

	VE_IGNORE_EINTR (execv (argv[0], argv));

	mdm_fdprintf (2, "MDM: Xserver not found: %s\n"
		      "Error: Command could not be executed!\n"
		      "Please install the X server or correct "
		      "MDM configuration and restart MDM.",
		      command);

	mdm_error (_("%s: Xserver not found: %s"), 
		   "mdm_server_spawn", command);
	
	_exit (SERVER_ABORT);
	
    case -1:
	g_strfreev (argv);
	g_free (command);
	mdm_error (_("%s: Can't fork Xserver process!"),
		   "mdm_server_spawn");
	d->servpid = 0;
	d->servstat = SERVER_DEAD;

	break;
	
    default:
	g_strfreev (argv);
	g_free (command);
	mdm_debug ("%s: Forked server on pid %d", 
		   "mdm_server_spawn", (int)pid);
	break;
    }
}

/**
 * mdm_server_usr1_handler:
 * @sig: Signal value
 *
 * Received when the server is ready to accept connections
 */

static void
mdm_server_usr1_handler (gint sig)
{
    mdm_in_signal++;

    d->servstat = SERVER_RUNNING; /* Server ready to accept connections */
    d->starttime = time (NULL);

    server_signal_notified = TRUE;
    /* this will quit the select */
    VE_IGNORE_EINTR (write (server_signal_pipe[1], "Yay!", 4));

    mdm_in_signal--;
}


/**
 * mdm_server_child_handler:
 * @sig: Signal value
 *
 * Received when server died during startup
 */

static void 
mdm_server_child_handler (int signal)
{
	mdm_in_signal++;

	/* go to the main child handler */
	mdm_slave_child_handler (signal);

	/* this will quit the select */
	VE_IGNORE_EINTR (write (server_signal_pipe[1], "Yay!", 4));

	mdm_in_signal--;
}


void
mdm_server_whack_clients (Display *dsp)
{
	int i, screen_count;
	int (* old_xerror_handler) (Display *, XErrorEvent *);

	if (dsp == NULL)
		return;

	old_xerror_handler = XSetErrorHandler (ignore_xerror_handler);

	XGrabServer (dsp);

	screen_count = ScreenCount (dsp);

	for (i = 0; i < screen_count; i++) {
		Window root_ret, parent_ret;
		Window *childs = NULL;
		unsigned int childs_count = 0;
		Window root = RootWindow (dsp, i);

		while (XQueryTree (dsp, root, &root_ret, &parent_ret,
				   &childs, &childs_count) &&
		       childs_count > 0) {
			int ii;

			for (ii = 0; ii < childs_count; ii++) {
				XKillClient (dsp, childs[ii]);
			}

			XFree (childs);
		}
	}

	XUngrabServer (dsp);

	XSync (dsp, False);
	XSetErrorHandler (old_xerror_handler);
}

static char *
get_font_path (const char *display)
{
	Display *disp;
	char **font_path;
	int n_fonts;
	int i;
	GString *gs;

	disp = XOpenDisplay (display);
	if (disp == NULL)
		return NULL;

	font_path = XGetFontPath (disp, &n_fonts);
	if (font_path == NULL) {
		XCloseDisplay (disp);
		return NULL;
	}

	gs = g_string_new (NULL);
	for (i = 0; i < n_fonts; i++) {
		if (i != 0)
			g_string_append_c (gs, ',');

	        if (mdm_daemon_config_get_value_bool (MDM_KEY_XNEST_UNSCALED_FONT_PATH) == TRUE)
			g_string_append (gs, font_path[i]);
		else {
			gchar *unscaled_ptr = NULL;

			/*
			 * When using Xsun Xnest, it doesn't support the
			 * ":unscaled" suffix in fontpath entries, so strip it.
			 */
			unscaled_ptr = g_strrstr (font_path[i], ":unscaled");
			if (unscaled_ptr != NULL) {
				gchar *temp_string;

				temp_string = g_strndup (font_path[i],
					strlen (font_path[i]) -
					strlen (":unscaled"));

mdm_debug ("font_path[i] is %s, temp_string is %s", font_path[i], temp_string);
				g_string_append (gs, temp_string);
				g_free (temp_string);
			} else {
mdm_debug ("font_path[i] is %s", font_path[i]);
				g_string_append (gs, font_path[i]);
			}
		}
	}

	XFreeFontPath (font_path);

	XCloseDisplay (disp);

	return g_string_free (gs, FALSE);
}

/* EOF */
