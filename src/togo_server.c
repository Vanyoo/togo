/*
 * server.c
 *
 * Created on: 2015-6-15
 * Author: zhuli
 */
#include "togo.h"
#include "togo_load.h"

static int last_thread = -1;
pthread_t server_main_thread;

/**
 * Initialize main thread of  the Togo.
 * The main thread is used to accpet a new client connection.
 * If have a new connection, The worker thread will take over this connection.
 */
static void togo_mt_init();

/**
 * The callback function of the main thread.
 */
static void* togo_mt_process(void* args);

/**
 * This function is used to accept client connections.
 */
static void togo_mt_doaccept(evutil_socket_t fd, short event, void *arg);

/**
 * Initialize worker thread of the Togo.
 * The worker thread is used to process to read/write event.
 * We can defined worker thread's number.Default worker thread's number is 8.
 * We can set config's param "worker_thread_num" according your needs.
 * Each worker thread have a independent event model.
 */
static void togo_wt_init();

/**
 * This function is used to set worker thread's event model and other information.
 * We can receive asynchronous notification through pipe.
 */
static void togo_wt_setup(TOGO_WORKER_THREAD *worker_thread);

/**
 * This function is used to create worker thread.
 */
static void togo_wt_create(TOGO_WORKER_THREAD *worker_thread);

/**
 * The callback function of the worker thread.
 */
static void * togo_wt_cb(void* args);

/**
 * The callback function of the pipe event.
 */
static void togo_wt_process(evutil_socket_t fd, short event, void *arg);

/**
 * The callback function of the read event.
 */
static void togo_wt_read_cb(struct bufferevent *bev, void *arg);

/**
 * The callback function When send the data to client
 */
static int togo_wt_send_cb(TOGO_THREAD_ITEM * socket_item);

/**
 * The callback function of the error event.
 */
static void togo_wt_event_cb(struct bufferevent *bev, short event, void *arg);

/**
 * Destroy a socket link
 */
static void togo_wt_destroy_socket(struct bufferevent *bev,
		TOGO_THREAD_ITEM * socket_item);

/**
 * Initialize a Togo queue.
 */
static void togo_q_init(TOGO_WORKER_THREAD *worker_thread);

/**
 * Push a socket_item into Togo queue.
 */
static void togo_q_push(TOGO_WORKER_THREAD *worker_thread,
		TOGO_THREAD_ITEM *socket_item);

/**
 * Pop a socket_item from Togo queue.
 */
static TOGO_THREAD_ITEM * togo_q_pop(TOGO_WORKER_THREAD *worker_thread);

BOOL togo_server_init()
{
	togo_mt_init();
	togo_wt_init();
	pthread_join(server_main_thread, NULL);
}

static void togo_mt_init()
{
	pthread_create(&server_main_thread, NULL, togo_mt_process, NULL);
	togo_log(INFO, "Initialize main thread, ip:%s port:%d.", togo_c.ip,
			togo_c.port);
}

static void * togo_mt_process(void* args)
{
	struct event_base *base_ev;
	struct event *ev;
	evutil_socket_t server_socketfd;
	struct sockaddr_in server_addr;

	togo_memzero(&server_addr, sizeof(server_addr));
	server_addr.sin_family = AF_INET;

	if (strcmp(togo_c.ip, TOGO_C_DEFAULT_IP) == 0) {
		server_addr.sin_addr.s_addr = INADDR_ANY;
	} else {
		server_addr.sin_addr.s_addr = inet_addr(togo_c.ip);
	}

	if (togo_c.port == 0) {
		server_addr.sin_port = htons(TOGO_C_DEFAULT_PORT);
	} else {
		server_addr.sin_port = htons(togo_c.port);
	}

	server_socketfd = socket(PF_INET, SOCK_STREAM, 0);
	if (server_socketfd < 0) {
		togo_log(ERROR, "Socket error.");
		togo_exit();
	}

	evutil_make_listen_socket_reuseable(server_socketfd);
	evutil_make_socket_nonblocking(server_socketfd);

	if (bind(server_socketfd, (struct sockaddr *) &server_addr,
			sizeof(struct sockaddr)) < 0) {
		togo_log(ERROR, "bind error.");
		togo_exit();
	}

	listen(server_socketfd, 32);
	togo_log(INFO, "Togo Server Start.......");

	/* event_base */
	base_ev = event_base_new();
	ev = event_new(base_ev, server_socketfd, EV_TIMEOUT | EV_READ | EV_PERSIST,
			togo_mt_doaccept, base_ev);

	event_add(ev, NULL);
	event_base_dispatch(base_ev); //loop
	event_base_free(base_ev);

}

static void togo_mt_doaccept(evutil_socket_t fd, short event, void *arg)
{
	struct sockaddr_in client_addr;
	int in_size = sizeof(struct sockaddr_in);
	u_char * rbuf;
	u_char * sbuf;
	int client_socketfd = accept(fd, (struct sockaddr *) &client_addr,
			&in_size);
	if (client_socketfd < 0) {
		togo_log(INFO, "Accept error.");
		return;
	}

	/**
	 * We through the way of polling to select a thread,
	 * Who will taking over a connection.
	 */
	int worker_thread_num = togo_c.worker_thread_num;
	int tid = (last_thread + 1) % worker_thread_num;
	TOGO_WORKER_THREAD * worker_thread = togo_worker_threads + tid;
	last_thread = tid;

	/**
	 * Initialize a socket_item. Each connection will have one socket_item.
	 * Through this socket_item , We store each connection details.
	 * When this connection is disconnect, The socket_item will be freed.
	 */
	TOGO_POOL * worker_pool = togo_pool_create(TOGO_WORKER_POOL_SIZE);
	TOGO_THREAD_ITEM * socket_item = togo_pool_calloc(worker_pool,
			sizeof(TOGO_THREAD_ITEM));
	rbuf = togo_pool_alloc(worker_pool, sizeof(u_char) * TOGO_S_RBUF_INIT_SIZE);
	sbuf = togo_pool_alloc(worker_pool, TOGO_S_SBUF_INIT_SIZE);

	socket_item->sfd = client_socketfd;
	socket_item->rbuf = rbuf;
	socket_item->rcurr = socket_item->rbuf;
	socket_item->rsize = sizeof(u_char) * TOGO_S_RBUF_INIT_SIZE;
	socket_item->rbytes = 0;
	socket_item->sbuf = sbuf;
	socket_item->ssize = 0;
	socket_item->sbuf_size = sizeof(u_char) * TOGO_S_SBUF_INIT_SIZE;
	socket_item->worker_pool = worker_pool;

	togo_q_push(worker_thread, socket_item);

	u_char buf[1];
	buf[0] = 'c';
	if (write(worker_thread->notify_send_fd, buf, 1) != 1) {
		togo_log(ERROR, "Write pipe error.");
	}

}

static void togo_wt_init()
{
	TOGO_WORKER_THREAD * temp;
	int i, j;
	int worker_thread_num = togo_c.worker_thread_num;

	togo_worker_threads = (TOGO_WORKER_THREAD *) togo_pool_calloc(togo_pool,
			sizeof(TOGO_WORKER_THREAD) * worker_thread_num);

	for (i = 0; i < worker_thread_num; i++) {
		togo_wt_setup(&togo_worker_threads[i]);
	}

	for (j = 0; j < worker_thread_num; j++) {
		togo_wt_create(&togo_worker_threads[j]);
	}

}

static void togo_wt_setup(TOGO_WORKER_THREAD *worker_thread)
{

	int pipe_fds[2];
	if (pipe(pipe_fds) < 0) {
		togo_log(ERROR, "Can't create pipe.");
		togo_exit();
	}

	worker_thread->notify_receive_fd = pipe_fds[0];
	worker_thread->notify_send_fd = pipe_fds[1];

	if (pthread_mutex_init(&worker_thread->mutex_lock, NULL) != 0) {
		togo_log(ERROR, "Can't init pthread mutex lock.");
		togo_exit();
	}

	togo_q_init(worker_thread);

	worker_thread->base = event_base_new();
	if (!worker_thread->base) {
		togo_log(ERROR, "Can't alloc event base.");
		togo_exit();
	}

	worker_thread->pipe_event = event_new(worker_thread->base,
			worker_thread->notify_receive_fd, EV_READ | EV_PERSIST,
			togo_wt_process, worker_thread);
	event_add(worker_thread->pipe_event, NULL);

}

static void togo_wt_create(TOGO_WORKER_THREAD *worker_thread)
{
	pthread_t worker_thread_id;
	pthread_create(&worker_thread_id, NULL, togo_wt_cb, worker_thread);
	togo_log(INFO, "Create Worker Thread TID:%d", worker_thread_id);
}

static void * togo_wt_cb(void* args)
{
	TOGO_WORKER_THREAD *worker_thread = (TOGO_WORKER_THREAD *) args;
	event_base_loop(worker_thread->base, 0); //事件循环
}

static void togo_wt_process(evutil_socket_t fd, short event, void *arg)
{
	u_char bufs[1];
	TOGO_WORKER_THREAD *worker_thread = (TOGO_WORKER_THREAD *) arg;

	if (read(fd, bufs, 1) != 1) {
		togo_log(INFO, "Read pipe error.");
		return;
	}

	switch (bufs[0]) {
	case 'c':
		if (worker_thread) {

			TOGO_THREAD_ITEM * socket_item = togo_q_pop(worker_thread);

			if (socket_item == NULL) {
				togo_log(INFO, "Can not pop a socket_item from queue.");
				return;
			} else {

				int client_socketfd = socket_item->sfd;

				struct bufferevent *bev = bufferevent_socket_new(
						worker_thread->base, client_socketfd,
						BEV_OPT_CLOSE_ON_FREE);
				socket_item->bev = bev;
				bufferevent_setcb(bev, togo_wt_read_cb, NULL, togo_wt_event_cb,
						socket_item);
				bufferevent_enable(bev, EV_READ | EV_WRITE | EV_PERSIST);
				bufferevent_setwatermark(bev, EV_READ, 0, 0);
			}
		}
		break;
	}

}

static void togo_wt_read_cb(struct bufferevent *bev, void *arg)
{
	TOGO_THREAD_ITEM * socket_item = (TOGO_THREAD_ITEM *) arg;

	/* Read the network data from socket */
	enum TOGO_READ_NETWORK read_ret = togo_command_read_network(bev,
			socket_item);

	if (read_ret == READ_DATA_RECEIVED) {

		while (1) {
			if (socket_item->rbytes == 0) {
				break;
			}

			if (socket_item->bstatus == 1) {
				togo_command_read_big_data(socket_item, togo_wt_send_cb);
				continue;
			}

			BOOL parse_ret = togo_command_parse_command(socket_item,
					togo_wt_send_cb);

			if (parse_ret == TRUE) {
				if (socket_item->rbytes > 0) {
					continue;
				}
			} else {

				/* If can't parse any command and the read buffer's size
				 * more than TOGO_S_RBUF_MAX_SIZE, we must to close the socket.
				 */
				if (socket_item->rsize >= TOGO_S_RBUF_MAX_SIZE) {
					togo_wt_destroy_socket(bev, socket_item);
				}
			}
			break;
		}

	}
}

int togo_wt_send_cb(TOGO_THREAD_ITEM * socket_item)
{
	if (socket_item->sfd < 1 || socket_item->sbuf == NULL
			|| socket_item->ssize == 0) {
		return 0;
	}
	int ret = bufferevent_write(socket_item->bev, socket_item->sbuf,
			socket_item->ssize);
	if (ret < 0) {
		togo_log(INFO, "Send data error");
	}
	socket_item->ssize = 0;
	return ret;
}

static void togo_wt_event_cb(struct bufferevent *bev, short event, void *arg)
{
	TOGO_THREAD_ITEM * socket_item = (TOGO_THREAD_ITEM *) arg;

	if (event & BEV_EVENT_TIMEOUT) {
		togo_log(DEBUG, "Timed out. fd = %u", socket_item->sfd);
	} else if (event & BEV_EVENT_EOF) {
		togo_log(DEBUG, "Connection closed. fd = %u", socket_item->sfd);
	} else if (event & BEV_EVENT_ERROR) {
		togo_log(DEBUG, "Some other error. fd = %u", socket_item->sfd);
	}

	togo_wt_destroy_socket(bev, socket_item);
}

static void togo_wt_destroy_socket(struct bufferevent *bev,
		TOGO_THREAD_ITEM * socket_item)
{
	if (bev) {
		bufferevent_free(bev);
	} else {
		if (socket_item->sfd >= 0) {
			close(socket_item->sfd);
		}
	}

	if (socket_item->worker_pool) {
		togo_pool_destroy(socket_item->worker_pool);
	}
}

static void togo_q_init(TOGO_WORKER_THREAD *worker_thread)
{
	worker_thread->queue = togo_pool_calloc(togo_pool,
			sizeof(TOGO_THREAD_QUEUE));
	worker_thread->queue->head = NULL;
	worker_thread->queue->tail = NULL;
}

static void togo_q_push(TOGO_WORKER_THREAD *worker_thread,
		TOGO_THREAD_ITEM *socket_item)
{
	TOGO_THREAD_QUEUE * queue = worker_thread->queue;

	if (socket_item != NULL) {
		socket_item->next = NULL;
		pthread_mutex_lock(&queue->queue_lock);

		if (queue->head == NULL && queue->tail == NULL) {
			queue->head = socket_item;
			queue->tail = socket_item;
		} else {
			if (queue->tail != NULL) {
				queue->tail->next = socket_item;
				queue->tail = socket_item;
			}
		}

		pthread_mutex_unlock(&queue->queue_lock);
	}
}

static TOGO_THREAD_ITEM * togo_q_pop(TOGO_WORKER_THREAD *worker_thread)
{
	TOGO_THREAD_QUEUE * queue = worker_thread->queue;
	pthread_mutex_lock(&queue->queue_lock);

	TOGO_THREAD_ITEM * head_item = queue->head;
	if (queue->head != NULL && queue->tail != NULL) {
		queue->head = head_item->next;
		if (queue->head == NULL) {
			queue->tail = NULL;
		}
	}

	pthread_mutex_unlock(&queue->queue_lock);
	return head_item;
}

