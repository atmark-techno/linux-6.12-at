/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __LINMUXTTY_H
#define __LINMUXTTY_H
//////////////////////////////////////////////////////////////////////////////
//
// linmuxtty.h
//
// This file contains the definition of the linmux tty interface.
//
//////////////////////////////////////////////////////////////////////////////

int mux_serial_probe(struct platform_device *pdev);
void mux_serial_remove(struct platform_device *pdev);

void mux_dev_release(struct device *dev);

#endif // __LINMUXTTY_H
