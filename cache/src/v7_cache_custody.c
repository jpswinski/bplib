/*
 * NASA Docket No. GSC-18,587-1 and identified as “The Bundle Protocol Core Flight
 * System Application (BP) v6.5”
 *
 * Copyright © 2020 United States Government as represented by the Administrator of
 * the National Aeronautics and Space Administration. All Rights Reserved.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "v7_cache_internal.h"
#include "crc.h"

static const bplib_crc_parameters_t *const BPLIB_CACHE_CUSTODY_HASH_ALGORITHM = &BPLIB_CRC32_CASTAGNOLI;

static const uint32_t BPLIB_CACHE_CUSTODY_HASH_SALT_DACS   = 0x3126c0cf;
static const uint32_t BPLIB_CACHE_CUSTODY_HASH_SALT_BUNDLE = 0x7739ae76;

void bplib_cache_custody_insert_tracking_block(bplib_cache_state_t *state, bplib_mpool_bblock_primary_t *pri_block,
                                               bplib_cache_custodian_info_t *custody_info)
{
    bplib_mpool_bblock_canonical_t *custody_block;

    custody_info->cblk = bplib_mpool_bblock_canonical_alloc(bplib_cache_parent_pool(state));
    if (custody_info->cblk != NULL)
    {
        bplib_mpool_bblock_primary_append(pri_block, custody_info->cblk);

        custody_block = bplib_mpool_bblock_canonical_cast(custody_info->cblk);
        if (custody_block != NULL)
        {
            custody_block->canonical_logical_data.canonical_block.blockType = bp_blocktype_custodyTrackingBlock;
            custody_block->canonical_logical_data.canonical_block.blockNum  = bp_blocktype_custodyTrackingBlock;
            custody_block->canonical_logical_data.canonical_block.crctype   = pri_block->pri_logical_data.crctype;
        }
    }
}

int bplib_cache_custody_find_dacs_match(void *arg, bplib_mpool_block_t *lblk)
{
    bplib_cache_entry_t          *store_entry;
    bplib_cache_dacs_pending_t   *dacs_pending;
    bplib_cache_custodian_info_t *custody_info;
    int                           result;

    custody_info = arg;

    /* possible match, but need to verify */
    store_entry = bplib_mpool_generic_data_cast(lblk, BPLIB_STORE_SIGNATURE_ENTRY);
    if (store_entry != NULL && store_entry->state == bplib_cache_entry_state_generate_dacs)
    {
        dacs_pending = &store_entry->data.dacs;

        /* confirm match of custodian ID */
        result = v7_compare_ipn2ipn(&dacs_pending->prev_custodian_id, &custody_info->custodian_id);
        if (result == 0)
        {
            /* confirm match of the flow ID */
            result = v7_compare_ipn2eid(&custody_info->flow_id, &dacs_pending->payload_ref->flow_source_eid);
            if (result == 0)
            {
                /* its a complete match */
                custody_info->store_entry = store_entry;
            }
        }
    }
    else
    {
        result = -1;
    }

    return result;
}

bool bplib_cache_custody_find_pending_dacs(bplib_cache_state_t *state, bplib_cache_custodian_info_t *dacs_info)
{
    bplib_rbt_link_t    *custody_rbt_link;
    bplib_cache_queue_t *store_queue;
    bplib_mpool_block_t *sblk;
    bp_crcval_t          hash;

    /* use a CRC as a hash function */
    /* when searching for DACS this includes flow and custodian but NOT sequence number (which has multiple values) */
    hash = bplib_crc_initial_value(BPLIB_CACHE_CUSTODY_HASH_ALGORITHM);
    hash = bplib_crc_update(BPLIB_CACHE_CUSTODY_HASH_ALGORITHM, hash, &dacs_info->flow_id, sizeof(dacs_info->flow_id));
    hash = bplib_crc_update(BPLIB_CACHE_CUSTODY_HASH_ALGORITHM, hash, &dacs_info->custodian_id,
                            sizeof(dacs_info->custodian_id));
    hash = bplib_crc_update(BPLIB_CACHE_CUSTODY_HASH_ALGORITHM, hash, &BPLIB_CACHE_CUSTODY_HASH_SALT_DACS,
                            sizeof(BPLIB_CACHE_CUSTODY_HASH_SALT_DACS));
    dacs_info->eid_hash = bplib_crc_finalize(BPLIB_CACHE_CUSTODY_HASH_ALGORITHM, hash);

    custody_rbt_link = bplib_rbt_search(dacs_info->eid_hash, &state->hash_index);
    if (custody_rbt_link != NULL)
    {
        store_queue = bplib_cache_queue_from_rbt_link(custody_rbt_link);

        /*
         * to handle possible hash collision/overlap, there is a list at this entry,
         * which needs to be searched.  Collisions should be unlikely enough that
         * the lists, if they ever become more than one entry, stay short enough
         * such that sequential search is not a burden.
         */
        sblk = bplib_mpool_search_list(&store_queue->bundle_list, bplib_cache_custody_find_dacs_match, dacs_info);
    }
    else
    {
        sblk = NULL;
    }

    return (sblk != NULL);
}

void bplib_cache_custody_init_info_from_pblock(bplib_cache_custodian_info_t *custody_info,
                                               bplib_mpool_bblock_primary_t *pri_block)
{
    bplib_mpool_bblock_canonical_t *custody_block;
    bp_ipn_addr_t                   final_dest_addr;

    memset(custody_info, 0, sizeof(*custody_info));

    v7_get_eid(&custody_info->flow_id, &pri_block->pri_logical_data.sourceEID);
    custody_info->sequence_num = pri_block->pri_logical_data.creationTimeStamp.sequence_num;

    v7_get_eid(&final_dest_addr, &pri_block->pri_logical_data.destinationEID);
    custody_info->final_dest_node = final_dest_addr.node_number;

    custody_info->cblk = bplib_mpool_bblock_primary_locate_canonical(pri_block, bp_blocktype_custodyTrackingBlock);
    if (custody_info->cblk != NULL)
    {
        custody_block = bplib_mpool_bblock_canonical_cast(custody_info->cblk);
        if (custody_block != NULL)
        {
            /* need to generate a DACS back to the previous custodian indicated in the custody block */
            v7_get_eid(&custody_info->custodian_id,
                       &custody_block->canonical_logical_data.data.custody_tracking_block.current_custodian);
        }
    }
}

bplib_mpool_ref_t bplib_cache_custody_create_dacs(bplib_cache_state_t                *state,
                                                  bplib_mpool_bblock_primary_t      **pri_block_out,
                                                  bp_custody_accept_payload_block_t **pay_out)
{
    int                  status;
    bplib_mpool_block_t *pblk;
    bplib_mpool_block_t *cblk;
    bplib_mpool_t       *ppool;

    bplib_mpool_bblock_primary_t   *pri_block;
    bp_primary_block_t             *pri;
    bplib_mpool_bblock_canonical_t *c_block;
    bp_canonical_block_buffer_t    *pay;

    /* This needs to turn the DACS information in the temporary object into a full bundle */
    /* Allocate Blocks */
    cblk   = NULL;
    pblk   = NULL;
    status = BP_ERROR;
    ppool  = bplib_cache_parent_pool(state);

    do
    {
        pblk      = bplib_mpool_bblock_primary_alloc(ppool);
        pri_block = bplib_mpool_bblock_primary_cast(pblk);
        if (pri_block == NULL)
        {
            bplog(NULL, BP_FLAG_OUT_OF_MEMORY, "Failed to allocate primary block\n");
            break;
        }

        pri = bplib_mpool_bblock_primary_get_logical(pri_block);

        /* Initialize Primary Block */
        pri->version = 7;

        v7_set_eid(&pri->sourceEID, &state->self_addr);
        v7_set_eid(&pri->reportEID, &state->self_addr);

        pri->creationTimeStamp.sequence_num = state->generated_dacs_seq;
        ++state->generated_dacs_seq;
        pri->creationTimeStamp.time = v7_get_current_time();

        pri->lifetime                     = BP_CACHE_DACS_LIFETIME;
        pri->controlFlags.isAdminRecord   = true;
        pri->controlFlags.mustNotFragment = true;
        pri->crctype                      = bp_crctype_CRC16;

        /* Add Payload Block */
        cblk    = bplib_mpool_bblock_canonical_alloc(ppool);
        c_block = bplib_mpool_bblock_canonical_cast(cblk);
        if (c_block == NULL)
        {
            bplog(NULL, BP_FLAG_OUT_OF_MEMORY, "Failed to allocate payload block\n");
            break;
        }

        pay = bplib_mpool_bblock_canonical_get_logical(c_block);

        pay->canonical_block.blockNum  = bp_blocktype_custodyAcceptPayloadBlock;
        pay->canonical_block.blockType = bp_blocktype_custodyAcceptPayloadBlock;
        pay->canonical_block.crctype   = bp_crctype_CRC16;

        bplib_mpool_bblock_primary_append(pri_block, cblk);
        cblk   = NULL; /* do not need now that it is stored */
        status = BP_SUCCESS;

        *pri_block_out = pri_block;
        *pay_out       = &pay->data.custody_accept_payload_block;
    }
    while (false);

    /* clean up, if anything did not work, recycle the blocks now */
    if (cblk != NULL)
    {
        bplib_mpool_recycle_block(cblk);
        cblk = NULL;
    }

    if (pblk != NULL && status != BP_SUCCESS)
    {
        bplib_mpool_recycle_block(pblk);
        pblk = NULL;
    }

    return bplib_mpool_ref_create(pblk);
}

void bplib_cache_custody_open_dacs(bplib_cache_state_t *state, bplib_cache_custodian_info_t *custody_info)
{
    bplib_mpool_block_t               *sblk;
    bplib_cache_entry_t               *store_entry;
    bplib_cache_dacs_pending_t        *dacs_pending;
    bplib_mpool_ref_t                  pending_bundle;
    bplib_mpool_bblock_primary_t      *pri_block;
    bp_custody_accept_payload_block_t *ack_content;
    bp_handle_t                        self_intf_id;

    /* Create the storage-specific data block for keeping local refs  */
    sblk           = bplib_mpool_generic_data_alloc(bplib_cache_parent_pool(state), BPLIB_STORE_SIGNATURE_ENTRY, state);
    pending_bundle = bplib_cache_custody_create_dacs(state, &pri_block, &ack_content);
    store_entry    = bplib_mpool_generic_data_cast(sblk, BPLIB_STORE_SIGNATURE_ENTRY);
    if (store_entry != NULL && pending_bundle != NULL)
    {
        /* need to fill out the delivery_data so this will look like a regular bundle when sent */
        self_intf_id                                 = bplib_mpool_get_external_id(bplib_cache_state_self_block(state));
        pri_block->delivery_data.delivery_policy     = bplib_policy_delivery_local_ack;
        pri_block->delivery_data.local_retx_interval = BP_CACHE_FAST_RETRY_TIME;
        pri_block->delivery_data.ingress_intf_id     = self_intf_id;
        pri_block->delivery_data.ingress_time        = bplib_os_get_dtntime_ms();
        pri_block->delivery_data.storage_intf_id     = self_intf_id;
        pri_block->delivery_data.committed_storage_id = (bp_sid_t)sblk;

        store_entry->state = bplib_cache_entry_state_generate_dacs;

        /* the "action_time" reflects when this bundle will be finalized and sent, until
         * then it is open for appending with additional sequence numbers. */
        store_entry->action_time = pri_block->delivery_data.ingress_time + BP_CACHE_DACS_OPEN_TIME;
        store_entry->refptr      = bplib_mpool_ref_duplicate(pending_bundle);

        /* the ack will be sent to the previous custodian of record */
        v7_set_eid(&pri_block->pri_logical_data.destinationEID, &custody_info->custodian_id);
        v7_set_eid(&ack_content->flow_source_eid, &custody_info->flow_id);

        /* set convenience pointers in the dacs_pending extension data - this is mainly
         * so these don't need to be re-found when they are needed again later */
        dacs_pending              = &store_entry->data.dacs;
        dacs_pending->payload_ref = ack_content;
        v7_get_eid(&dacs_pending->prev_custodian_id, &pri_block->pri_logical_data.destinationEID);

        bplib_cache_add_to_subindex(&state->hash_index, &store_entry->hash_link, custody_info->eid_hash);
        bplib_cache_entry_make_pending(
            sblk, BPLIB_STORE_FLAG_ACTIVITY | BPLIB_STORE_FLAG_LOCAL_CUSTODY | BPLIB_STORE_FLAG_ACTION_TIME_WAIT, 0);

        custody_info->store_entry = store_entry;
    }
    else
    {
        if (sblk != NULL)
        {
            bplib_mpool_recycle_block(sblk);
            sblk = NULL;
        }

        store_entry = NULL;
    }

    bplib_mpool_ref_release(pending_bundle);
}

void bplib_cache_custody_append_dacs(bplib_cache_state_t *state, bplib_cache_custodian_info_t *custody_info)
{
    bp_custody_accept_payload_block_t *payload;
    bp_integer_t                       i;

    if (custody_info->store_entry != NULL)
    {
        payload = custody_info->store_entry->data.dacs.payload_ref;

        /* check if this seq is already in the payload, this can happen if a duplicate is recvd */
        for (i = 0; i < payload->num_entries; ++i)
        {
            if (payload->sequence_nums[i] == custody_info->sequence_num)
            {
                break;
            }
        }

        if (i == payload->num_entries && i < BP_DACS_MAX_SEQ_PER_PAYLOAD)
        {
            payload->sequence_nums[i] = custody_info->sequence_num;
            ++payload->num_entries;
        }

        /* if DACS bundle is full now, mark it as "done" */
        if (payload->num_entries == BP_DACS_MAX_SEQ_PER_PAYLOAD)
        {
            bplib_cache_custody_finalize_dacs(state, custody_info->store_entry);
            bplib_cache_entry_make_pending(&custody_info->store_entry->hash_link, 0, BPLIB_STORE_FLAG_ACTION_TIME_WAIT);
        }
    }
}

void bplib_cache_custody_ack_tracking_block(bplib_cache_state_t                *state,
                                            const bplib_cache_custodian_info_t *custody_info)
{
    bplib_cache_custodian_info_t    dacs_info;
    bplib_mpool_bblock_canonical_t *custody_block;

    memset(&dacs_info, 0, sizeof(dacs_info));
    custody_block = bplib_mpool_bblock_canonical_cast(custody_info->cblk);
    if (custody_block != NULL)
    {
        v7_get_eid(&dacs_info.custodian_id,
                   &custody_block->canonical_logical_data.data.custody_tracking_block.current_custodian);
    }

    dacs_info.flow_id         = custody_info->flow_id;
    dacs_info.sequence_num    = custody_info->sequence_num;
    dacs_info.final_dest_node = custody_info->final_dest_node;

    if (!bplib_cache_custody_find_pending_dacs(state, &dacs_info))
    {
        /* open DACS bundle did not exist - make an empty one now */
        bplib_cache_custody_open_dacs(state, &dacs_info);
    }

    bplib_cache_custody_append_dacs(state, &dacs_info);
}

void bplib_cache_custody_update_tracking_block(bplib_cache_state_t *state, bplib_cache_custodian_info_t *custody_info)
{
    bplib_mpool_bblock_canonical_t *custody_block;

    custody_block = bplib_mpool_bblock_canonical_cast(custody_info->cblk);
    if (custody_block != NULL)
    {
        v7_set_eid(&custody_block->canonical_logical_data.data.custody_tracking_block.current_custodian,
                   &state->self_addr);
        custody_info->custodian_id = state->self_addr;
    }
}

void bplib_cache_custody_process_bundle(bplib_cache_state_t *state, bplib_mpool_bblock_primary_t *pri_block,
                                        bplib_cache_custodian_info_t *custody_info)
{
    bool is_local;

    /* check if this is the last stop on the custody train */
    is_local = (custody_info->final_dest_node == state->self_addr.node_number);

    if (custody_info->cblk != NULL)
    {
        /* Acknowledge the block in the bundle */
        bplib_cache_custody_ack_tracking_block(state, custody_info);

        if (is_local)
        {
            /* this only needs acceptance by the local delivery agent, do not expect an ack bundle */
            pri_block->delivery_data.delivery_policy = bplib_policy_delivery_local_ack;
        }
    }
    else if (!is_local)
    {
        /* There is no previous custodian, but the custody block needs to be added (because this
         * function is only invoked where full custody tracking is enabled).  This is the case when
         * this storage entity is the first custodian on locally generated bundles */
        bplib_cache_custody_insert_tracking_block(state, pri_block, custody_info);
    }

    if (custody_info->cblk != NULL)
    {
        /* update the custody block to reflect the new custodian (this service) -
         * whenever the bundle is finally forwarded, this tells the recipient to notify us */
        bplib_cache_custody_update_tracking_block(state, custody_info);
    }
}

int bplib_cache_custody_find_bundle_match(void *arg, bplib_mpool_block_t *lblk)
{
    bplib_cache_entry_t          *store_entry;
    bplib_mpool_bblock_primary_t *pri_block;
    bplib_cache_custodian_info_t *custody_info;
    int                           result;

    /* possible match, but need to verify */
    custody_info = arg;
    result       = -1;

    store_entry = bplib_mpool_generic_data_cast(lblk, BPLIB_STORE_SIGNATURE_ENTRY);
    if (store_entry != NULL)
    {
        pri_block = bplib_mpool_bblock_primary_cast(bplib_mpool_dereference(store_entry->refptr));

        if (pri_block != NULL &&
            pri_block->pri_logical_data.creationTimeStamp.sequence_num == custody_info->sequence_num)
        {
            result = v7_compare_ipn2eid(&custody_info->flow_id, &pri_block->pri_logical_data.sourceEID);
        }
    }

    if (result == 0)
    {
        custody_info->store_entry = store_entry;

        /* set the activity flag which tracks that this entry was used for some purpose.
         * this is part of the deletion age-out process, and indicates this should _not_
         * be fully discarded just yet, it still appears to be relevant */
        store_entry->flags |= BPLIB_STORE_FLAG_ACTIVITY;
    }

    return result;
}

bool bplib_cache_custody_find_existing_bundle(bplib_cache_state_t *state, bplib_cache_custodian_info_t *custody_info)
{
    bplib_rbt_link_t    *custody_rbt_link;
    bplib_cache_queue_t *store_queue;
    bplib_mpool_block_t *sblk;
    bp_crcval_t          hash;

    /* use a CRC as a hash function */
    /* when searching for bundles this includes flow and sequence number but NOT custodian (which would always be us) */
    hash = bplib_crc_initial_value(BPLIB_CACHE_CUSTODY_HASH_ALGORITHM);
    hash = bplib_crc_update(BPLIB_CACHE_CUSTODY_HASH_ALGORITHM, hash, &custody_info->flow_id,
                            sizeof(custody_info->flow_id));
    hash = bplib_crc_update(BPLIB_CACHE_CUSTODY_HASH_ALGORITHM, hash, &custody_info->sequence_num,
                            sizeof(custody_info->sequence_num));
    hash = bplib_crc_update(BPLIB_CACHE_CUSTODY_HASH_ALGORITHM, hash, &BPLIB_CACHE_CUSTODY_HASH_SALT_BUNDLE,
                            sizeof(BPLIB_CACHE_CUSTODY_HASH_SALT_BUNDLE));
    custody_info->eid_hash = bplib_crc_finalize(BPLIB_CACHE_CUSTODY_HASH_ALGORITHM, hash);

    custody_rbt_link = bplib_rbt_search(custody_info->eid_hash, &state->hash_index);
    if (custody_rbt_link != NULL)
    {
        store_queue = bplib_cache_queue_from_rbt_link(custody_rbt_link);

        /*
         * to handle possible hash collision/overlap, there is a list at this entry,
         * which needs to be searched.  Collisions should be unlikely enough that
         * the lists, if they ever become more than one entry, stay short enough
         * such that sequential search is not a burden.
         */
        sblk = bplib_mpool_search_list(&store_queue->bundle_list, bplib_cache_custody_find_bundle_match, custody_info);
    }
    else
    {
        sblk = NULL;
    }

    return (sblk != NULL);
}

void bplib_cache_custody_process_remote_dacs_bundle(bplib_cache_state_t *state, bplib_mpool_bblock_primary_t *pri_block,
                                                    const bp_custody_accept_payload_block_t *ack_payload)
{
    bp_integer_t                 i;
    bplib_cache_custodian_info_t custody_info;

    memset(&custody_info, 0, sizeof(custody_info));

    v7_get_eid(&custody_info.custodian_id, &pri_block->pri_logical_data.destinationEID);
    v7_get_eid(&custody_info.flow_id, &ack_payload->flow_source_eid);

    for (i = 0; i < ack_payload->num_entries; ++i)
    {
        custody_info.sequence_num = ack_payload->sequence_nums[i];
        if (bplib_cache_custody_find_existing_bundle(state, &custody_info))
        {
            /* found it ! */
            printf("%s(): Got custody ACK for seq %lu\n", __func__, (unsigned long)custody_info.sequence_num);

            /* confirmed that another custodian has the bundle -
             * can clear the flag that says we are the active custodian, and reevaluate */
            bplib_cache_entry_make_pending(&custody_info.store_entry->hash_link, 0, BPLIB_STORE_FLAG_LOCAL_CUSTODY);
        }
    }
}

void bplib_cache_custody_finalize_dacs(bplib_cache_state_t *state, bplib_cache_entry_t *store_entry)
{
    /* after this point, the entry becomes a normal bundle, it is removed from EID hash
     * so future appends are also prevented */
    bplib_cache_remove_from_subindex(&state->hash_index, &store_entry->hash_link);
}

bool bplib_cache_custody_check_dacs(bplib_cache_state_t *state, bplib_mpool_block_t *qblk)
{
    bplib_mpool_bblock_primary_t   *pri_block;
    bplib_mpool_bblock_canonical_t *c_block;

    c_block   = NULL;
    pri_block = bplib_mpool_bblock_primary_cast(qblk);
    if (pri_block != NULL && pri_block->pri_logical_data.controlFlags.isAdminRecord)
    {
        /* check if it has a custody_ack payload type */
        c_block = bplib_mpool_bblock_canonical_cast(
            bplib_mpool_bblock_primary_locate_canonical(pri_block, bp_blocktype_custodyAcceptPayloadBlock));
        if (c_block != NULL)
        {
            /* it is an acceptance block (dacs) */
            bplib_cache_custody_process_remote_dacs_bundle(
                state, pri_block, &c_block->canonical_logical_data.data.custody_accept_payload_block);
        }
    }

    return (c_block != NULL);
}

void bplib_cache_custody_store_bundle(bplib_cache_state_t *state, bplib_mpool_block_t *qblk)
{
    bplib_mpool_block_t          *sblk;
    bplib_mpool_bblock_primary_t *pri_block;
    bplib_cache_custodian_info_t  custody_info;

    memset(&custody_info, 0, sizeof(custody_info));
    sblk      = NULL;
    pri_block = bplib_mpool_bblock_primary_cast(qblk);
    if (pri_block == NULL)
    {
        /* only pri blocks are storable */
        return;
    }

    bplib_cache_custody_init_info_from_pblock(&custody_info, pri_block);

    if (bplib_cache_custody_find_existing_bundle(state, &custody_info))
    {
        /* found it - do not store again.  This is not necessarily an error, as retransmits/lost ACKs can
         * easily cause duplicate bundles to be seen here. */
        fprintf(stderr, "DEBUG: %s(): Got duplicate for seq %lu\n", __func__, (unsigned long)custody_info.sequence_num);
        bplib_cache_custody_ack_tracking_block(state, &custody_info);
        return;
    }

    /* Create the storage-specific data block for keeping local refs  */
    sblk = bplib_mpool_generic_data_alloc(bplib_cache_parent_pool(state), BPLIB_STORE_SIGNATURE_ENTRY, state);

    custody_info.store_entry = bplib_mpool_generic_data_cast(sblk, BPLIB_STORE_SIGNATURE_ENTRY);

    if (custody_info.store_entry != NULL)
    {
        custody_info.store_entry->parent = state;
        custody_info.store_entry->state  = bplib_cache_entry_state_idle; /* jphfix, check this */

        /* this keeps a copy of the ref here, after qblk is recycled */
        custody_info.store_entry->refptr = bplib_mpool_ref_from_block(qblk);

        bplib_cache_add_to_subindex(&state->dest_eid_index, &custody_info.store_entry->destination_link,
                                    custody_info.final_dest_node);

        /* when the custody ACK for this block comes in, this block needs to be found again,
         * so make an entry in the hash index for it */
        bplib_cache_add_to_subindex(&state->hash_index, &custody_info.store_entry->hash_link, custody_info.eid_hash);

        custody_info.store_entry->flags |= BPLIB_STORE_FLAG_LOCAL_CUSTODY | BPLIB_STORE_FLAG_ACTIVITY;

        pri_block->delivery_data.storage_intf_id = bplib_mpool_get_external_id(bplib_cache_state_self_block(state));
        pri_block->delivery_data.committed_storage_id = (bp_sid_t)sblk;

        /* NOTE:
         * This may also be the right place to generate a custody signal, if the bundle
         * has the full custody tracking/transfer service level.
         */
        if (pri_block->delivery_data.delivery_policy == bplib_policy_delivery_custody_tracking)
        {
            bplib_cache_custody_process_bundle(state, pri_block, &custody_info);
        }

        /* This puts it into the right spot for future holding */
        bplib_cache_fsm_execute(sblk);
    }
    else if (sblk != NULL)
    {
        /*
         * This should never happen... but do not leak blocks
         */
        bplib_mpool_recycle_block(sblk);
        sblk = NULL;
    }
}
