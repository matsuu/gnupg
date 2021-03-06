/* ks-engine-hkp.c - HKP keyserver engine
 * Copyright (C) 2011, 2012 Free Software Foundation, Inc.
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

#warning fixme Windows part not yet done
#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#ifdef HAVE_W32_SYSTEM
# include <windows.h>
#else /*!HAVE_W32_SYSTEM*/
# include <sys/types.h>
# include <sys/socket.h>
# include <netdb.h>
#endif /*!HAVE_W32_SYSTEM*/

#include "dirmngr.h"
#include "misc.h"
#include "userids.h"
#include "ks-engine.h"

/* To match the behaviour of our old gpgkeys helper code we escape
   more characters than actually needed. */
#define EXTRA_ESCAPE_CHARS "@!\"#$%&'()*+,-./:;<=>?[\\]^_{|}~"

/* How many redirections do we allow.  */
#define MAX_REDIRECTS 2

/* Objects used to maintain information about hosts.  */
struct hostinfo_s;
typedef struct hostinfo_s *hostinfo_t;
struct hostinfo_s
{
  time_t lastfail;   /* Time we tried to connect and failed.  */
  time_t lastused;   /* Time of last use.  */
  int *pool;         /* A -1 terminated array with indices into
                        HOSTTABLE or NULL if NAME is not a pool
                        name.  */
  int poolidx;       /* Index into POOL with the used host.  */
  unsigned int v4:1; /* Host supports AF_INET.  */
  unsigned int v6:1; /* Host supports AF_INET6.  */
  unsigned int dead:1; /* Host is currently unresponsive.  */
  char name[1];      /* The hostname.  */
};


/* An array of hostinfo_t for all hosts requested by the caller or
   resolved from a pool name and its allocated size.*/
static hostinfo_t *hosttable;
static int hosttable_size;

/* The number of host slots we initally allocate for HOSTTABLE.  */
#define INITIAL_HOSTTABLE_SIZE 10


/* Create a new hostinfo object, fill in NAME and put it into
   HOSTTABLE.  Return the index into hosttable on success or -1 on
   error. */
static int
create_new_hostinfo (const char *name)
{
  hostinfo_t hi, *newtable;
  int newsize;
  int idx, rc;

  hi = xtrymalloc (sizeof *hi + strlen (name));
  if (!hi)
    return -1;
  strcpy (hi->name, name);
  hi->pool = NULL;
  hi->poolidx = -1;
  hi->lastused = (time_t)(-1);
  hi->lastfail = (time_t)(-1);
  hi->v4 = 0;
  hi->v6 = 0;

  /* Add it to the hosttable. */
  for (idx=0; idx < hosttable_size; idx++)
    if (!hosttable[idx])
      {
        hosttable[idx] = hi;
        return idx;
      }
  /* Need to extend the hosttable.  */
  newsize = hosttable_size + INITIAL_HOSTTABLE_SIZE;
  newtable = xtryrealloc (hosttable, newsize * sizeof *hosttable);
  if (!newtable)
    {
      xfree (hi);
      return -1;
    }
  hosttable = newtable;
  idx = hosttable_size;
  hosttable_size = newsize;
  rc = idx;
  hosttable[idx++] = hi;
  while (idx < hosttable_size)
    hosttable[idx++] = NULL;

  return rc;
}


/* Find the host NAME in our table.  Return the index into the
   hosttable or -1 if not found.  */
static int
find_hostinfo (const char *name)
{
  int idx;

  for (idx=0; idx < hosttable_size; idx++)
    if (hosttable[idx] && !ascii_strcasecmp (hosttable[idx]->name, name))
      return idx;
  return -1;
}


static int
sort_hostpool (const void *xa, const void *xb)
{
  int a = *(int *)xa;
  int b = *(int *)xb;

  assert (a >= 0 && a < hosttable_size);
  assert (b >= 0 && b < hosttable_size);
  assert (hosttable[a]);
  assert (hosttable[b]);

  return ascii_strcasecmp (hosttable[a]->name, hosttable[b]->name);
}


/* Select a random host.  Consult TABLE which indices into the global
   hosttable.  Returns index into TABLE or -1 if no host could be
   selected.  */
static int
select_random_host (int *table)
{
  int *tbl;
  size_t tblsize;
  int pidx, idx;

  /* We create a new table so that we select only from currently alive
     hosts.  */
  for (idx=0, tblsize=0; (pidx = table[idx]) != -1; idx++)
    if (hosttable[pidx] && !hosttable[pidx]->dead)
      tblsize++;
  if (!tblsize)
    return -1; /* No hosts.  */

  tbl = xtrymalloc (tblsize * sizeof *tbl);
  if (!tbl)
    return -1;
  for (idx=0, tblsize=0; (pidx = table[idx]) != -1; idx++)
    if (hosttable[pidx] && !hosttable[pidx]->dead)
      tbl[tblsize++] = pidx;

  if (tblsize == 1)  /* Save a get_uint_nonce.  */
    pidx = tbl[0];
  else
    pidx = get_uint_nonce () % tblsize;

  xfree (tbl);
  return pidx;
}


/* Map the host name NAME to the actual to be used host name.  This
   allows us to manage round robin DNS names.  We use our own strategy
   to choose one of the hosts.  For example we skip those hosts which
   failed for some time and we stick to one host for a time
   independent of DNS retry times.  */
static char *
map_host (const char *name)
{
  hostinfo_t hi;
  int idx;

  /* No hostname means localhost.  */
  if (!name || !*name)
    return xtrystrdup ("localhost");

  /* See whether the host is in our table.  */
  idx = find_hostinfo (name);
  if (idx == -1)
    {
      /* We never saw this host.  Allocate a new entry.  */
      struct addrinfo hints, *aibuf, *ai;
      int *reftbl;
      size_t reftblsize;
      int refidx;

      reftblsize = 100;
      reftbl = xtrymalloc (reftblsize * sizeof *reftbl);
      if (!reftbl)
        return NULL;
      refidx = 0;

      idx = create_new_hostinfo (name);
      if (idx == -1)
        {
          xfree (reftbl);
          return NULL;
        }
      hi = hosttable[idx];

      /* Find all A records for this entry and put them into the pool
         list - if any.  */
      memset (&hints, 0, sizeof (hints));
      hints.ai_socktype = SOCK_STREAM;
      if (!getaddrinfo (name, NULL, &hints, &aibuf))
        {
          for (ai = aibuf; ai; ai = ai->ai_next)
            {
              char tmphost[NI_MAXHOST];
              int tmpidx;
              int ec;
              int i;

              if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6)
                continue;

              log_printhex ("getaddrinfo returned", ai->ai_addr,ai->ai_addrlen);
              if ((ec=getnameinfo (ai->ai_addr, ai->ai_addrlen,
                                   tmphost, sizeof tmphost,
                                   NULL, 0, NI_NAMEREQD)))
                log_info ("getnameinfo failed while checking '%s': %s\n",
                          name, gai_strerror (ec));
              else if (refidx+1 >= reftblsize)
                {
                  log_error ("getnameinfo returned for '%s': '%s'"
                            " [index table full - ignored]\n", name, tmphost);
                }
              else
                {

                  if ((tmpidx = find_hostinfo (tmphost)) != -1)
                    {
                      log_info ("getnameinfo returned for '%s': '%s'"
                                " [already known]\n", name, tmphost);
                      if (ai->ai_family == AF_INET)
                        hosttable[tmpidx]->v4 = 1;
                      if (ai->ai_family == AF_INET6)
                        hosttable[tmpidx]->v6 = 1;

                      for (i=0; i < refidx; i++)
                        if (reftbl[i] == tmpidx)
                      break;
                      if (!(i < refidx) && tmpidx != idx)
                        reftbl[refidx++] = tmpidx;
                    }
                  else
                    {
                      log_info ("getnameinfo returned for '%s': '%s'\n",
                                name, tmphost);
                      /* Create a new entry.  */
                      tmpidx = create_new_hostinfo (tmphost);
                      if (tmpidx == -1)
                        log_error ("map_host for '%s' problem: %s - '%s'"
                                   " [ignored]\n",
                                   name, strerror (errno), tmphost);
                      else
                        {
                          if (ai->ai_family == AF_INET)
                            hosttable[tmpidx]->v4 = 1;
                          if (ai->ai_family == AF_INET6)
                            hosttable[tmpidx]->v6 = 1;

                          for (i=0; i < refidx; i++)
                            if (reftbl[i] == tmpidx)
                              break;
                          if (!(i < refidx) && tmpidx != idx)
                            reftbl[refidx++] = tmpidx;
                        }
                    }
                }
            }
        }
      reftbl[refidx] = -1;
      if (refidx)
        {
          assert (!hi->pool);
          hi->pool = xtryrealloc (reftbl, (refidx+1) * sizeof *reftbl);
          if (!hi->pool)
            {
              log_error ("shrinking index table in map_host failed: %s\n",
                         strerror (errno));
              xfree (reftbl);
            }
          qsort (reftbl, refidx, sizeof *reftbl, sort_hostpool);
        }
      else
        xfree (reftbl);
    }

  hi = hosttable[idx];
  if (hi->pool)
    {
      /* If the currently selected host is now marked dead, force a
         re-selection .  */
      if (hi->poolidx >= 0 && hi->poolidx < hosttable_size
          && hosttable[hi->poolidx] && hosttable[hi->poolidx]->dead)
        hi->poolidx = -1;

      /* Select a host if needed.  */
      if (hi->poolidx == -1)
        {
          hi->poolidx = select_random_host (hi->pool);
          if (hi->poolidx == -1)
            {
              log_error ("no alive host found in pool '%s'\n", name);
              return NULL;
            }
        }

      assert (hi->poolidx >= 0 && hi->poolidx < hosttable_size);
      hi = hosttable[hi->poolidx];
      assert (hi);
    }

  if (hi->dead)
    {
      log_error ("host '%s' marked as dead\n", hi->name);
      return NULL;
    }

  return xtrystrdup (hi->name);
}


/* Mark the host NAME as dead.  */
static void
mark_host_dead (const char *name)
{
  hostinfo_t hi;
  int idx;

  if (!name || !*name || !strcmp (name, "localhost"))
    return;

  idx = find_hostinfo (name);
  if (idx == -1)
    return;
  hi = hosttable[idx];
  log_info ("marking host '%s' as dead%s\n", hi->name, hi->dead? " (again)":"");
  hi->dead = 1;
}


/* Debug function to print the entire hosttable.  */
void
ks_hkp_print_hosttable (void)
{
  int idx, idx2;
  hostinfo_t hi;

  for (idx=0; idx < hosttable_size; idx++)
    if ((hi=hosttable[idx]))
      {
        log_info ("hosttable %3d %s %s %s %s\n",
                  idx, hi->v4? "4":" ", hi->v6? "6":" ",
                  hi->dead? "d":" ", hi->name);
        if (hi->pool)
          {
            log_info ("          -->");
            for (idx2=0; hi->pool[idx2] != -1; idx2++)
              {
                log_printf (" %d", hi->pool[idx2]);
                if (hi->poolidx == idx2)
                  log_printf ("*");
              }
            log_printf ("\n");
            /* for (idx2=0; hi->pool[idx2] != -1; idx2++) */
            /*   log_info ("              (%s)\n", */
            /*              hosttable[hi->pool[idx2]]->name); */
          }
      }
}



/* Print a help output for the schemata supported by this module. */
gpg_error_t
ks_hkp_help (ctrl_t ctrl, parsed_uri_t uri)
{
  const char const data[] =
    "Handler for HKP URLs:\n"
    "  hkp://\n"
    "Supported methods: search, get, put\n";
  gpg_error_t err;

  if (!uri)
    err = ks_print_help (ctrl, "  hkp");
  else if (uri->is_http && !strcmp (uri->scheme, "hkp"))
    err = ks_print_help (ctrl, data);
  else
    err = 0;

  return err;
}


/* Build the remote part or the URL from SCHEME, HOST and an optional
   PORT.  Returns an allocated string or NULL on failure and sets
   ERRNO.  */
static char *
make_host_part (const char *scheme, const char *host, unsigned short port)
{
  char portstr[10];
  char *hostname;
  char *hostport;

  /* Map scheme and port.  */
  if (!strcmp (scheme, "hkps") || !strcmp (scheme,"https"))
    {
      scheme = "https";
      strcpy (portstr, "443");
    }
  else /* HKP or HTTP.  */
    {
      scheme = "http";
      strcpy (portstr, "11371");
    }
  if (port)
    snprintf (portstr, sizeof portstr, "%hu", port);
  else
    {
      /*fixme_do_srv_lookup ()*/
    }

  hostname = map_host (host);
  if (!hostname)
    return NULL;

  hostport = strconcat (scheme, "://", hostname, ":", portstr, NULL);
  xfree (hostname);
  return hostport;
}


/* Send an HTTP request.  On success returns an estream object at
   R_FP.  HOSTPORTSTR is only used for diagnostics.  If POST_CB is not
   NULL a post request is used and that callback is called to allow
   writing the post data.  */
static gpg_error_t
send_request (ctrl_t ctrl, const char *request, const char *hostportstr,
              gpg_error_t (*post_cb)(void *, http_t), void *post_cb_value,
              estream_t *r_fp)
{
  gpg_error_t err;
  http_t http = NULL;
  int redirects_left = MAX_REDIRECTS;
  estream_t fp = NULL;
  char *request_buffer = NULL;

  *r_fp = NULL;

 once_more:
  err = http_open (&http,
                   post_cb? HTTP_REQ_POST : HTTP_REQ_GET,
                   request,
                   /* fixme: AUTH */ NULL,
                   0,
                   /* fixme: proxy*/ NULL,
                   NULL, NULL,
                   /*FIXME curl->srvtag*/NULL);
  if (!err)
    {
      fp = http_get_write_ptr (http);
      /* Avoid caches to get the most recent copy of the key.  We set
         both the Pragma and Cache-Control versions of the header, so
         we're good with both HTTP 1.0 and 1.1.  */
      es_fputs ("Pragma: no-cache\r\n"
                "Cache-Control: no-cache\r\n", fp);
      if (post_cb)
        err = post_cb (post_cb_value, http);
      if (!err)
        {
          http_start_data (http);
          if (es_ferror (fp))
            err = gpg_error_from_syserror ();
        }
    }
  if (err)
    {
      /* Fixme: After a redirection we show the old host name.  */
      log_error (_("error connecting to '%s': %s\n"),
                 hostportstr, gpg_strerror (err));
      goto leave;
    }

  /* Wait for the response.  */
  dirmngr_tick (ctrl);
  err = http_wait_response (http);
  if (err)
    {
      log_error (_("error reading HTTP response for '%s': %s\n"),
                 hostportstr, gpg_strerror (err));
      goto leave;
    }

  switch (http_get_status_code (http))
    {
    case 200:
      err = 0;
      break; /* Success.  */

    case 301:
    case 302:
      {
        const char *s = http_get_header (http, "Location");

        log_info (_("URL '%s' redirected to '%s' (%u)\n"),
                  request, s?s:"[none]", http_get_status_code (http));
        if (s && *s && redirects_left-- )
          {
            xfree (request_buffer);
            request_buffer = xtrystrdup (s);
            if (request_buffer)
              {
                request = request_buffer;
                http_close (http, 0);
                http = NULL;
                goto once_more;
              }
            err = gpg_error_from_syserror ();
          }
        else
          err = gpg_error (GPG_ERR_NO_DATA);
        log_error (_("too many redirections\n"));
      }
      goto leave;

    default:
      log_error (_("error accessing '%s': http status %u\n"),
                 request, http_get_status_code (http));
      err = gpg_error (GPG_ERR_NO_DATA);
      goto leave;
    }

  fp = http_get_read_ptr (http);
  if (!fp)
    {
      err = gpg_error (GPG_ERR_BUG);
      goto leave;
    }

  /* Return the read stream and close the HTTP context.  */
  *r_fp = fp;
  http_close (http, 1);
  http = NULL;

 leave:
  http_close (http, 0);
  xfree (request_buffer);
  return err;
}


static gpg_error_t
armor_data (char **r_string, const void *data, size_t datalen)
{
  gpg_error_t err;
  struct b64state b64state;
  estream_t fp;
  long length;
  char *buffer;
  size_t nread;

  *r_string = NULL;

  fp = es_fopenmem (0, "rw");
  if (!fp)
    return gpg_error_from_syserror ();

  if ((err=b64enc_start_es (&b64state, fp, "PGP PUBLIC KEY BLOCK"))
      || (err=b64enc_write (&b64state, data, datalen))
      || (err = b64enc_finish (&b64state)))
    {
      es_fclose (fp);
      return err;
    }

  /* FIXME: To avoid the extra buffer allocation estream should
     provide a function to snatch the internal allocated memory from
     such a memory stream.  */
  length = es_ftell (fp);
  if (length < 0)
    {
      err = gpg_error_from_syserror ();
      es_fclose (fp);
      return err;
    }

  buffer = xtrymalloc (length+1);
  if (!buffer)
    {
      err = gpg_error_from_syserror ();
      es_fclose (fp);
      return err;
    }

  es_rewind (fp);
  if (es_read (fp, buffer, length, &nread))
    {
      err = gpg_error_from_syserror ();
      es_fclose (fp);
      return err;
    }
  buffer[nread] = 0;
  es_fclose (fp);

  *r_string = buffer;
  return 0;
}




/* Search the keyserver identified by URI for keys matching PATTERN.
   On success R_FP has an open stream to read the data.  */
gpg_error_t
ks_hkp_search (ctrl_t ctrl, parsed_uri_t uri, const char *pattern,
               estream_t *r_fp)
{
  gpg_error_t err;
  KEYDB_SEARCH_DESC desc;
  char fprbuf[2+40+1];
  char *hostport = NULL;
  char *request = NULL;
  estream_t fp = NULL;

  *r_fp = NULL;

  /* Remove search type indicator and adjust PATTERN accordingly.
     Note that HKP keyservers like the 0x to be present when searching
     by keyid.  We need to re-format the fingerprint and keyids so to
     remove the gpg specific force-use-of-this-key flag ("!").  */
  err = classify_user_id (pattern, &desc, 1);
  if (err)
    return err;
  switch (desc.mode)
    {
    case KEYDB_SEARCH_MODE_EXACT:
    case KEYDB_SEARCH_MODE_SUBSTR:
    case KEYDB_SEARCH_MODE_MAIL:
    case KEYDB_SEARCH_MODE_MAILSUB:
      pattern = desc.u.name;
      break;
    case KEYDB_SEARCH_MODE_SHORT_KID:
      snprintf (fprbuf, sizeof fprbuf, "0x%08lX", (ulong)desc.u.kid[1]);
      pattern = fprbuf;
      break;
    case KEYDB_SEARCH_MODE_LONG_KID:
      snprintf (fprbuf, sizeof fprbuf, "0x%08lX%08lX",
                (ulong)desc.u.kid[0], (ulong)desc.u.kid[1]);
      pattern = fprbuf;
      break;
    case KEYDB_SEARCH_MODE_FPR16:
      bin2hex (desc.u.fpr, 16, fprbuf);
      pattern = fprbuf;
      break;
    case KEYDB_SEARCH_MODE_FPR20:
    case KEYDB_SEARCH_MODE_FPR:
      bin2hex (desc.u.fpr, 20, fprbuf);
      pattern = fprbuf;
      break;
    default:
      return gpg_error (GPG_ERR_INV_USER_ID);
    }

  /* Build the request string.  */
  {
    char *searchkey;

    hostport = make_host_part (uri->scheme, uri->host, uri->port);
    if (!hostport)
      {
        err = gpg_error_from_syserror ();
        goto leave;
      }

    searchkey = http_escape_string (pattern, EXTRA_ESCAPE_CHARS);
    if (!searchkey)
      {
        err = gpg_error_from_syserror ();
        goto leave;
      }

    request = strconcat (hostport,
                         "/pks/lookup?op=index&options=mr&search=",
                         searchkey,
                         NULL);
    xfree (searchkey);
    if (!request)
      {
        err = gpg_error_from_syserror ();
        goto leave;
      }
  }

  /* Send the request.  */
  err = send_request (ctrl, request, hostport, NULL, NULL, &fp);
  if (err)
    goto leave;

  /* Start reading the response.  */
  {
    int c = es_getc (fp);
    if (c == -1)
      {
        err = es_ferror (fp)?gpg_error_from_syserror ():gpg_error (GPG_ERR_EOF);
        log_error ("error reading response: %s\n", gpg_strerror (err));
        goto leave;
      }
    if (c == '<')
      {
        /* The document begins with a '<', assume it's a HTML
           response, which we don't support.  */
        err = gpg_error (GPG_ERR_UNSUPPORTED_ENCODING);
        goto leave;
      }
    es_ungetc (c, fp);
  }

  /* Return the read stream.  */
  *r_fp = fp;
  fp = NULL;

 leave:
  es_fclose (fp);
  xfree (request);
  xfree (hostport);
  return err;
}


/* Get the key described key the KEYSPEC string from the keyserver
   identified by URI.  On success R_FP has an open stream to read the
   data.  */
gpg_error_t
ks_hkp_get (ctrl_t ctrl, parsed_uri_t uri, const char *keyspec, estream_t *r_fp)
{
  gpg_error_t err;
  KEYDB_SEARCH_DESC desc;
  char kidbuf[40+1];
  char *hostport = NULL;
  char *request = NULL;
  estream_t fp = NULL;

  *r_fp = NULL;

  /* Remove search type indicator and adjust PATTERN accordingly.
     Note that HKP keyservers like the 0x to be present when searching
     by keyid.  We need to re-format the fingerprint and keyids so to
     remove the gpg specific force-use-of-this-key flag ("!").  */
  err = classify_user_id (keyspec, &desc, 1);
  if (err)
    return err;
  switch (desc.mode)
    {
    case KEYDB_SEARCH_MODE_SHORT_KID:
      snprintf (kidbuf, sizeof kidbuf, "%08lX", (ulong)desc.u.kid[1]);
      break;
    case KEYDB_SEARCH_MODE_LONG_KID:
      snprintf (kidbuf, sizeof kidbuf, "%08lX%08lX",
		(ulong)desc.u.kid[0], (ulong)desc.u.kid[1]);
      break;
    case KEYDB_SEARCH_MODE_FPR20:
    case KEYDB_SEARCH_MODE_FPR:
      /* This is a v4 fingerprint. */
      bin2hex (desc.u.fpr, 20, kidbuf);
      break;

    case KEYDB_SEARCH_MODE_FPR16:
      log_error ("HKP keyservers do not support v3 fingerprints\n");
    default:
      return gpg_error (GPG_ERR_INV_USER_ID);
    }

  /* Build the request string.  */
  hostport = make_host_part (uri->scheme, uri->host, uri->port);
  if (!hostport)
    {
      err = gpg_error_from_syserror ();
      goto leave;
    }

  request = strconcat (hostport,
                       "/pks/lookup?op=get&options=mr&search=0x",
                       kidbuf,
                       NULL);
  if (!request)
    {
      err = gpg_error_from_syserror ();
      goto leave;
    }

  /* Send the request.  */
  err = send_request (ctrl, request, hostport, NULL, NULL, &fp);
  if (err)
    goto leave;

  /* Return the read stream and close the HTTP context.  */
  *r_fp = fp;
  fp = NULL;

 leave:
  es_fclose (fp);
  xfree (request);
  xfree (hostport);
  return err;
}




/* Callback parameters for put_post_cb.  */
struct put_post_parm_s
{
  char *datastring;
};


/* Helper for ks_hkp_put.  */
static gpg_error_t
put_post_cb (void *opaque, http_t http)
{
  struct put_post_parm_s *parm = opaque;
  gpg_error_t err = 0;
  estream_t fp;
  size_t len;

  fp = http_get_write_ptr (http);
  len = strlen (parm->datastring);

  es_fprintf (fp,
              "Content-Type: application/x-www-form-urlencoded\r\n"
              "Content-Length: %zu\r\n", len+8 /* 8 is for "keytext" */);
  http_start_data (http);
  if (es_fputs ("keytext=", fp) || es_write (fp, parm->datastring, len, NULL))
    err = gpg_error_from_syserror ();
  return err;
}


/* Send the key in {DATA,DATALEN} to the keyserver identified by  URI.  */
gpg_error_t
ks_hkp_put (ctrl_t ctrl, parsed_uri_t uri, const void *data, size_t datalen)
{
  gpg_error_t err;
  char *hostport = NULL;
  char *request = NULL;
  estream_t fp = NULL;
  struct put_post_parm_s parm;
  char *armored = NULL;

  parm.datastring = NULL;

  err = armor_data (&armored, data, datalen);
  if (err)
    goto leave;

  parm.datastring = http_escape_string (armored, EXTRA_ESCAPE_CHARS);
  if (!parm.datastring)
    {
      err = gpg_error_from_syserror ();
      goto leave;
    }
  xfree (armored);
  armored = NULL;

  /* Build the request string.  */
  hostport = make_host_part (uri->scheme, uri->host, uri->port);
  if (!hostport)
    {
      err = gpg_error_from_syserror ();
      goto leave;
    }

  request = strconcat (hostport, "/pks/add", NULL);
  if (!request)
    {
      err = gpg_error_from_syserror ();
      goto leave;
    }

  /* Send the request.  */
  err = send_request (ctrl, request, hostport, put_post_cb, &parm, &fp);
  if (err)
    goto leave;

 leave:
  es_fclose (fp);
  xfree (parm.datastring);
  xfree (armored);
  xfree (request);
  xfree (hostport);
  return err;
}
