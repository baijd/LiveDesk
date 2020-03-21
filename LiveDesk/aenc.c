#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include "stdafx.h"
#include "ringbuf.h"
#include "adev.h"
#include "aenc.h"
#include "faac.h"
#include "log.h"

#define ENC_BUF_SIZE  (64 * 1024)
typedef struct {
    void    *adev;
    uint8_t  buffer[ENC_BUF_SIZE];
    int      head;
    int      tail;
    int      size;

    #define TS_EXIT        (1 << 0)
    #define TS_START       (1 << 1)
    #define TS_REQUEST_IDR (1 << 2)
    int      status;

    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    pthread_t       thread;

    faacEncHandle faacenc;
    uint32_t      insamples;
    uint32_t      outbufsize;
    uint8_t      *aaccfgptr;
    uint32_t      aaccfgsize;
} AENC;

static void* aenc_encode_thread_proc(void *param)
{
    AENC *enc = (AENC*)param;
    uint8_t *inbuf, outbuf[2048];
    int32_t len;

    while (!(enc->status & TS_EXIT)) {
        if (!(enc->status & TS_START)) {
            usleep(100*1000); continue;
        }

        adev_ctrl(enc->adev, ADEV_LOCK_BUFFER, (void*)&inbuf, enc->insamples * sizeof(int16_t));
        len = faacEncEncode(enc->faacenc, (int32_t*)inbuf, enc->insamples, outbuf, sizeof(outbuf));
        adev_ctrl(enc->adev, ADEV_UNLOCK_BUFFER, NULL, 0);

        pthread_mutex_lock(&enc->mutex);
        if (len > 0 && len + (int)sizeof(int32_t) <= (int)sizeof(enc->buffer) - enc->size) {
            enc->tail  = ringbuf_write(enc->buffer, sizeof(enc->buffer), enc->tail, (uint8_t*)&len, sizeof(len));
            enc->tail  = ringbuf_write(enc->buffer, sizeof(enc->buffer), enc->tail, outbuf, len);
            enc->size += sizeof(int32_t) + len;
            pthread_cond_signal(&enc->cond);
        } else {
            log_printf("aenc drop data %d !\n", len);
        }
        pthread_mutex_unlock(&enc->mutex);
    }
    return NULL;
}

void* aenc_init(void *adev, int channels, int samplerate, int bitrate)
{
    faacEncConfigurationPtr conf;
    AENC *enc = calloc(1, sizeof(AENC));
    if (!enc) return NULL;
    enc->adev = adev;

    // init mutex & cond
    pthread_mutex_init(&enc->mutex, NULL);
    pthread_cond_init (&enc->cond , NULL);

    enc->faacenc = faacEncOpen((unsigned long)samplerate, (unsigned int)channels, &enc->insamples, &enc->outbufsize);
    conf = faacEncGetCurrentConfiguration(enc->faacenc);
    conf->aacObjectType = LOW;
    conf->mpegVersion   = MPEG4;
    conf->useLfe        = 0;
    conf->useTns        = 0;
    conf->allowMidside  = 0;
    conf->bitRate       = bitrate;
    conf->outputFormat  = 0;
    conf->inputFormat   = FAAC_INPUT_16BIT;
    conf->shortctl      = SHORTCTL_NORMAL;
    conf->quantqual     = 88;
    faacEncSetConfiguration(enc->faacenc, conf);
    faacEncGetDecoderSpecificInfo(enc->faacenc, &enc->aaccfgptr, &enc->aaccfgsize);

    pthread_create(&enc->thread, NULL, aenc_encode_thread_proc, enc);
    return enc;
}

void aenc_free(void *ctxt)
{
    AENC *enc = (AENC*)ctxt;
    if (!ctxt) return;

    enc->status |= TS_EXIT;
    pthread_join(enc->thread, NULL);

    if (enc->faacenc) faacEncClose(enc->faacenc);

    pthread_mutex_destroy(&enc->mutex);
    pthread_cond_destroy (&enc->cond );
    free(enc);
}

void aenc_start(void *ctxt, int start)
{
    AENC *enc = (AENC*)ctxt;
    if (!enc) return;
    if (start) enc->status |= TS_START;
    else {
        pthread_mutex_lock(&enc->mutex);
        enc->status &= ~TS_START;
        pthread_cond_signal(&enc->cond);
        pthread_mutex_unlock(&enc->mutex);
    }
}

int aenc_ctrl(void *ctxt, int cmd, void *buf, int size)
{
    AENC   *enc = (AENC*)ctxt;
    int32_t framesize, readsize = 0;
    if (!ctxt) return -1;

    switch (cmd) {
    case AENC_CMD_START:
        adev_ctrl(enc->adev, ADEV_CMD_START, NULL, 0);
        enc->status |= TS_START;
        break;
    case AENC_CMD_STOP:
        adev_ctrl(enc->adev, ADEV_CMD_STOP, NULL, 0);
        pthread_mutex_lock(&enc->mutex);
        enc->status &= ~TS_START;
        pthread_cond_signal(&enc->cond);
        pthread_mutex_unlock(&enc->mutex);
        break;
    case AENC_CMD_RESET_BUFFER:
        pthread_mutex_lock(&enc->mutex);
        enc->head = enc->tail = enc->size = 0;
        pthread_mutex_unlock(&enc->mutex);
        break;
    case AENC_CMD_READ:
        pthread_mutex_lock(&enc->mutex);
        while (enc->size <= 0 && (enc->status & TS_START)) pthread_cond_wait(&enc->cond, &enc->mutex);
        if (enc->size > 0) {
            enc->head = ringbuf_read(enc->buffer, sizeof(enc->buffer), enc->head, (uint8_t*)&framesize , sizeof(framesize));
            enc->size-= sizeof(framesize);
            readsize  = size < framesize ? size : framesize;
            if (readsize < framesize) {
                log_printf("AENC_CMD_READ drop data %d !\n", framesize - readsize);
            }
            enc->head = ringbuf_read(enc->buffer, sizeof(enc->buffer), enc->head,  buf , readsize);
            enc->head = ringbuf_read(enc->buffer, sizeof(enc->buffer), enc->head,  NULL, framesize - readsize);
            enc->size-= framesize;
        }
        pthread_mutex_unlock(&enc->mutex);
        return readsize;
    case AENC_CMD_GET_AACINFO:
        *(void**)buf = enc->aaccfgptr;
        break;
    }
    return 0;
}
