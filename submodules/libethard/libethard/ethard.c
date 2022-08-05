/// This software is distributed under the terms of the MIT License.
/// Copyright (c) 2016 OpenCyphal.
/// Author: Pavel Kirienko <pavel@opencyphal.org>

#include "ethard.h"
#include "cavl.h"
#include <string.h>

// --------------------------------------------- BUILD CONFIGURATION
// ---------------------------------------------

/// Define this macro to include build configuration header.
/// Usage example with CMake:
/// "-DETHARD_CONFIG_HEADER=\"${CMAKE_CURRENT_SOURCE_DIR}/my_ethard_config.h\""
#ifdef ETHARD_CONFIG_HEADER
#include ETHARD_CONFIG_HEADER
#endif

/// By default, this macro resolves to the standard assert(). The user can
/// redefine this if necessary. To disable assertion checks completely, make it
/// expand into `(void)(0)`.
#ifndef ETHARD_ASSERT
// Intentional violation of MISRA: inclusion not at the top of the file to
// eliminate unnecessary dependency on assert.h.
#include <assert.h> // NOSONAR
// Intentional violation of MISRA: assertion macro cannot be replaced with a
// function definition.
#define ETHARD_ASSERT(x) assert(x) // NOSONAR
#endif

/// Define ETHARD_CRC_TABLE=0 to use slow but ROM-efficient transfer-CRC
/// computation algorithm. Doing so is expected to save ca. 500 bytes of ROM and
/// increase the cost of RX/TX transfer processing by ~half.
#ifndef ETHARD_CRC_TABLE
#define ETHARD_CRC_TABLE 1
#endif

/// This macro is needed for testing and for library development.
#ifndef ETHARD_PRIVATE
#define ETHARD_PRIVATE static inline
#endif

#if !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 199901L)
#error "Unsupported language: ISO C99 or a newer version is required."
#endif

// --------------------------------------------- COMMON DEFINITIONS
// ---------------------------------------------

#define BITS_PER_BYTE 8U
#define BYTE_MAX 0xFFU

/// TODO - determine the minimum payload size for udp non-last frame. It should
/// be similar if not the same as CAN #define MFT_NON_LAST_FRAME_PAYLOAD_MIN 7U
/// /// The minimum payload size for the non-last frame in a multi-frame
/// transfer

#define PADDING_BYTE_VALUE 0U

#define ETHARD_END_OF_TRANSFER_OFFSET 31U
#define ETHARD_MAX_FRAME_INDEX (1U << ETHARD_END_OF_TRANSFER_OFFSET) - 1U

#define ETHARD_NODE_ID_MASK 255U /// 8 bits for now

/*
       fixed         reserved
      (9 bits)       (3 bits)
      ________          _
     /        \        / \
     11101111.0ddddddd.000sssss.ssssssss
     \__/      \_____/    \____________/
   (4 bits)    (7 bits)      (13 bits)
     IPv4      subnet-ID     subject-ID
   multicast   \_______________________/
    prefix             (23 bits)
               collision-free multicast
                  addressing limit of
                 Ethernet MAC for IPv4
*/

/// The multicast message transfer IP address node ID is formed of 3 reserved 0
/// bits and 13 bits for a subject id.
#define ETHARD_SUBJECT_ID_MASK 8191U /// 0x1FFF
#define ETHARD_SUBNET_OFFSET 16U
#define ETHARD_SUBNET_MASK (127U << ETHARD_SUBNET_OFFSET)
#define ETHARD_RESERVED_3BITS_OFFSET 13U
#define ETHARD_RESERVED_3BITS_MASK (7U << ETHARD_RESERVED_3BITS_OFFSET)
#define ETHARD_MULTICAST_OFFSET 23U
#define ETHARD_MULTICAST_PREFIX (478U << ETHARD_MULTICAST_OFFSET)
#define ETHARD_MULTICAST_ADDRESS_MASK (1U << ETHARD_MULTICAST_OFFSET) - 1U

/// Ports align with subject and service ids
/// Subjects use multicast and always use port 16383
/// Services use unicast and start with port 16384
/// Unique service id request / response are identified by initial port +
/// (service * 2) (+1 for response) A service response will always be > 16384
/// and will always be odd (port > initial && port % 2 == 1)
#define ETHARD_SUBJECT_ID_PORT 16383U
#define ETHARD_SERVICE_ID_INITIAL_PORT 16384U
#define ETHARD_SERVICE_ID_RESPONSE_MASK 1U

/// Used for inserting new items into AVL trees.
ETHARD_PRIVATE EthardTreeNode *avlTrivialFactory(void *const user_reference) {
  return (EthardTreeNode *)user_reference;
}

/// --------------------------------------------- TRANSFER CRC
/// ---------------------------------------------

typedef uint32_t TransferCRC;

#define CRC_INITIAL 0xFFFFFFFFU
#define CRC_RESIDUE 0x00000000U
#define CRC_XOR 0xFFFFFFFFU
#define CRC_SIZE_BYTES 4U

static const uint32_t CRCTable[256] = {
    0x00000000, 0xf26b8303, 0xe13b70f7, 0x1350f3f4, 0xc79a971f, 0x35f1141c,
    0x26a1e7e8, 0xd4ca64eb, 0x8ad958cf, 0x78b2dbcc, 0x6be22838, 0x9989ab3b,
    0x4d43cfd0, 0xbf284cd3, 0xac78bf27, 0x5e133c24, 0x105ec76f, 0xe235446c,
    0xf165b798, 0x030e349b, 0xd7c45070, 0x25afd373, 0x36ff2087, 0xc494a384,
    0x9a879fa0, 0x68ec1ca3, 0x7bbcef57, 0x89d76c54, 0x5d1d08bf, 0xaf768bbc,
    0xbc267848, 0x4e4dfb4b, 0x20bd8ede, 0xd2d60ddd, 0xc186fe29, 0x33ed7d2a,
    0xe72719c1, 0x154c9ac2, 0x061c6936, 0xf477ea35, 0xaa64d611, 0x580f5512,
    0x4b5fa6e6, 0xb93425e5, 0x6dfe410e, 0x9f95c20d, 0x8cc531f9, 0x7eaeb2fa,
    0x30e349b1, 0xc288cab2, 0xd1d83946, 0x23b3ba45, 0xf779deae, 0x05125dad,
    0x1642ae59, 0xe4292d5a, 0xba3a117e, 0x4851927d, 0x5b016189, 0xa96ae28a,
    0x7da08661, 0x8fcb0562, 0x9c9bf696, 0x6ef07595, 0x417b1dbc, 0xb3109ebf,
    0xa0406d4b, 0x522bee48, 0x86e18aa3, 0x748a09a0, 0x67dafa54, 0x95b17957,
    0xcba24573, 0x39c9c670, 0x2a993584, 0xd8f2b687, 0x0c38d26c, 0xfe53516f,
    0xed03a29b, 0x1f682198, 0x5125dad3, 0xa34e59d0, 0xb01eaa24, 0x42752927,
    0x96bf4dcc, 0x64d4cecf, 0x77843d3b, 0x85efbe38, 0xdbfc821c, 0x2997011f,
    0x3ac7f2eb, 0xc8ac71e8, 0x1c661503, 0xee0d9600, 0xfd5d65f4, 0x0f36e6f7,
    0x61c69362, 0x93ad1061, 0x80fde395, 0x72966096, 0xa65c047d, 0x5437877e,
    0x4767748a, 0xb50cf789, 0xeb1fcbad, 0x197448ae, 0x0a24bb5a, 0xf84f3859,
    0x2c855cb2, 0xdeeedfb1, 0xcdbe2c45, 0x3fd5af46, 0x7198540d, 0x83f3d70e,
    0x90a324fa, 0x62c8a7f9, 0xb602c312, 0x44694011, 0x5739b3e5, 0xa55230e6,
    0xfb410cc2, 0x092a8fc1, 0x1a7a7c35, 0xe811ff36, 0x3cdb9bdd, 0xceb018de,
    0xdde0eb2a, 0x2f8b6829, 0x82f63b78, 0x709db87b, 0x63cd4b8f, 0x91a6c88c,
    0x456cac67, 0xb7072f64, 0xa457dc90, 0x563c5f93, 0x082f63b7, 0xfa44e0b4,
    0xe9141340, 0x1b7f9043, 0xcfb5f4a8, 0x3dde77ab, 0x2e8e845f, 0xdce5075c,
    0x92a8fc17, 0x60c37f14, 0x73938ce0, 0x81f80fe3, 0x55326b08, 0xa759e80b,
    0xb4091bff, 0x466298fc, 0x1871a4d8, 0xea1a27db, 0xf94ad42f, 0x0b21572c,
    0xdfeb33c7, 0x2d80b0c4, 0x3ed04330, 0xccbbc033, 0xa24bb5a6, 0x502036a5,
    0x4370c551, 0xb11b4652, 0x65d122b9, 0x97baa1ba, 0x84ea524e, 0x7681d14d,
    0x2892ed69, 0xdaf96e6a, 0xc9a99d9e, 0x3bc21e9d, 0xef087a76, 0x1d63f975,
    0x0e330a81, 0xfc588982, 0xb21572c9, 0x407ef1ca, 0x532e023e, 0xa145813d,
    0x758fe5d6, 0x87e466d5, 0x94b49521, 0x66df1622, 0x38cc2a06, 0xcaa7a905,
    0xd9f75af1, 0x2b9cd9f2, 0xff56bd19, 0x0d3d3e1a, 0x1e6dcdee, 0xec064eed,
    0xc38d26c4, 0x31e6a5c7, 0x22b65633, 0xd0ddd530, 0x0417b1db, 0xf67c32d8,
    0xe52cc12c, 0x1747422f, 0x49547e0b, 0xbb3ffd08, 0xa86f0efc, 0x5a048dff,
    0x8ecee914, 0x7ca56a17, 0x6ff599e3, 0x9d9e1ae0, 0xd3d3e1ab, 0x21b862a8,
    0x32e8915c, 0xc083125f, 0x144976b4, 0xe622f5b7, 0xf5720643, 0x07198540,
    0x590ab964, 0xab613a67, 0xb831c993, 0x4a5a4a90, 0x9e902e7b, 0x6cfbad78,
    0x7fab5e8c, 0x8dc0dd8f, 0xe330a81a, 0x115b2b19, 0x020bd8ed, 0xf0605bee,
    0x24aa3f05, 0xd6c1bc06, 0xc5914ff2, 0x37faccf1, 0x69e9f0d5, 0x9b8273d6,
    0x88d28022, 0x7ab90321, 0xae7367ca, 0x5c18e4c9, 0x4f48173d, 0xbd23943e,
    0xf36e6f75, 0x0105ec76, 0x12551f82, 0xe03e9c81, 0x34f4f86a, 0xc69f7b69,
    0xd5cf889d, 0x27a40b9e, 0x79b737ba, 0x8bdcb4b9, 0x988c474d, 0x6ae7c44e,
    0xbe2da0a5, 0x4c4623a6, 0x5f16d052, 0xad7d5351};

ETHARD_PRIVATE TransferCRC crcAddByte(const TransferCRC crc,
                                      const uint8_t byte) {
  TransferCRC crc32c = (uint32_t)(crc ^ CRC_XOR);
  crc32c = CRCTable[(uint32_t)((uint32_t)(crc32c ^ byte) & BYTE_MAX)] ^
           (crc32c >> BITS_PER_BYTE);
  return (uint32_t)(crc32c ^ CRC_XOR);
}

ETHARD_PRIVATE TransferCRC crcAdd(const TransferCRC crc, const size_t size,
                                  const void *const data) {
  ETHARD_ASSERT((data != NULL) || (size == 0U));
  TransferCRC out = crc;
  const uint8_t *p = (const uint8_t *)data;
  for (size_t i = 0; i < size; i++) {
    out = crcAddByte(out, *p);
    ++p;
  }
  return out;
}

// --------------------------------------------- TRANSMISSION
// ---------------------------------------------

/// This is a subclass of EthardTxQueueItem. A pointer to this type can be cast
/// to EthardTxQueueItem safely. This is standard-compliant. The
/// paragraph 6.7.2.1.15 says:
///     A pointer to a structure object, suitably converted, points to its
///     initial member (or if that member is a bit-field, then to the unit in
///     which it resides), and vice versa. There may be unnamed padding within a
///     structure object, but not at its beginning.
typedef struct TxItem {
  EthardTxQueueItem base;
  uint8_t payload_buffer[ETHARD_MTU_MAX];
} TxItem;

/// Chain of TX frames prepared for insertion into a TX queue.
typedef struct {
  TxItem *head;
  TxItem *tail;
  size_t size;
} TxChain;

/// TODO - Determine what is needed for a Message Sessions Specifier
ETHARD_PRIVATE int32_t txMakeMessageSessionSpecifier(
    const EthardPortID subject_id, const EthardNodeID src_node_id,
    const EthardIPv4Addr local_node_addr,
    EthardSessionSpecifier *const out_spec) {
  ETHARD_ASSERT(src_node_id <= ETHARD_NODE_ID_MAX);
  ETHARD_ASSERT(subject_id <= ETHARD_SUBJECT_ID_MAX);
  /// Just the local ip address + source node id
  out_spec->source_route_specifier =
      (local_node_addr & ~(EthardIPv4Addr)ETHARD_NODE_ID_MASK) |
      (EthardIPv4Addr)src_node_id;
  out_spec->destination_route_specifier =
      ((local_node_addr & (EthardIPv4Addr)ETHARD_SUBNET_MASK) |
       (EthardIPv4Addr)ETHARD_MULTICAST_PREFIX |
       ((EthardIPv4Addr)ETHARD_SUBJECT_ID_MASK & (EthardIPv4Addr)subject_id)) &
      ~(EthardIPv4Addr)ETHARD_RESERVED_3BITS_MASK;
  out_spec->data_specifier = (EthardUdpPortID)ETHARD_SUBJECT_ID_PORT;
  return ETHARD_SUCCESS;
}

ETHARD_PRIVATE int32_t txMakeServiceSessionSpecifier(
    const EthardPortID service_id, const bool request_not_response,
    const EthardNodeID src_node_id, const EthardNodeID dst_node_id,
    const EthardIPv4Addr local_node_addr,
    EthardSessionSpecifier *const out_spec) {
  ETHARD_ASSERT(src_node_id <= ETHARD_NODE_ID_MAX);
  ETHARD_ASSERT(service_id < ETHARD_SERVICE_ID_MAX);
  /// Just the local ip address + source node id
  out_spec->source_route_specifier =
      (local_node_addr & ~(EthardIPv4Addr)ETHARD_NODE_ID_MASK) |
      (EthardIPv4Addr)src_node_id;
  out_spec->destination_route_specifier =
      (local_node_addr & ~(EthardIPv4Addr)ETHARD_NODE_ID_MASK) |
      (EthardIPv4Addr)dst_node_id;
  out_spec->data_specifier =
      (EthardUdpPortID)(ETHARD_SERVICE_ID_INITIAL_PORT + (service_id * 2U) +
                        (request_not_response ? 0U : 1U));
  return ETHARD_SUCCESS;
}

// This may need to be adjusted...
ETHARD_PRIVATE size_t adjustPresentationLayerMTU(const size_t mtu_bytes) {
  size_t mtu = 0U;
  if (mtu_bytes < ETHARD_MTU_UDP_IPV4) {
    mtu = ETHARD_MTU_UDP_IPV4;
  } else {
    mtu = ETHARD_MTU_MAX;
  }
  return mtu;
}

ETHARD_PRIVATE int32_t txMakeSessionSpecifier(
    const EthardTransferMetadata *const tr, const EthardNodeID local_node_id,
    const EthardIPv4Addr local_node_addr, const size_t presentation_layer_mtu,
    EthardSessionSpecifier *const spec) {
  ETHARD_ASSERT(tr != NULL);
  ETHARD_ASSERT(presentation_layer_mtu > 0);
  int32_t out = -ETHARD_ERROR_INVALID_ARGUMENT;
  if ((tr->transfer_kind == EthardTransferKindMessage) &&
      (ETHARD_NODE_ID_UNSET == tr->remote_node_id) &&
      (tr->port_id <= ETHARD_SUBJECT_ID_MAX)) {
    if (local_node_id <= ETHARD_NODE_ID_MAX) {
      out = txMakeMessageSessionSpecifier(tr->port_id, local_node_id,
                                          local_node_addr, spec);
      ETHARD_ASSERT(out >= 0);
    } else {
      out = -ETHARD_ERROR_INVALID_ARGUMENT; // Can't have a larger than max node
                                            // id
    }
  } else if (((tr->transfer_kind == EthardTransferKindRequest) ||
              (tr->transfer_kind == EthardTransferKindResponse)) &&
             (tr->remote_node_id <= ETHARD_NODE_ID_MAX) &&
             (tr->port_id < ETHARD_SERVICE_ID_MAX) &&
             (tr->remote_node_id != ETHARD_NODE_ID_UNSET)) {
    if (local_node_id != ETHARD_NODE_ID_UNSET) {
      out = txMakeServiceSessionSpecifier(
          tr->port_id, tr->transfer_kind == EthardTransferKindRequest,
          local_node_id, tr->remote_node_id, local_node_addr, spec);
      ETHARD_ASSERT(out >= 0);
    } else {
      out = -ETHARD_ERROR_INVALID_ARGUMENT; // Anonymous service transfers are
                                            // not allowed.
    }
  } else {
    out = -ETHARD_ERROR_INVALID_ARGUMENT;
  }

  if (out >= 0) {
    const uint32_t prio = (uint32_t)tr->priority;
    if (prio > ETHARD_PRIORITY_MAX) {
      out = -ETHARD_ERROR_INVALID_ARGUMENT; // Priority can't be greater than
                                            // max value
    }
  }
  return out;
}

/// Takes a frame payload size, returns a new size that is >=x and is rounded up
/// to the nearest valid DLC. Note: This is deprecated for UDP as there is no
/// DLC for UDP
ETHARD_PRIVATE size_t txRoundFramePayloadSizeUp(const size_t x) {
  /// TODO - determine if there is a algorithm for rounding UDP payload size
  return x;
}

ETHARD_PRIVATE void txMakeFrameHeader(EthardFrameHeader *const header,
                                      const uint8_t priority,
                                      const EthardTransferID transfer_id,
                                      const bool end_of_transfer,
                                      const uint32_t frame_index) {
  ETHARD_ASSERT(frame_index <= ETHARD_MAX_FRAME_INDEX);
  uint32_t end_of_transfer_mask = (uint32_t)(end_of_transfer ? 1 : 0)
                                  << (uint32_t)ETHARD_END_OF_TRANSFER_OFFSET;
  header->transfer_id = transfer_id;
  header->priority = priority;
  header->frame_index_eot = end_of_transfer_mask | frame_index;
}

/// The item is only allocated and initialized, but NOT included into the queue!
/// The caller needs to do that.
ETHARD_PRIVATE TxItem *txAllocateQueueItem(
    EthardInstance *const ins, const EthardSessionSpecifier *const spec,
    const EthardMicrosecond deadline_usec, const size_t payload_size) {
  ETHARD_ASSERT(ins != NULL);
  ETHARD_ASSERT(payload_size > 0U);
  TxItem *const out = (TxItem *)ins->memory_allocate(
      ins, sizeof(TxItem) - ETHARD_MTU_MAX + payload_size);
  if (out != NULL) {
    out->base.base.up = NULL;
    out->base.base.lr[0] = NULL;
    out->base.base.lr[1] = NULL;
    out->base.base.bf = 0;

    out->base.next_in_transfer = NULL; // Last by default.
    out->base.tx_deadline_usec = deadline_usec;

    out->base.frame.payload_size = payload_size;
    out->base.frame.payload = out->payload_buffer;
    out->base.specifier.data_specifier = spec->data_specifier;
    out->base.specifier.destination_route_specifier =
        spec->destination_route_specifier;
    out->base.specifier.source_route_specifier = spec->source_route_specifier;
  }
  return out;
}

/// Frames with identical UDP ID that are added later always compare greater
/// than their counterparts with same UDP ID. This ensures that UDP frames with
/// the same UDP ID are transmitted in the FIFO order. Frames that should be
/// transmitted earlier compare smaller (i.e., put on the left side of the
/// tree).
ETHARD_PRIVATE int8_t txAVLPredicate(
    void *const
        user_reference, // NOSONAR Cavl API requires pointer to non-const.
    const EthardTreeNode *const node) {
  const EthardTxQueueItem *const target =
      (const EthardTxQueueItem *)user_reference;
  const EthardTxQueueItem *const other = (const EthardTxQueueItem *)node;
  ETHARD_ASSERT((target != NULL) && (other != NULL));
  if (target->frame.udp_cyphal_header.priority >
      other->frame.udp_cyphal_header.priority) {
    return +1;
  }
  if (target->frame.udp_cyphal_header.transfer_id >=
      other->frame.udp_cyphal_header.transfer_id) {
    return +1;
  }
  return -1;
}

/// Returns the number of frames enqueued or error (i.e., =1 or <0).
ETHARD_PRIVATE int32_t
txPushSingleFrame(EthardTxQueue *const que, EthardInstance *const ins,
                  const EthardMicrosecond deadline_usec,
                  const EthardSessionSpecifier *const specifier,
                  const uint8_t priority, const EthardTransferID transfer_id,
                  const size_t payload_size, const void *const payload) {
  ETHARD_ASSERT(ins != NULL);
  ETHARD_ASSERT((payload != NULL) || (payload_size == 0));
  // The size of a Frame header shouldn't change, but best to check it is at
  // least bigger than 0
  ETHARD_ASSERT(sizeof(EthardFrameHeader) > 0); // NOLINT
  const size_t frame_payload_size = payload_size + sizeof(EthardFrameHeader);
  ETHARD_ASSERT(frame_payload_size > payload_size);
  const size_t padding_size =
      frame_payload_size - payload_size - sizeof(EthardFrameHeader);
  ETHARD_ASSERT((padding_size + payload_size + sizeof(EthardFrameHeader)) ==
                frame_payload_size);
  int32_t out = 0;
  TxItem *const tqi = (que->size < que->capacity)
                          ? txAllocateQueueItem(ins, specifier, deadline_usec,
                                                frame_payload_size)
                          : NULL;
  if (tqi != NULL) {
    if (payload_size > 0U) // The check is needed to avoid calling memcpy() with
                           // a NULL pointer, it's an UB.
    {
      ETHARD_ASSERT(payload != NULL);
      // Clang-Tidy raises an error recommending the use of memcpy_s() instead.
      // We ignore it because the safe functions are poorly supported; reliance
      // on them may limit the portability.
      (void)memcpy(&tqi->payload_buffer[sizeof(EthardFrameHeader)], payload,
                   payload_size); // NOLINT
    }
    // Clang-Tidy raises an error recommending the use of memset_s() instead.
    // We ignore it because the safe functions are poorly supported; reliance on
    // them may limit the portability.
    (void)memset(&tqi->payload_buffer[payload_size], PADDING_BYTE_VALUE,
                 padding_size); // NOLINT
    /// Create the FrameHeader
    txMakeFrameHeader(&tqi->base.frame.udp_cyphal_header, priority, transfer_id,
                      true, 1);
    // Clang-Tidy raises an error recommending the use of memcpy_s() instead.
    // We ignore it because the safe functions are poorly supported; reliance on
    // them may limit the portability.
    (void)memcpy(&tqi->payload_buffer[0], &tqi->base.frame.udp_cyphal_header,
                 sizeof(EthardFrameHeader)); // NOLINT
    // Insert the newly created TX item into the queue.
    const EthardTreeNode *const res = cavlSearch(
        &que->root, &tqi->base.base, &txAVLPredicate, &avlTrivialFactory);
    (void)res;
    ETHARD_ASSERT(res == &tqi->base.base);
    que->size++;
    ETHARD_ASSERT(que->size <= que->capacity);
    out = 1; // One frame enqueued.
  } else {
    out = -ETHARD_ERROR_OUT_OF_MEMORY;
  }
  ETHARD_ASSERT((out < 0) || (out == 1));
  return out;
}

/// Returns the number of frames enqueued or error.
ETHARD_PRIVATE int32_t txPushMultiFrame() {
  int32_t out = -ETHARD_ERROR_INVALID_ARGUMENT;
  return out;
}

// --------------------------------------------- RECEPTION
// ---------------------------------------------

#define RX_SESSIONS_PER_SUBSCRIPTION (ETHARD_NODE_ID_MAX + 1U)

/// The memory requirement model provided in the documentation assumes that the
/// maximum size of this structure never exceeds 48 bytes on any conventional
/// platform. A user that needs a detailed analysis of the worst-case memory
/// consumption may compute the size of this structure for the particular
/// platform at hand manually or by evaluating its sizeof(). The fields are
/// ordered to minimize the amount of padding on all conventional platforms.
typedef struct EthardInternalRxSession {
  EthardMicrosecond transfer_timestamp_usec; ///< Timestamp of the last received
                                             ///< start-of-transfer.
  size_t total_payload_size; ///< The payload size before the implicit
                             ///< truncation, including the CRC.
  size_t payload_size;       ///< How many bytes received so far.
  uint8_t *payload; ///< Dynamically allocated and handed off to the application
                    ///< when done.
  TransferCRC
      calculated_crc; ///< Updated with the received payload in real time.
  EthardTransferID transfer_id;
  uint8_t redundant_transport_index; ///< Arbitrary value in [0, 255].
} EthardInternalRxSession;

/// High-level transport frame model.
typedef struct {
  EthardMicrosecond timestamp_usec;
  EthardPriority priority;
  EthardTransferKind transfer_kind;
  EthardPortID port_id;
  EthardNodeID source_node_id;
  EthardNodeID destination_node_id;
  EthardTransferID transfer_id;
  bool start_of_transfer;
  bool end_of_transfer;
  size_t payload_size;
  const void *payload;
} RxFrameModel;

ETHARD_PRIVATE EthardNodeID
getNodeIdFromRouteSpecifier(EthardIPv4Addr src_ip_addr) {
  EthardNodeID out = (EthardNodeID)(src_ip_addr & ETHARD_NODE_ID_MASK);
  return out;
}

ETHARD_PRIVATE EthardNodeID getNodeIdFromRouteAndDataSpecifiers(
    EthardIPv4Addr route_specifier, EthardUdpPortID data_specifier) {
  EthardNodeID out = ETHARD_NODE_ID_UNSET;
  if (data_specifier > ETHARD_SUBJECT_ID_PORT) {
    out = getNodeIdFromRouteSpecifier(route_specifier);
  }
  return out;
}

ETHARD_PRIVATE EthardPortID getPortIdFromRouteAndDataSpecifiers(
    EthardIPv4Addr route_specifier, EthardUdpPortID data_specifier) {
  ETHARD_ASSERT(data_specifier >= ETHARD_SUBJECT_ID_PORT);
  if (data_specifier == ETHARD_SUBJECT_ID_PORT) {
    return (EthardPortID)(route_specifier & ETHARD_SUBJECT_ID_MASK);
  }
  return (data_specifier % 2 == 1)
             ? (EthardPortID)((data_specifier - ETHARD_SERVICE_ID_INITIAL_PORT -
                               1) /
                              2)
             : (EthardPortID)((data_specifier -
                               ETHARD_SERVICE_ID_INITIAL_PORT) /
                              2);
}

ETHARD_PRIVATE EthardTransferKind
getTransferKindFromDataSpecifier(EthardUdpPortID data_specifier) {
  ETHARD_ASSERT(data_specifier >= ETHARD_SUBJECT_ID_PORT);
  if (data_specifier == ETHARD_SUBJECT_ID_PORT) {
    return EthardTransferKindMessage;
  }
  return (data_specifier % 2 == 1) ? EthardTransferKindResponse
                                   : EthardTransferKindRequest;
}

/// Returns truth if the frame is valid and parsed successfully. False if the
/// frame is not a valid Cyphal/UDP frame.
ETHARD_PRIVATE bool
rxTryParseFrame(const EthardMicrosecond timestamp_usec,
                const EthardSessionSpecifier *const specifier,
                EthardFrame *const frame, RxFrameModel *const out) {
  ETHARD_ASSERT(frame != NULL);
  ETHARD_ASSERT(out != NULL);
  ETHARD_ASSERT(specifier != NULL);
  if (frame->payload_size < sizeof(frame->udp_cyphal_header)) {
    return false;
  }
  bool valid = true;
  // Get the Header out of the frame
  ETHARD_ASSERT(frame->payload != NULL);
  (void)memcpy(&frame->udp_cyphal_header, frame->payload,
               sizeof(frame->udp_cyphal_header)); // NOLINT
  out->timestamp_usec = timestamp_usec;

  out->priority = (EthardPriority)frame->udp_cyphal_header.priority;
  out->source_node_id =
      getNodeIdFromRouteSpecifier(specifier->source_route_specifier);
  out->transfer_kind =
      getTransferKindFromDataSpecifier(specifier->data_specifier);
  out->port_id = getPortIdFromRouteAndDataSpecifiers(
      specifier->destination_route_specifier, specifier->data_specifier);
  out->destination_node_id = getNodeIdFromRouteAndDataSpecifiers(
      specifier->destination_route_specifier, specifier->data_specifier);
  // Payload parsing.
  out->payload_size =
      frame->payload_size -
      sizeof(frame->udp_cyphal_header); // Cut off the header size.
  out->payload =
      (void *)((uint8_t *)frame->payload + sizeof(frame->udp_cyphal_header));

  out->transfer_id = frame->udp_cyphal_header.transfer_id;
  out->start_of_transfer = (((frame->udp_cyphal_header.frame_index_eot) &
                             (ETHARD_MAX_FRAME_INDEX)) == 1);
  out->end_of_transfer = ((frame->udp_cyphal_header.frame_index_eot >>
                           ETHARD_END_OF_TRANSFER_OFFSET) == 1);
  if (out->transfer_kind != EthardTransferKindMessage) {
    valid = valid && (out->source_node_id != out->destination_node_id);
  }
  // Anonymous transfers can be only single-frame transfers.
  valid = valid && ((out->start_of_transfer && out->end_of_transfer) ||
                    (ETHARD_NODE_ID_UNSET != out->source_node_id));
  // A frame that is a part of a multi-frame transfer cannot be empty (tail byte
  // not included).
  valid = valid && ((out->payload_size > 0) ||
                    (out->start_of_transfer && out->end_of_transfer));
  return valid;
}

ETHARD_PRIVATE void
rxInitTransferMetadataFromFrame(const RxFrameModel *const frame,
                                EthardTransferMetadata *const out_transfer) {
  ETHARD_ASSERT(frame != NULL);
  ETHARD_ASSERT(frame->payload != NULL);
  ETHARD_ASSERT(out_transfer != NULL);
  out_transfer->priority = frame->priority;
  out_transfer->transfer_kind = frame->transfer_kind;
  out_transfer->port_id = frame->port_id;
  out_transfer->remote_node_id = frame->source_node_id;
  out_transfer->transfer_id = frame->transfer_id;
}

// Assume we will never roll over a transfer id with 64bits
ETHARD_PRIVATE uint64_t rxComputeTransferIDDifference(const uint64_t a,
                                                      const uint64_t b) {
  ETHARD_ASSERT(a <= ETHARD_TRANSFER_ID_MAX);
  ETHARD_ASSERT(b <= ETHARD_TRANSFER_ID_MAX);
  return a - b;
}

ETHARD_PRIVATE int8_t rxSessionWritePayload(EthardInstance *const ins,
                                            EthardInternalRxSession *const rxs,
                                            const size_t extent,
                                            const size_t payload_size,
                                            const void *const payload) {
  ETHARD_ASSERT(ins != NULL);
  ETHARD_ASSERT(rxs != NULL);
  ETHARD_ASSERT((payload != NULL) || (payload_size == 0U));
  ETHARD_ASSERT(
      rxs->payload_size <=
      extent); // This invariant is enforced by the subscription logic.
  ETHARD_ASSERT(rxs->payload_size <= rxs->total_payload_size);

  rxs->total_payload_size += payload_size;

  // Allocate the payload lazily, as late as possible.
  if ((NULL == rxs->payload) && (extent > 0U)) {
    ETHARD_ASSERT(rxs->payload_size == 0);
    rxs->payload = ins->memory_allocate(ins, extent);
  }

  int8_t out = 0;
  if (rxs->payload != NULL) {
    // Copy the payload into the contiguous buffer. Apply the implicit
    // truncation rule if necessary.
    size_t bytes_to_copy = payload_size;
    if ((rxs->payload_size + bytes_to_copy) > extent) {
      ETHARD_ASSERT(rxs->payload_size <= extent);
      bytes_to_copy = extent - rxs->payload_size;
      ETHARD_ASSERT((rxs->payload_size + bytes_to_copy) == extent);
      ETHARD_ASSERT(bytes_to_copy < payload_size);
    }
    // This memcpy() call here is one of the two variable-complexity operations
    // in the RX pipeline; the other one is the search of the matching
    // subscription state. Excepting these two cases, the entire RX pipeline
    // contains neither loops nor recursion. Intentional violation of MISRA:
    // indexing on a pointer. This is done to avoid pointer arithmetics.
    // Clang-Tidy raises an error recommending the use of memcpy_s() instead.
    // We ignore it because the safe functions are poorly supported; reliance on
    // them may limit the portability.
    (void)memcpy(&rxs->payload[rxs->payload_size], payload,
                 bytes_to_copy); // NOLINT NOSONAR
    rxs->payload_size += bytes_to_copy;
    ETHARD_ASSERT(rxs->payload_size <= extent);
  } else {
    ETHARD_ASSERT(rxs->payload_size == 0);
    out = (extent > 0U) ? -ETHARD_ERROR_OUT_OF_MEMORY : 0;
  }
  ETHARD_ASSERT(out <= 0);
  return out;
}

ETHARD_PRIVATE void rxSessionRestart(EthardInstance *const ins,
                                     EthardInternalRxSession *const rxs) {
  ETHARD_ASSERT(ins != NULL);
  ETHARD_ASSERT(rxs != NULL);
  ins->memory_free(ins, rxs->payload); // May be NULL, which is OK.
  rxs->total_payload_size = 0U;
  rxs->payload_size = 0U;
  rxs->payload = NULL;
  rxs->calculated_crc = CRC_INITIAL;
  rxs->transfer_id =
      (EthardTransferID)(rxs->transfer_id + 1U) & ETHARD_TRANSFER_ID_MAX;
}

ETHARD_PRIVATE int8_t rxSessionAcceptFrame(
    EthardInstance *const ins, EthardInternalRxSession *const rxs,
    const RxFrameModel *const frame, const size_t extent,
    EthardRxTransfer *const out_transfer) {
  ETHARD_ASSERT(ins != NULL);
  ETHARD_ASSERT(rxs != NULL);
  ETHARD_ASSERT(frame != NULL);
  ETHARD_ASSERT(frame->payload != NULL);
  ETHARD_ASSERT(frame->transfer_id <= ETHARD_TRANSFER_ID_MAX);
  ETHARD_ASSERT(out_transfer != NULL);

  if (frame->start_of_transfer) // The transfer timestamp is the timestamp of
                                // its first frame.
  {
    rxs->transfer_timestamp_usec = frame->timestamp_usec;
  }

  const bool single_frame = frame->start_of_transfer && frame->end_of_transfer;
  if (!single_frame) {
    // Not currently supporting multiframe transfers
    rxSessionRestart(ins, rxs);
    return -ETHARD_ERROR_INVALID_ARGUMENT;
  }

  int8_t out = rxSessionWritePayload(ins, rxs, extent, frame->payload_size,
                                     frame->payload);
  if (out < 0) {
    ETHARD_ASSERT(-ETHARD_ERROR_OUT_OF_MEMORY == out);
    rxSessionRestart(ins, rxs); // Out-of-memory.
  } else if (frame->end_of_transfer) {
    ETHARD_ASSERT(0 == out);
    if (single_frame || (CRC_RESIDUE == rxs->calculated_crc)) {
      out = 1; // One transfer received, notify the application.
      rxInitTransferMetadataFromFrame(frame, &out_transfer->metadata);
      out_transfer->timestamp_usec = rxs->transfer_timestamp_usec;
      out_transfer->payload_size = rxs->payload_size;
      out_transfer->payload = rxs->payload;

      /* There is no CRC in single frame transfers and multiframe transfers are
         not supported yet
            // Cut off the CRC from the payload if it's there -- we don't want
         to expose it to the user. ETHARD_ASSERT(rxs->total_payload_size >=
         rxs->payload_size); const size_t truncated_amount =
         rxs->total_payload_size - rxs->payload_size; if ((!single_frame) &&
         (CRC_SIZE_BYTES > truncated_amount))  // Single-frame transfers don't
         have CRC.
            {
                ETHARD_ASSERT(out_transfer->payload_size >= (CRC_SIZE_BYTES -
         truncated_amount)); out_transfer->payload_size -= CRC_SIZE_BYTES -
         truncated_amount;
            }
      */

      rxs->payload = NULL; // Ownership passed over to the application, nullify
                           // to prevent freeing.
    }
    rxSessionRestart(ins, rxs); // Successful completion.
  }
  return out;
}

/// RX session state machine update is the most intricate part of any Cyphal
/// transport implementation. The state model used here is derived from the
/// reference pseudocode given in the original UAVCAN v0 specification. The
/// Cyphal/UDP v1 specification, which this library is an implementation of,
/// does not provide any reference pseudocode. Instead, it takes a higher-level,
/// more abstract approach, where only the high-level requirements are given and
/// the particular algorithms are left to be implementation-defined. Such
/// abstract approach is much advantageous because it allows implementers to
/// choose whatever solution works best for the specific application at hand,
/// while the wire compatibility is still guaranteed by the high-level
/// requirements given in the specification.
ETHARD_PRIVATE int8_t rxSessionUpdate(
    EthardInstance *const ins, EthardInternalRxSession *const rxs,
    const RxFrameModel *const frame, const uint8_t redundant_transport_index,
    const EthardMicrosecond transfer_id_timeout_usec, const size_t extent,
    EthardRxTransfer *const out_transfer) {
  ETHARD_ASSERT(ins != NULL);
  ETHARD_ASSERT(rxs != NULL);
  ETHARD_ASSERT(frame != NULL);
  ETHARD_ASSERT(out_transfer != NULL);
  ETHARD_ASSERT(rxs->transfer_id <= ETHARD_TRANSFER_ID_MAX);
  ETHARD_ASSERT(frame->transfer_id <= ETHARD_TRANSFER_ID_MAX);

  const bool tid_timed_out =
      (frame->timestamp_usec > rxs->transfer_timestamp_usec) &&
      ((frame->timestamp_usec - rxs->transfer_timestamp_usec) >
       transfer_id_timeout_usec);

  const bool not_previous_tid =
      rxComputeTransferIDDifference(rxs->transfer_id, frame->transfer_id) > 1;

  const bool need_restart =
      tid_timed_out ||
      ((rxs->redundant_transport_index == redundant_transport_index) &&
       frame->start_of_transfer && not_previous_tid);

  if (need_restart) {
    rxs->total_payload_size = 0U;
    rxs->payload_size = 0U;
    rxs->calculated_crc = CRC_INITIAL;
    rxs->transfer_id = frame->transfer_id;
    rxs->redundant_transport_index = redundant_transport_index;
  }

  int8_t out = 0;
  if (need_restart && (!frame->start_of_transfer)) {
    rxSessionRestart(ins, rxs); // SOT-miss, no point going further.
  } else {
    const bool correct_transport =
        (rxs->redundant_transport_index == redundant_transport_index);
    const bool correct_tid = (frame->transfer_id == rxs->transfer_id);
    if (correct_transport && correct_tid) {
      out = rxSessionAcceptFrame(ins, rxs, frame, extent, out_transfer);
    }
  }
  return out;
}

ETHARD_PRIVATE int8_t rxAcceptFrame(EthardInstance *const ins,
                                    EthardRxSubscription *const subscription,
                                    const RxFrameModel *const frame,
                                    const uint8_t redundant_transport_index,
                                    EthardRxTransfer *const out_transfer) {
  ETHARD_ASSERT(ins != NULL);
  ETHARD_ASSERT(subscription != NULL);
  ETHARD_ASSERT(subscription->port_id == frame->port_id);
  ETHARD_ASSERT(frame != NULL);
  ETHARD_ASSERT(frame->payload != NULL);
  ETHARD_ASSERT(frame->transfer_id <= ETHARD_TRANSFER_ID_MAX);
  ETHARD_ASSERT((ETHARD_NODE_ID_UNSET == frame->destination_node_id) ||
                (ins->node_id == frame->destination_node_id));
  ETHARD_ASSERT(out_transfer != NULL);

  int8_t out = 0;
  if ((frame->source_node_id <= ETHARD_NODE_ID_MAX) &&
      (frame->source_node_id != ETHARD_NODE_ID_UNSET)) {
    // If such session does not exist, create it. This only makes sense if this
    // is the first frame of a transfer, otherwise, we won't be able to receive
    // the transfer anyway so we don't bother.
    if ((NULL == subscription->sessions[frame->source_node_id]) &&
        frame->start_of_transfer) {
      EthardInternalRxSession *const rxs =
          (EthardInternalRxSession *)ins->memory_allocate(
              ins, sizeof(EthardInternalRxSession));
      subscription->sessions[frame->source_node_id] = rxs;
      if (rxs != NULL) {
        rxs->transfer_timestamp_usec = frame->timestamp_usec;
        rxs->total_payload_size = 0U;
        rxs->payload_size = 0U;
        rxs->payload = NULL;
        rxs->calculated_crc = CRC_INITIAL;
        rxs->transfer_id = frame->transfer_id;
        rxs->redundant_transport_index = redundant_transport_index;
      } else {
        out = -ETHARD_ERROR_OUT_OF_MEMORY;
      }
    }
    // There are two possible reasons why the session may not exist: 1. OOM; 2.
    // SOT-miss.
    if (subscription->sessions[frame->source_node_id] != NULL) {
      ETHARD_ASSERT(out == 0);
      out = rxSessionUpdate(ins, subscription->sessions[frame->source_node_id],
                            frame, redundant_transport_index,
                            subscription->transfer_id_timeout_usec,
                            subscription->extent, out_transfer);
    }
  } else {
    ETHARD_ASSERT(frame->source_node_id == ETHARD_NODE_ID_UNSET);
    // Anonymous transfers are stateless. No need to update the state machine,
    // just blindly accept it. We have to copy the data into an allocated
    // storage because the API expects it: the lifetime shall be independent of
    // the input data and the memory shall be free-able.
    const size_t payload_size = (subscription->extent < frame->payload_size)
                                    ? subscription->extent
                                    : frame->payload_size;
    void *const payload = ins->memory_allocate(ins, payload_size);
    if (payload != NULL) {
      rxInitTransferMetadataFromFrame(frame, &out_transfer->metadata);
      out_transfer->timestamp_usec = frame->timestamp_usec;
      out_transfer->payload_size = payload_size;
      out_transfer->payload = payload;
      // Clang-Tidy raises an error recommending the use of memcpy_s() instead.
      // We ignore it because the safe functions are poorly supported; reliance
      // on them may limit the portability.
      (void)memcpy(payload, frame->payload, payload_size); // NOLINT
      out = 1;
    } else {
      out = -ETHARD_ERROR_OUT_OF_MEMORY;
    }
  }
  return out;
}

ETHARD_PRIVATE int8_t rxSubscriptionPredicateOnPortID(
    void *const
        user_reference, // NOSONAR Cavl API requires pointer to non-const.
    const EthardTreeNode *const node) {
  const EthardPortID sought = *((const EthardPortID *)user_reference);
  const EthardPortID other = ((const EthardRxSubscription *)node)->port_id;
  static const int8_t NegPos[2] = {-1, +1};
  // Clang-Tidy mistakenly identifies a narrowing cast to int8_t here, which is
  // incorrect.
  return (sought == other)
             ? 0
             : NegPos[sought > other]; // NOLINT no narrowing conversion is
                                       // taking place here
}

ETHARD_PRIVATE int8_t rxSubscriptionPredicateOnStruct(
    void *const
        user_reference, // NOSONAR Cavl API requires pointer to non-const.
    const EthardTreeNode *const node) {
  return rxSubscriptionPredicateOnPortID(
      &((EthardRxSubscription *)user_reference)->port_id, node);
}

// --------------------------------------------- PUBLIC API
// ---------------------------------------------

EthardInstance ethardInit(const EthardMemoryAllocate memory_allocate,
                          const EthardMemoryFree memory_free) {
  ETHARD_ASSERT(memory_allocate != NULL);
  ETHARD_ASSERT(memory_free != NULL);
  const EthardInstance out = {
      .user_reference = NULL,
      .node_id = ETHARD_NODE_ID_UNSET,
      .memory_allocate = memory_allocate,
      .memory_free = memory_free,
      .rx_subscriptions = {NULL, NULL, NULL},
  };
  return out;
}

EthardTxQueue ethardTxInit(const size_t capacity, const size_t mtu_bytes) {
  EthardTxQueue out = {
      .capacity = capacity,
      .mtu_bytes = mtu_bytes,
      .size = 0,
      .root = NULL,
      .user_reference = NULL,
  };
  return out;
}

int32_t ethardTxPush(EthardTxQueue *const que, EthardInstance *const ins,
                     const EthardMicrosecond tx_deadline_usec,
                     const EthardTransferMetadata *const metadata,
                     const size_t payload_size, const void *const payload) {
  int32_t out = -ETHARD_ERROR_INVALID_ARGUMENT;
  if ((ins != NULL) && (que != NULL) && (metadata != NULL) &&
      ((payload != NULL) || (0U == payload_size))) {
    const size_t pl_mtu = adjustPresentationLayerMTU(que->mtu_bytes);
    EthardSessionSpecifier specifier;
    const int32_t specifier_result = txMakeSessionSpecifier(
        metadata, ins->node_id, ins->local_ip_addr, pl_mtu, &specifier);
    if (specifier_result >= 0) {
      if (payload_size <= pl_mtu) {
        out = txPushSingleFrame(que, ins, tx_deadline_usec, &specifier,
                                metadata->priority, metadata->transfer_id,
                                payload_size, payload);
        ETHARD_ASSERT((out < 0) || (out == 1));
      } else {
        out = txPushMultiFrame();
        ETHARD_ASSERT((out < 0) || (out >= 2));
      }
    } else {
      out = specifier_result;
    }
  }
  ETHARD_ASSERT(out != 0);
  return out;
}

const EthardTxQueueItem *ethardTxPeek(const EthardTxQueue *const que) {
  const EthardTxQueueItem *out = NULL;
  if (que != NULL) {
    // Paragraph 6.7.2.1.15 of the C standard says:
    //     A pointer to a structure object, suitably converted, points to its
    //     initial member, and vice versa.
    out = (const EthardTxQueueItem *)cavlFindExtremum(que->root, false);
  }
  return out;
}

EthardTxQueueItem *ethardTxPop(EthardTxQueue *const que,
                               const EthardTxQueueItem *const item) {
  EthardTxQueueItem *out = NULL;
  if ((que != NULL) && (item != NULL)) {
    // Intentional violation of MISRA: casting away const qualifier. This is
    // considered safe because the API contract dictates that the pointer shall
    // point to a mutable entity in RAM previously allocated by the memory
    // manager. It is difficult to avoid this cast in this context.
    out = (EthardTxQueueItem *)item; // NOSONAR casting away const qualifier.
    // Paragraph 6.7.2.1.15 of the C standard says:
    //     A pointer to a structure object, suitably converted, points to its
    //     initial member, and vice versa.
    // Note that the highest-priority frame is always a leaf node in the AVL
    // tree, which means that it is very cheap to remove.
    cavlRemove(&que->root, &item->base);
    que->size--;
  }
  return out;
}

int8_t ethardRxAccept(EthardInstance *const ins,
                      const EthardMicrosecond timestamp_usec,
                      EthardFrame *const frame,
                      const uint8_t redundant_transport_index,
                      EthardSessionSpecifier *const specifier,
                      EthardRxTransfer *const out_transfer,
                      EthardRxSubscription **const out_subscription) {
  int8_t out = -ETHARD_ERROR_INVALID_ARGUMENT;
  if ((ins != NULL) && (out_transfer != NULL) && (frame != NULL) &&
      ((frame->payload != NULL) ||
       (sizeof(frame->udp_cyphal_header) == frame->payload_size))) {
    RxFrameModel model = {0};
    if (rxTryParseFrame(timestamp_usec, specifier, frame, &model)) {
      if ((ETHARD_NODE_ID_UNSET == model.destination_node_id) ||
          (ins->node_id == model.destination_node_id)) {
        // This is the reason the function has a logarithmic time complexity of
        // the number of subscriptions. Note also that this one of the two
        // variable-complexity operations in the RX pipeline; the other one is
        // memcpy(). Excepting these two cases, the entire RX pipeline contains
        // neither loops nor recursion.
        EthardRxSubscription *const sub = (EthardRxSubscription *)cavlSearch(
            &ins->rx_subscriptions[(size_t)model.transfer_kind], &model.port_id,
            &rxSubscriptionPredicateOnPortID, NULL);
        if (out_subscription != NULL) {
          *out_subscription = sub; // Expose selected instance to the caller.
        }
        if (sub != NULL) {
          ETHARD_ASSERT(sub->port_id == model.port_id);
          out = rxAcceptFrame(ins, sub, &model, redundant_transport_index,
                              out_transfer);
        } else {
          out = 0; // No matching subscription.
        }
      } else {
        out = 0; // Mis-addressed frame (normally it should be filtered out by
                 // the hardware).
      }
    } else {
      out = 0; // A non-Cyphal/UDP input frame.
    }
  }
  ETHARD_ASSERT(out <= 1);
  return out;
}

/// DONE -> This shouldn't change from canard to ethard
int8_t ethardRxSubscribe(EthardInstance *const ins,
                         const EthardTransferKind transfer_kind,
                         const EthardPortID port_id, const size_t extent,
                         const EthardMicrosecond transfer_id_timeout_usec,
                         EthardRxSubscription *const out_subscription) {
  int8_t out = -ETHARD_ERROR_INVALID_ARGUMENT;
  const size_t tk = (size_t)transfer_kind;
  if ((ins != NULL) && (out_subscription != NULL) &&
      (tk < ETHARD_NUM_TRANSFER_KINDS)) {
    // Reset to the initial state. This is absolutely critical because the new
    // payload size limit may be larger than the old value; if there are any
    // payload buffers allocated, we may overrun them because they are shorter
    // than the new payload limit. So we clear the subscription and thus ensure
    // that no overrun may occur.
    out = ethardRxUnsubscribe(ins, transfer_kind, port_id);
    if (out >= 0) {
      out_subscription->transfer_id_timeout_usec = transfer_id_timeout_usec;
      out_subscription->extent = extent;
      out_subscription->port_id = port_id;
      for (size_t i = 0; i < RX_SESSIONS_PER_SUBSCRIPTION; i++) {
        // The sessions will be created ad-hoc. Normally, for a low-jitter
        // deterministic system, we could have pre-allocated sessions here, but
        // that requires too much memory to be feasible. We could accept an
        // extra argument that would instruct us to pre-allocate sessions here?
        out_subscription->sessions[i] = NULL;
      }
      const EthardTreeNode *const res =
          cavlSearch(&ins->rx_subscriptions[tk], out_subscription,
                     &rxSubscriptionPredicateOnStruct, &avlTrivialFactory);
      (void)res;
      ETHARD_ASSERT(res == &out_subscription->base);
      out = (out > 0) ? 0 : 1;
    }
  }
  return out;
}

/// DONE -> This shouldn't change from canard to ethard
int8_t ethardRxUnsubscribe(EthardInstance *const ins,
                           const EthardTransferKind transfer_kind,
                           const EthardPortID port_id) {
  int8_t out = -ETHARD_ERROR_INVALID_ARGUMENT;
  const size_t tk = (size_t)transfer_kind;
  if ((ins != NULL) && (tk < ETHARD_NUM_TRANSFER_KINDS)) {
    EthardPortID port_id_mutable = port_id;
    EthardRxSubscription *const sub = (EthardRxSubscription *)cavlSearch(
        &ins->rx_subscriptions[tk], &port_id_mutable,
        &rxSubscriptionPredicateOnPortID, NULL);
    if (sub != NULL) {
      cavlRemove(&ins->rx_subscriptions[tk], &sub->base);
      ETHARD_ASSERT(sub->port_id == port_id);
      out = 1;
      for (size_t i = 0; i < RX_SESSIONS_PER_SUBSCRIPTION; i++) {
        ins->memory_free(
            ins, (sub->sessions[i] != NULL) ? sub->sessions[i]->payload : NULL);
        ins->memory_free(ins, sub->sessions[i]);
        sub->sessions[i] = NULL;
      }
    } else {
      out = 0;
    }
  }
  return out;
}
