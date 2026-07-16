/*
 * Goodix 53x5 driver for libfprint — Named device commands and reply parsers
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

#include "goodix53x5-private.h"

/* ========================================================================
 * Named device commands
 *
 * These wrap goodix_run_cmd() so action modules read as device operations
 * instead of category/command bytes and hand-built payloads. Commands that
 * expect a data reply have a matching named reply parser below; commands
 * documented as "ACK only" advance the parent SSM after ACK validation.
 * ======================================================================== */

/* Ping the MCU. ACK only. */
void goodix_cmd_ping (FpiSsm *ssm, FpDevice *dev);

/* Read the firmware version string. Expects data. */
void goodix_cmd_read_fw_version (FpiSsm *ssm, FpDevice *dev);

/* Reset the sensor (reset type 0, no IRQ status). ACK only. */
void goodix_cmd_reset_sensor (FpiSsm *ssm, FpDevice *dev);

/* Read 4 bytes of chip ID from register address 0. Expects data. */
void goodix_cmd_read_chip_id (FpiSsm *ssm, FpDevice *dev);

/* 550c: write a single 16-bit sensor register (ACK only). */
void goodix_cmd_write_sensor_register (FpiSsm *ssm, FpDevice *dev,
                                       guint16 address, guint8 v0, guint8 v1);

/* Read the OTP calibration block. Expects data. */
void goodix_cmd_read_otp (FpiSsm *ssm, FpDevice *dev);

/* Production read of @read_type (e.g. 0xB003 = PSK hash). Expects data. */
void goodix_cmd_production_read (FpiSsm *ssm, FpDevice *dev,
                                 guint32 read_type);

/* Production write of @data under @data_type (e.g. 0xB002 = PSK white box).
 * Expects data (a one-byte status reply). */
void goodix_cmd_production_write (FpiSsm *ssm, FpDevice *dev,
                                  guint32 data_type,
                                  const guint8 *data, gsize data_len);

/* Send an MCU envelope of @data_type (GTLS handshake traffic). ACK only;
 * the device replies asynchronously via a plain receive. */
void goodix_cmd_mcu_send (FpiSsm *ssm, FpDevice *dev, guint32 data_type,
                          const guint8 *data, gsize data_len);

/* Upload the (patched) sensor config blob. Expects data (success flag). */
void goodix_cmd_upload_config (FpiSsm *ssm, FpDevice *dev,
                               const guint8 *config, gsize config_len);

/* Arm finger-down detection with @fdt_base. ACK only; the FDT event arrives
 * later via a cancellable receive. */
void goodix_cmd_fdt_down_setup (FpiSsm *ssm, FpDevice *dev,
                                const guint8 *fdt_base);

/* Arm finger-up detection with @fdt_base. ACK only; the FDT event arrives
 * later via a cancellable receive. */
void goodix_cmd_fdt_up_setup (FpiSsm *ssm, FpDevice *dev,
                              const guint8 *fdt_base);

/* Run a manual FDT check against @fdt_base, with sensor TX on or off.
 * Expects data (irq status, touch flag and live FDT data). */
void goodix_cmd_fdt_manual (FpiSsm *ssm, FpDevice *dev,
                            gboolean tx_enable, const guint8 *fdt_base);

/* Request an image frame. Expects data (the encrypted frame). */
void goodix_cmd_request_image (FpiSsm *ssm, FpDevice *dev,
                               gboolean tx_enable, gboolean hv_enable,
                               gboolean is_finger, guint16 dac);

/* Put the MCU into sleep mode. ACK only. */
void goodix_cmd_set_sleep_mode (FpiSsm *ssm, FpDevice *dev);

/* Switch sensor EC power on or off. Expects data (success flag). */
void goodix_cmd_ec_control (FpiSsm *ssm, FpDevice *dev, gboolean on);

/* ------------------------------------------------------------------------
 * IAP / provisioning commands (550c self-heal). See goodix53x5-session.c.
 * ------------------------------------------------------------------------ */

/* Read a PSK slot (0xE4). @flags selects the slot (e.g. 0xbb020001 = the
 * SHA256(PSK) hash slot). Expects a data reply; parse with
 * goodix_cmd_parse_preset_psk_read_reply. */
void goodix_cmd_preset_psk_read (FpiSsm *ssm, FpDevice *dev,
                                 guint32 flags, guint32 length, guint32 offset);

/* Erase the app to drop into IAP. ACK only (device re-enumerates after). */
void goodix_cmd_mcu_erase_app (FpiSsm *ssm, FpDevice *dev);

/* Write one offset-chunk of the PSK container. Expects data (status byte). */
void goodix_cmd_preset_psk_write_chunk (FpiSsm *ssm, FpDevice *dev,
                                        guint32 total_len, guint32 chunk_len,
                                        guint32 offset, const guint8 *chunk);

/* Write one firmware chunk at @offset (@number is the 550c's extra field, =2).
 * Expects data (status byte). */
void goodix_cmd_write_firmware (FpiSsm *ssm, FpDevice *dev,
                                guint32 offset, const guint8 *data,
                                gsize data_len, guint32 number);

/* Validate the reflashed app against a 32-byte PSK-derived HMAC. Expects data
 * (status byte). */
void goodix_cmd_check_firmware (FpiSsm *ssm, FpDevice *dev,
                                const guint8 *hmac32);

/* Soft-reset the MCU back into the app. ACK only (device re-enumerates). */
void goodix_cmd_mcu_reset_soft (FpiSsm *ssm, FpDevice *dev);

/* ========================================================================
 * Named reply parsers
 *
 * All returned payload pointers point into the current RX buffer and are
 * valid only until the next receive reset.
 * ======================================================================== */

/* Parse a preset_psk_read (0xE4) reply. On success @out_data / @out_data_len
 * point at the slot payload (e.g. the 32-byte SHA256(PSK) hash). */
gboolean goodix_cmd_parse_preset_psk_read_reply (FpDevice      *dev,
                                                 const guint8 **out_data,
                                                 gsize         *out_data_len,
                                                 GError       **error);

gboolean goodix_cmd_parse_fw_version_reply (FpDevice      *dev,
                                            const guint8 **out_payload,
                                            gsize         *out_payload_len,
                                            GError       **error);

gboolean goodix_cmd_parse_chip_id_reply (FpDevice      *dev,
                                         const guint8 **out_payload,
                                         gsize         *out_payload_len,
                                         GError       **error);

gboolean goodix_cmd_parse_otp_reply (FpDevice      *dev,
                                     const guint8 **out_payload,
                                     gsize         *out_payload_len,
                                     GError       **error);

gboolean goodix_cmd_parse_production_read_reply (FpDevice      *dev,
                                                 guint32        read_type,
                                                 const guint8 **out_data,
                                                 gsize         *out_data_len);

gboolean goodix_cmd_parse_production_write_reply (FpDevice      *dev,
                                                  const guint8 **out_payload,
                                                  gsize         *out_payload_len);

/* IAP reply parsers: TRUE on success, else FALSE with @error set. */
gboolean goodix_cmd_parse_preset_psk_write_reply (FpDevice *dev,
                                                  GError  **error);
gboolean goodix_cmd_parse_write_firmware_reply (FpDevice *dev,
                                                GError  **error);
gboolean goodix_cmd_parse_check_firmware_reply (FpDevice *dev,
                                                GError  **error);

gboolean goodix_cmd_parse_mcu_reply (FpDevice      *dev,
                                     guint32        expected_type,
                                     const guint8 **out_data,
                                     gsize         *out_data_len);

/* TRUE if the config upload reply reports success. */
gboolean goodix_cmd_parse_config_reply (FpDevice *dev);

/* TRUE if the EC power control reply reports success. */
gboolean goodix_cmd_parse_ec_control_reply (FpDevice *dev);

/* Parse a manual FDT reading returned by goodix_cmd_fdt_manual(). */
gboolean goodix_cmd_parse_fdt_manual_reply (FpDevice      *dev,
                                             const guint8 **out_payload,
                                             gsize         *out_payload_len,
                                             GError       **error);

/* Parse an FDT down/up event delivered after goodix_cmd_fdt_down_setup() /
 * goodix_cmd_fdt_up_setup(). The payload layout is
 * [irq_status(2)][touch_flag(2)][fdt_data(GOODIX_FDT_BASE_LEN)]. */
gboolean goodix_cmd_parse_fdt_event (FpDevice      *dev,
                                     gboolean       up,
                                     const guint8 **out_payload,
                                     gsize         *out_payload_len,
                                     GError       **error);
