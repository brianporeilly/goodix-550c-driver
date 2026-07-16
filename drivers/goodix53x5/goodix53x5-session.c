/*
 * Goodix 53x5 driver for libfprint — Device session (open, GTLS, reinit)
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

#define FP_COMPONENT "goodix53x5"

#include "drivers_api.h"
#include "goodix53x5-private.h"
#include "goodix53x5-transport.h"
#include "goodix53x5-commands.h"
#include "goodix53x5-calibration.h"
#include "goodix53x5-image.h"
#include "goodix53x5-session.h"
#include "goodix53x5-firmware550c.h"

#include <string.h>
#include <openssl/rand.h>

#define GOODIX_OPEN_FDT_MAX_RETRIES 2

/* Open SSM — full device initialization */
typedef enum {
  GOODIX_OPEN_USB_RESET = 0,
  GOODIX_OPEN_CLAIM_INTERFACE,
  GOODIX_OPEN_PING,
  GOODIX_OPEN_READ_FW_VERSION,
  GOODIX_OPEN_RESET,
  GOODIX_OPEN_READ_CHIP_ID,
  GOODIX_OPEN_READ_OTP,
  GOODIX_OPEN_PARSE_OTP,
  /* --- 550c TLS-PSK handshake (GOODIX_VARIANT_TLS_PSK only). Jumped in from
   *     PARSE_OTP, rejoins the shared flow at UPLOAD_CONFIG. The 53x5 GTLS
   *     path never enters these states. --- */
  GOODIX_OPEN_TLS_INIT,
  GOODIX_OPEN_TLS_PUMP,
  GOODIX_OPEN_TLS_AFTER_SEND,
  GOODIX_OPEN_TLS_RECV,
  GOODIX_OPEN_TLS_RECV_DONE,
  GOODIX_OPEN_TLS_ESTABLISHED,
  GOODIX_OPEN_TLS_FINISH,
  /* --- 53x5 GTLS path --- */
  GOODIX_OPEN_READ_PSK_HASH,
  GOODIX_OPEN_WRITE_PSK,
  GOODIX_OPEN_VERIFY_PSK_WRITE,
  GOODIX_OPEN_GTLS_CLIENT_HELLO,
  GOODIX_OPEN_GTLS_RECV_IDENTITY,
  GOODIX_OPEN_GTLS_SEND_VERIFY,
  GOODIX_OPEN_GTLS_RECV_DONE,
  GOODIX_OPEN_UPLOAD_CONFIG,
  GOODIX_OPEN_FDT_TX_ON,
  GOODIX_OPEN_VALIDATE_FDT,
  GOODIX_OPEN_FDT_TX_ON_2,
  GOODIX_OPEN_VALIDATE_FDT_2,
  GOODIX_OPEN_GENERATE_FDT_BASE,
  GOODIX_OPEN_SLEEP,
  GOODIX_OPEN_NUM_STATES,
} GoodixOpenState;


/*
 * All-zero PSK. The 550c is provisioned to this value (see the reverse-
 * engineering repo's provisioning tooling), matching the rest of the Goodix
 * 51x0/52xd/55x4 family. It is the default, so a provisioned unit is fully
 * plug-and-play with no configuration.
 */
static const guint8 goodix_psk[GOODIX_PSK_LEN] = { 0 };

/*
 * A unit still holding a Windows-provisioned (DPAPI-wrapped, per-provision
 * random) PSK can override the all-zero default with its recovered value via the
 * GOODIX550C_PSK environment variable (64 hex chars) or the file
 * /etc/goodix550c.psk. See the driver README for how to recover and, preferably,
 * how to reprovision the sensor to the all-zero PSK instead.
 */
#define GOODIX_550C_PSK_FILE "/etc/goodix550c.psk"

static gboolean
goodix_550c_load_psk (guint8   out[GOODIX_PSK_LEN],
                      GError **error)
{
  g_autofree gchar *hex = NULL;
  const gchar *env = g_getenv ("GOODIX550C_PSK");

  if (env != NULL && *env != '\0')
    {
      hex = g_strdup (env);
    }
  else if (!g_file_get_contents (GOODIX_550C_PSK_FILE, &hex, NULL, NULL))
    {
      /* No override configured: use the provisioned all-zero PSK. */
      memcpy (out, goodix_psk, GOODIX_PSK_LEN);
      return TRUE;
    }

  g_strstrip (hex);
  if (strlen (hex) != GOODIX_PSK_LEN * 2)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED,
                   "550c PSK must be %d hex chars, got %zu",
                   GOODIX_PSK_LEN * 2, strlen (hex));
      return FALSE;
    }

  for (int i = 0; i < GOODIX_PSK_LEN; i++)
    {
      gint hi = g_ascii_xdigit_value (hex[i * 2]);
      gint lo = g_ascii_xdigit_value (hex[i * 2 + 1]);

      if (hi < 0 || lo < 0)
        {
          g_set_error_literal (error, FP_DEVICE_ERROR,
                               FP_DEVICE_ERROR_NOT_SUPPORTED,
                               "550c PSK contains non-hex characters");
          return FALSE;
        }
      out[i] = (guint8) ((hi << 4) | lo);
    }

  return TRUE;
}

/*
 * TRUE if the operator has explicitly pinned this unit to a specific PSK
 * (env var or config file). Such a unit is deliberately on a per-machine PSK,
 * so self-heal must NOT reprovision it to all-zero — it would destroy the very
 * PSK the operator recovered. Only units left on the default (all-zero) path
 * are eligible for self-heal.
 */
static gboolean
goodix_550c_has_psk_override (void)
{
  const gchar *env = g_getenv ("GOODIX550C_PSK");

  if (env != NULL && *env != '\0')
    return TRUE;

  return g_file_test (GOODIX_550C_PSK_FILE, G_FILE_TEST_EXISTS);
}

/* PSK white box for writing all-zero PSK */
static const guint8 goodix_psk_white_box[GOODIX_PSK_WHITE_BOX_LEN] = {
  0xec, 0x35, 0xae, 0x3a, 0xbb, 0x45, 0xed, 0x3f,
  0x12, 0xc4, 0x75, 0x1f, 0x1e, 0x5c, 0x2c, 0xc0,
  0x5b, 0x3c, 0x54, 0x52, 0xe9, 0x10, 0x4d, 0x9f,
  0x2a, 0x31, 0x18, 0x64, 0x4f, 0x37, 0xa0, 0x4b,
  0x6f, 0xd6, 0x6b, 0x1d, 0x97, 0xcf, 0x80, 0xf1,
  0x34, 0x5f, 0x76, 0xc8, 0x4f, 0x03, 0xff, 0x30,
  0xbb, 0x51, 0xbf, 0x30, 0x8f, 0x2a, 0x98, 0x75,
  0xc4, 0x1e, 0x65, 0x92, 0xcd, 0x2a, 0x2f, 0x9e,
  0x60, 0x80, 0x9b, 0x17, 0xb5, 0x31, 0x60, 0x37,
  0xb6, 0x9b, 0xb2, 0xfa, 0x5d, 0x4c, 0x8a, 0xc3,
  0x1e, 0xdb, 0x33, 0x94, 0x04, 0x6e, 0xc0, 0x6b,
  0xbd, 0xac, 0xc5, 0x7d, 0xa6, 0xa7, 0x56, 0xc5,
};

/* ========================================================================
 * Open SSM — full device initialization
 * ======================================================================== */

static void
goodix_open_ssm_handler (FpiSsm   *ssm,
                         FpDevice *dev)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);
  GoodixOpenState state = fpi_ssm_get_cur_state (ssm);

  switch (state)
    {
    case GOODIX_OPEN_USB_RESET:
      {
        GError *error = NULL;

        if (!g_usb_device_reset (fpi_device_get_usb_device (dev), &error))
          {
            fpi_ssm_mark_failed (ssm, error);
            return;
          }
      }

      fpi_ssm_next_state (ssm);
      break;

    case GOODIX_OPEN_CLAIM_INTERFACE:
      {
        GError *error = NULL;

        if (!g_usb_device_claim_interface (
                fpi_device_get_usb_device (dev), self->usb_interface,
                G_USB_DEVICE_CLAIM_INTERFACE_BIND_KERNEL_DRIVER, &error))
          {
            fpi_ssm_mark_failed (ssm, error);
            return;
          }

        self->usb_interface_claimed = TRUE;
        fpi_ssm_next_state (ssm);
      }
      break;

    case GOODIX_OPEN_PING:
      goodix_cmd_ping (ssm, dev);
      break;

    case GOODIX_OPEN_READ_FW_VERSION:
      goodix_cmd_read_fw_version (ssm, dev);
      break;

    case GOODIX_OPEN_RESET:
      {
        /* Parse the firmware version reply before sending the reset */
        g_autoptr(GError) error = NULL;
        const guint8 *pl;
        gsize pl_len;

        if (!goodix_cmd_parse_fw_version_reply (dev, &pl, &pl_len, &error))
          {
            fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
            return;
          }

        g_clear_pointer (&self->fw_version, g_free);
        self->fw_version = g_strndup ((const gchar *) pl,
                                      strnlen ((const gchar *) pl, pl_len));

        goodix_cmd_reset_sensor (ssm, dev);
      }
      break;

    case GOODIX_OPEN_READ_CHIP_ID:
      goodix_cmd_read_chip_id (ssm, dev);
      break;

    case GOODIX_OPEN_READ_OTP:
      {
        /* Parse the chip ID reply before reading the OTP */
        g_autoptr(GError) error = NULL;
        const guint8 *pl;
        gsize pl_len;
        guint32 chip_id;

        if (!goodix_cmd_parse_chip_id_reply (dev, &pl, &pl_len, &error))
          {
            fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
            return;
          }

        if (pl_len != 4)
          {
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "Unexpected chip ID reply length: %zu",
                                                           pl_len));
            return;
          }

        chip_id = goodix_crypto_decode_u32 (pl);
        fp_dbg ("Chip ID: 0x%08x", chip_id);

        goodix_cmd_read_otp (ssm, dev);
      }
      break;

    case GOODIX_OPEN_PARSE_OTP:
      {
        const guint8 *pl;
        gsize pl_len;

        if (!goodix_cmd_parse_otp_reply (dev, &pl, &pl_len, NULL))
          {
            fpi_ssm_mark_failed (ssm,
                                  fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                            "Failed to parse OTP response"));
            return;
          }

        g_clear_pointer (&self->otp_data, g_free);
        self->otp_data = g_memdup2 (pl, pl_len);
        self->otp_len = pl_len;

        if (!goodix_device_verify_otp (pl, pl_len))
          {
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "OTP hash verification failed"));
            return;
          }

        goodix_device_parse_otp (pl, pl_len, &self->calib);

        /* 550c uses standard TLS-PSK instead of the 53x5 PSK-hash/GTLS path. */
        if (self->variant == GOODIX_VARIANT_TLS_PSK)
          fpi_ssm_jump_to_state (ssm, GOODIX_OPEN_TLS_INIT);
        else
          fpi_ssm_jump_to_state (ssm, GOODIX_OPEN_READ_PSK_HASH);
      }
      break;

    case GOODIX_OPEN_TLS_INIT:
      {
        guint8 psk[GOODIX_PSK_LEN];
        g_autoptr(GError) psk_error = NULL;

        /* Mark that the open reached the handshake: a failure from here (with
         * no PSK override) is what arms self-heal reprovisioning. */
        self->open_reached_tls = TRUE;

        if (!goodix_550c_load_psk (psk, &psk_error))
          {
            fpi_ssm_mark_failed (ssm, g_steal_pointer (&psk_error));
            return;
          }

        g_clear_pointer (&self->tls, goodix_tls_free);
        self->tls = goodix_tls_new (psk, GOODIX_PSK_LEN);
        if (self->tls == NULL)
          {
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                           "Failed to create TLS-PSK engine"));
            return;
          }
        self->tls_handshake_done = FALSE;

        /* request_tls_connection (cat 0xD / cmd 0): the sensor replies with a
         * 0xB0 pack carrying its TLS ClientHello. */
        goodix_run_cmd (ssm, dev, 0xD, 0x0,
                        (const guint8 *) "\x00\x00", 2, TRUE);
      }
      break;

    case GOODIX_OPEN_TLS_PUMP:
      {
        guint8 flag;
        const guint8 *pl;
        gsize pl_len;
        guint8 *out = NULL;
        gsize out_len = 0;
        gboolean done = FALSE;
        g_autoptr(GError) error = NULL;

        /* The device's handshake bytes are the payload of the last 0xB0 pack. */
        if (!goodix_proto_rx_pack (&self->rx, &flag, &pl, &pl_len) ||
            flag != GOODIX_PACK_FLAG_TLS)
          {
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "Expected a TLS (0xB0) pack during handshake"));
            return;
          }

        if (!goodix_tls_handshake_pump (self->tls, pl, pl_len,
                                        &out, &out_len, &done, &error))
          {
            fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
            return;
          }

        self->tls_handshake_done = done;

        if (out_len > 0)
          {
            /* Send our handshake bytes; the SSM advances to TLS_AFTER_SEND. */
            goodix_send_tls_pack (ssm, dev, out, out_len);
            g_free (out);
          }
        else
          {
            g_free (out);
            fpi_ssm_jump_to_state (ssm, done ? GOODIX_OPEN_TLS_ESTABLISHED
                                             : GOODIX_OPEN_TLS_RECV);
          }
      }
      break;

    case GOODIX_OPEN_TLS_AFTER_SEND:
      fpi_ssm_jump_to_state (ssm, self->tls_handshake_done
                                    ? GOODIX_OPEN_TLS_ESTABLISHED
                                    : GOODIX_OPEN_TLS_RECV);
      break;

    case GOODIX_OPEN_TLS_RECV:
      /* Receive the next 0xB0 handshake pack from the device. */
      goodix_recv_start (ssm, dev, GOODIX_DATA_TIMEOUT, NULL);
      break;

    case GOODIX_OPEN_TLS_RECV_DONE:
      fpi_ssm_jump_to_state (ssm, GOODIX_OPEN_TLS_PUMP);
      break;

    case GOODIX_OPEN_TLS_ESTABLISHED:
      /* tls_successfully_established (cat 0xD / cmd 2), no data reply. */
      fp_info ("TLS-PSK handshake completed");
      goodix_run_cmd (ssm, dev, 0xD, 0x2,
                      (const guint8 *) "\x00\x00", 2, FALSE);
      break;

    case GOODIX_OPEN_TLS_FINISH:
      fpi_ssm_jump_to_state (ssm, GOODIX_OPEN_UPLOAD_CONFIG);
      break;

    case GOODIX_OPEN_READ_PSK_HASH:
      /* read_psk_hash via production_read(0xB003) */
      goodix_cmd_production_read (ssm, dev, 0xB003);
      break;

    case GOODIX_OPEN_WRITE_PSK:
      {
        /* Check if PSK hash matches our all-zero PSK.
         * Parse the production_read response. */
        const guint8 *psk_data;
        gsize psk_data_len;

        if (!goodix_cmd_parse_production_read_reply (dev, 0xB003,
                                                     &psk_data, &psk_data_len))
          {
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "Failed to read PSK hash"));
            return;
          }

        /* Compute SHA256 of our PSK and compare */
        {
          g_autoptr(GChecksum) sha = g_checksum_new (G_CHECKSUM_SHA256);
          guint8 expected_hash[32];
          gsize hash_len = 32;

          g_checksum_update (sha, goodix_psk, GOODIX_PSK_LEN);
          g_checksum_get_digest (sha, expected_hash, &hash_len);

          if (psk_data_len >= 32 && memcmp (psk_data, expected_hash, 32) == 0)
            {
              fp_dbg ("PSK hash matches, no need to write");
              self->psk_write_verify_pending = FALSE;
              fpi_ssm_jump_to_state (ssm, GOODIX_OPEN_GTLS_CLIENT_HELLO);
              return;
            }
        }

        /* Need to write PSK white box */
        fp_info ("Writing PSK white box");
        self->psk_write_verify_pending = TRUE;
        goodix_cmd_production_write (ssm, dev, 0xB002, goodix_psk_white_box,
                                     GOODIX_PSK_WHITE_BOX_LEN);
      }
      break;

    case GOODIX_OPEN_VERIFY_PSK_WRITE:
      {
        const guint8 *pl;
        gsize pl_len;

        if (!goodix_cmd_parse_production_write_reply (dev, &pl, &pl_len) ||
            pl_len < 1)
          {
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "Failed to parse PSK write reply"));
            return;
          }

        if (pl[0] != 0)
          {
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "PSK write failed: %u",
                                                           pl[0]));
            return;
          }

        /* Re-read the PSK hash to verify the write before GTLS */
        goodix_cmd_production_read (ssm, dev, 0xB003);
      }
      break;

    case GOODIX_OPEN_GTLS_CLIENT_HELLO:
      {
        if (self->psk_write_verify_pending)
          {
            /* Parse the re-read PSK hash from the previous state */
            const guint8 *psk_data;
            gsize psk_data_len;
            g_autoptr(GChecksum) sha = g_checksum_new (G_CHECKSUM_SHA256);
            guint8 expected_hash[32];
            gsize hash_len = 32;

            if (!goodix_cmd_parse_production_read_reply (dev, 0xB003,
                                                         &psk_data,
                                                         &psk_data_len))
              {
                fpi_ssm_mark_failed (ssm,
                                     fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                               "Failed to re-read PSK hash"));
                return;
              }

            g_checksum_update (sha, goodix_psk, GOODIX_PSK_LEN);
            g_checksum_get_digest (sha, expected_hash, &hash_len);

            if (psk_data_len < 32 || memcmp (psk_data, expected_hash, 32) != 0)
              {
                fpi_ssm_mark_failed (ssm,
                                     fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                               "PSK hash mismatch after write"));
                return;
              }

            self->psk_write_verify_pending = FALSE;
          }

        /* Generate client_random and send via MCU */
        RAND_bytes (self->gtls.client_random, 32);
        goodix_crypto_gtls_init (&self->gtls, goodix_psk);
        RAND_bytes (self->gtls.client_random, 32);
        self->gtls.state = 2;

        goodix_cmd_mcu_send (ssm, dev, 0xFF01, self->gtls.client_random, 32);
      }
      break;

    case GOODIX_OPEN_GTLS_RECV_IDENTITY:
      /* Receive MCU message with server random + identity */
      goodix_recv_start (ssm, dev, GOODIX_DATA_TIMEOUT, NULL);
      break;

    case GOODIX_OPEN_GTLS_SEND_VERIFY:
      {
        /* Parse server identity response */
        const guint8 *mcu_data;
        gsize mcu_len;

        if (!goodix_cmd_parse_mcu_reply (dev, 0xFF02, &mcu_data, &mcu_len))
          {
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "Failed to parse GTLS server identity"));
            return;
          }

        if (mcu_len != 0x40)
          {
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "Wrong GTLS identity payload size: %zu",
                                                           mcu_len));
            return;
          }

        memcpy (self->gtls.server_random, mcu_data, 32);
        memcpy (self->gtls.server_identity, mcu_data + 32, 32);

        /* Derive session keys */
        goodix_crypto_gtls_derive_keys (&self->gtls);

        /* Verify identity */
        if (!goodix_crypto_gtls_verify_identity (&self->gtls))
          {
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "GTLS identity verification failed"));
            return;
          }

        /* Send client identity + \xee\xee\xee\xee via MCU */
        {
          guint8 verify_data[36];
          memcpy (verify_data, self->gtls.client_identity, 32);
          memset (verify_data + 32, 0xEE, 4);

          goodix_cmd_mcu_send (ssm, dev, 0xFF03, verify_data, 36);
        }

        self->gtls.state = 4;
      }
      break;

    case GOODIX_OPEN_GTLS_RECV_DONE:
      /* Receive MCU done message */
      goodix_recv_start (ssm, dev, GOODIX_DATA_TIMEOUT, NULL);
      break;

    case GOODIX_OPEN_UPLOAD_CONFIG:
      {
        /* The 53x5 GTLS path validates a "GTLS done" MCU reply here; the 550c
         * arrives via the standard TLS-PSK handshake and skips it. */
        if (self->variant != GOODIX_VARIANT_TLS_PSK)
          {
            const guint8 *mcu_data;
            gsize mcu_len;

            if (!goodix_cmd_parse_mcu_reply (dev, 0xFF04, &mcu_data, &mcu_len))
              {
                fpi_ssm_mark_failed (ssm,
                                     fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                               "Failed to parse GTLS done"));
                return;
              }

            if (mcu_len >= 4)
              {
                guint32 result = mcu_data[0] | ((guint32) mcu_data[1] << 8) |
                                 ((guint32) mcu_data[2] << 16) |
                                 ((guint32) mcu_data[3] << 24);
                if (result != 0)
                  {
                    fpi_ssm_mark_failed (ssm,
                                         fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                                   "GTLS handshake failed: %u",
                                                                   result));
                    return;
                  }
              }

            self->gtls.hmac_client_counter = self->gtls.hmac_client_counter_init;
            self->gtls.hmac_server_counter = self->gtls.hmac_server_counter_init;
            self->gtls.state = 5;
          }

        self->open_fdt_retries = 0;

        /* Build and upload config */
        gsize cfg_len;
        const guint8 *def_cfg = goodix_device_get_default_config (&cfg_len);
        guint8 *cfg = g_memdup2 (def_cfg, cfg_len);

        goodix_device_patch_config (cfg, cfg_len, &self->calib);
        goodix_cmd_upload_config (ssm, dev, cfg, cfg_len);
        g_free (cfg);
      }
      break;

    case GOODIX_OPEN_FDT_TX_ON:
      {
        /* Validate the config upload reply before the first FDT TX-on */
        if (!goodix_cmd_parse_config_reply (dev))
          {
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "Config upload failed"));
            return;
          }

        /* FDT manual operation with TX enabled. Both variants derive the FDT
         * baseline from live no-finger readings here (the 550c no longer uses
         * fixed Windows-captured bases). */
        goodix_cmd_fdt_manual (ssm, dev, TRUE, self->calib.fdt_base_manual);
      }
      break;

    case GOODIX_OPEN_VALIDATE_FDT:
      {
        /* Parse FDT response and save */
        g_autoptr(GError) error = NULL;
        const guint8 *pl;
        gsize pl_len;

        if (!goodix_cmd_parse_fdt_manual_reply (dev, &pl, &pl_len, &error))
          {
            fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
            return;
          }

        /* FDT data is the payload after 4 bytes of irq+touch_flag */
        g_free (self->fdt_data_tx_on);
        self->fdt_data_tx_on = g_memdup2 (pl + 4, GOODIX_FDT_BASE_LEN);

        fpi_ssm_next_state (ssm);
      }
      break;

    case GOODIX_OPEN_FDT_TX_ON_2:
      /* Second FDT TX on */
      goodix_cmd_fdt_manual (ssm, dev, TRUE, self->calib.fdt_base_manual);
      break;

    case GOODIX_OPEN_VALIDATE_FDT_2:
      {
        /* Parse and validate second FDT */
        g_autoptr(GError) error = NULL;
        const guint8 *pl;
        gsize pl_len;

        if (!goodix_cmd_parse_fdt_manual_reply (dev, &pl, &pl_len, &error))
          {
            fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
            return;
          }

        /* Validate against the open-time FDT base we just recorded. */
        if (!goodix_device_is_fdt_base_valid (pl + 4,
                                              self->fdt_data_tx_on,
                                              GOODIX_FDT_BASE_LEN,
                                              self->calib.delta_fdt))
          {
            if (self->open_fdt_retries++ < GOODIX_OPEN_FDT_MAX_RETRIES)
              {
                fp_warn ("Open FDT validation unstable, retrying (%u/%u)",
                         self->open_fdt_retries,
                         GOODIX_OPEN_FDT_MAX_RETRIES);
                g_free (self->fdt_data_tx_on);
                self->fdt_data_tx_on = g_memdup2 (pl + 4,
                                                  GOODIX_FDT_BASE_LEN);
                fpi_ssm_jump_to_state (ssm, GOODIX_OPEN_FDT_TX_ON_2);
                return;
              }

            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                           "Open FDT baseline did not stabilize"));
            return;
          }

        fpi_ssm_next_state (ssm);
      }
      break;

    case GOODIX_OPEN_GENERATE_FDT_BASE:
      {
        /* Generate FDT base from TX-on data */
        if (self->fdt_data_tx_on)
          {
            guint8 fdt_base[GOODIX_FDT_BASE_LEN];
            goodix_device_generate_fdt_base (self->fdt_data_tx_on,
                                             GOODIX_FDT_BASE_LEN, fdt_base);
            memcpy (self->calib.fdt_base_down, fdt_base, GOODIX_FDT_BASE_LEN);
            memcpy (self->calib.fdt_base_up, fdt_base, GOODIX_FDT_BASE_LEN);
            memcpy (self->calib.fdt_base_manual, fdt_base,
                    GOODIX_FDT_BASE_LEN);
          }

        fpi_ssm_next_state (ssm);
      }
      break;

    case GOODIX_OPEN_SLEEP:
      goodix_cmd_set_sleep_mode (ssm, dev);
      break;

    case GOODIX_OPEN_NUM_STATES:
      g_assert_not_reached ();
      break;
    }
}

/* ========================================================================
 * Self-heal PSK provisioning (550c only)
 *
 * When the all-zero TLS-PSK handshake fails on a 550c that has no explicit PSK
 * override, the unit is almost certainly still on a Windows-provisioned
 * per-machine PSK. Rather than fail the open, we replay the reverse-engineered
 * provisioning sequence to move it to the family-standard all-zero PSK, then
 * retry the open from scratch. This mirrors what Windows does (check, then
 * provision) and matches captures/provision_container_allzero.py.
 *
 * Sequence, in IAP mode (entered via mcu_erase_app):
 *   erase_app -> IAP
 *   preset_psk_write(container)   full Windows-style container: 56a5bb95 prefix
 *                                 + slot 0xbb010002 (device-ignored filler)
 *                                 + slot 0xbb010003 (all-zero family white-box)
 *   write_firmware(app, number=2) 256-byte chunks
 *   check_firmware(all-zero HMAC) commits + validates the reflash
 *   reset -> app; re-run open (handshake now succeeds on all-zero)
 *
 * A bare single-slot white-box write is accepted but does NOT commit the PSK on
 * the 550c — only the two-slot container write does. See the RE notes.
 *
 * HARDWARE-RISK: this writes firmware over USB. A mis-framed write can wedge the
 * USB stack until a cold boot (the device returns in IAP, so a re-run is
 * idempotent). The framing here is a faithful port of the proven Python; the
 * USB re-enumeration handling (erase/reset -> reclaim) is the part that still
 * needs on-hardware validation.
 * ======================================================================== */

static void goodix_cleanup_failed_open (FpDevice *dev);
static void goodix_launch_open_ssm (FpDevice *dev);

/* Container geometry (bytes). */
#define GOODIX_550C_CONTAINER_PREFIX_LEN 10
#define GOODIX_550C_PSK_FILLER_LEN       324   /* slot 0xbb010002 payload */
#define GOODIX_550C_CONTAINER_LEN        446   /* prefix + both TLV slots */
#define GOODIX_550C_FW_CHUNK             256
#define GOODIX_550C_FW_NUMBER            2
#define GOODIX_550C_REENUM_SETTLE_MS     1500  /* wait for USB re-enumeration */

/*
 * Slot 0xbb010002 filler. The firmware ignores this slot (it cannot unseal
 * Windows' DPAPI blob) and does NOT cross-check it against slot 0xbb010003, so
 * the driver ships neutral, non-secret padding here rather than any captured
 * per-machine blob.
 *
 * NOTE (pending on-hardware confirmation): all-zero filler is the hypothesis to
 * validate first — a live container write on an already-all-zero unit whose
 * check_firmware passes proves it. If the firmware turns out to require a
 * well-formed blob, replace this with a captured generic blob (documented as
 * non-secret padding). Nothing else in this file changes.
 */
static const guint8 goodix_550c_psk_filler[GOODIX_550C_PSK_FILLER_LEN] = { 0 };

/* The all-zero-PSK check_firmware HMAC over goodix_550c_app_firmware. Derived
 * at runtime from the target PSK (goodix_550c_fw_hmac); this constant is only a
 * self-check that the derivation and the embedded firmware are intact before we
 * touch the device. Verified against the genesis captures. */
static const guint8 goodix_550c_allzero_fw_hmac[32] = {
  0x02, 0x5b, 0x4d, 0x2d, 0x83, 0xa8, 0x98, 0xab,
  0x3f, 0x9d, 0xa1, 0xa0, 0xff, 0x0b, 0x2d, 0x72,
  0xc3, 0x1d, 0x38, 0x5c, 0x15, 0x66, 0x58, 0x4d,
  0x40, 0x3f, 0x23, 0xd2, 0xe2, 0xa6, 0xc2, 0x06,
};

/*
 * Derive the 550c check_firmware HMAC for @psk over @fw:
 *   pmk      = SHA256( len16be(psk) | zeros(len(psk)) | len16be(psk) | psk )
 *   pmk_hmac = HMAC-SHA256( pmk, bytes 1..64 )
 *   fw_hmac  = HMAC-SHA256( pmk_hmac, fw )
 * (This is the 550c formula; it diverges from the sibling family's
 * pmk=SHA256((len16be|psk)*2), though the two coincide for an all-zero PSK.)
 */
static void
goodix_550c_fw_hmac (const guint8 *psk, gsize psk_len,
                     const guint8 *fw, gsize fw_len,
                     guint8 out[32])
{
  gsize pre_len = 2 + psk_len + 2 + psk_len;
  g_autofree guint8 *pre = g_malloc0 (pre_len);
  guint8 pmk[32];
  gsize pmk_len = sizeof (pmk);
  guint8 mod[64];
  guint8 pmk_hmac[32];
  gsize hlen;
  g_autoptr(GChecksum) sha = g_checksum_new (G_CHECKSUM_SHA256);
  GHmac *h;

  pre[0] = (psk_len >> 8) & 0xff;
  pre[1] = psk_len & 0xff;
  /* middle zeros(len) already zeroed by g_malloc0 */
  pre[2 + psk_len] = (psk_len >> 8) & 0xff;
  pre[2 + psk_len + 1] = psk_len & 0xff;
  memcpy (pre + 2 + psk_len + 2, psk, psk_len);

  g_checksum_update (sha, pre, pre_len);
  g_checksum_get_digest (sha, pmk, &pmk_len);

  for (int i = 0; i < 64; i++)
    mod[i] = (guint8) (i + 1);

  hlen = sizeof (pmk_hmac);
  h = g_hmac_new (G_CHECKSUM_SHA256, pmk, sizeof (pmk));
  g_hmac_update (h, mod, sizeof (mod));
  g_hmac_get_digest (h, pmk_hmac, &hlen);
  g_hmac_unref (h);

  hlen = 32;
  h = g_hmac_new (G_CHECKSUM_SHA256, pmk_hmac, sizeof (pmk_hmac));
  g_hmac_update (h, fw, fw_len);
  g_hmac_get_digest (h, out, &hlen);
  g_hmac_unref (h);
}

static void
goodix_heal_put_u32_le (guint8 *out, guint32 v)
{
  out[0] = v & 0xff;
  out[1] = (v >> 8) & 0xff;
  out[2] = (v >> 16) & 0xff;
  out[3] = (v >> 24) & 0xff;
}

/* Assemble the 446-byte PSK container into a freshly allocated buffer. */
static guint8 *
goodix_550c_build_container (void)
{
  static const guint8 prefix[GOODIX_550C_CONTAINER_PREFIX_LEN] = {
    0x56, 0xa5, 0xbb, 0x95, 0x6b, 0x7c, 0x8d, 0x9e, 0x00, 0x00,
  };
  guint8 *buf = g_malloc (GOODIX_550C_CONTAINER_LEN);
  gsize o = 0;

  memcpy (buf + o, prefix, sizeof (prefix));
  o += sizeof (prefix);

  /* slot 0xbb010002 | len 324 | filler */
  goodix_heal_put_u32_le (buf + o, 0xbb010002); o += 4;
  goodix_heal_put_u32_le (buf + o, GOODIX_550C_PSK_FILLER_LEN); o += 4;
  memcpy (buf + o, goodix_550c_psk_filler, GOODIX_550C_PSK_FILLER_LEN);
  o += GOODIX_550C_PSK_FILLER_LEN;

  /* slot 0xbb010003 | len 96 | all-zero family white-box */
  goodix_heal_put_u32_le (buf + o, 0xbb010003); o += 4;
  goodix_heal_put_u32_le (buf + o, GOODIX_PSK_WHITE_BOX_LEN); o += 4;
  memcpy (buf + o, goodix_psk_white_box, GOODIX_PSK_WHITE_BOX_LEN);
  o += GOODIX_PSK_WHITE_BOX_LEN;

  g_assert (o == GOODIX_550C_CONTAINER_LEN);
  return buf;
}

/*
 * Drop a stale interface claim (best effort), reset the port, and re-claim.
 * Needed after the device re-enumerates on erase_app / reset. Synchronous, like
 * the open SSM's USB-reset state.
 */
static gboolean
goodix_heal_reset_and_claim (FpDevice *dev, GError **error)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);
  GUsbDevice *usb = fpi_device_get_usb_device (dev);

  if (self->usb_interface_claimed)
    {
      g_autoptr(GError) rel_err = NULL;
      /* Expected to fail after re-enumeration; recovery proceeds regardless. */
      if (!g_usb_device_release_interface (usb, self->usb_interface, 0,
                                           &rel_err))
        fp_dbg ("Releasing stale interface before heal reset: %s",
                rel_err->message);
      self->usb_interface_claimed = FALSE;
    }

  if (!g_usb_device_reset (usb, error))
    return FALSE;

  if (!g_usb_device_claim_interface (
          usb, self->usb_interface,
          G_USB_DEVICE_CLAIM_INTERFACE_BIND_KERNEL_DRIVER, error))
    return FALSE;

  self->usb_interface_claimed = TRUE;
  return TRUE;
}

typedef enum {
  GOODIX_HEAL_READ_PSK_HASH = 0,
  GOODIX_HEAL_DECIDE_PSK,
  GOODIX_HEAL_READ_FW_VERSION,
  GOODIX_HEAL_DECIDE_MODE,
  GOODIX_HEAL_ERASE_APP,
  GOODIX_HEAL_ERASE_SETTLE,
  GOODIX_HEAL_ERASE_RESET,
  GOODIX_HEAL_CONTAINER_WRITE1,
  GOODIX_HEAL_CONTAINER_WRITE2,
  GOODIX_HEAL_WRITE_FW_BEGIN,
  GOODIX_HEAL_WRITE_FW_SEND,
  GOODIX_HEAL_WRITE_FW_NEXT,
  GOODIX_HEAL_CHECK_FW,
  GOODIX_HEAL_RESET_MCU,
  GOODIX_HEAL_RESET_SETTLE,
  GOODIX_HEAL_NUM_STATES,
} GoodixHealState;

static void
goodix_heal_ssm_handler (FpiSsm   *ssm,
                         FpDevice *dev)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);
  GoodixHealState state = fpi_ssm_get_cur_state (ssm);

  switch (state)
    {
    case GOODIX_HEAL_READ_PSK_HASH:
      /* Before touching anything, read the SHA256(PSK) hash slot (0xbb020001).
       * This is the same read check_psk.py uses and it works in a freshly-booted
       * app. It lets us tell a genuinely wrong PSK (reflash needed) from a device
       * that is already on the all-zero PSK but failed the handshake transiently
       * (no reflash needed — just retry). Best-effort: a read-gated reply falls
       * through to the reflash path. */
      goodix_cmd_preset_psk_read (ssm, dev, 0xbb020001, 32, 0);
      break;

    case GOODIX_HEAL_DECIDE_PSK:
      {
        g_autoptr(GError) error = NULL;
        const guint8 *hash;
        gsize hash_len;
        guint8 expected[32];
        gsize expected_len = sizeof (expected);
        g_autoptr(GChecksum) sha = g_checksum_new (G_CHECKSUM_SHA256);

        if (!goodix_cmd_parse_preset_psk_read_reply (dev, &hash, &hash_len,
                                                     &error))
          {
            fp_warn ("Self-heal: could not read the PSK hash slot (%s); "
                     "proceeding with a full reprovision", error->message);
            fpi_ssm_jump_to_state (ssm, GOODIX_HEAL_READ_FW_VERSION);
            return;
          }

        g_checksum_update (sha, goodix_psk, GOODIX_PSK_LEN);
        g_checksum_get_digest (sha, expected, &expected_len);

        fp_warn ("Self-heal: live PSK hash slot = %02x%02x%02x%02x… (%zu bytes)",
                 hash_len > 0 ? hash[0] : 0, hash_len > 1 ? hash[1] : 0,
                 hash_len > 2 ? hash[2] : 0, hash_len > 3 ? hash[3] : 0,
                 hash_len);

        if (hash_len >= 32 && memcmp (hash, expected, 32) == 0)
          {
            fp_warn ("Self-heal: sensor is ALREADY on the all-zero PSK — the "
                     "handshake failure was transient, NOT a wrong PSK; "
                     "skipping the reflash and retrying the open");
            self->heal_already_provisioned = TRUE;
            fpi_ssm_mark_completed (ssm);
            return;
          }

        fp_warn ("Self-heal: sensor is on a non-default PSK; reprovisioning it "
                 "to the all-zero PSK");
        fpi_ssm_jump_to_state (ssm, GOODIX_HEAL_READ_FW_VERSION);
      }
      break;

    case GOODIX_HEAL_READ_FW_VERSION:
      goodix_cmd_read_fw_version (ssm, dev);
      break;

    case GOODIX_HEAL_DECIDE_MODE:
      {
        g_autoptr(GError) error = NULL;
        const guint8 *pl;
        gsize pl_len;
        g_autofree gchar *ver = NULL;

        if (!goodix_cmd_parse_fw_version_reply (dev, &pl, &pl_len, &error))
          {
            fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
            return;
          }

        ver = g_strndup ((const gchar *) pl,
                         strnlen ((const gchar *) pl, pl_len));
        fp_warn ("Self-heal: device firmware '%s'", ver);

        if (strstr (ver, "IAP") != NULL)
          {
            fpi_ssm_jump_to_state (ssm, GOODIX_HEAL_CONTAINER_WRITE1);
            return;
          }

        if (self->heal_erased)
          {
            fpi_ssm_mark_failed (ssm,
                fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                    "Self-heal: device did not enter IAP after erase"));
            return;
          }

        fpi_ssm_next_state (ssm);   /* -> ERASE_APP */
      }
      break;

    case GOODIX_HEAL_ERASE_APP:
      fp_warn ("Self-heal: erasing app to enter IAP");
      self->heal_erased = TRUE;
      goodix_cmd_mcu_erase_app (ssm, dev);
      break;

    case GOODIX_HEAL_ERASE_SETTLE:
      /* Let the device drop off the bus and re-enumerate into IAP. */
      fpi_ssm_next_state_delayed (ssm, GOODIX_550C_REENUM_SETTLE_MS);
      break;

    case GOODIX_HEAL_ERASE_RESET:
      {
        g_autoptr(GError) error = NULL;

        if (!goodix_heal_reset_and_claim (dev, &error))
          {
            fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
            return;
          }
        fpi_ssm_jump_to_state (ssm, GOODIX_HEAL_READ_FW_VERSION);
      }
      break;

    case GOODIX_HEAL_CONTAINER_WRITE1:
      fp_warn ("Self-heal: writing PSK container (chunk 1/2)");
      goodix_cmd_preset_psk_write_chunk (ssm, dev, GOODIX_550C_CONTAINER_LEN,
                                         GOODIX_550C_FW_CHUNK, 0,
                                         self->heal_container);
      break;

    case GOODIX_HEAL_CONTAINER_WRITE2:
      {
        g_autoptr(GError) error = NULL;

        if (!goodix_cmd_parse_preset_psk_write_reply (dev, &error))
          {
            fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
            return;
          }

        fp_warn ("Self-heal: writing PSK container (chunk 2/2)");
        goodix_cmd_preset_psk_write_chunk (
            ssm, dev, GOODIX_550C_CONTAINER_LEN,
            GOODIX_550C_CONTAINER_LEN - GOODIX_550C_FW_CHUNK,
            GOODIX_550C_FW_CHUNK,
            self->heal_container + GOODIX_550C_FW_CHUNK);
      }
      break;

    case GOODIX_HEAL_WRITE_FW_BEGIN:
      {
        g_autoptr(GError) error = NULL;

        if (!goodix_cmd_parse_preset_psk_write_reply (dev, &error))
          {
            fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
            return;
          }

        fp_warn ("Self-heal: reflashing app firmware (%d bytes)",
                 GOODIX_550C_APP_FIRMWARE_LEN);
        self->heal_fw_offset = 0;
        fpi_ssm_jump_to_state (ssm, GOODIX_HEAL_WRITE_FW_SEND);
      }
      break;

    case GOODIX_HEAL_WRITE_FW_SEND:
      {
        guint32 off = self->heal_fw_offset;
        gsize remaining;

        if (off >= GOODIX_550C_APP_FIRMWARE_LEN)
          {
            fpi_ssm_jump_to_state (ssm, GOODIX_HEAL_CHECK_FW);
            return;
          }

        remaining = GOODIX_550C_APP_FIRMWARE_LEN - off;
        goodix_cmd_write_firmware (
            ssm, dev, off, goodix_550c_app_firmware + off,
            MIN (GOODIX_550C_FW_CHUNK, remaining), GOODIX_550C_FW_NUMBER);
      }
      break;

    case GOODIX_HEAL_WRITE_FW_NEXT:
      {
        g_autoptr(GError) error = NULL;

        if (!goodix_cmd_parse_write_firmware_reply (dev, &error))
          {
            fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
            return;
          }

        self->heal_fw_offset += GOODIX_550C_FW_CHUNK;
        fpi_ssm_jump_to_state (ssm, GOODIX_HEAL_WRITE_FW_SEND);
      }
      break;

    case GOODIX_HEAL_CHECK_FW:
      fp_warn ("Self-heal: validating firmware (commits target PSK)");
      goodix_cmd_check_firmware (ssm, dev, self->heal_fw_hmac);
      break;

    case GOODIX_HEAL_RESET_MCU:
      {
        g_autoptr(GError) error = NULL;

        if (!goodix_cmd_parse_check_firmware_reply (dev, &error))
          {
            fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
            return;
          }

        fp_warn ("Self-heal: PSK committed; resetting into app");
        goodix_cmd_mcu_reset_soft (ssm, dev);
      }
      break;

    case GOODIX_HEAL_RESET_SETTLE:
      /* Let the device re-enumerate into the app, then finish. The fresh open
       * (started by the done handler) does its own USB reset + claim. */
      fpi_ssm_next_state_delayed (ssm, GOODIX_550C_REENUM_SETTLE_MS);
      break;

    case GOODIX_HEAL_NUM_STATES:
      g_assert_not_reached ();
      break;
    }
}

static void
goodix_heal_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);

  self->task_ssm = NULL;
  g_clear_pointer (&self->heal_container, g_free);

  if (error)
    {
      fp_warn ("Self-heal provisioning failed: %s", error->message);
      goodix_cleanup_failed_open (dev);
      fpi_device_open_complete (dev, error);
      return;
    }

  if (self->heal_already_provisioned)
    {
      /* The hash slot showed the sensor is already on the all-zero PSK, so no
       * reflash ran and the device did NOT re-enumerate — the handshake failure
       * was transient (the post-provision first-handshake quirk, or a wedged
       * prior attempt). Drop the stale interface claim and re-run the open in
       * place; the open SSM's leading USB reset clears the transient. Because
       * selfheal_attempted stays TRUE, a second failure fails the open cleanly
       * instead of triggering a needless reflash. */
      GUsbDevice *usb = fpi_device_get_usb_device (dev);

      self->heal_already_provisioned = FALSE;

      if (self->usb_interface_claimed)
        {
          g_autoptr(GError) rel_err = NULL;

          if (!g_usb_device_release_interface (usb, self->usb_interface, 0,
                                               &rel_err))
            fp_dbg ("Releasing interface before in-place reopen: %s",
                    rel_err->message);
          self->usb_interface_claimed = FALSE;
        }

      fp_warn ("Self-heal: retrying the open on the already-provisioned sensor "
               "(no reflash)");
      goodix_launch_open_ssm (dev);
      return;
    }

  /* Provisioning succeeded. The IAP->APP soft reset re-enumerates the sensor on
   * the USB bus with a new address (a USB-level reset alone does NOT jump IAP to
   * APP — only the reset command does, and it always re-enumerates), so this
   * FpDevice's USB handle is now stale and cannot be reused in place. Report the
   * device as removed and clean up; the re-enumerated sensor — now on the
   * all-zero PSK — reappears as a fresh device that opens normally. fprintd
   * picks up the hotplugged device automatically; a CLI caller just re-runs. */
  fp_warn ("Self-heal: sensor reprovisioned to the all-zero PSK and "
           "re-enumerated on the USB bus; re-open to use it");
  goodix_cleanup_failed_open (dev);
  fpi_device_open_complete (
      dev,
      fpi_device_error_new_msg (
          FP_DEVICE_ERROR_REMOVED,
          "Sensor was reprovisioned to the default PSK and re-enumerated; "
          "please retry"));
}

/*
 * Start the self-heal provisioning SSM. Ownership of the open completion passes
 * to this SSM: on success it re-runs the open, on failure it completes the open
 * with the error. Returns TRUE if started (caller must not touch the open).
 */
static gboolean
goodix_maybe_start_selfheal (FpDevice *dev)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);
  guint8 expected[32];
  FpiSsm *ssm;

  if (self->variant != GOODIX_VARIANT_TLS_PSK)
    return FALSE;
  /* Only when the handshake itself failed, never after it already succeeded. */
  if (!self->open_reached_tls || self->tls_handshake_done)
    return FALSE;
  if (self->selfheal_attempted)
    return FALSE;
  if (goodix_550c_has_psk_override ())
    {
      fp_info ("TLS-PSK handshake failed but a PSK override is configured; "
               "not reprovisioning (the unit is deliberately on a custom PSK)");
      return FALSE;
    }

  /* Derive the target (all-zero) check_firmware HMAC and self-check it against
   * the verified constant before we touch the device — a mismatch means the
   * embedded firmware or the derivation is wrong, so refuse to reflash. */
  goodix_550c_fw_hmac (goodix_psk, GOODIX_PSK_LEN,
                       goodix_550c_app_firmware, GOODIX_550C_APP_FIRMWARE_LEN,
                       expected);
  if (memcmp (expected, goodix_550c_allzero_fw_hmac, 32) != 0)
    {
      fp_warn ("Self-heal aborted: firmware HMAC self-check failed "
               "(embedded firmware or key derivation is corrupt)");
      return FALSE;
    }

  fp_warn ("550c TLS-PSK handshake failed and no PSK override is set — "
           "self-healing: checking the PSK hash slot, then reprovisioning to "
           "the all-zero PSK only if needed");

  self->selfheal_attempted = TRUE;
  self->heal_erased = FALSE;
  self->heal_already_provisioned = FALSE;
  self->heal_fw_offset = 0;
  memcpy (self->heal_fw_hmac, expected, 32);
  g_clear_pointer (&self->heal_container, g_free);
  self->heal_container = goodix_550c_build_container ();
  self->heal_container_len = GOODIX_550C_CONTAINER_LEN;

  ssm = fpi_ssm_new (dev, goodix_heal_ssm_handler, GOODIX_HEAL_NUM_STATES);
  self->task_ssm = ssm;
  fpi_ssm_start (ssm, goodix_heal_ssm_done);
  return TRUE;
}

static void
goodix_cleanup_failed_open (FpDevice *dev)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);
  GUsbDevice *usb_dev = fpi_device_get_usb_device (dev);
  g_autoptr(GError) cleanup_error = NULL;

  if (self->usb_interface_claimed)
    {
      if (!g_usb_device_release_interface (usb_dev, self->usb_interface, 0,
                                           &cleanup_error))
        fp_warn ("Failed to release USB interface after open failure: %s",
                 cleanup_error->message);

      self->usb_interface_claimed = FALSE;
      g_clear_error (&cleanup_error);
    }

  if (!g_usb_device_close (usb_dev, &cleanup_error))
    fp_warn ("Failed to close USB device after open failure: %s",
             cleanup_error->message);
}

static void
goodix_open_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);

  self->task_ssm = NULL;

  /* Clean up temp data */
  g_clear_pointer (&self->fdt_data_tx_on, g_free);

  if (error)
    {
      fp_warn ("Device open failed: %s", error->message);

      /* A 550c that failed the all-zero handshake with no PSK override is
       * likely still on a per-machine PSK. Try to reprovision it to all-zero
       * and re-open. When this takes over, it owns the open completion. */
      if (goodix_maybe_start_selfheal (dev))
        {
          g_error_free (error);
          return;
        }

      goodix_cleanup_failed_open (dev);
      fpi_device_open_complete (dev, error);
      return;
    }

  fp_info ("Device initialization complete");
  self->needs_reinit = FALSE;
  fpi_device_open_complete (dev, NULL);
}

static void
goodix_launch_open_ssm (FpDevice *dev)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);
  FpiSsm *ssm;

  self->variant = (GoodixVariant) fpi_device_get_driver_data (dev);
  self->open_reached_tls = FALSE;
  self->tls_handshake_done = FALSE;

  /* USB interface + bulk endpoints differ between the 53x5 and the 550c. */
  if (self->variant == GOODIX_VARIANT_TLS_PSK)
    {
      self->usb_interface = 0;
      self->ep_out = 0x01 | FPI_USB_ENDPOINT_OUT;   /* 0x01 */
      self->ep_in  = 0x03 | FPI_USB_ENDPOINT_IN;    /* 0x83 */
    }
  else
    {
      self->usb_interface = 1;
      self->ep_out = 0x03 | FPI_USB_ENDPOINT_OUT;   /* 0x03 */
      self->ep_in  = 0x01 | FPI_USB_ENDPOINT_IN;    /* 0x81 */
    }

  ssm = fpi_ssm_new (dev, goodix_open_ssm_handler,
                      GOODIX_OPEN_NUM_STATES);
  self->task_ssm = ssm;
  fpi_ssm_start (ssm, goodix_open_ssm_done);
}

void
goodix_start_open_ssm (FpDevice *dev)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);

  /* Fresh open: allow self-heal to run once if the handshake fails. */
  self->selfheal_attempted = FALSE;
  goodix_launch_open_ssm (dev);
}

/* ========================================================================
 * Post-sleep reinitialization
 * ======================================================================== */

/**
 * If the device needs reinitialization (system sleep happened while it was
 * open), release any stale interface claim and run the full open-time
 * initialization SSM as a sub-SSM of @ssm. The full sequence is required:
 * after an S4 reset/re-enumeration the kernel rebinds cdc_acm to our
 * interface, so recovery needs the same USB reset + claim-with-detach +
 * GTLS handshake as a fresh open.
 *
 * Returns TRUE if a reinit sub-SSM was started (caller returns and the
 * parent advances when it completes), FALSE if no reinit was needed.
 */
gboolean
goodix_maybe_start_reinit_subsm (FpiSsm   *ssm,
                                 FpDevice *dev)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);
  FpiSsm *sub;

  if (!self->needs_reinit)
    return FALSE;

  fp_info ("Reinitializing device after system sleep");

  if (self->usb_interface_claimed)
    {
      g_autoptr(GError) release_error = NULL;

      /* This is expected to fail with EINVAL after an S4 reset because the
       * kernel already dropped the claim; recovery proceeds either way. */
      if (!g_usb_device_release_interface (fpi_device_get_usb_device (dev),
                                           self->usb_interface,
                                           0, &release_error))
        fp_dbg ("Releasing stale USB interface before reinit failed "
                "(expected after S4 reset): %s", release_error->message);

      self->usb_interface_claimed = FALSE;
    }

  sub = fpi_ssm_new (dev, goodix_open_ssm_handler,
                     GOODIX_OPEN_NUM_STATES);
  fpi_ssm_start_subsm (ssm, sub);
  return TRUE;
}

/**
 * TRUE for errors that indicate the USB device/claim is likely stale or
 * gone (e.g. system slept while the device was claimed but idle, so the
 * driver suspend hook never ran). Setting needs_reinit on these makes the
 * next action attempt self-heal with a full reinitialization.
 */
gboolean
goodix_error_indicates_stale_device (const GError *error)
{
  return g_error_matches (error, G_USB_DEVICE_ERROR,
                          G_USB_DEVICE_ERROR_TIMED_OUT) ||
         g_error_matches (error, G_USB_DEVICE_ERROR,
                          G_USB_DEVICE_ERROR_NO_DEVICE) ||
         g_error_matches (error, G_USB_DEVICE_ERROR,
                          G_USB_DEVICE_ERROR_NOT_OPEN) ||
         g_error_matches (error, G_USB_DEVICE_ERROR,
                          G_USB_DEVICE_ERROR_IO) ||
         g_error_matches (error, G_USB_DEVICE_ERROR,
                          G_USB_DEVICE_ERROR_FAILED);
}

/* ========================================================================
 * Suspend / resume policy
 * ======================================================================== */

void
goodix_session_suspend (FpDevice *dev)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);
  FpiDeviceAction action = fpi_device_get_current_action (dev);

  /* Any system sleep while the device is open may invalidate the USB claim
   * and GTLS session: S4 resets or re-enumerates the device and rebinds the
   * cdc_acm kernel driver to our interface. Force a full reinitialization at
   * the start of the next action regardless of what we were doing when sleep
   * hit. A successfully completed action clears this again. */
  self->needs_reinit = TRUE;

  if (action != FPI_DEVICE_ACTION_VERIFY &&
      action != FPI_DEVICE_ACTION_IDENTIFY)
    {
      fpi_device_suspend_complete (dev,
          fpi_device_error_new (FP_DEVICE_ERROR_NOT_SUPPORTED));
      return;
    }

  if (self->blocking_ssm)
    {
      /* Cancel the pending read; suspend_complete called from rx callback */
      self->suspend_pending = TRUE;
      g_cancellable_cancel (self->cancel);
    }
  else
    {
      /* Not in a blocking read (e.g. mid-capture), complete immediately */
      fpi_device_suspend_complete (dev, NULL);
    }
}

void
goodix_session_resume (FpDevice *dev)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);
  FpiDeviceAction action = fpi_device_get_current_action (dev);

  if (action != FPI_DEVICE_ACTION_VERIFY &&
      action != FPI_DEVICE_ACTION_IDENTIFY)
    {
      fpi_device_resume_complete (dev,
          fpi_device_error_new (FP_DEVICE_ERROR_NOT_SUPPORTED));
      return;
    }

  g_clear_object (&self->cancel);
  self->cancel = g_cancellable_new ();

  /* Restart the SSM from the re-arm state (resubmits USB reads). Only
   * reachable if suspend completed successfully mid-capture and the SSM
   * armed a blocking wait before the system actually slept. */
  if (self->blocking_ssm)
    {
      fpi_ssm_jump_to_state (self->blocking_ssm, self->blocking_resume_state);
      self->blocking_ssm = NULL;
    }

  fpi_device_resume_complete (dev, NULL);
}
