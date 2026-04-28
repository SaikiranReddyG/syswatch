#define _POSIX_C_SOURCE 200809L

#include "syswatch.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* Create and initialize bounded event queue */
event_queue_t *queue_create(void) {
	event_queue_t *q = malloc(sizeof(event_queue_t));
	if (!q) return NULL;

	q->head = 0;
	q->tail = 0;
	q->count = 0;
	q->dropped_count = 0;
	q->emitted_count = 0;

	if (pthread_mutex_init(&q->lock, NULL) != 0) {
		free(q);
		return NULL;
	}

	if (pthread_cond_init(&q->not_empty, NULL) != 0) {
		pthread_mutex_destroy(&q->lock);
		free(q);
		return NULL;
	}

	return q;
}

/* Destroy queue and free resources */
void queue_destroy(event_queue_t *q) {
	if (!q) return;
	pthread_cond_destroy(&q->not_empty);
	pthread_mutex_destroy(&q->lock);
	free(q);
}

/* Enqueue event. Drop oldest if queue is full. */
int queue_enqueue(event_queue_t *q, const char *json_data, size_t len) {
	if (!q || !json_data || len == 0 || len > MAX_EVENT_SIZE) {
		return -1;
	}

	pthread_mutex_lock(&q->lock);

	/* If queue is full, drop oldest event (drop-oldest policy) */
	if (q->count >= MAX_QUEUE_DEPTH) {
		q->head = (q->head + 1) % MAX_QUEUE_DEPTH;
		q->count--;
		q->dropped_count++;
	}

	/* Enqueue at tail */
	event_queue_entry_t *entry = &q->entries[q->tail];
	memcpy(entry->json_data, json_data, len);
	entry->json_len = len;
	if (clock_gettime(CLOCK_REALTIME, &entry->enqueue_time) != 0) {
		entry->enqueue_time.tv_sec = 0;
		entry->enqueue_time.tv_nsec = 0;
	}

	q->tail = (q->tail + 1) % MAX_QUEUE_DEPTH;
	q->count++;
	q->emitted_count++;

	/* Signal waiting threads */
	pthread_cond_broadcast(&q->not_empty);

	pthread_mutex_unlock(&q->lock);
	return 0;
}

/* Dequeue up to max_count events into out array. Returns actual count in out_count. */
int queue_dequeue_batch(event_queue_t *q, event_queue_entry_t *out, 
						int max_count, int *out_count) {
	if (!q || !out || max_count <= 0 || !out_count) {
		return -1;
	}

	pthread_mutex_lock(&q->lock);

	int count = 0;
	while (count < max_count && q->count > 0) {
		memcpy(&out[count], &q->entries[q->head], sizeof(event_queue_entry_t));
		q->head = (q->head + 1) % MAX_QUEUE_DEPTH;
		q->count--;
		count++;
	}

	*out_count = count;
	pthread_mutex_unlock(&q->lock);

	return 0;
}

/* Get current queue depth */
int queue_size(event_queue_t *q) {
	if (!q) return -1;
	
	pthread_mutex_lock(&q->lock);
	int size = q->count;
	pthread_mutex_unlock(&q->lock);
	
	return size;
}

/* Get cumulative dropped count */
unsigned long long queue_dropped_count(event_queue_t *q) {
	if (!q) return 0;
	
	pthread_mutex_lock(&q->lock);
	unsigned long long count = q->dropped_count;
	pthread_mutex_unlock(&q->lock);
	
	return count;
}
