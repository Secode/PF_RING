/*
 * (C) 2003-15 - ntop 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#define _GNU_SOURCE
#include <signal.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <numa.h>

#include "pfring.h"
#include "pfring_zc.h"
#include "pfring_mod_sysdig.h"

#include "zutils.c"

#define ALARM_SLEEP             1
#define MAX_CARD_SLOTS      32768 //Only true for 10G cards, 4096 for igb/1G. (HW limit)
#define PREFETCH_BUFFERS        8
#define QUEUE_LEN            8192
#define POOL_SIZE              16
#define CACHE_LINE_LEN         64
#define MAX_NUM_APP	       32
#define IN_POOL_SIZE          256

pfring_zc_cluster *zc;
pfring_zc_worker *zw;
pfring_zc_queue **inzqs;
pfring_zc_queue **outzqs;
pfring_zc_multi_queue *outzmq; /* fanout */
pfring_zc_buffer_pool *wsp;

u_int32_t alarm_sec_sleep = 1;
u_int32_t num_devices = 0;
u_int32_t num_apps = 0;
u_int32_t num_consumer_queues = 0;
u_int32_t queue_len = QUEUE_LEN;
u_int32_t instances_per_app[MAX_NUM_APP];
char **devices = NULL;

int card_slots = 0;
int mtu = 0;

int cluster_id = -1;
int metadata_len = 0;

int bind_worker_core = -1;
int bind_time_pulse_core = -1;

volatile u_int64_t *pulse_timestamp_ns;

static struct timeval start_time;
u_int8_t wait_for_packet = 1, enable_vm_support = 0, time_pulse = 0, print_interface_stats = 0, proc_stats_only = 0, daemon_mode = 0;
volatile u_int8_t do_shutdown = 0;

u_int8_t n2disk_producer = 0;
u_int32_t n2disk_threads;

/* ******************************** */

#define SET_TS_FROM_PULSE(p, t) { u_int64_t __pts = t; p->ts.tv_sec = __pts >> 32; p->ts.tv_nsec = __pts & 0xffffffff; }

void *time_pulse_thread(void *data) {
  u_int64_t ns;
  struct timespec tn;
#if 1
  u_int64_t pulse_clone = 0;
#endif

  bind2core(bind_time_pulse_core);

  while (likely(!do_shutdown)) {
    /* clock_gettime takes up to 30 nsec to get the time */
    clock_gettime(CLOCK_REALTIME, &tn);

    ns = ((u_int64_t) ((u_int64_t) tn.tv_sec * 1000000000) + (tn.tv_nsec));

#if 1 /* reduce cache thrashing*/ 
    if(ns >= pulse_clone + 100 /* nsec precision (avoid updating each cycle) */ ) {
#endif
      *pulse_timestamp_ns = ((u_int64_t) ((u_int64_t) tn.tv_sec << 32) | tn.tv_nsec);
#if 1
      pulse_clone = ns;
    }
#endif
  }

  return NULL;
}

/* ******************************** */

void print_stats() {
  static u_int8_t print_all = 0;
  static struct timeval last_time;
  static unsigned long long last_tot_recv = 0, last_tot_slave_recv = 0;
  static unsigned long long last_tot_drop = 0, last_tot_slave_drop = 0;
  unsigned long long tot_recv = 0, tot_drop = 0, tot_slave_recv = 0, tot_slave_drop = 0;
  struct timeval end_time;
  char buf1[64], buf2[64], buf3[64], buf4[64];
  pfring_zc_stat stats;
  char stats_buf[1024];
  char time_buf[128];
  double duration;
  int i;

  if(start_time.tv_sec == 0)
    gettimeofday(&start_time, NULL);
  else
    print_all = 1;

  gettimeofday(&end_time, NULL);

  duration = delta_time(&end_time, &start_time);

  for (i = 0; i < num_devices; i++)
    if (pfring_zc_stats(inzqs[i], &stats) == 0)
      tot_recv += stats.recv, tot_drop += stats.drop;

  for (i = 0; i < num_consumer_queues; i++)
    if (pfring_zc_stats(outzqs[i], &stats) == 0)
      tot_slave_recv += stats.recv, tot_slave_drop += stats.drop;

  if (!daemon_mode && !proc_stats_only) {
    trace(TRACE_NORMAL, "=========================\n"
            "Absolute Stats: Recv %s pkts (%s drops) - Forwarded %s pkts (%s drops)\n", 
            pfring_format_numbers((double)tot_recv, buf1, sizeof(buf1), 0),
	    pfring_format_numbers((double)tot_drop, buf2, sizeof(buf2), 0),
	    pfring_format_numbers((double)tot_slave_recv, buf3, sizeof(buf3), 0),
	    pfring_format_numbers((double)tot_slave_drop, buf4, sizeof(buf4), 0)
    );
  }

  snprintf(stats_buf, sizeof(stats_buf), 
           "ClusterId:         %d\n"
           "TotQueues:         %d\n"
           "Applications:      %d\n", 
           cluster_id,
           num_consumer_queues,
           num_apps);

  for (i = 0; i < num_apps; i++)
    snprintf(&stats_buf[strlen(stats_buf)], sizeof(stats_buf)-strlen(stats_buf), 
             "App%dQueues:        %d\n", 
             i, instances_per_app[i]);

  snprintf(&stats_buf[strlen(stats_buf)], sizeof(stats_buf)-strlen(stats_buf),
           "Duration:          %s\n"
  	   "Packets:           %lu\n"
	   "Processed:         %lu\n",
           msec2dhmsm(duration, time_buf, sizeof(time_buf)),
	   (long unsigned int)tot_recv,
	   (long unsigned int)tot_slave_recv);

  if (print_interface_stats) {
    int i;
    u_int64_t tot_if_recv = 0, tot_if_drop = 0;
    for (i = 0; i < num_devices; i++) {
      if (pfring_zc_stats(inzqs[i], &stats) == 0) {
        tot_if_recv += stats.recv;
        tot_if_drop += stats.drop;
        if (!daemon_mode && !proc_stats_only) {
          trace(TRACE_NORMAL, "                %s RX %lu pkts Dropped %lu pkts (%.1f %%)\n", 
                  devices[i], stats.recv, stats.drop, 
	          stats.recv == 0 ? 0 : ((double)(stats.drop*100)/(double)(stats.recv + stats.drop)));
        }
      }
    }
    snprintf(&stats_buf[strlen(stats_buf)], sizeof(stats_buf)-strlen(stats_buf),
             "IFPackets:         %lu\n"
  	     "IFDropped:         %lu\n",
	     (long unsigned int)tot_if_recv, 
	     (long unsigned int)tot_if_drop);
    for (i = 0; i < num_consumer_queues; i++) {
      if (pfring_zc_stats(outzqs[i], &stats) == 0) {
        if (!daemon_mode && !proc_stats_only) {
          trace(TRACE_NORMAL, "                Q %u RX %lu pkts Dropped %lu pkts (%.1f %%)\n", 
                  i, stats.recv, stats.drop, 
	          stats.recv == 0 ? 0 : ((double)(stats.drop*100)/(double)(stats.recv + stats.drop)));
        }
      }
    }
  }

  pfring_zc_set_proc_stats(zc, stats_buf);

  if(print_all && last_time.tv_sec > 0) {
    double delta_msec = delta_time(&end_time, &last_time);
    unsigned long long diff_recv = tot_recv - last_tot_recv;
    unsigned long long diff_drop = tot_drop - last_tot_drop;
    unsigned long long diff_slave_recv = tot_slave_recv - last_tot_slave_recv;
    unsigned long long diff_slave_drop = tot_slave_drop - last_tot_slave_drop;

    if (!daemon_mode && !proc_stats_only) {
      trace(TRACE_NORMAL, "Actual Stats: Recv %s pps (%s drops) - Forwarded %s pps (%s drops)\n",
	      pfring_format_numbers(((double)diff_recv/(double)(delta_msec/1000)),  buf1, sizeof(buf1), 1),
	      pfring_format_numbers(((double)diff_drop/(double)(delta_msec/1000)),  buf2, sizeof(buf2), 1),
	      pfring_format_numbers(((double)diff_slave_recv/(double)(delta_msec/1000)),  buf3, sizeof(buf3), 1),
	      pfring_format_numbers(((double)diff_slave_drop/(double)(delta_msec/1000)),  buf4, sizeof(buf4), 1)
      );
    }
  }
  
  if (!daemon_mode && !proc_stats_only) 
    trace(TRACE_NORMAL, "=========================\n\n");
 
  last_tot_recv = tot_recv, last_tot_slave_recv = tot_slave_recv;
  last_tot_drop = tot_drop, last_tot_slave_drop = tot_slave_drop;
  last_time.tv_sec = end_time.tv_sec, last_time.tv_usec = end_time.tv_usec;
}

/* ******************************** */

void sigproc(int sig) {
  static int called = 0;
  trace(TRACE_NORMAL, "Leaving...\n");
  if(called) return; else called = 1;

  pfring_zc_kill_worker(zw);

  do_shutdown = 1;

  print_stats();
}

/* *************************************** */

void printHelp(void) {
  printf("zbalance_ipc - (C) 2014 ntop.org\n");
  printf("Using PFRING_ZC v.%s\n", pfring_zc_version());
  printf("A master process balancing packets to multiple consumer processes.\n\n");
  printf("Usage: zbalance_ipc -i <device> -c <cluster id> -n <num inst> -M <mtu>\n"
	 "                [-h] [-m <hash mode>] [-S <core id>] [-g <core_id>]\n"
	 "                [-N <num>] [-a] [-q <len>] [-Q <sock list>] [-d] \n"
	 "                [-D <username>] [-P <pid file>] \n\n");
  printf("-h              Print this help\n");
  printf("-i <device>     Device (comma-separated list) Note: use 'Q' as device name to create ingress sw queues\n");
  printf("-c <cluster id> Cluster id\n");
  printf("-n <num inst>   Number of application instances\n"
         "                In case of '-m 1' or '-m 4' it is possible to spread packets across multiple\n"
         "                instances of multiple applications, using a comma-separated list\n");
  printf("-m <hash mode>  Hashing modes:\n"
         "                0 - No hash: Round-Robin (default)\n"
         "                1 - IP hash, or TID (thread id) in case of '-i sysdig'\n"
         "                2 - Fan-out\n"
         "                3 - Fan-out (1st) + Round-Robin (2nd, 3rd, ..)\n"
         "                4 - GTP hash (Inner IP/Port or Seq-Num)\n"
         "                5 - BAD hash (Not completed): vlan,proto,sip,dip,sport,dport\n"
         "                6 - Vlan/ethtype/IP hash\n");
  printf("-M <max MTU>    Size of largest MTU we should handle\n");
  printf("-C <card slots> 4096 (igb) or 32768(ixgbe), defaults to ixgbe. Accepts any value >= igb\n");
  printf("-S <core id>    Enable Time Pulse thread and bind it to a core\n");
  printf("-g <core_id>    Bind this app to a core\n");
  printf("-q <len>        Number of slots in each queue (default: %u)\n", QUEUE_LEN);
  printf("-N <num>        Producer for n2disk multi-thread (<num> threads)\n");
  printf("-a              Active packet wait\n");
  printf("-Q <sock list>  Enable VM support (comma-separated list of QEMU monitor sockets)\n");
  printf("-r              Interval for statistics, defaults to 1 sec.\n");
  printf("-p              Print per-interface and per-queue absolute stats\n");
  printf("-d              Daemon mode\n");
  printf("-D <username>   Drop privileges\n");
  printf("-P <pid file>   Write pid to the specified file (daemon mode only)\n");
  exit(-1);
}

struct compact_eth_hdr {
  unsigned char   h_dest[ETH_ALEN];
  unsigned char   h_source[ETH_ALEN];
  u_int16_t       h_proto;
};

struct compact_ip_hdr {
  u_int8_t      ihl:4,
                version:4;
  u_int8_t      tos;
  u_int16_t     tot_len;
  u_int16_t     id;
  u_int16_t     frag_off;
  u_int8_t      ttl;
  u_int8_t      protocol;
  u_int16_t     check;
  u_int32_t     saddr;
  u_int32_t     daddr;
};

/*
struct in6_addr {
    unsigned char   s6_addr[16];
};
*/
struct compact_ipv6_hdr {
  u_int8_t              priority:4,
                        version:4;
  u_int8_t              flow_lbl[3];
  u_int16_t             payload_len;
  u_int8_t              nexthdr;
  u_int8_t              hop_limit;
  struct                in6_addr saddr;
  struct                in6_addr daddr;
};

struct compact_udp_hdr {
  u_int16_t       sport;
  u_int16_t       dport;
  u_int16_t       len;
  u_int16_t       check;
};

/* ***************************************
 Both ip_vlan_proto_hash and bad_hash look for "all" vlan tags and add them to hash,
 they also look at the eth_type after that and adds it to hash, this will help a minimal
 amount but it's "free" so why not. If it's ipv4/6 we also look at source and dest IP address.

 Only bad_hash looks at ports (not for ipv6!) - but do not work with fragmented traffic (yet..)!
 ip_vlan_proto_hash works OK with fragments.
   *************************************** */

int32_t ip_vlan_proto_hash(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  long num_out_queues = (long) user;
  u_int32_t buffer_len = pkt_handle->len;

  u_char *pkt_data = pfring_zc_pkt_buff_data(pkt_handle, in_queue);
  u_int32_t hash = 0, l3_offset = sizeof(struct compact_eth_hdr);

  u_int16_t eth_type;

  eth_type = (pkt_data[12] << 8) + pkt_data[13];
  // We could prob unroll this loop, how many vlan tags can you have? QinQinQ seems to be it.
  //while (eth_type == 0x8100 /* VLAN */ && l3_offset+4 < buffer_len) {
  while ((eth_type == 0x8100 || eth_type == 0x88a8 || eth_type == 0x9100 || eth_type == 0x9200 || eth_type == 0x9300) && l3_offset+4 < buffer_len) {
    hash += ((pkt_data[14] << 8) + pkt_data[15]) & 0x0fff;
    l3_offset += 4;
    eth_type = (pkt_data[l3_offset - 2] << 8) + pkt_data[l3_offset - 1];
  }

  hash += eth_type;

  switch (eth_type) {
  case 0x0800:
    {
      /* IPv4 */
      struct compact_ip_hdr *iph;

      if (unlikely(buffer_len < l3_offset + sizeof(struct compact_ip_hdr)))
        return hash;

      iph = (struct compact_ip_hdr *) &pkt_data[l3_offset];

      hash += __bswap_32(iph->saddr) + __bswap_32(iph->daddr);
    }
    break;
  case 0x86DD:
    {
      /* IPv6 */
      struct compact_ipv6_hdr *ipv6h;
      u_int32_t *s, *d;

      if (unlikely(buffer_len < l3_offset + sizeof(struct compact_ipv6_hdr)))
        return hash;

      ipv6h = (struct compact_ipv6_hdr *) &pkt_data[l3_offset];

      s = (u_int32_t *) &ipv6h->saddr, d = (u_int32_t *) &ipv6h->daddr;
      hash += s[0] + s[1] + s[2] + s[3] + d[0] + d[1] + d[2] + d[3];
    }
    break;
  default:
    return hash % num_out_queues;
  }

  return hash % num_out_queues;
}

/* *************************************** */

int32_t bad_hash(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  long num_out_queues = (long) user;
  u_int32_t buffer_len = pkt_handle->len;

  u_char *pkt_data = pfring_zc_pkt_buff_data(pkt_handle, in_queue);
  u_int32_t hash = 0, l3_offset = sizeof(struct compact_eth_hdr), l4_offset = 0;

  u_int16_t eth_type;
  u_int8_t l3_proto = 0; /* To remove warnings, is set, and checked against none 0 values so this is OK */

  /* Vlan fiddle info:                   (remember, var eth_type is 16 bits)
   * u_char = (C99) uint8_t, so pos [12] is shifted 8 bits to the left to remove every thing but TPID,
   * then pos [13] is added. If it is vlan, then pos [13] will be 0x00 (and [14] will be 0x80)
   * Ehhh, the vlan jungle...
   * 0x8100 is 802.1Q, 0x9100 is An old non-standard 802.1QinQ protocol used 0x9100. 802.1QInQ-2007 it seems, 0x88a8 is QinQ (802.1ad)
   * WHAT THE HELL ... there is more!? 0x9200 is mentioned in cisco doc's, 0x9300 is another ...
   * Only 0x8100 and 0x88a8 are standardized, the other three are not
   */

  eth_type = (pkt_data[12] << 8) + pkt_data[13];
  // We could prob unroll this loop, how many vlan tags can you have? QinQinQ seems to be it.
  while ((eth_type == 0x8100 || eth_type == 0x88a8 || eth_type == 0x9100 || eth_type == 0x9200 || eth_type == 0x9300) && l3_offset+4 < buffer_len) {
    hash += ((pkt_data[14] << 8) + pkt_data[15]) & 0x0fff;
    l3_offset += 4;
    eth_type = (pkt_data[l3_offset - 2] << 8) + pkt_data[l3_offset - 1];
  }

  hash += eth_type;

  switch (eth_type) {
  case 0x0800:
    {
      /* IPv4 */
      struct compact_ip_hdr *iph;

      if (unlikely(buffer_len < l3_offset + sizeof(struct compact_ip_hdr)))
        return hash;

      iph = (struct compact_ip_hdr *) &pkt_data[l3_offset];

      /* See the next comment, but we can inline this/replace with just a byte swap (big endian/network order VS little endian/intel/our order)
       * x = (x << 24) | ((x & 0xff00) << 8) | ((x & 0xff0000) >> 8) | (x >> 24);
       * or
       * glib's __bswap_32(x) which is __bswap_constant_32(x) which is:
       * ((((x) & 0xff000000u) >> 24) | (((x) & 0x00ff0000u) >>  8) | (((x) & 0x0000ff00u) <<  8) | (((x) & 0x000000ffu) << 24))
       * which do the same thing.
       */
      //our works, prob no diff vs ntohl as the compiler will take care of it:
      //hash = ntohl(iph->saddr) + ntohl(iph->daddr); /* this can be optimized by avoiding calls to ntohl(), but it can lead to balancing issues */
      hash += __bswap_32(iph->saddr) + __bswap_32(iph->daddr);

      l3_proto = iph->protocol;
      l4_offset = l3_offset + (iph->ihl * 4);
    }
    break;
  case 0x86DD:
    {
      /* IPv6 */
      struct compact_ipv6_hdr *ipv6h;
      u_int32_t *s, *d;

      if (unlikely(buffer_len < l3_offset + sizeof(struct compact_ipv6_hdr)))
        return hash;

      ipv6h = (struct compact_ipv6_hdr *) &pkt_data[l3_offset];

      s = (u_int32_t *) &ipv6h->saddr, d = (u_int32_t *) &ipv6h->daddr;
      hash += s[0] + s[1] + s[2] + s[3] + d[0] + d[1] + d[2] + d[3];

      /* Hmm, does not set l3_proto or l4_offset, "ok" but not complete compared to ipv4
       * So we cannot hash on proto or ports when we see ipv6 packets
       * We need to see what the "nextheader" is, look for 6,17 and
       */
    }
    break;
  default:
    return hash % num_out_queues;
  }

  // 6, 17, 132, we could add UDP Lite (136), RUDP/RTP too? (27)
  if(likely(l3_proto == IPPROTO_TCP || l3_proto == IPPROTO_UDP || l3_proto == IPPROTO_SCTP)) {
    struct compact_udp_hdr *udph = (struct compact_udp_hdr *)(&pkt_data[l4_offset]);
    //hash += ntohs(udph->sport) + ntohs(udph->dport);
    hash += __bswap_16(udph->sport) + __bswap_16(udph->dport);
  }

  /* Fragments are not handeld at all, this is a bit tricky:
   * Easy if all fragments come in order, less so if not.
   * In order, only remember where to send it.
   * Out of order, buffer until we know where to send.
   * Static rings etc
   */

  return hash % num_out_queues;
}

/* *************************************** */

int32_t ip_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  long num_out_queues = (long) user;
  if (time_pulse) SET_TS_FROM_PULSE(pkt_handle, *pulse_timestamp_ns);
  return pfring_zc_builtin_ip_hash(pkt_handle, in_queue) % num_out_queues;
}

/* *************************************** */

int32_t gtp_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  long num_out_queues = (long) user;
  if (time_pulse) SET_TS_FROM_PULSE(pkt_handle, *pulse_timestamp_ns);
  return pfring_zc_builtin_gtp_hash(pkt_handle, in_queue) % num_out_queues;
}

/* *************************************** */

static int rr = -1;

int32_t rr_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  long num_out_queues = (long) user;
  if (time_pulse) SET_TS_FROM_PULSE(pkt_handle, *pulse_timestamp_ns);
  if (++rr == num_out_queues) rr = 0;
  return rr;
}

/* *************************************** */

int32_t sysdig_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  /* NOTE: pkt_handle->hash contains the CPU id */
  struct sysdig_event_header *ev = (struct sysdig_event_header*)pfring_zc_pkt_buff_data(pkt_handle, in_queue); 
  long num_out_queues = (long) user;

  return(ev->thread_id % num_out_queues);
}

/* *************************************** */

int32_t fo_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  if (time_pulse) SET_TS_FROM_PULSE(pkt_handle, *pulse_timestamp_ns);
  return 0xffffffff; 
}

/* *************************************** */

int32_t fo_rr_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  long num_out_queues = (long) user;
  if (time_pulse) SET_TS_FROM_PULSE(pkt_handle, *pulse_timestamp_ns);
  if (++rr == (num_out_queues - 1)) rr = 0;
  return (1 << 0 /* full traffic on 1st slave */ ) | (1 << (1 + rr) /* round-robin on other slaves */ );
}

/* *************************************** */

int32_t fo_multiapp_ip_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  int32_t i, offset = 0, app_instance, consumers_mask = 0, hash;

  if (time_pulse) SET_TS_FROM_PULSE(pkt_handle, *pulse_timestamp_ns);

  hash = pfring_zc_builtin_ip_hash(pkt_handle, in_queue);

  for (i = 0; i < num_apps; i++) {
    app_instance = hash % instances_per_app[i];
    consumers_mask |= (1 << (offset + app_instance));
    offset += instances_per_app[i];
  }

  return consumers_mask;
}

/* *************************************** */

int32_t fo_multiapp_gtp_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  int32_t i, offset = 0, app_instance, consumers_mask = 0, hash;

  if (time_pulse) SET_TS_FROM_PULSE(pkt_handle, *pulse_timestamp_ns);

  hash = pfring_zc_builtin_gtp_hash(pkt_handle, in_queue);

  for (i = 0; i < num_apps; i++) {
    app_instance = hash % instances_per_app[i];
    consumers_mask |= (1 << (offset + app_instance));
    offset += instances_per_app[i];
  }

  return consumers_mask;
}

/* *************************************** */

int main(int argc, char* argv[]) {
  char c;
  char *device = NULL, *dev; 
  char *applications = NULL, *app, *app_pos = NULL;
  char *vm_sockets = NULL, *vm_sock; 
  long i, j, off;
  int hash_mode = 0;
  int num_additional_buffers = 0;
  pthread_t time_thread;
  int rc;
  int num_real_devices = 0, num_in_queues = 0;
  char *pid_file = NULL;
  int opt_argc;
  char **opt_argv;

  start_time.tv_sec = 0;

  if ((argc == 2) && (argv[1][0] != '-')) {
    if (load_args_from_file(argv[1], &opt_argc, &opt_argv) != 0) {
      trace(TRACE_ERROR, "Unable to read config file %s\n", argv[1]);
      exit(-1);
    }
  } else {
    opt_argc = argc;
    opt_argv = argv;
  }

  while((c = getopt(opt_argc, opt_argv,"aC:c:dg:hi:M:m:n:pQ:r:q:N:P:S:z")) != '?') {
    if((c == 255) || (c == -1)) break;

    switch(c) {
    case 'h':
      printHelp();
      break;
    case 'a':
      wait_for_packet = 0;
      break;
    case 'c':
      cluster_id = atoi(optarg);
      break;
    case 'C':
      card_slots = atoi(optarg);
      break;
    case 'd':
      daemon_mode = 1;
      break;
    case 'm':
      hash_mode = atoi(optarg);
      break;
    case 'M':
      mtu = atoi(optarg);
      break;
    case 'n':
      applications = strdup(optarg);
      break;
    case 'i':
      device = strdup(optarg);
      break;
    case 'g':
      bind_worker_core = atoi(optarg);
      break;
    case 'p':
      print_interface_stats = 1;
      break;
    case 'Q':
      enable_vm_support = 1;
      vm_sockets = strdup(optarg);
      break;
    case 'q':
      queue_len = upper_power_of_2(atoi(optarg));
      break;
    case 'N':
      n2disk_producer = 1;
      n2disk_threads = atoi(optarg);
      break;
    case 'r':
      alarm_sec_sleep = atoi(optarg);
      break;
    case 'P':
      pid_file = strdup(optarg);
      break;
    case 'S':
      time_pulse = 1;
      bind_time_pulse_core = atoi(optarg);
      break;
    case 'z':
      proc_stats_only = 1;
      break;
    }
  }
  
  if (device == NULL) printHelp();
  if (cluster_id < 0) printHelp();
  if (applications == NULL) printHelp();
  if (mtu == 0) printHelp();
  if (card_slots == 0) card_slots = MAX_CARD_SLOTS;

  if (n2disk_producer) {
    if (n2disk_threads < 1) printHelp();
    metadata_len = N2DISK_METADATA;
    num_additional_buffers += (n2disk_threads * (N2DISK_CONSUMER_QUEUE_LEN + 1)) + N2DISK_PREFETCH_BUFFERS;
  }

  dev = strtok(device, ",");
  while(dev != NULL) {
    devices = realloc(devices, sizeof(char *) * (num_devices+1));
    devices[num_devices] = strdup(dev);
    num_devices++;
    dev = strtok(NULL, ",");
  }

  app = strtok_r(applications, ",", &app_pos);
  while (app != NULL && num_apps < MAX_NUM_APP) {
    instances_per_app[num_apps] = atoi(app);
    if (instances_per_app[num_apps] == 0) printHelp();
    num_consumer_queues += instances_per_app[num_apps];
    num_apps++;
    app = strtok_r(NULL, ",", &app_pos);
  }

  if (num_apps == 0) printHelp();
  if (num_apps != 1 && hash_mode != 1 && hash_mode != 4) printHelp();

  for (i = 0; i < num_devices; i++) {
    if (strcmp(devices[i], "Q") != 0) num_real_devices++;
    else num_in_queues++;
  }

  if (daemon_mode)
    daemonize();

  zc = pfring_zc_create_cluster(
    cluster_id, 
    mtu,
    metadata_len,
    (num_real_devices * card_slots) + (num_in_queues * (queue_len + IN_POOL_SIZE)) 
     + (num_consumer_queues * (queue_len + POOL_SIZE)) + PREFETCH_BUFFERS + num_additional_buffers, 
    numa_node_of_cpu(bind_worker_core),
    NULL /* auto hugetlb mountpoint */ 
  );

  if(zc == NULL) {
    trace(TRACE_ERROR, "pfring_zc_create_cluster error [%s] Please check your hugetlb configuration\n",
	    strerror(errno));
    return -1;
  }

  inzqs  = calloc(num_devices, sizeof(pfring_zc_queue *));
  outzqs = calloc(num_consumer_queues,  sizeof(pfring_zc_queue *));

  for (i = 0; i < num_devices; i++) {
    if (strcmp(devices[i], "Q") != 0) {

      inzqs[i] = pfring_zc_open_device(zc, devices[i], rx_only, 0);

      if(inzqs[i] == NULL) {
        trace(TRACE_ERROR, "pfring_zc_open_device error [%s] Please check that %s is up and not already used\n",
	        strerror(errno), devices[i]);
        return -1;
      }

    } else { /* create sw queue as ingress device */

      inzqs[i] = pfring_zc_create_queue(zc, queue_len);

      if(inzqs[i] == NULL) {                                                                                                
        trace(TRACE_ERROR, "pfring_zc_create_queue error [%s]\n", strerror(errno));                                             
        return -1;                                                                                                           
      } 

      if (pfring_zc_create_buffer_pool(zc, IN_POOL_SIZE) == NULL) {
        trace(TRACE_ERROR, "pfring_zc_create_buffer_pool error\n");
        return -1;
      }

    }
  }

  for (i = 0; i < num_consumer_queues; i++) { 
    outzqs[i] = pfring_zc_create_queue(zc, queue_len);

    if(outzqs[i] == NULL) {
      trace(TRACE_ERROR, "pfring_zc_create_queue error [%s]\n", strerror(errno));
      return -1;
    }
  }

  for (i = 0; i < num_consumer_queues; i++) { 
    if (pfring_zc_create_buffer_pool(zc, POOL_SIZE) == NULL) {
      trace(TRACE_ERROR, "pfring_zc_create_buffer_pool error\n");
      return -1;
    }
  }

  wsp = pfring_zc_create_buffer_pool(zc, PREFETCH_BUFFERS);

  if (wsp == NULL) {
    trace(TRACE_ERROR, "pfring_zc_create_buffer_pool error\n");
    return -1;
  }

  if (n2disk_producer) {
    char queues_list[256];
    queues_list[0] = '\0';

    for (i = 0; i < n2disk_threads; i++) {
      if(pfring_zc_create_queue(zc, N2DISK_CONSUMER_QUEUE_LEN) == NULL) {
        trace(TRACE_ERROR, "pfring_zc_create_queue error [%s]\n", strerror(errno));
        return -1;
      }
      sprintf(&queues_list[strlen(queues_list)], "%ld,", i + num_consumer_queues);
    }
    queues_list[strlen(queues_list)-1] = '\0';

    if (pfring_zc_create_buffer_pool(zc, N2DISK_PREFETCH_BUFFERS + n2disk_threads) == NULL) {
      trace(TRACE_ERROR, "pfring_zc_create_buffer_pool error\n");
      return -1;
    }

    trace(TRACE_NORMAL, "Run n2disk10gzc with: -i %d@<queue id> --cluster-ipc-queues %s --cluster-ipc-pool %d --reader-threads <%d core ids>\n", 
      cluster_id, queues_list, num_in_queues + num_consumer_queues + 1, n2disk_threads);
  }

  if (enable_vm_support) {
    vm_sock = strtok(vm_sockets, ",");
    while(vm_sock != NULL) {

      rc = pfring_zc_vm_register(zc, vm_sock);

      if (rc < 0) {
        trace(TRACE_ERROR, "pfring_zc_vm_register(%s) error\n", vm_sock);
        return -1;
      }

      vm_sock = strtok(NULL, ",");
    }

    rc = pfring_zc_vm_backend_enable(zc);

    if (rc < 0) {
      trace(TRACE_ERROR, "pfring_zc_vm_backend_enable error\n");
      return -1;
    }
  }

  if (pid_file)
    create_pid_file(pid_file);

  signal(SIGINT,  sigproc);
  signal(SIGTERM, sigproc);
  signal(SIGINT,  sigproc);

  if (time_pulse) {
    pulse_timestamp_ns = calloc(CACHE_LINE_LEN/sizeof(u_int64_t), sizeof(u_int64_t));
    pthread_create(&time_thread, NULL, time_pulse_thread, NULL);
    while (!*pulse_timestamp_ns && !do_shutdown); /* wait for ts */
  }

  trace(TRACE_NORMAL, "Starting balancer with %d consumer queues..\n", num_consumer_queues);

  off = 0;

  if (num_in_queues > 0) {
    trace(TRACE_NORMAL, "Run your traffic generator as follows:\n");
    for (i = 0; i < num_in_queues; i++)
      trace(TRACE_NORMAL, "\tzsend -i zc:%d@%lu\n", cluster_id, off++);
  }

  trace(TRACE_NORMAL, "Run your application instances as follows:\n");
  for (i = 0; i < num_apps; i++) {
    if (num_apps > 1) trace(TRACE_NORMAL, "Application %lu\n", i);
    for (j = 0; j < instances_per_app[i]; j++)
      trace(TRACE_NORMAL, "\tpfcount -i zc:%d@%lu\n", cluster_id, off++);
  }

  if (hash_mode == 0 || hash_mode == 5 || hash_mode == 6 || ((hash_mode == 1 || hash_mode == 4) && num_apps == 1)) { /* balancer */
    pfring_zc_distribution_func func = NULL;

    switch (hash_mode) {
    case 0: func = rr_distribution_func;
      break;
    case 1: if (strcmp(device, "sysdig") == 0) func = sysdig_distribution_func; else if (time_pulse) func = ip_distribution_func; /* else built-in IP-based */
      break;
    case 4: if (strcmp(device, "sysdig") == 0) func = sysdig_distribution_func; else func =  gtp_distribution_func;
      break;
    case 5: func = bad_hash; /* Yes, atm it is */
      break;
    case 6: func = ip_vlan_proto_hash;
      break;
    }

    zw = pfring_zc_run_balancer(
      inzqs, 
      outzqs, 
      num_devices, 
      num_consumer_queues,
      wsp,
      round_robin_bursts_policy,
      NULL,
      func,
      (void *) ((long) num_consumer_queues),
      !wait_for_packet, 
      bind_worker_core
    );

  } else { /* fanout */
    pfring_zc_distribution_func func = NULL;
    
    outzmq = pfring_zc_create_multi_queue(outzqs, num_consumer_queues);

    if(outzmq == NULL) {
      trace(TRACE_ERROR, "pfring_zc_create_multi_queue error [%s]\n", strerror(errno));
      return -1;
    }

    switch (hash_mode) {
    case 1: func = fo_multiapp_ip_distribution_func;
      break;
    case 2: if (time_pulse) func = fo_distribution_func; /* else built-in send-to-all */
      break;
    case 3: func = fo_rr_distribution_func;
      break;
    case 4: func = fo_multiapp_gtp_distribution_func;
      break;
    }

    zw = pfring_zc_run_fanout(
      inzqs, 
      outzmq, 
      num_devices,
      wsp,
      round_robin_bursts_policy, 
      NULL /* idle callback */,
      func,
      (void *) ((long) num_consumer_queues),
      !wait_for_packet, 
      bind_worker_core
    );

  }

  if(zw == NULL) {
    trace(TRACE_ERROR, "pfring_zc_run_balancer error [%s]\n", strerror(errno));
    return -1;
  }
  
  while (!do_shutdown) {
    sleep(alarm_sec_sleep);
    print_stats();
  }

  if (time_pulse)
    pthread_join(time_thread, NULL);

  pfring_zc_destroy_cluster(zc);

  if(pid_file)
    remove_pid_file(pid_file);

  return 0;
}

