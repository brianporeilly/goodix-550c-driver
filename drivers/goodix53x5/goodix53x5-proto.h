/*
 * Goodix 53x5 driver for libfprint — Protocol layer
 * Copyright (C) 2024 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include <glib.h>

/* Wire message format: [cmd_byte(1)][size(2 LE)][payload(N)][checksum(1)]
 * cmd_byte = category<<4 | command<<1
 * checksum = (0xAA - sum(all_bytes)) & 0xFF, or 0x88 for handshake */

/* Max reassembly buffer (encrypted image can be ~15KB) */
#define GOODIX_RX_BUF_SIZE (16 * 1024)

/*
 * Pack-layer flags (550c / GOODIX_VARIANT_TLS_PSK only). The 53x5 family sends
 * bare messages; the 550c wraps every message in an outer pack:
 *   [flag(1)][payload_len(2 LE)][checksum(1) = (flag+lenlo+lenhi) & 0xFF][payload]
 * and chunks it into raw 64-byte USB writes with no continuation markers.
 */
#define GOODIX_PACK_FLAG_MESSAGE 0xA0  /* payload = a normal message frame  */
#define GOODIX_PACK_FLAG_TLS     0xB0  /* payload = raw TLS handshake bytes */
#define GOODIX_PACK_FLAG_TLS_DATA 0xB2 /* payload = a TLS app-data record   */

/* --- Reassembly buffer --- */
typedef struct
{
  guint8 *buf;       /* heap-allocated, GOODIX_RX_BUF_SIZE bytes */
  gsize   len;       /* bytes accumulated */
  gsize   expected;  /* total size from header (including header+checksum) */
  guint8  cmd_byte;  /* command byte from first chunk (bare-message framing) */
  gboolean pack_layer; /* TRUE: 550c pack framing (no continuation markers) */
  guint8  pack_flag; /* pack flag from first chunk when pack_layer */
} GoodixReassembly;

/*
 * Wrap @payload in a 550c pack with @flag. Returns a newly allocated buffer
 * (caller frees with g_free); *out_len is the packed length (before USB
 * padding).
 */
guint8  *goodix_proto_wrap_pack (guint8        flag,
                                 const guint8 *payload,
                                 gsize         payload_len,
                                 gsize        *out_len);

/*
 * Accessor for a completed pack (pack_layer only): returns the pack flag and a
 * pointer to the raw pack payload inside the rx buffer (valid until reset).
 */
gboolean goodix_proto_rx_pack (GoodixReassembly *rx,
                               guint8           *out_flag,
                               const guint8    **out_payload,
                               gsize            *out_payload_len);

guint8  *goodix_proto_build_message (guint8   category,
                                     guint8   command,
                                     const guint8 *payload,
                                     gsize    payload_len,
                                     gboolean use_checksum,
                                     gsize   *out_len);

gboolean goodix_proto_validate_checksum (const guint8 *data,
                                         gsize         len);

void     goodix_proto_rx_reset (GoodixReassembly *rx);
gboolean goodix_proto_rx_feed_chunk (GoodixReassembly *rx,
                                     const guint8     *chunk,
                                     gsize             chunk_len);
gboolean goodix_proto_rx_complete (GoodixReassembly *rx);

/* out_payload points into the reassembly buffer — it is only valid until the
 * next receive reset. */
gboolean goodix_proto_rx_parse (GoodixReassembly *rx,
                                guint8           *out_category,
                                guint8           *out_command,
                                const guint8    **out_payload,
                                gsize            *out_payload_len);

void goodix_proto_build_mcu_message (guint32       data_type,
                                     const guint8 *data,
                                     gsize         data_len,
                                     guint8      **out_payload,
                                     gsize        *out_payload_len);

gboolean goodix_proto_parse_mcu_message (const guint8 *payload,
                                         gsize         payload_len,
                                         guint32       expected_type,
                                         const guint8 **out_data,
                                         gsize         *out_data_len);

gboolean goodix_proto_parse_production_read (const guint8  *payload,
                                             gsize          payload_len,
                                             guint32        expected_type,
                                             const guint8 **out_data,
                                             gsize         *out_data_len);
