#define _GNU_SOURCE
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#include <sched.h>
#include <string.h>
#include <pthread.h>

#include <fobos_sdr.h>
#include "fobos.h"
#include "ringbuffer.h"
#include "receiver.h"

//#define DEBUG_RECEIVER

#ifdef DEBUG_RECEIVER
#define DEBUG_PRINTF(...)  fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINTF(...) 
#endif

typedef struct receiver_ctx_t{
    size_t buffer_size;
    fobos_device_t *dev;
    ringbuffer_t rb;
    pthread_t read_thread;
    receiver_mode_t mode;
    int exit;
    size_t sample_size;
    /* scan mode */
    double *scan_freqs;
    int     scan_count;
    int     scan_sps;      /* IQ samples per hardware buffer (samples_per_step) */
}receiver_ctx_t;

static receiver_ctx_t ctx;

static void fobos_data_callback(float *buf, unsigned int buf_length, void* sender, void *ctx);
static void scan_callback(float *buf, uint32_t buf_length, struct fobos_sdr_dev_t *dev, void *user);

/* Pin thread to core 3 and give it SCHED_FIFO priority.
 * Core 3 is dedicated to capture; cores 0-2 remain for DSP and the OS.
 * Requires CAP_SYS_NICE (run as root or set the capability on the binary). */
static void set_realtime_thread(pthread_t tid)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(3, &cpuset);
    if (pthread_setaffinity_np(tid, sizeof(cpuset), &cpuset) != 0)
        fprintf(stderr, "receiver: failed to set CPU affinity (need root?)\n");

    struct sched_param sp = { .sched_priority = 80 };
    if (pthread_setschedparam(tid, SCHED_FIFO, &sp) != 0)
        fprintf(stderr, "receiver: failed to set SCHED_FIFO (need root?)\n");
}

static void *sync_reader_async_starter(void *args){
    receiver_ctx_t *_ctx = (receiver_ctx_t *)args;
    int ret;
    if((ret = fobos_read_samples_async(_ctx->dev, _ctx->buffer_size, fobos_data_callback, _ctx))){
        printf("cannot start async read with %d error code\n", ret);
    }

    return NULL;
}

static void *sync_reader_func(void *args){
    receiver_ctx_t *_ctx = (receiver_ctx_t *)args;
    size_t ret;

    while(!ctx.exit){
        rb_chunk_t *chunk = rb_get_avialable_chunk(&ctx.rb);
        size_t total_data_size = 0;
        while (total_data_size != ctx.buffer_size){
            if((ret = fobos_read_samples_sync_direct(ctx.dev, ((float *)chunk->data) + total_data_size, ctx.buffer_size - total_data_size)) <= 0){
                printf("FobosSDR read error, read %ld, expect to read %ld abort!\n", ret, ctx.buffer_size);
                return NULL;
            }
            total_data_size += ret;
        }   

        chunk->used = ctx.buffer_size * _ctx->sample_size;
        rb_inform_reader(&ctx.rb);
    }

    return NULL;
}

static void fobos_data_callback(float *buf, unsigned int buf_length, void* sender, void *ctx){
    receiver_ctx_t *_ctx = (receiver_ctx_t *)ctx;
    int ret;
    DEBUG_PRINTF("callback to copy %d samples\n", buf_length);
    if((ret = rb_write(&_ctx->rb, buf, buf_length * _ctx->sample_size)) != buf_length * _ctx->sample_size){
        printf("cannot copy %d samples, actual copied size is %d to rb\n", buf_length, ret);
    }
}

/* Scan mode: async callback from fobos_sdr_read_async().
 * Skips buffers arriving while the hardware is re-tuning (channel == -1),
 * then writes the settled buffer into the ring buffer tagged with its freq. */
static void scan_callback(float *buf, uint32_t buf_length, struct fobos_sdr_dev_t *dev, void *user){
    receiver_ctx_t *_ctx = (receiver_ctx_t *)user;

    if(fobos_sdr_is_scanning(dev) != 1){
        fobos_sdr_cancel_async(dev);
        return;
    }

    int channel = fobos_sdr_get_scan_index(dev);
    if(channel == -1)
        return; /* hardware still tuning — discard */

    rb_chunk_t *chunk = rb_get_avialable_chunk(&_ctx->rb);
    size_t byte_count = (size_t)buf_length * _ctx->sample_size;
    memcpy(chunk->data, buf, byte_count);
    chunk->freq = _ctx->scan_freqs[channel];
    chunk->used = byte_count;
    rb_inform_reader(&_ctx->rb);
}

static void *scan_async_starter(void *args){
    receiver_ctx_t *_ctx = (receiver_ctx_t *)args;
    struct fobos_sdr_dev_t *sdr = (struct fobos_sdr_dev_t *)(_ctx->dev->dev);
    int ret = fobos_sdr_read_async(sdr, scan_callback, _ctx, 16, (uint32_t)_ctx->scan_sps);
    if(ret != FOBOS_ERR_OK)
        fprintf(stderr, "receiver: fobos_sdr_read_async error %d\n", ret);
    return NULL;
}


int receiver_init(size_t buffer_size, size_t buffer_count){
    int ret;

    //TODO add checking buffer size is pow 2

    do{
        if((ret = fobos_init()) != 0){
			break;
		}

		if((ret = fobos_get_device_count()) <= 0){
			fprintf(stderr, "no fobos device found\n");
            break;
		}

		if((ctx.dev = fobos_open_by_idx(0)) == NULL){
			fprintf(stderr, "failed to open fobos device\n");
			ret = EACCES;
		}

        ctx.buffer_size = buffer_size;
        ctx.sample_size = 2 * sizeof(float); // IQ float 32

        if((ret = rb_init(&ctx.rb, buffer_count, ctx.buffer_size*ctx.sample_size)) != 0)
            break;

        

    }while(0);

    return ret;
}

int receiver_close(void){
    return fobos_close(ctx.dev);
}

int receiver_set_freq(uint64_t freq){
    return fobos_set_frequency(ctx.dev, freq);
}

int receiver_set_samplerate(uint32_t sr){
    return fobos_set_samplerate(ctx.dev, sr);
}

int receiver_set_bw(float bw){
    return fobos_set_bandwidth(ctx.dev, bw);
}

int receiver_set_lna(uint8_t gain){
    return fobos_set_lna_gain(ctx.dev, gain);
}

int receiver_set_vga(uint8_t gain){
    return fobos_set_vga_gain(ctx.dev, gain);
}

int receiver_start(receiver_mode_t mode){
    ctx.mode = mode;
    ctx.exit = 0;
    int ret = 0;

    if(ctx.mode == receiver_mode_sync){
         if((ret = fobos_start_sync(ctx.dev, FOBOS_DEFAULT_VALUE))){
            printf("cannot start sync read with %d error code\n", ret);
         }else{
            pthread_create(&ctx.read_thread, NULL, sync_reader_func, &ctx);
            set_realtime_thread(ctx.read_thread);
         }
    }else{
        pthread_create(&ctx.read_thread, NULL, sync_reader_async_starter, &ctx);
        set_realtime_thread(ctx.read_thread);
    }
    return ret;
}

int receiver_stop(void){
    ctx.exit = 1;
    if(ctx.mode == receiver_mode_sync){
        pthread_join(ctx.read_thread, NULL);
        fobos_stop_sync(ctx.dev);
    }else{
        fobos_stop_async(ctx.dev);
    }
    return 0;
}

int receiver_read(void *buff, size_t size){
    DEBUG_PRINTF("need to  read %ld samples\n", size);

    uint32_t read_samples = rb_read(&ctx.rb, buff, size * ctx.sample_size)/ctx.sample_size;
    DEBUG_PRINTF("have read %d samples\n", read_samples);
    return read_samples;
}

int receiver_start_scan(double *freqs, int count, int samples_per_step){
    ctx.scan_freqs = freqs;
    ctx.scan_count = count;
    ctx.scan_sps   = samples_per_step;

    struct fobos_sdr_dev_t *sdr = (struct fobos_sdr_dev_t *)(ctx.dev->dev);

    fobos_sdr_set_auto_bandwidth(sdr, FOBOS_AUTO_BW);

    int ret = fobos_sdr_start_scan(sdr, freqs, (unsigned int)count);
    if(ret != FOBOS_ERR_OK){
        fprintf(stderr, "receiver: fobos_sdr_start_scan error %d\n", ret);
        return ret;
    }

    pthread_create(&ctx.read_thread, NULL, scan_async_starter, &ctx);
    set_realtime_thread(ctx.read_thread);
    return 0;
}

int receiver_stop_scan(void){
    struct fobos_sdr_dev_t *sdr = (struct fobos_sdr_dev_t *)(ctx.dev->dev);
    fobos_sdr_cancel_async(sdr);
    fobos_sdr_stop_scan(sdr);
    pthread_join(ctx.read_thread, NULL);
    return 0;
}

int receiver_read_scan_chunk(float *buf, size_t samples_per_step, double *freq_hz){
    size_t bytes = rb_read_chunk(&ctx.rb, buf,
                                 samples_per_step * ctx.sample_size, freq_hz);
    return (int)(bytes / ctx.sample_size);
}
