/*
 * Copyright 2023, Jérôme Duval, jerome.duval@gmail.com.
 * Distributed under the terms of the MIT License.
 */


#include <new>
#include <stddef.h>

#include <graphic_driver.h>

#include <lock.h>
#include <virtio.h>
#include <virtio_info.h>

#include <team.h>
#include <util/AutoLock.h>

#include "viogpu.h"


#define VIRTIO_GPU_DRIVER_MODULE_NAME "drivers/graphics/virtio_gpu/driver_v1"
#define VIRTIO_GPU_DEVICE_MODULE_NAME "drivers/graphics/virtio_gpu/device_v1"
#define VIRTIO_GPU_DEVICE_ID_GENERATOR	"virtio_gpu/device_id"

// Host display sizes below this are ignored: a host may report 0x0 or a
// tiny size while its window is minimized or changing.
#define MIN_DISPLAY_WIDTH	320
#define MIN_DISPLAY_HEIGHT	200


typedef struct {
	device_node*			node;
	::virtio_device			virtio_device;
	virtio_device_interface*	virtio;

	uint64 					features;

	::virtio_queue			controlQueue;
	mutex					commandLock;
	area_id					commandArea;
	addr_t					commandBuffer;
	phys_addr_t				commandPhysAddr;
	sem_id					commandDone;
	uint64					fenceId;

	::virtio_queue			cursorQueue;
	mutex					cursorLock;
	area_id					cursorArea;
	addr_t					cursorImage;
		// VIRTIO_GPU_CURSOR_SIZE² pixels, then the cursor queue command
	phys_addr_t				cursorPhysAddr;
	struct virtio_gpu_update_cursor cursor;
		// latest cursor state for the host
	uint32					cursorPending;
		// command type of a cursor state not sent yet, or 0
	virtio_gpu_cursor_image* cursorNewImage;
		// set by VIRTIO_GPU_SET_CURSOR, handed to the host by the update thread

	int						displayResourceId;
	uint32					framebufferWidth;
	uint32					framebufferHeight;
	area_id					framebufferArea;
	addr_t					framebuffer;
	size_t					framebufferSize;
	uint32					displayWidth;
	uint32					displayHeight;

	thread_id				updateThread;
	bool					updateThreadRunning;
	sem_id					updateSem;
		// wakes the update thread, see virtio_gpu_wake_update_thread()
	int32					updateWakePending;
	mutex					dirtyLock;
	virtio_gpu_dirty_rect	dirty;
		// the part of the framebuffer that changed since the last transfer,
		// empty when right < left
	int32					directClients;
		// BDirectWindows, which write the framebuffer without telling us
	int32					displayEvent;
		// set by the config change interrupt, handled by the update thread
	sem_id					displayChangeSem;
		// released when the host's display size changes, see
		// VIRTIO_GPU_GET_DISPLAY_CHANGE_SEM

	area_id					sharedArea;
	virtio_gpu_shared_info* sharedInfo;

	// The display is set up on the first open and torn down after the last
	// handle is freed; app_server and any accelerant clones share it.
	mutex					openLock;
	int32					openCount;
	char					deviceName[64];	// as published, for clones
} virtio_gpu_driver_info;


typedef struct {
	virtio_gpu_driver_info*		info;
} virtio_gpu_handle;


#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <fs/devfs.h>

#define ROUND_TO_PAGE_SIZE(x) (((x) + (B_PAGE_SIZE) - 1) & ~((B_PAGE_SIZE) - 1))

#define CURSOR_RESOURCE_ID		0x10000000
	// display resources count up from 1
#define CURSOR_IMAGE_SIZE		(VIRTIO_GPU_CURSOR_SIZE * VIRTIO_GPU_CURSOR_SIZE * 4)


#define DEVICE_NAME				"virtio_gpu"
#define ACCELERANT_NAME	"virtio_gpu.accelerant"
//#define TRACE_VIRTIO_GPU
#ifdef TRACE_VIRTIO_GPU
#	define TRACE(x...) dprintf(DEVICE_NAME ": " x)
#else
#	define TRACE(x...) ;
#endif
#define ERROR(x...)			dprintf("\33[33m" DEVICE_NAME ":\33[0m " x)
#define CALLED() 			TRACE("CALLED %s\n", __PRETTY_FUNCTION__)


static device_manager_info* sDeviceManager;


static void virtio_gpu_vqwait(void* driverCookie, void* cookie);
static void virtio_gpu_config_changed(void* cookie);


const char*
get_feature_name(uint64 feature)
{
	switch (feature) {
		case VIRTIO_GPU_F_VIRGL:
			return "virgl";
		case VIRTIO_GPU_F_EDID:
			return "edid";
		case VIRTIO_GPU_F_RESOURCE_UUID:
			return "res_uuid";
		case VIRTIO_GPU_F_RESOURCE_BLOB:
			return "res_blob";
	}
	return NULL;
}


static status_t
virtio_gpu_drain_queues(virtio_gpu_driver_info* info)
{
	while (info->virtio->queue_dequeue(info->controlQueue, NULL, NULL))
		;

	while (info->virtio->queue_dequeue(info->cursorQueue, NULL, NULL))
		;

	return B_OK;
}


status_t
virtio_gpu_send_cmd(virtio_gpu_driver_info* info, void *cmd, size_t cmdSize, void *response,
    size_t responseSize)
{
	struct virtio_gpu_ctrl_hdr *hdr = (struct virtio_gpu_ctrl_hdr *)info->commandBuffer;
	struct virtio_gpu_ctrl_hdr *responseHdr = (struct virtio_gpu_ctrl_hdr *)response;

	memcpy((void*)info->commandBuffer, cmd, cmdSize);
	memset((void*)(info->commandBuffer + cmdSize), 0, responseSize);
	hdr->flags |= VIRTIO_GPU_FLAG_FENCE;
	hdr->fence_id = ++info->fenceId;

	physical_entry entries[] {
		{ info->commandPhysAddr, cmdSize },
		{ info->commandPhysAddr + cmdSize, responseSize },
	};
	if (!info->virtio->queue_is_empty(info->controlQueue))
		return B_ERROR;

	status_t status = info->virtio->queue_request_v(info->controlQueue, entries, 1, 1, NULL);
	if (status != B_OK)
		return status;

	acquire_sem(info->commandDone);

	while (!info->virtio->queue_dequeue(info->controlQueue, NULL, NULL))
		spin(10);

	memcpy(response, (void*)(info->commandBuffer + cmdSize), responseSize);

	if (responseHdr->fence_id != info->fenceId) {
		ERROR("response fence id not right\n");
	}
	return B_OK;
}


/*!	Asks the host for the configuration of scanout 0. Called with the
	commandLock held.
*/
status_t
virtio_gpu_get_display_info(virtio_gpu_driver_info* info, uint32& width,
	uint32& height, bool& enabled)
{
	CALLED();
	struct virtio_gpu_ctrl_hdr hdr = {};
	struct virtio_gpu_resp_display_info displayInfo = {};

	hdr.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

	virtio_gpu_send_cmd(info, &hdr, sizeof(hdr), &displayInfo, sizeof(displayInfo));

	if (displayInfo.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
		ERROR("failed getting display info\n");
		return B_ERROR;
	}

	enabled = displayInfo.pmodes[0].enabled != 0;
	width = displayInfo.pmodes[0].r.width;
	height = displayInfo.pmodes[0].r.height;
	TRACE("virtio_gpu_get_display_info width %" B_PRIu32 " height %" B_PRIu32
		" enabled %d\n", width, height, enabled);

	return B_OK;
}


status_t
virtio_gpu_get_edids(virtio_gpu_driver_info* info, int scanout)
{
	CALLED();
	struct virtio_gpu_cmd_get_edid getEdid = {};
	struct virtio_gpu_resp_edid response = {};
	getEdid.hdr.type = VIRTIO_GPU_CMD_GET_EDID;
	getEdid.scanout = scanout;

	virtio_gpu_send_cmd(info, &getEdid, sizeof(getEdid), &response, sizeof(response));

	if (response.hdr.type != VIRTIO_GPU_RESP_OK_EDID) {
		ERROR("failed getting edids %d\n", response.hdr.type);
		return B_ERROR;
	}

	info->sharedInfo->has_edid = true;
	memcpy(&info->sharedInfo->edid_raw, response.edid, sizeof(edid1_raw));
	TRACE("virtio_gpu_get_edids success\n");

	return B_OK;
}


status_t
virtio_gpu_create_2d(virtio_gpu_driver_info* info, int resourceId, int width, int height,
	uint32 format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM)
{
	CALLED();
	struct virtio_gpu_resource_create_2d resource = {};
	struct virtio_gpu_ctrl_hdr response = {};

	resource.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
	resource.resource_id = resourceId;
	resource.format = format;
	resource.width = width;
	resource.height = height;

	virtio_gpu_send_cmd(info, &resource, sizeof(resource), &response, sizeof(response));

	if (response.type != VIRTIO_GPU_RESP_OK_NODATA) {
		ERROR("viogpu_create_2d: failed %d\n", response.type);
		return B_ERROR;
	}

	return B_OK;
}


status_t
virtio_gpu_unref(virtio_gpu_driver_info* info, int resourceId)
{
	CALLED();
	struct virtio_gpu_resource_unref resource = {};
	struct virtio_gpu_ctrl_hdr response = {};

	resource.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
	resource.resource_id = resourceId;

	virtio_gpu_send_cmd(info, &resource, sizeof(resource), &response, sizeof(response));

	if (response.type != VIRTIO_GPU_RESP_OK_NODATA) {
		ERROR("virtio_gpu_unref: failed %d\n", response.type);
		return B_ERROR;
	}

	return B_OK;
}


status_t
virtio_gpu_attach_backing(virtio_gpu_driver_info* info, int resourceId, addr_t address,
	size_t size)
{
	CALLED();
	struct virtio_gpu_resource_attach_backing_entries {
		struct virtio_gpu_resource_attach_backing backing;
		struct virtio_gpu_mem_entry entries[16];
	} _PACKED backing = {};
	struct virtio_gpu_ctrl_hdr response = {};

	physical_entry entries[16] = {};
	status_t status = get_memory_map((void*)address, size, entries, 16);
	if (status != B_OK) {
		ERROR("virtio_gpu_attach_backing get_memory_map failed: %s\n", strerror(status));
		return status;
	}

	backing.backing.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
	backing.backing.resource_id = resourceId;
	for (int i = 0; i < 16; i++) {
		if (entries[i].size == 0)
			break;
		TRACE("virtio_gpu_attach_backing %d %" B_PRIxPHYSADDR " %" B_PRIxPHYSADDR "\n", i,
			entries[i].address, entries[i].size);
		backing.entries[i].addr = entries[i].address;
		backing.entries[i].length = entries[i].size;
		backing.backing.nr_entries++;
	}

	virtio_gpu_send_cmd(info, &backing, sizeof(backing), &response, sizeof(response));

	if (response.type != VIRTIO_GPU_RESP_OK_NODATA) {
		ERROR("virtio_gpu_attach_backing failed: %d\n", response.type);
		return B_ERROR;
	}

	return B_OK;
}


status_t
virtio_gpu_detach_backing(virtio_gpu_driver_info* info, int resourceId)
{
	CALLED();
	struct virtio_gpu_resource_detach_backing backing;
	struct virtio_gpu_ctrl_hdr response = {};

	backing.hdr.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
	backing.resource_id = resourceId;

	virtio_gpu_send_cmd(info, &backing, sizeof(backing), &response, sizeof(response));

	if (response.type != VIRTIO_GPU_RESP_OK_NODATA) {
		ERROR("virtio_gpu_detach_backing failed: %d\n", response.type);
		return B_ERROR;
	}

	return B_OK;
}


status_t
virtio_gpu_set_scanout(virtio_gpu_driver_info* info, int scanoutId, int resourceId,
    uint32 width, uint32 height)
{
	CALLED();
	struct virtio_gpu_set_scanout set_scanout = {};
	struct virtio_gpu_ctrl_hdr response = {};

	set_scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
	set_scanout.scanout_id = scanoutId;
	set_scanout.resource_id = resourceId;
	set_scanout.r.width = width;
	set_scanout.r.height = height;

	virtio_gpu_send_cmd(info, &set_scanout, sizeof(set_scanout), &response, sizeof(response));

	if (response.type != VIRTIO_GPU_RESP_OK_NODATA) {
		ERROR("virtio_gpu_set_scanout failed %d\n", response.type);
		return B_ERROR;
	}

	return B_OK;
}


/*!	Copies the rectangle at \a x, \a y of the resource's backing to the host;
	\a offset is where its left top pixel is in the backing.
*/
status_t
virtio_gpu_transfer_to_host_2d(virtio_gpu_driver_info* info, int resourceId,
    uint32 width, uint32 height, uint32 x = 0, uint32 y = 0, uint64 offset = 0)
{
	struct virtio_gpu_transfer_to_host_2d transferToHost = {};
	struct virtio_gpu_ctrl_hdr response = {};

	transferToHost.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
	transferToHost.resource_id = resourceId;
	transferToHost.r.x = x;
	transferToHost.r.y = y;
	transferToHost.r.width = width;
	transferToHost.r.height = height;
	transferToHost.offset = offset;

	virtio_gpu_send_cmd(info, &transferToHost, sizeof(transferToHost), &response,
		sizeof(response));

	if (response.type != VIRTIO_GPU_RESP_OK_NODATA) {
		ERROR("virtio_gpu_transfer_to_host_2d failed %d\n", response.type);
		return B_ERROR;
	}

	return B_OK;
}


status_t
virtio_gpu_flush_resource(virtio_gpu_driver_info* info, int resourceId, uint32 width,
    uint32 height, uint32 x = 0, uint32 y = 0)
{
	struct virtio_gpu_resource_flush resourceFlush = {};
	struct virtio_gpu_ctrl_hdr response = {};

	resourceFlush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
	resourceFlush.resource_id = resourceId;
	resourceFlush.r.x = x;
	resourceFlush.r.y = y;
	resourceFlush.r.width = width;
	resourceFlush.r.height = height;

	virtio_gpu_send_cmd(info, &resourceFlush, sizeof(resourceFlush), &response, sizeof(response));

	if (response.type != VIRTIO_GPU_RESP_OK_NODATA) {
		ERROR("virtio_gpu_flush_resource failed %d\n", response.type);
		return B_ERROR;
	}

	return B_OK;
}


/*!	Handles a VIRTIO_GPU_EVENT_DISPLAY (the host resized its window): acknowledges
	it, re-reads the host's display size and, when it changed, releases the
	display change semaphore so app_server switches to the new preferred mode.
	Must be called with the command lock held.
*/
static void
virtio_gpu_handle_display_event(virtio_gpu_driver_info* info)
{
	uint32 events = 0;
	info->virtio->read_device_config(info->virtio_device,
		offsetof(struct virtio_gpu_config, events_read), &events, sizeof(events));
	if ((events & VIRTIO_GPU_EVENT_DISPLAY) == 0)
		return;
	info->virtio->write_device_config(info->virtio_device,
		offsetof(struct virtio_gpu_config, events_clear), &events, sizeof(events));

	uint32 width;
	uint32 height;
	bool enabled;
	if (virtio_gpu_get_display_info(info, width, height, enabled) != B_OK
		|| !enabled || width < MIN_DISPLAY_WIDTH
		|| height < MIN_DISPLAY_HEIGHT) {
		return;
	}
	width = min_c(width, info->framebufferWidth);
	height = min_c(height, info->framebufferHeight);
	if (width == info->sharedInfo->preferred_width
		&& height == info->sharedInfo->preferred_height) {
		return;
	}
	info->sharedInfo->preferred_width = width;
	info->sharedInfo->preferred_height = height;

	dprintf(DEVICE_NAME ": host display size now %" B_PRIu32 "x%" B_PRIu32 "\n",
		width, height);
	release_sem_etc(info->displayChangeSem, 1, B_DO_NOT_RESCHEDULE);
}


/*!	Sends the cursor state in info->cursor on the cursor queue. The host shows
	that cursor on its own, so the pointer does not wait for the framebuffer
	updates. Must be called with the cursor lock held.

	Never waits: app_server calls this for every mouse move while holding its
	exclusive lock, which stops all drawing. While the host has not finished
	the previous command, which shares our buffer, the command is only marked
	pending, and the update thread sends the latest state on its next cycle.
	\a type 0 just retries a pending command.
*/
static status_t
virtio_gpu_send_cursor(virtio_gpu_driver_info* info, uint32 type)
{
	// UPDATE_CURSOR carries the position too, so it supersedes a pending move
	if (info->cursorPending != VIRTIO_GPU_CMD_UPDATE_CURSOR && type != 0)
		info->cursorPending = type;

	while (info->virtio->queue_dequeue(info->cursorQueue, NULL, NULL))
		;
	if (info->cursorPending == 0
		|| !info->virtio->queue_is_empty(info->cursorQueue)) {
		return B_OK;
	}

	info->cursor.hdr.type = info->cursorPending;
	info->cursorPending = 0;
	memcpy((void*)(info->cursorImage + CURSOR_IMAGE_SIZE), &info->cursor,
		sizeof(info->cursor));
	physical_entry entry = { info->cursorPhysAddr + CURSOR_IMAGE_SIZE, sizeof(info->cursor) };
	status_t status = info->virtio->queue_request_v(info->cursorQueue, &entry, 1, 0,
		NULL);
	if (status != B_OK)
		info->cursorPending = info->cursor.hdr.type;
	return status;
}


/*!	Hands the image VIRTIO_GPU_SET_CURSOR left in info->cursorNewImage to the
	host. Called by the update thread only: the transfer needs the command
	lock, which the framebuffer updates hold for a long time, and app_server
	sets the cursor while holding its exclusive lock, which stops all drawing.
*/
static void
virtio_gpu_update_cursor_image(virtio_gpu_driver_info* info)
{
	virtio_gpu_cursor_image* cursor;
	{
		MutexLocker cursorLocker(&info->cursorLock);
		cursor = info->cursorNewImage;
		info->cursorNewImage = NULL;
	}
	if (cursor == NULL)
		return;

	// Not locked against the host: QEMU reads the backing only during the
	// transfer, into its own copy. A host that reads it later (unknown for
	// Parallels Desktop) could show a torn image while the cursor changes.
	memcpy((void*)info->cursorImage, cursor->bits, CURSOR_IMAGE_SIZE);
	status_t status;
	{
		MutexLocker commandLocker(&info->commandLock);
		status = virtio_gpu_transfer_to_host_2d(info, CURSOR_RESOURCE_ID,
			VIRTIO_GPU_CURSOR_SIZE, VIRTIO_GPU_CURSOR_SIZE);
	}

	if (status == B_OK) {
		// UPDATE_CURSOR makes the host take the new image; resource 0 means
		// that the cursor is hidden, the next show takes it then
		MutexLocker cursorLocker(&info->cursorLock);
		info->cursor.hot_x = cursor->hot_x;
		info->cursor.hot_y = cursor->hot_y;
		if (info->cursor.resource_id != 0)
			virtio_gpu_send_cursor(info, VIRTIO_GPU_CMD_UPDATE_CURSOR);
	}
	free(cursor);
}


// Even when nothing reports a change, the whole framebuffer is copied this
// often: a safety net for whatever writes to it without telling us.
static const bigtime_t kFullUpdateInterval = 1000000;
// While clients write to the framebuffer on their own (BWindowScreen,
// BDirectWindow), the whole of it is copied this often.
static const bigtime_t kDirectUpdateInterval = 20000;
// Changes in quick succession are copied together, at most this often.
static const bigtime_t kMinUpdateInterval = 8000;
// When a cursor command the host was not ready for is tried again.
static const bigtime_t kCursorRetryInterval = 5000;


/*!	Has the update thread look for work. May be called from an interrupt. */
static void
virtio_gpu_wake_update_thread(virtio_gpu_driver_info* info)
{
	// one wake up is enough until the thread got to it
	if (atomic_get_and_set(&info->updateWakePending, 1) == 0)
		release_sem_etc(info->updateSem, 1, B_DO_NOT_RESCHEDULE);
}


/*!	Copies the parts of the framebuffer that app_server reports as changed
	(VIRTIO_GPU_FRAME_BUFFER_CHANGED) to the host, when they change, and
	handles the display events and the cursor commands the host was not ready
	for.
*/
status_t
virtio_update_thread(void *arg)
{
	virtio_gpu_driver_info* info = (virtio_gpu_driver_info*)arg;
	bigtime_t lastUpdate = 0;
	bigtime_t lastFullUpdate = 0;
	bool cursorPending = false;

	while (info->updateThreadRunning) {
		// a BWindowScreen opens the device a second time
		bool direct = atomic_get(&info->directClients) > 0 || info->openCount > 1;
		bigtime_t fullUpdate = lastFullUpdate
			+ (direct ? kDirectUpdateInterval : kFullUpdateInterval);
		bigtime_t timeout = fullUpdate;
		if (cursorPending)
			timeout = min_c(timeout, system_time() + kCursorRetryInterval);
		acquire_sem_etc(info->updateSem, 1, B_ABSOLUTE_TIMEOUT, timeout);
		atomic_set(&info->updateWakePending, 0);
		if (!info->updateThreadRunning)
			break;

		virtio_gpu_update_cursor_image(info);
		{
			MutexLocker cursorLocker(&info->cursorLock);
			virtio_gpu_send_cursor(info, 0);
			cursorPending = info->cursorPending != 0;
		}

		bool full = system_time() >= fullUpdate;
		if (!full) {
			// let a burst of changes come together
			bool dirty;
			{
				MutexLocker dirtyLocker(&info->dirtyLock);
				dirty = info->dirty.right >= info->dirty.left;
			}
			if (dirty && system_time() < lastUpdate + kMinUpdateInterval)
				snooze_until(lastUpdate + kMinUpdateInterval, B_SYSTEM_TIMEBASE);
		}

		virtio_gpu_dirty_rect rect;
		{
			MutexLocker dirtyLocker(&info->dirtyLock);
			rect = info->dirty;
			info->dirty.left = 0;
			info->dirty.right = -1;
		}
		if (full) {
			rect.left = rect.top = 0;
			rect.right = rect.bottom = INT32_MAX;
		}

		MutexLocker commandLocker(&info->commandLock);
		if (atomic_get_and_set(&info->displayEvent, 0) != 0)
			virtio_gpu_handle_display_event(info);

		// the mode may have changed since
		rect.right = min_c(rect.right, (int32)info->displayWidth - 1);
		rect.bottom = min_c(rect.bottom, (int32)info->displayHeight - 1);
		if (rect.right >= rect.left && rect.bottom >= rect.top) {
			uint32 width = rect.right - rect.left + 1;
			uint32 height = rect.bottom - rect.top + 1;
			uint64 offset = ((uint64)rect.top * info->displayWidth + rect.left) * 4;
			virtio_gpu_transfer_to_host_2d(info, info->displayResourceId, width, height,
				rect.left, rect.top, offset);
			virtio_gpu_flush_resource(info, info->displayResourceId, width, height,
				rect.left, rect.top);
			lastUpdate = system_time();
		}
		if (full)
			lastFullUpdate = system_time();
	}
	return B_OK;
}


status_t
virtio_gpu_set_display_mode(virtio_gpu_driver_info* info, display_mode *mode)
{
	CALLED();

	// every resource is backed by the one framebuffer area, so a mode must
	// fit into it (the host would otherwise read past its end)
	if (mode->virtual_width == 0 || mode->virtual_height == 0
		|| mode->virtual_width > info->framebufferWidth
		|| mode->virtual_height > info->framebufferHeight) {
		ERROR("set_display_mode: %" B_PRIu16 " x %" B_PRIu16 " does not fit the "
			"%" B_PRIu32 " x %" B_PRIu32 " framebuffer\n", mode->virtual_width,
			mode->virtual_height, info->framebufferWidth, info->framebufferHeight);
		return B_BAD_VALUE;
	}

	// Switch without ever leaving the display without a valid resource: set
	// up the new resource and show it first, and only then release the old
	// one. Until the new resource is on screen any failure only undoes the
	// new one, so the current mode stays exactly as it was.
	const int oldResourceId = info->displayResourceId;
	const int newResourceId = oldResourceId + 1;
	const uint32 width = mode->virtual_width;
	const uint32 height = mode->virtual_height;

	TRACE("virtio_gpu_set_display_mode %" B_PRIu32 " %" B_PRIu32 "\n", width,
		height);

	status_t status = virtio_gpu_create_2d(info, newResourceId, width, height);
	if (status != B_OK)
		return status;

	status = virtio_gpu_attach_backing(info, newResourceId, info->framebuffer,
		info->framebufferSize);
	if (status != B_OK)
		goto err1;

	// the new resource's content, before it is shown
	status = virtio_gpu_transfer_to_host_2d(info, newResourceId, width, height);
	if (status != B_OK)
		goto err2;

	status = virtio_gpu_set_scanout(info, 0, newResourceId, width, height);
	if (status != B_OK)
		goto err2;

	// the new mode is on screen: from here on there is no going back
	info->displayResourceId = newResourceId;
	info->displayWidth = width;
	info->displayHeight = height;
	{
		virtio_gpu_shared_info& sharedInfo = *info->sharedInfo;
		sharedInfo.frame_buffer_area = info->framebufferArea;
		sharedInfo.frame_buffer = (uint8*)info->framebuffer;
		sharedInfo.bytes_per_row = width * 4;
		sharedInfo.current_mode.virtual_width = width;
		sharedInfo.current_mode.virtual_height = height;
		sharedInfo.current_mode.space = B_RGB32;
		sharedInfo.current_mode.timing = mode->timing;
	}

	// failures below only cost host memory or a frame; the update thread
	// flushes again shortly
	virtio_gpu_flush_resource(info, newResourceId, width, height);
	virtio_gpu_detach_backing(info, oldResourceId);
	virtio_gpu_unref(info, oldResourceId);
	return B_OK;

err2:
	virtio_gpu_detach_backing(info, newResourceId);
err1:
	virtio_gpu_unref(info, newResourceId);
	return status;
}


//	#pragma mark - device module API


static status_t
virtio_gpu_init_device(void* _info, void** _cookie)
{
	CALLED();
	virtio_gpu_driver_info* info = (virtio_gpu_driver_info*)_info;

	device_node* parent = sDeviceManager->get_parent_node(info->node);
	sDeviceManager->get_driver(parent, (driver_module_info**)&info->virtio,
		(void**)&info->virtio_device);
	sDeviceManager->put_node(parent);

	info->virtio->negotiate_features(info->virtio_device, VIRTIO_GPU_F_EDID,
		 &info->features, &get_feature_name);

	// TODO read config

	// Setup queues
	::virtio_queue virtioQueues[2];
	status_t status = info->virtio->alloc_queues(info->virtio_device, 2,
		virtioQueues, NULL);
	if (status != B_OK) {
		ERROR("queue allocation failed (%s)\n", strerror(status));
		return status;
	}

	info->controlQueue = virtioQueues[0];
	info->cursorQueue = virtioQueues[1];

	// create command buffer area
	info->commandArea = create_area("virtiogpu command buffer", (void**)&info->commandBuffer,
		B_ANY_KERNEL_BLOCK_ADDRESS, B_PAGE_SIZE,
		B_FULL_LOCK, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (info->commandArea < B_OK) {
		status = info->commandArea;
		goto err1;
	}

	physical_entry entry;
	status = get_memory_map((void*)info->commandBuffer, B_PAGE_SIZE, &entry, 1);
	if (status != B_OK)
		goto err2;

	info->commandPhysAddr = entry.address;
	mutex_init(&info->commandLock, "virtiogpu command lock");
	mutex_init(&info->cursorLock, "virtiogpu cursor lock");

	// Released by the control queue interrupt. It belongs to the device,
	// not to an open handle: any handle's commands (and the update thread's)
	// wait on it.
	info->commandDone = create_sem(1, "virtio_gpu_command");
	if (info->commandDone < B_OK) {
		status = info->commandDone;
		goto err3;
	}

	// Setup interrupt
	status = info->virtio->setup_interrupt(info->virtio_device,
		virtio_gpu_config_changed, info);
	if (status != B_OK) {
		ERROR("interrupt setup failed (%s)\n", strerror(status));
		goto err4;
	}

	status = info->virtio->queue_setup_interrupt(info->controlQueue,
		virtio_gpu_vqwait, info);
	if (status != B_OK) {
		ERROR("queue interrupt setup failed (%s)\n", strerror(status));
		goto err5;
	}

	*_cookie = info;
	return B_OK;

err5:
	info->virtio->free_interrupts(info->virtio_device);
err4:
	delete_sem(info->commandDone);
	info->commandDone = -1;
err3:
	mutex_destroy(&info->commandLock);
	mutex_destroy(&info->cursorLock);
err2:
	delete_area(info->commandArea);
	info->commandArea = -1;
err1:
	info->virtio->free_queues(info->virtio_device);
	return status;
}


static void
virtio_gpu_uninit_device(void* _cookie)
{
	CALLED();
	virtio_gpu_driver_info* info = (virtio_gpu_driver_info*)_cookie;

	info->virtio->free_interrupts(info->virtio_device);

	delete_sem(info->commandDone);
	info->commandDone = -1;
	mutex_destroy(&info->commandLock);
	mutex_destroy(&info->cursorLock);

	delete_area(info->commandArea);
	info->commandArea = -1;
	info->virtio->free_queues(info->virtio_device);
}


/*!	Undoes virtio_gpu_setup_display(), also a partial one. Called with the
	openLock held, when the last handle goes away (or the first open fails).
*/
static void
virtio_gpu_teardown_display(virtio_gpu_driver_info* info)
{
	CALLED();

	// stop the update thread first: it uses the resource and the framebuffer
	if (info->updateThread >= 0) {
		info->updateThreadRunning = false;
		release_sem(info->updateSem);
		status_t result;
		wait_for_thread(info->updateThread, &result);
		info->updateThread = -1;
	}

	// Hide the cursor, and give the host a moment to finish the cursor
	// queue, whose command lives in the cursor area.
	bool cursorDone = true;
	if (info->cursorArea >= 0) {
		MutexLocker cursorLocker(&info->cursorLock);
		info->cursor.resource_id = 0;
		virtio_gpu_send_cursor(info, VIRTIO_GPU_CMD_UPDATE_CURSOR);
		for (int32 i = 0; i < 100 && (info->cursorPending != 0
				|| !info->virtio->queue_is_empty(info->cursorQueue)); i++) {
			snooze(1000);
			virtio_gpu_send_cursor(info, 0);
		}
		cursorDone = info->cursorPending == 0
			&& info->virtio->queue_is_empty(info->cursorQueue);
	}

	// Release the cursor resource, so that the next first open can create it
	// again, before freeing the image the host reads.
	if (info->cursorArea >= 0 && cursorDone) {
		MutexLocker commandLocker(&info->commandLock);
		cursorDone = virtio_gpu_unref(info, CURSOR_RESOURCE_ID) == B_OK;
	}

	if (info->displayResourceId > 0) {
		MutexLocker commandLocker(&info->commandLock);
		virtio_gpu_set_scanout(info, 0, 0, 0, 0);
		virtio_gpu_detach_backing(info, info->displayResourceId);
		virtio_gpu_unref(info, info->displayResourceId);
		info->displayResourceId = 0;
	}
	virtio_gpu_drain_queues(info);

	delete_area(info->framebufferArea);
	info->framebufferArea = -1;
	info->framebuffer = 0;
	// the host reads the cursor image: keep it if the host did not let go
	if (cursorDone)
		delete_area(info->cursorArea);
	else
		ERROR("teardown: the host may still use the cursor, keeping it\n");
	info->cursorArea = -1;
	free(info->cursorNewImage);
	info->cursorNewImage = NULL;
	delete_area(info->sharedArea);
	info->sharedArea = -1;
	info->sharedInfo = NULL;
	delete_sem(info->displayChangeSem);
	info->displayChangeSem = -1;
	delete_sem(info->updateSem);
	info->updateSem = -1;
}


/*!	Sets up the display for the first open: the area shared with the
	accelerant, the framebuffer, the host resource and scanout, and the update
	thread. Called with the openLock held.
*/
static status_t
virtio_gpu_setup_display(virtio_gpu_driver_info* info)
{
	CALLED();
	size_t sharedSize = (sizeof(virtio_gpu_shared_info) + 7) & ~7;
	status_t status;
	bool enabled;

	info->sharedArea = create_area("virtio_gpu shared info",
		(void**)&info->sharedInfo, B_ANY_KERNEL_ADDRESS,
		ROUND_TO_PAGE_SIZE(sharedSize), B_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_CLONEABLE_AREA);
	if (info->sharedArea < 0)
		return info->sharedArea;
	memset(info->sharedInfo, 0, sizeof(virtio_gpu_shared_info));

	{
		MutexLocker commandLocker(&info->commandLock);

		// so we can fit every mode
		info->framebufferWidth = 3840;
		info->framebufferHeight = 2160;
		info->sharedInfo->max_width = info->framebufferWidth;
		info->sharedInfo->max_height = info->framebufferHeight;

		status = virtio_gpu_get_display_info(info, info->displayWidth,
			info->displayHeight, enabled);
		if (status == B_OK && !enabled) {
			ERROR("pmodes[0] is not enabled\n");
			status = B_BAD_VALUE;
		}
		if (status != B_OK)
			goto error;
		if (info->displayWidth < MIN_DISPLAY_WIDTH
			|| info->displayHeight < MIN_DISPLAY_HEIGHT) {
			// not a usable size, start with a standard one
			info->displayWidth = 1024;
			info->displayHeight = 768;
		}
		info->displayWidth = min_c(info->displayWidth, info->framebufferWidth);
		info->displayHeight = min_c(info->displayHeight,
			info->framebufferHeight);
		info->sharedInfo->preferred_width = info->displayWidth;
		info->sharedInfo->preferred_height = info->displayHeight;

		if ((info->features & VIRTIO_GPU_F_EDID) != 0)
			virtio_gpu_get_edids(info, 0);

		// create framebuffer area
		info->framebufferSize = 4 * info->framebufferWidth * info->framebufferHeight;
		info->framebufferArea = create_area("virtio_gpu framebuffer",
			(void**)&info->framebuffer, B_ANY_KERNEL_ADDRESS, info->framebufferSize,
			B_FULL_LOCK | B_CONTIGUOUS, B_READ_AREA | B_WRITE_AREA);
		if (info->framebufferArea < B_OK) {
			status = info->framebufferArea;
			goto error;
		}

		status = virtio_gpu_create_2d(info, 1, info->displayWidth,
			info->displayHeight);
		if (status != B_OK)
			goto error;
		info->displayResourceId = 1;

		status = virtio_gpu_attach_backing(info, info->displayResourceId,
			info->framebuffer, info->framebufferSize);
		if (status != B_OK)
			goto error;

		status = virtio_gpu_set_scanout(info, 0, info->displayResourceId,
			info->displayWidth, info->displayHeight);
		if (status != B_OK)
			goto error;
	}

	{
		virtio_gpu_shared_info& sharedInfo = *info->sharedInfo;
		sharedInfo.frame_buffer_area = info->framebufferArea;
		sharedInfo.frame_buffer = (uint8*)info->framebuffer;
		sharedInfo.bytes_per_row = info->displayWidth * 4;
		sharedInfo.current_mode.virtual_width = info->displayWidth;
		sharedInfo.current_mode.virtual_height = info->displayHeight;
		sharedInfo.current_mode.space = B_RGB32;
	}

	// The cursor is optional: without it, app_server draws it in the framebuffer.
	{
		MutexLocker commandLocker(&info->commandLock);
		info->cursorArea = create_area("virtio_gpu cursor", (void**)&info->cursorImage,
			B_ANY_KERNEL_ADDRESS, ROUND_TO_PAGE_SIZE(CURSOR_IMAGE_SIZE + sizeof(info->cursor)),
			B_FULL_LOCK | B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
		if (info->cursorArea >= B_OK) {
			physical_entry entry;
			status = get_memory_map((void*)info->cursorImage, B_PAGE_SIZE, &entry, 1);
			if (status == B_OK) {
				status = virtio_gpu_create_2d(info, CURSOR_RESOURCE_ID, VIRTIO_GPU_CURSOR_SIZE,
					VIRTIO_GPU_CURSOR_SIZE, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM);
			}
			if (status == B_OK) {
				status = virtio_gpu_attach_backing(info, CURSOR_RESOURCE_ID, info->cursorImage,
					CURSOR_IMAGE_SIZE);
				if (status != B_OK)
					virtio_gpu_unref(info, CURSOR_RESOURCE_ID);
			}
			if (status != B_OK) {
				delete_area(info->cursorArea);
				info->cursorArea = -1;
			} else
				info->cursorPhysAddr = entry.address;
		}
	}
	memset(&info->cursor, 0, sizeof(info->cursor));
	info->cursorPending = 0;
	info->cursorNewImage = NULL;

	info->dirty.left = 0;
	info->dirty.right = -1;
	info->directClients = 0;
	info->updateWakePending = 0;
	info->updateSem = create_sem(0, "virtio_gpu update");
	if (info->updateSem < B_OK) {
		status = info->updateSem;
		goto error;
	}

	info->displayEvent = 1;
		// acknowledges an event still pending from before we opened
	info->updateThreadRunning = true;
	info->updateThread = spawn_kernel_thread(virtio_update_thread,
		"virtio_gpu update", B_DISPLAY_PRIORITY, info);
	if (info->updateThread < B_OK) {
		status = info->updateThread;
		info->updateThread = -1;
		goto error;
	}
	resume_thread(info->updateThread);
	return B_OK;

error:
	virtio_gpu_teardown_display(info);
	return status;
}


static status_t
virtio_gpu_open(void* _info, const char* path, int openMode, void** _cookie)
{
	CALLED();
	virtio_gpu_driver_info* info = (virtio_gpu_driver_info*)_info;

	virtio_gpu_handle* handle = (virtio_gpu_handle*)malloc(
		sizeof(virtio_gpu_handle));
	if (handle == NULL)
		return B_NO_MEMORY;
	handle->info = info;

	// app_server opens the device once; accelerant clones (BWindowScreen)
	// open it again and must share the display, not set up a new one
	MutexLocker openLocker(&info->openLock);
	if (info->openCount == 0) {
		status_t status = virtio_gpu_setup_display(info);
		if (status != B_OK) {
			ERROR("open: display setup failed: %s\n", strerror(status));
			free(handle);
			return status;
		}
	}
	info->openCount++;
	if (info->openCount > 1)
		virtio_gpu_wake_update_thread(info);

	*_cookie = handle;
	return B_OK;
}


static status_t
virtio_gpu_close(void* cookie)
{
	CALLED();
	// nothing to do per handle: the display is shared and torn down in free
	// once the last handle is gone
	return B_OK;
}


static status_t
virtio_gpu_free(void* cookie)
{
	CALLED();
	virtio_gpu_handle* handle = (virtio_gpu_handle*)cookie;
	virtio_gpu_driver_info* info = handle->info;

	MutexLocker openLocker(&info->openLock);
	if (--info->openCount == 0)
		virtio_gpu_teardown_display(info);

	free(handle);
	return B_OK;
}


/*!	Config change interrupt: only flags it, the update thread queries the
	device, as that needs the command lock.
*/
static void
virtio_gpu_config_changed(void* cookie)
{
	virtio_gpu_driver_info* info = (virtio_gpu_driver_info*)cookie;
	atomic_set(&info->displayEvent, 1);
	virtio_gpu_wake_update_thread(info);
}


static void
virtio_gpu_vqwait(void* driverCookie, void* cookie)
{
	CALLED();
	virtio_gpu_driver_info* info = (virtio_gpu_driver_info*)cookie;

	release_sem_etc(info->commandDone, 1, B_DO_NOT_RESCHEDULE);
}


static status_t
virtio_gpu_read(void* cookie, off_t pos, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
virtio_gpu_write(void* cookie, off_t pos, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
virtio_gpu_ioctl(void* cookie, uint32 op, void* buffer, size_t length)
{
	CALLED();
	virtio_gpu_handle* handle = (virtio_gpu_handle*)cookie;
	virtio_gpu_driver_info* info = handle->info;

	// TRACE("ioctl(op = %lx)\n", op);

	switch (op) {
		case B_GET_ACCELERANT_SIGNATURE:
			dprintf(DEVICE_NAME ": acc: %s\n", ACCELERANT_NAME);
			if (user_strlcpy((char*)buffer, ACCELERANT_NAME,
					B_FILE_NAME_LENGTH) < B_OK)
				return B_BAD_ADDRESS;

			return B_OK;

		// needed to share data between kernel and accelerant
		case VIRTIO_GPU_GET_PRIVATE_DATA:
			return user_memcpy(buffer, &info->sharedArea, sizeof(area_id));

		// the accelerant's clone info: the device path below /dev, which
		// a clone opens to share this display
		case VIRTIO_GPU_GET_DEVICE_NAME:
			if (user_strlcpy((char*)buffer, info->deviceName,
					B_PATH_NAME_LENGTH) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;

		// For the accelerant's ACCELERANT_DISPLAY_CHANGE_SEMAPHORE hook, which
		// only app_server calls. Userland may not use semaphores of the kernel
		// team, so the semaphore belongs to the caller's team; when that team
		// is gone (app_server restarted), so is the semaphore, and the next
		// caller gets a new one. Only we delete it, on the last free.
		case VIRTIO_GPU_GET_DISPLAY_CHANGE_SEM:
		{
			if (length != sizeof(sem_id))
				return B_BAD_VALUE;

			MutexLocker commandLocker(&info->commandLock);
			int32 count;
			if (get_sem_count(info->displayChangeSem, &count) != B_OK) {
				sem_id sem = create_sem(0, "virtio_gpu display change");
				if (sem < B_OK)
					return sem;
				status_t status = set_sem_owner(sem,
					team_get_current_team_id());
				if (status != B_OK) {
					delete_sem(sem);
					return status;
				}
				info->displayChangeSem = sem;
			}
			return user_memcpy(buffer, &info->displayChangeSem, sizeof(sem_id));
		}

		case VIRTIO_GPU_SET_DISPLAY_MODE:
		{
			if (length != sizeof(display_mode))
				return B_BAD_VALUE;

			display_mode mode;
			if (user_memcpy(&mode, buffer, sizeof(display_mode)) != B_OK)
				return B_BAD_ADDRESS;

			MutexLocker commandLocker(&info->commandLock);

			return virtio_gpu_set_display_mode(info, &mode);
		}

		case VIRTIO_GPU_SET_CURSOR:
		{
			if (info->cursorArea < 0)
				return B_UNSUPPORTED;
			if (length != sizeof(virtio_gpu_cursor_image))
				return B_BAD_VALUE;

			virtio_gpu_cursor_image* cursor
				= (virtio_gpu_cursor_image*)malloc(sizeof(virtio_gpu_cursor_image));
			if (cursor == NULL)
				return B_NO_MEMORY;
			status_t status = user_memcpy(cursor, buffer, sizeof(*cursor));
			if (status != B_OK)
				status = B_BAD_ADDRESS;
			else if (cursor->hot_x >= VIRTIO_GPU_CURSOR_SIZE
				|| cursor->hot_y >= VIRTIO_GPU_CURSOR_SIZE)
				status = B_BAD_VALUE;
			else {
				// the update thread hands it to the host, replacing one it
				// did not get to yet
				MutexLocker cursorLocker(&info->cursorLock);
				virtio_gpu_cursor_image* previous = info->cursorNewImage;
				info->cursorNewImage = cursor;
				cursor = previous;
			}
			free(cursor);
			return status;
		}

		case VIRTIO_GPU_MOVE_CURSOR:
		{
			virtio_gpu_cursor_position position;
			if (info->cursorArea < 0)
				return B_UNSUPPORTED;
			if (length != sizeof(position))
				return B_BAD_VALUE;
			if (user_memcpy(&position, buffer, sizeof(position)) != B_OK)
				return B_BAD_ADDRESS;

			MutexLocker cursorLocker(&info->cursorLock);
			info->cursor.pos.x = position.x;
			info->cursor.pos.y = position.y;
			status_t status = virtio_gpu_send_cursor(info, VIRTIO_GPU_CMD_MOVE_CURSOR);
			if (info->cursorPending != 0)
				virtio_gpu_wake_update_thread(info);
			return status;
		}

		case VIRTIO_GPU_SHOW_CURSOR:
		{
			bool visible;
			if (info->cursorArea < 0)
				return B_UNSUPPORTED;
			if (length != sizeof(visible))
				return B_BAD_VALUE;
			if (user_memcpy(&visible, buffer, sizeof(visible)) != B_OK)
				return B_BAD_ADDRESS;

			// resource 0 hides the cursor
			MutexLocker cursorLocker(&info->cursorLock);
			info->cursor.resource_id = visible ? CURSOR_RESOURCE_ID : 0;
			status_t status = virtio_gpu_send_cursor(info, VIRTIO_GPU_CMD_UPDATE_CURSOR);
			if (info->cursorPending != 0)
				virtio_gpu_wake_update_thread(info);
			return status;
		}

		case VIRTIO_GPU_FRAME_BUFFER_CHANGED:
		{
			virtio_gpu_dirty_rect rect;
			if (length != sizeof(rect))
				return B_BAD_VALUE;
			if (user_memcpy(&rect, buffer, sizeof(rect)) != B_OK)
				return B_BAD_ADDRESS;

			// the update thread clips it to the display
			rect.left = max_c(rect.left, 0);
			rect.top = max_c(rect.top, 0);
			if (rect.right < rect.left || rect.bottom < rect.top)
				return B_OK;

			{
				MutexLocker dirtyLocker(&info->dirtyLock);
				virtio_gpu_dirty_rect& dirty = info->dirty;
				if (dirty.right < dirty.left)
					dirty = rect;
				else {
					// shortcut: one bounding rectangle, so two small changes
					// far apart copy all in between; keep several rectangles
					// if that shows up in the host's load
					dirty.left = min_c(dirty.left, rect.left);
					dirty.top = min_c(dirty.top, rect.top);
					dirty.right = max_c(dirty.right, rect.right);
					dirty.bottom = max_c(dirty.bottom, rect.bottom);
				}
			}
			virtio_gpu_wake_update_thread(info);
			return B_OK;
		}

		case VIRTIO_GPU_DIRECT_CLIENT:
		{
			bool added;
			if (length != sizeof(added))
				return B_BAD_VALUE;
			if (user_memcpy(&added, buffer, sizeof(added)) != B_OK)
				return B_BAD_ADDRESS;

			atomic_add(&info->directClients, added ? 1 : -1);
			virtio_gpu_wake_update_thread(info);
			return B_OK;
		}
		default:
			ERROR("ioctl: unknown message %" B_PRIx32 "\n", op);
			break;
	}


	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - driver module API


static float
virtio_gpu_supports_device(device_node* parent)
{
	CALLED();
	const char* bus;
	uint16 deviceType;

	// make sure parent is really the Virtio bus manager
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false))
		return -1;

	if (strcmp(bus, "virtio"))
		return 0.0;

	// check whether it's really a Virtio GPU device
	if (sDeviceManager->get_attr_uint16(parent, VIRTIO_DEVICE_TYPE_ITEM,
			&deviceType, true) != B_OK || deviceType != VIRTIO_DEVICE_ID_GPU)
		return 0.0;

	TRACE("Virtio gpu device found!\n");

	return 0.6;
}


static status_t
virtio_gpu_register_device(device_node* node)
{
	CALLED();

	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "Virtio GPU"} },
		{ NULL }
	};

	return sDeviceManager->register_node(node, VIRTIO_GPU_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
virtio_gpu_init_driver(device_node* node, void** cookie)
{
	CALLED();

	virtio_gpu_driver_info* info = (virtio_gpu_driver_info*)malloc(
		sizeof(virtio_gpu_driver_info));
	if (info == NULL)
		return B_NO_MEMORY;

	memset(info, 0, sizeof(*info));

	info->framebufferArea = -1;
	info->cursorArea = -1;
	info->sharedArea = -1;
	info->commandArea = -1;
	info->commandDone = -1;
	info->updateThread = -1;
	info->displayChangeSem = -1;
	info->updateSem = -1;
	mutex_init(&info->openLock, "virtio_gpu open lock");
	mutex_init(&info->dirtyLock, "virtio_gpu dirty lock");

	info->node = node;

	*cookie = info;
	return B_OK;
}


static void
virtio_gpu_uninit_driver(void* _cookie)
{
	CALLED();
	virtio_gpu_driver_info* info = (virtio_gpu_driver_info*)_cookie;
	mutex_destroy(&info->openLock);
	mutex_destroy(&info->dirtyLock);
	free(info);
}


static status_t
virtio_gpu_register_child_devices(void* _cookie)
{
	CALLED();
	virtio_gpu_driver_info* info = (virtio_gpu_driver_info*)_cookie;
	status_t status;

	int32 id = sDeviceManager->create_id(VIRTIO_GPU_DEVICE_ID_GENERATOR);
	if (id < 0)
		return id;

	snprintf(info->deviceName, sizeof(info->deviceName),
		"graphics/virtio/%" B_PRId32, id);

	status = sDeviceManager->publish_device(info->node, info->deviceName,
		VIRTIO_GPU_DEVICE_MODULE_NAME);
	if (status != B_OK) {
		sDeviceManager->free_id(VIRTIO_GPU_DEVICE_ID_GENERATOR, id);
		info->deviceName[0] = '\0';
	}

	return status;
}


//	#pragma mark -


module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager},
	{}
};

struct device_module_info sVirtioGpuDevice = {
	{
		VIRTIO_GPU_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	virtio_gpu_init_device,
	virtio_gpu_uninit_device,
	NULL, // remove,

	virtio_gpu_open,
	virtio_gpu_close,
	virtio_gpu_free,
	virtio_gpu_read,
	virtio_gpu_write,
	NULL,	// io
	virtio_gpu_ioctl,

	NULL,	// select
	NULL,	// deselect
};

struct driver_module_info sVirtioGpuDriver = {
	{
		VIRTIO_GPU_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	virtio_gpu_supports_device,
	virtio_gpu_register_device,
	virtio_gpu_init_driver,
	virtio_gpu_uninit_driver,
	virtio_gpu_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};

module_info* modules[] = {
	(module_info*)&sVirtioGpuDriver,
	(module_info*)&sVirtioGpuDevice,
	NULL
};
