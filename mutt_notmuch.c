/**
 * @file
 * NotMuch virtual mailbox type
 *
 * @authors
 * Copyright (C) 2011-2016 Karel Zak <kzak@redhat.com>
 * Copyright (C) 2016-2017 Richard Russon <rich@flatcap.org>
 * Copyright (C) 2016 Kevin Velghe <kevin@paretje.be>
 * Copyright (C) 2017 Bernard 'Guyzmo' Pratz <guyzmo+github+pub@m0g.net>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ## Notes
 *
 * - notmuch uses private Context->data and private Header->data
 *
 * - all exported functions are usable within notmuch context only
 *
 * - all functions have to be covered by "ctx->magic == MUTT_NOTMUCH" check
 *   (it's implemented in get_ctxdata() and init_context() functions).
 *
 * - exception are nm_nonctx_* functions -- these functions use nm_default_uri
 *   (or parse URI from another resource)
 */

#include "config.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <notmuch.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "mutt.h"
#include "mutt_notmuch.h"
#include "body.h"
#include "buffy.h"
#include "context.h"
#include "envelope.h"
#include "globals.h"
#include "hash.h"
#include "header.h"
#include "lib.h"
#include "mailbox.h"
#include "mutt_curses.h"
#include "mx.h"
#include "protos.h"
#include "thread.h"
#include "url.h"

#ifdef LIBNOTMUCH_CHECK_VERSION
#undef LIBNOTMUCH_CHECK_VERSION
#endif

/* @def The definition in <notmuch.h> is broken */
#define LIBNOTMUCH_CHECK_VERSION(_major, _minor, _micro)                            \
  (LIBNOTMUCH_MAJOR_VERSION > (_major) ||                                           \
   (LIBNOTMUCH_MAJOR_VERSION == (_major) && LIBNOTMUCH_MINOR_VERSION > (_minor)) || \
   (LIBNOTMUCH_MAJOR_VERSION == (_major) && LIBNOTMUCH_MINOR_VERSION == (_minor) && \
    LIBNOTMUCH_MICRO_VERSION >= (_micro)))

/**
 * enum NmQueryType - NotMuch Query Types
 *
 * Read whole-thread or matching messages only?
 */
enum NmQueryType
{
  NM_QUERY_TYPE_MESGS = 1, /**< Default: Messages only */
  NM_QUERY_TYPE_THREADS    /**< Whole threads */
};

/**
 * struct UriTag - Parsed NotMuch-URI arguments
 *
 * The arguments in a URI are saved in a linked list.
 *
 * @sa NmCtxData#query_items
 */
struct UriTag
{
  char *name;
  char *value;
  struct UriTag *next;
};

/**
 * struct NmHdrTag - NotMuch Mail Header Tags
 *
 * Keep a linked list of header tags and their transformed values.
 * Textual tags can be transformed to symbols to save space.
 *
 * @sa NmHdrData#tag_list
 */
struct NmHdrTag
{
  char *tag;
  char *transformed;
  struct NmHdrTag *next;
};

/**
 * struct NmHdrData - NotMuch data attached to an email
 *
 * This stores all the NotMuch data associated with an email.
 *
 * @sa Header#data, MUTT_MBOX
 */
struct NmHdrData
{
  char *folder; /**< Location of the email */
  char *tags;
  char *tags_transformed;
  struct NmHdrTag *tag_list;
  char *oldpath;
  char *virtual_id; /**< Unique NotMuch Id */
  int magic;        /**< Type of mailbox the email is in */
};

/**
 * struct NmCtxData - NotMuch data attached to a context
 *
 * This stores the global NotMuch data, such as the database connection.
 *
 * @sa Context#data, NotmuchDBLimit, NM_QUERY_TYPE_MESGS
 */
struct NmCtxData
{
  notmuch_database_t *db;

  char *db_filename; /**< Filename of the NotMuch database */
  char *db_query;    /**< Previous query */
  int db_limit;      /**< Maximum number of results to return */
  int query_type;    /**< Messages or Threads */

  struct UriTag *query_items;

  struct Progress progress; /**< A progress bar */
  int oldmsgcount;
  int ignmsgcount; /**< Ignored messages */

  bool noprogress : 1;     /**< Don't show the progress bar */
  bool longrun : 1;        /**< A long-lived action is in progress */
  bool trans : 1;          /**< Atomic transaction in progress */
  bool progress_ready : 1; /**< A progress bar has been initialised */
};


#if 0
/**
 * debug_print_filenames - Show a message's filenames
 * @param msg NotMuch Message
 *
 * Print a list of all the filenames associated with a NotMuch message.
 */
static void debug_print_filenames(notmuch_message_t *msg)
{
  notmuch_filenames_t *ls = NULL;
  const char *id = notmuch_message_get_message_id(msg);

  for (ls = notmuch_message_get_filenames(msg);
       ls && notmuch_filenames_valid(ls);
       notmuch_filenames_move_to_next(ls))
  {
    mutt_debug (2, "nm: %s: %s\n", id, notmuch_filenames_get(ls));
  }
}

/**
 * debug_print_tags - Show a message's tags
 * @param msg NotMuch Message
 *
 * Print a list of all the tags associated with a NotMuch message.
 */
static void debug_print_tags(notmuch_message_t *msg)
{
  notmuch_tags_t *tags = NULL;
  const char *id = notmuch_message_get_message_id(msg);

  for (tags = notmuch_message_get_tags(msg);
       tags && notmuch_tags_valid(tags);
       notmuch_tags_move_to_next(tags))
  {
    mutt_debug (2, "nm: %s: %s\n", id, notmuch_tags_get(tags));
  }
}
#endif

/**
 * url_free_tags - Free a list of tags
 * @param tags List of tags
 *
 * Tags are stored as a singly-linked list.
 * Free all the strings and the list, itself.
 */
static void url_free_tags(struct UriTag *tags)
{
  while (tags)
  {
    struct UriTag *next = tags->next;
    FREE(&tags->name);
    FREE(&tags->value);
    FREE(&tags);
    tags = next;
  }
}

/**
 * url_parse_query - Extract the tokens from a query URI
 * @param[in]  url      URI to parse
 * @param[out] filename Save the filename
 * @param[out] tags     Save the list of tags
 * @return
 * * true  Success
 * * false Error: Bad format
 *
 * Parse a NotMuch URI, such as:
 * *    notmuch:///path/to/db?query=tag:lkml&limit=1000
 * *    notmuch://?query=neomutt
 *
 * Extract the database filename (optional) and any search parameters (tags).
 * The tags will be saved in a linked list (#UriTag).
 */
static bool url_parse_query(const char *url, char **filename, struct UriTag **tags)
{
  char *p = strstr(url, "://"); /* remote unsupported */
  char *e = NULL;
  struct UriTag *tag, *last = NULL;

  *filename = NULL;
  *tags = NULL;

  if (!p || !*(p + 3))
    return false;

  p += 3;
  *filename = p;

  e = strchr(p, '?');

  *filename = e ? (e == p) ? NULL : mutt_substrdup(p, e) : safe_strdup(p);
  if (!e)
    return true; /* only filename */

  if (*filename && (url_pct_decode(*filename) < 0))
    goto err;

  e++; /* skip '?' */
  p = e;

  while (p && *p)
  {
    tag = safe_calloc(1, sizeof(struct UriTag));

    if (!*tags)
      last = *tags = tag;
    else
    {
      last->next = tag;
      last = tag;
    }

    e = strchr(p, '=');
    if (!e)
      e = strchr(p, '&');
    tag->name = e ? mutt_substrdup(p, e) : safe_strdup(p);
    if (!tag->name || (url_pct_decode(tag->name) < 0))
      goto err;
    if (!e)
      break;

    p = e + 1;

    if (*e == '&')
      continue;

    e = strchr(p, '&');
    tag->value = e ? mutt_substrdup(p, e) : safe_strdup(p);
    if (!tag->value || (url_pct_decode(tag->value) < 0))
      goto err;
    if (!e)
      break;
    p = e + 1;
  }

  return true;
err:
  FREE(&(*filename));
  url_free_tags(*tags);
  return false;
}

/**
 * free_tag_list - Free a list of tags
 * @param tag_list List of tags
 *
 * Take a nm_hdrtag struct (a singly-linked list) and free the attached strings
 * and the list itself.
 */
static void free_tag_list(struct NmHdrTag **tag_list)
{
  struct NmHdrTag *tmp = NULL;

  while ((tmp = *tag_list) != NULL)
  {
    *tag_list = tmp->next;
    FREE(&tmp->tag);
    FREE(&tmp->transformed);
    FREE(&tmp);
  }

  *tag_list = 0;
}

/**
 * free_hdrdata - Free header data attached to an email
 * @param data Header data
 *
 * Each email can have an attached nm_hdrdata struct, which contains things
 * like the tags (labels).  This function frees all the resources and the
 * nm_hdrdata struct itself.
 */
static void free_hdrdata(struct NmHdrData *data)
{
  if (!data)
    return;

  mutt_debug(2, "nm: freeing header %p\n", (void *) data);
  FREE(&data->folder);
  FREE(&data->tags);
  FREE(&data->tags_transformed);
  free_tag_list(&data->tag_list);
  FREE(&data->oldpath);
  FREE(&data->virtual_id);
  FREE(&data);
}

/**
 * free_ctxdata - Free data attached to the context
 * @param data A mailbox CONTEXT
 *
 * The nm_ctxdata struct stores global NotMuch data, such as the connection to
 * the database.  This function will close the database, free the resources and
 * the struct itself.
 */
static void free_ctxdata(struct NmCtxData *data)
{
  if (!data)
    return;

  mutt_debug(1, "nm: freeing context data %p\n", (void *) data);

  if (data->db)
#ifdef NOTMUCH_API_3
    notmuch_database_destroy(data->db);
#else
    notmuch_database_close(data->db);
#endif
  data->db = NULL;

  FREE(&data->db_filename);
  FREE(&data->db_query);
  url_free_tags(data->query_items);
  FREE(&data);
}

/**
 * new_ctxdata - Create a new nm_ctxdata object from a query
 * @param uri NotMuch query string
 * @return a new nm_ctxdata struct
 *
 * A new nm_ctxdata struct is created, then the query is parsed and saved
 * within it.  This should be freed using free_ctxdata().
 */
static struct NmCtxData *new_ctxdata(char *uri)
{
  struct NmCtxData *data = NULL;

  if (!uri)
    return NULL;

  data = safe_calloc(1, sizeof(struct NmCtxData));
  mutt_debug(1, "nm: initialize context data %p\n", (void *) data);

  data->db_limit = NotmuchDBLimit;

  if (!url_parse_query(uri, &data->db_filename, &data->query_items))
  {
    mutt_error(_("failed to parse notmuch uri: %s"), uri);
    FREE(&data);
    return NULL;
  }

  return data;
}

/**
 * init_context - Add NotMuch data to the Context
 * @param ctx A mailbox CONTEXT
 * @return
 * *  0 Success
 * * -1 Error: Bad format
 *
 * Create a new nm_ctxdata struct and add it CONTEXT::data.
 * NotMuch-specific data will be stored in this struct.
 * This struct can be freed using free_hdrdata().
 */
static int init_context(struct Context *ctx)
{
  if (!ctx || (ctx->magic != MUTT_NOTMUCH))
    return -1;

  if (ctx->data)
    return 0;

  ctx->data = new_ctxdata(ctx->path);
  if (!ctx->data)
    return -1;

  return 0;
}

static char *header_get_id(struct Header *h)
{
  return (h && h->data) ? ((struct NmHdrData *) h->data)->virtual_id : NULL;
}

static char *header_get_fullpath(struct Header *h, char *buf, size_t bufsz)
{
  snprintf(buf, bufsz, "%s/%s", nm_header_get_folder(h), h->path);
  return buf;
}

static struct NmCtxData *get_ctxdata(struct Context *ctx)
{
  if (ctx && (ctx->magic == MUTT_NOTMUCH))
    return ctx->data;

  return NULL;
}

static int string_to_query_type(const char *str)
{
  if (!str)
    str = NotmuchQueryType; /* user's default */
  if (!str)
    return NM_QUERY_TYPE_MESGS; /* hardcoded default */

  if (strcmp(str, "threads") == 0)
    return NM_QUERY_TYPE_THREADS;
  else if (strcmp(str, "messages") == 0)
    return NM_QUERY_TYPE_MESGS;

  mutt_error(_("failed to parse notmuch query type: %s"), str);
  return NM_QUERY_TYPE_MESGS;
}

/**
 * query_window_check_timebase - Checks if a given timebase string is valid
 * @param[in] timebase: string containing a time base
 * @return true if the given time base is valid
 *
 * This function returns whether a given timebase string is valid or not,
 * which is used to validate the user settable configuration setting:
 *
 *     nm_query_window_timebase
 */
static bool query_window_check_timebase(const char *timebase)
{
  if ((strcmp(timebase, "hour") == 0) || (strcmp(timebase, "day") == 0) ||
      (strcmp(timebase, "week") == 0) || (strcmp(timebase, "month") == 0) ||
      (strcmp(timebase, "year") == 0))
    return true;
  return false;
}

/**
 * query_window_reset - Restore vfolder's search window to its original position
 *
 * After moving a vfolder search window backward and forward, calling this function
 * will reset the search position to its original value, setting to 0 the user settable
 * variable:
 *
 *     nm_query_window_current_position
 */
static void query_window_reset(void)
{
  mutt_debug(2, "query_window_reset ()\n");
  NotmuchQueryWindowCurrentPosition = 0;
}

/**
 * windowed_query_from_query - transforms a vfolder search query into a windowed one
 * @param[in]  query vfolder search string
 * @param[out] buf   allocated string buffer to receive the modified search query
 * @param[in]  bufsz allocated maximum size of the buf string buffer
 * @return boolean value set to true if a transformed search query is available as
 *         a string in buf, otherwise if the search query shall not be transformed.
 *
 * This is where the magic of windowed queries happens. Taking a vfolder search
 * query string as parameter, it will use the following two user settings:
 *
 * - `nm_query_window_duration` and
 * - `nm_query_window_timebase`
 *
 * to amend given vfolder search window. Then using a third parameter:
 *
 * - `nm_query_window_current_position`
 *
 * it will generate a proper notmuch `date:` parameter. For example, given a
 * duration of `2`, a timebase set to `week` and a position defaulting to `0`,
 * it will prepend to the 'tag:inbox' notmuch search query the following string:
 *
 * - `query`: `tag:inbox`
 * - `buf`:   `date:2week..now and tag:inbox`
 *
 * If the position is set to `4`, with `duration=3` and `timebase=month`:
 *
 * - `query`: `tag:archived`
 * - `buf`:   `date:12month..9month and tag:archived`
 *
 * The window won't be applied:
 *
 * - If the duration of the search query is set to `0` this function will be disabled.
 * - If the timebase is invalid, it will show an error message and do nothing.
 *
 * If there's no search registered in `nm_query_window_current_search` or this is
 * a new search, it will reset the window and do the search.
 */
static bool windowed_query_from_query(const char *query, char *buf, size_t bufsz)
{
  mutt_debug(2, "nm: windowed_query_from_query (%s)\n", query);

  int beg = NotmuchQueryWindowDuration * (NotmuchQueryWindowCurrentPosition + 1);
  int end = NotmuchQueryWindowDuration * NotmuchQueryWindowCurrentPosition;

  /* if the duration is a non positive integer, disable the window */
  if (NotmuchQueryWindowDuration <= 0)
  {
    query_window_reset();
    return false;
  }

  /* if the query has changed, reset the window position */
  if (NotmuchQueryWindowCurrentSearch == NULL ||
      (strcmp(query, NotmuchQueryWindowCurrentSearch) != 0))
    query_window_reset();

  if (!query_window_check_timebase(NotmuchQueryWindowTimebase))
  {
    mutt_message(_("Invalid nm_query_window_timebase value (valid values are: "
                   "hour, day, week, month or year)."));
    mutt_debug(2, "Invalid nm_query_window_timebase value\n");
    return false;
  }

  if (end == 0)
    snprintf(buf, bufsz, "date:%d%s..now and %s", beg,
             NotmuchQueryWindowTimebase, NotmuchQueryWindowCurrentSearch);
  else
    snprintf(buf, bufsz, "date:%d%s..%d%s and %s", beg, NotmuchQueryWindowTimebase,
             end, NotmuchQueryWindowTimebase, NotmuchQueryWindowCurrentSearch);

  mutt_debug(2, "nm: windowed_query_from_query (%s) -> %s\n", query, buf);

  return true;
}

/**
 * get_query_string - builds the notmuch vfolder search string
 * @param data   internal notmuch context
 * @param window if true enable application of the window on the search string
 * @return string containing a notmuch search query, or a NULL pointer
 *         if none can be generated.
 *
 * This function parses the internal representation of a search, and returns
 * a search query string ready to be fed to the notmuch API, given the search
 * is valid.
 *
 * As a note, the window parameter here is here to decide contextually whether
 * we want to return a search query with window applied (for the actual search
 * result in buffy) or not (for the count in the sidebar). It is not aimed at
 * enabling/disabling the feature.
 */
static char *get_query_string(struct NmCtxData *data, int window)
{
  mutt_debug(2, "nm: get_query_string(%d)\n", window);

  struct UriTag *item = NULL;

  if (!data)
    return NULL;
  if (data->db_query)
    return data->db_query;

  for (item = data->query_items; item; item = item->next)
  {
    if (!item->value || !item->name)
      continue;

    if (strcmp(item->name, "limit") == 0)
    {
      if (mutt_atoi(item->value, &data->db_limit))
        mutt_error(_("failed to parse notmuch limit: %s"), item->value);
    }
    else if (strcmp(item->name, "type") == 0)
      data->query_type = string_to_query_type(item->value);

    else if (strcmp(item->name, "query") == 0)
      data->db_query = safe_strdup(item->value);
  }

  if (!data->db_query)
    return NULL;

  if (!data->query_type)
    data->query_type = string_to_query_type(NULL);

  if (window)
  {
    char buf[LONG_STRING];
    mutt_str_replace(&NotmuchQueryWindowCurrentSearch, data->db_query);

    /* if a date part is defined, do not apply windows (to avoid the risk of
     * having a non-intersected date frame). A good improvement would be to
     * accept if they intersect
     */
    if (!strstr(data->db_query, "date:") &&
        windowed_query_from_query(data->db_query, buf, sizeof(buf)))
      data->db_query = safe_strdup(buf);

    mutt_debug(2, "nm: query (windowed) '%s'\n", data->db_query);
  }
  else
    mutt_debug(2, "nm: query '%s'\n", data->db_query);

  return data->db_query;
}

static int get_limit(struct NmCtxData *data)
{
  return data ? data->db_limit : 0;
}

static int get_query_type(struct NmCtxData *data)
{
  return (data && data->query_type) ? data->query_type : string_to_query_type(NULL);
}

static const char *get_db_filename(struct NmCtxData *data)
{
  char *db_filename = NULL;

  if (!data)
    return NULL;

  db_filename = data->db_filename ? data->db_filename : NotmuchDefaultUri;
  if (!db_filename)
    db_filename = Maildir;
  if (!db_filename)
    return NULL;
  if (strncmp(db_filename, "notmuch://", 10) == 0)
    db_filename += 10;

  mutt_debug(2, "nm: db filename '%s'\n", db_filename);
  return db_filename;
}

static notmuch_database_t *do_database_open(const char *filename, int writable, int verbose)
{
  notmuch_database_t *db = NULL;
  int ct = 0;
  notmuch_status_t st = NOTMUCH_STATUS_SUCCESS;

  mutt_debug(1, "nm: db open '%s' %s (timeout %d)\n", filename,
             writable ? "[WRITE]" : "[READ]", NotmuchOpenTimeout);
  do
  {
#ifdef NOTMUCH_API_3
    st = notmuch_database_open(filename, writable ? NOTMUCH_DATABASE_MODE_READ_WRITE : NOTMUCH_DATABASE_MODE_READ_ONLY,
                               &db);
#else
    db = notmuch_database_open(filename,
                               writable ? NOTMUCH_DATABASE_MODE_READ_WRITE :
                                          NOTMUCH_DATABASE_MODE_READ_ONLY);
#endif
    if (db || !NotmuchOpenTimeout || ((ct / 2) > NotmuchOpenTimeout))
      break;

    if (verbose && ct && ((ct % 2) == 0))
      mutt_error(_("Waiting for notmuch DB... (%d sec)"), ct / 2);
    usleep(500000);
    ct++;
  } while (true);

  if (verbose)
  {
    if (!db)
      mutt_error(_("Cannot open notmuch database: %s: %s"), filename,
                 st ? notmuch_status_to_string(st) : _("unknown reason"));
    else if (ct > 1)
      mutt_clear_error();
  }
  return db;
}

static notmuch_database_t *get_db(struct NmCtxData *data, int writable)
{
  if (!data)
    return NULL;
  if (!data->db)
  {
    const char *db_filename = get_db_filename(data);

    if (db_filename)
      data->db = do_database_open(db_filename, writable, true);
  }
  return data->db;
}

static int release_db(struct NmCtxData *data)
{
  if (data && data->db)
  {
    mutt_debug(1, "nm: db close\n");
#ifdef NOTMUCH_API_3
    notmuch_database_destroy(data->db);
#else
    notmuch_database_close(data->db);
#endif
    data->db = NULL;
    data->longrun = false;
    return 0;
  }

  return -1;
}

/**
 * db_trans_begin - Start a NotMuch database transaction
 * @param data Header data
 * @return
 * * < 0 = error
 * * 1 = new transaction started
 * * 0 = already within transaction
 */
static int db_trans_begin(struct NmCtxData *data)
{
  if (!data || !data->db)
    return -1;

  if (!data->trans)
  {
    mutt_debug(2, "nm: db trans start\n");
    if (notmuch_database_begin_atomic(data->db))
      return -1;
    data->trans = true;
    return 1;
  }

  return 0;
}

static int db_trans_end(struct NmCtxData *data)
{
  if (!data || !data->db)
    return -1;

  if (data->trans)
  {
    mutt_debug(2, "nm: db trans end\n");
    data->trans = false;
    if (notmuch_database_end_atomic(data->db))
      return -1;
  }

  return 0;
}

/**
 * is_longrun - Is NotMuch in the middle of a long-running transaction
 * @param data Header data
 * @return true if it is
 */
static int is_longrun(struct NmCtxData *data)
{
  return data && data->longrun;
}

/**
 * get_database_mtime - Get the database modification time
 * @param[in]  data  Struct holding database info
 * @param[out] mtime Save the modification time
 * @return
 * *  0 Success (result in mtime)
 * * -1 Error
 *
 * Get the "mtime" (modification time) of the database file.
 * This is the time of the last update.
 */
static int get_database_mtime(struct NmCtxData *data, time_t *mtime)
{
  char path[_POSIX_PATH_MAX];
  struct stat st;

  if (!data)
    return -1;

  snprintf(path, sizeof(path), "%s/.notmuch/xapian", get_db_filename(data));
  mutt_debug(2, "nm: checking '%s' mtime\n", path);

  if (stat(path, &st))
    return -1;

  if (mtime)
    *mtime = st.st_mtime;

  return 0;
}

static void apply_exclude_tags(notmuch_query_t *query)
{
  char *buf = NULL, *p = NULL, *end = NULL, *tag = NULL;

  if (!NotmuchExcludeTags || !*NotmuchExcludeTags)
    return;
  buf = safe_strdup(NotmuchExcludeTags);

  for (p = buf; p && *p; p++)
  {
    if (!tag && isspace(*p))
      continue;
    if (!tag)
      tag = p; /* begin of the tag */
    if ((*p == ',') || (*p == ' '))
      end = p; /* terminate the tag */
    else if (*(p + 1) == '\0')
      end = p + 1; /* end of optstr */
    if (!tag || !end)
      continue;
    if (tag >= end)
      break;
    *end = '\0';

    mutt_debug(2, "nm: query exclude tag '%s'\n", tag);
    notmuch_query_add_tag_exclude(query, tag);
    end = tag = NULL;
  }
  notmuch_query_set_omit_excluded(query, 1);
  FREE(&buf);
}

static notmuch_query_t *get_query(struct NmCtxData *data, int writable)
{
  notmuch_database_t *db = NULL;
  notmuch_query_t *q = NULL;
  const char *str = NULL;

  if (!data)
    return NULL;

  db = get_db(data, writable);
  str = get_query_string(data, true);

  if (!db || !str)
    goto err;

  q = notmuch_query_create(db, str);
  if (!q)
    goto err;

  apply_exclude_tags(q);
  notmuch_query_set_sort(q, NOTMUCH_SORT_NEWEST_FIRST);
  mutt_debug(2, "nm: query successfully initialized (%s)\n", str);
  return q;
err:
  if (!is_longrun(data))
    release_db(data);
  return NULL;
}

static void append_str_item(char **str, const char *item, int sep)
{
  char *p = NULL;
  size_t sz = strlen(item);
  size_t ssz = *str ? strlen(*str) : 0;

  safe_realloc(str, ssz + ((ssz && sep) ? 1 : 0) + sz + 1);
  p = *str + ssz;
  if (sep && ssz)
    *p++ = sep;
  memcpy(p, item, sz + 1);
}

static int update_header_tags(struct Header *h, notmuch_message_t *msg)
{
  struct NmHdrData *data = h->data;
  notmuch_tags_t *tags = NULL;
  char *tstr = NULL, *ttstr = NULL;
  struct NmHdrTag *tag_list = NULL, *tmp = NULL;

  mutt_debug(2, "nm: tags update requested (%s)\n", data->virtual_id);

  for (tags = notmuch_message_get_tags(msg); tags && notmuch_tags_valid(tags);
       notmuch_tags_move_to_next(tags))
  {
    const char *t = notmuch_tags_get(tags);
    const char *tt = NULL;

    if (!t || !*t)
      continue;

    tt = hash_find(TagTransforms, t);
    if (!tt)
      tt = t;

    /* tags list contains all tags */
    tmp = safe_calloc(1, sizeof(*tmp));
    tmp->tag = safe_strdup(t);
    tmp->transformed = safe_strdup(tt);
    tmp->next = tag_list;
    tag_list = tmp;

    /* filter out hidden tags */
    if (NotmuchHiddenTags)
    {
      char *p = strstr(NotmuchHiddenTags, t);
      size_t xsz = p ? strlen(t) : 0;

      if (p && ((p == NotmuchHiddenTags) || (*(p - 1) == ',') || (*(p - 1) == ' ')) &&
          ((*(p + xsz) == '\0') || (*(p + xsz) == ',') || (*(p + xsz) == ' ')))
        continue;
    }

    /* expand the transformed tag string */
    append_str_item(&ttstr, tt, ' ');

    /* expand the un-transformed tag string */
    append_str_item(&tstr, t, ' ');
  }

  free_tag_list(&data->tag_list);
  data->tag_list = tag_list;

  if (data->tags && tstr && (strcmp(data->tags, tstr) == 0))
  {
    FREE(&tstr);
    FREE(&ttstr);
    mutt_debug(2, "nm: tags unchanged\n");
    return 1;
  }

  /* free old version */
  FREE(&data->tags);
  FREE(&data->tags_transformed);

  /* new version */
  data->tags = tstr;
  mutt_debug(2, "nm: new tags: '%s'\n", tstr);

  data->tags_transformed = ttstr;
  mutt_debug(2, "nm: new tag transforms: '%s'\n", ttstr);

  return 0;
}

static int update_message_path(struct Header *h, const char *path)
{
  struct NmHdrData *data = h->data;
  char *p = NULL;

  mutt_debug(2, "nm: path update requested path=%s, (%s)\n", path, data->virtual_id);

  p = strrchr(path, '/');
  if (p && ((p - path) > 3) &&
      ((strncmp(p - 3, "cur", 3) == 0) || (strncmp(p - 3, "new", 3) == 0) ||
       (strncmp(p - 3, "tmp", 3) == 0)))
  {
    data->magic = MUTT_MAILDIR;

    FREE(&h->path);
    FREE(&data->folder);

    p -= 3; /* skip subfolder (e.g. "new") */
    h->path = safe_strdup(p);

    for (; (p > path) && (*(p - 1) == '/'); p--)
      ;

    data->folder = mutt_substrdup(path, p);

    mutt_debug(2, "nm: folder='%s', file='%s'\n", data->folder, h->path);
    return 0;
  }

  return 1;
}

static char *get_folder_from_path(const char *path)
{
  char *p = strrchr(path, '/');

  if (p && ((p - path) > 3) &&
      ((strncmp(p - 3, "cur", 3) == 0) || (strncmp(p - 3, "new", 3) == 0) ||
       (strncmp(p - 3, "tmp", 3) == 0)))
  {
    p -= 3;
    for (; (p > path) && (*(p - 1) == '/'); p--)
      ;

    return mutt_substrdup(path, p);
  }

  return NULL;
}

static void deinit_header(struct Header *h)
{
  if (h)
  {
    free_hdrdata(h->data);
    h->data = NULL;
  }
}

/**
 * nm2mutt_message_id - converts notmuch message Id to mutt message Id
 * @param id NotMuch ID to convert
 * @return Mutt message ID
 *
 * Caller must free the Mutt Message ID
 */
static char *nm2mutt_message_id(const char *id)
{
  size_t sz;
  char *mid = NULL;

  if (!id)
    return NULL;
  sz = strlen(id) + 3;
  mid = safe_malloc(sz);

  snprintf(mid, sz, "<%s>", id);
  return mid;
}

static int init_header(struct Header *h, const char *path, notmuch_message_t *msg)
{
  const char *id = NULL;

  if (h->data)
    return 0;

  id = notmuch_message_get_message_id(msg);

  h->data = safe_calloc(1, sizeof(struct NmHdrData));
  h->free_cb = deinit_header;

  /*
   * Notmuch ensures that message Id exists (if not notmuch Notmuch will
   * generate an ID), so it's more safe than use mutt Header->env->id
   */
  ((struct NmHdrData *) h->data)->virtual_id = safe_strdup(id);

  mutt_debug(2, "nm: initialize header data: [hdr=%p, data=%p] (%s)\n",
             (void *) h, (void *) h->data, id);

  if (!h->env->message_id)
    h->env->message_id = nm2mutt_message_id(id);

  if (update_message_path(h, path))
    return -1;

  update_header_tags(h, msg);

  return 0;
}

static const char *get_message_last_filename(notmuch_message_t *msg)
{
  notmuch_filenames_t *ls = NULL;
  const char *name = NULL;

  for (ls = notmuch_message_get_filenames(msg);
       ls && notmuch_filenames_valid(ls); notmuch_filenames_move_to_next(ls))
  {
    name = notmuch_filenames_get(ls);
  }

  return name;
}

static void progress_reset(struct Context *ctx)
{
  struct NmCtxData *data = NULL;

  if (ctx->quiet)
    return;

  data = get_ctxdata(ctx);
  if (!data)
    return;

  memset(&data->progress, 0, sizeof(data->progress));
  data->oldmsgcount = ctx->msgcount;
  data->ignmsgcount = 0;
  data->noprogress = false;
  data->progress_ready = false;
}

static void progress_update(struct Context *ctx, notmuch_query_t *q)
{
  struct NmCtxData *data = get_ctxdata(ctx);

  if (ctx->quiet || !data || data->noprogress)
    return;

  if (!data->progress_ready && q)
  {
    unsigned count;
    static char msg[STRING];
    snprintf(msg, sizeof(msg), _("Reading messages..."));

#if LIBNOTMUCH_CHECK_VERSION(4, 3, 0)
    if (notmuch_query_count_messages_st(q, &count) != NOTMUCH_STATUS_SUCCESS)
      count = 0; /* may not be defined on error */
#else
    count = notmuch_query_count_messages(q);
#endif
    mutt_progress_init(&data->progress, msg, MUTT_PROGRESS_MSG, ReadInc, count);
    data->progress_ready = true;
  }

  if (data->progress_ready)
    mutt_progress_update(&data->progress,
                         ctx->msgcount + data->ignmsgcount - data->oldmsgcount, -1);
}

static struct Header *get_mutt_header(struct Context *ctx, notmuch_message_t *msg)
{
  char *mid = NULL;
  const char *id = NULL;
  struct Header *h = NULL;

  if (!ctx || !msg)
    return NULL;

  id = notmuch_message_get_message_id(msg);
  if (!id)
    return NULL;

  mutt_debug(2, "nm: mutt header, id='%s'\n", id);

  if (!ctx->id_hash)
  {
    mutt_debug(2, "nm: init hash\n");
    ctx->id_hash = mutt_make_id_hash(ctx);
    if (!ctx->id_hash)
      return NULL;
  }

  mid = nm2mutt_message_id(id);
  mutt_debug(2, "nm: mutt id='%s'\n", mid);

  h = hash_find(ctx->id_hash, mid);
  FREE(&mid);
  return h;
}

static void append_message(struct Context *ctx, notmuch_query_t *q,
                           notmuch_message_t *msg, int dedup)
{
  char *newpath = NULL;
  const char *path = NULL;
  struct Header *h = NULL;

  struct NmCtxData *data = get_ctxdata(ctx);
  if (!data)
    return;

  /* deduplicate */
  if (dedup && get_mutt_header(ctx, msg))
  {
    data->ignmsgcount++;
    progress_update(ctx, q);
    mutt_debug(2, "nm: ignore id=%s, already in the context\n",
               notmuch_message_get_message_id(msg));
    return;
  }

  path = get_message_last_filename(msg);
  if (!path)
    return;

  mutt_debug(2, "nm: appending message, i=%d, id=%s, path=%s\n", ctx->msgcount,
             notmuch_message_get_message_id(msg), path);

  if (ctx->msgcount >= ctx->hdrmax)
  {
    mutt_debug(2, "nm: allocate mx memory\n");
    mx_alloc_memory(ctx);
  }
  if (access(path, F_OK) == 0)
    h = maildir_parse_message(MUTT_MAILDIR, path, 0, NULL);
  else
  {
    /* maybe moved try find it... */
    char *folder = get_folder_from_path(path);

    if (folder)
    {
      FILE *f = maildir_open_find_message(folder, path, &newpath);
      if (f)
      {
        h = maildir_parse_stream(MUTT_MAILDIR, f, newpath, 0, NULL);
        fclose(f);

        mutt_debug(1, "nm: not up-to-date: %s -> %s\n", path, newpath);
      }
    }
    FREE(&folder);
  }

  if (!h)
  {
    mutt_debug(1, "nm: failed to parse message: %s\n", path);
    goto done;
  }
  if (init_header(h, newpath ? newpath : path, msg) != 0)
  {
    mutt_free_header(&h);
    mutt_debug(1, "nm: failed to append header!\n");
    goto done;
  }

  h->active = true;
  h->index = ctx->msgcount;
  ctx->size += h->content->length + h->content->offset - h->content->hdr_offset;
  ctx->hdrs[ctx->msgcount] = h;
  ctx->msgcount++;

  if (newpath)
  {
    /* remember that file has been moved -- nm_sync_mailbox() will update the DB */
    struct NmHdrData *hd = (struct NmHdrData *) h->data;

    if (hd)
    {
      mutt_debug(1, "nm: remember obsolete path: %s\n", path);
      hd->oldpath = safe_strdup(path);
    }
  }
  progress_update(ctx, q);
done:
  FREE(&newpath);
}

/**
 * append_replies - add all the replies to a given messages into the display
 *
 * Careful, this calls itself recursively to make sure we get everything.
 */
static void append_replies(struct Context *ctx, notmuch_query_t *q,
                           notmuch_message_t *top, int dedup)
{
  notmuch_messages_t *msgs = NULL;

  for (msgs = notmuch_message_get_replies(top); notmuch_messages_valid(msgs);
       notmuch_messages_move_to_next(msgs))
  {
    notmuch_message_t *m = notmuch_messages_get(msgs);
    append_message(ctx, q, m, dedup);
    /* recurse through all the replies to this message too */
    append_replies(ctx, q, m, dedup);
    notmuch_message_destroy(m);
  }
}

/**
 * append_thread - add each top level reply in the thread
 *
 * add each top level reply in the thread, and then add each reply to the top
 * level replies
 */
static void append_thread(struct Context *ctx, notmuch_query_t *q,
                          notmuch_thread_t *thread, int dedup)
{
  notmuch_messages_t *msgs = NULL;

  for (msgs = notmuch_thread_get_toplevel_messages(thread);
       notmuch_messages_valid(msgs); notmuch_messages_move_to_next(msgs))
  {
    notmuch_message_t *m = notmuch_messages_get(msgs);
    append_message(ctx, q, m, dedup);
    append_replies(ctx, q, m, dedup);
    notmuch_message_destroy(m);
  }
}

static bool read_mesgs_query(struct Context *ctx, notmuch_query_t *q, int dedup)
{
  struct NmCtxData *data = get_ctxdata(ctx);
  int limit;
  notmuch_messages_t *msgs = NULL;

  if (!data)
    return false;

  limit = get_limit(data);

#if LIBNOTMUCH_CHECK_VERSION(4, 3, 0)
  if (notmuch_query_search_messages_st(q, &msgs) != NOTMUCH_STATUS_SUCCESS)
    return false;
#else
  msgs = notmuch_query_search_messages(q);
#endif

  for (; notmuch_messages_valid(msgs) && ((limit == 0) || (ctx->msgcount < limit));
       notmuch_messages_move_to_next(msgs))
  {
    if (SigInt == 1)
    {
      SigInt = 0;
      return false;
    }
    notmuch_message_t *m = notmuch_messages_get(msgs);
    append_message(ctx, q, m, dedup);
    notmuch_message_destroy(m);
  }
  return true;
}

static bool read_threads_query(struct Context *ctx, notmuch_query_t *q, int dedup, int limit)
{
  struct NmCtxData *data = get_ctxdata(ctx);
  notmuch_threads_t *threads = NULL;

  if (!data)
    return false;

#if LIBNOTMUCH_CHECK_VERSION(4, 3, 0)
  if (notmuch_query_search_threads_st(q, &threads) != NOTMUCH_STATUS_SUCCESS)
    return false;
#else
  threads = notmuch_query_search_threads(q);
#endif

  for (; notmuch_threads_valid(threads) && ((limit == 0) || (ctx->msgcount < limit));
       notmuch_threads_move_to_next(threads))
  {
    if (SigInt == 1)
    {
      SigInt = 0;
      return false;
    }
    notmuch_thread_t *thread = notmuch_threads_get(threads);
    append_thread(ctx, q, thread, dedup);
    notmuch_thread_destroy(thread);
  }
  return true;
}

static notmuch_message_t *get_nm_message(notmuch_database_t *db, struct Header *hdr)
{
  notmuch_message_t *msg = NULL;
  char *id = header_get_id(hdr);

  mutt_debug(2, "nm: find message (%s)\n", id);

  if (id && db)
    notmuch_database_find_message(db, id, &msg);

  return msg;
}

static bool nm_message_has_tag(notmuch_message_t *msg, char *tag)
{
  const char *possible_match_tag = NULL;
  notmuch_tags_t *tags = NULL;

  for (tags = notmuch_message_get_tags(msg); notmuch_tags_valid(tags);
       notmuch_tags_move_to_next(tags))
  {
    possible_match_tag = notmuch_tags_get(tags);
    if (mutt_strcmp(possible_match_tag, tag) == 0)
    {
      return true;
    }
  }
  return false;
}

static int update_tags(notmuch_message_t *msg, const char *tags)
{
  char *tag = NULL, *end = NULL, *p = NULL;
  char *buf = safe_strdup(tags);

  if (!buf)
    return -1;

  notmuch_message_freeze(msg);

  for (p = buf; p && *p; p++)
  {
    if (!tag && isspace(*p))
      continue;
    if (!tag)
      tag = p; /* begin of the tag */
    if ((*p == ',') || (*p == ' '))
      end = p; /* terminate the tag */
    else if (*(p + 1) == '\0')
      end = p + 1; /* end of optstr */
    if (!tag || !end)
      continue;
    if (tag >= end)
      break;

    *end = '\0';

    if (*tag == '-')
    {
      mutt_debug(1, "nm: remove tag: '%s'\n", tag + 1);
      notmuch_message_remove_tag(msg, tag + 1);
    }
    else if (*tag == '!')
    {
      mutt_debug(1, "nm: toggle tag: '%s'\n", tag + 1);
      if (nm_message_has_tag(msg, tag + 1))
      {
        notmuch_message_remove_tag(msg, tag + 1);
      }
      else
      {
        notmuch_message_add_tag(msg, tag + 1);
      }
    }
    else
    {
      mutt_debug(1, "nm: add tag: '%s'\n", (*tag == '+') ? tag + 1 : tag);
      notmuch_message_add_tag(msg, (*tag == '+') ? tag + 1 : tag);
    }
    end = tag = NULL;
  }

  notmuch_message_thaw(msg);
  FREE(&buf);
  return 0;
}

/**
 * update_header_flags - Update the header flags
 *
 * TODO: extract parsing of string to separate function, join
 * update_header_tags and update_header_flags, which are given an array of
 * tags.
 */
static int update_header_flags(struct Context *ctx, struct Header *hdr, const char *tags)
{
  char *tag = NULL, *end = NULL, *p = NULL;
  char *buf = safe_strdup(tags);

  if (!buf)
    return -1;

  for (p = buf; p && *p; p++)
  {
    if (!tag && isspace(*p))
      continue;
    if (!tag)
      tag = p; /* begin of the tag */
    if ((*p == ',') || (*p == ' '))
      end = p; /* terminate the tag */
    else if (*(p + 1) == '\0')
      end = p + 1; /* end of optstr */
    if (!tag || !end)
      continue;
    if (tag >= end)
      break;

    *end = '\0';

    if (*tag == '-')
    {
      tag = tag + 1;
      if (strcmp(tag, "unread") == 0)
        mutt_set_flag(ctx, hdr, MUTT_READ, 1);
      else if (strcmp(tag, "replied") == 0)
        mutt_set_flag(ctx, hdr, MUTT_REPLIED, 0);
      else if (strcmp(tag, "flagged") == 0)
        mutt_set_flag(ctx, hdr, MUTT_FLAG, 0);
    }
    else
    {
      tag = (*tag == '+') ? tag + 1 : tag;
      if (strcmp(tag, "unread") == 0)
        mutt_set_flag(ctx, hdr, MUTT_READ, 0);
      else if (strcmp(tag, "replied") == 0)
        mutt_set_flag(ctx, hdr, MUTT_REPLIED, 1);
      else if (strcmp(tag, "flagged") == 0)
        mutt_set_flag(ctx, hdr, MUTT_FLAG, 1);
    }
    end = tag = NULL;
  }

  FREE(&buf);
  return 0;
}

static int rename_maildir_filename(const char *old, char *newpath, size_t newsz,
                                   struct Header *h)
{
  char filename[_POSIX_PATH_MAX];
  char suffix[_POSIX_PATH_MAX];
  char folder[_POSIX_PATH_MAX];
  char *p = NULL;

  strfcpy(folder, old, sizeof(folder));
  p = strrchr(folder, '/');
  if (p)
  {
    *p = '\0';
    p++;
  }
  else
    p = folder;

  strfcpy(filename, p, sizeof(filename));

  /* remove (new,cur,...) from folder path */
  p = strrchr(folder, '/');
  if (p)
    *p = '\0';

  /* remove old flags from filename */
  p = strchr(filename, ':');
  if (p)
    *p = '\0';

  /* compose new flags */
  maildir_flags(suffix, sizeof(suffix), h);

  snprintf(newpath, newsz, "%s/%s/%s%s", folder,
           (h->read || h->old) ? "cur" : "new", filename, suffix);

  if (strcmp(old, newpath) == 0)
    return 1;

  if (rename(old, newpath) != 0)
  {
    mutt_debug(1, "nm: rename(2) failed %s -> %s\n", old, newpath);
    return -1;
  }

  return 0;
}

static int remove_filename(struct NmCtxData *data, const char *path)
{
  notmuch_status_t st;
  notmuch_filenames_t *ls = NULL;
  notmuch_message_t *msg = NULL;
  notmuch_database_t *db = get_db(data, true);
  int trans;

  mutt_debug(2, "nm: remove filename '%s'\n", path);

  if (!db)
    return -1;
  st = notmuch_database_find_message_by_filename(db, path, &msg);
  if (st || !msg)
    return -1;
  trans = db_trans_begin(data);
  if (trans < 0)
    return -1;

  /*
   * note that unlink() is probably unnecessary here, it's already removed
   * by mh_sync_mailbox_message(), but for sure...
   */
  st = notmuch_database_remove_message(db, path);
  switch (st)
  {
    case NOTMUCH_STATUS_SUCCESS:
      mutt_debug(2, "nm: remove success, call unlink\n");
      unlink(path);
      break;
    case NOTMUCH_STATUS_DUPLICATE_MESSAGE_ID:
      mutt_debug(2, "nm: remove success (duplicate), call unlink\n");
      unlink(path);
      for (ls = notmuch_message_get_filenames(msg);
           ls && notmuch_filenames_valid(ls); notmuch_filenames_move_to_next(ls))
      {
        path = notmuch_filenames_get(ls);

        mutt_debug(2, "nm: remove duplicate: '%s'\n", path);
        unlink(path);
        notmuch_database_remove_message(db, path);
      }
      break;
    default:
      mutt_debug(1, "nm: failed to remove '%s' [st=%d]\n", path, (int) st);
      break;
  }

  notmuch_message_destroy(msg);
  if (trans)
    db_trans_end(data);
  return 0;
}

static int rename_filename(struct NmCtxData *data, const char *old,
                           const char *new, struct Header *h)
{
  int rc = -1;
  notmuch_status_t st;
  notmuch_filenames_t *ls = NULL;
  notmuch_message_t *msg = NULL;
  notmuch_database_t *db = get_db(data, true);
  int trans;

  if (!db || !new || !old || (access(new, F_OK) != 0))
    return -1;

  mutt_debug(1, "nm: rename filename, %s -> %s\n", old, new);
  trans = db_trans_begin(data);
  if (trans < 0)
    return -1;

  mutt_debug(2, "nm: rename: add '%s'\n", new);
  st = notmuch_database_add_message(db, new, &msg);

  if ((st != NOTMUCH_STATUS_SUCCESS) && (st != NOTMUCH_STATUS_DUPLICATE_MESSAGE_ID))
  {
    mutt_debug(1, "nm: failed to add '%s' [st=%d]\n", new, (int) st);
    goto done;
  }

  mutt_debug(2, "nm: rename: rem '%s'\n", old);
  st = notmuch_database_remove_message(db, old);
  switch (st)
  {
    case NOTMUCH_STATUS_SUCCESS:
      break;
    case NOTMUCH_STATUS_DUPLICATE_MESSAGE_ID:
      mutt_debug(2, "nm: rename: syncing duplicate filename\n");
      notmuch_message_destroy(msg);
      msg = NULL;
      notmuch_database_find_message_by_filename(db, new, &msg);

      for (ls = notmuch_message_get_filenames(msg);
           msg && ls && notmuch_filenames_valid(ls); notmuch_filenames_move_to_next(ls))
      {
        const char *path = notmuch_filenames_get(ls);
        char newpath[_POSIX_PATH_MAX];

        if (strcmp(new, path) == 0)
          continue;

        mutt_debug(2, "nm: rename: syncing duplicate: %s\n", path);

        if (rename_maildir_filename(path, newpath, sizeof(newpath), h) == 0)
        {
          mutt_debug(2, "nm: rename dup %s -> %s\n", path, newpath);
          notmuch_database_remove_message(db, path);
          notmuch_database_add_message(db, newpath, NULL);
        }
      }
      notmuch_message_destroy(msg);
      msg = NULL;
      notmuch_database_find_message_by_filename(db, new, &msg);
      st = NOTMUCH_STATUS_SUCCESS;
      break;
    default:
      mutt_debug(1, "nm: failed to remove '%s' [st=%d]\n", old, (int) st);
      break;
  }

  if ((st == NOTMUCH_STATUS_SUCCESS) && h && msg)
  {
    notmuch_message_maildir_flags_to_tags(msg);
    update_header_tags(h, msg);
    update_tags(msg, nm_header_get_tags(h));
  }

  rc = 0;
done:
  if (msg)
    notmuch_message_destroy(msg);
  if (trans)
    db_trans_end(data);
  return rc;
}

static unsigned count_query(notmuch_database_t *db, const char *qstr)
{
  unsigned res = 0;
  notmuch_query_t *q = notmuch_query_create(db, qstr);

  if (q)
  {
    apply_exclude_tags(q);
#if LIBNOTMUCH_CHECK_VERSION(4, 3, 0)
    if (notmuch_query_count_messages_st(q, &res) != NOTMUCH_STATUS_SUCCESS)
      res = 0; /* may not be defined on error */
#else
    res = notmuch_query_count_messages(q);
#endif
    notmuch_query_destroy(q);
    mutt_debug(1, "nm: count '%s', result=%d\n", qstr, res);
  }
  return res;
}


char *nm_header_get_folder(struct Header *h)
{
  return (h && h->data) ? ((struct NmHdrData *) h->data)->folder : NULL;
}

char *nm_header_get_tags(struct Header *h)
{
  return (h && h->data) ? ((struct NmHdrData *) h->data)->tags : NULL;
}

char *nm_header_get_tags_transformed(struct Header *h)
{
  return (h && h->data) ? ((struct NmHdrData *) h->data)->tags_transformed : NULL;
}

char *nm_header_get_tag_transformed(char *tag, struct Header *h)
{
  struct NmHdrTag *tmp = NULL;

  if (!h || !h->data)
    return NULL;

  for (tmp = ((struct NmHdrData *) h->data)->tag_list; tmp != NULL; tmp = tmp->next)
  {
    if (strcmp(tag, tmp->tag) == 0)
      return tmp->transformed;
  }

  return NULL;
}

void nm_longrun_init(struct Context *ctx, int writable)
{
  struct NmCtxData *data = get_ctxdata(ctx);

  if (data && get_db(data, writable))
  {
    data->longrun = true;
    mutt_debug(2, "nm: long run initialized\n");
  }
}

void nm_longrun_done(struct Context *ctx)
{
  struct NmCtxData *data = get_ctxdata(ctx);

  if (data && (release_db(data) == 0))
    mutt_debug(2, "nm: long run deinitialized\n");
}

void nm_debug_check(struct Context *ctx)
{
  struct NmCtxData *data = get_ctxdata(ctx);
  if (!data)
    return;

  if (data->db)
  {
    mutt_debug(1, "nm: ERROR: db is open, closing\n");
    release_db(data);
  }
}

int nm_read_entire_thread(struct Context *ctx, struct Header *h)
{
  struct NmCtxData *data = get_ctxdata(ctx);
  const char *id = NULL;
  char *qstr = NULL;
  notmuch_query_t *q = NULL;
  notmuch_database_t *db = NULL;
  notmuch_message_t *msg = NULL;
  int rc = -1;

  if (!data)
    return -1;
  if (!(db = get_db(data, false)) || !(msg = get_nm_message(db, h)))
    goto done;

  mutt_debug(1, "nm: reading entire-thread messages...[current count=%d]\n", ctx->msgcount);

  progress_reset(ctx);
  id = notmuch_message_get_thread_id(msg);
  if (!id)
    goto done;
  append_str_item(&qstr, "thread:", 0);
  append_str_item(&qstr, id, 0);

  q = notmuch_query_create(db, qstr);
  FREE(&qstr);
  if (!q)
    goto done;
  apply_exclude_tags(q);
  notmuch_query_set_sort(q, NOTMUCH_SORT_NEWEST_FIRST);

  read_threads_query(ctx, q, 1, 0);
  ctx->mtime = time(NULL);
  rc = 0;

  if (ctx->msgcount > data->oldmsgcount)
    mx_update_context(ctx, ctx->msgcount - data->oldmsgcount);
done:
  if (q)
    notmuch_query_destroy(q);
  if (!is_longrun(data))
    release_db(data);

  if (ctx->msgcount == data->oldmsgcount)
    mutt_message(_("No more messages in the thread."));

  data->oldmsgcount = 0;
  mutt_debug(1,
             "nm: reading entire-thread messages... done [rc=%d, count=%d]\n",
             rc, ctx->msgcount);
  return rc;
}

char *nm_uri_from_query(struct Context *ctx, char *buf, size_t bufsz)
{
  mutt_debug(2, "nm_uri_from_query (%s)\n", buf);
  struct NmCtxData *data = get_ctxdata(ctx);
  char uri[_POSIX_PATH_MAX + LONG_STRING + 32]; /* path to DB + query + URI "decoration" */

  if (data)
    snprintf(uri, sizeof(uri), "notmuch://%s?query=%s", get_db_filename(data), buf);
  else if (NotmuchDefaultUri)
    snprintf(uri, sizeof(uri), "%s?query=%s", NotmuchDefaultUri, buf);
  else if (Maildir)
    snprintf(uri, sizeof(uri), "notmuch://%s?query=%s", Maildir, buf);
  else
    return NULL;

  strncpy(buf, uri, bufsz);
  buf[bufsz - 1] = '\0';

  mutt_debug(1, "nm: uri from query '%s'\n", buf);
  return buf;
}

/**
 * nm_normalize_uri - takes a notmuch URI, parses it and reformat it in a canonical way
 * @param new_uri    allocated string receiving the reformatted URI
 * @param orig_uri   original URI to be parsed
 * @param new_uri_sz size of the allocated new_uri string
 * @return
 * * true if new_uri contains a normalized version of the query
 * * false if orig_uri contains an invalid query
 *
 * This function aims at making notmuch searches URI representations deterministic,
 * so that when comparing two equivalent searches they will be the same. It works
 * by building a notmuch context object from the original search string, and
 * building a new from the notmuch context object.
 *
 * It's aimed to be used by buffy when parsing the virtual_mailboxes to make the
 * parsed user written search strings comparable to the internally generated ones.
 */
bool nm_normalize_uri(char *new_uri, const char *orig_uri, size_t new_uri_sz)
{
  mutt_debug(2, "nm_normalize_uri (%s)\n", orig_uri);
  char buf[LONG_STRING];

  struct Context tmp_ctx;
  struct NmCtxData tmp_ctxdata;

  tmp_ctx.magic = MUTT_NOTMUCH;
  tmp_ctx.data = &tmp_ctxdata;
  tmp_ctxdata.db_query = NULL;

  if (!url_parse_query(orig_uri, &tmp_ctxdata.db_filename, &tmp_ctxdata.query_items))
  {
    mutt_error(_("failed to parse notmuch uri: %s"), orig_uri);
    mutt_debug(2, "nm_normalize_uri () -> error #1\n");
    return false;
  }

  mutt_debug(2, "nm_normalize_uri #1 () -> db_query: %s\n", tmp_ctxdata.db_query);

  if (get_query_string(&tmp_ctxdata, false) == NULL)
  {
    mutt_error(_("failed to parse notmuch uri: %s"), orig_uri);
    mutt_debug(2, "nm_normalize_uri () -> error #2\n");
    return false;
  }

  mutt_debug(2, "nm_normalize_uri #2 () -> db_query: %s\n", tmp_ctxdata.db_query);

  strfcpy(buf, tmp_ctxdata.db_query, sizeof(buf));

  if (nm_uri_from_query(&tmp_ctx, buf, sizeof(buf)) == NULL)
  {
    mutt_error(_("failed to parse notmuch uri: %s"), orig_uri);
    mutt_debug(2, "nm_normalize_uri () -> error #3\n");
    return true;
  }

  strncpy(new_uri, buf, new_uri_sz);

  mutt_debug(2, "nm_normalize_uri #3 (%s) -> %s\n", orig_uri, new_uri);
  return true;
}

/**
 * nm_query_window_forward - Function to move the current search window forward in time
 *
 * Updates `nm_query_window_current_position` by decrementing it by 1, or does nothing
 * if the current window already is set to 0.
 *
 * The lower the value of `nm_query_window_current_position` is, the more recent the
 * result will be.
 */
void nm_query_window_forward(void)
{
  if (NotmuchQueryWindowCurrentPosition != 0)
    NotmuchQueryWindowCurrentPosition--;

  mutt_debug(2, "nm_query_window_forward (%d)\n", NotmuchQueryWindowCurrentPosition);
}

/**
 * nm_query_window_backward - Function to move the current search window backward in time
 *
 * Updates `nm_query_window_current_position` by incrementing it by 1
 *
 * The higher the value of `nm_query_window_current_position` is, the less recent the
 * result will be.
 */
void nm_query_window_backward(void)
{
  NotmuchQueryWindowCurrentPosition++;
  mutt_debug(2, "nm_query_window_backward (%d)\n", NotmuchQueryWindowCurrentPosition);
}

int nm_modify_message_tags(struct Context *ctx, struct Header *hdr, char *buf)
{
  struct NmCtxData *data = get_ctxdata(ctx);
  notmuch_database_t *db = NULL;
  notmuch_message_t *msg = NULL;
  int rc = -1;

  if (!buf || !*buf || !data)
    return -1;

  if (!(db = get_db(data, true)) || !(msg = get_nm_message(db, hdr)))
    goto done;

  mutt_debug(1, "nm: tags modify: '%s'\n", buf);

  update_tags(msg, buf);
  update_header_flags(ctx, hdr, buf);
  update_header_tags(hdr, msg);
  mutt_set_header_color(ctx, hdr);

  rc = 0;
  hdr->changed = true;
done:
  if (!is_longrun(data))
    release_db(data);
  if (hdr->changed)
    ctx->mtime = time(NULL);
  mutt_debug(1, "nm: tags modify done [rc=%d]\n", rc);
  return rc;
}

bool nm_message_is_still_queried(struct Context *ctx, struct Header *hdr)
{
  char *orig_str = NULL;
  char *new_str = NULL;
  struct NmCtxData *data = get_ctxdata(ctx);
  notmuch_database_t *db = NULL;
  notmuch_query_t *q = NULL;
  bool result = false;

  db = get_db(data, false);
  orig_str = get_query_string(data, true);

  if (!db || !orig_str)
    return false;

  if (safe_asprintf(&new_str, "id:%s and (%s)", header_get_id(hdr), orig_str) < 0)
    return false;

  mutt_debug(2, "nm: checking if message is still queried: %s\n", new_str);

  q = notmuch_query_create(db, new_str);

  switch (get_query_type(data))
  {
    case NM_QUERY_TYPE_MESGS:
    {
      notmuch_messages_t *messages = NULL;
#if LIBNOTMUCH_CHECK_VERSION(4, 3, 0)
      if (notmuch_query_search_messages_st(q, &messages) != NOTMUCH_STATUS_SUCCESS)
        return false;
#else
      messages = notmuch_query_search_messages(q);
#endif
      result = notmuch_messages_valid(messages);
      notmuch_messages_destroy(messages);
      break;
    }
    case NM_QUERY_TYPE_THREADS:
    {
      notmuch_threads_t *threads = NULL;
#if LIBNOTMUCH_CHECK_VERSION(4, 3, 0)
      if (notmuch_query_search_threads_st(q, &threads) != NOTMUCH_STATUS_SUCCESS)
        return false;
#else
      threads = notmuch_query_search_threads(q);
#endif
      result = notmuch_threads_valid(threads);
      notmuch_threads_destroy(threads);
      break;
    }
  }

  notmuch_query_destroy(q);

  mutt_debug(2, "nm: checking if message is still queried: %s = %s\n", new_str,
             result ? "true" : "false");

  return result;
}

int nm_update_filename(struct Context *ctx, const char *old, const char *new,
                       struct Header *h)
{
  char buf[PATH_MAX];
  int rc;
  struct NmCtxData *data = get_ctxdata(ctx);

  if (!data || !new)
    return -1;

  if (!old && h && h->data)
  {
    header_get_fullpath(h, buf, sizeof(buf));
    old = buf;
  }

  rc = rename_filename(data, old, new, h);

  if (!is_longrun(data))
    release_db(data);
  ctx->mtime = time(NULL);
  return rc;
}

int nm_nonctx_get_count(char *path, int *all, int *new)
{
  struct UriTag *query_items = NULL, *item = NULL;
  char *db_filename = NULL, *db_query = NULL;
  notmuch_database_t *db = NULL;
  int rc = -1;
  bool dflt = false;

  mutt_debug(1, "nm: count\n");

  if (!url_parse_query(path, &db_filename, &query_items))
  {
    mutt_error(_("failed to parse notmuch uri: %s"), path);
    goto done;
  }
  if (!query_items)
    goto done;

  for (item = query_items; item; item = item->next)
  {
    if (item->value && (strcmp(item->name, "query") == 0))
    {
      db_query = item->value;
      break;
    }
  }

  if (!db_query)
    goto done;

  if (!db_filename)
  {
    if (NotmuchDefaultUri)
    {
      if (strncmp(NotmuchDefaultUri, "notmuch://", 10) == 0)
        db_filename = NotmuchDefaultUri + 10;
      else
        db_filename = NotmuchDefaultUri;
    }
    else if (Maildir)
      db_filename = Maildir;
    dflt = true;
  }

  /* don't be verbose about connection, as we're called from
   * sidebar/buffy very often */
  db = do_database_open(db_filename, false, false);
  if (!db)
    goto done;

  /* all emails */
  if (all)
    *all = count_query(db, db_query);

  /* new messages */
  if (new)
  {
    char *qstr = NULL;

    safe_asprintf(&qstr, "( %s ) tag:%s", db_query, NotmuchUnreadTag);
    *new = count_query(db, qstr);
    FREE(&qstr);
  }

  rc = 0;
done:
  if (db)
  {
#ifdef NOTMUCH_API_3
    notmuch_database_destroy(db);
#else
    notmuch_database_close(db);
#endif
    mutt_debug(1, "nm: count close DB\n");
  }
  if (!dflt)
    FREE(&db_filename);
  url_free_tags(query_items);

  mutt_debug(1, "nm: count done [rc=%d]\n", rc);
  return rc;
}

char *nm_get_description(struct Context *ctx)
{
  for (struct Buffy *b = Incoming; b; b = b->next)
    if (b->desc && (strcmp(b->path, ctx->path) == 0))
      return b->desc;

  return NULL;
}

int nm_description_to_path(const char *desc, char *buf, size_t bufsz)
{
  if (!desc || !buf || !bufsz)
    return -EINVAL;

  for (struct Buffy *b = Incoming; b; b = b->next)
  {
    if ((b->magic != MUTT_NOTMUCH) && b->desc && (strcmp(desc, b->desc) == 0))
    {
      strncpy(buf, b->path, bufsz);
      buf[bufsz - 1] = '\0';
      return 0;
    }
  }

  return -1;
}

int nm_record_message(struct Context *ctx, char *path, struct Header *h)
{
  notmuch_database_t *db = NULL;
  notmuch_status_t st;
  notmuch_message_t *msg = NULL;
  int rc = -1, trans;
  struct NmCtxData *data = get_ctxdata(ctx);

  if (!path || !data || (access(path, F_OK) != 0))
    return 0;
  db = get_db(data, true);
  if (!db)
    return -1;

  mutt_debug(1, "nm: record message: %s\n", path);
  trans = db_trans_begin(data);
  if (trans < 0)
    goto done;

  st = notmuch_database_add_message(db, path, &msg);

  if ((st != NOTMUCH_STATUS_SUCCESS) && (st != NOTMUCH_STATUS_DUPLICATE_MESSAGE_ID))
  {
    mutt_debug(1, "nm: failed to add '%s' [st=%d]\n", path, (int) st);
    goto done;
  }

  if (st == NOTMUCH_STATUS_SUCCESS && msg)
  {
    notmuch_message_maildir_flags_to_tags(msg);
    if (h)
      update_tags(msg, nm_header_get_tags(h));
    if (NotmuchRecordTags)
      update_tags(msg, NotmuchRecordTags);
  }

  rc = 0;
done:
  if (msg)
    notmuch_message_destroy(msg);
  if (trans == 1)
    db_trans_end(data);
  if (!is_longrun(data))
    release_db(data);
  return rc;
}

/**
 * nm_get_all_tags - Fill a list with all notmuch tags
 *
 * If tag_list is NULL, just count the tags.
 */
int nm_get_all_tags(struct Context *ctx, char **tag_list, int *tag_count)
{
  struct NmCtxData *data = get_ctxdata(ctx);
  notmuch_database_t *db = NULL;
  notmuch_tags_t *tags = NULL;
  const char *tag = NULL;
  int rc = -1;

  if (!data)
    return -1;

  if (!(db = get_db(data, false)) || !(tags = notmuch_database_get_all_tags(db)))
    goto done;

  *tag_count = 0;
  mutt_debug(1, "nm: get all tags\n");

  while (notmuch_tags_valid(tags))
  {
    tag = notmuch_tags_get(tags);
    /* Skip empty string */
    if (*tag)
    {
      if (tag_list)
        tag_list[*tag_count] = safe_strdup(tag);
      (*tag_count)++;
    }
    notmuch_tags_move_to_next(tags);
  }

  rc = 0;
done:
  if (tags)
    notmuch_tags_destroy(tags);

  if (!is_longrun(data))
    release_db(data);

  mutt_debug(1, "nm: get all tags done [rc=%d tag_count=%u]\n", rc, *tag_count);
  return rc;
}


/**
 * nm_open_mailbox - Open a notmuch virtual mailbox
 * @param ctx A mailbox CONTEXT
 * @return
 * *  0 Success
 * * -1 Error
 */

static int nm_open_mailbox(struct Context *ctx)
{
  notmuch_query_t *q = NULL;
  struct NmCtxData *data = NULL;
  int rc = -1;

  if (init_context(ctx) != 0)
    return -1;

  data = get_ctxdata(ctx);
  if (!data)
    return -1;

  mutt_debug(1, "nm: reading messages...[current count=%d]\n", ctx->msgcount);

  progress_reset(ctx);

  q = get_query(data, false);
  if (q)
  {
    rc = 0;
    switch (get_query_type(data))
    {
      case NM_QUERY_TYPE_MESGS:
        if (!read_mesgs_query(ctx, q, 0))
          rc = -2;
        break;
      case NM_QUERY_TYPE_THREADS:
        if (!read_threads_query(ctx, q, 0, get_limit(data)))
          rc = -2;
        break;
    }
    notmuch_query_destroy(q);
  }

  if (!is_longrun(data))
    release_db(data);

  ctx->mtime = time(NULL);

  mx_update_context(ctx, ctx->msgcount);
  data->oldmsgcount = 0;

  mutt_debug(1, "nm: reading messages... done [rc=%d, count=%d]\n", rc, ctx->msgcount);
  return rc;
}

/**
 * nm_close_mailbox - Close a notmuch virtual mailbox
 * @param ctx A mailbox CONTEXT
 * @return
 * *  0 Success
 * * -1 Error
 */
static int nm_close_mailbox(struct Context *ctx)
{
  if (!ctx || (ctx->magic != MUTT_NOTMUCH))
    return -1;

  for (int i = 0; i < ctx->msgcount; i++)
  {
    struct Header *h = ctx->hdrs[i];

    if (h)
    {
      free_hdrdata(h->data);
      h->data = NULL;
    }
  }

  free_ctxdata(ctx->data);
  ctx->data = NULL;
  return 0;
}

/**
 * nm_check_mailbox - Check a notmuch mailbox for new mail
 * @param ctx         A mailbox CONTEXT
 * @param index_hint  Remeber our place in the index
 * @return
 * * -1 Error
 * *  0 QWQ
 * * #MUTT_NEW_MAIL - new mail has arrived
 * * #MUTT_REOPENED - mailbox closed and reopened
 * * #MUTT_FLAGS - QWQ
 */
static int nm_check_mailbox(struct Context *ctx, int *index_hint)
{
  struct NmCtxData *data = get_ctxdata(ctx);
  time_t mtime = 0;
  notmuch_query_t *q = NULL;
  notmuch_messages_t *msgs = NULL;
  int i, limit, new_flags = 0;
  bool occult = false;

  if (!data || (get_database_mtime(data, &mtime) != 0))
    return -1;

  if (ctx->mtime >= mtime)
  {
    mutt_debug(2, "nm: check unnecessary (db=%lu ctx=%lu)\n", mtime, ctx->mtime);
    return 0;
  }

  mutt_debug(1, "nm: checking (db=%lu ctx=%lu)\n", mtime, ctx->mtime);

  q = get_query(data, false);
  if (!q)
    goto done;

  mutt_debug(1, "nm: start checking (count=%d)\n", ctx->msgcount);
  data->oldmsgcount = ctx->msgcount;
  data->noprogress = true;

  for (i = 0; i < ctx->msgcount; i++)
    ctx->hdrs[i]->active = false;

  limit = get_limit(data);

#if LIBNOTMUCH_CHECK_VERSION(4, 3, 0)
  if (notmuch_query_search_messages_st(q, &msgs) != NOTMUCH_STATUS_SUCCESS)
    goto done;
#else
  msgs = notmuch_query_search_messages(q);
#endif

  for (i = 0; notmuch_messages_valid(msgs) && ((limit == 0) || (i < limit));
       notmuch_messages_move_to_next(msgs), i++)
  {
    char old[_POSIX_PATH_MAX];
    const char *new = NULL;

    notmuch_message_t *m = notmuch_messages_get(msgs);
    struct Header *h = get_mutt_header(ctx, m);

    if (!h)
    {
      /* new email */
      append_message(ctx, NULL, m, 0);
      notmuch_message_destroy(m);
      continue;
    }

    /* message already exists, merge flags */
    h->active = true;

    /* Check to see if the message has moved to a different subdirectory.
     * If so, update the associated filename.
     */
    new = get_message_last_filename(m);
    header_get_fullpath(h, old, sizeof(old));

    if (mutt_strcmp(old, new) != 0)
      update_message_path(h, new);

    if (!h->changed)
    {
      /* if the user hasn't modified the flags on
       * this message, update the flags we just
       * detected.
       */
      struct Header tmp;
      memset(&tmp, 0, sizeof(tmp));
      maildir_parse_flags(&tmp, new);
      maildir_update_flags(ctx, h, &tmp);
    }

    if (update_header_tags(h, m) == 0)
      new_flags++;

    notmuch_message_destroy(m);
  }

  for (i = 0; i < ctx->msgcount; i++)
  {
    if (!ctx->hdrs[i]->active)
    {
      occult = true;
      break;
    }
  }

  if (ctx->msgcount > data->oldmsgcount)
    mx_update_context(ctx, ctx->msgcount - data->oldmsgcount);
done:
  if (q)
    notmuch_query_destroy(q);

  if (!is_longrun(data))
    release_db(data);

  ctx->mtime = time(NULL);

  mutt_debug(1, "nm: ... check done [count=%d, new_flags=%d, occult=%d]\n",
             ctx->msgcount, new_flags, occult);

  return occult ? MUTT_REOPENED :
                  (ctx->msgcount > data->oldmsgcount) ? MUTT_NEW_MAIL :
                                                        new_flags ? MUTT_FLAGS : 0;
}

/**
 * nm_sync_mailbox - Sync a notmuch mailbox
 * @param ctx        A mailbox CONTEXT
 * @param index_hint Remember our place in the index
 */
static int nm_sync_mailbox(struct Context *ctx, int *index_hint)
{
  struct NmCtxData *data = get_ctxdata(ctx);
  int rc = 0;
  char msgbuf[STRING];
  struct Progress progress;
  char *uri = ctx->path;
  bool changed = false;

  if (!data)
    return -1;

  mutt_debug(1, "nm: sync start ...\n");

  if (!ctx->quiet)
  {
    /* all is in this function so we don't use data->progress here */
    snprintf(msgbuf, sizeof(msgbuf), _("Writing %s..."), ctx->path);
    mutt_progress_init(&progress, msgbuf, MUTT_PROGRESS_MSG, WriteInc, ctx->msgcount);
  }

  for (int i = 0; i < ctx->msgcount; i++)
  {
    char old[_POSIX_PATH_MAX], new[_POSIX_PATH_MAX];
    struct Header *h = ctx->hdrs[i];
    struct NmHdrData *hd = h->data;

    if (!ctx->quiet)
      mutt_progress_update(&progress, i, -1);

    *old = *new = '\0';

    if (hd->oldpath)
    {
      strncpy(old, hd->oldpath, sizeof(old));
      old[sizeof(old) - 1] = '\0';
      mutt_debug(2, "nm: fixing obsolete path '%s'\n", old);
    }
    else
      header_get_fullpath(h, old, sizeof(old));

    ctx->path = hd->folder;
    ctx->magic = hd->magic;
#ifdef USE_HCACHE
    rc = mh_sync_mailbox_message(ctx, i, NULL);
#else
    rc = mh_sync_mailbox_message(ctx, i);
#endif
    ctx->path = uri;
    ctx->magic = MUTT_NOTMUCH;

    if (rc)
      break;

    if (!h->deleted)
      header_get_fullpath(h, new, sizeof(new));

    if (h->deleted || (strcmp(old, new) != 0))
    {
      if (h->deleted && (remove_filename(data, old) == 0))
        changed = true;
      else if (*new &&*old && (rename_filename(data, old, new, h) == 0))
        changed = true;
    }

    FREE(&hd->oldpath);
  }

  ctx->path = uri;
  ctx->magic = MUTT_NOTMUCH;

  if (!is_longrun(data))
    release_db(data);
  if (changed)
    ctx->mtime = time(NULL);

  mutt_debug(1, "nm: .... sync done [rc=%d]\n", rc);
  return rc;
}

/**
 * nm_open_message - Open a message from a notmuch mailbox
 * @param ctx   A mailbox CONTEXT
 * @param msg   Message to open
 * @param msgno Index of message to open
 * @return
 * * 0 Success
 * * 1 Error
 */
static int nm_open_message(struct Context *ctx, struct Message *msg, int msgno)
{
  if (!ctx || !msg)
    return 1;
  struct Header *cur = ctx->hdrs[msgno];
  char *folder = ctx->path;
  char path[_POSIX_PATH_MAX];
  folder = nm_header_get_folder(cur);

  snprintf(path, sizeof(path), "%s/%s", folder, cur->path);

  msg->fp = fopen(path, "r");
  if (!msg->fp && (errno == ENOENT) &&
      ((ctx->magic == MUTT_MAILDIR) || (ctx->magic == MUTT_NOTMUCH)))
    msg->fp = maildir_open_find_message(folder, cur->path, NULL);

  mutt_debug(1, "%s\n", __func__);
  return !msg->fp;
}

/**
 * nm_close_message - Close a message
 * @param ctx A mailbox CONTEXT
 * @param msg Message to close
 * @return
 * * 0 Success
 * * 1 Error
 */
static int nm_close_message(struct Context *ctx, struct Message *msg)
{
  if (!msg)
    return 1;
  safe_fclose(&(msg->fp));
  return 0;
}

static int nm_commit_message(struct Context *ctx, struct Message *msg)
{
  mutt_error(_("Can't write to virtual folder."));
  return -1;
}


/**
 * struct mx_notmuch_ops - Mailbox API
 *
 * These functions are common to all mailbox types.
 */
struct MxOps mx_notmuch_ops = {
  .open = nm_open_mailbox, /* calls init_context() */
  .open_append = NULL,
  .close = nm_close_mailbox,
  .check = nm_check_mailbox,
  .sync = nm_sync_mailbox,
  .open_msg = nm_open_message,
  .close_msg = nm_close_message,
  .commit_msg = nm_commit_message,
  .open_new_msg = NULL,
};
