/*
 * props.c :  routines dealing with properties in the working copy
 *
 * ====================================================================
 * Copyright (c) 2000-2002 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */



#include <stdio.h>       /* temporary, for printf() */
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <apr_pools.h>
#include <apr_hash.h>
#include <apr_tables.h>
#include <apr_file_io.h>
#include <apr_strings.h>
#include <apr_lib.h>
#include <apr_general.h>
#include "svn_types.h"
#include "svn_delta.h"
#include "svn_string.h"
#include "svn_pools.h"
#include "svn_path.h"
#include "svn_xml.h"
#include "svn_error.h"
#include "svn_io.h"
#include "svn_hash.h"
#include "svn_wc.h"
#include "svn_time.h"

#include "wc.h"
#include "log.h"
#include "adm_files.h"
#include "entries.h"
#include "props.h"
#include "translate.h"
#include "questions.h"


/*---------------------------------------------------------------------*/

/*** Deducing local changes to properties ***/

/* Given two property hashes, deduce the differences between them
   (from BASEPROPS -> LOCALPROPS).  Return these changes as a series
   of svn_prop_t structures stored in LOCAL_PROPCHANGES, allocated
   from POOL.
   
   For note, here's a quick little table describing the logic of this
   routine:

   basehash        localhash         event
   --------        ---------         -----
   value = foo     value = NULL      Deletion occurred.
   value = foo     value = bar       Set occurred (modification)
   value = NULL    value = baz       Set occurred (creation)
*/
svn_error_t *
svn_wc_get_local_propchanges (apr_array_header_t **local_propchanges,
                              apr_hash_t *localprops,
                              apr_hash_t *baseprops,
                              apr_pool_t *pool)
{
  apr_hash_index_t *hi;
  apr_array_header_t *ary = apr_array_make (pool, 1, sizeof(svn_prop_t));

  /* Note: we will be storing the pointers to the keys (from the hashes)
     into the local_propchanges array. It is acceptable for us to
     reference the same memory as the base/localprops hash. */

  /* Loop over baseprops and examine each key.  This will allow us to
     detect any `deletion' events or `set-modification' events.  */
  for (hi = apr_hash_first (pool, baseprops); hi; hi = apr_hash_next (hi))
    {
      const void *key;
      apr_ssize_t klen;
      void *val;
      svn_stringbuf_t *propval1, *propval2;

      /* Get next property */
      apr_hash_this (hi, &key, &klen, &val);
      propval1 = (svn_stringbuf_t *) val;

      /* Does property name exist in localprops? */
      propval2 = (svn_stringbuf_t *) apr_hash_get (localprops, key, klen);

      if (propval2 == NULL)
        {
          /* Add a delete event to the array */
          svn_prop_t *p = apr_array_push (ary);
          p->name = key;
          p->value = NULL;
        }
      else if (! svn_stringbuf_compare (propval1, propval2))
        {
          /* Add a set (modification) event to the array */
          svn_prop_t *p = apr_array_push (ary);
          p->name = key;
          p->value = svn_string_create_from_buf (propval2, pool);
        }
    }

  /* Loop over localprops and examine each key.  This allows us to
     detect `set-creation' events */
  for (hi = apr_hash_first (pool, localprops); hi; hi = apr_hash_next (hi))
    {
      const void *key;
      apr_ssize_t klen;
      void *val;
      svn_stringbuf_t *propval1, *propval2;

      /* Get next property */
      apr_hash_this (hi, &key, &klen, &val);
      propval2 = (svn_stringbuf_t *) val;

      /* Does property name exist in baseprops? */
      propval1 = (svn_stringbuf_t *) apr_hash_get (baseprops, key, klen);

      if (propval1 == NULL)
        {
          /* Add a set (creation) event to the array */
          svn_prop_t *p = apr_array_push (ary);
          p->name = key;
          p->value = svn_string_create_from_buf (propval2, pool);
        }
    }


  /* Done building our array of user events. */
  *local_propchanges = ary;

  return SVN_NO_ERROR;
}


/*---------------------------------------------------------------------*/

/*** Detecting a property conflict ***/


/* Given two propchange objects, return TRUE iff they conflict.  If
   there's a conflict, DESCRIPTION will contain an english description
   of the problem. */

/* For note, here's the table being implemented:

              |  update set     |    update delete   |
  ------------|-----------------|--------------------|
  user set    | conflict iff    |      conflict      |
              |  vals differ    |                    |
  ------------|-----------------|--------------------|
  user delete |   conflict      |      merge         |
              |                 |    (no problem)    |
  ----------------------------------------------------

*/
svn_boolean_t
svn_wc__conflicting_propchanges_p (const svn_string_t **description,
                                   const svn_prop_t *local,
                                   const svn_prop_t *update,
                                   apr_pool_t *pool)
{
  /* We're assuming that whoever called this routine has already
     deduced that local and change2 affect the same property name.
     (After all, if they affect different property names, how can they
     possibly conflict?)  But still, let's make this routine
     `complete' by checking anyway. */
  if (strcmp(local->name, update->name) == 0)
    return FALSE;  /* no conflict */

  /* If one change wants to delete a property and the other wants to
     set it, this is a conflict.  This check covers two bases of our
     chi-square. */
  if ((local->value != NULL) && (update->value == NULL))
    {
      *description =
        svn_string_createf
        (pool, "prop `%s': user set value to '%s', but update deletes it.\n",
         local->name, local->value->data);
      return TRUE;  /* conflict */
    }
  if ((local->value == NULL) && (update->value != NULL))
    {
      *description =
        svn_string_createf
        (pool, "prop `%s': user deleted, but update sets it to '%s'.\n",
         local->name, update->value->data);
      return TRUE;  /* conflict */
    }

  /* If both changes delete the same property, there's no conflict.
     It's an implicit merge.  :)  */
  if ((local->value == NULL) && (update->value == NULL))
    return FALSE;  /* no conflict */

  /* If both changes set the property, it's a conflict iff the values
     are different */
  else if (! svn_string_compare (local->value, update->value))
    {
      *description =
        svn_string_createf
        (pool, "prop `%s': user set to '%s', but update set to '%s'.\n",
         local->name, local->value->data, update->value->data);
      return TRUE;  /* conflict */
    }
  else
    /* values are the same, so another implicit merge. */
    return FALSE;  /* no conflict */

  /* Default (will anyone ever reach this line?) */
  return FALSE;  /* no conflict found */
}



/*---------------------------------------------------------------------*/

/*** Reading/writing property hashes from disk ***/

/* The real functionality here is part of libsvn_subr, in hashdump.c.
   But these are convenience routines for use in in libsvn_wc. */



/* If PROPFILE_PATH exists (and is a file), assume it's full of
   properties and load this file into HASH.  Otherwise, leave HASH
   untouched.  */
svn_error_t *
svn_wc__load_prop_file (const char *propfile_path,
                        apr_hash_t *hash,
                        apr_pool_t *pool)
{
  enum svn_node_kind kind;

  SVN_ERR (svn_io_check_path (propfile_path, &kind, pool));

  if (kind == svn_node_file)
    {
      /* Ah, this file already has on-disk properties.  Load 'em. */
      apr_status_t status;
      apr_file_t *propfile = NULL;

      status = apr_file_open (&propfile, propfile_path,
                         APR_READ, APR_OS_DEFAULT, pool);
      if (status)
        return svn_error_createf (status, 0, NULL, pool,
                                  "load_prop_file: can't open `%s'",
                                  propfile_path);

      status = svn_hash_read (hash, svn_pack_bytestring,
                              propfile, pool);
      if (status)
        return svn_error_createf (status, 0, NULL, pool,
                                  "load_prop_file:  can't parse `%s'",
                                  propfile_path);

      status = apr_file_close (propfile);
      if (status)
        return svn_error_createf (status, 0, NULL, pool,
                                  "load_prop_file: can't close `%s'",
                                  propfile_path);
    }

  return SVN_NO_ERROR;
}



/* Given a HASH full of property name/values, write them to a file
   located at PROPFILE_PATH.  */
svn_error_t *
svn_wc__save_prop_file (const char *propfile_path,
                        apr_hash_t *hash,
                        apr_pool_t *pool)
{
  apr_status_t apr_err;
  apr_file_t *prop_tmp;

  apr_err = apr_file_open (&prop_tmp, propfile_path,
                      (APR_WRITE | APR_CREATE | APR_TRUNCATE),
                      APR_OS_DEFAULT, pool);
  if (apr_err)
    return svn_error_createf (apr_err, 0, NULL, pool,
                              "save_prop_file: can't open `%s'",
                              propfile_path);

  apr_err = svn_hash_write (hash, svn_unpack_bytestring,
                            prop_tmp, pool);
  if (apr_err)
    return svn_error_createf (apr_err, 0, NULL, pool,
                              "save_prop_file: can't write prop hash to `%s'",
                              propfile_path);

  apr_err = apr_file_close (prop_tmp);
  if (apr_err)
    return svn_error_createf (apr_err, 0, NULL, pool,
                              "save_prop_file: can't close `%s'",
                              propfile_path);

  return SVN_NO_ERROR;
}


/*---------------------------------------------------------------------*/

/*** Misc ***/

/* Assuming FP is a filehandle already open for appending, write
   CONFLICT_DESCRIPTION to file. */
static svn_error_t *
append_prop_conflict (apr_file_t *fp,
                      const svn_string_t *conflict_description,
                      apr_pool_t *pool)
{
  /* TODO:  someday, perhaps prefix each conflict_description with a
     timestamp or something? */
  apr_size_t written;
  apr_status_t status;

  status = apr_file_write_full (fp, conflict_description->data,
                                conflict_description->len, &written);
  if (status)
    return svn_error_create (status, 0, NULL, pool,
                             "append_prop_conflict: apr_file_write_full failed.");
  return SVN_NO_ERROR;
}


/* ### not used outside this file. make it static? */
svn_error_t *
svn_wc__get_existing_prop_reject_file (const svn_string_t **reject_file,
                                       const char *path,
                                       const char *name,
                                       apr_pool_t *pool)
{
  apr_hash_t *entries;
  svn_wc_entry_t *the_entry;

  /* ### be nice to get rid of this */
  svn_stringbuf_t *pathbuf = svn_stringbuf_create (path, pool);

  SVN_ERR (svn_wc_entries_read (&entries, pathbuf, pool));
  the_entry = apr_hash_get (entries, name, APR_HASH_KEY_STRING);

  if (! the_entry)
    return svn_error_createf
      (SVN_ERR_ENTRY_NOT_FOUND, 0, NULL, pool,
       "get_existing_reject_prop_reject_file: can't find entry '%s' in '%s'",
       name, path);

  *reject_file = the_entry->prejfile 
                 ? svn_string_create_from_buf (the_entry->prejfile, pool)
                 : NULL;
  return SVN_NO_ERROR;
}



/*---------------------------------------------------------------------*/

/*** Merging propchanges into the working copy ***/

svn_error_t *
svn_wc_merge_prop_diffs (const char *path,
                         const apr_array_header_t *propchanges,
                         apr_pool_t *pool)
{
  apr_status_t apr_err;
  apr_hash_t *ignored_conflicts;
  svn_wc_entry_t *entry;
  svn_stringbuf_t *path_s, *parent_s, *basename_s, *log_accum;
  apr_file_t *log_fp = NULL;

  SVN_ERR (svn_wc_entry (&entry, svn_stringbuf_create (path, pool), pool));
  if (entry == NULL)
    return svn_error_createf (SVN_ERR_ENTRY_NOT_FOUND, 0, NULL, pool,
                              "Can't merge props into '%s':"
                              "it's not under revision control.", path);

  /* Notice that we're not using svn_path_split_if_file(), because
     that looks at the actual working file.  It's existence shouldn't
     matter, so we're looking at entry->kind instead. */
  switch (entry->kind)
    {
    case svn_node_dir:
      parent_s = svn_stringbuf_create (path, pool);
      basename_s = NULL;
      break;
    case svn_node_file:
      path_s = svn_stringbuf_create (path, pool);
      svn_path_split (path_s, &parent_s, &basename_s, pool);
    default:
      return SVN_NO_ERROR; /* ### svn_node_none or svn_node_unknown */
    }

  SVN_ERR (svn_wc_lock (parent_s, 0, pool));
  SVN_ERR (svn_wc__open_adm_file (&log_fp, parent_s, SVN_WC__ADM_LOG,
                                  (APR_WRITE | APR_CREATE), /* not excl */
                                  pool));
  log_accum = svn_stringbuf_create ("", pool);
  
  /* Note that while this routine does the "real" work, it's only
     prepping tempfiles and writing log commands.  */
  SVN_ERR (svn_wc__merge_prop_diffs (parent_s->data,
                                     basename_s ? basename_s->data : NULL,
                                     propchanges,
                                     pool,
                                     &log_accum,
                                     &ignored_conflicts));

  apr_err = apr_file_write_full (log_fp, log_accum->data, 
                                 log_accum->len, NULL);
  if (apr_err)
    {
      apr_file_close (log_fp);
      return svn_error_createf (apr_err, 0, NULL, pool,
                                "svn_wc_merge_prop_diffs:"
                                "error writing log for %s", path);
    }

  SVN_ERR (svn_wc__close_adm_file (log_fp, parent_s, SVN_WC__ADM_LOG,
                                   1, /* sync */ pool));
  SVN_ERR (svn_wc__run_log (parent_s, pool));

  SVN_ERR (svn_wc_unlock (parent_s, pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__merge_prop_diffs (const char *path,
                          const char *name,
                          const apr_array_header_t *propchanges,
                          apr_pool_t *pool,
                          svn_stringbuf_t **entry_accum,
                          apr_hash_t **conflicts)
{
  int i;
  svn_boolean_t is_dir;
  const char * str;
  apr_off_t len;
  
  /* Zillions of pathnames to compute!  yeargh!  */
  svn_stringbuf_t *base_propfile_path, *local_propfile_path;
  svn_stringbuf_t *base_prop_tmp_path, *local_prop_tmp_path;
  svn_stringbuf_t *tmp_prop_base, *real_prop_base;
  svn_stringbuf_t *tmp_props, *real_props;

  const char *entryname;
  svn_stringbuf_t *full_path;
  
  apr_array_header_t *local_propchanges; /* propchanges that the user
                                            has made since last update */
  apr_hash_t *localhash;   /* all `working' properties */
  apr_hash_t *basehash;    /* all `pristine' properties */

  /* For writing conflicts to a .prej file */
  apr_file_t *reject_fp = NULL;           /* the real conflicts file */
  const svn_string_t *reject_path = NULL;
  svn_stringbuf_t *reject_pathbuf = NULL;

  apr_file_t *reject_tmp_fp = NULL;       /* the temporary conflicts file */
  svn_stringbuf_t *reject_tmp_path = NULL;

  *conflicts = apr_hash_make (pool);

  if (name == NULL)
    {
      /* We must be merging props on the directory PATH  */
      entryname = SVN_WC_ENTRY_THIS_DIR;
      full_path = svn_stringbuf_create (path, pool);
      is_dir = TRUE;
    }
  else
    {
      /* We must be merging props on the file PATH/NAME */
      entryname = name;
      full_path = svn_stringbuf_create (path, pool);
      svn_path_add_component_nts (full_path, name);
      is_dir = FALSE;
    }

  /* Get paths to the local and pristine property files. */
  SVN_ERR (svn_wc__prop_path (&local_propfile_path, full_path, 0, pool));
  
  SVN_ERR (svn_wc__prop_base_path (&base_propfile_path, full_path, 0, pool));

  /* Load the base & working property files into hashes */
  localhash = apr_hash_make (pool);
  basehash = apr_hash_make (pool);
  
  SVN_ERR (svn_wc__load_prop_file (base_propfile_path->data, basehash, pool));
  
  SVN_ERR (svn_wc__load_prop_file (local_propfile_path->data, localhash, pool));
  
  /* Deduce any local propchanges the user has made since the last
     update.  */
  SVN_ERR (svn_wc_get_local_propchanges (&local_propchanges,
                                         localhash, basehash, pool));
  
  /* Looping over the array of `update' propchanges we want to apply: */
  for (i = 0; i < propchanges->nelts; i++)
    {
      int j;
      int found_match = 0;          
      const svn_string_t *conflict_description;
      const svn_prop_t *update_change;
      const svn_prop_t *local_change = NULL;
      svn_stringbuf_t *value_buf;
      
      update_change = &APR_ARRAY_IDX(propchanges, i, svn_prop_t);

      if (update_change->value == NULL)
        value_buf = NULL;
      else
        value_buf = svn_stringbuf_create_from_string (update_change->value,
                                                      pool);

      /* Apply the update_change to the pristine hash, no
         questions asked. */
      apr_hash_set (basehash,
                    update_change->name, APR_HASH_KEY_STRING,
                    value_buf);
      
      /* Now, does the update_change conflict with some local change?  */
      
      /* First check if the property name even exists in our list
         of local changes... */
      for (j = 0; j < local_propchanges->nelts; j++)

        {
          local_change = &APR_ARRAY_IDX(local_propchanges, j, svn_prop_t);

          if (strcmp(local_change->name, update_change->name) == 0)
            {
              found_match = 1;
              break;
            }
        }
      
      if (found_match)
        /* Now see if the two changes actually conflict */
        if (svn_wc__conflicting_propchanges_p (&conflict_description,
                                               local_change,
                                               update_change,
                                               pool))
          {
            /* Found a conflict! */

            const svn_prop_t *conflict_prop;

            /* Copy the conflicting prop structure out of the array so that
               changes to the array do not muck up the pointers stored into
               the hash table. */
            conflict_prop = apr_pmemdup (pool,
                                         update_change,
                                         sizeof(*update_change));

            /* Note the conflict in the conflict-hash. */
            apr_hash_set (*conflicts,
                          update_change->name, APR_HASH_KEY_STRING,
                          conflict_prop);

            if (! reject_tmp_fp)
              {
                /* This is the very first prop conflict found on this
                   node. */
                svn_stringbuf_t *tmppath;
                const char *tmpname;

                /* Get path to /temporary/ local prop file */
                SVN_ERR (svn_wc__prop_path (&tmppath, full_path, 1, pool));

                /* Reserve a .prej file based on it.  */
                SVN_ERR (svn_io_open_unique_file (&reject_tmp_fp,
                                                  &reject_tmp_path,
                                                  tmppath->data,
                                                  SVN_WC__PROP_REJ_EXT,
                                                  FALSE,
                                                  pool));

                /* reject_tmp_path is an absolute path at this point,
                   but that's no good for us.  We need to convert this
                   path to a *relative* path to use in the logfile. */
                tmpname = svn_path_basename (reject_tmp_path->data, pool);

                if (is_dir)
                  {
                    /* Dealing with directory "path" */
                    reject_tmp_path = 
                      svn_wc__adm_path (svn_stringbuf_create ("", pool),
                                        TRUE, /* use tmp */
                                        pool,
                                        tmpname,
                                        NULL);
                  }
                else
                  {
                    /* Dealing with file "path/name" */
                    reject_tmp_path = 
                      svn_wc__adm_path (svn_stringbuf_create ("", pool),
                                        TRUE, 
                                        pool,
                                        SVN_WC__ADM_PROPS,
                                        tmpname,
                                        NULL);
                  }               
              }

            /* Append the conflict to the open tmp/PROPS/---.prej file */
            SVN_ERR (append_prop_conflict (reject_tmp_fp,
                                           conflict_description,
                                           pool));

            continue;  /* skip to the next update_change */
          }
      
      /* If we reach this point, there's no conflict, so we can safely
         apply the update_change to our working property hash. */
      apr_hash_set (localhash,
                    update_change->name, APR_HASH_KEY_STRING,
                    value_buf);
    }
  
  
  /* Done merging property changes into both pristine and working
  hashes.  Now we write them to temporary files.  Notice that the
  paths computed are ABSOLUTE pathnames, which is what our disk
  routines require.*/

  SVN_ERR (svn_wc__prop_base_path (&base_prop_tmp_path, full_path, 1, pool));

  SVN_ERR (svn_wc__prop_path (&local_prop_tmp_path, full_path, 1, pool));
  
  /* Write the merged pristine prop hash to either
     path/.svn/tmp/prop-base/name or path/.svn/tmp/dir-prop-base */
  SVN_ERR (svn_wc__save_prop_file (base_prop_tmp_path->data, basehash, pool));
  
  /* Write the merged local prop hash to path/.svn/tmp/props/name or
     path/.svn/tmp/dir-props */
  SVN_ERR (svn_wc__save_prop_file (local_prop_tmp_path->data, localhash, pool));
  
  /* Compute pathnames for the "mv" log entries.  Notice that these
     paths are RELATIVE pathnames (each beginning with ".svn/"), so
     that each .svn subdir remains separable when executing run_log().  */
  str = strstr (base_prop_tmp_path->data, SVN_WC_ADM_DIR_NAME);
  len = base_prop_tmp_path->data + base_prop_tmp_path->len - str;
  tmp_prop_base = svn_stringbuf_ncreate (str, len, pool);

  str = strstr (base_propfile_path->data, SVN_WC_ADM_DIR_NAME);
  len = base_propfile_path->data + base_propfile_path->len - str;
  real_prop_base = svn_stringbuf_ncreate (str, len, pool);

  str = strstr (local_prop_tmp_path->data, SVN_WC_ADM_DIR_NAME);
  len = local_prop_tmp_path->data + local_prop_tmp_path->len - str;
  tmp_props = svn_stringbuf_ncreate (str, len, pool);

  str = strstr (local_propfile_path->data, SVN_WC_ADM_DIR_NAME);
  len = local_propfile_path->data + local_propfile_path->len - str;
  real_props = svn_stringbuf_ncreate (str, len, pool);
  
  /* Write log entry to move pristine tmp copy to real pristine area. */
  svn_xml_make_open_tag (entry_accum,
                         pool,
                         svn_xml_self_closing,
                         SVN_WC__LOG_MV,
                         SVN_WC__LOG_ATTR_NAME,
                         tmp_prop_base,
                         SVN_WC__LOG_ATTR_DEST,
                         real_prop_base,
                         NULL);

  /* Make prop-base readonly */
  svn_xml_make_open_tag (entry_accum,
                         pool,
                         svn_xml_self_closing,
                         SVN_WC__LOG_READONLY,
                         SVN_WC__LOG_ATTR_NAME,
                         real_prop_base,
                         NULL);

  /* Write log entry to move working tmp copy to real working area. */
  svn_xml_make_open_tag (entry_accum,
                         pool,
                         svn_xml_self_closing,
                         SVN_WC__LOG_MV,
                         SVN_WC__LOG_ATTR_NAME,
                         tmp_props,
                         SVN_WC__LOG_ATTR_DEST,
                         real_props,
                         NULL);

  /* Make props readonly */
  svn_xml_make_open_tag (entry_accum,
                         pool,
                         svn_xml_self_closing,
                         SVN_WC__LOG_READONLY,
                         SVN_WC__LOG_ATTR_NAME,
                         real_props,
                         NULL);

  if (reject_tmp_fp)
    {
      svn_stringbuf_t *entryname_buf;

      /* There's a .prej file sitting in .svn/tmp/ somewhere.  Deal
         with the conflicts.  */

      /* First, _close_ this temporary conflicts file.  We've been
         appending to it all along. */
      apr_status_t status;
      status = apr_file_close (reject_tmp_fp);
      if (status)
        return svn_error_createf (status, 0, NULL, pool,
                                  "do_property_merge: can't close '%s'",
                                  reject_tmp_path->data);
                                  
      /* Now try to get the name of a pre-existing .prej file from the
         entries file */
      SVN_ERR (svn_wc__get_existing_prop_reject_file (&reject_path,
                                                      path,
                                                      entryname,
                                                      pool));

      /* ### it would be nice if the XML funcs too an svn_string_t */
      if (reject_path != NULL)
        reject_pathbuf = svn_stringbuf_create_from_string (reject_path, pool);

      if (! reject_pathbuf)
        {
          /* Reserve a new .prej file *above* the .svn/ directory by
             opening and closing it. */
          svn_stringbuf_t *reserved_path;
          const char *full_reject_path;

          if (is_dir)
            full_reject_path = svn_path_join (path,
                                              SVN_WC__THIS_DIR_PREJ,
                                              pool);
          else
            full_reject_path = svn_path_join (path, name, pool);

          SVN_ERR (svn_io_open_unique_file (&reject_fp,
                                            &reserved_path,
                                            full_reject_path,
                                            SVN_WC__PROP_REJ_EXT,
                                            FALSE,
                                            pool));

          status = apr_file_close (reject_fp);
          if (status)
            return svn_error_createf (status, 0, NULL, pool,
                                      "do_property_merge: can't close '%s'",
                                      full_reject_path);
          
          /* This file will be overwritten when the log is run; that's
             ok, because at least now we have a reservation on
             disk. */

          /* Now just get the name of the reserved file.  This is the
             "relative" path we will use in the log entry. */
          reject_pathbuf =
            svn_stringbuf_create (svn_path_basename (reserved_path->data,
                                                     pool),
                                  pool);
        }

      /* We've now guaranteed that some kind of .prej file exists
         above the .svn/ dir.  We write log entries to append our
         conflicts to it. */      
      svn_xml_make_open_tag (entry_accum,
                             pool,
                             svn_xml_self_closing,
                             SVN_WC__LOG_APPEND,
                             SVN_WC__LOG_ATTR_NAME,
                             reject_tmp_path,
                             SVN_WC__LOG_ATTR_DEST,
                             reject_pathbuf,
                             NULL);

      /* And of course, delete the temporary reject file. */
      svn_xml_make_open_tag (entry_accum,
                             pool,
                             svn_xml_self_closing,
                             SVN_WC__LOG_RM,
                             SVN_WC__LOG_ATTR_NAME,
                             reject_tmp_path,
                             NULL);
      
      /* ### be nice if the XML funcs took svn_string_t */
      entryname_buf = svn_stringbuf_create (entryname, pool);

      /* Mark entry as "conflicted" with a particular .prej file. */
      svn_xml_make_open_tag (entry_accum,
                             pool,
                             svn_xml_self_closing,
                             SVN_WC__LOG_MODIFY_ENTRY,
                             SVN_WC__LOG_ATTR_NAME,
                             entryname_buf,
                             SVN_WC__ENTRY_ATTR_PREJFILE,
                             reject_pathbuf,
                             NULL);      

    } /* if (reject_tmp_fp) */
  
  /* At this point, we need to write log entries that bump revision
     number and set new entry timestamps.  The caller of this function
     should (hopefully) add those commands to the log accumulator. */

  return SVN_NO_ERROR;
}



/*------------------------------------------------------------------*/

/*** Private 'wc prop' functions ***/

/* A clone of svn_wc_prop_list, for the most part, except that it
   returns 'wc' props instead of normal props.  */
static svn_error_t *
wcprop_list (apr_hash_t **props,
             const char *path,
             apr_pool_t *pool)
{
  enum svn_node_kind kind, pkind;
  svn_stringbuf_t *prop_path;
  
  /* ### be nice to eliminate this... */
  svn_stringbuf_t *pathbuf = svn_stringbuf_create (path, pool);

  *props = apr_hash_make (pool);

  /* Check validity of PATH */
  SVN_ERR( svn_io_check_path (pathbuf->data, &kind, pool) );
  
#if 0
  if (kind == svn_node_none)
    return svn_error_createf (SVN_ERR_BAD_FILENAME, 0, NULL, pool,
                              "wcprop_list: non-existent path '%s'.",
                              path);
  
  if (kind == svn_node_unknown)
    return svn_error_createf (SVN_ERR_UNKNOWN_NODE_KIND, 0, NULL, pool,
                              "wcprop_list: unknown node kind: '%s'.",
                              path);
#endif

  /* Construct a path to the relevant property file */
  SVN_ERR( svn_wc__wcprop_path (&prop_path, pathbuf, 0, pool) );

  /* Does the property file exist? */
  SVN_ERR( svn_io_check_path (prop_path->data, &pkind, pool) );
  
  if (pkind == svn_node_none)
    /* No property file exists.  Just go home, with an empty hash. */
    return SVN_NO_ERROR;
  
  /* else... */

  SVN_ERR( svn_wc__load_prop_file (prop_path->data, *props, pool) );

  return SVN_NO_ERROR;
}


/* This is what RA_DAV will use to fetch 'wc' properties.  It will be
   passed to ra_session_baton->do_commit(). */
svn_error_t *
svn_wc__wcprop_get (const svn_string_t **value,
                    const char *name,
                    const char *path,
                    apr_pool_t *pool)
{
  svn_error_t *err;
  apr_hash_t *prophash;
  svn_stringbuf_t *pvaluebuf;

  err = wcprop_list (&prophash, path, pool);
  if (err)
    return
      svn_error_quick_wrap
      (err, "svn_wc__wcprop_get: failed to load props from disk.");

  /* ### it would be nice if the hash contained svn_string_t values */
  pvaluebuf = apr_hash_get (prophash, name, APR_HASH_KEY_STRING);
  *value = pvaluebuf ? svn_string_create_from_buf (pvaluebuf, pool) : NULL;

  return SVN_NO_ERROR;
}



/* This is what RA_DAV will use to store 'wc' properties.  It will be
   passed to ra_session_baton->do_commit(). */
svn_error_t *
svn_wc__wcprop_set (const char *name,
                    const svn_string_t *value,
                    const char *path,
                    apr_pool_t *pool)
{
  svn_error_t *err;
  apr_status_t apr_err;
  apr_hash_t *prophash;
  apr_file_t *fp = NULL;

  /* ### be nice to eliminate these... */
  svn_stringbuf_t *valuebuf = svn_stringbuf_create_from_string (value, pool);
  svn_stringbuf_t *pathbuf = svn_stringbuf_create (path, pool);

  err = wcprop_list (&prophash, path, pool);
  if (err)
    return
      svn_error_quick_wrap
      (err, "svn_wc__wcprop_set: failed to load props from disk.");

  /* Now we have all the properties in our hash.  Simply merge the new
     property into it. */
  apr_hash_set (prophash, name, APR_HASH_KEY_STRING, valuebuf);

  /* Open the propfile for writing. */
  SVN_ERR (svn_wc__open_props (&fp, 
                               pathbuf, /* open in PATH */
                               (APR_WRITE | APR_CREATE),
                               0, /* not base props */
                               1, /* we DO want wcprops */
                               pool));
  /* Write. */
  apr_err = svn_hash_write (prophash, svn_unpack_bytestring, fp, pool);
  if (apr_err)
    return svn_error_createf (apr_err, 0, NULL, pool,
                              "can't write prop hash for %s", path);
  
  /* Close file, and doing an atomic "move". */
  SVN_ERR (svn_wc__close_props (fp, pathbuf, 0, 1,
                                1, /* sync! */
                                pool));

  return SVN_NO_ERROR;
}




/*------------------------------------------------------------------*/

/*** Public Functions ***/


svn_error_t *
svn_wc_prop_list (apr_hash_t **props,
                  const char *path,
                  apr_pool_t *pool)
{
  enum svn_node_kind pkind;
  svn_stringbuf_t *prop_path;

  /* ### be nice to eliminate this */
  svn_stringbuf_t *pathbuf = svn_stringbuf_create (path, pool);

  *props = apr_hash_make (pool);

  /* Construct a path to the relevant property file */
  SVN_ERR (svn_wc__prop_path (&prop_path, pathbuf, 0, pool));

  /* Does the property file exist? */
  SVN_ERR (svn_io_check_path (prop_path->data, &pkind, pool));
  
  if (pkind == svn_node_none)
    /* No property file exists.  Just go home, with an empty hash. */
    return SVN_NO_ERROR;
  
  /* else... */

  SVN_ERR (svn_wc__load_prop_file (prop_path->data, *props, pool));

  return SVN_NO_ERROR;
}





svn_error_t *
svn_wc_prop_get (const svn_string_t **value,
                 const char *name,
                 const char *path,
                 apr_pool_t *pool)
{
  svn_error_t *err;
  apr_hash_t *prophash;
  svn_stringbuf_t *pvaluebuf;

  /* Boy, this is an easy routine! */
  err = svn_wc_prop_list (&prophash, path, pool);
  if (err)
    return
      svn_error_quick_wrap
      (err, "svn_wc_prop_get: failed to load props from disk.");

  pvaluebuf = apr_hash_get (prophash, name, APR_HASH_KEY_STRING);
  *value = pvaluebuf ? svn_string_create_from_buf (pvaluebuf, pool) : NULL;

  return SVN_NO_ERROR;
}




svn_error_t *
svn_wc_prop_set (const char *name,
                 const svn_string_t *value,
                 const char *path,
                 apr_pool_t *pool)
{
  svn_error_t *err;
  apr_status_t apr_err;
  apr_hash_t *prophash;
  apr_file_t *fp = NULL;
  svn_wc_keywords_t *old_keywords;

  /* ### argh */
  svn_stringbuf_t *pathbuf = svn_stringbuf_create (path, pool);
  svn_stringbuf_t *valuebuf;

  err = svn_wc_prop_list (&prophash, path, pool);
  if (err)
    return
      svn_error_quick_wrap
      (err, "svn_wc_prop_set: failed to load props from disk.");

  /* If we're changing this file's list of expanded keywords, then
   * we'll need to invalidate its text timestamp, since keyword
   * expansion affects the comparison of working file to text base.
   *
   * Here we retrieve the old list of expanded keywords; after the
   * property is set, we'll grab the new list and see if it differs
   * from the old one.
   */
  if ((strcmp (name, SVN_PROP_KEYWORDS)) == 0)
    SVN_ERR (svn_wc__get_keywords (&old_keywords, path, NULL, pool));

  /* ### darned stringbuf */
  valuebuf = value ? svn_stringbuf_create_from_string (value, pool) : NULL;

  /* Now we have all the properties in our hash.  Simply merge the new
     property into it. */
  apr_hash_set (prophash, name, APR_HASH_KEY_STRING, valuebuf);
  
  /* Open the propfile for writing. */
  SVN_ERR (svn_wc__open_props (&fp, 
                               pathbuf, /* open in PATH */
                               (APR_WRITE | APR_CREATE),
                               0, /* not base props */
                               0, /* not wcprops */
                               pool));
  /* Write. */
  apr_err = svn_hash_write (prophash, svn_unpack_bytestring, fp, pool);
  if (apr_err)
    return svn_error_createf (apr_err, 0, NULL, pool,
                              "can't write prop hash for %s", path);
  
  /* Close file, and doing an atomic "move". */
  SVN_ERR (svn_wc__close_props (fp, pathbuf, 0, 0,
                                1, /* sync! */
                                pool));

  if ((strcmp (name, SVN_PROP_KEYWORDS)) == 0)
    {
      svn_wc_keywords_t *new_keywords;
      SVN_ERR (svn_wc__get_keywords (&new_keywords, path, NULL, pool));

      if (svn_wc_keywords_differ (old_keywords, new_keywords, FALSE))
        {
          svn_stringbuf_t *pdir, *basename;
          svn_wc_entry_t tmp_entry;

          /* If we changed the keywords or newlines, void the entry
             timestamp for this file, so svn_wc_text_modified_p() does
             a real (albeit slow) check later on. */
          svn_path_split (pathbuf, &pdir, &basename, pool);
          tmp_entry.kind = svn_node_file;
          tmp_entry.text_time = 0;
          SVN_ERR (svn_wc__entry_modify (pdir, basename, &tmp_entry,
                                         SVN_WC__ENTRY_MODIFY_TEXT_TIME,
                                         pool));
        }
    }

  return SVN_NO_ERROR;
}


svn_boolean_t
svn_wc_is_normal_prop (const char *name)
{
  apr_size_t namelen = strlen(name);
  apr_size_t wc_prefix_len = sizeof (SVN_PROP_WC_PREFIX) - 1;
  apr_size_t entry_prefix_len = sizeof (SVN_PROP_ENTRY_PREFIX) - 1;

  /* quick answer */
  if ((namelen < wc_prefix_len) && (namelen < entry_prefix_len))
    return TRUE;

  if ((strncmp (name, SVN_PROP_WC_PREFIX, wc_prefix_len) == 0)
      || (strncmp (name, SVN_PROP_ENTRY_PREFIX, entry_prefix_len) == 0))
    return FALSE;
  else
    return TRUE;
}



svn_boolean_t
svn_wc_is_wc_prop (const char *name)
{
  apr_size_t namelen = strlen(name);
  apr_size_t prefix_len = sizeof (SVN_PROP_WC_PREFIX) - 1;

  if ((namelen < prefix_len)
      || (strncmp (name, SVN_PROP_WC_PREFIX, prefix_len) != 0))
    return FALSE;
  else
    return TRUE;
}


svn_boolean_t
svn_wc_is_entry_prop (const char *name)
{
  apr_size_t namelen = strlen(name);
  apr_size_t prefix_len = sizeof (SVN_PROP_ENTRY_PREFIX) - 1;

  if ((namelen < prefix_len)
      || (strncmp (name, SVN_PROP_ENTRY_PREFIX, prefix_len) != 0))
    return FALSE;
  else
    return TRUE;
}


void
svn_wc__strip_entry_prefix (svn_stringbuf_t *name)
{
  apr_size_t prefix_len = sizeof (SVN_PROP_ENTRY_PREFIX) - 1;

  if (! svn_wc_is_entry_prop (name->data))
    return;

  name->data += prefix_len;
  name->len -= prefix_len;
}



/* Helper to optimize svn_wc_props_modified_p().

   If PATH_TO_PROP_FILE is nonexistent, or is of size 4 bytes ("END"),
   then set EMPTY_P to true.   Otherwise set EMPTY_P to false, which
   means that the file must contain real properties.  */
static svn_error_t *
empty_props_p (svn_boolean_t *empty_p,
               svn_stringbuf_t *path_to_prop_file,
               apr_pool_t *pool)
{
  enum svn_node_kind kind;

  SVN_ERR (svn_io_check_path (path_to_prop_file->data, &kind, pool));

  if (kind == svn_node_none)
    *empty_p = TRUE;

  else 
    {
      apr_finfo_t finfo;
      apr_status_t status;

      status = apr_stat (&finfo, path_to_prop_file->data, APR_FINFO_MIN, pool);
      if (status)
        return svn_error_createf (status, 0, NULL, pool,
                                  "couldn't stat '%s'...",
                                  path_to_prop_file->data);

      /* If we remove props from a propfile, eventually the file will
         contain nothing but "END\n" */
      if (finfo.size == 4)  
        *empty_p = TRUE;

      else
        *empty_p = FALSE;

      /* ### really, if the size is < 4, then something is corrupt.
         If the size is between 4 and 16, then something is corrupt,
         because 16 is the -smallest- the file can possibly be if it
         contained only one property.  someday we should check for
         this. */

    }

  return SVN_NO_ERROR;
}


/* Simple wrapper around empty_props_p, and inversed. */
svn_error_t *
svn_wc__has_props (svn_boolean_t *has_props,
                   svn_stringbuf_t *path,
                   apr_pool_t *pool)
{
  svn_boolean_t is_empty;
  svn_stringbuf_t *prop_path;

  SVN_ERR (svn_wc__prop_path (&prop_path, path, 0, pool));
  SVN_ERR (empty_props_p (&is_empty, prop_path, pool));

  if (is_empty)
    *has_props = FALSE;
  else
    *has_props = TRUE;

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_props_modified_p (svn_boolean_t *modified_p,
                         svn_stringbuf_t *path,
                         apr_pool_t *pool)
{
  svn_boolean_t bempty, wempty;
  svn_stringbuf_t *prop_path;
  svn_stringbuf_t *prop_base_path;
  svn_boolean_t different_filesizes, equal_timestamps;
  apr_pool_t *subpool = svn_pool_create (pool);

  /* First, get the paths of the working and 'base' prop files. */
  SVN_ERR (svn_wc__prop_path (&prop_path, path, 0, subpool));
  SVN_ERR (svn_wc__prop_base_path (&prop_base_path, path, 0, subpool));

  /* Decide if either path is "empty" of properties. */
  SVN_ERR (empty_props_p (&wempty, prop_path, subpool));
  SVN_ERR (empty_props_p (&bempty, prop_base_path, subpool));

  /* Easy out:  if the base file is empty, we know the answer
     immediately. */
  if (bempty)
    {
      if (! wempty)
        {
          /* base is empty, but working is not */
          *modified_p = TRUE;
          goto cleanup;
        }
      else
        {
          /* base and working are both empty */
          *modified_p = FALSE;
          goto cleanup;
        }
    }

  /* OK, so the base file is non-empty.  One more easy out: */
  if (wempty)
    {
      /* base exists, working is empty */
      *modified_p = TRUE;
      goto cleanup;
    }

  /* At this point, we know both files exists.  Therefore we have no
     choice but to start checking their contents. */
  
  /* There are at least three tests we can try in succession. */
  
  /* Easy-answer attempt #1:  */
  
  /* Check if the the local and prop-base file have *definitely*
     different filesizes. */
  SVN_ERR (svn_io_filesizes_different_p (&different_filesizes,
                                         prop_path->data,
                                         prop_base_path->data,
                                         subpool));
  if (different_filesizes) 
    {
      *modified_p = TRUE;
      goto cleanup;
    }
  
  /* Easy-answer attempt #2:  */
      
  /* See if the local file's prop timestamp is the same as the one
     recorded in the administrative directory.  */
  SVN_ERR (svn_wc__timestamps_equal_p (&equal_timestamps, path,
                                       svn_wc__prop_time, subpool));
  if (equal_timestamps)
    {
      *modified_p = FALSE;
      goto cleanup;
    }
  
  /* Last ditch attempt:  */
  
  /* If we get here, then we know that the filesizes are the same,
     but the timestamps are different.  That's still not enough
     evidence to make a correct decision;  we need to look at the
     files' contents directly.

     However, doing a byte-for-byte comparison won't work.  The two
     properties files may have the *exact* same name/value pairs, but
     arranged in a different order.  (Our hashdump format makes no
     guarantees about ordering.)

     Therefore, rather than use contents_identical_p(), we use
     svn_wc_get_local_propchanges(). */
  {
    apr_array_header_t *local_propchanges;
    apr_hash_t *localprops = apr_hash_make (subpool);
    apr_hash_t *baseprops = apr_hash_make (subpool);

    SVN_ERR (svn_wc__load_prop_file (prop_path->data, localprops, subpool));
    SVN_ERR (svn_wc__load_prop_file (prop_base_path->data,
                                     baseprops,
                                     subpool));
    SVN_ERR (svn_wc_get_local_propchanges (&local_propchanges,
                                           localprops,
                                           baseprops,
                                           subpool));
                                         
    if (local_propchanges->nelts > 0)
      *modified_p = TRUE;
    else
      *modified_p = FALSE;
  }
 
 cleanup:
  svn_pool_destroy (subpool);
  
  return SVN_NO_ERROR;
}



svn_error_t *
svn_wc_get_prop_diffs (apr_array_header_t **propchanges,
                       apr_hash_t **original_props,
                       const char *path,
                       apr_pool_t *pool)
{
  svn_stringbuf_t *path_s, *prop_path, *prop_base_path;
  apr_array_header_t *local_propchanges;
  apr_hash_t *localprops = apr_hash_make (pool);
  apr_hash_t *baseprops = apr_hash_make (pool);

  path_s = svn_stringbuf_create (path, pool);

  SVN_ERR (svn_wc__prop_path (&prop_path, path_s, 0, pool));
  SVN_ERR (svn_wc__prop_base_path (&prop_base_path, path_s, 0, pool));

  SVN_ERR (svn_wc__load_prop_file (prop_path->data, localprops, pool));
  SVN_ERR (svn_wc__load_prop_file (prop_base_path->data, baseprops, pool));

  /* At this point, if either of the propfiles are non-existent, then
     the corresponding hash is simply empty. */

  SVN_ERR (svn_wc_get_local_propchanges (&local_propchanges,
                                         localprops,
                                         baseprops,
                                         pool));

  if (original_props != NULL)
    *original_props = baseprops;

  *propchanges = local_propchanges;

  return SVN_NO_ERROR;
}



/*
 * local variables:
 * eval: (load-file "../../tools/dev/svn-dev.el")
 * end: */
