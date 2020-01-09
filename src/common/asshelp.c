/* asshelp.c - Helper functions for Assuan
 * Copyright (C) 2002, 2004, 2007, 2009 Free Software Foundation, Inc.
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#ifdef HAVE_LOCALE_H
#include <locale.h>
#endif

#include "i18n.h"
#include "util.h"
#include "exechelp.h"
#include "sysutils.h"
#include "status.h" 
#include "asshelp.h"

static gpg_error_t
send_one_option (assuan_context_t ctx, gpg_err_source_t errsource,
                 const char *name, const char *value, int use_putenv)
{
  gpg_error_t err;
  char *optstr;

  (void)errsource;

  if (!value || !*value)
    err = 0;  /* Avoid sending empty strings.  */
  else if (asprintf (&optstr, "OPTION %s%s=%s", 
                     use_putenv? "putenv=":"", name, value) < 0)
    err = gpg_error_from_syserror ();
  else
    {
      err = assuan_transact (ctx, optstr, NULL, NULL, NULL, NULL, NULL, NULL);
      xfree (optstr);
    }

  return err;
}


/* Send the assuan commands pertaining to the pinentry environment.  The
   OPT_* arguments are optional and may be used to override the
   defaults taken from the current locale. */
gpg_error_t
send_pinentry_environment (assuan_context_t ctx,
                           gpg_err_source_t errsource,
                           const char *opt_lc_ctype,
                           const char *opt_lc_messages,
                           session_env_t session_env)

{
  gpg_error_t err = 0;
  char *old_lc = NULL; 
  char *dft_lc = NULL;
  const char *dft_ttyname;
  int iterator;
  const char *name, *assname, *value;
  int is_default;

  iterator = 0; 
  while ((name = session_env_list_stdenvnames (&iterator, &assname)))
    {
      value = session_env_getenv_or_default (session_env, name, NULL);
      if (!value)
        continue;

      if (assname)
        err = send_one_option (ctx, errsource, assname, value, 0);
      else
        {
          err = send_one_option (ctx, errsource, name, value, 1);
          if (gpg_err_code (err) == GPG_ERR_UNKNOWN_OPTION)
            err = 0;  /* Server too old; can't pass the new envvars.  */
        }
      if (err)
        return err;
    }


  dft_ttyname = session_env_getenv_or_default (session_env, "GPG_TTY", 
                                               &is_default);
  if (dft_ttyname && !is_default)
    dft_ttyname = NULL;  /* We need the default value.  */

  /* Send the value for LC_CTYPE.  */
#if defined(HAVE_SETLOCALE) && defined(LC_CTYPE)
  old_lc = setlocale (LC_CTYPE, NULL);
  if (old_lc)
    {
      old_lc = xtrystrdup (old_lc);
      if (!old_lc)
        return gpg_error_from_syserror ();
    }
  dft_lc = setlocale (LC_CTYPE, "");
#endif
  if (opt_lc_ctype || (dft_ttyname && dft_lc))
    {
      err = send_one_option (ctx, errsource, "lc-ctype", 
                             opt_lc_ctype ? opt_lc_ctype : dft_lc, 0);
    }
#if defined(HAVE_SETLOCALE) && defined(LC_CTYPE)
  if (old_lc)
    {
      setlocale (LC_CTYPE, old_lc);
      xfree (old_lc);
    }
#endif
  if (err)
    return err;

  /* Send the value for LC_MESSAGES.  */
#if defined(HAVE_SETLOCALE) && defined(LC_MESSAGES)
  old_lc = setlocale (LC_MESSAGES, NULL);
  if (old_lc)
    {
      old_lc = xtrystrdup (old_lc);
      if (!old_lc)
        return gpg_error_from_syserror ();
    }
  dft_lc = setlocale (LC_MESSAGES, "");
#endif
  if (opt_lc_messages || (dft_ttyname && dft_lc))
    {
      err = send_one_option (ctx, errsource, "lc-messages", 
                             opt_lc_messages ? opt_lc_messages : dft_lc, 0);
    }
#if defined(HAVE_SETLOCALE) && defined(LC_MESSAGES)
  if (old_lc)
    {
      setlocale (LC_MESSAGES, old_lc);
      xfree (old_lc);
    }
#endif
  if (err)
    return err;

  return 0;
}


/* Try to connect to the agent via socket or fork it off and work by
   pipes.  Handle the server's initial greeting.  Returns a new assuan
   context at R_CTX or an error code. */
gpg_error_t
start_new_gpg_agent (assuan_context_t *r_ctx,
                     gpg_err_source_t errsource,
                     const char *homedir,
                     const char *agent_program,
                     const char *opt_lc_ctype,
                     const char *opt_lc_messages,
                     session_env_t session_env,
                     int verbose, int debug,
                     gpg_error_t (*status_cb)(ctrl_t, int, ...),
                     ctrl_t status_cb_arg)
{
  /* If we ever failed to connect via a socket we will force the use
     of the pipe based server for the lifetime of the process.  */
  static int force_pipe_server = 0;

  gpg_error_t rc = 0;
  char *infostr, *p;
  assuan_context_t ctx;

  *r_ctx = NULL;

 restart:
  infostr = force_pipe_server? NULL : getenv ("GPG_AGENT_INFO");
  if (!infostr || !*infostr)
    {
      char *sockname;

      /* First check whether we can connect at the standard
         socket.  */
      sockname = make_filename (homedir, "S.gpg-agent", NULL);
      rc = assuan_socket_connect (&ctx, sockname, 0);

      if (rc)
        {
          /* With no success start a new server.  */
          if (verbose)
            log_info (_("no running gpg-agent - starting one\n"));
          
          if (status_cb)
            status_cb (status_cb_arg, STATUS_PROGRESS, 
                       "starting_agent ? 0 0", NULL);
          
          if (fflush (NULL))
            {
              gpg_error_t tmperr = gpg_error (gpg_err_code_from_errno (errno));
              log_error ("error flushing pending output: %s\n",
                         strerror (errno));
              xfree (sockname);
              return tmperr;
            }
          
          if (!agent_program || !*agent_program)
            agent_program = gnupg_module_name (GNUPG_MODULE_NAME_AGENT);

#ifdef HAVE_W32_SYSTEM
          {
            /* Under Windows we start the server in daemon mode.  This
               is because the default is to use the standard socket
               and thus there is no need for the GPG_AGENT_INFO
               envvar.  This is possible as we don't have a real unix
               domain socket but use a plain file and thus there is no
               need to care about non-local file systems. */
            const char *argv[3];

            argv[0] = "--daemon";
            argv[1] = "--use-standard-socket"; 
            argv[2] = NULL;  

            rc = gnupg_spawn_process_detached (agent_program, argv, NULL);
            if (rc)
              log_debug ("failed to start agent `%s': %s\n",
                         agent_program, gpg_strerror (rc));
            else
              {
                /* Give the agent some time to prepare itself. */
                gnupg_sleep (3);
                /* Now try again to connect the agent.  */
                rc = assuan_socket_connect (&ctx, sockname, 0);
              }
          }
#else /*!HAVE_W32_SYSTEM*/
          {
            const char *pgmname;
            const char *argv[3];
            int no_close_list[3];
            int i;

            if ( !(pgmname = strrchr (agent_program, '/')))
              pgmname = agent_program;
            else
              pgmname++;
            
            argv[0] = pgmname;
            argv[1] = "--server";
            argv[2] = NULL;
            
            i=0;
            if (log_get_fd () != -1)
              no_close_list[i++] = log_get_fd ();
            no_close_list[i++] = fileno (stderr);
            no_close_list[i] = -1;
            
            /* Connect to the agent and perform initial handshaking. */
            rc = assuan_pipe_connect (&ctx, agent_program, argv,
                                      no_close_list);
          }
#endif /*!HAVE_W32_SYSTEM*/
        }
      xfree (sockname);
    }
  else
    {
      int prot;
      int pid;

      infostr = xstrdup (infostr);
      if ( !(p = strchr (infostr, PATHSEP_C)) || p == infostr)
        {
          log_error (_("malformed GPG_AGENT_INFO environment variable\n"));
          xfree (infostr);
          force_pipe_server = 1;
          goto restart;
        }
      *p++ = 0;
      pid = atoi (p);
      while (*p && *p != PATHSEP_C)
        p++;
      prot = *p? atoi (p+1) : 0;
      if (prot != 1)
        {
          log_error (_("gpg-agent protocol version %d is not supported\n"),
                     prot);
          xfree (infostr);
          force_pipe_server = 1;
          goto restart;
        }

      rc = assuan_socket_connect (&ctx, infostr, pid);
      xfree (infostr);
      if (gpg_err_code (rc) == GPG_ERR_ASS_CONNECT_FAILED)
        {
          log_info (_("can't connect to the agent - trying fall back\n"));
          force_pipe_server = 1;
          goto restart;
        }
    }

  if (rc)
    {
      log_error ("can't connect to the agent: %s\n", gpg_strerror (rc));
      return gpg_error (GPG_ERR_NO_AGENT);
    }

  if (debug)
    log_debug ("connection to agent established\n");

  rc = assuan_transact (ctx, "RESET",
                        NULL, NULL, NULL, NULL, NULL, NULL);
  if (!rc)
    rc = send_pinentry_environment (ctx, errsource,
                                    opt_lc_ctype, opt_lc_messages,
                                    session_env);
  if (rc)
    {
      assuan_disconnect (ctx);
      return rc;
    }

  *r_ctx = ctx;
  return 0;
}

