/*
 * Copyright 2026, DruSatori, dru@druware.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef _ACCELERANT_PRIVATE_HOOKS_H
#define _ACCELERANT_PRIVATE_HOOKS_H


/*!	Accelerant hooks that app_server knows, but that are not part of the
	public accelerant API in <Accelerant.h>.
*/


#include <Accelerant.h>


/*!	Optional: returns a semaphore that the accelerant releases whenever the
	preferred display mode changes, e.g. when the window of a virtual machine
	is resized. The caller only acquires it.
*/
#define ACCELERANT_DISPLAY_CHANGE_SEMAPHORE ((uint32)B_ACCELERANT_PRIVATE_START)


/*!	Optional: app_server calls it after it changed the frame buffer between
	\a left, \a top and \a right, \a bottom (inclusive), for a device that only
	shows what it is told about, e.g. a virtual GPU that copies the frame
	buffer to the host.
*/
#define ACCELERANT_FRAME_BUFFER_CHANGED \
	((uint32)B_ACCELERANT_PRIVATE_START + 1)

/*!	Optional: app_server calls it with \a added true when a client that writes
	to the frame buffer on its own appears (a BDirectWindow), and with false
	when it is gone. While there is one, ACCELERANT_FRAME_BUFFER_CHANGED does
	not cover every change.
*/
#define ACCELERANT_DIRECT_FRAME_BUFFER_CLIENT \
	((uint32)B_ACCELERANT_PRIVATE_START + 2)


typedef sem_id (*accelerant_display_change_semaphore)(void);
typedef void (*accelerant_frame_buffer_changed)(int32 left, int32 top,
	int32 right, int32 bottom);
typedef void (*accelerant_direct_frame_buffer_client)(bool added);


#endif	/* _ACCELERANT_PRIVATE_HOOKS_H */
