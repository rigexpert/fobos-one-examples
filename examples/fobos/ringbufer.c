#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "ringbuffer.h"

int rb_init(ringbuffer_t* rb, size_t chunk_count, size_t chunk_size){
    memset(rb, 0, sizeof(*rb));

    rb->chunk_count = chunk_count;
    rb->chunk_size = chunk_size;

    rb->chunks = calloc(chunk_count, sizeof(rb_chunk_t));
    if(!rb->chunks)
        return -1;

    for(size_t i = 0; i < chunk_count; i++){
        rb->chunks[i].data = aligned_alloc(64, chunk_size);

        if(!rb->chunks[i].data)
            return -1;

        rb->chunks[i].used = 0;
    }

    atomic_init(&rb->write_idx, 0);
    atomic_init(&rb->read_idx, 0);
    atomic_init(&rb->count, 0);

    pthread_mutex_init(&rb->wait_lock, NULL);
    pthread_cond_init(&rb->data_available, NULL);

    return 0;
}

void rb_destroy(ringbuffer_t* rb){
    for(size_t i = 0; i < rb->chunk_count; i++)
        free(rb->chunks[i].data);

    free(rb->chunks);

    pthread_mutex_destroy(&rb->wait_lock);
    pthread_cond_destroy(&rb->data_available);
}


int rb_write(ringbuffer_t* rb, const void* data, size_t len){
    if(len > rb->chunk_size){
        fprintf(stderr, "rb_write: chunk too large\n");
        return -1;
    }

    size_t write_idx = atomic_load_explicit(&rb->write_idx,
                             memory_order_relaxed);

    rb_chunk_t* chunk = &rb->chunks[write_idx];

    //
    // COPY WITHOUT LOCK
    //
    memcpy(chunk->data, data, len);

    //
    // publish size only after memcpy
    //
    atomic_thread_fence(memory_order_release);

    chunk->used = len;

    size_t next_write = (write_idx + 1) % rb->chunk_count;

    atomic_store_explicit(&rb->write_idx, next_write, memory_order_release);

    size_t old_count = atomic_fetch_add_explicit(&rb->count, 1, memory_order_acq_rel);

    //
    // overflow -> drop oldest
    //
    if(old_count >= rb->chunk_count){

        fprintf(stderr, "ringbuffer overflow: overwriting oldest chunk\n");

        atomic_fetch_sub_explicit(&rb->count, 1, memory_order_acq_rel);

        size_t old_read = atomic_load_explicit(&rb->read_idx, memory_order_relaxed);

        size_t next_read = (old_read + 1) % rb->chunk_count;

        atomic_store_explicit(&rb->read_idx, next_read, memory_order_release);
    }

    //
    // wakeup reader
    //
    pthread_mutex_lock(&rb->wait_lock);
    pthread_cond_signal(&rb->data_available);
    pthread_mutex_unlock(&rb->wait_lock);
    return len;
}

rb_chunk_t* rb_get_avialable_chunk(ringbuffer_t* rb){
    void* ptr;

    size_t write_idx = atomic_load_explicit(&rb->write_idx,
                             memory_order_relaxed);

    rb_chunk_t* chunk = &rb->chunks[write_idx];


    ptr = chunk;

    //
    // publish size only after memcpy
    //


    size_t next_write = (write_idx + 1) % rb->chunk_count;

    atomic_store_explicit(&rb->write_idx, next_write, memory_order_release);

    size_t old_count = atomic_fetch_add_explicit(&rb->count, 1, memory_order_acq_rel);

    //
    // overflow -> drop oldest
    //
    if(old_count >= rb->chunk_count){

        fprintf(stderr, "ringbuffer overflow: overwriting oldest chunk\n");

        atomic_fetch_sub_explicit(&rb->count, 1, memory_order_acq_rel);

        size_t old_read = atomic_load_explicit(&rb->read_idx, memory_order_relaxed);

        size_t next_read = (old_read + 1) % rb->chunk_count;

        atomic_store_explicit(&rb->read_idx, next_read, memory_order_release);
    }

    return ptr;
}

void rb_inform_reader(ringbuffer_t* rb){
    atomic_thread_fence(memory_order_release);

    pthread_mutex_lock(&rb->wait_lock);
    pthread_cond_signal(&rb->data_available);
    pthread_mutex_unlock(&rb->wait_lock);
}

size_t rb_read_chunk(ringbuffer_t* rb, void* out, size_t max_bytes, double *freq_out){
    pthread_mutex_lock(&rb->wait_lock);
    while(atomic_load_explicit(&rb->count, memory_order_acquire) == 0){
        pthread_cond_wait(&rb->data_available, &rb->wait_lock);
    }
    pthread_mutex_unlock(&rb->wait_lock);

    size_t read_idx = atomic_load_explicit(&rb->read_idx, memory_order_relaxed);
    rb_chunk_t* chunk = &rb->chunks[read_idx];
    atomic_thread_fence(memory_order_acquire);

    size_t to_copy = (chunk->used < max_bytes) ? chunk->used : max_bytes;
    memcpy(out, chunk->data, to_copy);
    if(freq_out) *freq_out = chunk->freq;

    size_t next_read = (read_idx + 1) % rb->chunk_count;
    atomic_store_explicit(&rb->read_idx, next_read, memory_order_release);
    atomic_fetch_sub_explicit(&rb->count, 1, memory_order_acq_rel);

    return to_copy;
}

size_t rb_read(ringbuffer_t* rb, void* out, size_t len){
    uint8_t* dst = out;
    size_t total = 0;

    while(total < len){
        //
        // fast path
        //
        size_t count = atomic_load_explicit(&rb->count, memory_order_acquire);

        //
        // no data -> blocking wait
        //
        if(count == 0){

            pthread_mutex_lock(&rb->wait_lock);

            while(atomic_load_explicit(&rb->count, memory_order_acquire) == 0){
                pthread_cond_wait(&rb->data_available, &rb->wait_lock);
            }

            pthread_mutex_unlock(&rb->wait_lock);

            continue;
        }

        //
        // current chunk
        //
        size_t read_idx = atomic_load_explicit(&rb->read_idx, memory_order_relaxed);

        rb_chunk_t* chunk = &rb->chunks[read_idx];

        //
        // ensure writer memcpy visible
        //
        atomic_thread_fence(memory_order_acquire);

        size_t available = chunk->used - rb->read_offset;
        size_t to_copy = (len - total < available) ? (len - total) : available;

        //
        // COPY WITHOUT LOCK
        //
        memcpy(dst + total, chunk->data + rb->read_offset, to_copy);

        total += to_copy;
        rb->read_offset += to_copy;

        //
        // chunk consumed
        //
        if(rb->read_offset == chunk->used){
            rb->read_offset = 0;
            size_t next_read = (read_idx + 1) % rb->chunk_count;

            atomic_store_explicit(&rb->read_idx, next_read, memory_order_release);
            atomic_fetch_sub_explicit(&rb->count, 1, memory_order_acq_rel);
        }
    }

    return total;
}
