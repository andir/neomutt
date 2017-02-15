/*
 * Copyright (C) 2006-2007,2009 Brendan Cully <brendan@kublai.com>
 * Copyright (C) 2006,2009 Rocco Rutte <pdmef@gmx.net>
 *
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program; if not, write to the Free Software
 *     Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif				/* HAVE_CONFIG_H */

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>
#include <stdio.h>

#include "mutt.h"
#include "account.h"
#include "url.h"
#include "bcache.h"

#include "lib.h"

struct body_cache {
  char path[_POSIX_PATH_MAX];
  size_t pathlen;
};

static int bcache_path(ACCOUNT *account, const char *mailbox,
		       char *dst, size_t dstlen)
{
  char host[STRING];
  char path[_POSIX_PATH_MAX];
  ciss_url_t url;
  int len;

  if (!account || !MessageCachedir || !*MessageCachedir || !dst || !dstlen)
    return -1;

  /* make up a ciss_url_t we can turn into a string */
  memset (&url, 0, sizeof (ciss_url_t));
  mutt_account_tourl (account, &url);
  /*
   * mutt_account_tourl() just sets up some pointers;
   * if this ever changes, we have a memleak here
   */
  url.path = NULL;
  if (url_ciss_tostring (&url, host, sizeof (host), U_PATH) < 0)
  {
    return -1;
  }

  mutt_encode_path (path, sizeof (path), NONULL (mailbox));

  len = snprintf (dst, dstlen-1, "%s/%s%s%s", MessageCachedir,
		  host, path,
		  (*path && path[mutt_strlen (path) - 1] == '/') ? "" : "/");


  if (len < 0 || len >= dstlen-1)
    return -1;


  return 0;
}

body_cache_t *mutt_bcache_open (ACCOUNT *account, const char *mailbox)
{
  struct body_cache *bcache = NULL;

  if (!account)
    goto bail;

  bcache = safe_calloc (1, sizeof (struct body_cache));
  if (bcache_path (account, mailbox, bcache->path,
		   sizeof (bcache->path)) < 0)
    goto bail;
  bcache->pathlen = mutt_strlen (bcache->path);

  return bcache;

bail:
  if (bcache)
    FREE(&bcache);
  return NULL;
}

void mutt_bcache_close (body_cache_t **bcache)
{
  if (!bcache || !*bcache)
    return;
  FREE(bcache);			/* __FREE_CHECKED__ */
}

FILE* mutt_bcache_get(body_cache_t *bcache, const char *id)
{
  char path[_POSIX_PATH_MAX];
  FILE* fp = NULL;

  if (!id || !*id || !bcache)
    return NULL;

  path[0] = '\0';
  safe_strncat (path, sizeof (path), bcache->path, bcache->pathlen);
  safe_strncat (path, sizeof (path), id, mutt_strlen (id));

  fp = safe_fopen (path, "r");


  return fp;
}

FILE* mutt_bcache_put(body_cache_t *bcache, const char *id, int tmp)
{
  char path[_POSIX_PATH_MAX];
  struct stat sb;

  if (!id || !*id || !bcache)
    return NULL;

  if (stat(bcache->path, &sb) == 0)
  {
    if (!S_ISDIR(sb.st_mode))
    {
      mutt_error(_("Message cache isn't a directory: %s."), bcache->path);
      return NULL;
    }
  }
  else
  {
    if (mutt_mkdir(bcache->path, S_IRWXU | S_IRWXG | S_IRWXO) < 0)
    {
      mutt_error (_("Can't create %s %s"), bcache->path, strerror(errno));
      return NULL;
    }
  }

  snprintf(path, sizeof (path), "%s%s%s", bcache->path, id, tmp ? ".tmp" : "");

  return safe_fopen(path, "w+");
}

int mutt_bcache_commit(body_cache_t* bcache, const char* id)
{
  char tmpid[_POSIX_PATH_MAX];

  snprintf (tmpid, sizeof (tmpid), "%s.tmp", id);

  return mutt_bcache_move (bcache, tmpid, id);
}

int mutt_bcache_move(body_cache_t* bcache, const char* id, const char* newid)
{
  char path[_POSIX_PATH_MAX];
  char newpath[_POSIX_PATH_MAX];

  if (!bcache || !id || !*id || !newid || !*newid)
    return -1;

  snprintf (path, sizeof (path), "%s%s", bcache->path, id);
  snprintf (newpath, sizeof (newpath), "%s%s", bcache->path, newid);


  return rename (path, newpath);
}

int mutt_bcache_del(body_cache_t *bcache, const char *id)
{
  char path[_POSIX_PATH_MAX];

  if (!id || !*id || !bcache)
    return -1;

  path[0] = '\0';
  safe_strncat (path, sizeof (path), bcache->path, bcache->pathlen);
  safe_strncat (path, sizeof (path), id, mutt_strlen (id));


  return unlink (path);
}

int mutt_bcache_exists(body_cache_t *bcache, const char *id)
{
  char path[_POSIX_PATH_MAX];
  struct stat st;
  int rc = 0;

  if (!id || !*id || !bcache)
    return -1;

  path[0] = '\0';
  safe_strncat (path, sizeof (path), bcache->path, bcache->pathlen);
  safe_strncat (path, sizeof (path), id, mutt_strlen (id));

  if (stat (path, &st) < 0)
    rc = -1;
  else
    rc = S_ISREG(st.st_mode) && st.st_size != 0 ? 0 : -1;


  return rc;
}

int mutt_bcache_list(body_cache_t *bcache,
		     int (*want_id)(const char *id, body_cache_t *bcache,
				    void *data), void *data)
{
  DIR *d = NULL;
  struct dirent *de;
  int rc = -1;

  if (!bcache || !(d = opendir (bcache->path)))
    goto out;

  rc = 0;


  while ((de = readdir (d)))
  {
    if (mutt_strncmp (de->d_name, ".", 1) == 0 ||
	mutt_strncmp (de->d_name, "..", 2) == 0)
      continue;


    if (want_id && want_id (de->d_name, bcache, data) != 0)
      goto out;

    rc++;
  }

out:
  if (d)
  {
    if (closedir (d) < 0)
      rc = -1;
  }
  return rc;
}
