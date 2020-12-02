#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "adev.h"
#include "vdev.h"
#include "codec.h"
#include "ffrdp.h"

#ifdef WIN32
#include <winsock2.h>
#define usleep(t) Sleep((t) / 1000)
#define get_tick_count GetTickCount
#define snprintf       _snprintf
typedef   signed char    int8_t;
typedef unsigned char   uint8_t;
typedef   signed short  int16_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef   signed int    int32_t;
#pragma warning(disable:4996) // disable warnings
#else
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#define SOCKET int
#define closesocket close
#define stricmp strcasecmp
#define strtok_s strtok_r
static uint32_t get_tick_count()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
#endif

#define FFRDP_MSS_SIZE      (1500 - 8) // should align to 4 bytes and <= 1500 - 8
#define FFRDP_MIN_RTO        50
#define FFRDP_MAX_RTO        1000
#define FFRDP_MAX_WAITSND    2048
#define FFRDP_QUERY_CYCLE    500
#define FFRDP_FLUSH_TIMEOUT  500
#define FFRDP_DEAD_TIMEOUT   5000
#define FFRDP_MIN_CWND_SIZE  16
#define FFRDP_DEF_CWND_SIZE  32
#define FFRDP_MAX_CWND_SIZE  64
#define FFRDP_RECVBUF_SIZE  (128 * (FFRDP_MSS_SIZE + 0))
#define FFRDP_UDPSBUF_SIZE  (64  * (FFRDP_MSS_SIZE + 6))
#define FFRDP_UDPRBUF_SIZE  (128 * (FFRDP_MSS_SIZE + 6))
#define FFRDP_SELECT_SLEEP   1
#define FFRDP_SELECT_TIMEOUT 10000
#define FFRDP_USLEEP_TIMEOUT 1000

#define MIN(a, b)               ((a) < (b) ? (a) : (b))
#define MAX(a, b)               ((a) > (b) ? (a) : (b))
#define GET_FRAME_SEQ(f)        (*(uint32_t*)(f)->data >> 8)
#define SET_FRAME_SEQ(f, seq)   do { *(uint32_t*)(f)->data = ((f)->data[0]) | (((seq) & 0xFFFFFF) << 8); } while (0)

enum {
    FFRDP_FRAME_TYPE_FEC0,
    FFRDP_FRAME_TYPE_ACK ,
    FFRDP_FRAME_TYPE_QUERY,
    FFRDP_FRAME_TYPE_FEC3,
    FFRDP_FRAME_TYPE_FEC63 = 63,
};

typedef struct tagFFRDP_FRAME_NODE {
    struct tagFFRDP_FRAME_NODE *next;
    struct tagFFRDP_FRAME_NODE *prev;
    uint16_t size; // frame size
    uint8_t *data; // frame data
    #define FLAG_FIRST_SEND     (1 << 0) // after frame first send, this flag will be set
    #define FLAG_TIMEOUT_RESEND (1 << 1) // data frame wait ack timeout and be resend
    #define FLAG_FAST_RESEND    (1 << 2) // data frame need fast resend when next update
    uint32_t flags;        // frame flags
    uint32_t tick_1sts;    // frame first time send tick
    uint32_t tick_send;    // frame send tick
    uint32_t tick_timeout; // frame ack timeout tick
} FFRDP_FRAME_NODE;

typedef struct {
    uint8_t  recv_buff[FFRDP_RECVBUF_SIZE];
    int32_t  recv_size, recv_head, recv_tail;
    #define FLAG_SERVER    (1 << 0)
    #define FLAG_CONNECTED (1 << 1)
    #define FLAG_FLUSH     (1 << 2)
    uint32_t flags;
    SOCKET   udp_fd;
    struct   sockaddr_in server_addr;
    struct   sockaddr_in client_addr;

    FFRDP_FRAME_NODE *send_list_head;
    FFRDP_FRAME_NODE *send_list_tail;
    FFRDP_FRAME_NODE *recv_list_head;
    FFRDP_FRAME_NODE *recv_list_tail;
    FFRDP_FRAME_NODE *cur_new_node;
    uint32_t          cur_new_size;
    uint32_t          cur_new_tick;
    uint32_t send_seq; // send seq
    uint32_t recv_seq; // send seq
    uint32_t wait_snd; // data frame number wait to send
    uint32_t rttm, rtts, rttd, rto;
    uint32_t swnd, cwnd, ssthresh;
    uint32_t tick_recv_ack;
    uint32_t tick_send_query;
    uint32_t tick_ffrdp_dump;
    uint32_t counter_send_bytes;
    uint32_t counter_recv_bytes;
    uint32_t counter_send_1sttime;
    uint32_t counter_send_failed;
    uint32_t counter_send_query;
    uint32_t counter_resend_fast;
    uint32_t counter_resend_rto;
    uint32_t counter_reach_maxrto;

    uint8_t  fec_txbuf[4 + FFRDP_MSS_SIZE + 2];
    uint8_t  fec_rxbuf[4 + FFRDP_MSS_SIZE + 2];
    uint32_t fec_redundancy;
    uint32_t fec_txseq;
    uint32_t fec_rxseq;
    uint32_t fec_rxcnt;
    uint32_t fec_rxmask;
    uint32_t counter_fec_tx_short;
    uint32_t counter_fec_tx_full;
    uint32_t counter_fec_rx_short;
    uint32_t counter_fec_rx_full;
    uint32_t counter_fec_ok;
    uint32_t counter_fec_failed;
} FFRDPCONTEXT;

static uint32_t ringbuf_write(uint8_t *rbuf, uint32_t maxsize, uint32_t tail, uint8_t *src, uint32_t len)
{
    uint8_t *buf1 = rbuf + tail;
    int      len1 = MIN(maxsize-tail, len);
    uint8_t *buf2 = rbuf;
    int      len2 = len  - len1;
    memcpy(buf1, src + 0   , len1);
    memcpy(buf2, src + len1, len2);
    return len2 ? len2 : tail + len1;
}

static uint32_t ringbuf_read(uint8_t *rbuf, uint32_t maxsize, uint32_t head, uint8_t *dst, uint32_t len)
{
    uint8_t *buf1 = rbuf + head;
    int      len1 = MIN(maxsize-head, len);
    uint8_t *buf2 = rbuf;
    int      len2 = len  - len1;
    if (dst) memcpy(dst + 0   , buf1, len1);
    if (dst) memcpy(dst + len1, buf2, len2);
    return len2 ? len2 : head + len1;
}

static int seq_distance(uint32_t seq1, uint32_t seq2) // calculate seq distance
{
    int c = seq1 - seq2;
    if      (c >=  0x7FFFFF) return c - 0x1000000;
    else if (c <= -0x7FFFFF) return c + 0x1000000;
    else return c;
}

static FFRDP_FRAME_NODE* frame_node_new(int type, int size) // create a new frame node
{
    FFRDP_FRAME_NODE *node = malloc(sizeof(FFRDP_FRAME_NODE) + 4 + size + (type ? 2 : 0));
    if (!node) return NULL;
    memset(node, 0, sizeof(FFRDP_FRAME_NODE));
    node->size    = 4 + size + (type ? 2 : 0);
    node->data    = (uint8_t*)node + sizeof(FFRDP_FRAME_NODE);
    node->data[0] = type;
    return node;
}

static int frame_payload_size(FFRDP_FRAME_NODE *node) {
    return  node->size - 4 - (node->data[0] ? 2 : 0);
}

static void list_enqueue(FFRDP_FRAME_NODE **head, FFRDP_FRAME_NODE **tail, FFRDP_FRAME_NODE *node)
{
    FFRDP_FRAME_NODE *p;
    uint32_t seqnew, seqcur;
    int      dist;
    if (*head == NULL) {
        *head = node;
        *tail = node;
    } else {
        seqnew = GET_FRAME_SEQ(node);
        for (p=*tail; p; p=p->prev) {
            seqcur = GET_FRAME_SEQ(p);
            dist   = seq_distance(seqnew, seqcur);
            if (dist == 0) return;
            if (dist >  0) {
                if (p->next) p->next->prev = node;
                else *tail = node;
                node->next = p->next;
                node->prev = p;
                p->next    = node;
                return;
            }
        }
        node->next = *head;
        node->next->prev = node;
        *head = node;
    }
}

static void list_remove(FFRDP_FRAME_NODE **head, FFRDP_FRAME_NODE **tail, FFRDP_FRAME_NODE *node)
{
    if (node->next) node->next->prev = node->prev;
    else *tail = node->prev;
    if (node->prev) node->prev->next = node->next;
    else *head = node->next;
    free(node);
}

static void list_free(FFRDP_FRAME_NODE **head, FFRDP_FRAME_NODE **tail)
{
    while (*head) list_remove(head, tail, *head);
}

static int ffrdp_sleep(FFRDPCONTEXT *ffrdp, int flag)
{
    if (flag) {
        struct timeval tv;
        fd_set  rs;
        FD_ZERO(&rs);
        FD_SET(ffrdp->udp_fd, &rs);
        tv.tv_sec  = 0;
        tv.tv_usec = FFRDP_SELECT_TIMEOUT;
        if (select((int)ffrdp->udp_fd + 1, &rs, NULL, NULL, &tv) <= 0) return -1;
    } else usleep(FFRDP_USLEEP_TIMEOUT);
    return 0;
}

static int ffrdp_send_data_frame(FFRDPCONTEXT *ffrdp, FFRDP_FRAME_NODE *frame, struct sockaddr_in *dstaddr)
{
    if (frame->size == 4 + FFRDP_MSS_SIZE + 2) {
        *(uint16_t*)(frame->data + 4 + FFRDP_MSS_SIZE) = (uint16_t)ffrdp->fec_txseq++;
        ffrdp->counter_fec_tx_full++;  // full frame
    } else {
        ffrdp->counter_fec_tx_short++; // short frame
    }
    if (sendto(ffrdp->udp_fd, frame->data, frame->size, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in)) != frame->size) return -1;

    if (frame->size == 4 + FFRDP_MSS_SIZE + 2) { // full frame
        uint32_t *psrc = (uint32_t*)frame->data, *pdst = (uint32_t*)ffrdp->fec_txbuf, i;
        for (i=0; i<(4+FFRDP_MSS_SIZE)/sizeof(uint32_t); i++) *pdst++ ^= *psrc++; // make xor fec frame
        if (ffrdp->fec_txseq % ffrdp->fec_redundancy == ffrdp->fec_redundancy - 1) {
            *(uint16_t*)(ffrdp->fec_txbuf + 4 + FFRDP_MSS_SIZE) = ffrdp->fec_txseq++; ffrdp->fec_txbuf[0] = ffrdp->fec_redundancy;
            sendto(ffrdp->udp_fd, ffrdp->fec_txbuf, sizeof(ffrdp->fec_txbuf), 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in)); // send fec frame
            memset(ffrdp->fec_txbuf, 0, sizeof(ffrdp->fec_txbuf)); // clear tx_fecbuf
            ffrdp->counter_fec_tx_full++;
        }
    }
    return 0;
}

static int ffrdp_recv_data_frame(FFRDPCONTEXT *ffrdp, FFRDP_FRAME_NODE *frame)
{
    uint32_t fecseq, fecrdc, *psrc, *pdst, type, i;
    if (frame->size != 4 + FFRDP_MSS_SIZE + 2) { // short frame
        ffrdp->counter_fec_rx_short++; return 0;
    } else {
        ffrdp->counter_fec_rx_full++;
        fecseq = *(uint16_t*)(frame->data + 4 + FFRDP_MSS_SIZE); // full frame
        fecrdc = frame->data[0];
    }
    if (fecseq / fecrdc != ffrdp->fec_rxseq / fecrdc) { //group changed
        memcpy(ffrdp->fec_rxbuf, frame->data, sizeof(ffrdp->fec_rxbuf));
        ffrdp->fec_rxseq = fecseq; ffrdp->fec_rxmask = 1 << (fecseq % fecrdc); ffrdp->fec_rxcnt = 0;
        return fecseq % fecrdc != fecrdc - 1 ? 0 : -1;
    } else ffrdp->fec_rxseq = fecseq; // group not changed
    if (fecseq % fecrdc == fecrdc - 1) { // it's redundance frame
        if (ffrdp->fec_rxcnt == fecrdc - 1) return -1;
        if (ffrdp->fec_rxcnt != fecrdc - 2) { ffrdp->counter_fec_failed++; return -1; }
        type = frame->data[0];
        psrc = (uint32_t*)ffrdp->fec_rxbuf; pdst = (uint32_t*)frame->data;
        for (i=0; i<(4+FFRDP_MSS_SIZE)/sizeof(uint32_t); i++) *pdst++ ^= *psrc++;
        frame->data[0] = type;
        ffrdp->counter_fec_ok++;
    } else if (!(ffrdp->fec_rxmask & (1 << (fecseq % fecrdc)))) { // update fec_rxbuf
        psrc = (uint32_t*)frame->data; pdst = (uint32_t*)ffrdp->fec_rxbuf;
        for (i=0; i<(4+FFRDP_MSS_SIZE)/sizeof(uint32_t); i++) *pdst++ ^= *psrc++;
        ffrdp->fec_rxmask |= 1 << (fecseq % fecrdc); ffrdp->fec_rxcnt++;
    }
    return 0;
}

void* ffrdp_init(char *ip, int port, int server, int fec)
{
#ifdef WIN32
    WSADATA wsaData;
#endif
    unsigned long opt;
    FFRDPCONTEXT *ffrdp = calloc(1, sizeof(FFRDPCONTEXT));
    if (!ffrdp) return NULL;

#ifdef WIN32
    timeBeginPeriod(1);
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed !\n");
        return NULL;
    }
#endif

    ffrdp->swnd           = FFRDP_RECVBUF_SIZE / FFRDP_MSS_SIZE;
    ffrdp->cwnd           = FFRDP_DEF_CWND_SIZE;
    ffrdp->ssthresh       = FFRDP_DEF_CWND_SIZE;
    ffrdp->rtts           = (uint32_t) -1;
    ffrdp->rto            = FFRDP_MIN_RTO;
    ffrdp->fec_redundancy = MAX(0, MIN(fec, 63));
    ffrdp->tick_ffrdp_dump= get_tick_count();

    ffrdp->server_addr.sin_family      = AF_INET;
    ffrdp->server_addr.sin_port        = htons(port);
    ffrdp->server_addr.sin_addr.s_addr = inet_addr(ip);
    ffrdp->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ffrdp->udp_fd < 0) {
        printf("failed to open socket !\n");
        goto failed;
    }

#ifdef WIN32
    opt = 1; ioctlsocket(ffrdp->udp_fd, FIONBIO, &opt); // setup non-block io mode
#else
    fcntl(ffrdp->udp_fd, F_SETFL, fcntl(ffrdp->udp_fd, F_GETFL, 0) | O_NONBLOCK);  // setup non-block io mode
#endif
    opt = FFRDP_UDPSBUF_SIZE; setsockopt(ffrdp->udp_fd, SOL_SOCKET, SO_SNDBUF   , (char*)&opt, sizeof(int)); // setup udp send buffer size
    opt = FFRDP_UDPRBUF_SIZE; setsockopt(ffrdp->udp_fd, SOL_SOCKET, SO_RCVBUF   , (char*)&opt, sizeof(int)); // setup udp recv buffer size
    opt = 1;                  setsockopt(ffrdp->udp_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(int)); // setup reuse addr

    if (server) {
        ffrdp->flags |= FLAG_SERVER;
        if (bind(ffrdp->udp_fd, (struct sockaddr*)&ffrdp->server_addr, sizeof(ffrdp->server_addr)) == -1) {
            printf("failed to bind !\n");
            goto failed;
        }
    }
    return ffrdp;

failed:
    if (ffrdp->udp_fd > 0) closesocket(ffrdp->udp_fd);
    free(ffrdp);
    return NULL;
}

void ffrdp_free(void *ctxt)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    if (!ctxt) return;
    if (ffrdp->udp_fd > 0) closesocket(ffrdp->udp_fd);
    if (ffrdp->cur_new_node) free(ffrdp->cur_new_node);
    list_free(&ffrdp->send_list_head, &ffrdp->send_list_tail);
    list_free(&ffrdp->recv_list_head, &ffrdp->recv_list_tail);
    free(ffrdp);
#ifdef WIN32
    WSACleanup();
    timeEndPeriod(1);
#endif
}

int ffrdp_send(void *ctxt, char *buf, int len)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    int           n = len, size;
    if (  !ffrdp || ((ffrdp->flags & FLAG_SERVER) && (ffrdp->flags & FLAG_CONNECTED) == 0)
       || ((len + FFRDP_MSS_SIZE - 1) / FFRDP_MSS_SIZE + ffrdp->wait_snd > FFRDP_MAX_WAITSND)) {
        if (ffrdp) ffrdp->counter_send_failed++;
        return -1;
    }
    while (n > 0) {
        if (!ffrdp->cur_new_node) ffrdp->cur_new_node = frame_node_new(ffrdp->fec_redundancy, FFRDP_MSS_SIZE);
        if (!ffrdp->cur_new_node) break;
        else SET_FRAME_SEQ(ffrdp->cur_new_node, ffrdp->send_seq);
        size = MIN(n, (FFRDP_MSS_SIZE - (int)ffrdp->cur_new_size));
        memcpy(ffrdp->cur_new_node->data + 4 + ffrdp->cur_new_size, buf, size);
        ffrdp->cur_new_size += size; buf += size; n -= size;
        if (ffrdp->cur_new_size == FFRDP_MSS_SIZE) {
            list_enqueue(&ffrdp->send_list_head, &ffrdp->send_list_tail, ffrdp->cur_new_node);
            ffrdp->send_seq++; ffrdp->wait_snd++;
            ffrdp->cur_new_node = NULL;
            ffrdp->cur_new_size = 0;
        } else ffrdp->cur_new_tick = get_tick_count();
    }
    return len - n;
}

int ffrdp_recv(void *ctxt, char *buf, int len)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    int           ret;
    if (!ctxt) return -1;
    ret = MIN(len, ffrdp->recv_size);
    if (ret > 0) {
        ffrdp->recv_head = ringbuf_read(ffrdp->recv_buff, sizeof(ffrdp->recv_buff), ffrdp->recv_head, (uint8_t*)buf, ret);
        ffrdp->recv_size-= ret; ffrdp->counter_recv_bytes += ret;
    }
    return ret;
}

int ffrdp_isdead(void *ctxt)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    if (!ctxt) return -1;
    if (!ffrdp->send_list_head) return 0;
    if (ffrdp->send_list_head->flags & FLAG_FIRST_SEND) {
        return (int32_t)get_tick_count() - (int32_t)ffrdp->send_list_head->tick_1sts > FFRDP_DEAD_TIMEOUT;
    } else {
        return (int32_t)ffrdp->tick_send_query - (int32_t)ffrdp->tick_recv_ack > FFRDP_DEAD_TIMEOUT;
    }
}

static void ffrdp_recvdata_and_sendack(FFRDPCONTEXT *ffrdp, struct sockaddr_in *dstaddr)
{
    FFRDP_FRAME_NODE *p;
    int32_t dist, recv_mack, recv_wnd, size, i;
    uint8_t data[8];
    while (ffrdp->recv_list_head) {
        dist = seq_distance(GET_FRAME_SEQ(ffrdp->recv_list_head), ffrdp->recv_seq);
        if (dist == 0 && (size = frame_payload_size(ffrdp->recv_list_head)) <= (int)(sizeof(ffrdp->recv_buff) - ffrdp->recv_size)) {
            ffrdp->recv_tail = ringbuf_write(ffrdp->recv_buff, sizeof(ffrdp->recv_buff), ffrdp->recv_tail, ffrdp->recv_list_head->data + 4, size);
            ffrdp->recv_size+= size;
            ffrdp->recv_seq++; ffrdp->recv_seq &= 0xFFFFFF;
            list_remove(&ffrdp->recv_list_head, &ffrdp->recv_list_tail, ffrdp->recv_list_head);
        } else break;
    }
    for (recv_mack=0,i=0,p=ffrdp->recv_list_head; i<=24&&p; i++,p=p->next) {
        dist = seq_distance(GET_FRAME_SEQ(p), ffrdp->recv_seq);
        if (dist <= 24) recv_mack |= 1 << (dist - 1); // dist is obviously > 0
    }
    recv_wnd = (sizeof(ffrdp->recv_buff) - ffrdp->recv_size) / FFRDP_MSS_SIZE;
    recv_wnd = MIN(recv_wnd, 255);
    *(uint32_t*)(data + 0) = (FFRDP_FRAME_TYPE_ACK << 0) | (ffrdp->recv_seq << 8);
    *(uint32_t*)(data + 4) = (recv_mack <<  0);
    *(uint32_t*)(data + 4)|= (recv_wnd  << 24);
    sendto(ffrdp->udp_fd, data, sizeof(data), 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in)); // send ack frame
}

enum { CEVENT_ACK_OK, CEVENT_ACK_TIMEOUT, CEVENT_FAST_RESEND, CEVENT_SEND_FAILED };
static void ffrdp_congestion_control(FFRDPCONTEXT *ffrdp, int event)
{
    switch (event) {
    case CEVENT_ACK_OK:
        if (ffrdp->cwnd < ffrdp->ssthresh) ffrdp->cwnd *= 2;
        else ffrdp->cwnd++;
        ffrdp->cwnd = MIN(ffrdp->cwnd, FFRDP_MAX_CWND_SIZE);
        ffrdp->cwnd = MAX(ffrdp->cwnd, FFRDP_MIN_CWND_SIZE);
        break;
    case CEVENT_ACK_TIMEOUT:
    case CEVENT_SEND_FAILED:
        ffrdp->ssthresh = MAX(ffrdp->cwnd / 2, FFRDP_MIN_CWND_SIZE);
        ffrdp->cwnd     = FFRDP_MIN_CWND_SIZE;
        break;
    case CEVENT_FAST_RESEND:
        ffrdp->ssthresh = MAX(ffrdp->cwnd / 2, FFRDP_MIN_CWND_SIZE);
        ffrdp->cwnd     = ffrdp->ssthresh;
        break;
    }
}

void ffrdp_update(void *ctxt)
{
    FFRDPCONTEXT       *ffrdp   = (FFRDPCONTEXT*)ctxt;
    FFRDP_FRAME_NODE   *node    = NULL, *p = NULL, *t = NULL;
    struct sockaddr_in *dstaddr = NULL, srcaddr;
    uint32_t addrlen = sizeof(srcaddr);
    int32_t  una, mack, ret, got_data = 0, got_query = 0, send_una, send_mack = 0, recv_una, dist, maxack, i;
    uint8_t  data[8];

    if (!ctxt) return;
    dstaddr  = ffrdp->flags & FLAG_SERVER ? &ffrdp->client_addr : &ffrdp->server_addr;
    send_una = ffrdp->send_list_head ? GET_FRAME_SEQ(ffrdp->send_list_head) : 0;
    recv_una = ffrdp->recv_seq;

    if (ffrdp->cur_new_node && ((int32_t)get_tick_count() - (int32_t)ffrdp->cur_new_tick > FFRDP_FLUSH_TIMEOUT || ffrdp->flags & FLAG_FLUSH)) {
        ffrdp->cur_new_node->data[0] = FFRDP_FRAME_TYPE_FEC0;
        ffrdp->cur_new_node->size    = 4 + ffrdp->cur_new_size;
        list_enqueue(&ffrdp->send_list_head, &ffrdp->send_list_tail, ffrdp->cur_new_node);
        ffrdp->send_seq++; ffrdp->wait_snd++;
        ffrdp->cur_new_node = NULL;
        ffrdp->cur_new_size = 0;
    }

    for (i=0,p=ffrdp->send_list_head; i<(int32_t)ffrdp->cwnd&&p; i++,p=p->next) {
        if (!(p->flags & FLAG_FIRST_SEND)) { // first send
            if (ffrdp->swnd > 0) {
                if (ffrdp_send_data_frame(ffrdp, p, dstaddr) != 0) { ffrdp_congestion_control(ffrdp, CEVENT_SEND_FAILED); break; }
                p->tick_1sts = p->tick_send = get_tick_count();
                p->tick_timeout = p->tick_send + ffrdp->rto;
                p->flags       |= FLAG_FIRST_SEND;
                ffrdp->swnd--; ffrdp->counter_send_1sttime++;
            } else if ((int32_t)get_tick_count() - (int32_t)ffrdp->tick_send_query > FFRDP_QUERY_CYCLE) { // query remote receive window size
                data[0] = FFRDP_FRAME_TYPE_QUERY; sendto(ffrdp->udp_fd, data, 1, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in));
                ffrdp->tick_send_query = get_tick_count(); ffrdp->counter_send_query++;
                break;
            }
        } else if ((p->flags & FLAG_FIRST_SEND) && ((int32_t)get_tick_count() - (int32_t)p->tick_timeout > 0 || (p->flags & FLAG_FAST_RESEND))) { // resend
            ffrdp_congestion_control(ffrdp, CEVENT_ACK_TIMEOUT);
            if (ffrdp_send_data_frame(ffrdp, p, dstaddr) != 0) break;
            if (!(p->flags & FLAG_FAST_RESEND)) {
                if (ffrdp->rto == FFRDP_MAX_RTO) {
                    p->tick_send = get_tick_count();
                    p->flags    &=~FLAG_TIMEOUT_RESEND;
                    ffrdp->counter_reach_maxrto++;
                } else p->flags |= FLAG_TIMEOUT_RESEND;
                ffrdp->rto += ffrdp->rto / 2;
                ffrdp->rto  = MIN(ffrdp->rto, FFRDP_MAX_RTO);
                ffrdp->counter_resend_rto++;
            } else {
                p->flags &= ~(FLAG_FAST_RESEND|FLAG_TIMEOUT_RESEND);
                ffrdp->counter_resend_fast++;
            }
            p->tick_timeout+= ffrdp->rto;
        }
    }

    if (ffrdp_sleep(ffrdp, FFRDP_SELECT_SLEEP) != 0) return;
    for (node=NULL;;) { // receive data
        if (!node && !(node = frame_node_new(FFRDP_FRAME_TYPE_FEC3, FFRDP_MSS_SIZE))) break;;
        if ((ret = recvfrom(ffrdp->udp_fd, node->data, node->size, 0, (struct sockaddr*)&srcaddr, &addrlen)) <= 0) break;
        if ((ffrdp->flags & FLAG_SERVER) && (ffrdp->flags & FLAG_CONNECTED) == 0) {
            if (ffrdp->flags & FLAG_CONNECTED) {
                if (memcmp(&srcaddr, &ffrdp->client_addr, sizeof(srcaddr)) != 0) continue;
            } else {
                ffrdp->flags |= FLAG_CONNECTED;
                memcpy(&ffrdp->client_addr, &srcaddr, sizeof(ffrdp->client_addr));
            }
        }

        if (node->data[0] == FFRDP_FRAME_TYPE_FEC0 || (node->data[0] >= FFRDP_FRAME_TYPE_FEC3 && node->data[0] <= FFRDP_FRAME_TYPE_FEC63)) { // data frame
            node->size = ret; // frame size is the return size of recvfrom
            if (ffrdp_recv_data_frame(ffrdp, node) == 0) {
                dist = seq_distance(GET_FRAME_SEQ(node), recv_una);
                if (dist == 0) { recv_una++; }
                if (dist >= 0) { list_enqueue(&ffrdp->recv_list_head, &ffrdp->recv_list_tail, node); node = NULL; }
                got_data = 1;
            }
        } else if (node->data[0] == FFRDP_FRAME_TYPE_ACK ) {
            una  = *(uint32_t*)(node->data + 0) >> 8;
            mack = *(uint32_t*)(node->data + 4) & 0xFFFFFF;
            dist = seq_distance(una, send_una);
            if (dist >= 0) {
                send_una    = una;
                send_mack   = (send_mack >> dist) | mack;
                ffrdp->swnd = node->data[7]; ffrdp->tick_recv_ack = get_tick_count();
            }
        } else if (node->data[0] == FFRDP_FRAME_TYPE_QUERY) got_query = 1;
    }
    if (node) free(node);

    if (got_data || got_query) ffrdp_recvdata_and_sendack(ffrdp, dstaddr); // send ack frame
    if (ffrdp->send_list_head && seq_distance(send_una, GET_FRAME_SEQ(ffrdp->send_list_head)) > 0) { // got ack frame
        for (p=ffrdp->send_list_head; p;) {
            dist = seq_distance(GET_FRAME_SEQ(p), send_una);
            for (i=23; i>=0 && !(send_mack&(1<<i)); i--);
            if (i < 0) maxack = (send_una - 1) & 0xFFFFFF;
            else maxack = (send_una + i + 1) & 0xFFFFFF;

            if (dist > 24 || !(p->flags & FLAG_FIRST_SEND)) break;
            else if (dist < 0 || (dist > 0 && (send_mack & (1 << (dist-1))))) { // this frame got ack
                ffrdp->counter_send_bytes += frame_payload_size(p); ffrdp->wait_snd--;
                ffrdp_congestion_control(ffrdp, CEVENT_ACK_OK);
                if (!(p->flags & FLAG_TIMEOUT_RESEND)) {
                    ffrdp->rttm = (int32_t)get_tick_count() - (int32_t)p->tick_send;
                    if (ffrdp->rtts == (uint32_t)-1) {
                        ffrdp->rtts = ffrdp->rttm;
                        ffrdp->rttd = ffrdp->rttm / 2;
                    } else {
                        ffrdp->rtts = (7 * ffrdp->rtts + 1 * ffrdp->rttm) / 8;
                        ffrdp->rttd = (3 * ffrdp->rttd + 1 * abs((int)ffrdp->rttm - (int)ffrdp->rtts)) / 4;
                    }
                    ffrdp->rto = ffrdp->rtts + 4 * ffrdp->rttd;
                    ffrdp->rto = MAX(FFRDP_MIN_RTO, ffrdp->rto);
                    ffrdp->rto = MIN(FFRDP_MAX_RTO, ffrdp->rto);
                }
                t = p; p = p->next; list_remove(&ffrdp->send_list_head, &ffrdp->send_list_tail, t); continue;
            } else if (seq_distance(maxack, GET_FRAME_SEQ(p)) > 0) {
                ffrdp_congestion_control(ffrdp, CEVENT_FAST_RESEND);
                p->flags |= FLAG_FAST_RESEND;
            }
            p = p->next;
        }
    }
}

void ffrdp_flush(void *ctxt)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    if (ffrdp) ffrdp->flags |= FLAG_FLUSH;
}

void ffrdp_dump(void *ctxt, int clearhistory)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt; int secs;
    if (!ctxt) return;
    secs = ((int32_t)get_tick_count() - (int32_t)ffrdp->tick_ffrdp_dump) / 1000;
    secs = secs ? secs : 1;
    printf("rttm: %u, rtts: %u, rttd: %u, rto: %u\n", ffrdp->rttm, ffrdp->rtts, ffrdp->rttd, ffrdp->rto);
    printf("total_send, total_recv: %.2fMB, %.2fMB\n"    , ffrdp->counter_send_bytes / (1024.0 * 1024), ffrdp->counter_recv_bytes / (1024.0 * 1024));
    printf("averg_send, averg_recv: %.2fKB/s, %.2fKB/s\n", ffrdp->counter_send_bytes / (1024.0 * secs), ffrdp->counter_recv_bytes / (1024.0 * secs));
    printf("recv_size           : %d\n"  , ffrdp->recv_size           );
    printf("flags               : %x\n"  , ffrdp->flags               );
    printf("send_seq            : %u\n"  , ffrdp->send_seq            );
    printf("recv_seq            : %u\n"  , ffrdp->recv_seq            );
    printf("wait_snd            : %u\n"  , ffrdp->wait_snd            );
    printf("swnd, cwnd, ssthresh: %u, %u, %u\n", ffrdp->swnd, ffrdp->cwnd, ffrdp->ssthresh);
    printf("counter_send_1sttime: %u\n"  , ffrdp->counter_send_1sttime);
    printf("counter_send_failed : %u\n"  , ffrdp->counter_send_failed );
    printf("counter_send_query  : %u\n"  , ffrdp->counter_send_query  );
    printf("counter_resend_rto  : %u\n"  , ffrdp->counter_resend_rto  );
    printf("counter_resend_fast : %u\n"  , ffrdp->counter_resend_fast );
    printf("counter_resend_ratio: %.2f%%\n", 100.0 * (ffrdp->counter_resend_rto + ffrdp->counter_resend_fast) / MAX(ffrdp->counter_send_1sttime, 1));
    printf("counter_reach_maxrto: %u\n"  , ffrdp->counter_reach_maxrto);
    printf("fec_txseq           : %d\n"  , ffrdp->fec_txseq           );
    printf("fec_rxseq           : %d\n"  , ffrdp->fec_rxseq           );
    printf("fec_rxmask          : %08x\n", ffrdp->fec_rxmask          );
    printf("counter_fec_tx_short: %u\n"  , ffrdp->counter_fec_tx_short);
    printf("counter_fec_tx_full : %u\n"  , ffrdp->counter_fec_tx_full );
    printf("counter_fec_rx_short: %u\n"  , ffrdp->counter_fec_rx_short);
    printf("counter_fec_rx_full : %u\n"  , ffrdp->counter_fec_rx_full );
    printf("counter_fec_ok      : %u\n"  , ffrdp->counter_fec_ok      );
    printf("counter_fec_failed  : %u\n\n", ffrdp->counter_fec_failed  );
    if (secs > 1 && clearhistory) {
        ffrdp->tick_ffrdp_dump      = get_tick_count();
        ffrdp->counter_send_bytes   = ffrdp->counter_recv_bytes = 0;
        ffrdp->counter_send_1sttime = ffrdp->counter_send_failed = ffrdp->counter_send_query   = 0;
        ffrdp->counter_resend_rto   = ffrdp->counter_resend_fast = ffrdp->counter_reach_maxrto = 0;
        ffrdp->counter_fec_tx_short = ffrdp->counter_fec_tx_full = ffrdp->counter_fec_rx_short = ffrdp->counter_fec_rx_full = ffrdp->counter_fec_ok = ffrdp->counter_fec_failed = 0;
    }
}

int ffrdp_qos(void *ctxt)
{
    int resend_ratio;
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    if (!ctxt) return 0;
    resend_ratio = 100 * (ffrdp->counter_resend_rto + ffrdp->counter_resend_fast) / MAX(ffrdp->counter_send_1sttime, 1);
    ffrdp->counter_resend_rto = ffrdp->counter_resend_fast = ffrdp->counter_reach_maxrto = 0;
    if (ffrdp->rto > 100 && ffrdp->wait_snd > 25) return -1;
    if (ffrdp->rto < 55  && ffrdp->wait_snd < 3  && resend_ratio < 5) return  1;
    return 0;
}

typedef struct {
    #define TS_EXIT             (1 << 0)
    #define TS_START            (1 << 1)
    #define TS_CLIENT_CONNECTED (1 << 2)
    #define TS_KEYFRAME_DROPPED (1 << 3)
    #define TS_ADAPTIVE_BITRATE (1 << 4)
    uint32_t  status;
    pthread_t pthread;

    int channels, samprate, width, height, frate;

    void     *ffrdp;
    void     *adev;
    void     *vdev;
    CODEC    *aenc;
    CODEC    *venc;
    int       port;

    char      avinfostr[256];
    uint8_t   buff[2 * 1024 * 1024];

    #define MAX_BITRATE_LIST_SIZE  100
    int       bitrate_list_buf[MAX_BITRATE_LIST_SIZE];
    int       bitrate_list_size;
    int       bitrate_cur_idx;
    uint32_t  cnt_qos_increase;
    uint32_t  tick_qos_check;
} FFRDPS;

static int ffrdp_send_packet(FFRDPS *ffrdps, char type, uint8_t *buf, int len)
{
    int ret;
    *(int32_t*)buf = (type << 0) | (len << 8);
    ret = ffrdp_send(ffrdps->ffrdp, buf, len + sizeof(int32_t));
    if (ret != len + sizeof(int32_t)) {
        printf("ffrdp_send_packet send packet failed ! %d %d\n", ret, len + sizeof(int32_t));
        return -1;
    } else return 0;
}

static void buf2hexstr(char *str, int len, uint8_t *buf, int size)
{
    char tmp[3];
    int  i;
    for (i=0; i<size; i++) {
        snprintf(tmp, sizeof(tmp), "%02x", buf[i]);
        strncat(str, tmp, len);
    }
}

static void* ffrdps_thread_proc(void *argv)
{
    FFRDPS  *ffrdps = (FFRDPS*)argv;
    uint8_t  buffer[256];
    int      ret;
    while (!(ffrdps->status & TS_EXIT)) {
        if (!(ffrdps->status & TS_START)) { usleep(100*1000); continue; }

        if (!ffrdps->ffrdp) {
            ffrdps->ffrdp = ffrdp_init("0.0.0.0", ffrdps->port, 1, 0);
            if (!ffrdps->ffrdp) { usleep(100*1000); continue; }
        }

        ret = ffrdp_recv(ffrdps->ffrdp, (char*)buffer, sizeof(buffer));
        if (ret > 0) {
            if ((ffrdps->status & TS_CLIENT_CONNECTED) == 0) {
                uint8_t spsbuf[256], ppsbuf[256];
                char    spsstr[256] = "", ppsstr[256] = "";
                int     spslen, ppslen;
                codec_reset(ffrdps->aenc, CODEC_RESET_CLEAR_INBUF|CODEC_RESET_CLEAR_OUTBUF|CODEC_RESET_REQUEST_IDR);
                codec_reset(ffrdps->venc, CODEC_RESET_CLEAR_INBUF|CODEC_RESET_CLEAR_OUTBUF|CODEC_RESET_REQUEST_IDR);
                codec_start(ffrdps->aenc, 1);
                codec_start(ffrdps->venc, 1);
                adev_start (ffrdps->adev, 1);
                vdev_start (ffrdps->vdev, 1);
                spslen = h264enc_getinfo(ffrdps->venc, "sps", spsbuf, sizeof(spsbuf));
                ppslen = h264enc_getinfo(ffrdps->venc, "pps", ppsbuf, sizeof(ppsbuf));
                buf2hexstr(spsstr, sizeof(spsstr), spsbuf, spslen);
                buf2hexstr(ppsstr, sizeof(ppsstr), ppsbuf, ppslen);
                snprintf(ffrdps->avinfostr+sizeof(uint32_t), sizeof(ffrdps->avinfostr)-sizeof(uint32_t),
                    "aenc=%s,channels=%d,samprate=%d;venc=%s,width=%d,height=%d,frate=%d,sps=%s,pps=%s;",
                    ffrdps->aenc->name, ffrdps->channels, ffrdps->samprate, ffrdps->venc->name, ffrdps->width, ffrdps->height, ffrdps->frate, spsstr, ppsstr);
                ret = ffrdp_send_packet(ffrdps, 'I', ffrdps->avinfostr, (int)strlen(ffrdps->avinfostr+sizeof(uint32_t)) + 1);
                if (ret == 0) {
                    ffrdps->status |= TS_CLIENT_CONNECTED;
                    printf("client connected !\n");
                }
            }
        }

        if ((ffrdps->status & TS_CLIENT_CONNECTED)) {
            int readsize, framesize, keyframe;
            readsize = codec_read(ffrdps->aenc, ffrdps->buff + sizeof(int32_t), sizeof(ffrdps->buff) - sizeof(int32_t), &framesize, &keyframe, 0);
            if (readsize > 0 && readsize == framesize && readsize <= 0xFFFFFF) {
                ret = ffrdp_send_packet(ffrdps, 'A', ffrdps->buff, framesize);
            }
            readsize = codec_read(ffrdps->venc, ffrdps->buff + sizeof(int32_t), sizeof(ffrdps->buff) - sizeof(int32_t), &framesize, &keyframe, 0);
            if (readsize > 0 && readsize == framesize && readsize <= 0xFFFFFF) {
                if ((ffrdps->status & TS_KEYFRAME_DROPPED) && !keyframe) {
                    printf("ffrdp key frame has dropped, and current frame is non-key frame, so drop it !\n");
                } else {
                    ret = ffrdp_send_packet(ffrdps, 'V', ffrdps->buff, framesize);
                    if (ret == 0 && keyframe) ffrdps->status &=~TS_KEYFRAME_DROPPED;
                    if (ret != 0 && keyframe) ffrdps->status |= TS_KEYFRAME_DROPPED;
                }
            }
        }

        ffrdp_update(ffrdps->ffrdp);
        if ((ffrdps->status & TS_CLIENT_CONNECTED) && ffrdp_isdead(ffrdps->ffrdp)) {
            printf("client lost !\n");
            codec_start(ffrdps->aenc, 0);
            codec_start(ffrdps->venc, 0);
            adev_start (ffrdps->adev, 0);
            vdev_start (ffrdps->vdev, 0);
            ffrdp_free(ffrdps->ffrdp); ffrdps->ffrdp = NULL;
            ffrdps->status &= ~TS_CLIENT_CONNECTED;
        }

        if ((ffrdps->status & TS_CLIENT_CONNECTED) && (ffrdps->status & TS_ADAPTIVE_BITRATE) && (int32_t)get_tick_count() - (int32_t)ffrdps->tick_qos_check > 1000) {
            int last_idx = ffrdps->bitrate_cur_idx, qos = ffrdp_qos(ffrdps->ffrdp);
            if (qos < 0) {
                ffrdps->bitrate_cur_idx = MAX(ffrdps->bitrate_cur_idx - 1, 0);
                ffrdps->cnt_qos_increase= 0;
            } else if (qos > 0) {
                if (ffrdps->cnt_qos_increase == 3) {
                    ffrdps->bitrate_cur_idx  = MIN(ffrdps->bitrate_cur_idx + 1, ffrdps->bitrate_list_size - 1);
                    ffrdps->cnt_qos_increase = 0;
                } else ffrdps->cnt_qos_increase++;
            }
            if (ffrdps->bitrate_cur_idx != last_idx) {
                ffrdps_reconfig_bitrate(ffrdps, ffrdps->bitrate_list_buf[ffrdps->bitrate_cur_idx]);
            }
            ffrdps->tick_qos_check = get_tick_count();
        }
    }

    ffrdp_free(ffrdps->ffrdp);
    return NULL;
}

void* ffrdps_init(int port, int channels, int samprate, int width, int height, int frate, void *adev, void *vdev, CODEC *aenc, CODEC *venc)
{
    FFRDPS *ffrdps = calloc(1, sizeof(FFRDPS));
    if (!ffrdps) {
        printf("failed to allocate memory for ffrdps !\n");
        return NULL;
    }

    ffrdps->adev     = adev;
    ffrdps->vdev     = vdev;
    ffrdps->aenc     = aenc;
    ffrdps->venc     = venc;
    ffrdps->port     = port;
    ffrdps->channels = channels;
    ffrdps->samprate = samprate;
    ffrdps->width    = width;
    ffrdps->height   = height;
    ffrdps->frate    = frate;

    // create server thread
    pthread_create(&ffrdps->pthread, NULL, ffrdps_thread_proc, ffrdps);
    ffrdps_start(ffrdps, 1);
    return ffrdps;
}

void ffrdps_exit(void *ctxt)
{
    FFRDPS *ffrdps = ctxt;
    if (!ctxt) return;
    adev_start (ffrdps->adev, 0);
    vdev_start (ffrdps->vdev, 0);
    codec_start(ffrdps->aenc, 0);
    codec_start(ffrdps->venc, 0);
    ffrdps->status |= TS_EXIT;
    pthread_join(ffrdps->pthread, NULL);
    free(ctxt);
}

void ffrdps_start(void *ctxt, int start)
{
    FFRDPS *ffrdps = ctxt;
    if (!ctxt) return;
    if (start) {
        ffrdps->status |= TS_START;
    } else {
        ffrdps->status &=~TS_START;
    }
}

void ffrdps_dump(void *ctxt, int clearhistory)
{
    FFRDPS *ffrdps = ctxt;
    if (!ctxt) return;
    ffrdp_dump(ffrdps->ffrdp, clearhistory);
}

void ffrdps_reconfig_bitrate(void *ctxt, int bitrate)
{
    FFRDPS *ffrdps = ctxt;
    if (!ctxt) return;
    h264enc_reconfig(ffrdps->venc, bitrate);
}

void ffrdps_adaptive_bitrate_setup(void *ctxt, int *blist, int n)
{
    FFRDPS *ffrdps = ctxt;
    if (!ctxt) return;
    ffrdps->bitrate_list_size = MIN(n, MAX_BITRATE_LIST_SIZE);
    memcpy(ffrdps->bitrate_list_buf, blist, ffrdps->bitrate_list_size * sizeof(int));
}

void ffrdps_adaptive_bitrate_enable(void *ctxt, int en)
{
    FFRDPS *ffrdps = ctxt;
    if (!ctxt) return;
    ffrdps->bitrate_cur_idx= ffrdps->bitrate_list_size / 2;
    h264enc_reconfig(ffrdps->venc, ffrdps->bitrate_list_buf[ffrdps->bitrate_cur_idx]);
    if (en && ffrdps->bitrate_list_size > 0) {
        ffrdps->tick_qos_check = get_tick_count();
        ffrdps->status |= TS_ADAPTIVE_BITRATE;
    } else {
        ffrdps->status &=~TS_ADAPTIVE_BITRATE;
    }
}