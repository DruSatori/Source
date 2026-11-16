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


typedef sem_id (*accelerant_display_change_semaphore)(void);


#endif	/* _ACCELERANT_PRIVATE_HOOKS_H */
