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

int server() {
  struct ibv_device** dev_list;
  struct ibv_device* ib_dev;
  struct pingpong_context* ctx;
  int size = 4096;
  int rx_depth = 500;
  int ib_port = 1;

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

	printf('Everything works so far..\n');
}