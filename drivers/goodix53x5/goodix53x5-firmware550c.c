/*
 * Goodix 53x5 driver for libfprint -- 550c application firmware (REMOVED)
 *
 * This file previously embedded the Goodix 550c application image: 23,436
 * bytes of Goodix's signed, proprietary firmware, captured verbatim from a
 * Windows provisioning sequence, under an LGPL-2.1 header that credited
 * "goodix-fp-linux-dev contributors".
 *
 * That attribution was wrong. The image is Goodix's copyrighted work; no one
 * in this project's lineage holds the right to redistribute it or to place it
 * under the LGPL. It has been purged from this repository's history.
 *
 * The driver now loads the image at runtime from a file the machine's owner
 * supplies. See goodix53x5-firmware550c.c at the tip of this branch, and the
 * README section "Supplying the 550c application firmware".
 *
 * Commits between this one and the removal do not build as a result. That is
 * the intended trade-off: the vendor binary does not belong in this history.
 */
