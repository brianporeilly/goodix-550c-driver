/*
 * Goodix 53x5 driver for libfprint — Standard TLS 1.2-PSK engine (550c variant)
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
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

/*
 * Unlike the 53x5 family (which uses the custom "GTLS" channel implemented in
 * goodix53x5-crypto.c), the 550c wraps sensor images in a *standard* TLS 1.2
 * PSK session: the sensor is the TLS client, the host is the TLS server, PSK
 * identity "Client_identity", cipher suite PSK-AES128 at security level 0.
 *
 * This module is a thin, libfprint-independent wrapper around an OpenSSL
 * server-side SSL object driven through memory BIOs, so the caller ferries the
 * raw handshake/record bytes over the Goodix USB command layer. It replaces the
 * prototype's `openssl s_server` subprocess bridge with an in-process engine.
 */

#pragma once

#include <glib.h>

typedef struct _GoodixTls GoodixTls;

/* Create a TLS-1.2-PSK server engine bound to @psk. Returns NULL on failure. */
GoodixTls *goodix_tls_new (const guint8 *psk,
                           gsize         psk_len);

void       goodix_tls_free (GoodixTls *tls);

/*
 * Drive the handshake one step. Feed @in bytes received from the device (may be
 * NULL/0 on the first call if the device speaks first — for the 550c the device
 * sends ClientHello first, so the first call passes it in). On return:
 *   - @out / @out_len: newly-allocated bytes the caller must send to the device
 *     (NULL/0 if none this step); free with g_free().
 *   - @done: TRUE once the handshake has completed.
 * Returns FALSE (with @error set) only on a fatal TLS error.
 */
gboolean   goodix_tls_handshake_pump (GoodixTls     *tls,
                                      const guint8  *in,
                                      gsize          in_len,
                                      guint8       **out,
                                      gsize         *out_len,
                                      gboolean      *done,
                                      GError       **error);

/*
 * Decrypt application-data records. Feed the encrypted TLS record bytes @in
 * (17 03 03 ...) received from the device; returns the decrypted plaintext in
 * @plain / @plain_len (newly allocated, free with g_free()). @plain may be
 * empty if a full record has not yet arrived.
 */
gboolean   goodix_tls_decrypt (GoodixTls     *tls,
                               const guint8  *in,
                               gsize          in_len,
                               guint8       **plain,
                               gsize         *plain_len,
                               GError       **error);

/* TRUE once the handshake has finished. */
gboolean   goodix_tls_is_established (GoodixTls *tls);
