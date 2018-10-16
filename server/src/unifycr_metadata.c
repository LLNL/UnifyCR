/*
 * Copyright (c) 2017, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 *
 * Copyright 2017-2018, UT-Battelle, LLC.
 *
 * LLNL-CODE-741539
 * All rights reserved.
 *
 * This is the license for UnifyCR.
 * For details, see https://github.com/LLNL/UnifyCR.
 * Please read https://github.com/LLNL/UnifyCR/LICENSE for full license text.
 */

/*
 * Copyright (c) 2017, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * Copyright (c) 2017, Florida State University. Contributions from
 * the Computer Architecture and Systems Research Laboratory (CASTL)
 * at the Department of Computer Science.
 *
 * Written by: Teng Wang, Adam Moody, Wekuan Yu, Kento Sato, Kathryn Mohror
 * LLNL-CODE-728877. All rights reserved.
 *
 * This file is part of burstfs.
 * For details, see https://github.com/llnl/burstfs
 * Please read https://github.com/llnl/burstfs/LICNSE for full license text.
 */

#include "mdhim.h"
#include "indexes.h"
#include "log.h"
#include "unifycr_meta.h"
#include "unifycr_metadata.h"
#include "arraylist.h"
#include "unifycr_const.h"
#include "unifycr_global.h"

//unifycr_key_t **unifycr_keys;
//unifycr_val_t **unifycr_vals;

//fattr_key_t **fattr_keys;
//fattr_val_t **fattr_vals;

char *manifest_path;

struct mdhim_brm_t *brm, *brmp;
struct mdhim_bgetrm_t *bgrm, *bgrmp;

mdhim_options_t *db_opts;
struct mdhim_t *md;

int md_size;
int unifycr_key_lens[MAX_META_PER_SEND] = {0};
int unifycr_val_lens[MAX_META_PER_SEND] = {0};

int fattr_key_lens[MAX_FILE_CNT_PER_NODE] = {0};
int fattr_val_lens[MAX_FILE_CNT_PER_NODE] = {0};

struct index_t *unifycr_indexes[2];
size_t max_recs_per_slice;

void debug_log_key_val(const char* ctx,
                       unifycr_key_t *key,
                       unifycr_val_t *val)
{
    if ((key != NULL) && (val != NULL)) {
        LOG(LOG_DBG,
            "@%s - key(fid=%lu, offset=%lu), "
            "val(del=%d, len=%lu, addr=%lu, app=%d, rank=%d)\n",
            ctx, key->fid, key->offset,
            val->delegator_id, val->len, val->addr, val->app_id, val->rank);
    } else if (key != NULL) {
        LOG(LOG_DBG,
            "@%s - key(fid=%lu, offset=%lu)\n",
            ctx, key->fid, key->offset);
    }
}

void debug_log_client_req(const char* ctx,
                          shm_meta_t *req)
{
    if (req != NULL) {
        LOG(LOG_DBG,
            "@%s - req(fid=%d, offset=%zu, length=%zu)\n",
            ctx, req->src_fid, req->offset, req->length);
    }
}

int unifycr_key_compare(unifycr_key_t *a, unifycr_key_t *b)
{
    assert((NULL != a) && (NULL != b));
    if (a->fid == b->fid) {
        if (a->offset == b->offset)
            return 0;
        else if (a->offset < b->offset)
            return -1;
        else
            return 1;
    } else if (a->fid < b->fid)
        return -1;
    else
        return 1;
}

/**
* initialize the key-value store
*/
int meta_init_store(unifycr_cfg_t *cfg)
{
    int rc, ser_ratio;
    size_t path_len;
    long svr_ratio, range_sz;
    MPI_Comm comm = MPI_COMM_WORLD;

    if (cfg == NULL)
        return -1;

    db_opts = calloc(1, sizeof(struct mdhim_options_t));
    if (db_opts == NULL)
        return -1;

    /* UNIFYCR_META_DB_PATH: file that stores the key value pair*/
    db_opts->db_path = strdup(cfg->meta_db_path);
    if (db_opts->db_path == NULL)
        return -1;

    db_opts->manifest_path = NULL;
    db_opts->db_type = LEVELDB;
    db_opts->db_create_new = 1;

    /* META_SERVER_RATIO: number of metadata servers =
        number of processes/META_SERVER_RATIO */
    svr_ratio = 0;
    rc = configurator_int_val(cfg->meta_server_ratio, &svr_ratio);
    if (rc != 0)
        return -1;
    ser_ratio = (int) svr_ratio;
    db_opts->rserver_factor = ser_ratio;

    db_opts->db_paths = NULL;
    db_opts->num_paths = 0;
    db_opts->num_wthreads = 1;

    path_len = strlen(db_opts->db_path) + strlen(MANIFEST_FILE_NAME) + 1;
    manifest_path = malloc(path_len);
    if (manifest_path == NULL)
        return -1;
    sprintf(manifest_path, "%s/%s", db_opts->db_path, MANIFEST_FILE_NAME);
    db_opts->manifest_path = manifest_path;

    db_opts->db_name = strdup(cfg->meta_db_name);
    if (db_opts->db_name == NULL)
        return -1;

    db_opts->db_key_type = MDHIM_UNIFYCR_KEY;
    db_opts->debug_level = MLOG_CRIT;

    /* indices/attributes are striped to servers according
     * to UnifyCR_META_RANGE_SIZE.
     */
    range_sz = 0;
    rc = configurator_int_val(cfg->meta_range_size, &range_sz);
    if (rc != 0)
        return -1;
    max_recs_per_slice = (size_t) range_sz;
    db_opts->max_recs_per_slice = (uint64_t) range_sz;

    md = mdhimInit(&comm, db_opts);

    /*this index is created for storing index metadata*/
    unifycr_indexes[0] = md->primary_index;

    /*this index is created for storing file attribute metadata*/
    unifycr_indexes[1] = create_global_index(md, ser_ratio, 1,
                         LEVELDB, MDHIM_INT_KEY, "file_attr");

    MPI_Comm_size(md->mdhim_comm, &md_size);

#if 0
    rc = meta_init_indices();
    if (rc != 0)
        return -1;
#endif
    return 0;

}

#if 0
/**
* initialize the key and value list used to
* put/get key-value pairs
* ToDo: split once the number of metadata exceeds MAX_META_PER_SEND
*/
int meta_init_indices()
{

    int i;

    /*init index metadata*/
    unifycr_keys = (unifycr_key_t **)malloc(MAX_META_PER_SEND
                                            * sizeof(unifycr_key_t *));

    unifycr_vals = (unifycr_val_t **)malloc(MAX_META_PER_SEND
                                            * sizeof(unifycr_val_t *));

    for (i = 0; i < MAX_META_PER_SEND; i++) {
        unifycr_keys[i] = (unifycr_key_t *)malloc(sizeof(unifycr_key_t));
        if (unifycr_keys[i] == NULL)
            return (int)UNIFYCR_ERROR_NOMEM;
        memset(unifycr_keys[i], 0, sizeof(unifycr_key_t));
    }

    for (i = 0; i < MAX_META_PER_SEND; i++) {
        unifycr_vals[i] = (unifycr_val_t *)malloc(sizeof(unifycr_val_t));
        if (unifycr_vals[i] == NULL)
            return (int)UNIFYCR_ERROR_NOMEM;
        memset(unifycr_vals[i], 0, sizeof(unifycr_val_t));
    }

    /*init attribute metadata*/
    fattr_keys = (fattr_key_t **)malloc(MAX_FILE_CNT_PER_NODE
                                        * sizeof(fattr_key_t *));

    fattr_vals = (fattr_val_t **)malloc(MAX_FILE_CNT_PER_NODE
                                        * sizeof(fattr_val_t *));

    for (i = 0; i < MAX_FILE_CNT_PER_NODE; i++) {
        fattr_keys[i] = (fattr_key_t *)malloc(sizeof(fattr_key_t));
        if (fattr_keys[i] == NULL)
            return (int)UNIFYCR_ERROR_NOMEM;
        memset(fattr_keys[i], 0, sizeof(fattr_key_t));
    }

    for (i = 0; i < MAX_FILE_CNT_PER_NODE; i++) {
        fattr_vals[i] = (fattr_val_t *)malloc(sizeof(fattr_val_t));
        if (fattr_vals[i] == NULL)
            return (int)UNIFYCR_ERROR_NOMEM;
        memset(fattr_vals[i], 0, sizeof(fattr_val_t));
    }

    return 0;

}
#endif

/*synchronize all the indices and file attributes
* to the key-value store
* @param sock_id: the connection id in poll_set of the delegator
* @return success/error code
*/

int meta_process_fsync(int sock_id)
{
    int ret = 0;

    int app_id = invert_sock_ids[sock_id];
    app_config_t *app_config = (app_config_t *)arraylist_get(app_config_list,
                               app_id);

    int client_side_id = app_config->client_ranks[sock_id];

    size_t num_entries =
        *((size_t *)(app_config->shm_superblocks[client_side_id]
                     + app_config->meta_offset));

    /* indices are stored in the superblock shared memory
     *  created by the client*/
    int page_sz = getpagesize();
    unifycr_index_t *meta_payload =
        (unifycr_index_t *)(app_config->shm_superblocks[client_side_id]
                            + app_config->meta_offset + page_sz);

    md->primary_index = unifycr_indexes[0];

    long i;
    for (i = 0; i < num_entries; i++) {
        unifycr_keys[i]->fid = meta_payload[i].fid;
        unifycr_keys[i]->offset = meta_payload[i].file_pos;

        unifycr_vals[i]->addr = meta_payload[i].mem_pos;
        unifycr_vals[i]->len = meta_payload[i].length;
        unifycr_vals[i]->delegator_id = glb_rank;
        unifycr_vals[i]->app_id = app_id;
        unifycr_vals[i]->rank = client_side_id;

        // debug_log_key_val("before put", unifycr_keys[i], unifycr_vals[i]);

        unifycr_key_lens[i] = sizeof(unifycr_key_t);
        unifycr_val_lens[i] = sizeof(unifycr_val_t);
    }

    // print_fsync_indices(unifycr_keys, unifycr_vals, num_entries);

    brm = mdhimBPut(md, (void **)(&unifycr_keys[0]), unifycr_key_lens,
                    (void **)(&unifycr_vals[0]), unifycr_val_lens, num_entries,
                    NULL, NULL);
    brmp = brm;
    if (!brmp || brmp->error) {
        ret = (int)UNIFYCR_ERROR_MDHIM;
        LOG(LOG_DBG, "Rank - %d: Error inserting keys/values into MDHIM\n",
            md->mdhim_rank);
    }

    while (brmp) {
        if (brmp->error < 0) {
            ret = (int)UNIFYCR_ERROR_MDHIM;
            break;
        }

        brm = brmp;
        brmp = brmp->next;
        mdhim_full_release_msg(brm);

    }

    md->primary_index = unifycr_indexes[1];

    num_entries =
        *((size_t *)(app_config->shm_superblocks[client_side_id]
                     + app_config->fmeta_offset));

    /* file attributes are stored in the superblock shared memory
     * created by the client*/
    unifycr_file_attr_t *attr_payload =
        (unifycr_file_attr_t *)(app_config->shm_superblocks[client_side_id]
                                + app_config->fmeta_offset + page_sz);


    for (i = 0; i < num_entries; i++) {
        *fattr_keys[i] = attr_payload[i].gfid;
        fattr_vals[i]->file_attr = attr_payload[i].file_attr;
        strcpy(fattr_vals[i]->fname, attr_payload[i].filename);

        fattr_key_lens[i] = sizeof(fattr_key_t);
        fattr_val_lens[i] = sizeof(fattr_val_t);
    }

    brm = mdhimBPut(md, (void **)(&fattr_keys[0]), fattr_key_lens,
                    (void **)(&fattr_vals[0]), fattr_val_lens, num_entries,
                    NULL, NULL);
    brmp = brm;
    if (!brmp || brmp->error) {
        ret = (int)UNIFYCR_ERROR_MDHIM;
        LOG(LOG_DBG, "Rank - %d: Error inserting keys/values into MDHIM\n",
            md->mdhim_rank);
    }

    while (brmp) {
        if (brmp->error < 0) {
            ret = (int)UNIFYCR_ERROR_MDHIM;
            break;
        }

        brm = brmp;
        brmp = brmp->next;
        mdhim_full_release_msg(brm);

    }

    return ret;
}




/* get the locations of all the requested file segments from
 * the key-value store.
* @param app_id: client's application id
* @param client_id: client-side process id
* @param del_req_set: the set of read requests to be
* @param thrd_id: the thread created for processing
* its client's read requests.
* @param dbg_rank: the client process's rank in its
* own application, used for debug purpose
* @param shm_reqbuf: shmem buffer containing client's read requests
* @del_req_set: contains metadata information for all
* the read requests, such as the locations of the
* requested segments
* @return success/error code
*/
int meta_batch_get(int app_id, int client_id, int thrd_id, int dbg_rank,
                   shm_meta_t *meta_reqs, size_t req_cnt,
                   msg_meta_t *del_req_set)
{
    // LOG(LOG_DBG, "meta_batch_get - req_cnt=%zu)\n", req_cnt);

    shm_meta_t *req;
    size_t i, ndx;
    for (i = 0; i < req_cnt; i++) {
        ndx = 2 * i;
        req = meta_reqs + i;
        unifycr_keys[ndx]->fid = req->src_fid;
        unifycr_keys[ndx]->offset = req->offset;
        unifycr_keys[ndx + 1]->fid = req->src_fid;
        unifycr_keys[ndx + 1]->offset = req->offset + req->length - 1;
        unifycr_key_lens[ndx] = sizeof(unifycr_key_t);
        unifycr_key_lens[ndx + 1] = sizeof(unifycr_key_t);
        // debug_log_client_req("before get", req);
    }

    md->primary_index = unifycr_indexes[0];
    bgrm = mdhimBGet(md, md->primary_index, (void **)unifycr_keys,
                     unifycr_key_lens, 2 * req_cnt, MDHIM_RANGE_BGET);

    int rc = 0;
    int tot_num = 0;
    unifycr_key_t *tmp_key;
    unifycr_val_t *tmp_val;
    send_msg_t *tmp_msg;

    while (bgrm) {
        bgrmp = bgrm;
        if (bgrmp->error < 0)
            rc = (int)UNIFYCR_ERROR_MDHIM;

        for (i = 0; i < bgrmp->num_keys; i++) {
            tmp_key = (unifycr_key_t *)bgrmp->keys[i];
            tmp_val = (unifycr_val_t *)bgrmp->values[i];
            // debug_log_key_val("after get", tmp_key, tmp_val);

            tmp_msg = &(del_req_set->msg_meta[tot_num]);
            memset(tmp_msg, 0, sizeof(send_msg_t));
            tot_num++;

            /* physical offset of the requested file segment on the log file */
            tmp_msg->dest_offset = (off_t) tmp_val->addr;

            /* rank of the remote delegator */
            tmp_msg->dest_delegator_rank = tmp_val->delegator_id;

            /* dest_client_id and dest_app_id uniquely identify the remote
             * physical log file that contains the requested segments */
            tmp_msg->dest_client_id = tmp_val->rank;
            tmp_msg->dest_app_id = tmp_val->app_id;
            tmp_msg->length = (size_t) tmp_val->len;

            /* src_app_id and src_cli_id identifies the requested client */
            tmp_msg->src_app_id = app_id;
            tmp_msg->src_cli_id = client_id;

            /* src_offset is the logical offset of the shared file */
            tmp_msg->src_offset = (off_t) tmp_key->offset;
            tmp_msg->src_delegator_rank = glb_rank;
            tmp_msg->src_fid = (int) tmp_key->fid;
            tmp_msg->src_dbg_rank = dbg_rank;
            tmp_msg->src_thrd = thrd_id;
        }
        bgrm = bgrmp->next;
        mdhim_full_release_msg(bgrmp);
    }

    del_req_set->num = tot_num;
//    print_bget_indices(app_id, client_id, del_req_set->msg_meta, tot_num);

    return rc;
}

void print_bget_indices(int app_id, int cli_id,
                        send_msg_t *index_set, int tot_num)
{
    int i;

    long dest_offset;
    int dest_delegator_rank;
    int dest_client_id;
    int dest_app_id;
    long length;
    int src_app_id;
    int src_cli_id;
    long src_offset;
    int src_delegator_rank;
    int src_fid;
    int dbg_rank;

    for (i = 0; i < tot_num;  i++) {
        dest_offset = index_set[i].dest_offset;
        dest_delegator_rank = index_set[i].dest_delegator_rank;
        dest_client_id = index_set[i].dest_client_id;
        dest_app_id = index_set[i].dest_app_id;
        length = index_set[i].length;
        src_app_id = index_set[i].src_app_id;
        src_cli_id = index_set[i].src_cli_id;
        src_offset = index_set[i].src_offset;

        src_delegator_rank = index_set[i].src_delegator_rank;
        src_fid = index_set[i].src_fid;
        dbg_rank = index_set[i].src_dbg_rank;

        LOG(LOG_DBG, "index:dbg_rank:%d, dest_offset:%ld, "
            "dest_del_rank:%d, dest_cli_id:%d, dest_app_id:%d, "
            "length:%ld, src_app_id:%d, src_cli_id:%d, src_offset:%ld, "
            "src_del_rank:%d, "
            "src_fid:%d, num:%d\n", dbg_rank, dest_offset,
            dest_delegator_rank, dest_client_id,
            dest_app_id, length, src_app_id, src_cli_id,
            src_offset, src_delegator_rank,
            src_fid, tot_num);

    }


}

void print_fsync_indices(unifycr_key_t **unifycr_keys,
                         unifycr_val_t **unifycr_vals, long num_entries)
{
    long i;
    for (i = 0; i < num_entries; i++) {
        LOG(LOG_DBG, "fid:%lu, offset:%lu, addr:%lu, len:%lu, del_id:%d\n",
            unifycr_keys[i]->fid, unifycr_keys[i]->offset,
            unifycr_vals[i]->addr, unifycr_vals[i]->len,
            unifycr_vals[i]->delegator_id);

    }
}

int meta_finalize(void)
{
    int rc = ULFS_SUCCESS;

    char dbfilename[UNIFYCR_MAX_FILENAME] = {0};
    char statfilename[UNIFYCR_MAX_FILENAME] = {0};
    char manifestname[UNIFYCR_MAX_FILENAME] = {0};

    char dbfilename1[UNIFYCR_MAX_FILENAME] = {0};
    char statfilename1[UNIFYCR_MAX_FILENAME] = {0};
    char manifestname1[UNIFYCR_MAX_FILENAME] = {0};
    sprintf(dbfilename, "%s/%s-%d-%d", md->db_opts->db_path,
            md->db_opts->db_name, unifycr_indexes[0]->id, md->mdhim_rank);

    sprintf(statfilename, "%s_stats", dbfilename);
    sprintf(manifestname, "%s%d_%d_%d", md->db_opts->manifest_path,
            unifycr_indexes[0]->type,
            unifycr_indexes[0]->id, md->mdhim_rank);

    sprintf(dbfilename1, "%s/%s-%d-%d", md->db_opts->db_path,
            md->db_opts->db_name, unifycr_indexes[1]->id, md->mdhim_rank);

    sprintf(statfilename1, "%s_stats", dbfilename1);
    sprintf(manifestname1, "%s%d_%d_%d", md->db_opts->manifest_path,
            unifycr_indexes[1]->type,
            unifycr_indexes[1]->id, md->mdhim_rank);

    mdhimClose(md);
    rc = mdhimSanitize(dbfilename, statfilename, manifestname);
    rc = mdhimSanitize(dbfilename1, statfilename1, manifestname1);

    mdhim_options_destroy(db_opts);
    return rc;
}

/*
 *
 */
int unifycr_set_file_attribute(unifycr_file_attr_t *fattr_ptr)
{
    int rc = UNIFYCR_SUCCESS;

    int gfid = fattr_ptr->gfid;

    md->primary_index = unifycr_indexes[1];
    brm = mdhimPut(md, &gfid, sizeof(int),
                   fattr_ptr, sizeof(unifycr_file_attr_t),
                   NULL, NULL);
    if (!brm || brm->error) {
        // return UNIFYCR_ERROR_MDHIM on error
        rc = (int)UNIFYCR_ERROR_MDHIM;
    }

    mdhim_full_release_msg(brm);

    return rc;
}

/*
 *
 */
int unifycr_set_file_attributes(int num_entries, fattr_key_t *keys,
                                int *key_lens,
                                unifycr_file_attr_t *fattr_ptr, int *val_lens)
{
    int rc = UNIFYCR_SUCCESS;

    md->primary_index = unifycr_indexes[1];
    brm = mdhimBPut(md, (void **)&keys[0], key_lens, (void **)&fattr_ptr[0],
                    val_lens, num_entries, NULL, NULL);
    brmp = brm;
    if (!brmp || brmp->error) {
        rc = (int)UNIFYCR_ERROR_MDHIM;
        LOG(LOG_DBG, "Rank - %d: Error inserting keys/values into MDHIM\n",
            md->mdhim_rank);
    }

    while (brmp) {
        if (brmp->error < 0) {
            rc = (int)UNIFYCR_ERROR_MDHIM;
            break;
        }

        brm = brmp;
        brmp = brmp->next;
        mdhim_full_release_msg(brm);

    }

    return rc;
}

/*
 *
 */
int unifycr_get_file_attribute(int gfid,
                               unifycr_file_attr_t *attr_val_ptr)
{
    int rc = UNIFYCR_SUCCESS;
    unifycr_file_attr_t *tmp_ptr_attr;

    md->primary_index = unifycr_indexes[1];
    bgrm = mdhimGet(md, md->primary_index, &gfid,
                    sizeof(int), MDHIM_GET_EQ);

    if (!bgrm || bgrm->error)
        rc = (int)UNIFYCR_ERROR_MDHIM;
    else {
        tmp_ptr_attr = (unifycr_file_attr_t *)bgrm->values[0];

        attr_val_ptr->file_attr = tmp_ptr_attr->file_attr;
        attr_val_ptr->fid = tmp_ptr_attr->fid;
        attr_val_ptr->gfid = tmp_ptr_attr->gfid;
        strcpy(attr_val_ptr->filename, tmp_ptr_attr->filename);
    }

    mdhim_full_release_msg(bgrm);
    return rc;
}

/*
 *
 */
int unifycr_get_file_extents(int num_keys, unifycr_key_t *keys,
                             int *unifycr_key_lens, int *num_values,
                             unifycr_keyval_t **keyval)
{
    /*
     * This is using a modified version of mdhim. The function will return all
     * key-value pairs within the range of the key tuple.
     * We need to re-evaluate this function to use different key-value stores.
     */

    int rc = UNIFYCR_SUCCESS;
    int i;

    md->primary_index = unifycr_indexes[0];
    bgrm = mdhimBGet(md, md->primary_index, (void **)keys,
                     unifycr_key_lens, num_keys, MDHIM_RANGE_BGET);

    int tot_num = 0;

    unifycr_key_t *tmp_key;
    unifycr_val_t *tmp_val;

    bgrmp = bgrm;
    while (bgrmp) {
        if (bgrmp->error < 0)
            rc = (int)UNIFYCR_ERROR_MDHIM;

        // allocate memory for values
        *keyval = calloc(bgrmp->num_keys, sizeof(unifycr_keyval_t));

        for (i = 0; i < bgrmp->num_keys; i++) {
            tmp_key = (unifycr_key_t *)bgrm->keys[i];
            tmp_val = (unifycr_val_t *)bgrm->values[i];
            (*keyval)[i].key.fid = tmp_key->fid;
            (*keyval)[i].key.offset = tmp_key->offset;

            (*keyval)[i].val.addr = tmp_val->addr;
            (*keyval)[i].val.app_rank_id = tmp_val->app_rank_id;
            (*keyval)[i].val.delegator_id = tmp_val->delegator_id;
            (*keyval)[i].val.addr = tmp_val->len;
        }
        bgrmp = bgrmp->next;
        mdhim_full_release_msg(bgrm);
        bgrm = bgrmp;
        tot_num++;
    }

    *num_values = tot_num;

    return rc;
}

/*
 *
 */
int unifycr_set_file_extents(int num_entries, unifycr_key_t *keys,
                             int *unifycr_key_lens, unifycr_val_t *vals,
                             int *unifycr_val_lens)
{
    int rc = UNIFYCR_SUCCESS;

    md->primary_index = unifycr_indexes[0];

    brm = mdhimBPut(md, (void **)(&keys[0]), unifycr_key_lens,
                    (void **)(&vals[0]), unifycr_val_lens, num_entries,
                    NULL, NULL);
    brmp = brm;
    if (!brmp || brmp->error) {
        rc = (int)UNIFYCR_ERROR_MDHIM;
        LOG(LOG_DBG, "Rank - %d: Error inserting keys/values into MDHIM\n",
            md->mdhim_rank);
    }

    while (brmp) {
        if (brmp->error < 0) {
            rc = (int)UNIFYCR_ERROR_MDHIM;
            break;
        }

        brm = brmp;
        brmp = brmp->next;
        mdhim_full_release_msg(brm);
    }

    return rc;
}
