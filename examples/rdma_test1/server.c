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

static struct pingpong_dest* server_exch_dest(struct pingpong_context* ctx, int ib_port, 
	enum ibv_mtu mtu, int port, int sl, const struct pingpong_dest* my_dest)
{
	MPI_Status status;
	struct pingpong_dest* rem_dest = NULL;
	int res;

	rem_dest = malloc(sizeof *rem_dest);

	// Goal: receiving a LID, QPN, PSN from a client
	MPI_Recv(&(rem_dest->lid), 1, MPI_INT, MPI_ANY_SOURCE, return_data_tag, MPI_COMM_WORLD, &status);
	MPI_Recv(&(rem_dest->qpn), 1, MPI_INT, MPI_ANY_SOURCE, return_data_tag, MPI_COMM_WORLD, &status);
	MPI_Recv(&(rem_dest->psn), 1, MPI_INT, MPI_ANY_SOURCE, return_data_tag, MPI_COMM_WORLD, &status);

	fprintf(stderr, "The server received information from a client!!!!!\n");
}

int server() {
  struct ibv_device** dev_list;
  struct ibv_device* ib_dev;
  struct pingpong_context* ctx;
  struct pingpong_dest my_dest;
  struct pingpong_dest* rem_dest;
  enum ibv_mtu mtu = IBV_MTU_1024;
  int size = 4096;
  int rx_depth = 500;
  int port = 18515;
  int ib_port = 1;
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

	rem_dest = server_exch_dest(ctx, ib_port, mtu, port, sl, &my_dest);
}