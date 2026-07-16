/*
 * Goodix 53x5 driver for libfprint — 550c application firmware loader
 * Copyright (C) 2026 goodix-550c-driver contributors
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
 * Loads the Goodix 550c application image that self-heal reflashes after the
 * erase-to-IAP step. The image itself is Goodix's proprietary firmware and is
 * not shipped with this driver — see goodix53x5-firmware550c.h and README.md.
 *
 * The image is validated against a known size and SHA-256 before the caller is
 * allowed to touch the device, so a truncated, wrong-device or corrupt file is
 * rejected before anything has been erased.
 */

#define FP_COMPONENT "goodix53x5"

#include "goodix53x5-firmware550c.h"

#include <gio/gio.h>
#include <string.h>

/* Env var override, mirroring GOODIX550C_PSK. */
#define GOODIX_550C_FIRMWARE_ENV "GOODIX550C_FIRMWARE"

/* Search path, in order. The first file that exists is used; a file that
 * exists but fails validation is an error rather than a reason to keep
 * looking, so a bad install gets reported instead of silently skipped. */
static const gchar *const goodix_550c_firmware_paths[] = {
  "/etc/goodix550c-app.bin",
  "/usr/lib/firmware/goodix/550c-app.bin",
  "/usr/local/lib/firmware/goodix/550c-app.bin",
  NULL,
};

static gboolean
goodix_550c_firmware_validate (const guint8 *data,
                               gsize         len,
                               const gchar  *path,
                               GError      **error)
{
  g_autofree gchar *digest = NULL;

  if (len != GOODIX_550C_APP_FIRMWARE_LEN)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "550c firmware '%s' is %" G_GSIZE_FORMAT " bytes, expected %d",
                   path, len, GOODIX_550C_APP_FIRMWARE_LEN);
      return FALSE;
    }

  digest = g_compute_checksum_for_data (G_CHECKSUM_SHA256, data, len);
  if (g_strcmp0 (digest, GOODIX_550C_APP_FIRMWARE_SHA256) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "550c firmware '%s' has sha256 %s, expected %s",
                   path, digest, GOODIX_550C_APP_FIRMWARE_SHA256);
      return FALSE;
    }

  return TRUE;
}

static const gchar *
goodix_550c_firmware_find (void)
{
  const gchar *env = g_getenv (GOODIX_550C_FIRMWARE_ENV);

  if (env != NULL && *env != '\0')
    return env;

  for (int i = 0; goodix_550c_firmware_paths[i] != NULL; i++)
    {
      if (g_file_test (goodix_550c_firmware_paths[i], G_FILE_TEST_EXISTS))
        return goodix_550c_firmware_paths[i];
    }

  return NULL;
}

const guint8 *
goodix_550c_app_firmware_load (gsize   *len,
                               GError **error)
{
  static guint8 *cached = NULL;
  static gsize cached_len = 0;

  g_autofree gchar *data = NULL;
  gsize data_len = 0;
  const gchar *path;

  if (cached != NULL)
    {
      *len = cached_len;
      return cached;
    }

  path = goodix_550c_firmware_find ();
  if (path == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "no 550c application firmware installed (looked at $%s and %s) — "
                   "self-heal needs it to restore the sensor after the erase-to-IAP "
                   "step; see the driver README",
                   GOODIX_550C_FIRMWARE_ENV, goodix_550c_firmware_paths[0]);
      return NULL;
    }

  if (!g_file_get_contents (path, &data, &data_len, error))
    return NULL;

  if (!goodix_550c_firmware_validate ((const guint8 *) data, data_len, path, error))
    return NULL;

  cached = (guint8 *) g_steal_pointer (&data);
  cached_len = data_len;

  *len = cached_len;
  return cached;
}
