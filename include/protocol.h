#ifndef NERVFS_PROTOCOL_H
#define NERVFS_PROTOCOL_H

#include <stdint.h>

#define NERVFS_MAGIC 0xAE
#define NERVFS_VERSION 1

/* fixed 16 byte header for every message, see documentation section 4 */
#pragma pack(push, 1)
typedef struct {
    uint8_t  magic;
    uint8_t  version;
    uint16_t opcode;
    uint32_t payload_len;
    uint64_t reserved;
} nervfs_header_t;
#pragma pack(pop)

/* opcodes */
#define OP_REQ_UPLOAD    0x01
#define OP_REQ_DOWNLOAD  0x02
#define OP_REQ_LIST      0x03
#define OP_REQ_DELETE    0x04
#define OP_REQ_CHMOD     0x05
#define OP_RESP_OK       0x80
#define OP_RESP_ERR      0x81
#define OP_RESP_FILE_DATA 0x82
#define OP_RESP_LIST_DATA 0x83

/* error codes for RESP_ERR */
#define ERR_NOT_FOUND     404
#define ERR_FORBIDDEN     403
#define ERR_ALREADY_EXISTS 409
#define ERR_INTERNAL      500
#define ERR_BAD_REQUEST   400

/* phase 3 will add parsing and serialization functions here */

#endif
