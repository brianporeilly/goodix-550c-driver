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
 * Self-heal PSK provisioning has to erase the sensor's application firmware to
 * reach IAP mode, so it must write a valid application image back afterwards.
 * That image is Goodix's proprietary signed firmware: it is not part of this
 * driver, is not covered by this driver's licence, and is not redistributed
 * here. The driver loads it at runtime from a file the machine's owner
 * supplies (see README.md, "Supplying the 550c application firmware").
 *
 * Without that file self-heal is unavailable, and the driver reports that
 * rather than erasing a sensor it would be unable to restore.
 */

#pragma once

#include <glib.h>

/* Expected size of the 550c application image, in bytes. */
#define GOODIX_550C_APP_FIRMWARE_LEN 23436

/* SHA-256 of the expected application image, as lowercase hex. */
#define GOODIX_550C_APP_FIRMWARE_SHA256 \
  "7a230f035b09e07a243bf7dd8c5055c8c8e8faf85dc102fd6c4f7b6249da6e8c"

/*
 * Load the 550c application image from the local firmware search path.
 *
 * Returns a pointer to the cached image and stores its length in @len, or NULL
 * with @error set when no image is installed or the installed one does not
 * match the expected size and digest. The buffer is owned by the driver and
 * stays valid for the process lifetime; the caller must not free it.
 */
const guint8 *goodix_550c_app_firmware_load (gsize   *len,
                                             GError **error);
