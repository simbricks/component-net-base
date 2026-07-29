/*
 * Copyright 2021 Max Planck Institute for Software Systems, and
 * National University of Singapore
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <pcap/pcap.h>
#include <unistd.h>

#include <cassert>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <simbricks/base/cxxatomicfix.h>
extern "C" {
#include <simbricks/network/if.h>
#include <simbricks/nicif/nicif.h>
};

// #define NETSWITCH_DEBUG
#define PKTGEN_STAT

/* One second, in picoseconds. */
#define ONE_SEC_PS (1000ULL * 1000 * 1000 * 1000)

struct SimbricksBaseIfParams netParams;
static pcap_dumper_t *dumpfile = nullptr;
#define PKT_LEN 1500                                           // byte
static uint64_t bit_rate = 100 * 1000ULL * 1000ULL * 1000ULL;  // 100 Gbps
/* Simulated time (ps) at which the generator stops by itself; 0 = run until
   signalled. Set with -t, in ns (like -S/-E). Only reachable on a synchronized
   link, since cur_ts does not advance otherwise. */
static uint64_t target_tick = ONE_SEC_PS;
/* Packet generation on/off; -b 0 turns it off and makes pktgen a pure sink. */
static bool gen_enabled = true;
static uint64_t pkt_recv_num = 0;
static uint64_t pkt_recv_byte = 0;
static uint64_t pkt_tx_num = 0;
static uint64_t pkt_tx_byte = 0;
// ps between packet departures at bit_rate; only meaningful if gen_enabled
static uint64_t period = (ONE_SEC_PS * 8 * PKT_LEN) / bit_rate;
static uint8_t packet[PKT_LEN];

#ifdef PKTGEN_STAT
static uint64_t d2n_poll_total = 0;
static uint64_t d2n_poll_suc = 0;
static uint64_t d2n_poll_sync = 0;

static uint64_t s_d2n_poll_total = 0;
static uint64_t s_d2n_poll_suc = 0;
static uint64_t s_d2n_poll_sync = 0;

static volatile sig_atomic_t stat_flag = 0;
#endif

/* MAC address type */
struct MAC {
  const uint8_t *data;

  explicit MAC(const uint8_t *data) : data(data) {
  }

  bool operator==(const MAC &other) const {
    for (int i = 0; i < 6; i++) {
      if (data[i] != other.data[i]) {
        return false;
      }
    }
    return true;
  }
};

struct mac_addr {
  uint8_t addr[6];
};

namespace std {
template <>
struct hash<MAC> {
  size_t operator()(const MAC &m) const {
    size_t res = 0;
    for (int i = 0; i < 6; i++) {
      res = (res << 4) | (res ^ m.data[i]);
    }
    return res;
  }
};
}  // namespace std

/** Abstract base switch port */
class Port {
 public:
  enum RxPollState {
    kRxPollSuccess = 0,
    kRxPollFail = 1,
    kRxPollSync = 2,
  };

  struct mac_addr my_mac;
  struct mac_addr dest_mac;

  /* Per-port token bucket: simulated time (ps) at which this port last emitted
     a packet. Each port paces independently, so -b is the rate offered on
     every port, not a budget shared between them. */
  uint64_t last_pkt_sent = 0;

  virtual ~Port() = default;

  virtual bool Connect(const char *path, int sync) = 0;
  virtual bool IsSync() = 0;
  virtual void Sync(uint64_t cur_ts) = 0;
  virtual uint64_t NextTimestamp() = 0;
  virtual enum RxPollState RxPacket(const void *&data, size_t &len,
                                    uint64_t cur_ts) = 0;
  virtual void RxDone() = 0;
  virtual bool TxPacket(const void *data, size_t len, uint64_t cur_ts) = 0;
};

/** Normal network switch port (conneting to a NIC) */
class NetPort : public Port {
 protected:
  struct SimbricksNetIf netifObj_;
  struct SimbricksNetIf *netif_;
  volatile union SimbricksProtoNetMsg *rx_;
  int sync_;

 public:
  NetPort() : netif_(&netifObj_), rx_(nullptr), sync_(0) {
    memset(&netifObj_, 0, sizeof(netifObj_));
    memset(&my_mac, 0, sizeof(my_mac));
    memset(&dest_mac, 0, sizeof(dest_mac));
  }

  NetPort(const NetPort &other)
      : netifObj_(other.netifObj_),
        netif_(&netifObj_),
        rx_(other.rx_),
        sync_(other.sync_) {
  }

  bool Connect(const char *path, int sync) override {
    sync_ = sync;
    return SimbricksNetIfInit(netif_, &netParams, path, &sync_) == 0;
  }

  bool IsSync() override {
    return sync_;
  }

  void Sync(uint64_t cur_ts) override {
    while (SimbricksNetIfOutSync(netif_, cur_ts)) {
    }
  }

  uint64_t NextTimestamp() override {
    return SimbricksNetIfInTimestamp(netif_);
  }

  enum RxPollState RxPacket(const void *&data, size_t &len,
                            uint64_t cur_ts) override {
    assert(rx_ == nullptr);

    rx_ = SimbricksNetIfInPoll(netif_, cur_ts);
    if (!rx_)
      return kRxPollFail;

    uint8_t type = SimbricksNetIfInType(netif_, rx_);
    if (type == SIMBRICKS_PROTO_NET_MSG_PACKET) {
      data = (const void *)rx_->packet.data;
      len = rx_->packet.len;
      return kRxPollSuccess;
    } else if (type == SIMBRICKS_PROTO_MSG_TYPE_SYNC) {
      return kRxPollSync;
    } else {
      fprintf(stderr, "pktgen: unsupported type=%u\n", type);
      abort();
    }
  }

  void RxDone() override {
    assert(rx_ != nullptr);

    SimbricksNetIfInDone(netif_, rx_);
    rx_ = nullptr;
  }

  bool TxPacket(const void *data, size_t len, uint64_t cur_ts) override {
    volatile union SimbricksProtoNetMsg *msg_to =
        SimbricksNetIfOutAlloc(netif_, cur_ts);
    if (!msg_to && !sync_) {
      return false;
    } else if (!msg_to && sync_) {
      while (!msg_to)
        msg_to = SimbricksNetIfOutAlloc(netif_, cur_ts);
    }
    volatile struct SimbricksProtoNetMsgPacket *rx;
    rx = &msg_to->packet;
    rx->len = len;
    rx->port = 0;
    memcpy((void *)rx->data, data, len);

    SimbricksNetIfOutSend(netif_, msg_to, SIMBRICKS_PROTO_NET_MSG_PACKET);
    return true;
  }
};

/** Hosting network switch port (connected to another network) */
class NetHostPort : public NetPort {
 protected:
  struct SimbricksNicIf nicif_;

 public:
  NetHostPort() {
    netif_ = &nicif_.net;
    memset(&nicif_, 0, sizeof(nicif_));
  }

  NetHostPort(const NetHostPort &other) : NetPort(other), nicif_(other.nicif_) {
    netif_ = &nicif_.net;
  }

  bool Connect(const char *path, int sync) override {
    sync_ = sync;
    std::string shm_path = path;
    shm_path += "-shm";
    struct SimbricksBaseIfParams params = netParams;
    params.sock_path = path;
    if (!sync)
      params.sync_mode = kSimbricksBaseIfSyncDisabled;
    int ret = SimbricksNicIfInit(&nicif_, shm_path.c_str(), &params, nullptr,
                                 nullptr);
    sync_ = SimbricksBaseIfSyncEnabled(&netif_->base);
    return ret == 0;
  }

  bool IsSync() override {
    return sync_;
  }
};

/* Global variables */
static uint64_t cur_ts = 0;
static volatile sig_atomic_t exiting = 0;
static const uint8_t bcast[6] = {0xFF};
static const MAC bcast_addr(bcast);
static std::vector<Port *> ports;
static std::unordered_map<MAC, int> mac_table;

static void sigint_handler(int dummy) {
  exiting = 1;
}

static void sigusr1_handler(int dummy) {
  fprintf(stderr, "main_time = %lu\n", cur_ts);
}

#ifdef PKTGEN_STAT
static void sigusr2_handler(int dummy) {
  stat_flag = 1;
}
#endif

static void pollq(Port &port) {
  // poll N2D queue and count what arrives; pktgen is a sink, it forwards nothing
  const void *pkt_data;
  size_t pkt_len;

#ifdef PKTGEN_STAT
  d2n_poll_total += 1;
  if (stat_flag) {
    s_d2n_poll_total += 1;
  }
#endif

  enum Port::RxPollState poll = port.RxPacket(pkt_data, pkt_len, cur_ts);
  if (poll == Port::kRxPollFail) {
    return;  // do nothing
  }

#ifdef PKTGEN_STAT
  d2n_poll_suc += 1;
  if (stat_flag) {
    s_d2n_poll_suc += 1;
  }
#endif

  if (poll == Port::kRxPollSuccess) {
    // stat received bytes
    pkt_recv_num++;
    pkt_recv_byte += pkt_len;
  } else if (poll == Port::kRxPollSync) {
#ifdef PKTGEN_STAT
    d2n_poll_sync += 1;
    if (stat_flag) {
      s_d2n_poll_sync += 1;
    }
#endif
  } else {
    fprintf(stderr, "pktgen: unsupported poll result=%u\n", poll);
    abort();
  }
  port.RxDone();
}

static void sendq(Port &port) {
  if (!gen_enabled)
    return;

  if (port.IsSync()) {
    /* Synchronized link: pace in simulated time. last_pkt_sent is the token
       bucket, emitting every packet whose scheduled departure has come due.
       Packets are stamped cur_ts rather than their scheduled departure because
       outgoing timestamps on a channel must be non-decreasing: Sync() may
       already have sent a sync message stamped cur_ts this iteration, and
       SimbricksBaseIfOutAlloc() assigns out_timestamp unconditionally.
       TxPacket() spins on a full out queue when synchronized, so it always
       succeeds here. */
    while (port.last_pkt_sent + period <= cur_ts) {
      port.last_pkt_sent += period;
      port.TxPacket(packet, PKT_LEN, cur_ts);
      pkt_tx_num++;
      pkt_tx_byte += PKT_LEN;
    }
  } else {
    /* Unsynchronized link: cur_ts never advances (no port reports a next
       timestamp), so there is no clock to pace against and -b cannot be
       honored -- main() warns about this. Offer one packet per iteration and
       let the out queue running full provide the backpressure. */
    if (port.TxPacket(packet, PKT_LEN, cur_ts)) {
      pkt_tx_num++;
      pkt_tx_byte += PKT_LEN;
    }
  }
}

/* Parse a complete numeric option argument into [min, max]. Returns false and
   leaves *out untouched if arg is not a whole number in range. strtol's
   overflow results (LONG_MIN/LONG_MAX) are rejected by the range check. */
static bool ParseNumOpt(const char *arg, long min, long max, long *out) {
  char *end = nullptr;
  long val = strtol(arg, &end, 0);
  if (end == arg || *end != '\0' || val < min || val > max)
    return false;
  *out = val;
  return true;
}

int main(int argc, char *argv[]) {
  int c;
  int bad_option = 0;
  int sync_eth = 1;
  pcap_t *pc = nullptr;
  int my_num = 0;

  SimbricksNetIfDefaultParams(&netParams);

  // Parse command line argument
  while ((c = getopt(argc, argv, "s:h:uS:E:p:n:b:t:")) != -1 && !bad_option) {
    switch (c) {
      case 's': {
        NetPort *port = new NetPort;
        fprintf(stderr, "pktgen connecting to: %s\n", optarg);
        if (!port->Connect(optarg, sync_eth)) {
          fprintf(stderr, "connecting to %s failed\n", optarg);
          return EXIT_FAILURE;
        }
        ports.push_back(port);
        break;
      }

      case 'h': {
        NetHostPort *port = new NetHostPort;
        fprintf(stderr, "pktgen listening on: %s\n", optarg);
        if (!port->Connect(optarg, sync_eth)) {
          fprintf(stderr, "listening on %s failed\n", optarg);
          return EXIT_FAILURE;
        }
        ports.push_back(port);
        break;
      }

      case 'u':
        sync_eth = 0;
        break;

      case 'S':
        netParams.sync_interval = strtoull(optarg, NULL, 0) * 1000ULL;
        break;

      case 'E':
        netParams.link_latency = strtoull(optarg, NULL, 0) * 1000ULL;
        break;

      case 'p':
        pc = pcap_open_dead_with_tstamp_precision(DLT_EN10MB, 65535,
                                                  PCAP_TSTAMP_PRECISION_NANO);
        if (pc == nullptr) {
          perror("pcap_open_dead failed");
          return EXIT_FAILURE;
        }

        dumpfile = pcap_dump_open(pc, optarg);
        break;
      case 'n': {
        long val;
        // 254 is the largest id whose peer (my_num +/- 1) still fits in a byte
        if (!ParseNumOpt(optarg, 0, 254, &val)) {
          fprintf(stderr, "invalid -n value '%s' (expected 0..254)\n", optarg);
          bad_option = 1;
          break;
        }
        my_num = (int)val;
        fprintf(stderr, "my_num is: %d\n", my_num);
        break;
      }

      case 'b': {
        long brate;
        if (!ParseNumOpt(optarg, 0, 200, &brate)) {
          fprintf(stderr,
                  "invalid -b value '%s' (expected 1..200 Gbps, or 0 to "
                  "disable generation)\n",
                  optarg);
          bad_option = 1;
          break;
        }
        if (brate == 0) {
          gen_enabled = false;
          fprintf(stderr, "bit rate is 0: packet generation disabled\n");
        } else {
          gen_enabled = true;
          bit_rate = (uint64_t)brate * 1000ULL * 1000ULL * 1000ULL;
          period = (ONE_SEC_PS * 8 * PKT_LEN) / bit_rate;  // per packet
          fprintf(stderr,
                  "bit rate set to: %ld Gbps (%lu ps per %d B packet)\n", brate,
                  period, PKT_LEN);
        }
        break;
      }

      case 't': {
        long val;
        if (!ParseNumOpt(optarg, 0, LONG_MAX / 1000, &val)) {
          fprintf(stderr,
                  "invalid -t value '%s' (expected ns, 0 = run until "
                  "signalled)\n",
                  optarg);
          bad_option = 1;
          break;
        }
        target_tick = (uint64_t)val * 1000ULL;
        break;
      }

      default:
        fprintf(stderr, "unknown option %c\n", c);
        bad_option = 1;
        break;
    }
  }

  if (ports.empty() || bad_option) {
    fprintf(stderr,
            "Usage: net_pktgen [-S SYNC-PERIOD] [-E ETH-LATENCY] [-u] "
            "[-p PCAP-FILE] [-n ID] [-b GBPS] [-t NS] "
            "-s SOCKET-A [-s SOCKET-B ...] [-h SOCKET-C ...]\n"
            "  -n  generator id, 0..254; src MAC 00:..:<id>, dst MAC of the\n"
            "      paired id (even ids send to id+1, odd ids to id-1)\n"
            "  -b  offered rate in Gbps (default 100; 0 disables generation)\n"
            "  -t  simulated run time in ns (default 1000000000 = 1s;"
            " 0 = until signalled)\n"
            "  -b and -t are only enforced on synchronized links.\n"
            "  NOTE: -S/-E/-u only affect ports opened after them on the"
            " command line; pass them first.\n");
    return EXIT_FAILURE;
  }

  /* Every port generates at the full -b rate, so the offered load scales with
     the port count; they all share the one identity configured by -n. */
  bool any_unsync = false;
  for (Port *port : ports)
    any_unsync |= !port->IsSync();
  if (gen_enabled && any_unsync)
    fprintf(stderr,
            "pktgen: warning: unsynchronized port(s), -b and -t are not "
            "enforced there; generating as fast as the peer accepts\n");

  Port *pkt_port = ports.front();
  pkt_port->my_mac.addr[5] = my_num;
  if (my_num % 2) {  // odd num
    pkt_port->dest_mac.addr[5] = my_num - 1;
  } else {  // even number
    pkt_port->dest_mac.addr[5] = my_num + 1;
  }
  struct mac_addr *mac_tmp = (struct mac_addr *)(&packet[0]);
  mac_tmp->addr[5] = pkt_port->dest_mac.addr[5];  // dest mac
  mac_tmp = (struct mac_addr *)(&packet[6]);
  mac_tmp->addr[5] = pkt_port->my_mac.addr[5];  // source mac

  /* Fill ethertype + payload. The bound is PKT_LEN, not PKT_LEN - 12: the
     leading 12 skips both MAC addresses, there is no trailer to leave clear. */
  memset(&packet[12], 0xFF, PKT_LEN - 12);

  signal(SIGINT, sigint_handler);
  signal(SIGTERM, sigint_handler);
  signal(SIGUSR1, sigusr1_handler);

#ifdef PKTGEN_STAT
  signal(SIGUSR2, sigusr2_handler);
#endif

  printf("start polling\n");
  while (!exiting) {
    // Sync all interfaces
    for (auto port : ports)
      port->Sync(cur_ts);

    // Drain and generate on every port
    uint64_t min_ts;
    do {
      min_ts = ULLONG_MAX;
      for (Port *port : ports) {
        pollq(*port);
        sendq(*port);
        if (port->IsSync()) {
          uint64_t ts = port->NextTimestamp();
          min_ts = ts < min_ts ? ts : min_ts;
        }
      }
    } while (!exiting && (min_ts <= cur_ts));

    // Update cur_ts
    if (min_ts < ULLONG_MAX) {
      // a bit broken but should probably do
      cur_ts = min_ts;
      if (target_tick != 0 && cur_ts >= target_tick) {
        printf("pktgen: reached target time %lu ps, exiting\n", cur_ts);
        exiting = 1;
      }
    }
  }

#ifdef PKTGEN_STAT
  fprintf(stderr, "sent packet: %20lu  [%20lu Byte]\n", pkt_tx_num,
          pkt_tx_byte);
  fprintf(stderr, "recv packet: %20lu  [%20lu Byte]\n", pkt_recv_num,
          pkt_recv_byte);

  fprintf(stderr, "%20s: %22lu %20s: %22lu  poll_suc_rate: %f\n",
          "d2n_poll_total", d2n_poll_total, "d2n_poll_suc", d2n_poll_suc,
          d2n_poll_total ? (double)d2n_poll_suc / d2n_poll_total : 0.0);
  fprintf(stderr, "%65s: %22lu  sync_rate: %f\n", "d2n_poll_sync",
          d2n_poll_sync,
          d2n_poll_suc ? (double)d2n_poll_sync / d2n_poll_suc : 0.0);

  fprintf(stderr, "%20s: %22lu %20s: %22lu  poll_suc_rate: %f\n",
          "s_d2n_poll_total", s_d2n_poll_total, "s_d2n_poll_suc",
          s_d2n_poll_suc,
          s_d2n_poll_total ? (double)s_d2n_poll_suc / s_d2n_poll_total : 0.0);
  fprintf(stderr, "%65s: %22lu  sync_rate: %f\n", "s_d2n_poll_sync",
          s_d2n_poll_sync,
          s_d2n_poll_suc ? (double)s_d2n_poll_sync / s_d2n_poll_suc : 0.0);
#endif

  return 0;
}
