/*
 * togo_m_queue.h
 *
 *  Created on: 2015-7-11
 *      Author: zhuli
 */

#ifndef TOGO_M_QUEUE_H_
#define TOGO_M_QUEUE_H_

#define TOGO_M_QUEUE_POOL_SIZE (1024 * 1024)
#define TOGO_M_QUEUE_BLOCK_SIZE (1024 * 1024 * 8)
#define TOGO_M_QUEUE_BLOCK_NUM 5
#define TOGO_M_QUEUE_BLOCK_FREELIST_MAX 8

typedef struct togo_m_queue TOGO_M_QUEUE;
typedef struct togo_m_queue_block TOGO_M_QUEUE_BLOCK;
typedef struct togo_m_queue_fblock TOGO_M_QUEUE_FBLOCK;
typedef struct togo_m_queue_item TOGO_M_QUEUE_ITEM;

struct togo_m_queue {
	u_char * name;
	uint32_t total_elt;
	uint32_t total_block;
	TOGO_M_QUEUE_BLOCK * block;
	TOGO_M_QUEUE_ITEM * head;
	TOGO_M_QUEUE_ITEM * tail;
	pthread_mutex_t qlock;
	TOGO_POOL * pool;
};

struct togo_m_queue_block {
	size_t size;
	u_char * buf;
	u_char * curr;
	uint32_t nelt;
	TOGO_M_QUEUE_BLOCK * prev;
	TOGO_M_QUEUE_BLOCK * next;
};

struct togo_m_queue_fblock {
	TOGO_M_QUEUE_BLOCK * block;
	uint32_t total;
	pthread_mutex_t flock;
};

struct togo_m_queue_item {
	u_char * val;
	size_t size;
	TOGO_M_QUEUE_ITEM * next;
	TOGO_M_QUEUE_ITEM * prev;
	TOGO_M_QUEUE_BLOCK * block;
};
void togo_m_queue_command(TOGO_COMMAND_TAG command_tag[], TOGO_THREAD_ITEM *socket_item);
void togo_m_queue_init(void);
BOOL togo_m_queue_rpush(u_char * name, u_char * val, size_t len);
BOOL togo_m_queue_lpush(u_char * name, u_char * val, size_t len);
void togo_m_queue_lpop(u_char * name, TOGO_THREAD_ITEM *socket_item);
void togo_m_queue_rpop(u_char * name, TOGO_THREAD_ITEM *socket_item);
void togo_m_queue_count(u_char * name, TOGO_THREAD_ITEM * socket_item);

TOGO_POOL * togo_m_queue_pool;
TOGO_HASHTABLE * togo_m_queue_hashtable;
TOGO_M_QUEUE_FBLOCK * togo_m_queue_fblock;

#endif /* TOGO_M_QUEUE_H_ */
