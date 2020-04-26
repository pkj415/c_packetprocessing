/* Flowenv */
#include <nfp.h>
#include <stdint.h>

#include <pkt/pkt.h>
#include <net/eth.h>
#include <net/ip.h>
#include <net/udp.h>
#include <nfp/me.h>
#include <nfp/mem_atomic.h>
#include <nfp/mem_bulk.h>
#include <pcie/compat.h>
#include <pcie/pcie.h>
#include <blm.h>

/*
 * Mapping between channel and TM queue
 */
#ifndef NBI
#define NBI 0
#endif

#define UDP_PACKET_SZ_BYTES 64

/* DEBUG MACROS */

__volatile __shared __emem uint32_t debug[8192*64]; // _export
__volatile __shared __emem uint32_t debug_idx; // __export

#define DEBUG(_a, _b, _c, _d) do { \
    __xrw uint32_t _idx_val = 4; \
    __xwrite uint32_t _dvals[4]; \
    mem_test_add(&_idx_val, \
            (__mem40 void *)&debug_idx, sizeof(_idx_val)); \
    _dvals[0] = _a; \
    _dvals[1] = _b; \
    _dvals[2] = _c; \
    _dvals[3] = _d; \
    mem_write_atomic(_dvals, (__mem40 void *)\
                    (debug + (_idx_val % (1024 * 64))), sizeof(_dvals)); \
    } while(0)

#define DEBUG_MEM(_a, _len) do { \
    int i = 0; \
    __xrw uint32_t _idx_val = 4; \
    __xwrite uint32_t _dvals[4]; \
    mem_test_add(&_idx_val, \
            (__mem40 void *)&debug_idx, sizeof(_idx_val)); \
    _dvals[0] = 0x87654321; \
    _dvals[1] = (uint64_t)_a >> 32; \
    _dvals[2] = (uint64_t)_a & 0xffffffff; \
    _dvals[3] = _len; \
    mem_write_atomic(_dvals, (__mem40 void *)\
                    (debug + (_idx_val % (1024 * 64))), sizeof(_dvals)); \
    for (i = 0; i < (_len+15)/16; i++) { \
      mem_test_add(&_idx_val, \
              (__mem40 void *)&debug_idx, sizeof(_idx_val)); \
      _dvals[0] = i*16 < _len ? *((__mem40 uint32_t *)_a+i*4) : 0x12345678; \
      _dvals[1] = i*16 + 4 < _len ? *((__mem40 uint32_t *)_a+i*4+1) : 0x12345678; \
      _dvals[2] = i*16 + 8 < _len ? *((__mem40 uint32_t *)_a+i*4+2) : 0x12345678; \
      _dvals[3] = i*16 + 12 < _len ? *((__mem40 uint32_t *)_a+i*4+3) : 0x12345678; \
      mem_write_atomic(_dvals, (__mem40 void *)\
                      (debug + (_idx_val % (1024 * 64))), sizeof(_dvals)); \
    } } while(0)

// __declspec(shared ctm) is one copy shared by all threads in an ME, in CTM
// __declspec(shared export ctm) is one copy shared by all MEs in an island in CTM (CTM default scope for 'export' of island)
// __declspec(shared export imem) is one copy shared by all MEs on the chip in IMU (IMU default scope for 'export' of global)

__packed struct pkt_hdr {
    struct {
        uint32_t mac_timestamp;
        uint32_t mac_prepend;
    };
    struct eth_hdr pkt;
    struct ip4_hdr ip_header;
    struct udp_hdr udp_header;
};

struct pkt_rxed {
    struct nbi_meta_catamaran nbi_meta;
    struct pkt_hdr            pkt_hdr;
};

__declspec(shared cls) volatile uint64_t rx_buf_start;
__declspec(shared cls) volatile struct ring_meta rx_meta;

__declspec(shared cls) volatile uint64_t shadow_tail;
__declspec(local_mem) volatile uint64_t shadow_tail_lc, shadow_tail_prev_lc, rx_meta_len_lc;

__mem40 struct pkt_hdr *
receive_packet( struct pkt_rxed *pkt_rxed,
                size_t size )
{
    __xread struct pkt_rxed pkt_rxed_in;
    int island, pnum;
    int pkt_off;
    __mem40 struct pkt_hdr *pkt_hdr;

    pkt_nbi_recv(&pkt_rxed_in, sizeof(pkt_rxed->nbi_meta));
    pkt_rxed->nbi_meta = pkt_rxed_in.nbi_meta;

    pkt_off  = PKT_NBI_OFFSET;
    island   = pkt_rxed->nbi_meta.pkt_info.isl;
    pnum     = pkt_rxed->nbi_meta.pkt_info.pnum;
    pkt_hdr  = pkt_ctm_ptr40(island, pnum, pkt_off);

    // mem_read32(&(pkt_rxed_in.pkt_hdr), pkt_hdr, sizeof(pkt_rxed_in.pkt_hdr));
    // pkt_rxed->pkt_hdr = pkt_rxed_in.pkt_hdr;

    return pkt_hdr;

    // return pkt_ctm_ptr40(island, pnum, pkt_off);
}

void
rewrite_packet(__mem40 struct pkt_hdr *pkt_hdr )
{
    uint32_t temp;
    struct eth_addr temp_eth_addr;

    // Swap desitnation MAC and source MAC.
    temp_eth_addr = pkt_hdr->pkt.dst;
    pkt_hdr->pkt.dst = pkt_hdr->pkt.src;
    pkt_hdr->pkt.src = temp_eth_addr;

    // Swap desitnation IP and source IP.
    temp = pkt_hdr->ip_header.dst;
    pkt_hdr->ip_header.dst = pkt_hdr->ip_header.src;
    pkt_hdr->ip_header.src = temp;
}

void
drop_packet( struct nbi_meta_catamaran *nbi_meta)
{
    int island, pnum;

    island = nbi_meta->pkt_info.isl;
    pnum   = nbi_meta->pkt_info.pnum;
    pkt_ctm_free(island, pnum);
}

/*
 * Fill out all the common parts of the DMA command structure, plus
 * other setup required for DMA engines tests.
 *
 * Once setup, the caller only needs to patch in the PCIe address and
 * is ready to go.
 */
__intrinsic static void
pcie_dma_setup(__gpr struct nfp_pcie_dma_cmd *cmd,
               int signo, uint32_t len, uint32_t dev_addr_hi, uint32_t dev_addr_lo)
{
    unsigned int meid = __MEID;

    struct nfp_pcie_dma_cfg cfg;
    __xwrite struct nfp_pcie_dma_cfg cfg_wr;
    unsigned int mode_msk_inv;
    unsigned int mode;

    /* Zero the descriptor. Same size for 3200 and 6000 */
    cmd->__raw[0] = 0;
    cmd->__raw[1] = 0;
    cmd->__raw[2] = 0;
    cmd->__raw[3] = 0;

    /* We just write config register 0 and 1. no one else is using them */
    cfg.__raw = 0;
    cfg.target_64_even = 1;
    cfg.cpp_target_even = 7;
    cfg.target_64_odd = 1;
    cfg.cpp_target_odd = 7;

    cfg_wr = cfg;
    pcie_dma_cfg_set_pair(0, 0, &cfg_wr);

    /* Signalling setup */
    mode_msk_inv = ((1 << NFP_PCIE_DMA_CMD_DMA_MODE_shf) - 1);
    mode = (((meid & 0xF) << 13) | (((meid >> 4) & 0x3F) << 7) |
            ((__ctx() & 0x7) << 4) | signo);
    cmd->__raw[1] = ((mode << NFP_PCIE_DMA_CMD_DMA_MODE_shf) |
                     (cmd->__raw[1] & mode_msk_inv));

    cmd->cpp_token = 0;
    cmd->cpp_addr_hi = dev_addr_hi;
    cmd->cpp_addr_lo = dev_addr_lo;
    /* On the 6k the length is length - 1 */
    cmd->length = len - 1;
}

void dma_packet(__mem40 void *addr, uint8_t dma_len) {
    __gpr struct nfp_pcie_dma_cmd dma_cmd;
    __xwrite struct nfp_pcie_dma_cmd dma_cmd_wr;
    SIGNAL cmpl_sig, enq_sig;

    pcie_dma_setup(&dma_cmd,
        __signal_number(&cmpl_sig),
        dma_len,
        (uint32_t)((uint64_t)addr >> 32), (uint32_t)((uint64_t)addr & 0xffffffff));

    dma_cmd.pcie_addr_hi = shadow_tail_lc >> 32;
    dma_cmd.pcie_addr_lo = shadow_tail_lc & 0xffffffff;

    dma_cmd_wr = dma_cmd;
    __pcie_dma_enq(0, &dma_cmd_wr, NFP_PCIE_DMA_TOPCI_MED,
                     sig_done, &enq_sig);
    wait_for_all(&cmpl_sig, &enq_sig);

    shadow_tail_lc += dma_len;
    if (shadow_tail_lc == (rx_buf_start + rx_meta_len_lc)) {
        shadow_tail_lc = rx_buf_start;
    }
}

int
main(void)
{
    struct pkt_rxed pkt_rxed; /* The packet header received by the thread */
    __mem40 void *pkt_off;    /* The packet in the CTM */

    uint8_t pkt_len, pkt_len_ctm, pkt_len_mu, first_dma_len;
    __mem40 struct pkt_hdr *pkt_hdr;
    char buffer_full = 0;

    uint64_t curr_rx_head;

    rx_meta.head = 0;
    rx_meta.tail = 0;
    rx_meta.len = 0;
    rx_meta.buffer_full_cnt = 0;
    shadow_tail = 0;
    ctx_swap();

    while (rx_meta.head == 0 || rx_meta.tail == 0 || rx_meta.len == 0) {
    }

    if (ctx() != 0) {
        while (shadow_tail == 0)
          ctx_swap();
    } else {
        shadow_tail = rx_meta.tail;
        rx_buf_start = rx_meta.head;
    }

    rx_meta_len_lc = rx_meta.len;

    for (;;) {
        /* Receive a packet */

        pkt_hdr = receive_packet(&pkt_rxed, sizeof(pkt_rxed));
        pkt_off  = pkt_ctm_ptr40(pkt_rxed.nbi_meta.pkt_info.isl,
          pkt_rxed.nbi_meta.pkt_info.pnum,
          PKT_NBI_OFFSET+2*MAC_PREPEND_BYTES);

        // TODO - Accessing pkt_hdr directly will be too slow. Fix this, read once and use it.
        if (pkt_hdr->pkt.type==0x88cc) {
          // Drop LLDP packets.
          drop_packet(&pkt_rxed.nbi_meta);
          continue;
        }

        if (pkt_rxed.nbi_meta.pkt_info.len != UDP_PACKET_SZ_BYTES + 2*MAC_PREPEND_BYTES) {
          drop_packet(&pkt_rxed.nbi_meta);
          continue;
        }

        pkt_len = pkt_rxed.nbi_meta.pkt_info.len - 2*MAC_PREPEND_BYTES;
        pkt_len_ctm = pkt_ctm_data_size(pkt_len, PKT_NBI_OFFSET, PKT_CTM_SIZE_256);
        pkt_len_mu = pkt_len - pkt_len_ctm;

        rewrite_packet(pkt_hdr);

        // DMA the part of packet in CTM.
        buffer_full = 1;
        while (1) {

            curr_rx_head = rx_meta.head;
            shadow_tail_lc = shadow_tail;
            if (curr_rx_head <= shadow_tail_lc) {
                if (rx_meta_len_lc - (shadow_tail_lc - curr_rx_head) > pkt_len) {
                  // Can DMA.
                  break;
                }
            } else {
                if (curr_rx_head - shadow_tail > pkt_len) {
                  // Can DMA
                  break;
                }
            }
            //if (buffer_full) {
            //    // TODO - Make this atomic add.
            //    rx_meta.buffer_full_cnt += 1;
            //    buffer_full = 0;
            //}
        }

        shadow_tail_prev_lc = shadow_tail_lc;
        shadow_tail_lc += pkt_len;
        if (shadow_tail_lc >= (rx_buf_start + rx_meta_len_lc)) {
            shadow_tail_lc = rx_buf_start + (shadow_tail_lc - (rx_buf_start + rx_meta_len_lc));
        }

        shadow_tail = shadow_tail_lc;
        shadow_tail_lc = shadow_tail_prev_lc;

        first_dma_len = pkt_len_ctm;

        if ((shadow_tail_lc + (uint64_t)first_dma_len) > (rx_buf_start + (uint64_t)rx_meta_len_lc)) {
            first_dma_len = rx_buf_start + rx_meta_len_lc - shadow_tail_lc;
        }

        dma_packet(pkt_off, first_dma_len);
        if (first_dma_len != pkt_len_ctm)
            dma_packet((__mem40 char *)pkt_off + first_dma_len, pkt_len_ctm-first_dma_len);

        // DMA the part of packet in MU buffer
        if (pkt_len_mu != 0) {
            first_dma_len = pkt_len_mu;

            if ((shadow_tail_lc + (uint64_t)pkt_len_mu) > (rx_buf_start + (uint64_t)rx_meta_len_lc)) {
                first_dma_len = rx_buf_start + rx_meta_len_lc - shadow_tail_lc;
            }

            dma_packet((__mem40 void*)pkt_rxed.nbi_meta.pkt_info.muptr, first_dma_len);
            if (first_dma_len != pkt_len_mu)
                dma_packet((__mem40 char *)pkt_rxed.nbi_meta.pkt_info.muptr + first_dma_len, pkt_len_mu-first_dma_len);
        }

        while (rx_meta.tail != shadow_tail_prev_lc) {
        }

        rx_meta.tail = shadow_tail_lc;

        blm_buf_free(pkt_rxed.nbi_meta.pkt_info.muptr, pkt_rxed.nbi_meta.pkt_info.bls);
        pkt_ctm_free(pkt_rxed.nbi_meta.pkt_info.isl, pkt_rxed.nbi_meta.pkt_info.pnum);
    }

    return 0;
}

/* -*-  Mode:C; c-basic-offset:4; tab-width:4 -*- */
