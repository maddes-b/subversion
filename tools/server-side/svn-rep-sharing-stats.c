/*
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include <apr_signal.h>

#include "svn_cmdline.h"
#include "svn_dirent_uri.h"
#include "svn_pools.h"
#include "svn_repos.h"
#include "svn_opt.h"
#include "svn_utf.h"
#include "svn_version.h"

#include "../../subversion/libsvn_fs_fs/fs.h"
#include "../../subversion/libsvn_fs_fs/fs_fs.h"
/* for svn_fs_fs__id_* (used in assertions only) */
#include "../../subversion/libsvn_fs_fs/id.h"
#include "../../subversion/libsvn_fs_fs/cached_data.h"

#include "private/svn_cmdline_private.h"

#include "svn_private_config.h"


/** Help messages and version checking. **/

static svn_error_t *
version(apr_pool_t *pool)
{
  return svn_opt_print_help4(NULL, "svn-rep-sharing-stats", TRUE, FALSE, FALSE,
                             NULL, NULL, NULL, NULL, NULL, NULL, pool);
}

static void
usage(apr_pool_t *pool)
{
  svn_error_clear(svn_cmdline_fprintf
                  (stderr, pool,
                   _("Type 'svn-rep-sharing-stats --help' for usage.\n")));
}


static void
help(const apr_getopt_option_t *options, apr_pool_t *pool)
{
  svn_error_clear
    (svn_cmdline_fprintf
     (stdout, pool,
      _("usage: svn-rep-sharing-stats [OPTIONS] REPOS_PATH\n\n"
        "  Prints the reference count statistics for representations\n"
        "  in an FSFS repository.\n"
        "\n"
        "  At least one of the options --data/--prop/--both must be specified.\n"
        "\n"
        "Valid options:\n")));
  while (options->description)
    {
      const char *optstr;
      svn_opt_format_option(&optstr, options, TRUE, pool);
      svn_error_clear(svn_cmdline_fprintf(stdout, pool, "  %s\n", optstr));
      ++options;
    }
  svn_error_clear(svn_cmdline_fprintf(stdout, pool, "\n"));
}


/* Version compatibility check */
static svn_error_t *
check_lib_versions(void)
{
  static const svn_version_checklist_t checklist[] =
    {
      /* ### check FSFS version */
      { "svn_subr",   svn_subr_version },
      { "svn_fs",     svn_fs_version },
      { NULL, NULL }
    };
  SVN_VERSION_DEFINE(my_version);

  return svn_error_trace(svn_ver_check_list2(&my_version, checklist,
                                             svn_ver_equal));
}



/** Cancellation stuff, ### copied from subversion/svn/main.c */

/* A flag to see if we've been cancelled by the client or not. */
static volatile sig_atomic_t cancelled = FALSE;

/* A signal handler to support cancellation. */
static void
signal_handler(int signum)
{
  apr_signal(signum, SIG_IGN);
  cancelled = TRUE;
}

/* Our cancellation callback. */
static svn_error_t *
svn_cl__check_cancel(void *baton)
{
  if (cancelled)
    return svn_error_create(SVN_ERR_CANCELLED, NULL, _("Caught signal"));
  else
    return SVN_NO_ERROR;
}

static svn_cancel_func_t cancel_func = svn_cl__check_cancel;

static void set_up_cancellation(void)
{
  /* Set up our cancellation support. */
  apr_signal(SIGINT, signal_handler);
#ifdef SIGBREAK
  /* SIGBREAK is a Win32 specific signal generated by ctrl-break. */
  apr_signal(SIGBREAK, signal_handler);
#endif
#ifdef SIGHUP
  apr_signal(SIGHUP, signal_handler);
#endif
#ifdef SIGTERM
  apr_signal(SIGTERM, signal_handler);
#endif

#ifdef SIGPIPE
  /* Disable SIGPIPE generation for the platforms that have it. */
  apr_signal(SIGPIPE, SIG_IGN);
#endif

#ifdef SIGXFSZ
  /* Disable SIGXFSZ generation for the platforms that have it, otherwise
   * working with large files when compiled against an APR that doesn't have
   * large file support will crash the program, which is uncool. */
  apr_signal(SIGXFSZ, SIG_IGN);
#endif
}


/** Program-specific code. **/
enum {
  OPT_VERSION = SVN_OPT_FIRST_LONGOPT_ID,
  OPT_DATA,
  OPT_PROP,
  OPT_BOTH
};

static svn_error_t *check_experimental(void)
{
  if (getenv("SVN_REP_SHARING_STATS_IS_EXPERIMENTAL"))
      return SVN_NO_ERROR;

  return svn_error_create(APR_EGENERAL, NULL,
                          "This code is experimental and should not "
                          "be used on live data.");
}

/* The parts of a rep that determine whether it's being shared. */
struct key_t
{
  svn_revnum_t revision;
  apr_off_t offset;
};

/* What we need to know about a rep. */
struct value_t
{
  svn_checksum_t checksum;
  unsigned char sha1_digest[APR_SHA1_DIGESTSIZE];
  apr_uint64_t refcount;
};

/* Increment records[rep] if both are non-NULL and REP contains a sha1.
 * Allocate keys and values in RESULT_POOL.
 */
static svn_error_t *record(apr_hash_t *records,
                           representation_t *rep,
                           apr_pool_t *result_pool)
{
  struct key_t *key;
  struct value_t *value;

  /* Skip if we ignore this particular kind of reps, or if the rep doesn't
   * exist or doesn't have the checksum we are after.  (The latter case
   * often corresponds to node_rev->kind == svn_node_dir.)
   */
  if (records == NULL || rep == NULL || !rep->has_sha1)
    return SVN_NO_ERROR;

  /* Construct the key.
   *
   * Must use calloc() because apr_hash_* pay attention to padding bytes too.
   */
  key = apr_pcalloc(result_pool, sizeof(*key));
  key->revision = rep->revision;
  key->offset = rep->item_index;

  /* Update or create the value. */
  if ((value = apr_hash_get(records, key, sizeof(*key))))
    {
      /* Paranoia. */
      SVN_ERR_ASSERT(memcmp(value->sha1_digest,
                            rep->sha1_digest,
                            sizeof(value->sha1_digest)));
      /* Real work. */
      value->refcount++;
    }
  else
    {
      value = apr_palloc(result_pool, sizeof(*value));
      value->checksum.digest = value->sha1_digest;
      value->checksum.kind = svn_checksum_sha1;
      value->refcount = 1;
      memcpy(value->sha1_digest, rep->sha1_digest, sizeof(value->sha1_digest));
    }

  /* Store them. */
  apr_hash_set(records, key, sizeof(*key), value);

  return SVN_NO_ERROR;
}

/* Inspect the data and/or prop reps of revision REVNUM in FS.  Store
 * reference count tallies in passed hashes (allocated in RESULT_POOL).
 *
 * If PROP_REPS or DATA_REPS is NULL, the respective kind of reps are not
 * tallied.
 *
 * Print progress report to STDERR unless QUIET is true.
 *
 * Use SCRATCH_POOL for temporary allocations.
 */
static svn_error_t *
process_one_revision(svn_fs_t *fs,
                     svn_revnum_t revnum,
                     svn_boolean_t quiet,
                     apr_hash_t *prop_reps,
                     apr_hash_t *data_reps,
                     apr_hash_t *both_reps,
                     apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool)
{
  svn_fs_root_t *rev_root;
  apr_hash_t *paths_changed;
  apr_hash_index_t *hi;

  if (! quiet)
    SVN_ERR(svn_cmdline_fprintf(stderr, scratch_pool,
                                "processing r%ld\n", revnum));

  /* Get the changed paths. */
  SVN_ERR(svn_fs_revision_root(&rev_root, fs, revnum, scratch_pool));
  SVN_ERR(svn_fs_paths_changed2(&paths_changed, rev_root, scratch_pool));

  /* Iterate them. */
  /* ### use iterpool? */
  for (hi = apr_hash_first(scratch_pool, paths_changed);
       hi; hi = apr_hash_next(hi))
    {
      const char *path;
      const svn_fs_path_change2_t *change;
      const svn_fs_id_t *node_rev_id1, *node_rev_id2;
      const svn_fs_id_t *the_id;

      node_revision_t *node_rev;

      path = svn__apr_hash_index_key(hi);
      change = svn__apr_hash_index_val(hi);
      if (! quiet)
        SVN_ERR(svn_cmdline_fprintf(stderr, scratch_pool,
                                    "processing r%ld:%s\n", revnum, path));

      if (change->change_kind == svn_fs_path_change_delete)
        /* Can't ask for reps of PATH at REVNUM if the path no longer exists
         * at that revision! */
        continue;

      /* Okay, we have two node_rev id's for this change: the txn one and
       * the revision one.  We'll use the latter. */
      node_rev_id1 = change->node_rev_id;
      SVN_ERR(svn_fs_node_id(&node_rev_id2, rev_root, path, scratch_pool));

      SVN_ERR_ASSERT(svn_fs_fs__id_txn_id(node_rev_id1) != NULL);
      SVN_ERR_ASSERT(svn_fs_fs__id_rev(node_rev_id2) != SVN_INVALID_REVNUM);

      the_id = node_rev_id2;

      /* Get the node_rev using the chosen node_rev_id. */
      SVN_ERR(svn_fs_fs__get_node_revision(&node_rev, fs, the_id, scratch_pool));

      /* Maybe record the sha1's. */
      SVN_ERR(record(prop_reps, node_rev->prop_rep, result_pool));
      SVN_ERR(record(data_reps, node_rev->data_rep, result_pool));
      SVN_ERR(record(both_reps, node_rev->prop_rep, result_pool));
      SVN_ERR(record(both_reps, node_rev->data_rep, result_pool));
    }

  return SVN_NO_ERROR;
}

/* Print REPS_REF_COUNT (a hash as for process_one_revision())
 * to stdout in "refcount => sha1" format.  A sha1 may appear
 * more than once if not all its instances are shared.  Prepend
 * each line by NAME.
 *
 * Use SCRATCH_POOL for temporary allocations.
 */
static svn_error_t *
pretty_print(const char *name,
             apr_hash_t *reps_ref_counts,
             apr_pool_t *scratch_pool)
{
  apr_hash_index_t *hi;

  if (reps_ref_counts == NULL)
    return SVN_NO_ERROR;

  for (hi = apr_hash_first(scratch_pool, reps_ref_counts);
       hi; hi = apr_hash_next(hi))
    {
      struct value_t *value;

      SVN_ERR(cancel_func(NULL));

      value = svn__apr_hash_index_val(hi);
      SVN_ERR(svn_cmdline_printf(scratch_pool, "%s %" APR_UINT64_T_FMT " %s\n",
                                 name, value->refcount,
                                 svn_checksum_to_cstring_display(
                                   &value->checksum,
                                   scratch_pool)));
    }

  return SVN_NO_ERROR;
}

/* Return an error unless FS is an fsfs fs. */
static svn_error_t *is_fs_fsfs(svn_fs_t *fs, apr_pool_t *scratch_pool)
{
  const char *actual, *expected, *path;

  path = svn_fs_path(fs, scratch_pool);

  expected = SVN_FS_TYPE_FSFS;
  SVN_ERR(svn_fs_type(&actual, path, scratch_pool));

  if (strcmp(actual, expected) != 0)
    return svn_error_createf(SVN_ERR_FS_UNKNOWN_FS_TYPE, NULL,
                             "Filesystem '%s' is not of type '%s'",
                             svn_dirent_local_style(path, scratch_pool),
                             actual);

  return SVN_NO_ERROR;
}

/* The core logic.  This function iterates the repository REPOS_PATH
 * and sends all the (DATA and/or PROP) reps in each revision for counting
 * by process_one_revision().  QUIET is passed to process_one_revision().
 */
static svn_error_t *process(const char *repos_path,
                            svn_boolean_t prop,
                            svn_boolean_t data,
                            svn_boolean_t quiet,
                            apr_pool_t *scratch_pool)
{
  apr_hash_t *prop_reps = NULL;
  apr_hash_t *data_reps = NULL;
  apr_hash_t *both_reps = NULL;
  svn_revnum_t rev, youngest;
  apr_pool_t *iterpool;
  svn_repos_t *repos;
  svn_fs_t *fs;

  if (prop)
    prop_reps = apr_hash_make(scratch_pool);
  if (data)
    data_reps = apr_hash_make(scratch_pool);
  if (prop && data)
    both_reps = apr_hash_make(scratch_pool);

  /* Open the FS. */
  SVN_ERR(svn_repos_open3(&repos, repos_path, NULL, scratch_pool,
                          scratch_pool));
  fs = svn_repos_fs(repos);

  SVN_ERR(is_fs_fsfs(fs, scratch_pool));

  SVN_ERR(svn_fs_youngest_rev(&youngest, fs, scratch_pool));

  /* Iterate the revisions. */
  iterpool = svn_pool_create(scratch_pool);
  for (rev = 0; rev <= youngest; rev++)
    {
      svn_pool_clear(iterpool);
      SVN_ERR(cancel_func(NULL));
      SVN_ERR(process_one_revision(fs, rev, quiet,
                                   prop_reps, data_reps, both_reps,
                                   scratch_pool, iterpool));
    }
  svn_pool_destroy(iterpool);

  /* Print stats. */
  SVN_ERR(pretty_print("prop", prop_reps, scratch_pool));
  SVN_ERR(pretty_print("data", data_reps, scratch_pool));
  SVN_ERR(pretty_print("both", both_reps, scratch_pool));

  return SVN_NO_ERROR;
}

/*
 * On success, leave *EXIT_CODE untouched and return SVN_NO_ERROR. On error,
 * either return an error to be displayed, or set *EXIT_CODE to non-zero and
 * return SVN_NO_ERROR.
 */
static svn_error_t *
sub_main(int *exit_code, int argc, const char *argv[], apr_pool_t *pool)
{
  const char *repos_path;
  svn_boolean_t prop = FALSE, data = FALSE;
  svn_boolean_t quiet = FALSE;
  apr_getopt_t *os;
  const apr_getopt_option_t options[] =
    {
      {"data", OPT_DATA, 0, N_("display data reps stats")},
      {"prop", OPT_PROP, 0, N_("display prop reps stats")},
      {"both", OPT_BOTH, 0, N_("display combined (data+prop) reps stats")},
      {"quiet", 'q', 0, N_("no progress (only errors) to stderr")},
      {"help", 'h', 0, N_("display this help")},
      {"version", OPT_VERSION, 0,
       N_("show program version information")},
      {0,             0,  0,  0}
    };

  /* Check library versions */
  SVN_ERR(check_lib_versions());

  SVN_ERR(svn_cmdline__getopt_init(&os, argc, argv, pool));

  SVN_ERR(check_experimental());

  os->interleave = 1;
  while (1)
    {
      int opt;
      const char *arg;
      apr_status_t status = apr_getopt_long(os, options, &opt, &arg);
      if (APR_STATUS_IS_EOF(status))
        break;
      if (status != APR_SUCCESS)
        {
          usage(pool);
          *exit_code = EXIT_FAILURE;
          return SVN_NO_ERROR;
        }
      switch (opt)
        {
        case OPT_DATA:
          data = TRUE;
          break;
        /* It seems we don't actually rep-share props yet. */
        case OPT_PROP:
          prop = TRUE;
          break;
        case OPT_BOTH:
          data = TRUE;
          prop = TRUE;
          break;
        case 'q':
          quiet = TRUE;
          break;
        case 'h':
          help(options, pool);
          return SVN_NO_ERROR;
        case OPT_VERSION:
          SVN_ERR(version(pool));
          return SVN_NO_ERROR;
        default:
          usage(pool);
          *exit_code = EXIT_FAILURE;
          return SVN_NO_ERROR;
        }
    }

  /* Exactly 1 non-option argument,
   * and at least one of "--data"/"--prop"/"--both".
   */
  if (os->ind + 1 != argc || (!data && !prop))
    {
      usage(pool);
      *exit_code = EXIT_FAILURE;
      return SVN_NO_ERROR;
    }

  /* Grab REPOS_PATH from argv. */
  SVN_ERR(svn_utf_cstring_to_utf8(&repos_path, os->argv[os->ind], pool));
  repos_path = svn_dirent_internal_style(repos_path, pool);

  set_up_cancellation();

  /* Do something. */
  SVN_ERR(process(repos_path, prop, data, quiet, pool));

  /* We're done. */
  return SVN_NO_ERROR;
}

int
main(int argc, const char *argv[])
{
  apr_pool_t *pool;
  int exit_code = EXIT_SUCCESS;
  svn_error_t *err;

  /* Initialize the app. */
  if (svn_cmdline_init("svn-rep-sharing-stats", stderr) != EXIT_SUCCESS)
    return EXIT_FAILURE;

  /* Create our top-level pool.  Use a separate mutexless allocator,
   * given this application is single threaded.
   */
  pool = apr_allocator_owner_get(svn_pool_create_allocator(FALSE));

  err = sub_main(&exit_code, argc, argv, pool);

  /* Flush stdout and report if it fails. It would be flushed on exit anyway
     but this makes sure that output is not silently lost if it fails. */
  err = svn_error_compose_create(err, svn_cmdline_fflush(stdout));

  if (err)
    {
      exit_code = EXIT_FAILURE;
      svn_cmdline_handle_exit_error(err, NULL, "svn-rep-sharing-stats: ");
    }

  svn_pool_destroy(pool);
  return exit_code;
}
