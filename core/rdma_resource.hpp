/*
 * Copyright (c) 2016 Shanghai Jiao Tong University.
 *     All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS
 *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 *  express or implied.  See the License for the specific language
 *  governing permissions and limitations under the License.
 *
 * For more about this software visit:
 *
 *      http://ipads.se.sjtu.edu.cn/projects/wukong.html
 *
 */

#pragma once

#include "tcp_adaptor.hpp"

#pragma GCC diagnostic warning "-fpermissive"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <endian.h>
#include <byteswap.h>
#include <getopt.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <vector>
#include <pthread.h>

#include "timer.hpp"


#ifdef HAS_RDMA

struct config_t {
    const char *dev_name;         /* IB device name */
    char *server_name;            /* server host name */
    u_int32_t tcp_port;           /* server TCP port */
    int ib_port;                  /* local IB port to work with */
    int gid_idx;                  /* gid index to use */
};

/* structure to exchange data which is needed to connect the QPs */
struct cm_con_data_t {
    uint64_t addr;                /* Buffer address */
    uint32_t rkey;                /* Remote key */
    uint32_t qp_num;              /* QP number */
    uint16_t lid;                 /* LID of the IB port */
    uint8_t gid[16];              /* gid */
} __attribute__ ((packed));

/* structure of system resources */
struct dev_resource {
    struct ibv_device_attr device_attr; /* Device attributes */
    struct ibv_port_attr port_attr;     /* IB port attributes */
    struct ibv_context *ib_ctx;         /* device handle */

    struct ibv_pd *pd;                  /* PD handle */
    struct ibv_mr *mr;                  /* MR handle for buf */
    char *buf;                          /* memory buffer pointer, used for RDMA and send*/
};

struct QP {
    struct cm_con_data_t remote_props;  /* values to connect to remote side */
    struct ibv_pd *pd;            /* PD handle */
    struct ibv_cq *cq;            /* CQ handle */
    struct ibv_qp *qp;            /* QP handle */
    struct ibv_mr *mr;            /* MR handle for buf */

    struct dev_resource *dev;
};

struct normal_op_req {
    ibv_wr_opcode opcode;
    char *local_buf;
    int size; // default set to sizeof(uint64_t)
    int remote_offset;

    //for atomicity operations
    uint64_t compare_and_add;
    uint64_t swap;

    //for internal usage!!
    struct ibv_send_wr sr;
    struct ibv_sge sge;
};

struct config_t rdma_config = {
    NULL,                         /* dev_name */
    NULL,                         /* server_name */
    19875,                        /* tcp_port */
    1,                            /* ib_port */
    -1                            /* gid_idx */
};

#if __BYTE_ORDER == __LITTLE_ENDIAN

static inline uint64_t htonll(uint64_t x) { return bswap_64(x); }
static inline uint64_t ntohll(uint64_t x) { return bswap_64(x); }

#elif __BYTE_ORDER == __BIG_ENDIAN

static inline uint64_t htonll(uint64_t x) { return x; }
static inline uint64_t ntohll(uint64_t x) { return x; }

#else

#error __BYTE_ORDER is neither __LITTLE_ENDIAN nor __BIG_ENDIAN

#endif


static void dev_resources_init(struct dev_resource *res) {
    memset(res, 0, sizeof * res);
}

static int dev_resources_create(struct dev_resource *res, char* buf, uint64_t size) {
    struct ibv_device **dev_list = NULL;
    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_device *ib_dev = NULL;
    int i;
    int num_devices;
    int mr_flags = 0;
    int rc = 0;

    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) {
        fprintf(stderr, "failed to get IB devices list\n");
        rc = 1;
        goto dev_resources_create_exit;
    }
    /* if there isn't any IB device in host */
    if (!num_devices) {
        fprintf(stderr, "found %d device(s)\n", num_devices);
        rc = 1;
        goto dev_resources_create_exit;
    }
    //  fprintf (stdout, "found %d device(s)\n", num_devices);
    /* search for the specific device we want to work with */
    for (i = 0; i < num_devices; i++) {
        if (!rdma_config.dev_name) {
            rdma_config.dev_name = strdup(ibv_get_device_name(dev_list[i]));
            //fprintf (stdout,
            //           "device not specified, using first one found: %s\n",
            //           rdma_config.dev_name);
        }
        if (!strcmp(ibv_get_device_name(dev_list[i]), rdma_config.dev_name)) {
            ib_dev = dev_list[i];
            break;
        }
    }
    /* if the device wasn't found in host */
    if (!ib_dev) {
        fprintf(stderr, "IB device %s wasn't found\n", rdma_config.dev_name);
        rc = 1;
        goto dev_resources_create_exit;
    }
    /* get device handle */
    res->ib_ctx = ibv_open_device (ib_dev);

    if (!res->ib_ctx) {
        fprintf(stderr, "failed to open device %s\n", rdma_config.dev_name);
        rc = 1;
        goto dev_resources_create_exit;
    }

    // check the atomicity level for rdma operation
    int ret;
    ret = ibv_query_device(res->ib_ctx, &(res->device_attr));
    if (ret) {
        fprintf(stderr, "ibv quert device %d\n", ret);
        assert(false);
    }

    //fprintf(stdout,"The max size can reg: %ld\n",res->device_attr.max_mr_size);

    switch (res->device_attr.atomic_cap) {
    case IBV_ATOMIC_NONE:
        fprintf(stdout, "atomic none\n");
        break;
    case IBV_ATOMIC_HCA:
        fprintf(stdout, "atmoic hca (within device)\n");
        break;
    case IBV_ATOMIC_GLOB:
        fprintf(stdout, "atomic globally\n");
        break;
    default:
        fprintf(stdout, "atomic unknown !!\n");
        assert(false);
    }

    /* We are now done with device list, free it */
    ibv_free_device_list(dev_list);
    dev_list = NULL;
    ib_dev = NULL;
    /* query port properties */
    if (ibv_query_port(res->ib_ctx, rdma_config.ib_port, &res->port_attr)) {
        fprintf (stderr, "ibv_query_port on port %u failed\n", rdma_config.ib_port);
        rc = 1;
        goto dev_resources_create_exit;
    }

    /* allocate Protection Domain */
    res->pd = ibv_alloc_pd(res->ib_ctx);
    if (!res->pd) {
        fprintf (stderr, "ibv_alloc_pd failed\n");
        rc = 1;
        goto dev_resources_create_exit;
    }

    res->buf = buf;
    assert(buf != NULL);
    //  memset (res->buf, 0, size);//TODO!!!

    /* register the memory buffer */
    mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
               IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;//add cmp op

    fprintf(stdout, "registering memory\n");
    res->mr = ibv_reg_mr(res->pd, res->buf, size, mr_flags);
    if (!res->mr) {
        fprintf(stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags);
        rc = 1;
        goto dev_resources_create_exit;
    }

dev_resources_create_exit:
    if (rc) {
        /* Error encountered, cleanup */
        if (res->mr) {
            ibv_dereg_mr(res->mr);
            res->mr = NULL;
        }

        if (res->pd) {
            ibv_dealloc_pd(res->pd);
            res->pd = NULL;
        }

        if (res->ib_ctx) {
            ibv_close_device(res->ib_ctx);
            res->ib_ctx = NULL;
        }

        if (dev_list) {
            ibv_free_device_list(dev_list);
            dev_list = NULL;
        }

    }
    return rc;
}

static void QP_init(struct QP *res) {
    memset (res, 0, sizeof * res);
}

static int QP_create(struct QP *res, struct dev_resource *dev) {
    res->dev = dev;

    struct ibv_qp_init_attr qp_init_attr;

    res->pd = dev->pd;
    res->mr = dev->mr;

    int rc = 0;
    int cq_size = 40;
    res->cq = ibv_create_cq(dev->ib_ctx, cq_size, NULL, NULL, 0);
    if (!res->cq) {
        fprintf(stderr, "failed to create CQ with %u entries\n", cq_size);
        rc = 1;
        goto resources_create_exit;
    }

    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.sq_sig_all = 0;
    qp_init_attr.send_cq = res->cq;
    qp_init_attr.recv_cq = res->cq;
    qp_init_attr.cap.max_send_wr = 128;
    qp_init_attr.cap.max_recv_wr = 128;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    res->qp = ibv_create_qp(res->pd, &qp_init_attr);
    if (!res->qp) {
        fprintf(stderr, "failed to create QP\n");
        rc = 1;
        goto resources_create_exit;
    }

resources_create_exit:
    if (rc) {
        /* Error encountered, cleanup */
        if (res->qp) {
            ibv_destroy_qp(res->qp);
            res->qp = NULL;
        }

        if (res->cq) {
            ibv_destroy_cq(res->cq);
            res->cq  = NULL;
        }
    }

    return rc;
}

static struct cm_con_data_t get_local_con_data(struct QP *res) {
    struct cm_con_data_t local_con_data;
    union ibv_gid my_gid;
    int rc;

    if (rdma_config.gid_idx >= 0) {
        rc = ibv_query_gid(res->dev->ib_ctx, rdma_config.ib_port, rdma_config.gid_idx, &my_gid);
        if (rc) {
            fprintf(stderr, "could not get gid for port %d, index %d\n",
                    rdma_config.ib_port, rdma_config.gid_idx);
            assert(false);
        }
    } else {
        memset(&my_gid, 0, sizeof(my_gid));
    }

    local_con_data.addr = htonll((uintptr_t)(res->dev->buf));
    local_con_data.rkey = htonl(res->mr->rkey);
    local_con_data.qp_num = htonl(res->qp->qp_num);
    local_con_data.lid = htons(res->dev->port_attr.lid);
    memcpy(local_con_data.gid, &my_gid, 16);
    // fprintf(stdout, "\nLocal LID = 0x%x\n", res->port_attr.lid);

    return local_con_data;
}

static int modify_qp_to_init(struct ibv_qp *qp) {
    struct ibv_qp_attr attr;
    int flags;
    int rc;

    memset(&attr, 0, sizeof (attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = rdma_config.ib_port;
    attr.pkey_index = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                           IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;

    flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    rc = ibv_modify_qp(qp, &attr, flags);
    if (rc)
        fprintf(stderr, "failed to modify QP state to INIT\n");

    return rc;
}

static int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn,
                            uint16_t dlid, uint8_t * dgid) {
    struct ibv_qp_attr attr;
    int flags;
    int rc;

    memset(&attr, 0, sizeof (attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_256;
    attr.dest_qp_num = remote_qpn;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 16;
    attr.min_rnr_timer = 0x12;
    attr.ah_attr.is_global = 0;
    attr.ah_attr.dlid = dlid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = rdma_config.ib_port;

    if (rdma_config.gid_idx >= 0) {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.port_num = 1;
        memcpy (&attr.ah_attr.grh.dgid, dgid, 16);
        attr.ah_attr.grh.flow_label = 0;
        attr.ah_attr.grh.hop_limit = 1;
        attr.ah_attr.grh.sgid_index = rdma_config.gid_idx;
        attr.ah_attr.grh.traffic_class = 0;
    }

    flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
            IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    rc = ibv_modify_qp(qp, &attr, flags);
    if (rc)
        fprintf(stderr, "failed to modify QP state to RTR\n");

    return rc;
}

static int modify_qp_to_rts(struct ibv_qp *qp) {
    struct ibv_qp_attr attr;
    int flags;
    int rc;

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 0x12;
    attr.retry_cnt = 6;
    attr.rnr_retry = 0;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 16;
    attr.max_dest_rd_atomic = 16;

    flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    rc = ibv_modify_qp (qp, &attr, flags);
    if (rc)
        fprintf (stderr, "failed to modify QP state to RTS\n");

    return rc;
}

static int connect_qp(struct QP *res, struct cm_con_data_t tmp_con_data) {
    struct cm_con_data_t remote_con_data;
    char temp_char;
    int rc = 0;

    /* exchange using TCP sockets info required to connect QPs */
    remote_con_data.addr = ntohll(tmp_con_data.addr);
    remote_con_data.rkey = ntohl(tmp_con_data.rkey);

    remote_con_data.qp_num = ntohl(tmp_con_data.qp_num);
    remote_con_data.lid = ntohs(tmp_con_data.lid);
    memcpy(remote_con_data.gid, tmp_con_data.gid, 16);
    /* save the remote side attributes, we will need it for the post SR */
    res->remote_props = remote_con_data;

    if (rdma_config.gid_idx >= 0) {
        uint8_t *p = remote_con_data.gid;
        fprintf (stdout,
                 "Remote GID = %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
                 p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9],
                 p[10], p[11], p[12], p[13], p[14], p[15]);
    }

    /* modify the QP to init */
    rc = modify_qp_to_init(res->qp);
    if (rc) {
        fprintf(stderr, "change QP state to INIT failed\n");
        goto connect_qp_exit;
    }

    /* let the client post RR to be prepared for incoming messages */

    /* modify the QP to RTR */
    rc = modify_qp_to_rtr(res->qp, remote_con_data.qp_num,
                          remote_con_data.lid, remote_con_data.gid);
    if (rc) {
        fprintf(stderr, "failed to modify QP state to RTR\n");
        goto connect_qp_exit;
    }

    rc = modify_qp_to_rts(res->qp);
    if (rc) {
        fprintf(stderr, "failed to modify QP state to RTR\n");
        goto connect_qp_exit;
    }

    /* sync to make sure that both sides are in states
       that they can connect to prevent packet loose */

connect_qp_exit:
    return rc;
}

static int post_send(struct QP *res, ibv_wr_opcode opcode, char *local_buf,
                     size_t size, size_t remote_offset, bool signal) {
    struct ibv_send_wr sr;
    struct ibv_sge sge;
    struct ibv_send_wr *bad_wr = NULL;
    int rc;
    /* prepare the scatter/gather entry */
    memset (&sge, 0, sizeof (sge));
    sge.addr = (uintptr_t) local_buf;
    sge.length = size;
    sge.lkey = res->mr->lkey;
    /* prepare the send work request */
    memset (&sr, 0, sizeof (sr));
    sr.next = NULL;
    sr.wr_id = 0;
    sr.sg_list = &sge;
    sr.num_sge = 1;
    sr.opcode = opcode;
    if (signal)
        sr.send_flags = IBV_SEND_SIGNALED ;
    else
        sr.send_flags = 0;

    if (opcode != IBV_WR_SEND) {
        sr.wr.rdma.remote_addr = res->remote_props.addr + remote_offset;
        sr.wr.rdma.rkey = res->remote_props.rkey;
    }

    /* there is a Receive Request in the responder side,
    so we won't get any into RNR flow */
    rc = ibv_post_send(res->qp, &sr, &bad_wr);
    if (rc)
        fprintf (stderr, "failed to post SR\n");
    else {
        /*
        switch (opcode)
        {
        case IBV_WR_SEND:
        fprintf (stdout, "Send Request was posted\n");
        break;
        case IBV_WR_RDMA_READ:
        fprintf (stdout, "RDMA Read Request was posted\n");
        break;
        case IBV_WR_RDMA_WRITE:
        fprintf (stdout, "RDMA Write Request was posted\n");
        break;
        default:
        fprintf (stdout, "Unknown Request was posted\n");
        break;
        }*/
    }

    return rc;
}

static int poll_completion(struct QP *res) {
    struct ibv_wc wc;
    unsigned long start_time_msec;
    unsigned long cur_time_msec;
    struct timeval cur_time;
    int poll_result;
    int rc = 0;

    /* poll the completion for a while before giving up of doing it .. */
    do {
        poll_result = ibv_poll_cq (res->cq, 1, &wc);
    } while ((poll_result == 0));

    if (poll_result < 0) {
        /* poll CQ failed */
        fprintf (stderr, "poll CQ failed\n");
        rc = 1;
    } else if (poll_result == 0) {
        /* the CQ is empty */
        fprintf (stderr, "completion wasn't found in the CQ after timeout\n");
        rc = 1;
    } else {
        /* CQE found */
        //      fprintf (stdout, "completion was found in CQ with status 0x%x\n",
        //               wc.status);
        /* check the completion status (here we don't care about the completion opcode) */
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf (stderr,
                     "got bad completion with status: 0x%x, vendor syndrome: 0x%x\n",
                     wc.status, wc.vendor_err);
            rc = 1;
        }
    }

    return rc;
}

static int batch_poll_completion(struct QP *res, int total) {
    struct ibv_wc wc[32];
    unsigned long start_time_msec;
    unsigned long cur_time_msec;
    struct timeval cur_time;
    int poll_result = 0;
    int rc = 0;

    /* poll the completion for a while before giving up of doing it .. */
    do {
        total = total - poll_result;
        poll_result = ibv_poll_cq (res->cq, total, &wc[0]);
    } while ((poll_result == 0));

    if (poll_result < 0) {
        /* poll CQ failed */
        fprintf (stderr, "poll CQ failed\n");
        rc = 1;
    } else if (poll_result == 0) {
        /* the CQ is empty */
        fprintf (stderr, "completion wasn't found in the CQ after timeout\n");
        rc = 1;
    } else {
        /* CQE found */
        //      fprintf (stdout, "completion was found in CQ with status 0x%x\n",
        //               wc.status);
        /* check the completion status (here we don't care about the completion opcode */
        // if (wc.status != IBV_WC_SUCCESS)
        // {
        //   fprintf (stderr,
        //      "got bad completion with status: 0x%x, vendor syndrome: 0x%x\n",
        //      wc.status, wc.vendor_err);
        //   rc = 1;
        // }
    }

    return poll_result;
}

static inline uint64_t internal_rdtsc(void) {
    uint32_t hi, lo;
    __asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)lo) | (((uint64_t)hi) << 32);
}

// 1 logical queue = 1 client-queue + N-1 server-queues
class Logical_Queue {
    int num_nodes;
    uint64_t cnt; // round robin checking

public:
    Logical_Queue(int num_nodes): num_nodes(num_nodes), cnt(0) { }

    int next_nid() { return (cnt++) % num_nodes; }
};

class RdmaResource {
    const static uint64_t BATCH_FACTOR = 32;

    int num_nodes;
    int num_threads;
    int node_id;

    vector<Logical_Queue> logical_queues; // pre-thread

    struct dev_resource *dev0; //for remote usage
    struct dev_resource *dev1; //for local usage

    vector<string> ipset;

    struct QP **res;

    char *rdma_mem;     // RDMA memory: kvstore | local_buffer | logical_queue
    uint64_t mem_sz;    // mem_sz = kvs_sz + buf_sz * num_threads + msg_sz * num_threads
    uint64_t kvs_sz;
    uint64_t buf_sz;
    uint64_t msg_sz;    // msg_sz = rbf_size * num_nodes

    uint64_t rbf_size;

    int rdmaOp(int dst_tid, int dst_nid, char *buf, uint64_t size,
               uint64_t off, ibv_wr_opcode op) {
        assert(off < mem_sz);
        assert(dst_nid < num_nodes);
        assert(dst_tid < num_threads);

        if (post_send(res[dst_tid] + dst_nid, op, buf, size, off, true)) {
            cout << "ERROR: failed to post request!" << endl;
            assert(false);
        }

        if (poll_completion(res[dst_tid] + dst_nid)) {
            cout << "poll completion failed!" << endl;
            assert(false);
        }

        return 0;
    }

    int batch_rdmaOp(int dst_tid, int dst_nid, char *buf, uint64_t size,
                     uint64_t off, ibv_wr_opcode op) {
        assert(off < mem_sz);
        assert(dst_nid < num_nodes);
        assert(dst_tid < num_threads);

        uint64_t start = internal_rdtsc();
        uint64_t sum = 0;

        for (int i = 0; i < BATCH_FACTOR; i++) {
            if (post_send(res[dst_tid] + dst_nid, op, buf, size, off, true) ) {
                fprintf(stderr, "failed to post request.");
                assert(false);
            }
        }

        int count = 0;
        while (true) {
            int this_round =
                batch_poll_completion(res[dst_tid] + dst_nid, BATCH_FACTOR - count);
            sum = sum + (internal_rdtsc() - start) * this_round;
            count = count + this_round;
            if (count == BATCH_FACTOR)
                break;
        }

        //TODO! we need to
        return sum / BATCH_FACTOR;
    }

    void init() {
        assert(num_nodes > 0 && num_threads > 0 && node_id >= 0);
        cout << "init RDMA devs" << endl;

        dev0 = new dev_resource;
        dev1 = new dev_resource;

        dev_resources_init(dev0);
        dev_resources_init(dev1);

        if (dev_resources_create(dev0, rdma_mem, mem_sz) ) {
            cout << "ERROR: failed to create dev resources" << endl;
            assert(false);
        }

        //cout << "creating remote QPs" << endl;
        res = new struct QP *[num_threads];
        for (int i = 0; i < num_threads ; i++) {
            res[i] = new struct QP[num_nodes];
            for (int j = 0; j < num_nodes; j++) {
                QP_init(res[i] + j);
                if (QP_create(res[i] + j, dev0)) {
                    cout << "ERROR: failed to create QP!" << endl;
                    assert(false);
                }
            }
        }

        // init logical queue
        logical_queues.resize(num_threads, Logical_Queue(num_nodes));

        RemoteMeta.resize(num_nodes);
        for (int i = 0; i < RemoteMeta.size(); i++)
            RemoteMeta[i].resize(num_threads);

        LocalMeta.resize(num_threads);
        for (int i = 0; i < LocalMeta.size(); i++)
            LocalMeta[i].resize(num_nodes);
    }

public:
    RdmaResource(int num_nodes, int num_threads, int node_id, string fname,
                 char *rdma_mem, uint64_t mem_sz,
                 uint64_t kvs_sz, uint64_t buf_sz, uint64_t msg_sz)
        : num_nodes(num_nodes), num_threads(num_threads), node_id(node_id),
          rdma_mem(rdma_mem), mem_sz(mem_sz),
          kvs_sz(kvs_sz), buf_sz(buf_sz), msg_sz(msg_sz) {

        rbf_size = floor(msg_sz / num_nodes, 64);

        // record IPs of ndoes
        ifstream hostfile(fname);
        string ip;
        while (hostfile >> ip)
            ipset.push_back(ip);

        init();
    }

    void servicing() {
        pthread_t update_tid;
        pthread_create(&update_tid, NULL, service_thread, (void *)this);
    }

    void connect() {
        // rolling start from next node, i.e., (node_id + j) % num_nodes
        for (int j = 1; j < num_nodes; j++) {
            int id = (node_id + j) % num_nodes;

            zmq::context_t context(1);
            zmq::socket_t socket(context, ZMQ_REQ);

            int port = global_rdma_port_base + id;
            char address[32] = "";
            snprintf(address, 32, "tcp://%s:%d", ip_of(id).c_str(), port);
            socket.connect(address);

            // request QP info from all threads of other nodes and build an one-to-one connect
            for (int tid = 0; tid < num_threads; tid++) {
                // 16-bit encoding: nid(8) | tid(8)
                uint16_t msg = node_id << 8 | tid;
                zmq::message_t request(2);
                memcpy(request.data(), &msg, 2);
                socket.send(request);

                //get reply
                zmq::message_t reply;
                socket.recv(&reply);
                struct cm_con_data_t remote_con_data;
                memcpy(&remote_con_data, (char *)reply.data(), sizeof(remote_con_data));

                // a one-to-one mapping between the same thread on each node
                if (connect_qp(res[tid] + id, remote_con_data) ) {
                    fprintf (stderr, "failed to connect QPs\n");
                    assert(false);
                    exit(-1);
                }
            }
        }

        cout << "RDMA connect QP done." << endl;
    }

    string ip_of(int sid) { return ipset[sid]; }

    inline char *get_kvstore() { return rdma_mem; }

    inline uint64_t get_kvstore_size() { return kvs_sz; }

    inline char *get_buffer(int dst_tid) {
        return (char *)(rdma_mem + kvs_sz + buf_sz * dst_tid);
    }

    uint64_t get_buffer_size() { return buf_sz; }



    // 0 on success, -1 otherwise
    int RdmaRead(int dst_tid, int dst_nid, char *local,
                 uint64_t size, uint64_t off) {
        return rdmaOp(dst_tid, dst_nid, local, size, off, IBV_WR_RDMA_READ);
    }

    int RdmaWrite(int dst_tid, int dst_nid, char *local,
                  uint64_t size, uint64_t off) {
        return rdmaOp(dst_tid, dst_nid, local, size, off, IBV_WR_RDMA_WRITE);
    }

    int RdmaCmpSwap(int dst_tid, int dst_nid, char *local,
                    uint64_t compare, uint64_t swap,
                    uint64_t size, uint64_t off) {
        struct QP *r = res[dst_tid] + dst_nid;
        assert(r != NULL);

        struct ibv_send_wr sr;
        struct ibv_sge sge;
        struct ibv_send_wr *bad_wr = NULL;
        int rc;

        memset(&sge, 0, sizeof(sge));
        sge.addr = (uintptr_t)local;
        sge.length = sizeof(uint64_t);
        sge.lkey = r->mr->lkey;

        memset(&sr, 0, sizeof(sr));
        sr.next = NULL;
        sr.wr_id = 0;
        sr.sg_list = &sge;
        sr.num_sge = 1;
        sr.opcode = IBV_WR_ATOMIC_CMP_AND_SWP;
        sr.send_flags = IBV_SEND_SIGNALED;
        sr.wr.atomic.remote_addr = r->remote_props.addr
                                   + off;//this field is uint64_t
        sr.wr.atomic.rkey = r->remote_props.rkey;
        sr.wr.atomic.compare_add = compare;
        sr.wr.atomic.swap = swap;

        rc = ibv_post_send(r->qp, &sr, &bad_wr);
        if (rc) {
            fprintf(stderr, "failed to post SR CAS\n");
        } else {

        }
        // if(poll_completion(r) ){
        //   fprintf(stderr,"poll completion failed\n");
        //   assert(false);
        // }
        //TODO! we need to
        return 0;
    }

    // the service thread is used to answer the query about QP info
    static void *service_thread(void * arg) {
        RdmaResource *rdma = (RdmaResource *)arg;

        zmq::context_t context(1);
        zmq::socket_t socket(context, ZMQ_REP);

        int port = global_rdma_port_base + rdma->node_id;
        char address[32] = "";
        snprintf(address, 32, "tcp://%s:%d", rdma->ip_of(rdma->node_id).c_str(), port);
        socket.bind(address);

        // wait the connect request from all threads of other nodes
        for (int i = 0; i < (rdma->num_nodes - 1) * rdma->num_threads; i++) {
            zmq::message_t request;
            socket.recv(&request);

            // 16-bit encoding: nid(8) | tid(8)
            int remote_nid = *(uint16_t *)request.data() >> 8;
            int remote_tid = *(uint16_t *)request.data() & 0xFF;
            struct cm_con_data_t local_con_data =
                get_local_con_data((rdma->res)[remote_tid] + remote_nid);

            zmq::message_t reply(sizeof(local_con_data));
            memcpy(reply.data(), &local_con_data, sizeof(local_con_data));
            socket.send(reply);
        }
    }

    //used to send message to remote queue
    struct RemoteQueueMeta {
        uint64_t remote_tail; // directly write to remote_tail of remote machine
        pthread_spinlock_t remote_lock;
        char padding1[64];

        RemoteQueueMeta() {
            remote_tail = 0;
            pthread_spin_init(&remote_lock, 0);
        }
        void lock() {
            pthread_spin_lock(&remote_lock);
        }
        bool trylock() {
            return pthread_spin_trylock(&remote_lock);
        }
        void unlock() {
            pthread_spin_unlock(&remote_lock);
        }
    };

    struct LocalQueueMeta {
        uint64_t local_tail; // recv from here
        pthread_spinlock_t local_lock;
        char padding1[64];

        LocalQueueMeta() {
            local_tail = 0;
            pthread_spin_init(&local_lock, 0);
        }
        void lock() {
            pthread_spin_lock(&local_lock);
        }
        bool trylock() {
            return pthread_spin_trylock(&local_lock);
        }
        void unlock() {
            pthread_spin_unlock(&local_lock);
        }
    };

    std::vector<std::vector<RemoteQueueMeta>> RemoteMeta; // RemoteMeta[0..m-1][0..t-1]
    std::vector<std::vector<LocalQueueMeta>> LocalMeta;  // LocalMeta[0..t-1][0..m-1]

    uint64_t inline floor(uint64_t original, uint64_t n) {
        assert(n != 0);
        return original - original % n;
    }

    uint64_t inline ceil(uint64_t original, uint64_t n) {
        assert(n != 0);
        if (original % n == 0)
            return original;
        return original - original % n + n;
    }

    uint64_t start_of_recv_queue(int local_tid, int remote_mid) {
        //[t0,m0][t0,m1] [t0,m5], [t1,m0],...
        uint64_t result = kvs_sz + buf_sz * num_threads; // skip kvstore and read_buffer
        result = result + rbf_size * (local_tid * num_nodes + remote_mid);
        return result;
    }

    void rbfSend(int local_tid, int remote_mid, int remote_tid, const char * str_ptr, uint64_t str_size) {
        RemoteQueueMeta * meta = &RemoteMeta[remote_mid][remote_tid];
        meta->lock();
        uint64_t remote_start = start_of_recv_queue(remote_tid, node_id);
        if (node_id == remote_mid) {
            char * ptr = rdma_mem + remote_start;
            uint64_t tail = meta->remote_tail;
            (meta->remote_tail) += sizeof(uint64_t) * 2 + ceil(str_size, sizeof(uint64_t));
            meta->unlock();

            *((uint64_t*)(ptr + (tail) % rbf_size )) = str_size;
            tail += sizeof(uint64_t);
            for (uint64_t i = 0; i < str_size; i++) {
                *(ptr + (tail + i) % rbf_size) = str_ptr[i];
            }
            tail += ceil(str_size, sizeof(uint64_t));
            *((uint64_t*)(ptr + (tail) % rbf_size)) = str_size;
        } else {
            uint64_t total_write_size = sizeof(uint64_t) * 2 + ceil(str_size, sizeof(uint64_t));
            char* local_buffer = get_buffer(local_tid);
            *((uint64_t*)local_buffer) = str_size;
            local_buffer += sizeof(uint64_t);
            memcpy(local_buffer, str_ptr, str_size);
            local_buffer += ceil(str_size, sizeof(uint64_t));
            *((uint64_t*)local_buffer) = str_size;
            uint64_t tail = meta->remote_tail;
            meta->remote_tail = meta->remote_tail + total_write_size;
            meta->unlock();
            if (tail / rbf_size == (tail + total_write_size - 1) / rbf_size ) {
                uint64_t remote_msg_offset = remote_start + (tail % rbf_size);
                RdmaWrite(local_tid, remote_mid, get_buffer(local_tid), total_write_size, remote_msg_offset);
            } else {
                uint64_t length1 = rbf_size - (tail % rbf_size);
                uint64_t length2 = total_write_size - length1;
                uint64_t remote_msg_offset1 = remote_start + (tail % rbf_size);
                uint64_t remote_msg_offset2 = remote_start;
                RdmaWrite(local_tid, remote_mid, get_buffer(local_tid), length1, remote_msg_offset1);
                RdmaWrite(local_tid, remote_mid, get_buffer(local_tid) + length1, length2, remote_msg_offset2);
            }
        }
    }

    bool check_rbf_msg(int local_tid, int mid) {
        LocalQueueMeta *meta = &LocalMeta[local_tid][mid];
        char *rbf_ptr = rdma_mem + start_of_recv_queue(local_tid, mid);
        uint64_t msg_size = *(volatile uint64_t*)(rbf_ptr + meta->local_tail % rbf_size );
        uint64_t skip_size = sizeof(uint64_t) + ceil(msg_size, sizeof(uint64_t));
        //   volatile uint64_t * msg_end_ptr=(uint64_t*)(rbf_ptr+ (meta->local_tail+skip_size)%rbf_size);
        //   wait for longer time
        //   if(msg_size==0 || *msg_end_ptr !=msg_size){
        //       return false;
        //   }

        if (msg_size == 0)
            return false;
        return true;
    }

    std::string fetch_rbf_msg(int local_tid, int mid) {
        LocalQueueMeta * meta = &LocalMeta[local_tid][mid];

        char * rbf_ptr = rdma_mem + start_of_recv_queue(local_tid, mid);
        uint64_t msg_size = *(volatile uint64_t*)(rbf_ptr + meta->local_tail % rbf_size );
        uint64_t t1 = timer::get_usec();
        //clear head
        *(uint64_t*)(rbf_ptr + (meta->local_tail) % rbf_size) = 0;

        uint64_t skip_size = sizeof(uint64_t) + ceil(msg_size, sizeof(uint64_t));
        volatile uint64_t * msg_end_ptr = (uint64_t*)(rbf_ptr + (meta->local_tail + skip_size) % rbf_size);
        while (*msg_end_ptr != msg_size) {
            //timer::cpu_relax(10);
            uint64_t tmp = *msg_end_ptr;
            if (tmp != 0 && tmp != msg_size) {
                printf("waiting for %ld, but actually %ld\n", msg_size, tmp);
                exit(0);
            }
        }
        //clear tail
        *msg_end_ptr = 0;
        uint64_t t2 = timer::get_usec();
        //copy from (meta->local_tail+sizeof(uint64_t) , meta->local_tail+sizeof(uint64_t)+ msg_size )
        //      or
        std::string result;
        result.reserve(msg_size);
        {
            size_t msg_head = (meta->local_tail + sizeof(uint64_t)) % rbf_size;
            size_t msg_tail = (meta->local_tail + sizeof(uint64_t) + msg_size) % rbf_size;
            if (msg_head < msg_tail) {
                result.append(rbf_ptr + msg_head, msg_size);
                memset(rbf_ptr + msg_head, 0, ceil(msg_size, sizeof(uint64_t)));
            } else {
                result.append(rbf_ptr + msg_head, msg_size - msg_tail);
                result.append(rbf_ptr, msg_tail);
                memset(rbf_ptr + msg_head, 0, msg_size - msg_tail);
                memset(rbf_ptr, 0, ceil(msg_tail, sizeof(uint64_t)));
            }
        }
        //   for(uint64_t i=0;i<ceil(msg_size,sizeof(uint64_t));i++){
        //     char * tmp=rbf_ptr+(meta->local_tail+sizeof(uint64_t)+i)%rbf_size;
        //     if(i<msg_size)
        //       result.push_back(*tmp);
        //     //clear data
        //     *tmp=0;
        //   }

        meta->local_tail += 2 * sizeof(uint64_t) + ceil(msg_size, sizeof(uint64_t));
        uint64_t t3 = timer::get_usec();
        return result;
    }

    std::string rbfRecv(int tid) {
        while (true) {
            // NOTE: a logical queue = (N-1) * physical queue
            // check the queues for other nodes in round robin
            int nid = logical_queues[tid].next_nid();
            if (check_rbf_msg(tid, nid))
                return fetch_rbf_msg(tid, nid);
        }
    }

    bool rbfTryRecv(int tid, std::string &msg) {
        for (int nid = 0; nid < num_nodes; nid++) {
            if (check_rbf_msg(tid, nid)) {
                msg = fetch_rbf_msg(tid, nid);
                return true;
            }
        }
        return false;
    }
}; // end of class RdmaResource

#else

class RdmaResource {
public:
    RdmaResource(int num_nodes, int num_threads, int node_id, string fname,
                 char *rdma_mem, uint64_t mem_sz,
                 uint64_t kvs_sz, uint64_t buf_sz, uint64_t msg_sz) {
        cout << "This system is compiled without RDMA support." << endl;
        assert(false);
    }

    void servicing() {
        cout << "This system is compiled without RDMA support." << endl;
        assert(false);
    }

    void connect() {
        cout << "This system is compiled without RDMA support." << endl;
        assert(false);
    }

    string ip_of(int sid) {
        cout << "This system is compiled without RDMA support." << endl;
        assert(false);
        return string();
    }

    char *get_kvstore() {
        cout << "This system is compiled without RDMA support." << endl;
        assert(false);
        return NULL;
    }

    uint64_t get_kvstore_size() {
        cout << "This system is compiled without RDMA support." << endl;
        assert(false);
        return 0ul;
    }

    inline char *get_buffer(int dst_tid) {
        cout << "This system is compiled without RDMA support." << endl;
        assert(false);
        return NULL;
    }

    uint64_t get_buffer_size() {
        cout << "This system is compiled without RDMA support." << endl;
        assert(false);
        return 0ul;
    }

    int RdmaRead(int dst_tid, int dst_nid, char *local,
                 uint64_t size, uint64_t remote_offset) {
        cout << "This system is compiled without RDMA support." << endl;
        assert(false);
        return 0;
    }

    int RdmaWrite(int dst_tid, int dst_nid, char *local,
                  uint64_t size, uint64_t remote_offset) {
        cout << "This system is compiled without RDMA support." << endl;
        assert(false);
        return 0;
    }

    int RdmaCmpSwap(int dst_tid, int dst_nid, char *local,
                    uint64_t compare, uint64_t swap,
                    uint64_t size, uint64_t off) {
        cout << "This system is compiled without RDMA support." << endl;
        assert(false);
        return 0;
    }

    void rbfSend(int local_tid, int remote_mid, int remote_tid,
                 const char *str_ptr, uint64_t str_size) {
        cout << "This system is compiled without RDMA support." << endl;
        assert(false);
    }

    std::string rbfRecv(int local_tid) {
        cout << "This system is compiled without RDMA support." << endl;
        assert(false);
        return string();
    }

    bool rbfTryRecv(int local_tid, std::string & ret) {
        cout << "This system is compiled without RDMA support." << endl;
        assert(false);
        return false;
    }

}; // end of class RdmaResource

#endif