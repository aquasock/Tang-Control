/*
 * Copyright 2026 aquasock
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

// Configure the BL616 OTG connector as a USB CDC ACM device.
void usb_cdc_console_init(void);

// Start the command-processing task after the other TangCore tasks exist.
void usb_cdc_console_start_task(void);
