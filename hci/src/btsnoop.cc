/******************************************************************************
 *
 *  Copyright (C) 2014 Google, Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#define LOG_TAG "bt_snoop"

#include <mutex>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "bt_types.h"
#include "hci/include/btsnoop.h"
#include "hci/include/btsnoop_mem.h"
#include "hci_layer.h"
#include "osi/include/log.h"
#include "osi/include/properties.h"
#include "osi/include/time.h"
#include "stack_config.h"
#include "l2c_api.h"

// The number of of packets per btsnoop file before we rotate to the next
// file. As of right now there are two snoop files that are rotated through.
// The size can be dynamically configured by seting the relevant system
// property
#define DEFAULT_BTSNOOP_SIZE 0xffff
#define HANDLE_MASK 0x0FFF
#define START_PACKET_BOUNDARY 2
#define GET_BOUNDARY_FLAG(handle) (((handle) >> 12) & 0x0003)

#define BTSNOOP_ENABLE_PROPERTY "persist.bluetooth.btsnoopenable"
#define BTSNOOP_PATH_PROPERTY "persist.bluetooth.btsnooppath"
#define DEFAULT_BTSNOOP_PATH "/data/misc/bluetooth/logs/btsnoop_hci.log"
#define BTSNOOP_MAX_PACKETS_PROPERTY "persist.bluetooth.btsnoopsize"

typedef enum {
  kCommandPacket = 1,
  kAclPacket = 2,
  kScoPacket = 3,
  kEventPacket = 4
} packet_type_t;

// Epoch in microseconds since 01/01/0000.
static const uint64_t BTSNOOP_EPOCH_DELTA = 0x00dcddb30f2f8000ULL;

static int logfile_fd = INVALID_FD;
static std::mutex btsnoop_mutex;
static std::mutex btSnoopFd_mutex;

static int32_t packets_per_file;
static int32_t packet_counter;
static bool sock_snoop_active = false;

extern bt_logger_interface_t *logger_interface;
int64_t gmt_offset;

// TODO(zachoverflow): merge btsnoop and btsnoop_net together
void btsnoop_net_open();
void btsnoop_net_close();
void btsnoop_net_write(const void* data, size_t length);
static bool is_avdt_media_packet(const uint8_t *p, bool is_local_cid);

static void delete_btsnoop_files();
static bool is_btsnoop_enabled();
static char* get_btsnoop_log_path(char* log_path);
static char* get_btsnoop_last_log_path(char* last_log_path, char* log_path);
static void open_next_snoop_file();
static void btsnoop_write_packet(packet_type_t type, uint8_t* packet,
                                 bool is_received, uint64_t timestamp_us);

// Module lifecycle functions

static future_t* start_up(void) {
  std::lock_guard<std::mutex> lock(btsnoop_mutex);
  time_t t = time(NULL);
  struct tm tm_cur;

  localtime_r (&t, &tm_cur);
  gmt_offset = tm_cur.tm_gmtoff;

  if (!is_btsnoop_enabled()) {
    delete_btsnoop_files();
  } else {
    open_next_snoop_file();
    packets_per_file = osi_property_get_int32(BTSNOOP_MAX_PACKETS_PROPERTY,
                                              DEFAULT_BTSNOOP_SIZE);
    btsnoop_net_open();
    START_SNOOP_LOGGING();
  }
  LOG_DEBUG(LOG_TAG, "%s: vendor_logging_level values is %d ", __func__, vendor_logging_level);

  return NULL;
}

static future_t* shut_down(void) {
  std::lock_guard<std::mutex> lock(btsnoop_mutex);

  if (!is_btsnoop_enabled()) {
    delete_btsnoop_files();
  }

  if (logfile_fd != INVALID_FD) close(logfile_fd);
  logfile_fd = INVALID_FD;

  STOP_SNOOP_LOGGING();
  btsnoop_net_close();

  return NULL;
}

EXPORT_SYMBOL extern const module_t btsnoop_module = {
    .name = BTSNOOP_MODULE,
    .init = NULL,
    .start_up = start_up,
    .shut_down = shut_down,
    .clean_up = NULL,
    .dependencies = {STACK_CONFIG_MODULE, NULL}};

// Interface functions
static void capture(const BT_HDR* buffer, bool is_received) {
  uint8_t* p = const_cast<uint8_t*>(buffer->data + buffer->offset);

  std::lock_guard<std::mutex> lock(btsnoop_mutex);
  uint64_t timestamp_us = time_gettimeofday_us();
  timestamp_us += gmt_offset*1000000LL;
  btsnoop_mem_capture(buffer, timestamp_us);

  if (logfile_fd == INVALID_FD) return;

  switch (buffer->event & MSG_EVT_MASK) {
    case MSG_HC_TO_STACK_HCI_EVT:
      btsnoop_write_packet(kEventPacket, p, false, timestamp_us);
      break;
    case MSG_HC_TO_STACK_HCI_ACL:
    case MSG_STACK_TO_HC_HCI_ACL:
      btsnoop_write_packet(kAclPacket, p, is_received, timestamp_us);
      break;
    case MSG_HC_TO_STACK_HCI_SCO:
    case MSG_STACK_TO_HC_HCI_SCO:
      btsnoop_write_packet(kScoPacket, p, is_received, timestamp_us);
      break;
    case MSG_STACK_TO_HC_HCI_CMD:
      btsnoop_write_packet(kCommandPacket, p, true, timestamp_us);
      break;
  }
}

static const btsnoop_t interface = {capture};

const btsnoop_t* btsnoop_get_interface() {
  return &interface;
}

// Internal functions
static void delete_btsnoop_files() {
  LOG_VERBOSE(LOG_TAG, "Deleting snoop log if it exists");
  char log_path[PROPERTY_VALUE_MAX];
  char last_log_path[PROPERTY_VALUE_MAX + sizeof(".last")];
  get_btsnoop_log_path(log_path);
  get_btsnoop_last_log_path(last_log_path, log_path);
  remove(log_path);
  remove(last_log_path);
}

static bool is_btsnoop_enabled() {
  char btsnoop_enabled[PROPERTY_VALUE_MAX] = {0};
  osi_property_get(BTSNOOP_ENABLE_PROPERTY, btsnoop_enabled, "false");
  return strncmp(btsnoop_enabled, "true", 4) == 0;
}

static char* get_btsnoop_log_path(char* btsnoop_path) {
  osi_property_get(BTSNOOP_PATH_PROPERTY, btsnoop_path, DEFAULT_BTSNOOP_PATH);
  return btsnoop_path;
}

static char* get_btsnoop_last_log_path(char* last_log_path,
                                       char* btsnoop_path) {
  snprintf(last_log_path, PROPERTY_VALUE_MAX + sizeof(".last"), "%s.last",
           btsnoop_path);
  return last_log_path;
}

static void open_next_snoop_file() {
  packet_counter = 0;

  std::lock_guard<std::mutex> lock(btSnoopFd_mutex);
  if(sock_snoop_active)
    return;

  if (logfile_fd != INVALID_FD) {
    close(logfile_fd);
    logfile_fd = INVALID_FD;
  }

  char log_path[PROPERTY_VALUE_MAX];
  char last_log_path[PROPERTY_VALUE_MAX + sizeof(".last")];
  get_btsnoop_log_path(log_path);
  get_btsnoop_last_log_path(last_log_path, log_path);

  if (!rename(log_path, last_log_path) && errno != ENOENT)
    LOG_ERROR(LOG_TAG, "%s unable to rename '%s' to '%s': %s", __func__,
              log_path, last_log_path, strerror(errno));

  mode_t prevmask = umask(0);
  logfile_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC,
                    S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
  umask(prevmask);
  if (logfile_fd == INVALID_FD) {
    LOG_ERROR(LOG_TAG, "%s unable to open '%s': %s", __func__, log_path,
              strerror(errno));
    return;
  }

  write(logfile_fd, "btsnoop\0\0\0\0\1\0\0\x3\xea", 16);
}

typedef struct {
  uint32_t length_original;
  uint32_t length_captured;
  uint32_t flags;
  uint32_t dropped_packets;
  uint64_t timestamp;
  uint8_t type;
} __attribute__((__packed__)) btsnoop_header_t;

static uint64_t htonll(uint64_t ll) {
  const uint32_t l = 1;
  if (*(reinterpret_cast<const uint8_t*>(&l)) == 1)
    return static_cast<uint64_t>(htonl(ll & 0xffffffff)) << 32 |
           htonl(ll >> 32);

  return ll;
}

static void calculate_acl_packet_length(uint32_t *length, uint8_t* packet, bool is_received) {
  uint32_t def_len = (packet[3] << 8) + packet[2] + 5;
  static const size_t HCI_ACL_HEADER_SIZE = 4;
  static const size_t MAX_HCI_ACL_LEN = 14;
  static const size_t L2CAP_CID_OFFSET = (HCI_ACL_HEADER_SIZE + 2);
  static const uint16_t L2CAP_SIGNALING_CID = 0x0001;
  // Assigning to zero
  *length = 0;

  if (vendor_logging_level & HCI_SNOOP_LOG_FULL) {
    *length = def_len;
  } else if (vendor_logging_level & HCI_SNOOP_ONLY_HEADER) {
    uint8_t* stream = packet;
    uint16_t handle;

    STREAM_TO_UINT16(handle, stream);
    uint8_t boundary_flag = GET_BOUNDARY_FLAG(handle);
    handle = handle & HANDLE_MASK;

    if (boundary_flag == START_PACKET_BOUNDARY) {
      uint16_t l2cap_cid =
         stream[L2CAP_CID_OFFSET] | (stream[L2CAP_CID_OFFSET + 1] << 8);
      if (l2cap_cid == L2CAP_SIGNALING_CID || handle == 0x0edc) {
        *length = def_len;
      } else {
        if(def_len < MAX_HCI_ACL_LEN) {
          *length = def_len;
        } else {
          // Otherwise, return as much as we reasonably can
          *length = MAX_HCI_ACL_LEN;
        }
      }
    }
  } else if (vendor_logging_level & HCI_SNOOP_LOG_LITE) {
     if(!is_avdt_media_packet(packet, is_received))
       *length = def_len;
  }
}

static void btsnoop_write_packet(packet_type_t type, uint8_t* packet,
                                 bool is_received, uint64_t timestamp_us) {
  uint32_t length_he = 0;

  uint32_t flags = 0;

  LOG_DEBUG(LOG_TAG, "%s: -->", __func__);

  switch (type) {
    case kCommandPacket:
      length_he = packet[2] + 4;
      flags = 2;
      break;
    case kAclPacket:
      calculate_acl_packet_length(&length_he, packet, is_received);
      flags = is_received;
      break;
    case kScoPacket:
      length_he = packet[2] + 4;
      flags = is_received;
      break;
    case kEventPacket:
      length_he = packet[1] + 3;
      flags = 3;
      break;
  }

  if (!length_he)
    return;

  btsnoop_header_t header;
  header.length_original = htonl(length_he);
  header.length_captured = header.length_original;
  header.flags = htonl(flags);
  header.dropped_packets = 0;
  header.timestamp = htonll(timestamp_us + BTSNOOP_EPOCH_DELTA);
  header.type = type;

  btsnoop_net_write(&header, sizeof(btsnoop_header_t));
  btsnoop_net_write(packet, length_he - 1);

  LOG_DEBUG(LOG_TAG, "%s: finished net_write", __func__);

  if (logfile_fd != INVALID_FD) {
    packet_counter++;
    if (!sock_snoop_active && packet_counter > packets_per_file) {
      open_next_snoop_file();
    }

    iovec iov[] = {{&header, sizeof(btsnoop_header_t)},
                   {reinterpret_cast<void*>(packet), length_he - 1}};
    TEMP_FAILURE_RETRY(writev(logfile_fd, iov, 2));
  }

  LOG_DEBUG(LOG_TAG, "%s: <--", __func__);
}

void update_snoop_fd(int snoop_fd) {
  std::lock_guard<std::mutex> lock(btSnoopFd_mutex);
  LOG_INFO(LOG_TAG, "%s Now writing to server socket", __func__);
  sock_snoop_active = true;
  logfile_fd = snoop_fd;
}

static bool is_avdt_media_packet(const uint8_t *p, bool is_received) {
  uint16_t cid, handle;
  bool is_local_cid = is_received;
  /*is_received signifies Rx packet so packet will have local_cid at offset 6
   * Tx packet with is_received as false and have remote_cid at the offset*/

  handle = (uint16_t)((p[0] + (p[1] << 8)) & 0x0FFF);
  cid =  (uint16_t)(p[6] + (p[7] << 8));

  return L2CA_isMediaChannel(handle, cid, is_local_cid);
}
