/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Crane Che <cranechu@gmail.com>
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/sysinfo.h>

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/crc32.h"
#include "spdk/rpc.h"
#include "spdk_internal/log.h"
#include "spdk/lib/nvme/nvme_internal.h"
#include "driver.h"


#define US_PER_S              (1000ULL*1000ULL)
#define MIN(X,Y)              ((X) < (Y) ? (X) : (Y))

#ifndef BIT
#define BIT(a)                (1UL << (a))
#endif /* BIT */

// the global configuration of the driver
#define DCFG_VERIFY_READ      (BIT(0))


//// shared data
///////////////////////////////

#define DRIVER_IO_TOKEN_NAME      "driver_io_token"
#define DRIVER_CRC32_TABLE_NAME   "driver_crc32_table"
#define DRIVER_GLOBAL_CONFIG_NAME "driver_global_config"

// TODO: support multiple namespace
static uint64_t g_driver_table_size = 0;
static uint64_t* g_driver_io_token_ptr = NULL;
static uint32_t* g_driver_csum_table_ptr = NULL;
static uint64_t* g_driver_global_config_ptr = NULL;

static int memzone_reserve_shared_memory(uint64_t table_size)
{
  if (spdk_process_is_primary())
  {
    assert(g_driver_io_token_ptr == NULL);
    assert(g_driver_csum_table_ptr == NULL);

    // get the shared memory for token
    SPDK_INFOLOG(SPDK_LOG_NVME, "create token table, size: %ld\n", table_size);
    g_driver_table_size = table_size;
    g_driver_csum_table_ptr = spdk_memzone_reserve(DRIVER_CRC32_TABLE_NAME,
                                                   table_size,
                                                   0, SPDK_MEMZONE_NO_IOVA_CONTIG);
    g_driver_io_token_ptr = spdk_memzone_reserve(DRIVER_IO_TOKEN_NAME,
                                                 sizeof(uint64_t),
                                                 0, 0);
  }
  else
  {
    // find the shared memory for token
    g_driver_table_size = table_size;
    g_driver_io_token_ptr = spdk_memzone_lookup(DRIVER_IO_TOKEN_NAME);
    g_driver_csum_table_ptr = spdk_memzone_lookup(DRIVER_CRC32_TABLE_NAME);
  }

  if (g_driver_csum_table_ptr == NULL)
  {
    SPDK_ERRLOG("memory is not large enough to keep CRC32 of the whole drive data. Data verification is disabled\n");
  }
  
  if (g_driver_io_token_ptr == NULL)
  {
    SPDK_ERRLOG("fail to find memzone space\n");
    return -1;
  }

  return 0;
}

void crc32_clear(uint64_t lba, uint64_t lba_count, int sanitize, int uncorr)
{
  int c = uncorr ? 0xff : 0;
  size_t len = lba_count*sizeof(uint32_t);
  
  if (sanitize == true)
  {
    assert(lba == 0);
    assert(g_driver_table_size != 0); //Namspace instance not exist, you may need to add nvme0n1 in the fixture list
    SPDK_DEBUGLOG(SPDK_LOG_NVME, "clear the whole table\n");
    len = g_driver_table_size;
  }

  if (g_driver_csum_table_ptr != NULL)
  {
    SPDK_DEBUGLOG(SPDK_LOG_NVME, "clear checksum table, lba 0x%lx, c %d, len %ld\n",
                  lba, c, len);
    memset(&g_driver_csum_table_ptr[lba], c, len);
  }
}

static void crc32_fini(void)
{
  if (spdk_process_is_primary())
  {
    spdk_memzone_free(DRIVER_IO_TOKEN_NAME);
    spdk_memzone_free(DRIVER_CRC32_TABLE_NAME);
  }
  g_driver_io_token_ptr = NULL;
  g_driver_csum_table_ptr = NULL;
}


////module: buffer
///////////////////////////////

void* buffer_init(size_t bytes, uint64_t *phys_addr)
{
  void* buf = spdk_dma_zmalloc(bytes, 0x1000, phys_addr);

  SPDK_DEBUGLOG(SPDK_LOG_NVME, "buffer: alloc ptr at %p, size %ld\n",
               buf, bytes);

  assert(buf != NULL);
  return buf;
}

static inline uint32_t buffer_calc_csum(uint64_t* ptr, int len)
{
  uint32_t crc = spdk_crc32c_update(ptr, len, 0);

  //reserve 0: nomapping
  //reserve 0xffffffff: uncorrectable
  if (crc == 0) crc = 1;
  if (crc == 0xffffffff) crc = 0xfffffffe;
  
  return crc;
}

static void buffer_fill_data(void* buf,
                             uint64_t lba,
                             uint32_t lba_count,
                             uint32_t lba_size)
{
  // token is keeping increasing, so every write has different data
  uint64_t token = __atomic_fetch_add(g_driver_io_token_ptr,
                                      lba_count,
                                      __ATOMIC_SEQ_CST);
  
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "token: %ld\n", token);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "lba count: %d\n", lba_count);

  for (uint32_t i=0; i<lba_count; i++, lba++)
  {
    uint64_t* ptr = (uint64_t*)(buf+i*lba_size);

    //first and last 64bit-words are filled with special data
    ptr[0] = lba;
    ptr[lba_size/sizeof(uint64_t)-1] = token+i;

    //keep crc in memory if allocated
    // suppose device modify data correctly. If the command fail, we cannot
    // tell what part of data is updated, while what not. Even when atomic
    // write is supported, we still cannot tell that.
    if (g_driver_csum_table_ptr != NULL)
    {
      g_driver_csum_table_ptr[lba] = buffer_calc_csum(ptr, lba_size);
    }
  }
}

static int buffer_verify_data(const void* buf,
                              const unsigned long lba_first,
                              const uint32_t lba_count,
                              const uint32_t lba_size)
{
  unsigned long lba = lba_first;

  for (uint32_t i=0; i<lba_count; i++, lba++)
  {
    unsigned long* ptr = (unsigned long*)(buf+i*lba_size);
    uint32_t computed_crc = buffer_calc_csum(ptr, lba_size);
    uint32_t expected_crc = computed_crc;

    // if crc table is not available, just use computed crc as
    //expected crc, to bypass verification
    if (g_driver_csum_table_ptr != NULL)
    {
      expected_crc = g_driver_csum_table_ptr[lba];
    }

    if (expected_crc == 0)
    {
      //no mapping, nothing to verify
      continue;
    }
    
    if (expected_crc == 0xffffffff)
    {
      SPDK_WARNLOG("lba uncorrectable: lba 0x%lx\n", lba);
      return -1;
    }

    if (lba != ptr[0])
    {
      SPDK_WARNLOG("lba mismatch: lba 0x%lx, but got: 0x%lx\n", lba, ptr[0]);
      return -2;
    }
    
    if (computed_crc != expected_crc)
    {
      SPDK_WARNLOG("crc mismatch: lba 0x%lx, expected crc 0x%x, but got: 0x%x\n",
                   lba, expected_crc, computed_crc);
      return -3;
    }
  }
  
  return 0;
}

void buffer_fini(void* buf)
{
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "buffer: free ptr at %p\n", buf);
  assert(buf != NULL);
  spdk_dma_free(buf);
}


////cmd log
///////////////////////////////

// log_table contains latest cmd and cpl and their timestamps
// queue_table traces cmd log tables by queue pairs
// CMD_LOG_DEPTH should be larger than Q depth to keep all outstanding commands.
#define CMD_LOG_DEPTH (2048-1)  // reserved one slot space for tail value
#define CMD_LOG_MAX_Q (16)

struct cmd_log_entry_t {
  // cmd and cpl
  struct timeval time_cmd;
  struct spdk_nvme_cmd cmd;
  struct timeval time_cpl;
  struct spdk_nvme_cpl cpl;

  // for data verification after read
  void* buf;
  uint64_t lba;
  uint16_t lba_count;
  uint32_t lba_size;
  
  // callback to user functions
  spdk_nvme_cmd_cb cb_fn;
  void* cb_arg;

  uint64_t dummy[5];
};
static_assert(sizeof(struct cmd_log_entry_t) == 192, "cacheline aligned");

struct cmd_log_table_t {
  struct cmd_log_entry_t table[CMD_LOG_DEPTH];
  uint32_t tail_index;
  uint32_t dummy[47];
};
static_assert(sizeof(struct cmd_log_table_t) == sizeof(struct cmd_log_entry_t)*(CMD_LOG_DEPTH+1), "cacheline aligned");

#define DRIVER_CMDLOG_TABLE_NAME  "driver_cmdlog_table"
static struct cmd_log_table_t* cmd_log_queue_table;


static unsigned int timeval_to_us(struct timeval* t)
{
  return t->tv_sec*US_PER_S + t->tv_usec;
}


static void cmd_log_qpair_init(uint16_t qid)
{
  assert(qid < CMD_LOG_MAX_Q);

  // set tail to invalid value, means the qpair is empty
  cmd_log_queue_table[qid].tail_index = 0;
}


static void cmd_log_qpair_clear(uint16_t qid)
{
  assert(qid < CMD_LOG_MAX_Q);

  // set tail to invalid value, means the qpair is empty
  cmd_log_queue_table[qid].tail_index = CMD_LOG_DEPTH;
}


static int cmd_log_init(void)
{
  if (spdk_process_is_primary())
  {
    cmd_log_queue_table = spdk_memzone_reserve(DRIVER_CMDLOG_TABLE_NAME,
                                               sizeof(struct cmd_log_table_t)*CMD_LOG_MAX_Q, 
                                               0, SPDK_MEMZONE_NO_IOVA_CONTIG);

    // clear all qpair's cmd log
    for (int i=0; i<CMD_LOG_MAX_Q; i++)
    {
      cmd_log_qpair_clear(i);
    }

    // also init config word with cmdlog
    g_driver_global_config_ptr = spdk_memzone_reserve(DRIVER_GLOBAL_CONFIG_NAME,
                                                      sizeof(uint64_t),
                                                      0, 0);
    *g_driver_global_config_ptr = 0;
  }
  else
  {
    cmd_log_queue_table = spdk_memzone_lookup(DRIVER_CMDLOG_TABLE_NAME);
    g_driver_global_config_ptr = spdk_memzone_lookup(DRIVER_GLOBAL_CONFIG_NAME);
  }

  if (cmd_log_queue_table == NULL)
  {
    fprintf(stderr, "Cannot allocate or find the cmdlog memory!\n");
    return -1;
  }

  return 0;
}


static void cmd_log_finish(void)
{
  spdk_memzone_free(DRIVER_CMDLOG_TABLE_NAME);
  spdk_memzone_free(DRIVER_GLOBAL_CONFIG_NAME);
}


static struct cmd_log_entry_t*
cmd_log_add_cmd(uint16_t qid,
                void* buf,
                uint64_t lba,
                uint16_t lba_count,
                uint32_t lba_size,
                const struct spdk_nvme_cmd* cmd,
                spdk_nvme_cmd_cb cb_fn,
                void *cb_arg)
{
  struct cmd_log_table_t* log_table = &cmd_log_queue_table[qid];
  uint32_t tail_index = log_table->tail_index;
  struct cmd_log_entry_t* log_entry = &log_table->table[tail_index];

  assert(qid < CMD_LOG_MAX_Q);
  assert(log_table != NULL);
  assert(tail_index < CMD_LOG_DEPTH);

  log_entry->buf = buf;
  log_entry->lba = lba;
  log_entry->lba_count = lba_count;
  log_entry->lba_size = lba_size;
  log_entry->cb_fn = cb_fn;
  log_entry->cb_arg = cb_arg;
  memcpy(&log_entry->cmd, cmd, sizeof(struct spdk_nvme_cmd));
  gettimeofday(&log_entry->time_cmd, NULL);
  tail_index += 1;
  if (tail_index == CMD_LOG_DEPTH)
  {
    tail_index = 0;
  }
  log_table->tail_index = tail_index;

  return log_entry;
}

static void cmd_log_add_cpl_cb(void* cb_ctx, const struct spdk_nvme_cpl* cpl)
{
  struct timeval diff;
  struct cmd_log_entry_t* log_entry = (struct cmd_log_entry_t*)cb_ctx;

  assert(cpl != NULL);
  assert(log_entry != NULL);

  //reuse dword2 of cpl as latency value
  gettimeofday(&log_entry->time_cpl, NULL);
  memcpy(&log_entry->cpl, cpl, sizeof(struct spdk_nvme_cpl));
  timersub(&log_entry->time_cpl, &log_entry->time_cmd, &diff);
  (&log_entry->cpl.cdw0)[2] = timeval_to_us(&diff);
  //SPDK_DEBUGLOG(SPDK_LOG_NVME, "cmd completed, cid %d\n", log_entry->cpl.cid);
  
  //verify read data
  if (log_entry->cmd.opc == 2 && log_entry->buf != NULL)
  {
    if ((*g_driver_global_config_ptr & DCFG_VERIFY_READ) != 0)
    {
      int ret = 0;
    
      assert (log_entry->lba_count != 0);
      assert (log_entry->lba_size != 0);
      assert (log_entry->lba_size == 512);

      ret = buffer_verify_data(log_entry->buf,
                               log_entry->lba,
                               log_entry->lba_count,
                               log_entry->lba_size);
      if (ret != 0)
      {
        //Unrecovered Read Error: The read data could not be recovered from the media.
        log_entry->cpl.status.sct = 0x02;
        log_entry->cpl.status.sc = 0x81;
      }
    }
  }
  
  //callback to cython layer
  if (log_entry->cb_fn)
  {
    log_entry->cb_fn(log_entry->cb_arg, &log_entry->cpl);
  }
}


//// probe callbacks
///////////////////////////////

struct cb_ctx {
  struct spdk_nvme_transport_id* trid;
  struct spdk_nvme_ctrlr* ctrlr;
};

static bool probe_cb(void *cb_ctx,
                     const struct spdk_nvme_transport_id *trid,
                     struct spdk_nvme_ctrlr_opts *opts)
{
	if (trid->trtype == SPDK_NVME_TRANSPORT_PCIE)
  {
    struct spdk_nvme_transport_id* target = ((struct cb_ctx*)cb_ctx)->trid;
    if (0 != spdk_nvme_transport_id_compare(target, trid))
    {
      SPDK_ERRLOG("Wrong address %s\n", trid->traddr);
      return false;
    }

    opts->use_cmb_sqs = false;
		SPDK_INFOLOG(SPDK_LOG_NVME, "Attaching to NVMe Controller at %s\n",
                 trid->traddr);
	}
  else
  {
    SPDK_INFOLOG(SPDK_LOG_NVME, "Attaching to NVMe over Fabrics controller at %s:%s: %s\n",
                 trid->traddr, trid->trsvcid, trid->subnqn);
	}

	/* Set io_queue_size to UINT16_MAX, NVMe driver
	 * will then reduce this to MQES to maximize
	 * the io_queue_size as much as possible.
	 */
  opts->io_queue_size = UINT16_MAX;

	/* Set the header and data_digest */
  opts->header_digest = false;
	opts->data_digest = false;

	return true;
}


static void attach_cb(void *cb_ctx,
                      const struct spdk_nvme_transport_id *trid,
                      struct spdk_nvme_ctrlr *ctrlr,
                      const struct spdk_nvme_ctrlr_opts *opts)
{
	const struct spdk_nvme_ctrlr_data *cdata = spdk_nvme_ctrlr_get_data(ctrlr);

  SPDK_INFOLOG(SPDK_LOG_NVME,
               "attached device %s: %s, %d namespaces, pid %d\n",
               trid->traddr, cdata->mn,
               spdk_nvme_ctrlr_get_num_ns(ctrlr),
               getpid());

  ((struct cb_ctx*)cb_ctx)->ctrlr = ctrlr;
}


////rpc
///////////////////////////////

static void* rpc_server(void* args)
{
  int rc = 0;

  SPDK_DEBUGLOG(SPDK_LOG_NVME, "starting rpc server ...\n");
  
  // start the rpc
  rc = spdk_rpc_listen("/var/tmp/spdk.sock");
  if (rc != 0)
  {
    SPDK_ERRLOG("rpc fail to get the sock \n");
    return NULL;
  }

  spdk_rpc_set_state(SPDK_RPC_STARTUP);

  while(1)
  {
    spdk_rpc_accept();
    usleep(100000);
  }

  spdk_rpc_close();
}


static void
rpc_get_nvme_controllers(struct spdk_jsonrpc_request *request,
                         const struct spdk_json_val *params)
{
  struct spdk_json_write_ctx *w;

  w = spdk_jsonrpc_begin_result(request);
  if (w == NULL) {
    return;
  }

  spdk_json_write_array_begin(w);

  for (int i=0; i<CMD_LOG_MAX_Q; i++)
  {
    if (cmd_log_queue_table[i].tail_index < CMD_LOG_DEPTH)
    {
      uint32_t tail = cmd_log_queue_table[i].tail_index;
      struct cmd_log_entry_t* table = cmd_log_queue_table[i].table;

      spdk_json_write_uint32(w, tail);

      // send commands details
      spdk_json_write_array_begin(w);
      for (int j=0; j<4; j++)
      {
        uint32_t index = (tail+CMD_LOG_DEPTH-1-j)%CMD_LOG_DEPTH;
        spdk_json_write_uint32(w, table[index].cmd.opc);
      }
      spdk_json_write_array_end(w);
    }
  }

  spdk_json_write_array_end(w);
  
  spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_nvme_controllers", rpc_get_nvme_controllers, SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)


////driver system
///////////////////////////////

int driver_init(void)
{
  int ret = 0;
  char buf[20];
  struct spdk_env_opts opts;

  //init random sequence reproducible
  srandom(1);
  
  // distribute multiprocessing to different cores
  spdk_env_opts_init(&opts);
  sprintf(buf, "0x%x", 1<<(getpid()%get_nprocs()));
  opts.core_mask = buf;
  opts.shm_id = 0;
  opts.name = "pynvme";
  opts.mem_size = 2048;
  if (spdk_env_init(&opts) < 0)
  {
    fprintf(stderr, "Unable to initialize SPDK env\n");
    return -1;
  }

  // distribute multiprocessing to different cores  
  // log level setup
  spdk_log_set_flag("nvme");
  spdk_log_set_print_level(SPDK_LOG_INFO);

  // start rpc server in primary process only
  if (spdk_process_is_primary())
  {
    pthread_t rpc_t;
    pthread_create(&rpc_t, NULL, rpc_server, NULL);
  }

  // init cmd log
  ret = cmd_log_init();
  if (ret != 0)
  {
    return ret;
  }

  cmd_log_qpair_init(0);

  return ret;
}


int driver_fini(void)
{
  //delete cmd log of admin queue
  if (spdk_process_is_primary())
  {
    cmd_log_qpair_clear(0);
    cmd_log_finish();
    SPDK_DEBUGLOG(SPDK_LOG_NVME, "pynvme driver unloaded.\n");
  }  
	return 0;
}


void driver_config(uint64_t cfg_word)
{
  *g_driver_global_config_ptr = cfg_word;
}


////module: pcie ctrlr
///////////////////////////////

struct spdk_pci_device* pcie_init(struct spdk_nvme_ctrlr* ctrlr)
{
  return spdk_nvme_ctrlr_get_pci_device(ctrlr);
}

int pcie_cfg_read8(struct spdk_pci_device* pci,
                   unsigned char* value,
                   unsigned int offset)
{
  return spdk_pci_device_cfg_read8(pci, value, offset);
}

int pcie_cfg_write8(struct spdk_pci_device* pci,
                    unsigned char value,
                    unsigned int offset)
{
  return spdk_pci_device_cfg_write8(pci, value, offset);
}


////module: nvme ctrlr
///////////////////////////////
struct spdk_nvme_ctrlr* nvme_probe(char* traddr)
{
  struct spdk_nvme_transport_id trid;
  struct cb_ctx cb_ctx;
	int rc;

  SPDK_DEBUGLOG(SPDK_LOG_NVME, "looking for NVMe @%s\n", traddr);

  // device address
  memset(&trid, 0, sizeof(trid));
  if (strchr(traddr, ':') == NULL)
  {
    // tcp/ip address: fixed port to 4420
    trid.trtype = SPDK_NVME_TRANSPORT_TCP;
    trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
    strncpy(trid.traddr, traddr, strlen(traddr)+1);
    strncpy(trid.trsvcid, "4420", 4+1);
    snprintf(trid.subnqn, sizeof(trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);
  }
  else
  {
    // pcie address: contains ':' characters
    trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
    strncpy(trid.traddr, traddr, strlen(traddr)+1);
  }

  cb_ctx.trid = &trid;
  cb_ctx.ctrlr = NULL;
  rc = spdk_nvme_probe(&trid, &cb_ctx, probe_cb, attach_cb, NULL);
  if (rc != 0 || cb_ctx.ctrlr == NULL)
  {
    SPDK_ERRLOG("not found device: %s, rc %d, cb_ctx.ctrlr %p\n",
                trid.traddr, rc, cb_ctx.ctrlr);
    return NULL;
  }
  
  return cb_ctx.ctrlr;
}

struct spdk_nvme_ctrlr* nvme_init(char * traddr)
{
  struct spdk_nvme_ctrlr* ctrlr;

  //enum the device
  ctrlr = nvme_probe(traddr);
  if (ctrlr == NULL)
  {
    return NULL;
  }

  SPDK_DEBUGLOG(SPDK_LOG_NVME, "found device: %s\n", ctrlr->trid.traddr);
  return ctrlr;
}

int nvme_fini(struct spdk_nvme_ctrlr* ctrlr)
{
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "free ctrlr: %s\n", ctrlr->trid.traddr);

  if (ctrlr == NULL)
  {
    return 0;
  }

  // io qpairs should all be deleted before closing master controller 
  if (true == spdk_process_is_primary() &&
      false == TAILQ_EMPTY(&ctrlr->active_io_qpairs))
  {
    return -1;
  }
  
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "close device: %s\n", ctrlr->trid.traddr);
  return spdk_nvme_detach(ctrlr);
}

int nvme_set_reg32(struct spdk_nvme_ctrlr* ctrlr,
                   unsigned int offset,
                   unsigned int value)
{
  return nvme_pcie_ctrlr_set_reg_4(ctrlr, offset, value);
}

int nvme_get_reg32(struct spdk_nvme_ctrlr* ctrlr,
                   unsigned int offset,
                   unsigned int* value)
{
  return nvme_pcie_ctrlr_get_reg_4(ctrlr, offset, value);
}

int nvme_wait_completion_admin(struct spdk_nvme_ctrlr* ctrlr)
{
  return spdk_nvme_ctrlr_process_admin_completions(ctrlr);
}

static void nvme_deallocate_ranges(struct spdk_nvme_dsm_range *ranges,
                                   unsigned int count)
{
  for (unsigned int i=0; i<count; i++)
  {
    SPDK_DEBUGLOG(SPDK_LOG_NVME, "deallocate lba 0x%lx, count %d\n",
                 ranges[i].starting_lba,
                 ranges[i].length);
    crc32_clear(ranges[i].starting_lba, ranges[i].length, 0, 0);
  }
}

int nvme_send_cmd_raw(struct spdk_nvme_ctrlr* ctrlr,
                      struct spdk_nvme_qpair *qpair,
                      unsigned int opcode,
                      unsigned int nsid,
                      void* buf, size_t len,
                      unsigned int cdw10,
                      unsigned int cdw11,
                      unsigned int cdw12,
                      unsigned int cdw13,
                      unsigned int cdw14,
                      unsigned int cdw15,
                      spdk_nvme_cmd_cb cb_fn,
                      void* cb_arg)
{
  uint16_t qid;
  struct spdk_nvme_cmd cmd;
  struct cmd_log_entry_t* log_entry;

  assert(ctrlr != NULL);

  //setup cmd structure
  memset(&cmd, 0, sizeof(struct spdk_nvme_cmd));
  cmd.opc = opcode;
  cmd.nsid = nsid;
  cmd.cdw10 = cdw10;
  cmd.cdw11 = cdw11;
  cmd.cdw12 = cdw12;
  cmd.cdw13 = cdw13;
  cmd.cdw14 = cdw14;
  cmd.cdw15 = cdw15;

  qid = qpair ? qpair->id : 0;
  log_entry = cmd_log_add_cmd(qid, NULL, 0, 0, 0,
                              &cmd, cb_fn, cb_arg);

  if (qpair)
  {
    // update host-side table for the trimed data
    // other write-like operation updates crc32 table in driver wraper
    if (opcode == 9)
    {
      nvme_deallocate_ranges(buf, cdw10+1);
    }
    
    //send io cmd in qpair
    return spdk_nvme_ctrlr_cmd_io_raw(ctrlr, qpair, &cmd, buf, len,
                                      cmd_log_add_cpl_cb, log_entry);
  }
  else
  {
    //not qpair, admin cmd
    return spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, buf, len,
                                         cmd_log_add_cpl_cb, log_entry);
  }
}


void nvme_register_aer_cb(struct spdk_nvme_ctrlr* ctrlr,
                          spdk_nvme_aer_cb aer_cb,
                          void* aer_cb_arg)
{
  spdk_nvme_ctrlr_register_aer_callback(ctrlr, aer_cb, aer_cb_arg);
}

void nvme_register_timeout_cb(struct spdk_nvme_ctrlr* ctrlr,
                              spdk_nvme_timeout_cb timeout_cb,
                              unsigned int timeout)
{
  spdk_nvme_ctrlr_register_timeout_callback(
      ctrlr, (uint64_t)timeout*US_PER_S, timeout_cb, NULL);
}

int nvme_cpl_is_error(const struct spdk_nvme_cpl* cpl)
{
  return spdk_nvme_cpl_is_error(cpl);
}


////module: qpair
///////////////////////////////

struct spdk_nvme_qpair *qpair_create(struct spdk_nvme_ctrlr* ctrlr,
                                      int prio, int depth)
{
  struct spdk_nvme_qpair* qpair;
  struct spdk_nvme_io_qpair_opts opts;

  //user options
  opts.qprio = prio;
  opts.io_queue_size = depth;
  opts.io_queue_requests = depth*2;

  qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, &opts, sizeof(opts));
  if (qpair == NULL)
  {
    SPDK_ERRLOG("alloc io qpair fail\n");
    return NULL;
  }

  // limited qpair count
  if (qpair->id >= CMD_LOG_MAX_Q)
  {
    SPDK_ERRLOG("not support so many queue pairs\n");
    spdk_nvme_ctrlr_free_io_qpair(qpair);
    return NULL;
  }

  cmd_log_qpair_init(qpair->id);
  return qpair;
}

int qpair_wait_completion(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
  return spdk_nvme_qpair_process_completions(qpair, max_completions);
}

int qpair_get_id(struct spdk_nvme_qpair* q)
{
  // q NULL is admin queue
  return q ? q->id : 0;
}

int qpair_free(struct spdk_nvme_qpair* q)
{
  if (q == NULL)
  {
    return 0;
  }
  
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "free qpair: %d\n", q->id);
  cmd_log_qpair_clear(q->id);

  return spdk_nvme_ctrlr_free_io_qpair(q);
}


////module: namespace
///////////////////////////////

struct spdk_nvme_ns* ns_init(struct spdk_nvme_ctrlr* ctrlr, uint32_t nsid)
{
  struct spdk_nvme_ns* ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
  uint64_t nsze = spdk_nvme_ns_get_num_sectors(ns);

  assert(ns != NULL);
  if (0 != memzone_reserve_shared_memory(sizeof(uint32_t)*nsze))
  {
    return NULL;
  }

  return ns;
}

int ns_cmd_read_write(int is_read,
                      struct spdk_nvme_ns* ns,
                      struct spdk_nvme_qpair* qpair,
                      void* buf,
                      size_t len,
                      uint64_t lba,
                      uint16_t lba_count,
                      uint32_t io_flags,
                      spdk_nvme_cmd_cb cb_fn,
                      void* cb_arg)
{
  struct spdk_nvme_cmd cmd;
  struct cmd_log_entry_t* log_entry;
  uint32_t lba_size = spdk_nvme_ns_get_sector_size(ns);

  assert(ns != NULL);
  assert(qpair != NULL);

  //only support 1 namespace now
  assert(ns->id == 1);
  
  //validate data buffer
  assert(buf != NULL);
  assert(lba_size == 512);
  assert(len >= lba_count*lba_size);
  assert((io_flags&0xffff) == 0);
  
  //setup cmd structure
  memset(&cmd, 0, sizeof(struct spdk_nvme_cmd));
  cmd.opc = is_read ? 2 : 1;
  cmd.nsid = ns->id;
  cmd.cdw10 = lba;
  cmd.cdw11 = lba>>32;
  cmd.cdw12 = io_flags | (lba_count-1);
  cmd.cdw13 = 0;
  cmd.cdw14 = 0;
  cmd.cdw15 = 0;

  //fill write buffer with lba, token, and checksum
  if (is_read != true)
  {
    //for write buffer
    buffer_fill_data(buf, lba, lba_count, lba_size);
  }

  //get entry in cmd log
  log_entry = cmd_log_add_cmd(qpair->id, buf, lba, lba_count, lba_size,
                              &cmd, cb_fn, cb_arg);

  //send io cmd in qpair
  return spdk_nvme_ctrlr_cmd_io_raw(ns->ctrlr, qpair, &cmd, buf, len,
                                    cmd_log_add_cpl_cb, log_entry);
}

uint32_t ns_get_sector_size(struct spdk_nvme_ns* ns)
{
  return spdk_nvme_ns_get_sector_size(ns);
}

uint64_t ns_get_num_sectors(struct spdk_nvme_ns* ns)
{
  return spdk_nvme_ns_get_num_sectors(ns);
}

int ns_fini(struct spdk_nvme_ns* ns)
{
  crc32_fini();
  return 0;
}


////module: ioworker
///////////////////////////////

// used for callback
struct ioworker_io_ctx {
  void* data_buf;
  size_t data_buf_len;
  bool is_read;
  struct timeval time_sent;
  struct ioworker_global_ctx* gctx;
};

struct ioworker_global_ctx {
  struct ioworker_args* args;
  struct ioworker_rets* rets;
  struct spdk_nvme_ns* ns;
  struct spdk_nvme_qpair *qpair;
  struct timeval due_time;
  struct timeval io_due_time;
  struct timeval io_delay_time;
  struct timeval time_next_sec;
  uint64_t io_count_till_last_sec;
  uint64_t sequential_lba;
  uint64_t io_count_sent;
  uint64_t io_count_cplt;
  uint32_t last_sec;
  bool flag_finish;
};

#define ALIGN_UP(n, a)    (((n)%(a))?((n)+(a)-((n)%(a))):((n)))
#define ALIGN_DOWN(n, a)  ((n)-((n)%(a)))

static int ioworker_send_one(struct spdk_nvme_ns* ns,
                             struct spdk_nvme_qpair *qpair,
                             struct ioworker_io_ctx* ctx,
                             struct ioworker_global_ctx* gctx);


static inline void timeradd_second(struct timeval* now,
                                     unsigned int seconds,
                                     struct timeval* due)
{
  struct timeval duration;

  duration.tv_sec = seconds;
  duration.tv_usec = 0;
  timeradd(now, &duration, due);
}

static bool ioworker_send_one_is_finish(struct ioworker_args* args,
                                        struct ioworker_global_ctx* c)
{
  struct timeval now;

  // limit by io count, and/or time, which happens first
  if (c->io_count_sent == args->io_count)
  {
    SPDK_DEBUGLOG(SPDK_LOG_NVME, "ioworker finish, sent %ld io\n", c->io_count_sent);
    return true;
  }

  assert(c->io_count_sent < args->io_count);
  gettimeofday(&now, NULL);
  if (true == timercmp(&now, &c->due_time, >))
  {
    SPDK_DEBUGLOG(SPDK_LOG_NVME, "ioworker finish, due time %ld us\n", c->due_time.tv_usec);
    return true;
  }

  return false;
}

static void ioworker_one_io_throttle(struct ioworker_global_ctx* gctx,
                                     struct timeval* now)
{
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "this io due at %ld.%06ld\n",
                gctx->io_due_time.tv_sec, gctx->io_due_time.tv_usec);
  if (true == timercmp(&gctx->io_due_time, now, >))
  {
    //delay usec to meet the IOPS prequisit
    struct timeval diff;
    timersub(&gctx->io_due_time, now, &diff);
    usleep(timeval_to_us(&diff));
  }

  timeradd(&gctx->io_due_time, &gctx->io_delay_time, &gctx->io_due_time);
}

static uint32_t ioworker_get_duration(struct timeval* start,
                                      struct ioworker_global_ctx* gctx)
{
  struct timeval now;
  struct timeval diff;
  uint32_t msec;
  
  gettimeofday(&now, NULL);
  timersub(&now, start, &diff);
  msec = diff.tv_sec * 1000UL;
  return msec + (diff.tv_usec+500)/1000;
}

static uint32_t ioworker_update_rets(struct ioworker_io_ctx* ctx,
                                     struct ioworker_rets* ret,
                                     struct timeval* now)
{
  struct timeval diff;
  uint32_t latency;

  timersub(now, &ctx->time_sent, &diff);
  latency = timeval_to_us(&diff);
  if (latency > ret->latency_max_us)
  {
    ret->latency_max_us = latency;
  }

  if (ctx->is_read == true)
  {
    ret->io_count_read ++;
  }
  else
  {
    ret->io_count_write ++;
  }

  return latency;
}

static inline void ioworker_update_io_count_per_second(
    struct ioworker_global_ctx* gctx, 
    struct ioworker_args* args,
    struct ioworker_rets* rets)
{
  uint64_t current_io_count = rets->io_count_read + rets->io_count_write;
  
  // update to next second
  timeradd_second(&gctx->time_next_sec, 1, &gctx->time_next_sec);
  args->io_counter_per_second[gctx->last_sec ++] = current_io_count - gctx->io_count_till_last_sec;
  gctx->io_count_till_last_sec = current_io_count;
}

static void ioworker_one_cb(void* ctx_in, const struct spdk_nvme_cpl *cpl)
{
  uint32_t latency_us;
  struct timeval now;
  struct ioworker_io_ctx* ctx = (struct ioworker_io_ctx*)ctx_in;
  struct ioworker_args* args = ctx->gctx->args;
  struct ioworker_global_ctx* gctx = ctx->gctx;
  struct ioworker_rets* rets = gctx->rets;

  SPDK_DEBUGLOG(SPDK_LOG_NVME, "one io completed, ctx %p, io delay time: %ld\n",
               ctx, gctx->io_delay_time.tv_usec);

  gctx->io_count_cplt ++;

  // update statistics in ret structure
  gettimeofday(&now, NULL);
  latency_us = ioworker_update_rets(ctx, rets, &now);

  // update io count per latency
  if (args->io_counter_per_latency != NULL)
  {
    args->io_counter_per_latency[MIN(US_PER_S-1, latency_us)] ++;
  }
  
  // throttle IOPS by delay
  if (gctx->io_delay_time.tv_usec != 0)
  {
    ioworker_one_io_throttle(gctx, &now);
  }

  if (true == nvme_cpl_is_error(cpl))
  {
    // terminate ioworker when any error happen
    // only keep the first error code
    uint16_t error = ((*(unsigned short*)(&cpl->status))>>1)&0x7ff;
    SPDK_DEBUGLOG(SPDK_LOG_NVME, "ioworker error happen in cpl\n");
    gctx->flag_finish = true;
    if (rets->error == 0)
    {
      rets->error = error;
    }
  }

  // update io counter per second when required
  if (args->io_counter_per_second != NULL)
  {
    if (true == timercmp(&now, &gctx->time_next_sec, >))
    {
      ioworker_update_io_count_per_second(gctx, args, rets);
    }
  }

  // check if all io are sent
  if (gctx->flag_finish != true)
  {
    //update finish flag
    gctx->flag_finish = ioworker_send_one_is_finish(args, gctx);
  }

  if (gctx->flag_finish != true)
  {
    // send more io
    ioworker_send_one(gctx->ns, gctx->qpair, ctx, gctx);
  }
}

static inline bool ioworker_send_one_is_read(unsigned short read_percentage)
{
  return random()%100 < read_percentage;
}

static uint64_t ioworker_send_one_lba_sequential(struct ioworker_args* args,
                                                 struct ioworker_global_ctx* gctx)
{
  uint64_t ret;

  SPDK_DEBUGLOG(SPDK_LOG_NVME, "gctx lba: %ld, align:%d\n", gctx->sequential_lba, args->lba_align);
  ret = gctx->sequential_lba + args->lba_align;
  if (ret > args->region_end)
  {
    ret = args->region_start;
  }

  return ret;
}

static inline uint64_t ioworker_send_one_lba_random(struct ioworker_args* args)
{
  return (random()%(args->region_end-args->region_start)) + args->region_start;
}

static uint64_t ioworker_send_one_lba(struct ioworker_args* args,
                                      struct ioworker_global_ctx* gctx)
{
  uint64_t ret;

  if (args->lba_random == 0)
  {
    ret = ioworker_send_one_lba_sequential(args, gctx);
    gctx->sequential_lba = ret;
  }
  else
  {
    ret = ioworker_send_one_lba_random(args);
  }

  return ALIGN_DOWN(ret, args->lba_align);
}

static int ioworker_send_one(struct spdk_nvme_ns* ns,
                             struct spdk_nvme_qpair *qpair,
                             struct ioworker_io_ctx* ctx,
                             struct ioworker_global_ctx* gctx)
{
  int ret;
  struct ioworker_args* args = gctx->args;
  bool is_read = ioworker_send_one_is_read(args->read_percentage);
  uint64_t lba_starting = ioworker_send_one_lba(args, gctx);
  uint16_t lba_count = args->lba_size;

  SPDK_DEBUGLOG(SPDK_LOG_NVME, "sending one io, ctx %p, lba %ld\n", ctx, lba_starting);
  assert(ctx->data_buf != NULL);

  ret = ns_cmd_read_write(is_read, ns, qpair,
                          ctx->data_buf, ctx->data_buf_len,
                          lba_starting, lba_count,
                          0,  //do not have more options in ioworkers
                          ioworker_one_cb, ctx);
  if (ret != 0)
  {
    SPDK_DEBUGLOG(SPDK_LOG_NVME, "ioworker error happen in cpl\n");
    gctx->flag_finish = true;
    return ret;
  }

  //sent one io cmd successfully
  gctx->io_count_sent ++;
  ctx->is_read = is_read;
  gettimeofday(&ctx->time_sent, NULL);
  return 0;
}


int ioworker_entry(struct spdk_nvme_ns* ns,
                   struct spdk_nvme_qpair *qpair,
                   struct ioworker_args* args,
                   struct ioworker_rets* rets)
{
  int ret = 0;
  uint64_t nsze = spdk_nvme_ns_get_num_sectors(ns);
  uint32_t sector_size = spdk_nvme_ns_get_sector_size(ns);
  struct timeval test_start;
  struct ioworker_global_ctx gctx;
  struct ioworker_io_ctx* io_ctx = malloc(sizeof(struct ioworker_io_ctx)*args->qdepth);

  //init rets
  rets->io_count_read = 0;
  rets->io_count_write = 0;
  rets->latency_max_us = 0;
  rets->mseconds = 0;
  rets->error = 0;

  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.lba_start = %ld\n", args->lba_start);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.lba_size = %d\n", args->lba_size);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.lba_align = %d\n", args->lba_align);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.lba_random = %d\n", args->lba_random);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.region_start = %ld\n", args->region_start);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.region_end = %ld\n", args->region_end);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.read_percentage = %d\n", args->read_percentage);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.iops = %d\n", args->iops);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.io_count = %ld\n", args->io_count);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.seconds = %d\n", args->seconds);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.qdepth = %d\n", args->qdepth);

  //check args
  assert(ns != NULL);
  assert(args->read_percentage <= 100);
  assert(args->io_count != 0 || args->seconds != 0);
  assert(args->seconds < 24*3600ULL);
  assert(args->lba_size != 0);
  assert(args->region_start < args->region_end);
  assert(args->read_percentage >= 0);
  assert(args->read_percentage <= 100);
  assert(args->qdepth <= CMD_LOG_DEPTH/2);

  // check io size
  if (args->lba_size*sector_size > ns->ctrlr->max_xfer_size)
  {
    SPDK_ERRLOG("IO size is larger than max xfer size, %d\n", ns->ctrlr->max_xfer_size);
    rets->error = 0x0002;  // Invalid Field in Command
    free(io_ctx);
    return -2;
  }

  //revise args
  if (args->io_count == 0)
  {
    args->io_count = (unsigned long)-1;
  }
  if (args->seconds == 0 || args->seconds > 24*3600ULL)
  {
    // run ioworker for 24hr at most
    args->seconds = 24*3600ULL;
  }
  if (args->region_end > nsze)
  {
    args->region_end = nsze;
  }
  
  //adjust region to start_lba's region
  args->region_start = ALIGN_UP(args->region_start, args->lba_align);
  args->region_end = args->region_end - args->lba_size - 1;
  args->region_end = ALIGN_DOWN(args->region_end, args->lba_align);
  if (args->lba_start < args->region_start)
  {
    args->lba_start = args->region_start;
  }
  if (args->io_count < args->qdepth)
  {
    args->qdepth = args->io_count;
  }

  //init global ctx
  memset(&gctx, 0, sizeof(gctx));
  gctx.ns = ns;
  gctx.qpair = qpair;
  gctx.sequential_lba = args->lba_start;
  gctx.io_count_sent = 0;
  gctx.io_count_cplt = 0;
  gctx.flag_finish = false;
  gctx.args = args;
  gctx.rets = rets;
  gettimeofday(&test_start, NULL);
  timeradd_second(&test_start, args->seconds, &gctx.due_time);
  gctx.io_delay_time.tv_sec = 0;
  gctx.io_delay_time.tv_usec = args->iops ? US_PER_S/args->iops : 0;
  timeradd(&test_start, &gctx.io_delay_time, &gctx.io_due_time);
  timeradd_second(&test_start, 1, &gctx.time_next_sec);
  gctx.io_count_till_last_sec = 0;
  gctx.last_sec = 0;

  // sending the first batch of IOs, all remaining IOs are sending
  // in callbacks till end
  for (unsigned int i=0; i<args->qdepth; i++)
  {
    io_ctx[i].data_buf_len = args->lba_size * sector_size;
    io_ctx[i].data_buf = buffer_init(io_ctx[i].data_buf_len, NULL);
    io_ctx[i].gctx = &gctx;
    ioworker_send_one(ns, qpair, &io_ctx[i], &gctx);
  }

  // callbacks check the end condition and mark the flag. Check the
  // flag here if it is time to stop the ioworker and return the
  // statistics data
  while (gctx.io_count_sent != gctx.io_count_cplt ||
         gctx.flag_finish != true)
  {
    //exceed 10 seconds more than the expected test time, abort ioworker
    if (ioworker_get_duration(&test_start, &gctx) >
        args->seconds*1000UL + 10*1000UL)
    {
      //generic error
      ret = -3;
      break;
    }

    // collect completions
    spdk_nvme_qpair_process_completions(qpair, 0);
  }

  // final duration
  rets->mseconds = ioworker_get_duration(&test_start, &gctx);

  //release io ctx
  for (unsigned int i=0; i<args->qdepth; i++)
  {
    buffer_fini(io_ctx[i].data_buf);
  }

  free(io_ctx);
  return ret;
}


////module: log
///////////////////////////////

void log_buf_dump(const char* header, const void* buf, size_t len)
{
  spdk_log_dump(stderr, header, buf, len);
}

void log_cmd_dump(struct spdk_nvme_qpair* qpair, size_t count)
{
  int dump_count = count;
  uint16_t qid = qpair->id;
  struct cmd_log_table_t* log_table = &cmd_log_queue_table[qid];

  assert(qid < CMD_LOG_MAX_Q);
  assert(log_table != NULL);

  if (count == 0 || count > CMD_LOG_DEPTH)
  {
    dump_count = CMD_LOG_DEPTH;
  }

  // cmdlog is NOT SQ/CQ. cmdlog keeps CMD/CPL for script test debug purpose
  SPDK_NOTICELOG("dump qpair %d, latest tail in cmdlog: %d\n",
                 qid, log_table->tail_index);
  for (int i=0; i<dump_count; i++)
  {
    char tmbuf[64];
    struct timeval tv;
    struct tm* time;

    //cmd part
    tv = log_table->table[i].time_cmd;
    time = localtime(&tv.tv_sec);
    strftime(tmbuf, sizeof(tmbuf), "%Y-%m-%d %H:%M:%S", time);
    SPDK_NOTICELOG("index %d, %s.%06ld\n", i, tmbuf, tv.tv_usec);
    nvme_qpair_print_command(qpair, &log_table->table[i].cmd);

    //cpl part
    tv = log_table->table[i].time_cpl;
    time = localtime(&tv.tv_sec);
    strftime(tmbuf, sizeof(tmbuf), "%Y-%m-%d %H:%M:%S", time);
    SPDK_NOTICELOG("index %d, %s.%06ld\n", i, tmbuf, tv.tv_usec);
    nvme_qpair_print_completion(qpair, &log_table->table[i].cpl);
  }
}

void log_cmd_dump_admin(struct spdk_nvme_ctrlr* ctrlr, size_t count)
{
  log_cmd_dump(ctrlr->adminq, count);
}


////module: commands name, SPDK
///////////////////////////////

static const char *
admin_opc_name(uint8_t opc)
{
	switch (opc) {
	case SPDK_NVME_OPC_DELETE_IO_SQ:
		return "Delete I/O Submission Queue";
	case SPDK_NVME_OPC_CREATE_IO_SQ:
		return "Create I/O Submission Queue";
	case SPDK_NVME_OPC_GET_LOG_PAGE:
		return "Get Log Page";
	case SPDK_NVME_OPC_DELETE_IO_CQ:
		return "Delete I/O Completion Queue";
	case SPDK_NVME_OPC_CREATE_IO_CQ:
		return "Create I/O Completion Queue";
	case SPDK_NVME_OPC_IDENTIFY:
		return "Identify";
	case SPDK_NVME_OPC_ABORT:
		return "Abort";
	case SPDK_NVME_OPC_SET_FEATURES:
		return "Set Features";
	case SPDK_NVME_OPC_GET_FEATURES:
		return "Get Features";
	case SPDK_NVME_OPC_ASYNC_EVENT_REQUEST:
		return "Asynchronous Event Request";
	case SPDK_NVME_OPC_NS_MANAGEMENT:
		return "Namespace Management";
	case SPDK_NVME_OPC_FIRMWARE_COMMIT:
		return "Firmware Commit";
	case SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD:
		return "Firmware Image Download";
	case SPDK_NVME_OPC_DEVICE_SELF_TEST:
		return "Device Self-test";
	case SPDK_NVME_OPC_NS_ATTACHMENT:
		return "Namespace Attachment";
	case SPDK_NVME_OPC_KEEP_ALIVE:
		return "Keep Alive";
	case SPDK_NVME_OPC_DIRECTIVE_SEND:
		return "Directive Send";
	case SPDK_NVME_OPC_DIRECTIVE_RECEIVE:
		return "Directive Receive";
	case SPDK_NVME_OPC_VIRTUALIZATION_MANAGEMENT:
		return "Virtualization Management";
	case SPDK_NVME_OPC_NVME_MI_SEND:
		return "NVMe-MI Send";
	case SPDK_NVME_OPC_NVME_MI_RECEIVE:
		return "NVMe-MI Receive";
	case SPDK_NVME_OPC_DOORBELL_BUFFER_CONFIG:
		return "Doorbell Buffer Config";
	case SPDK_NVME_OPC_FORMAT_NVM:
		return "Format NVM";
	case SPDK_NVME_OPC_SECURITY_SEND:
		return "Security Send";
	case SPDK_NVME_OPC_SECURITY_RECEIVE:
		return "Security Receive";
	case SPDK_NVME_OPC_SANITIZE:
		return "Sanitize";
	default:
		if (opc >= 0xC0) {
			return "Vendor specific";
		}
		return "Unknown";
	}
}

static const char *
io_opc_name(uint8_t opc)
{
	switch (opc) {
	case SPDK_NVME_OPC_FLUSH:
		return "Flush";
	case SPDK_NVME_OPC_WRITE:
		return "Write";
	case SPDK_NVME_OPC_READ:
		return "Read";
	case SPDK_NVME_OPC_WRITE_UNCORRECTABLE:
		return "Write Uncorrectable";
	case SPDK_NVME_OPC_COMPARE:
		return "Compare";
	case SPDK_NVME_OPC_WRITE_ZEROES:
		return "Write Zeroes";
	case SPDK_NVME_OPC_DATASET_MANAGEMENT:
		return "Dataset Management";
	case SPDK_NVME_OPC_RESERVATION_REGISTER:
		return "Reservation Register";
	case SPDK_NVME_OPC_RESERVATION_REPORT:
		return "Reservation Report";
	case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
		return "Reservation Acquire";
	case SPDK_NVME_OPC_RESERVATION_RELEASE:
		return "Reservation Release";
	default:
		if (opc >= 0x80) {
			return "Vendor specific";
		}
		return "Unknown command";
	}
}

const char* cmd_name(uint8_t opc, int set)
{
  if (set == 0)
  {
    return admin_opc_name(opc);
  }
  else if (set == 1)
  {
    return io_opc_name(opc);
  }
  else
  {
    return "Unknown command set";
  }
}
