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

#include "goodix53x5-tls.h"

#include <gio/gio.h>   /* G_IO_ERROR (kept libfprint-independent for unit tests) */
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <string.h>

#define GOODIX_TLS_PSK_MAX 64
#define GOODIX_TLS_DRAIN_CHUNK 4096

struct _GoodixTls
{
  SSL_CTX *ctx;
  SSL     *ssl;
  BIO     *rbio;   /* bytes from device feed in here */
  BIO     *wbio;   /* bytes to send to device drain out here */
  guint8   psk[GOODIX_TLS_PSK_MAX];
  guint    psk_len;
};

/* ex_data index used to retrieve the GoodixTls* inside the PSK callback. */
static int goodix_tls_ex_index = -1;

static void
goodix_tls_ensure_ex_index (void)
{
  if (goodix_tls_ex_index == -1)
    goodix_tls_ex_index =
      SSL_get_ex_new_index (0, (void *) "GoodixTls", NULL, NULL, NULL);
}

/*
 * Server-side PSK callback. The 550c sends identity "Client_identity"; we do
 * not gate on it (the PSK itself is the secret), we just hand back our key.
 */
static unsigned int
goodix_tls_psk_server_cb (SSL          *ssl,
                          const char   *identity,
                          unsigned char *psk_out,
                          unsigned int   max_psk_len)
{
  GoodixTls *tls = SSL_get_ex_data (ssl, goodix_tls_ex_index);

  (void) identity;
  if (tls == NULL || tls->psk_len == 0 || tls->psk_len > max_psk_len)
    return 0;

  memcpy (psk_out, tls->psk, tls->psk_len);
  return tls->psk_len;
}

static gchar *
goodix_tls_pop_error (void)
{
  unsigned long e = ERR_get_error ();
  char buf[256];

  if (e == 0)
    return g_strdup ("unknown TLS error");
  ERR_error_string_n (e, buf, sizeof (buf));
  return g_strdup (buf);
}

GoodixTls *
goodix_tls_new (const guint8 *psk,
                gsize         psk_len)
{
  GoodixTls *tls;

  if (psk == NULL || psk_len == 0 || psk_len > GOODIX_TLS_PSK_MAX)
    return NULL;

  goodix_tls_ensure_ex_index ();

  tls = g_new0 (GoodixTls, 1);
  memcpy (tls->psk, psk, psk_len);
  tls->psk_len = (guint) psk_len;

  tls->ctx = SSL_CTX_new (TLS_server_method ());
  if (tls->ctx == NULL)
    goto fail;

  /* Force TLS 1.2 (the sensor firmware speaks exactly TLS 1.2-PSK). */
  SSL_CTX_set_min_proto_version (tls->ctx, TLS1_2_VERSION);
  SSL_CTX_set_max_proto_version (tls->ctx, TLS1_2_VERSION);

  /* PSK cipher suites live below the default security level. */
  SSL_CTX_set_security_level (tls->ctx, 0);
  if (SSL_CTX_set_cipher_list (tls->ctx, "PSK") != 1)
    goto fail;

  SSL_CTX_set_psk_server_callback (tls->ctx, goodix_tls_psk_server_cb);

  tls->ssl = SSL_new (tls->ctx);
  if (tls->ssl == NULL)
    goto fail;

  SSL_set_ex_data (tls->ssl, goodix_tls_ex_index, tls);

  tls->rbio = BIO_new (BIO_s_mem ());
  tls->wbio = BIO_new (BIO_s_mem ());
  if (tls->rbio == NULL || tls->wbio == NULL)
    goto fail;

  /* SSL_set_bio takes ownership of the BIOs. */
  SSL_set_bio (tls->ssl, tls->rbio, tls->wbio);
  SSL_set_accept_state (tls->ssl);

  return tls;

fail:
  goodix_tls_free (tls);
  return NULL;
}

void
goodix_tls_free (GoodixTls *tls)
{
  if (tls == NULL)
    return;

  if (tls->ssl)
    SSL_free (tls->ssl);           /* also frees rbio/wbio */
  else
    {
      if (tls->rbio)
        BIO_free (tls->rbio);
      if (tls->wbio)
        BIO_free (tls->wbio);
    }
  if (tls->ctx)
    SSL_CTX_free (tls->ctx);

  memset (tls->psk, 0, sizeof (tls->psk));
  g_free (tls);
}

/* Drain everything queued in wbio into a newly-allocated buffer. */
static void
goodix_tls_drain_wbio (GoodixTls *tls,
                       guint8   **out,
                       gsize     *out_len)
{
  GByteArray *acc = g_byte_array_new ();
  guint8 chunk[GOODIX_TLS_DRAIN_CHUNK];
  int n;

  while ((n = BIO_read (tls->wbio, chunk, sizeof (chunk))) > 0)
    g_byte_array_append (acc, chunk, (guint) n);

  *out_len = acc->len;
  *out = g_byte_array_free (acc, acc->len == 0); /* free seg if empty */
  if (*out_len == 0)
    *out = NULL;
}

gboolean
goodix_tls_handshake_pump (GoodixTls     *tls,
                           const guint8  *in,
                           gsize          in_len,
                           guint8       **out,
                           gsize         *out_len,
                           gboolean      *done,
                           GError       **error)
{
  int ret, ssl_err;

  *out = NULL;
  *out_len = 0;
  *done = FALSE;

  if (in != NULL && in_len > 0)
    {
      if (BIO_write (tls->rbio, in, (int) in_len) <= 0)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "TLS: failed to buffer inbound bytes");
          return FALSE;
        }
    }

  ret = SSL_accept (tls->ssl);
  if (ret <= 0)
    {
      ssl_err = SSL_get_error (tls->ssl, ret);
      if (ssl_err != SSL_ERROR_WANT_READ && ssl_err != SSL_ERROR_WANT_WRITE)
        {
          g_autofree gchar *msg = goodix_tls_pop_error ();
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "TLS handshake failed: %s", msg);
          return FALSE;
        }
    }

  /* Whatever the SSL engine wants to send this step. */
  goodix_tls_drain_wbio (tls, out, out_len);

  *done = SSL_is_init_finished (tls->ssl);
  return TRUE;
}

gboolean
goodix_tls_decrypt (GoodixTls     *tls,
                    const guint8  *in,
                    gsize          in_len,
                    guint8       **plain,
                    gsize         *plain_len,
                    GError       **error)
{
  GByteArray *acc;
  guint8 chunk[GOODIX_TLS_DRAIN_CHUNK];
  int n, ssl_err;

  *plain = NULL;
  *plain_len = 0;

  if (in != NULL && in_len > 0)
    {
      if (BIO_write (tls->rbio, in, (int) in_len) <= 0)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "TLS: failed to buffer ciphertext");
          return FALSE;
        }
    }

  acc = g_byte_array_new ();
  for (;;)
    {
      n = SSL_read (tls->ssl, chunk, sizeof (chunk));
      if (n > 0)
        {
          g_byte_array_append (acc, chunk, (guint) n);
          continue;
        }

      ssl_err = SSL_get_error (tls->ssl, n);
      if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE)
        break; /* need more ciphertext — return what we have */

      if (ssl_err == SSL_ERROR_ZERO_RETURN)
        break; /* peer closed */

      g_byte_array_free (acc, TRUE);
      {
        g_autofree gchar *msg = goodix_tls_pop_error ();
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "TLS decrypt failed: %s", msg);
      }
      return FALSE;
    }

  *plain_len = acc->len;
  *plain = g_byte_array_free (acc, acc->len == 0);
  if (*plain_len == 0)
    *plain = NULL;
  return TRUE;
}

gboolean
goodix_tls_is_established (GoodixTls *tls)
{
  return tls != NULL && SSL_is_init_finished (tls->ssl);
}
