// #include "com_dev.h"
#include "flexio_ps_dev_utils.h"

// threads and scheduler context
struct dpa_thread_context dpa_thds_ctx[190];
struct dpa_sche_context dpa_schs_ctx[32];

// Offload dispatch info for each thread, used for scheduler to dispatch packets to threads.
struct offload_dispatch_info offload_info[190];
size_t check[2];

// 只swap mac 需要 1400 cycle 左右+-10 
void process_packet(struct flexio_dev_thread_ctx *dtctx, struct dpa_thread_context* tenant)
{
	/* RX packet handling variables */
	struct flexio_dev_wqe_rcv_data_seg *rwqe;
	/* RQ WQE index */
	uint32_t rq_wqe_idx;
	/* Pointer to RQ data */
	char *rq_data;

	/* TX packet handling variables */
	union flexio_dev_sqe_seg *swqe;

	/* Size of the data */
	uint32_t data_sz;

	/* Extract relevant data from the CQE */
	rq_wqe_idx = be16_to_cpu((volatile __be16)tenant->rq_cq_ctx.cqe->wqe_counter);
	data_sz = be32_to_cpu((volatile __be32)tenant->rq_cq_ctx.cqe->byte_cnt);

	/* Get the RQ WQE pointed to by the CQE */
	rwqe = &(tenant->rq_ctx.rq_ring[rq_wqe_idx & RQ_IDX_MASK]);

	/* Extract data (whole packet) pointed to by the RQ WQE */
	rq_data = (void *)be64_to_cpu((volatile __be64)rwqe->addr);

	// get_swap_mac(rq_data);
	swap_mac(rq_data);
	
	swqe = &(tenant->sq_ctx.sq_ring[(tenant->sq_ctx.sq_wqe_seg_idx + 2) & SQ_IDX_MASK]);
	tenant->sq_ctx.sq_wqe_seg_idx += 4;
	flexio_dev_swqe_seg_mem_ptr_data_set(swqe, data_sz, tenant->sq_lkey, (uint64_t)rq_data);

	/* Ring DB */
	__dpa_thread_memory_writeback();
	flexio_dev_qp_sq_ring_db(dtctx, ++tenant->sq_ctx.sq_pi, tenant->sq_ctx.sq_number);
	flexio_dev_dbr_rq_inc_pi(tenant->rq_ctx.rq_dbr);
}

void pp_queue(struct flexio_dev_thread_ctx *dtctx, struct dpa_thread_context* this_thd_ctx, struct flexio_dpa_dev_queue* tenant)
{
	/* RX packet handling variables */
	struct flexio_dev_wqe_rcv_data_seg *rwqe;
	/* RQ WQE index */
	uint32_t rq_wqe_idx;
	/* Pointer to RQ data */
	char *rq_data;

	/* TX packet handling variables */
	union flexio_dev_sqe_seg *swqe;

	/* Size of the data */
	uint32_t data_sz;

	/* Extract relevant data from the CQE */
	rq_wqe_idx = be16_to_cpu((volatile __be16)tenant->rq_cq_ctx.cqe->wqe_counter);
	data_sz = be32_to_cpu((volatile __be32)tenant->rq_cq_ctx.cqe->byte_cnt);

	/* Get the RQ WQE pointed to by the CQE */
	rwqe = &(tenant->rq_ctx.rq_ring[rq_wqe_idx & RQ_IDX_MASK]);

	/* Extract data (whole packet) pointed to by the RQ WQE */
	rq_data = (void *)be64_to_cpu((volatile __be64)rwqe->addr);

	// get_swap_mac(rq_data);
	swap_mac(rq_data);
	// uint64_t src_mac = *((uint64_t *)rq_data);
	// uint64_t dst_mac = *((uint64_t *)(rq_data + 6));
	// *((uint64_t *)rq_data) = dst_mac;
	// *((uint64_t *)(rq_data + 6)) = (src_mac & 0x0000FFFFFFFFFFFF) | (dst_mac & 0xFFFF000000000000);

	
	swqe = &(this_thd_ctx->sq_ctx.sq_ring[(this_thd_ctx->sq_ctx.sq_wqe_seg_idx + 2) & SQ_IDX_MASK]);
	this_thd_ctx->sq_ctx.sq_wqe_seg_idx += 4;
	flexio_dev_swqe_seg_mem_ptr_data_set(swqe, data_sz, tenant->rq_lkey, (uint64_t)rq_data);
	
	// static int first = 1;
	// if (first) {
	// 	uint64_t dst_mac = *((uint64_t *)rq_data);
	// 	uint64_t src_mac = *((uint64_t *)(rq_data + 6));
	// 	flexio_dev_print("data_sz and addr of 1st packet: %u, %p\n", data_sz, (void*)rq_data);
	// 	flexio_dev_print("src mac: %lx, dst mac: %lx\n", src_mac, dst_mac);
	// 	first = 0;
	// }

	/* Ring DB */
	__dpa_thread_memory_writeback();
	flexio_dev_qp_sq_ring_db(dtctx, ++this_thd_ctx->sq_ctx.sq_pi, this_thd_ctx->sq_ctx.sq_number);
	flexio_dev_dbr_rq_inc_pi(tenant->rq_ctx.rq_dbr);
}

flexio_dev_rpc_handler_t thd_ctx_init;
__dpa_rpc__ uint64_t thd_ctx_init(uint64_t data)
{
	struct host2dev_packet_processor_data_thd *data_from_host = (struct host2dev_packet_processor_data_thd *)data;
	struct flexio_dev_thread_ctx *dtctx;
	flexio_dev_get_thread_ctx(&dtctx);
	int i = data_from_host->thd_id;
	dpa_thds_ctx[i].packets_count = 0;
	dpa_thds_ctx[i].sq_lkey = data_from_host->sq_transf.wqd_mkey_id;
	dpa_thds_ctx[i].rq_lkey = data_from_host->rq_transf.wqd_mkey_id;
	dpa_thds_ctx[i].window_id = data_from_host->window_id;
	dpa_thds_ctx[i].idx = i;
	dpa_thds_ctx[i].MAC = data_from_host->MAC;
	dpa_thds_ctx[i].data_sz = data_from_host->data_sz;
	/* Set context for RQ's CQ */
	com_cq_ctx_init(&(dpa_thds_ctx[i].rq_cq_ctx),
			data_from_host->rq_cq_transf.cq_num,
			data_from_host->rq_cq_transf.log_cq_depth,
			data_from_host->rq_cq_transf.cq_ring_daddr,
			data_from_host->rq_cq_transf.cq_dbr_daddr);
	flexio_dev_print("thd %d rq_cq_num %u, dpa_thds_ctx: %p\n", i, data_from_host->rq_cq_transf.cq_num, (void*)&(dpa_thds_ctx[0]));
	// flexio_dev_print("thd %d rq_cq_num %u\n", i, dpa_thds_ctx[i].rq_cq_ctx.cq_number);

	/* Set context for RQ */
	com_rq_ctx_init(&(dpa_thds_ctx[i].rq_ctx),
			data_from_host->rq_transf.wq_num,
			data_from_host->rq_transf.wq_ring_daddr,
			data_from_host->rq_transf.wq_dbr_daddr);

	/* Set context for SQ */
	com_sq_ctx_init(&(dpa_thds_ctx[i].sq_ctx),
			data_from_host->sq_transf.wq_num,
			data_from_host->sq_transf.wq_ring_daddr);

	/* Set context for SQ's CQ */
	com_cq_ctx_init(&(dpa_thds_ctx[i].sq_cq_ctx),
			data_from_host->sq_cq_transf.cq_num,
			data_from_host->sq_cq_transf.log_cq_depth,
			data_from_host->sq_cq_transf.cq_ring_daddr,
			data_from_host->sq_cq_transf.cq_dbr_daddr);

	/* Set context for data */
	com_dt_ctx_init(&(dpa_thds_ctx[i].dt_ctx), data_from_host->sq_transf.wqd_daddr);


	for (uint64_t a = 0; a < (1UL << LOG_Q_DEPTH); a++) {

		union flexio_dev_sqe_seg *swqe;
        swqe = get_next_sqe(&(dpa_thds_ctx[i].sq_ctx), SQ_IDX_MASK);
		flexio_dev_swqe_seg_ctrl_set(swqe, a, dpa_thds_ctx[i].sq_ctx.sq_number,
				     MLX5_CTRL_SEG_CE_CQE_ON_CQE_ERROR, FLEXIO_CTRL_SEG_SEND_EN);

		swqe = get_next_sqe(&(dpa_thds_ctx[i].sq_ctx), SQ_IDX_MASK);
		flexio_dev_swqe_seg_eth_set(swqe, 0, 0, 0, NULL);

        swqe = get_next_sqe(&(dpa_thds_ctx[i].sq_ctx), SQ_IDX_MASK);
		flexio_dev_swqe_seg_mem_ptr_data_set(swqe, 0, dpa_thds_ctx[i].sq_lkey, 0);

        swqe = get_next_sqe(&(dpa_thds_ctx[i].sq_ctx), SQ_IDX_MASK);
	}
    dpa_thds_ctx[i].sq_ctx.sq_wqe_seg_idx = 0;
	dpa_thds_ctx[i].rq_ctx.rqd_dpa_addr = data_from_host->rq_transf.wqd_daddr;
	dpa_thds_ctx[i].sq_ctx.sqd_dpa_addr = data_from_host->sq_transf.wqd_daddr;
	// flexio_dev_status_t ret;
	// ret = flexio_dev_window_config(dtctx, (uint16_t)dpa_thds_ctx[i].window_id, data_from_host->result_buffer_mkey_id);
	// if (ret != FLEXIO_DEV_STATUS_SUCCESS) {
	// 	flexio_dev_print("failed to config rq window, thread %d\n", i);
	// }
	// ret = flexio_dev_window_ptr_acquire(dtctx, (uint64_t)(data_from_host->result_buffer), &(result));
	// if (ret != FLEXIO_DEV_STATUS_SUCCESS) {
	// 	flexio_dev_print("failed to acquire result ptr, thread %d\n", i);
	// }
	// ret = flexio_dev_window_ptr_acquire(dtctx, (uint64_t)(data_from_host->host_buffer),  &(dpa_thds_ctx[i].host_buffer));
	// if (ret != FLEXIO_DEV_STATUS_SUCCESS) {
	// 	flexio_dev_print("failed to acquire result ptr, thread %d\n", i);
	// }
	return 0;
}

flexio_dev_rpc_handler_t dpa_send_first_pkt;
__dpa_rpc__ uint64_t dpa_send_first_pkt(uint64_t data){
	int *index = (int *)data;
	struct flexio_dev_thread_ctx *dtctx;
	flexio_dev_get_thread_ctx(&dtctx);
	// flexio_dev_print("In dpa_send_first_pkt, thd_id: %d\n", *index);
	send_packet(dtctx, &dpa_thds_ctx[*index]);
	return 0;
}

inline flexio_dev_status_t change_status(uint16_t thd_id, eu_status old_status, eu_status new_status){
	eu_status status = __atomic_load_n(&offload_info[thd_id].status, __ATOMIC_ACQUIRE);
	flexio_dev_print("thd: %d, status: %d, old_status: %d, new_status: %d\n", thd_id, status, old_status, new_status);
	if (status == old_status){
		__atomic_store_n(&offload_info[thd_id].status, new_status, __ATOMIC_RELEASE);
		return FLEXIO_DEV_STATUS_SUCCESS;
	}else{
		return FLEXIO_DEV_STATUS_FAILED;
	}
}

inline void spin_on_status(uint16_t thd_id, eu_status expected_status){
	eu_status status;
	do{
		status = __atomic_load_n(&offload_info[thd_id].status, __ATOMIC_ACQUIRE);
	}while (status != expected_status);
}

static void prepare_packet(struct dpa_thread_context* this_thd_ctx, void *sq_data){
	struct ether_hdr *eth_hdr;
    struct ipv4_hdr *ip_hdr;
    struct udp_hdr *udp_hdr;

    eth_hdr = (struct ether_hdr *)sq_data;
    eth_hdr->src_addr = SRC_ADDR;
    eth_hdr->dst_addr = this_thd_ctx->MAC;
    eth_hdr->ether_type = cpu_to_be16(0x0800);

    ip_hdr = (struct ipv4_hdr *)(eth_hdr + 1);
    ip_hdr->version_ihl = 0x45;
    ip_hdr->type_of_service = 0;
    ip_hdr->total_length = cpu_to_be16(sizeof(uint64_t) * 2 + sizeof(struct udp_hdr) + sizeof(struct ipv4_hdr));
    ip_hdr->packet_id = cpu_to_be16(0);
    ip_hdr->fragment_offset = cpu_to_be16(0);
    ip_hdr->time_to_live = 64;
    ip_hdr->next_proto_id = 17;
    ip_hdr->src_addr = cpu_to_be16(1);
    ip_hdr->dst_addr = cpu_to_be16(1);

    udp_hdr = (struct udp_hdr *)(ip_hdr + 1);
    udp_hdr->dgram_len = cpu_to_be16(sizeof(uint64_t) * 2 + sizeof(struct udp_hdr));
    udp_hdr->src_port = cpu_to_be16(1);
    udp_hdr->dst_port = cpu_to_be16(1);
}

static void prepare_send_packet(struct dpa_thread_context* this_thd_ctx, void *data_addr, uint32_t data_sz){
	union flexio_dev_sqe_seg *swqe;
	
	swqe = &(this_thd_ctx->sq_ctx.sq_ring[(this_thd_ctx->sq_ctx.sq_wqe_seg_idx + 2) & SQ_IDX_MASK]);
	this_thd_ctx->sq_ctx.sq_wqe_seg_idx += 4;

	flexio_dev_swqe_seg_mem_ptr_data_set(swqe, data_sz, this_thd_ctx->rq_lkey, (uint64_t)data_addr);
}

static void finish_send(struct flexio_dev_thread_ctx *dtctx, sq_ctx_t *sq_ctx){
	/* Ring DB */
	__dpa_thread_memory_writeback();
	flexio_dev_qp_sq_ring_db(dtctx, ++sq_ctx->sq_pi, sq_ctx->sq_number);
}

// static void finish_send(struct flexio_dev_thread_ctx *dtctx, struct dpa_thread_context* this_thd_ctx){
// 	/* Ring DB */
// 	__dpa_thread_memory_writeback();
// 	flexio_dev_qp_sq_ring_db(dtctx, ++this_thd_ctx->sq_ctx.sq_pi, this_thd_ctx->sq_ctx.sq_number);
// }

void send_packet(struct flexio_dev_thread_ctx *dtctx, struct dpa_thread_context* this_thd_ctx){
	char *sq_data = get_next_dte(&this_thd_ctx->dt_ctx, DATA_IDX_MASK, LOG_Q_DATA_ENTRY_BSIZE);
	prepare_packet(this_thd_ctx, sq_data);
	prepare_send_packet(this_thd_ctx, sq_data, this_thd_ctx->data_sz);
	flexio_dev_print("dst_mac: %lx, index: %d\n", sq_data, this_thd_ctx->idx);
	finish_send(dtctx, &this_thd_ctx->sq_ctx);
	// finish_send(dtctx, this_thd_ctx);
}
