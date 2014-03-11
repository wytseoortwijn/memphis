const int BUFFER_SIZE = 1024;

struct context {
  struct ibv_context *ctx;
  struct ibv_pd *pd;
  struct ibv_cq *cq;
  struct ibv_comp_channel *comp_channel;

  pthread_t cq_poller_thread;
};

struct connection {
	struct ibv_qp* qp;
	struct ibv_mr* recv_mr;
	struct ibv_mr* send_mr;

	char* recv_region;
	char* send_region;
};

static int on_event(struct rdma_cm_event* event);
static int on_connect_request(struct rdma_cm_id* id);
static int on_connection(void* context);
static int on_disconnect(struct rdma_cm_id* id);
static void build_context(struct ibv_context* verbs);
static void* poll_cq(void*);
static void build_qp_attr(struct ibv_qp_init_attr* qp_attr);
static void post_receives(struct connection* conn);
static void register_memory(struct connection* conn);
static void on_completion(struct ibv_wc *wc);
static void build_qp_attr(struct ibv_qp_init_attr *qp_attr);

static struct context* s_ctx = NULL;

void server() {
	struct sockaddr_in addr;
	struct rdma_cm_event* event;
	struct rdma_cm_id* listener;
	struct rdma_event_channel* ec;
	uint16_t port = 0;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	int status;

	ec = rdma_create_event_channel();
	if (!ec) printf("Failed to create an event channel.\n");

	status = rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP);
	if (status) printf("Failed to create an id.\n");

	status = rdma_bind_addr(listener, (struct sockaddr *)&addr);
	if (status) printf("Failed to bind the address.\n");

	status = rdma_listen(listener, 10);
	if (status) printf("Failed to listen.\n");

	port = ntohs(rdma_get_src_port(listener));

	printf("Listening on port %d.\n", port);

  //test
  struct sockaddr_in* myaddr;
  myaddr = rdma_get_local_addr(listener);
  printf("My address is %s.\n", inet_ntoa(&myaddr)); 
  //test

	while (rdma_get_cm_event(ec, &event) == 0) {
		printf("server received an event.\n");
		struct rdma_cm_event event_copy;

		memcpy(&event_copy, event, sizeof(*event));
		rdma_ack_cm_event(event);

		if (on_event(&event_copy))
			break;
	}

	rdma_destroy_id(listener);
	rdma_destroy_event_channel(ec);

	printf("Server stopped.\n");
}

int on_event(struct rdma_cm_event* event) {
	int r;

	if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST) 
		r = on_connect_request(event->id);
	else if (event->event == RDMA_CM_EVENT_ESTABLISHED) 
		r = on_connection(event->id->context);
	else if (event->event == RDMA_CM_EVENT_DISCONNECTED) 
		r = on_disconnect(event->id);
	else
		printf("Unknown event received.\n");

	return r;
}

int on_connect_request(struct rdma_cm_id* id) {
	struct ibv_qp_init_attr qp_attr;
	struct rdma_conn_param cm_params;
	struct connection* conn;
	int status;

	printf("Received connection request.\n");

	build_context(id->verbs);
	build_qp_attr(&qp_attr);

	status = rdma_create_qp(id, s_ctx->pd, &qp_attr);
	if (status) printf("Failed to create a queuing pair.\n");

	id->context = conn = (struct connection*)malloc(sizeof(struct connection));
	conn->qp = id->qp;

	register_memory(conn);
	post_receives(conn);

	memset(&cm_params, 0, sizeof(cm_params));

	status = rdma_accept(id, &cm_params);
	if (status) printf("Failed to accept the connection.\n");

	return 0;
}

void build_context(struct ibv_context *verbs)
{
	int status;

  if (s_ctx) {
    if (s_ctx->ctx != verbs)
      printf("cannot handle events in more than one context.\n");

    return;
  }

  s_ctx = (struct context *)malloc(sizeof(struct context));
  s_ctx->ctx = verbs;

  s_ctx->pd = ibv_alloc_pd(s_ctx->ctx);
  if (!s_ctx->pd) printf("Failed to allocate a protection domain.\n");

  s_ctx->comp_channel = ibv_create_comp_channel(s_ctx->ctx);
  if (!s_ctx->comp_channel) printf("Failed to allocate a completion channel.\n");

  s_ctx->cq = ibv_create_cq(s_ctx->ctx, 10, NULL, s_ctx->comp_channel, 0); 
  if (!s_ctx->cq) printf("Failed to create a completion queue.\n");

  status = ibv_req_notify_cq(s_ctx->cq, 0);
  if (status) printf("Failed to request a completion notification on the completion queue.\n");

  status = pthread_create(&s_ctx->cq_poller_thread, NULL, poll_cq, NULL);
  if (status) printf("Failed to create a thread.\n");
}

void* poll_cq(void* ctx)
{
  struct ibv_cq* cq;
  struct ibv_wc wc;
  int status;

  while (1) {
    status = ibv_get_cq_event(s_ctx->comp_channel, &cq, &ctx);
    if (status) printf("Failed to get an completion queue event.\n");

    ibv_ack_cq_events(cq, 1);
    status = ibv_req_notify_cq(cq, 0);
    if (status) printf("Failed to request a completion notification on the completion queue (2).\n");

    while (ibv_poll_cq(cq, 1, &wc))
      on_completion(&wc);
  }

  return NULL;
}

void build_qp_attr(struct ibv_qp_init_attr* qp_attr)
{
  memset(qp_attr, 0, sizeof(*qp_attr));

  qp_attr->send_cq = s_ctx->cq;
  qp_attr->recv_cq = s_ctx->cq;
  qp_attr->qp_type = IBV_QPT_RC;

  qp_attr->cap.max_send_wr = 10;
  qp_attr->cap.max_recv_wr = 10;
  qp_attr->cap.max_send_sge = 1;
  qp_attr->cap.max_recv_sge = 1;
}

void register_memory(struct connection *conn)
{
  conn->send_region = malloc(BUFFER_SIZE);
  conn->recv_region = malloc(BUFFER_SIZE);

  conn->send_mr = ibv_reg_mr(s_ctx->pd, conn->send_region, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  if (!conn->send_mr) printf("Failed to register a 'send' memory region.\n");

  conn->recv_mr = ibv_reg_mr(s_ctx->pd, conn->recv_region, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  if (!conn->recv_mr) printf("Failed to register a 'receive' memory region.\n");
}

void post_receives(struct connection* conn)
{
  struct ibv_recv_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;
  int status;

  wr.wr_id = (uintptr_t)conn;
  wr.next = NULL;
  wr.sg_list = &sge;
  wr.num_sge = 1;

  sge.addr = (uintptr_t)conn->recv_region;
  sge.length = BUFFER_SIZE;
  sge.lkey = conn->recv_mr->lkey;

  status = ibv_post_recv(conn->qp, &wr, &bad_wr);
  if (status) printf("Failed to post the linked list of work requests.\n");
}

int on_connection(void* context)
{
  struct connection *conn = (struct connection *)context;
  struct ibv_send_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;
  int status;

  snprintf(conn->send_region, BUFFER_SIZE, "message from passive/server side with pid %d", getpid());

  printf("connected. posting send...\n");

  memset(&wr, 0, sizeof(wr));

  wr.opcode = IBV_WR_SEND;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.send_flags = IBV_SEND_SIGNALED;

  sge.addr = (uintptr_t)conn->send_region;
  sge.length = BUFFER_SIZE;
  sge.lkey = conn->send_mr->lkey;

  status = ibv_post_send(conn->qp, &wr, &bad_wr);
  if (status) printf("Failed to post a send request.\n");

  return 0;
}

int on_disconnect(struct rdma_cm_id *id)
{
  struct connection *conn = (struct connection *)id->context;

  printf("peer disconnected.\n");

  rdma_destroy_qp(id);

  ibv_dereg_mr(conn->send_mr);
  ibv_dereg_mr(conn->recv_mr);

  free(conn->send_region);
  free(conn->recv_region);

  free(conn);

  rdma_destroy_id(id);

  return 0;
}

void on_completion(struct ibv_wc *wc)
{
  if (wc->status != IBV_WC_SUCCESS)
    printf("on_completion: status is not IBV_WC_SUCCESS.\n");

  if (wc->opcode & IBV_WC_RECV) {
    struct connection *conn = (struct connection *)(uintptr_t)wc->wr_id;

    printf("received message: %s\n", conn->recv_region);

  } else if (wc->opcode == IBV_WC_SEND) {
    printf("send completed successfully.\n");
  }
}