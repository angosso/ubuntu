/*
 * tree.c: reading a generic tree
 *
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

#include "svn_dirent_uri.h"
#include "client.h"
#include "tree.h"


/*-----------------------------------------------------------------*/


/* */
typedef struct disk_tree_baton_t
{
  const char *tree_abspath;
} disk_tree_baton_t;

/* */
static svn_error_t *
disk_tree_get_kind(svn_client_tree_t *tree,
                   svn_node_kind_t *kind,
                   const char *relpath,
                   apr_pool_t *scratch_pool)
{
  disk_tree_baton_t *baton = tree->priv;
  const char *abspath = svn_dirent_join(baton->tree_abspath, relpath,
                                        scratch_pool);

  SVN_ERR(svn_io_check_path(abspath, kind, scratch_pool));
  return SVN_NO_ERROR;
}

/* */
static svn_error_t *
disk_tree_get_file(svn_client_tree_t *tree,
                   svn_stream_t **stream,
                   apr_hash_t **props,
                   const char *relpath,
                   apr_pool_t *result_pool,
                   apr_pool_t *scratch_pool)
{
  disk_tree_baton_t *baton = tree->priv;
  const char *abspath = svn_dirent_join(baton->tree_abspath, relpath,
                                        scratch_pool);

  if (stream)
    SVN_ERR(svn_stream_open_readonly(stream, abspath,
                                     result_pool, scratch_pool));
  if (props)
    *props = NULL;

  return SVN_NO_ERROR;
}

/* */
static svn_error_t *
disk_tree_get_dir(svn_client_tree_t *tree,
                  apr_hash_t **dirents,
                  apr_hash_t **props,
                  const char *relpath,
                  apr_pool_t *result_pool,
                  apr_pool_t *scratch_pool)
{
  disk_tree_baton_t *baton = tree->priv;
  const char *abspath = svn_dirent_join(baton->tree_abspath, relpath,
                                        scratch_pool);

  if (dirents)
    {
      SVN_ERR(svn_io_get_dirents3(dirents, abspath, FALSE,
                                  result_pool, scratch_pool));
    }
  if (props)
    *props = NULL;

  return SVN_NO_ERROR;
}

/* */
static const svn_client_tree__vtable_t disk_tree_vtable =
{
  disk_tree_get_kind,
  disk_tree_get_file,
  disk_tree_get_dir
};

svn_error_t *
svn_client__disk_tree(svn_client_tree_t **tree_p,
                      const char *abspath,
                      svn_delta_editor_t *editor,
                      apr_pool_t *result_pool)
{
  svn_client_tree_t *tree = apr_palloc(result_pool, sizeof(*tree));
  disk_tree_baton_t *baton = apr_palloc(result_pool, sizeof(*baton));

  baton->tree_abspath = abspath;

  tree->vtable = &disk_tree_vtable;
  tree->pool = result_pool;
  tree->priv = baton;

  *tree_p = tree;
  return SVN_NO_ERROR;
}

/*-----------------------------------------------------------------*/


svn_error_t *
svn_client__wc_base_tree(svn_client_tree_t **tree_p,
                         const char *path,
                         svn_delta_editor_t *editor,
                         apr_pool_t *result_pool)
{
  svn_client_tree_t *tree = apr_pcalloc(result_pool, sizeof(*tree));

  *tree_p = tree;
  return SVN_NO_ERROR;
}

svn_error_t *
svn_client__wc_working_tree(svn_client_tree_t **tree_p,
                            const char *path,
                            svn_delta_editor_t *editor,
                            apr_pool_t *result_pool)
{
  svn_client_tree_t *tree = apr_pcalloc(result_pool, sizeof(*tree));

  *tree_p = tree;
  return SVN_NO_ERROR;
}

/*-----------------------------------------------------------------*/


/* */
typedef struct ra_tree_baton_t
{
  svn_ra_session_t *ra_session;
  svn_revnum_t revnum;
} ra_tree_baton_t;

/* */
static svn_error_t *
ra_tree_get_kind(svn_client_tree_t *tree,
                 svn_node_kind_t *kind,
                 const char *relpath,
                 apr_pool_t *scratch_pool)
{
  ra_tree_baton_t *baton = tree->priv;

  SVN_ERR(svn_ra_check_path(baton->ra_session, relpath, baton->revnum,
                            kind, scratch_pool));
  return SVN_NO_ERROR;
}

/* */
static svn_error_t *
ra_tree_get_file(svn_client_tree_t *tree,
                 svn_stream_t **stream,
                 apr_hash_t **props,
                 const char *relpath,
                 apr_pool_t *result_pool,
                 apr_pool_t *scratch_pool)
{
  ra_tree_baton_t *baton = tree->priv;
  svn_stream_t *holding_stream;
  
  SVN_ERR(svn_stream_open_unique(&holding_stream, NULL, NULL,
                                 svn_io_file_del_on_close,
                                 scratch_pool, scratch_pool));
  SVN_ERR(svn_ra_get_file(baton->ra_session, relpath, baton->revnum,
                          holding_stream, NULL, props, result_pool));
  SVN_ERR(svn_stream_reset(holding_stream));
  *stream = holding_stream;
  return SVN_NO_ERROR;
}

/* */
static svn_error_t *
ra_tree_get_dir(svn_client_tree_t *tree,
                apr_hash_t **dirents,
                apr_hash_t **props,
                const char *relpath,
                apr_pool_t *result_pool,
                apr_pool_t *scratch_pool)
{
  ra_tree_baton_t *baton = tree->priv;
  apr_hash_t *ra_dirents = NULL;

  SVN_ERR(svn_ra_get_dir2(baton->ra_session,
                          dirents ? &ra_dirents : NULL, NULL, props,
                          relpath, baton->revnum,
                          SVN_DIRENT_KIND | SVN_DIRENT_SIZE,
                          result_pool));
  if (ra_dirents)
    {
      apr_hash_index_t *hi;

      *dirents = apr_hash_make(result_pool);
      for (hi = apr_hash_first(scratch_pool, ra_dirents);
           hi;
           hi = apr_hash_next(hi))
        {
          const char *entry_name = svn__apr_hash_index_key(hi);
          svn_dirent_t *ra_dirent = svn__apr_hash_index_val(hi);
          svn_client_tree_dirent_t *dirent
            = apr_palloc(result_pool, sizeof(*dirent));

          dirent->kind = ra_dirent->kind;
          dirent->filesize = ra_dirent->size;
          /* ### dirent->special = ... */
          apr_hash_set(*dirents, entry_name, APR_HASH_KEY_STRING, dirent);
        }
    }
  return SVN_NO_ERROR;
}

/* */
static const svn_client_tree__vtable_t ra_tree_vtable =
{
  ra_tree_get_kind,
  ra_tree_get_file,
  ra_tree_get_dir
};

/* */
static svn_error_t *
read_ra_tree(svn_client_tree_t **tree_p,
             svn_ra_session_t *ra_session,
             svn_revnum_t revnum,
             svn_client_ctx_t *ctx,
             apr_pool_t *result_pool)
{
  svn_client_tree_t *tree = apr_pcalloc(result_pool, sizeof(*tree));
  ra_tree_baton_t *baton = apr_palloc(result_pool, sizeof(*baton));

  baton->ra_session = ra_session;
  baton->revnum = revnum;

  tree->vtable = &ra_tree_vtable;
  tree->pool = result_pool;
  tree->priv = baton;

  *tree_p = tree;
  return SVN_NO_ERROR;
}

svn_error_t *
svn_client__repository_tree(svn_client_tree_t **tree_p,
                            const char *path_or_url,
                            const svn_opt_revision_t *peg_revision,
                            const svn_opt_revision_t *revision,
                            svn_client_ctx_t *ctx,
                            apr_pool_t *result_pool)
{
  svn_ra_session_t *ra_session;
  svn_revnum_t revnum;
  const char *url;

  /* Get the RA connection. */
  SVN_ERR(svn_client__ra_session_from_path(&ra_session, &revnum,
                                           &url, path_or_url, NULL,
                                           peg_revision, revision,
                                           ctx, result_pool));

  SVN_ERR(read_ra_tree(tree_p, ra_session, revnum,
                       ctx, result_pool));

  return SVN_NO_ERROR;
}

/*-----------------------------------------------------------------*/
