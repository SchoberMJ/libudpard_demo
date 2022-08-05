/*
        Simple udp client
*/
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>  //printf
#include <stdlib.h> //exit(0);
#include <string.h> //memset
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

//#include "register.h"
#include <ethard.h>
#include <o1heap.h>

// #include
// #include <uavcan/_register/Access_1_0.h>
// #include <uavcan/_register/List_1_0.h>
// #include <uavcan/node/ExecuteCommand_1_1.h>
// #include <uavcan/node/GetInfo_1_0.h>
// #include <uavcan/node/Heartbeat_1_0.h>
// #include <uavcan/node/port/List_0_1.h>
// #include <uavcan/pnp/NodeIDAllocationData_2_0.h>

// Use /sample/ instead of /unit/ if you need timestamping.
//#include <uavcan/si/unit/pressure/Scalar_1_0.h>
#include <uavcan/si/unit/temperature/Scalar_1_0.h>
// #include
// "../submodules/public_regulated_data_types/uavcan/si/unit/temperature/Scalar.1.0.h"

#define KILO 1000L
#define MEGA ((int64_t)KILO * KILO)

#define UDP_REDUNDANCY_FACTOR 1
#define UDP_TX_QUEUE_CAPACITY 100

#define MULTICAST_GROUP "239.255.255.250"
#define MULTICAST_GROUP_INTERFACE "239.0.0.24"
#define BUFLEN 512      // Max length of buffer, should be same size as the MTU
#define PORT 16383      // The port on which to send data
#define SUBJECT_ID 2346 // Can be replaced by registers

typedef struct State {
  EthardMicrosecond started_at;

  /// Some memory heap (demo uses pavel's o1heap)
  O1HeapInstance *heap;
  EthardInstance ethard;
  EthardTxQueue ethard_tx_queues[UDP_REDUNDANCY_FACTOR];

  /// These values are read from the registers at startup
  struct {
    struct {
      EthardPortID message_one; //< ...temperature.Scalar
    } pub;
  } port_id;

  /// A transfer-ID is an integer that is incremented whenever a new message is
  /// published on a given subject. It is used by the protocol for
  /// deduplication, message loss detection, and other critical things. For UDP,
  /// each value can be of type uint64_t
  struct {
    // uint64_t uavcan_node_heartbeat;
    // uint64_t uavcan_node_port_list;
    // uint64_t uavcan_pnp_allocation;
    // Tip: messages published synchronously can share the same transfer-ID.
    uint64_t message_one;
    // uint64_t message_two;
  } next_transfer_id;
} State;

static void handleLoop(State *const state,
                       const EthardMicrosecond monotonic_time) {
  uavcan_si_unit_temperature_Scalar_1_0 msg = {0};
  // msg.kelvin = (float)rand() * 0.1F;
  msg.kelvin = 100;

  // Serialize and publish the message
  uint8_t serialized
      [uavcan_si_unit_temperature_Scalar_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_] =
          {0};
  size_t serialized_size = sizeof(serialized);
  const int8_t err = uavcan_si_unit_temperature_Scalar_1_0_serialize_(
      &msg, &serialized[0], &serialized_size);
  if (err >= 0) {
    const EthardTransferMetadata meta = {
        .priority = EthardPriorityHigh,
        .transfer_kind = EthardTransferKindMessage,
        .port_id = state->port_id.pub.message_one,
        .remote_node_id = ETHARD_NODE_ID_UNSET,
        .transfer_id = (EthardTransferID)state->next_transfer_id.message_one++,
    };
    (void)ethardTxPush(&state->ethard_tx_queues[0], &state->ethard,
                       monotonic_time * 10 * KILO, &meta, serialized_size,
                       &serialized[0]);
  }
}

static EthardMicrosecond getMonotonicMicroseconds() {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    abort();
  }
  return (uint64_t)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
}

static void *ethardAllocate(EthardInstance *const ins, const size_t amount) {
  O1HeapInstance *const heap = ((State *)ins->user_reference)->heap;
  assert(o1heapDoInvariantsHold(heap));
  return o1heapAllocate(heap, amount);
}

static void ethardFree(EthardInstance *const ins, void *const pointer) {
  O1HeapInstance *const heap = ((State *)ins->user_reference)->heap;
  o1heapFree(heap, pointer);
}

void die(char *s) {
  perror(s);
  exit(1);
}

int send_multicast(const EthardTxQueueItem *tqi) {
  printf("Setting up a multicast socket...\n");
  struct sockaddr_in group_addr, remote_addr;
  struct in_addr local_interface;
  int group_socket, slen = sizeof(group_addr);
  // Setup a group socket for sending multicast messages
  if ((group_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
    die("Local socket creation failed");
  }
  memset((char *)&group_addr, 0, sizeof(group_addr));
  group_addr.sin_family = AF_INET;
  group_addr.sin_port = 0; // Ephemeral Port
  group_addr.sin_addr.s_addr = htonl(tqi->specifier.source_route_specifier);
  if (bind(group_socket, (struct sockaddr *)&group_addr, sizeof(group_addr)) <
      0) {
    die("bind failed");
  }
  local_interface.s_addr = htonl(tqi->specifier.source_route_specifier);
  if (setsockopt(group_socket, IPPROTO_IP, IP_MULTICAST_IF,
                 (char *)&local_interface, sizeof(local_interface)) < 0) {
    die("Setting local interface error");
  }
  remote_addr.sin_port = htons(tqi->specifier.data_specifier);
  remote_addr.sin_family = AF_INET;
  // remote_addr.sin_addr.s_addr = inet_addr(MULTICAST_GROUP);
  remote_addr.sin_addr.s_addr =
      htonl(tqi->specifier.destination_route_specifier);
  if (connect(group_socket, (struct sockaddr *)&remote_addr,
              sizeof(remote_addr)) < 0) {
    die("Connect failed");
  }
  printf("Sending Message...");

  const int16_t result =
      send(group_socket, tqi->frame.payload, tqi->frame.payload_size, 0);
  printf("\n");

  printf("Closing the multicast socket...\n");
  if (close(group_socket) < 0) {
    die("Closing failed");
  }
  return result;
}

int main(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);

  // Setup the state and ethard instance
  State state = {0};

  _Alignas(O1HEAP_ALIGNMENT) static uint8_t heap_arena[1024 * 16] = {0};
  state.heap = o1heapInit(heap_arena, sizeof(heap_arena));

  state.ethard = ethardInit(&ethardAllocate, &ethardFree);
  state.ethard.user_reference = &state;
  state.ethard.node_id = 24U;
  uint32_t ipv4_addr =
      (127U << 24U) | (0U << 16U) | (0U << 8U) | (state.ethard.node_id);
  // You can alternatively use ntohl and inet_addr to get the proper value
  printf("Converting 127.0.0.24 to a uint32_t\n");
  uint32_t demo_ipv4_addr = ntohl(inet_addr("127.0.0.24"));
  printf("Converted value in hex: %02X\n", demo_ipv4_addr);
  state.ethard.local_ip_addr = ipv4_addr;

  state.ethard_tx_queues[0] = ethardTxInit(UDP_TX_QUEUE_CAPACITY, BUFLEN);

  state.port_id.pub.message_one = SUBJECT_ID;
  state.next_transfer_id.message_one = 1;

  // Setup the local socket for receiving server reply
  struct sockaddr_in local_addr;
  int local_socket, slen = sizeof(local_addr);
  char buf[BUFLEN];

  if ((local_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
    die("Local socket creation failed");
  }
  struct timeval read_timeout;
  read_timeout.tv_sec = 0;
  read_timeout.tv_usec = 1;
  if (setsockopt(local_socket, SOL_SOCKET, SO_RCVTIMEO, &read_timeout,
                 sizeof(read_timeout)) < 0) {
    die("Setting timeout error");
  }
  memset((char *)&local_addr, 0, sizeof(local_addr));
  local_addr.sin_family = AF_INET;
  local_addr.sin_port = htons(PORT);
  local_addr.sin_addr.s_addr = htonl(state.ethard.local_ip_addr);
  if (bind(local_socket, (struct sockaddr *)&local_addr, sizeof(local_addr)) <
      0) {
    die("bind failed");
  }

  // Create the subscription for accepting and deserializing the message
  // Note that each message will need a dedicated socket for receiving a message
  // of a given type and subject id.
  // See the server implementation for setting up a multicast listener.
  static EthardRxSubscription rx;
  const int8_t res = ethardRxSubscribe(
      &state.ethard, EthardTransferKindMessage, state.port_id.pub.message_one,
      uavcan_si_unit_temperature_Scalar_1_0_EXTENT_BYTES_,
      ETHARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC, &rx);
  if (res < 0) {
    die("subscribe");
  }

  // Setup a session specifier to use for accepting transfers
  // Note that this session specifier should be specific to the subscription
  // and/or socket that is receiving a message
  EthardSessionSpecifier specifier = {
      .data_specifier = 0,
      .destination_route_specifier = 0,
      .source_route_specifier = 0,
  };

  while (1) {
    EthardMicrosecond monotonic_time = getMonotonicMicroseconds();
    handleLoop(&state, monotonic_time);

    // Check the transmit queue
    EthardTxQueue *const que = &state.ethard_tx_queues[0];
    const EthardTxQueueItem *tqi = ethardTxPeek(que);
    while (tqi != NULL) {
      if ((tqi->tx_deadline_usec == 0) ||
          (tqi->tx_deadline_usec > monotonic_time)) {
        // Get the specifier and print it
        printf("\n---------\n");
        specifier.source_route_specifier =
            tqi->specifier.source_route_specifier;
        specifier.destination_route_specifier =
            tqi->specifier.destination_route_specifier;
        specifier.data_specifier = tqi->specifier.data_specifier;
        // Structure used to convert number to string for ip address
        struct sockaddr_in session_addr;
        session_addr.sin_addr.s_addr = htonl(specifier.source_route_specifier);
        printf("Session Specifier:\n");
        printf("\tSource Route: %s\n", inet_ntoa(session_addr.sin_addr));
        session_addr.sin_addr.s_addr =
            htonl(specifier.destination_route_specifier);
        printf("\tDestination Route: %s\n", inet_ntoa(session_addr.sin_addr));
        printf("\tData Specifier: %d\n", specifier.data_specifier);
        printf("---------\n");

        // send the message as a multicast
        const int16_t result = send_multicast(tqi);
        if (result == 0) {
          break;
        }
        if (result < 0) {
          die("Multicast send");
        }
      }
      state.ethard.memory_free(&state.ethard, ethardTxPop(que, tqi));
      tqi = ethardTxPeek(que);
    }

    // receive a reply and print it
    // clear the buffer by filling null, it might have previously received data
    memset(buf, 0, BUFLEN);
    ssize_t read_size = recvfrom(local_socket, buf, BUFLEN, 0,
                                 (struct sockaddr *)&local_addr, &slen);
    while (read_size > 0) {
      printf("\n---------\n");
      printf("Received packet from %s:%d\n", inet_ntoa(local_addr.sin_addr),
             ntohs(local_addr.sin_port));
      printf("Read size = %d\n", read_size);
      for (int i = 0; i < read_size; i++) {
        printf("%02X ", buf[i]);
      }
      printf("\n");
      EthardFrame frame = {
          .payload = buf,
          .payload_size = read_size,
      };

      // Handle accepting the transfer
      const EthardMicrosecond timestamp_usec = getMonotonicMicroseconds();
      EthardRxTransfer transfer = {0};
      specifier.source_route_specifier = ntohl(local_addr.sin_addr.s_addr);
      specifier.data_specifier = ntohs(local_addr.sin_port);
      const int8_t ethard_result =
          ethardRxAccept(&state.ethard, timestamp_usec, &frame, 0, &specifier,
                         &transfer, NULL);
      printf("Received transfer id %lu\n", transfer.metadata.transfer_id);
      printf("Ethard Rx Accept Result %d\n", ethard_result);
      printf("---------\n");
      if (ethard_result > 0) {
        uavcan_si_unit_temperature_Scalar_1_0 msg = {0};
        if (uavcan_si_unit_temperature_Scalar_1_0_deserialize_(
                &msg, transfer.payload, &transfer.payload_size) >= 0) {
          printf("\n---------\n");
          printf("Transfer received\n");
          printf("Data: %f\n", msg.kelvin);
          printf("---------\n");
        }
        state.ethard.memory_free(&state.ethard, (void *)transfer.payload);
      }
      memset(buf, 0, BUFLEN);
      read_size = recvfrom(local_socket, buf, BUFLEN, 0,
                           (struct sockaddr *)&local_addr, &slen);
    }

    // Sleep for 1 second... This is just for demo purposes
    sleep(1);
  }

  close(local_socket);
  return 0;
}
