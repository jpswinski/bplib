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

/******************************************************************************
 INCLUDES
 ******************************************************************************/

#include "v7_cache_internal.h"

bplib_cache_state_t *bplib_cache_get_state(bplib_mpool_block_t *intf_block)
{
    bplib_cache_state_t *state;

    state = bplib_mpool_generic_data_cast(intf_block, BPLIB_STORE_SIGNATURE_STATE);
    if (state == NULL)
    {
        bplog(NULL, BP_FLAG_DIAGNOSTIC, "%s(): storage_block incorrect for bplib_cache_state_t\n", __func__);
        return NULL;
    }

    return state;
}

void bplib_cache_entry_make_pending(bplib_mpool_block_t *qblk, uint32_t set_flags, uint32_t clear_flags)
{
    bplib_cache_entry_t *store_entry;
    bplib_mpool_block_t *sblk;

    sblk        = bplib_mpool_get_block_from_link(qblk);
    store_entry = bplib_mpool_generic_data_cast(sblk, BPLIB_STORE_SIGNATURE_ENTRY);
    if (store_entry != NULL)
    {
        /* update the flags based on the event */
        store_entry->flags |= set_flags;
        store_entry->flags &= ~clear_flags;

        bplib_mpool_extract_node(sblk);
        bplib_mpool_insert_before(&store_entry->parent->pending_list, sblk);
    }
}

int bplib_cache_handle_ref_recycle(void *arg, bplib_mpool_block_t *rblk)
{
    bplib_cache_blockref_t *block_ref;

    block_ref = bplib_mpool_generic_data_cast(rblk, BPLIB_STORE_SIGNATURE_BLOCKREF);
    if (block_ref == NULL)
    {
        return BP_ERROR;
    }

    assert(block_ref->storage_entry_block != NULL);

    /*
     * always put back into pending_list, this will re-evalute its current
     * state and reclassify it as appropriate.  This also clears the BPLIB_STORE_FLAG_LOCALLY_QUEUED
     * flag.
     */
    bplib_cache_entry_make_pending(block_ref->storage_entry_block, 0, BPLIB_STORE_FLAG_LOCALLY_QUEUED);

    return BP_SUCCESS;
}

void bplib_cache_remove_from_subindex(bplib_rbt_root_t *index_root, bplib_mpool_block_t *index_link)
{
    bplib_cache_queue_t *store_queue;
    bplib_mpool_block_t *list_ptr;
    bplib_mpool_block_t *self_block;

    /* grab the list ptr before removal (in case it becomes empty by this) */
    list_ptr = bplib_mpool_get_next_block(index_link);
    bplib_mpool_extract_node(index_link);

    /* If this arrived back at an empty head node, that means this was the last entry in that subq,
     * which then needs to be removed from its parent index tree */
    if (list_ptr != index_link && bplib_mpool_is_empty_list_head(list_ptr))
    {
        self_block  = bplib_mpool_get_block_from_link(list_ptr);
        store_queue = bplib_mpool_generic_data_cast(self_block, BPLIB_STORE_SIGNATURE_QUEUE);

        /* if node was already extracted/not in the tree, this has no effect */
        bplib_rbt_extract_node(index_root, &store_queue->rbt_link);
        bplib_mpool_recycle_block(self_block);
    }
}

int bplib_cache_construct_queue(void *arg, bplib_mpool_block_t *tblk)
{
    bplib_cache_queue_t *store_queue;

    store_queue = bplib_mpool_generic_data_cast(tblk, BPLIB_STORE_SIGNATURE_QUEUE);
    if (store_queue == NULL)
    {
        return BP_ERROR;
    }

    bplib_mpool_init_list_head(tblk, &store_queue->bundle_list);

    return BP_SUCCESS;
}

int bplib_cache_destruct_queue(void *arg, bplib_mpool_block_t *qblk)
{
    bplib_cache_queue_t *store_queue;

    store_queue = bplib_mpool_generic_data_cast(qblk, BPLIB_STORE_SIGNATURE_QUEUE);
    if (store_queue == NULL)
    {
        return BP_ERROR;
    }

    assert(bplib_mpool_is_empty_list_head(&store_queue->bundle_list));

    return BP_SUCCESS;
}

void bplib_cache_add_to_subindex(bplib_rbt_root_t *index_root, bplib_mpool_block_t *index_link, bp_val_t index_val)
{
    bplib_cache_queue_t *store_queue;
    bplib_mpool_block_t *tblk;
    bplib_rbt_link_t    *tlink;

    tlink = bplib_rbt_search(index_val, index_root);
    if (tlink != NULL)
    {
        /* not the first time this was seen, add to the existing subq */
        store_queue = bplib_cache_queue_from_rbt_link(tlink);
    }
    else
    {
        /* first occurrance of this particular index, need to create a subq block */
        tblk        = bplib_mpool_generic_data_alloc(bplib_mpool_get_parent_pool_from_link(index_link),
                                                     BPLIB_STORE_SIGNATURE_QUEUE, NULL);
        store_queue = bplib_mpool_generic_data_cast(tblk, BPLIB_STORE_SIGNATURE_QUEUE);
        if (store_queue != NULL)
        {
            /* This should always work, because it was already known _not_ to be a duplicate */
            bplib_rbt_insert_value(index_val, index_root, &store_queue->rbt_link);
        }
        else
        {
            /* must be out of memory */
            store_queue = NULL;
        }
    }

    if (store_queue != NULL)
    {
        bplib_mpool_insert_before(&store_queue->bundle_list, index_link);
    }
}

int bplib_cache_construct_entry(void *arg, bplib_mpool_block_t *sblk)
{
    bplib_cache_entry_t *store_entry;

    store_entry = bplib_mpool_generic_data_cast(sblk, BPLIB_STORE_SIGNATURE_ENTRY);
    if (store_entry == NULL)
    {
        return BP_ERROR;
    }

    store_entry->parent = arg;
    bplib_mpool_init_secondary_link(sblk, &store_entry->hash_link, bplib_mpool_blocktype_secondary_generic);
    bplib_mpool_init_secondary_link(sblk, &store_entry->time_link, bplib_mpool_blocktype_secondary_generic);
    bplib_mpool_init_secondary_link(sblk, &store_entry->destination_link, bplib_mpool_blocktype_secondary_generic);

    return BP_SUCCESS;
}

int bplib_cache_destruct_entry(void *arg, bplib_mpool_block_t *sblk)
{
    bplib_cache_entry_t *store_entry;
    bplib_cache_state_t *state;

    store_entry = bplib_mpool_generic_data_cast(sblk, BPLIB_STORE_SIGNATURE_ENTRY);
    if (store_entry == NULL)
    {
        return BP_ERROR;
    }

    state = store_entry->parent;

    /* need to make sure this is removed from all index trees */
    bplib_cache_remove_from_subindex(&state->hash_index, &store_entry->hash_link);
    bplib_cache_remove_from_subindex(&state->time_index, &store_entry->time_link);
    bplib_cache_remove_from_subindex(&state->dest_eid_index, &store_entry->destination_link);

    /* release the refptr */
    bplib_mpool_ref_release(store_entry->refptr);
    store_entry->refptr = NULL;

    return BP_SUCCESS;
}

int bplib_cache_egress_impl(void *arg, bplib_mpool_block_t *subq_src)
{
    bplib_mpool_flow_t  *flow;
    bplib_mpool_block_t *qblk;
    bplib_mpool_block_t *intf_block;
    bplib_cache_state_t *state;
    int                  forward_count;

    intf_block = bplib_mpool_get_block_from_link(subq_src);
    state      = bplib_cache_get_state(intf_block);
    if (state == NULL)
    {
        return -1;
    }

    flow = bplib_mpool_flow_cast(intf_block);
    if (flow == NULL)
    {
        return -1;
    }

    state->action_time = bplib_os_get_dtntime_ms();
    forward_count      = 0;
    while (true)
    {
        qblk = bplib_mpool_flow_try_pull(&flow->egress, 0);
        if (qblk == NULL)
        {
            /* no more bundles */
            break;
        }

        ++forward_count;

        /* Is this a data bundle that needs to be stored, or is this a custody ack? */
        if (!bplib_cache_custody_check_dacs(state, qblk))
        {
            bplib_cache_custody_store_bundle(state, qblk);
        }

        /*
         * The original/input ref to the bundle can be removed without issue, a copy
         * was stored, so this should not trigger a zero refcount unless the storage
         * failed.
         */
        if (bplib_mpool_is_indirect_block(qblk))
        {
            bplib_mpool_recycle_block(qblk);
        }
    }

    return forward_count;
}

void bplib_cache_flush_pending(bplib_cache_state_t *state)
{
    bplib_mpool_list_iter_t list_it;
    int                     status;
    bplib_mpool_flow_t     *self_flow;

    self_flow = bplib_cache_get_flow(state);

    /* Attempt to re-route all bundles in the pending list */
    /* In some cases the bundle can get re-added to the pending list, so this is done in a loop */
    status = bplib_mpool_list_iter_goto_first(&state->pending_list, &list_it);
    while (status == BP_SUCCESS && bplib_mpool_subq_workitem_may_push(&self_flow->ingress))
    {
        /* removal of an iterator node is allowed */
        bplib_mpool_extract_node(list_it.position);
        bplib_cache_fsm_execute(list_it.position);
        status = bplib_mpool_list_iter_forward(&list_it);
    }
}

int bplib_cache_do_poll(bplib_cache_state_t *state)
{
    bplib_rbt_iter_t        rbt_it;
    bplib_mpool_list_iter_t list_it;
    bplib_cache_queue_t    *store_queue;
    int                     rbt_status;
    int                     list_status;

    rbt_status = bplib_rbt_iter_goto_max(bplib_os_get_dtntime_ms(), &state->time_index, &rbt_it);
    while (rbt_status == BP_SUCCESS)
    {
        store_queue = bplib_cache_queue_from_rbt_link(rbt_it.position);

        /* preemptively move the iterator - the current entry will be removed,
         * and if that was done first, it would invalidate the iterator */
        rbt_status = bplib_rbt_iter_prev(&rbt_it);

        /* move the entire set of nodes on this tree entry to the pending_list */
        /* remove everything from the time index because its time has passed and will be rescheduled */
        list_status = bplib_mpool_list_iter_goto_first(&store_queue->bundle_list, &list_it);
        while (list_status == BP_SUCCESS)
        {
            /* removal of an iterator node is allowed */
            bplib_mpool_extract_node(list_it.position);
            bplib_cache_entry_make_pending(list_it.position, 0, 0);
            list_status = bplib_mpool_list_iter_forward(&list_it);
        }

        /* done with this entry in the time index (will be re-added when pending_list is processed) */
        bplib_rbt_extract_node(&state->time_index, &store_queue->rbt_link);

        bplib_mpool_recycle_block(bplib_mpool_get_block_from_link(&store_queue->bundle_list));
    }

    return BP_SUCCESS;
}

int bplib_cache_do_route_up(bplib_cache_state_t *state, bp_ipn_t dest, bp_ipn_t mask)
{
    bplib_rbt_iter_t        rbt_it;
    bplib_mpool_list_iter_t list_it;
    bplib_cache_queue_t    *store_queue;
    bp_ipn_t                curr_ipn;
    int                     rbt_status;
    int                     list_status;

    rbt_status = bplib_rbt_iter_goto_min(dest, &state->dest_eid_index, &rbt_it);
    while (rbt_status == BP_SUCCESS)
    {
        curr_ipn = bplib_rbt_get_key_value(rbt_it.position);
        if ((curr_ipn & mask) != dest)
        {
            /* no longer a route match, all done */
            break;
        }

        rbt_status  = bplib_rbt_iter_next(&rbt_it);
        store_queue = bplib_cache_queue_from_rbt_link(rbt_it.position);

        /* put everything on the bundle list here onto the pending_list, but
         * do not remove from the bundle list at this time */
        list_status = bplib_mpool_list_iter_goto_first(&store_queue->bundle_list, &list_it);
        while (list_status == BP_SUCCESS)
        {
            bplib_cache_entry_make_pending(list_it.position, 0, 0);
            list_status = bplib_mpool_list_iter_forward(&list_it);
        }
    }

    return BP_SUCCESS;
}

int bplib_cache_do_intf_statechange(bplib_cache_state_t *state, bool is_up)
{
    bplib_mpool_flow_t *self_flow;

    self_flow = bplib_cache_get_flow(state);
    if (is_up)
    {
        self_flow->ingress.current_depth_limit = BP_MPOOL_MAX_SUBQ_DEPTH;
        self_flow->egress.current_depth_limit  = BP_MPOOL_MAX_SUBQ_DEPTH;
    }
    else
    {
        self_flow->ingress.current_depth_limit = 0;
        self_flow->egress.current_depth_limit  = 0;
    }
    return BP_SUCCESS;
}

int bplib_cache_event_impl(void *event_arg, bplib_mpool_block_t *intf_block)
{
    bplib_cache_state_t              *state;
    bplib_mpool_flow_generic_event_t *event;
    bp_handle_t                       self_intf_id;

    event        = event_arg;
    self_intf_id = bplib_mpool_get_external_id(intf_block);
    state        = bplib_cache_get_state(intf_block);
    if (state == NULL)
    {
        return -1;
    }

    state->action_time = bplib_os_get_dtntime_ms();
    if (event->event_type == bplib_mpool_flow_event_poll)
    {
        bplib_cache_do_poll(state);
    }
    else if ((event->event_type == bplib_mpool_flow_event_up || event->event_type == bplib_mpool_flow_event_down) &&
             bp_handle_equal(self_intf_id, event->intf_state.intf_id))
    {
        bplib_cache_do_intf_statechange(state, event->event_type == bplib_mpool_flow_event_up);
    }

    /* any sort of action may have put bundles in the pending queue, so flush it now */
    bplib_cache_flush_pending(state);

    return BP_SUCCESS;
}

int bplib_cache_construct_state(void *arg, bplib_mpool_block_t *sblk)
{
    bplib_cache_state_t *state;

    state = bplib_mpool_generic_data_cast(sblk, BPLIB_STORE_SIGNATURE_STATE);
    if (state == NULL)
    {
        return BP_ERROR;
    }

    bplib_mpool_init_list_head(sblk, &state->pending_list);
    bplib_mpool_init_list_head(sblk, &state->idle_list);

    bplib_rbt_init_root(&state->hash_index);
    bplib_rbt_init_root(&state->dest_eid_index);
    bplib_rbt_init_root(&state->time_index);

    return BP_SUCCESS;
}

int bplib_cache_destruct_state(void *arg, bplib_mpool_block_t *sblk)
{
    bplib_cache_state_t *state;

    state = bplib_mpool_generic_data_cast(sblk, BPLIB_STORE_SIGNATURE_STATE);
    if (state == NULL)
    {
        return BP_ERROR;
    }

    /* At this point, all the sub-indices and lists should be empty.  The application
     * should have made this so before attempting to delete the intf.
     * If not so, they cannot be cleaned up now, because the state object is no longer valid,
     * the desctructors for these objects will not work correctly */
    assert(bplib_rbt_tree_is_empty(&state->time_index));
    assert(bplib_rbt_tree_is_empty(&state->dest_eid_index));
    assert(bplib_rbt_tree_is_empty(&state->hash_index));
    assert(bplib_mpool_is_link_unattached(&state->idle_list));
    assert(bplib_mpool_is_link_unattached(&state->pending_list));

    return BP_SUCCESS;
}

int bplib_cache_construct_blockref(void *arg, bplib_mpool_block_t *sblk)
{
    bplib_cache_blockref_t *blockref;
    bplib_cache_entry_t    *store_entry;

    store_entry = arg;
    blockref    = bplib_mpool_generic_data_cast(sblk, BPLIB_STORE_SIGNATURE_BLOCKREF);
    if (blockref == NULL)
    {
        return BP_ERROR;
    }

    /*
     * note, this needs a ref back to the block itself, not the store_entry object.
     */
    blockref->storage_entry_block = bplib_cache_entry_self_block(store_entry);
    return BP_SUCCESS;
}

void bplib_cache_init(bplib_mpool_t *pool)
{
    const bplib_mpool_blocktype_api_t state_api = (bplib_mpool_blocktype_api_t) {
        .construct = bplib_cache_construct_state,
        .destruct  = bplib_cache_destruct_state,
    };

    const bplib_mpool_blocktype_api_t entry_api = (bplib_mpool_blocktype_api_t) {
        .construct = bplib_cache_construct_entry,
        .destruct  = bplib_cache_destruct_entry,
    };

    const bplib_mpool_blocktype_api_t queue_api = (bplib_mpool_blocktype_api_t) {
        .construct = bplib_cache_construct_queue,
        .destruct  = bplib_cache_destruct_queue,
    };

    const bplib_mpool_blocktype_api_t blockref_api = (bplib_mpool_blocktype_api_t) {
        .construct = bplib_cache_construct_blockref,
        .destruct  = bplib_cache_handle_ref_recycle,
    };

    bplib_mpool_register_blocktype(pool, BPLIB_STORE_SIGNATURE_STATE, &state_api, sizeof(bplib_cache_state_t));
    bplib_mpool_register_blocktype(pool, BPLIB_STORE_SIGNATURE_ENTRY, &entry_api, sizeof(bplib_cache_entry_t));
    bplib_mpool_register_blocktype(pool, BPLIB_STORE_SIGNATURE_QUEUE, &queue_api, sizeof(bplib_cache_queue_t));
    bplib_mpool_register_blocktype(pool, BPLIB_STORE_SIGNATURE_BLOCKREF, &blockref_api, sizeof(bplib_cache_blockref_t));
}

bp_handle_t bplib_cache_attach(bplib_routetbl_t *tbl, const bp_ipn_addr_t *service_addr)
{
    bplib_cache_state_t *state;
    bplib_mpool_block_t *sblk;
    bplib_mpool_t       *pool;
    bplib_mpool_ref_t    flow_block_ref;
    bp_handle_t          storage_intf_id;

    pool = bplib_route_get_mpool(tbl);

    /* register Mem Cache storage module */
    bplib_cache_init(pool);

    sblk = bplib_mpool_flow_alloc(pool, BPLIB_STORE_SIGNATURE_STATE, pool);
    if (sblk == NULL)
    {
        bplog(NULL, BP_FLAG_OUT_OF_MEMORY, "%s(): Insufficient memory to create file storage\n", __func__);
        return BP_INVALID_HANDLE;
    }

    /* this must always work, it was just created above */
    flow_block_ref = bplib_mpool_ref_create(sblk);
    state          = bplib_mpool_generic_data_cast(sblk, BPLIB_STORE_SIGNATURE_STATE);

    storage_intf_id = bplib_dataservice_attach(tbl, service_addr, bplib_dataservice_type_storage, flow_block_ref);
    if (!bp_handle_is_valid(storage_intf_id))
    {
        bplog(NULL, BP_FLAG_DIAGNOSTIC, "%s(): cannot attach - service addr invalid?\n", __func__);
        bplib_mpool_ref_release(flow_block_ref);
    }
    else
    {
        /* there should be no reason for any of these reg calls to fail */
        bplib_route_register_forward_egress_handler(tbl, storage_intf_id, bplib_cache_egress_impl);
        bplib_route_register_forward_ingress_handler(tbl, storage_intf_id, bplib_route_ingress_to_parent);
        bplib_route_register_event_handler(tbl, storage_intf_id, bplib_cache_event_impl);

        /* This will keep the ref to itself inside of the state struct, this
         * creates a circular reference and prevents the refcount from ever becoming 0
         */
        state->self_addr = *service_addr;
    }

    return storage_intf_id;
}

int bplib_cache_detach(bplib_routetbl_t *tbl, const bp_ipn_addr_t *service_addr)
{
    bplib_cache_state_t *state;
    bplib_mpool_ref_t    flow_block_ref;
    int                  status;

    flow_block_ref = bplib_dataservice_detach(tbl, service_addr);
    if (flow_block_ref != NULL)
    {
        state = bplib_mpool_generic_data_cast(bplib_mpool_dereference(flow_block_ref), BPLIB_STORE_SIGNATURE_STATE);
    }
    else
    {
        state = NULL;
    }

    if (state == NULL)
    {
        bplog(NULL, BP_FLAG_DIAGNOSTIC, "%s(): cannot detach - service addr invalid?\n", __func__);
        status = BP_ERROR;
    }
    else
    {
        /* Release the local ref - this should cause the refcount to become 0 */
        bplib_mpool_ref_release(flow_block_ref);
        status = BP_SUCCESS;
    }

    return status;
}

void bplib_cache_debug_scan(bplib_routetbl_t *tbl, bp_handle_t intf_id)
{
    bplib_mpool_ref_t    intf_block_ref;
    bplib_cache_state_t *state;

    intf_block_ref = bplib_route_get_intf_controlblock(tbl, intf_id);
    if (intf_block_ref == NULL)
    {
        bplog(NULL, BP_FLAG_DIAGNOSTIC, "%s(): Parent intf invalid\n", __func__);
        return;
    }

    state = bplib_cache_get_state(bplib_mpool_dereference(intf_block_ref));
    if (state == NULL)
    {
        bplog(NULL, BP_FLAG_DIAGNOSTIC, "%s(): Parent intf is not a storage cache\n", __func__);
        return;
    }

    printf("DEBUG: %s() intf_id=%d\n", __func__, bp_handle_printable(intf_id));

    bplib_mpool_debug_print_list_stats(&state->pending_list, "pending_list");
    bplib_mpool_debug_print_list_stats(&state->idle_list, "idle_list");

    bplib_route_release_intf_controlblock(tbl, intf_block_ref);
}
