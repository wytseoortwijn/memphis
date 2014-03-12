#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mpi.h>
#include <rdma/rdma_cma.h>

#define send_data_tag 2001
#define return_data_tag 2002

enum {
	PINGPONG_RECV_WRID = 1,
	PINGPONG_SEND_WRID = 2,
};

struct pingpong_context {
	struct ibv_context* context;
	struct ibv_comp_channel* channel;
	struct ibv_pd* pd;
	struct ibv_mr* mr;
	struct ibv_cq* cq;
	struct ibv_qp* qp;
	void* buf;
	int	size;
	int	rx_depth;
	int	pending;
};

struct pingpong_dest {
	int lid;
	int qpn;
	int psn;
};

enum ibv_mtu pp_mtu_to_enum(int mtu)
{
	switch (mtu) {
	case 256: return IBV_MTU_256;
	case 512: return IBV_MTU_512;
	case 1024: return IBV_MTU_1024;
	case 2048: return IBV_MTU_2048;
	case 4096: return IBV_MTU_4096;
	default: return -1;
	}
}

uint16_t pp_get_local_lid(struct ibv_context* context, int port)
{
	struct ibv_port_attr attr;

	if (ibv_query_port(context, (uint8_t)port, &attr))
		return 0;

	return attr.lid;
}

static struct pingpong_context* init_ctx(struct ibv_device* ib_dev, int size, int rx_depth, int port)
{
	struct pingpong_context* ctx;

	ctx = malloc(sizeof *ctx);
	if (!ctx)
		return NULL;

	ctx->size = size;
	ctx->rx_depth = rx_depth;
	ctx->buf = malloc(size);

	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		return NULL;
	}

	memset(ctx->buf, 0, size);

	ctx->context = ibv_open_device(ib_dev);
	if (!ctx->context) {
		fprintf(stderr, "Couldn't get context for %s\n", ibv_get_device_name(ib_dev));
		return NULL;
	}

	ctx->channel = NULL;

	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		return NULL;
	}

	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, IBV_ACCESS_LOCAL_WRITE);
	if (!ctx->mr) {
		fprintf(stderr, "Couldn't register MR\n");
		return NULL;
	}

	ctx->cq = ibv_create_cq(ctx->context, rx_depth + 1, NULL, ctx->channel, 0);
	if (!ctx->cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		return NULL;
	}

	{
		struct ibv_qp_init_attr attr;
		memset(&attr, 0, sizeof attr);

		attr.send_cq = ctx->cq;
		attr.recv_cq = ctx->cq;
		attr.cap.max_send_wr  = 1;
		attr.cap.max_recv_wr  = rx_depth;
		attr.cap.max_send_sge = 1;
		attr.cap.max_recv_sge = 1;
		attr.qp_type = IBV_QPT_RC;

		ctx->qp = ibv_create_qp(ctx->pd, &attr);
		if (!ctx->qp)  {
			fprintf(stderr, "Couldn't create QP\n");
			return NULL;
		}
	}

	{
		struct ibv_qp_attr attr;
		memset(&attr, 0, sizeof attr);

		attr.qp_state = IBV_QPS_INIT;
		attr.pkey_index = 0;
		attr.port_num = (uint8_t)port;
		attr.qp_access_flags = 0;

		if (ibv_modify_qp(ctx->qp, &attr,
				IBV_QP_STATE |
				IBV_QP_PKEY_INDEX |
				IBV_QP_PORT |
				IBV_QP_ACCESS_FLAGS)) {
			fprintf(stderr, "Failed to modify QP to INIT\n");
			return NULL;
		}
	}

	return ctx;
}

static int connect_ctx(struct pingpong_context* ctx, int port, int my_psn, enum ibv_mtu mtu, int sl, struct pingpong_dest* dest)
{
	struct ibv_qp_attr attr;

	attr.qp_state	= IBV_QPS_RTR;
	attr.path_mtu	= mtu;
	attr.dest_qp_num = dest->qpn;
	attr.rq_psn	= dest->psn;
	attr.max_dest_rd_atomic	= 1;
	attr.min_rnr_timer = 12;
	attr.ah_attr.is_global = 0;
	attr.ah_attr.dlid	= (uint16_t)dest->lid;
	attr.ah_attr.sl	= (uint8_t)sl;
	attr.ah_attr.src_path_bits = 0;
	attr.ah_attr.port_num	= (uint8_t)port;

	if (ibv_modify_qp(ctx->qp, &attr,
				IBV_QP_STATE |
				IBV_QP_AV |
				IBV_QP_PATH_MTU |
				IBV_QP_DEST_QPN |
				IBV_QP_RQ_PSN |
				IBV_QP_MAX_DEST_RD_ATOMIC |
				IBV_QP_MIN_RNR_TIMER)) {
		fprintf(stderr, "Failed to modify QP to RTR\n");
		return 1;
	}

	attr.qp_state	= IBV_QPS_RTS;
	attr.timeout = 14;
	attr.retry_cnt = 7;
	attr.rnr_retry = 7;
	attr.sq_psn = my_psn;
	attr.max_rd_atomic = 1;

	if (ibv_modify_qp(ctx->qp, &attr,
			IBV_QP_STATE |
			IBV_QP_TIMEOUT |
			IBV_QP_RETRY_CNT |
			IBV_QP_RNR_RETRY |
			IBV_QP_SQ_PSN |
			IBV_QP_MAX_QP_RD_ATOMIC)) {
		fprintf(stderr, "Failed to modify QP to RTS\n");
		return 1;
	}

	return 0;
}

static int post_send(struct pingpong_context* ctx)
{
	struct ibv_sge list;
	struct ibv_send_wr wr;
	struct ibv_send_wr* bad_wr;

	list.addr	= (uintptr_t)ctx->buf;
	list.length = ctx->size;
	list.lkey	= ctx->mr->lkey;

	wr.next = NULL;
	wr.wr_id = PINGPONG_SEND_WRID;
	wr.sg_list = &list;
	wr.num_sge = 1;
	wr.opcode = IBV_WR_SEND;
	wr.send_flags = IBV_SEND_SIGNALED;

	return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

static int post_recv(struct pingpong_context* ctx, int n)
{
	struct ibv_sge list;
	struct ibv_recv_wr wr;
	struct ibv_recv_wr* bad_wr;
	int i;

	list.addr	= (uintptr_t)ctx->buf;
	list.length = ctx->size;
	list.lkey	= ctx->mr->lkey;

	wr.next	= NULL;
	wr.wr_id = PINGPONG_RECV_WRID;
	wr.sg_list = &list;
	wr.num_sge = 1;

	for (i = 0; i < n; ++i)
		if (ibv_post_recv(ctx->qp, &wr, &bad_wr))
			break;

	return i;
}

static struct pingpong_dest* client_exch_dest(const struct pingpong_dest* my_dest)
{
	struct pingpong_dest* rem_dest = NULL;
	int lid, qpn, psn;
	MPI_Status status;

	rem_dest = malloc(sizeof *rem_dest);

	lid = my_dest->lid;
	qpn = my_dest->qpn;
	psn = my_dest->psn;

	MPI_Send(&lid, 1, MPI_INT, 0, send_data_tag, MPI_COMM_WORLD);
	MPI_Send(&qpn, 1, MPI_INT, 0, send_data_tag, MPI_COMM_WORLD);
	MPI_Send(&psn, 1, MPI_INT, 0, send_data_tag, MPI_COMM_WORLD);

	MPI_Recv(&lid, 1, MPI_INT, 0, return_data_tag, MPI_COMM_WORLD, &status);
	MPI_Recv(&qpn, 1, MPI_INT, 0, return_data_tag, MPI_COMM_WORLD, &status);
	MPI_Recv(&psn, 1, MPI_INT, 0, return_data_tag, MPI_COMM_WORLD, &status);

	rem_dest->lid = lid;
	rem_dest->qpn = qpn;
	rem_dest->psn = psn;

	return rem_dest;
}

static struct pingpong_dest* server_exch_dest(struct pingpong_context* ctx, int ib_port, 
	enum ibv_mtu mtu, int sl, const struct pingpong_dest* my_dest)
{
	MPI_Status status;
	struct pingpong_dest* rem_dest = NULL;
	int lid, qpn, psn;
	int res;

	rem_dest = malloc(sizeof *rem_dest);

	MPI_Recv(&lid, 1, MPI_INT, 2, send_data_tag, MPI_COMM_WORLD, &status);
	MPI_Recv(&qpn, 1, MPI_INT, 2, send_data_tag, MPI_COMM_WORLD, &status);
	MPI_Recv(&psn, 1, MPI_INT, 2, send_data_tag, MPI_COMM_WORLD, &status);

	rem_dest->lid = lid;
	rem_dest->qpn = qpn;
	rem_dest->psn = psn;

	if (connect_ctx(ctx, ib_port, my_dest->psn, mtu, sl, rem_dest)) {
		fprintf(stderr, "Couldn't connect to remote QP\n");
		free(rem_dest);
		return NULL;
	}

	lid = my_dest->lid;
	qpn = my_dest->qpn;
	psn = my_dest->psn;

	MPI_Send(&lid, 1, MPI_INT, 2, return_data_tag, MPI_COMM_WORLD);
	MPI_Send(&qpn, 1, MPI_INT, 2, return_data_tag, MPI_COMM_WORLD);
	MPI_Send(&psn, 1, MPI_INT, 2, return_data_tag, MPI_COMM_WORLD);

	return rem_dest;
}

int close_ctx(struct pingpong_context* ctx)
{
	if (ibv_destroy_qp(ctx->qp)) {
		fprintf(stderr, "Couldn't destroy QP\n");
		return 1;
	}

	if (ibv_destroy_cq(ctx->cq)) {
		fprintf(stderr, "Couldn't destroy CQ\n");
		return 1;
	}

	if (ibv_dereg_mr(ctx->mr)) {
		fprintf(stderr, "Couldn't deregister MR\n");
		return 1;
	}

	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "Couldn't deallocate PD\n");
		return 1;
	}

	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel)) {
			fprintf(stderr, "Couldn't destroy completion channel\n");
			return 1;
		}
	}

	if (ibv_close_device(ctx->context)) {
		fprintf(stderr, "Couldn't release context\n");
		return 1;
	}

	free(ctx->buf);
	free(ctx);

	return 0;
}

int start(int mpi_id) {
  struct ibv_device** dev_list;
  struct ibv_device* ib_dev;
  struct pingpong_context* ctx;
  struct pingpong_dest my_dest;
  struct pingpong_dest* rem_dest;
  enum ibv_mtu mtu = IBV_MTU_1024;
  int num_cq_events = 0;
  int rcnt, scnt;
  int size = 4096;
  int rx_depth = 500;
  int port = 18515;
  int ib_port = 1;
  int iters = 1000;
  int sl = 0;
  int routs;

	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		fprintf(stderr, "No IB devices found\n");
		return 1;
	}

	ib_dev = *dev_list;
	if (!ib_dev) {
		fprintf(stderr, "No IB devices found\n");
		return 1;
	}

	ctx = init_ctx(ib_dev, size, rx_depth, ib_port);
	if (!ctx)
		return 1;

	routs = post_recv(ctx, ctx->rx_depth);
	if (routs < ctx->rx_depth) {
		fprintf(stderr, "Couldn't post receive (%d)\n", routs);
		return 1;
	}

	my_dest.lid = pp_get_local_lid(ctx->context, ib_port);
	my_dest.qpn = ctx->qp->qp_num;
	my_dest.psn = rand() & 0xffffff;

	if (!my_dest.lid) {
		fprintf(stderr, "Couldn't get local LID\n");
		return 1;
	}

	printf("local address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x\n", my_dest.lid, my_dest.qpn, my_dest.psn);

	if (mpi_id == 0) // node == server??
		rem_dest = server_exch_dest(ctx, ib_port, mtu, sl, &my_dest);
	else if (mpi_id == 2) // node == client?
		rem_dest = client_exch_dest(&my_dest);

	if (!rem_dest) {
		fprintf(stderr, "rem_dest isn't built correctly\n");
		return 1;
	}

	printf("remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x\n", rem_dest->lid, rem_dest->qpn, rem_dest->psn);

	if (mpi_id == 2) 
		if (connect_ctx(ctx, ib_port, my_dest.psn, mtu, sl, rem_dest))
			return 1;

	ctx->pending = PINGPONG_RECV_WRID;

	if (mpi_id == 2) {
		if (post_send(ctx)) {
			fprintf(stderr, "Couldn't post send\n");
			return 1;
		}
		ctx->pending |= PINGPONG_SEND_WRID;
	}

	rcnt = scnt = 0;
	while (rcnt < iters || scnt < iters) {
		struct ibv_wc wc[2];
		int ne, i;

		do {
			ne = ibv_poll_cq(ctx->cq, 2, wc);
			if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
			}
		} while (ne < 1);

		for (i = 0; i < ne; ++i) {
			if (wc[i].status != IBV_WC_SUCCESS) {
				fprintf(stderr, "Failed status %s (%d) for wr_id %d\n", ibv_wc_status_str(wc[i].status), wc[i].status, (int) wc[i].wr_id);
				return 1;
			}

			switch ((int) wc[i].wr_id) {
			case PINGPONG_SEND_WRID:
				++scnt;
				break;

			case PINGPONG_RECV_WRID:
				if (--routs <= 1) {
					routs += post_recv(ctx, ctx->rx_depth - routs);
					if (routs < ctx->rx_depth) {
						fprintf(stderr, "Couldn't post receive (%d)\n", routs);
						return 1;
					}
					else {
						printf("Just posted a receive on %i...\n", mpi_id);
					}
				}

				++rcnt;
				break;

			default:
				fprintf(stderr, "Completion for unknown wr_id %d\n", (int) wc[i].wr_id);
				return 1;
			}

			ctx->pending &= ~(int) wc[i].wr_id;
			if (scnt < iters && !ctx->pending) {
				if (post_send(ctx)) {
					fprintf(stderr, "Couldn't post send\n");
					return 1;
				} else {
					//printf("Just posted a send on %i...\n", mpi_id);
				}
				ctx->pending = PINGPONG_RECV_WRID | PINGPONG_SEND_WRID;
			}
		}
	}

	ibv_ack_cq_events(ctx->cq, num_cq_events);

	if (close_ctx(ctx))
		return 1;

	ibv_free_device_list(dev_list);
	free(rem_dest);
}


int main(int argc, char** argv) {
	int ierr, num_procs, id, len;
	char name[MPI_MAX_PROCESSOR_NAME];
	MPI_Status status;

	ierr = MPI_Init(&argc, &argv);
	ierr = MPI_Comm_rank(MPI_COMM_WORLD, &id);
	ierr = MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
	ierr = MPI_Get_processor_name(name, &len);

	printf("I am process %i of %i on %s.\n", id, num_procs, name);

	if (id == 0 || id == 2)
		start(id);

	MPI_Finalize();
	printf("Process %i finalized.\n", id);
	
	return 0;
}