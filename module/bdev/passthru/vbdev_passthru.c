/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * This is a simple example of a virtual block device module that passes IO
 * down to a bdev (or bdevs) that its configured to attach to.
 */

#include "spdk/stdinc.h"

#include "vbdev_passthru.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/endian.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"

#include "spdk/bdev_module.h"
#include "spdk/log.h"

/* This namespace UUID was generated using uuid_generate() method. */
#define BDEV_PASSTHRU_NAMESPACE_UUID "7e25812e-c8c0-4d3f-8599-16d790555b85"
#define SPDK_PAGE_SIZE 0x1000
static int vbdev_passthru_init(void);
static int vbdev_passthru_get_ctx_size(void);
static void vbdev_passthru_examine(struct spdk_bdev *bdev);
static void vbdev_passthru_finish(void);
static int vbdev_passthru_config_json(struct spdk_json_write_ctx *w);

static struct spdk_bdev_module passthru_if = {
	.name = "passthru",
	.module_init = vbdev_passthru_init,
	.get_ctx_size = vbdev_passthru_get_ctx_size,
	.examine_config = vbdev_passthru_examine,
	.module_fini = vbdev_passthru_finish,
	.config_json = vbdev_passthru_config_json
};

SPDK_BDEV_MODULE_REGISTER(passthru, &passthru_if)
#define MAX_MD_ALLOC 4096

/* List of pt_bdev names and their base bdevs via configuration file.
 * Used so we can parse the conf once at init and use this list in examine().
 */
struct bdev_names {
	char			*vbdev_name;
	char			*bdev_name;
	struct spdk_uuid	uuid;
	uint32_t block_sz;
	uint32_t md_sz;
	uint32_t mode;
	TAILQ_ENTRY(bdev_names)	link;
};
static TAILQ_HEAD(, bdev_names) g_bdev_names = TAILQ_HEAD_INITIALIZER(g_bdev_names);

/* List of virtual bdevs and associated info for each. */
struct vbdev_passthru {
	struct spdk_bdev		*base_bdev; /* the thing we're attaching to */
	struct spdk_bdev_desc		*base_desc; /* its descriptor we get from open */
	struct spdk_bdev		pt_bdev;    /* the PT virtual bdev */
	struct spdk_io_channel		*md_channel;
	struct spdk_spinlock		used_lock;
	uint64_t multiplier;
	uint32_t md_len;
	bool mode;
	void				*malloc_md_buf;
	uint64_t offset_start;
	TAILQ_ENTRY(vbdev_passthru)	link;
	struct spdk_thread		*thread;    /* thread where base device is opened */
};
static TAILQ_HEAD(, vbdev_passthru) g_pt_nodes = TAILQ_HEAD_INITIALIZER(g_pt_nodes);

/* The pt vbdev channel struct. It is allocated and freed on my behalf by the io channel code.
 * If this vbdev needed to implement a poller or a queue for IO, this is where those things
 * would be defined. This passthru bdev doesn't actually need to allocate a channel, it could
 * simply pass back the channel of the bdev underneath it but for example purposes we will
 * present its own to the upper layers.
 */
struct pt_io_channel {
	struct spdk_io_channel	*base_ch; /* IO channel of base device */
};

/* Just for fun, this pt_bdev module doesn't need it but this is essentially a per IO
 * context that we get handed by the bdev layer.
 */
struct passthru_bdev_io {
	uint8_t test;

	/* bdev related */
	struct spdk_io_channel *ch;

	/* for bdev_io_wait */
	struct spdk_bdev_io_wait_entry bdev_io_wait;
};

struct arg_requst {
	struct spdk_bdev_io *bdev_io;
	void *buf;
};

static void vbdev_passthru_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);


/* Callback for unregistering the IO device. */
static void
_device_unregister_cb(void *io_device)
{
	struct vbdev_passthru *pt_node  = io_device;

	/* Done with this pt_node. */
	free(pt_node->pt_bdev.name);
	free(pt_node);
}

/* Wrapper for the bdev close operation. */
static void
_vbdev_passthru_destruct(void *ctx)
{
	struct spdk_bdev_desc *desc = ctx;

	spdk_bdev_close(desc);
}

// static void write_buffer_to_file(const void *buffer, size_t size, bool mode) {
//     // Hard-coded file name
// 	const char *file_name = "/root/sadegh.bin";
// 	if(mode)
//     	file_name = "/root/sadegh_1.bin";
    
//     // Open the file in binary write mode
//     FILE *file = fopen(file_name, "wb");
//     if (file == NULL) {
//         perror("Failed to open file");
//         return;
//     }
//     size_t CHUNK_SIZE = 1024 * 1024;
//     // Write the buffer to the file in chunks
//     const unsigned char *buf_ptr = (const unsigned char *)buffer;
//     size_t remaining = size;
//     while (remaining > 0) {
//         size_t chunk_size = remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE;
//         size_t written = fwrite(buf_ptr, 1, chunk_size, file);
//         if (written != chunk_size) {
//             perror("Failed to write the entire chunk to file");
//             break;
//         }
//         remaining -= written;
//         buf_ptr += written;
//     }

//     if (remaining == 0) {
//         printf("Buffer successfully written to %s\n", file_name);
//     } else {
//         printf("Failed to write the entire buffer to %s\n", file_name);
//     }

//     // Close the file
//     fclose(file);
// }


/* Called after we've unregistered following a hot remove callback.
 * Our finish entry point will be called next.
 */
static int
vbdev_passthru_destruct(void *ctx)
{
	struct vbdev_passthru *pt_node = (struct vbdev_passthru *)ctx;

	/* It is important to follow this exact sequence of steps for destroying
	 * a vbdev...
	 */
	// write_buffer_to_file(pt_node->malloc_md_buf, (pt_node->offset_start * pt_node->pt_bdev.blocklen), pt_node->mode);
	TAILQ_REMOVE(&g_pt_nodes, pt_node, link);

	/* Unclaim the underlying bdev. */
	spdk_bdev_module_release_bdev(pt_node->base_bdev);

	/* Close the underlying bdev on its same opened thread. */
	if (pt_node->thread && pt_node->thread != spdk_get_thread()) {
		spdk_thread_send_msg(pt_node->thread, _vbdev_passthru_destruct, pt_node->base_desc);
	} else {
		spdk_bdev_close(pt_node->base_desc);
	}

	/* Unregister the io_device. */
	spdk_io_device_unregister(pt_node, _device_unregister_cb);

	return 0;
}

/* Completion callback for IO that were issued from this bdev. The original bdev_io
 * is passed in as an arg so we'll complete that one with the appropriate status
 * and then free the one that this module issued.
 */
static void
_pt_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	struct passthru_bdev_io *io_ctx = (struct passthru_bdev_io *)orig_io->driver_ctx;

	/* We setup this value in the submission routine, just showing here that it is
	 * passed back to us.
	 */
	if (io_ctx->test != 0x5a) {
		SPDK_ERRLOG("Error, original IO device_ctx is wrong! 0x%x\n",
			    io_ctx->test);
	}

	if(status == SPDK_BDEV_IO_STATUS_FAILED){
		SPDK_ERRLOG("error status on passthru bdev");
	}

	/* Complete the original IO and then free the one that we created here
	 * as a result of issuing an IO via submit_request.
	 */
	spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}

static void
_pt_complete_io_zero(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	// struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	// struct passthru_bdev_io *io_ctx = (struct passthru_bdev_io *)orig_io->driver_ctx;

	/* We setup this value in the submission routine, just showing here that it is
	 * passed back to us.
	 */
	// if (io_ctx->test != 0x5a) {
	// 	SPDK_ERRLOG("Error, original IO device_ctx is wrong! 0x%x\n",
	// 		    io_ctx->test);
	// }
	if(status == SPDK_BDEV_IO_STATUS_FAILED){
		SPDK_ERRLOG("error status on passthru bdev");
	}
	/* Complete the original IO and then free the one that we created here
	 * as a result of issuing an IO via submit_request.
	 */
	// spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}



static void
_pt_complete_zcopy_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	if(status == SPDK_BDEV_IO_STATUS_FAILED) {
		SPDK_ERRLOG("error status on passthru bdev");
	}
	struct passthru_bdev_io *io_ctx = (struct passthru_bdev_io *)orig_io->driver_ctx;

	/* We setup this value in the submission routine, just showing here that it is
	 * passed back to us.
	 */
	if (io_ctx->test != 0x5a) {
		SPDK_ERRLOG("Error, original IO device_ctx is wrong! 0x%x\n",
			    io_ctx->test);
	}

	/* Complete the original IO and then free the one that we created here
	 * as a result of issuing an IO via submit_request.
	 */
	spdk_bdev_io_set_buf(orig_io, bdev_io->u.bdev.iovs[0].iov_base, bdev_io->u.bdev.iovs[0].iov_len);
	spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}

static void
vbdev_passthru_resubmit_io(void *arg)
{
	struct spdk_bdev_io *bdev_io = (struct spdk_bdev_io *)arg;
	struct passthru_bdev_io *io_ctx = (struct passthru_bdev_io *)bdev_io->driver_ctx;

	vbdev_passthru_submit_request(io_ctx->ch, bdev_io);
}

static void
vbdev_passthru_queue_io(struct spdk_bdev_io *bdev_io)
{
	struct passthru_bdev_io *io_ctx = (struct passthru_bdev_io *)bdev_io->driver_ctx;
	struct pt_io_channel *pt_ch = spdk_io_channel_get_ctx(io_ctx->ch);
	int rc;

	io_ctx->bdev_io_wait.bdev = bdev_io->bdev;
	io_ctx->bdev_io_wait.cb_fn = vbdev_passthru_resubmit_io;
	io_ctx->bdev_io_wait.cb_arg = bdev_io;

	/* Queue the IO using the channel of the base device. */
	rc = spdk_bdev_queue_io_wait(bdev_io->bdev, pt_ch->base_ch, &io_ctx->bdev_io_wait);
	if (rc != 0) {
		SPDK_ERRLOG("Queue io failed in vbdev_passthru_queue_io, rc=%d.\n", rc);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}



static uint64_t
get_md_offset(struct spdk_bdev_io *bdev_io, uint32_t md_len)
{
	return bdev_io->u.bdev.offset_blocks * md_len;
}


static uint64_t
get_md_count(struct spdk_bdev_io *bdev_io, uint32_t md_len)
{
	return bdev_io->u.bdev.num_blocks * md_len;
}


static inline uint64_t
byte_to_lba(struct spdk_bdev_io *bdev_io, uint32_t md_len, uint64_t length)
{
	return (length / bdev_io->bdev->blocklen + ((length % bdev_io->bdev->blocklen) ? 1 : 0) + ((get_md_offset(bdev_io, md_len) % bdev_io->bdev->blocklen) ? 1 : 0));
}

static inline uint64_t
start_byte_to_lba(struct spdk_bdev_io *bdev_io, uint64_t offset)
{
	return offset / bdev_io->bdev->blocklen;
}

static void
_pt_complete_io6(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
		// struct spdk_bdev_io *orig_io = cb_arg;
	struct arg_requst *request = (struct arg_requst *)cb_arg;
	struct spdk_bdev_io *orig_io = request->bdev_io;
	// int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	struct passthru_bdev_io *io_ctx = (struct passthru_bdev_io *)orig_io->driver_ctx;
	// struct vbdev_passthru *pt_node = SPDK_CONTAINEROF(orig_io->bdev, struct vbdev_passthru, pt_bdev);

	/* We setup this value in the submission routine, just showing here that it is
	 * passed back to us.
	 */
	if (io_ctx->test != 0x5a) {
		SPDK_ERRLOG("Error, original IO device_ctx is wrong! 0x%x\n",
			    io_ctx->test);
	}


	if (request->buf) {
		spdk_free(request->buf);
		request->buf = NULL;
	}

    
    free(request);
	_pt_complete_io(bdev_io, success, orig_io);
	
}


static void
_pt_complete_io2(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	struct passthru_bdev_io *io_ctx = (struct passthru_bdev_io *)orig_io->driver_ctx;
	// struct pt_io_channel *pt_ch = spdk_io_channel_get_ctx(io_ctx->ch);
	struct vbdev_passthru *pt_node = SPDK_CONTAINEROF(orig_io->bdev, struct vbdev_passthru, pt_bdev);	
	uint64_t lb,lb_count;
	struct arg_requst *request;
	int rc;

	/* We setup this value in the submission routine, just showing here that it is
	 * passed back to us.
	 */
	if (io_ctx->test != 0x5a) {
		SPDK_ERRLOG("Error, original IO device_ctx is wrong! 0x%x\n",
			    io_ctx->test);
	}

	if (!success) {
        spdk_bdev_io_complete(orig_io, status);
		spdk_bdev_free_io(bdev_io);
        return;
    }

	if (orig_io->u.bdev.md_buf == NULL) {
		_pt_complete_io(bdev_io, success, cb_arg);
		return;
	}

	lb = start_byte_to_lba(orig_io, get_md_offset(orig_io, pt_node->md_len));
	lb_count = byte_to_lba(orig_io, pt_node->md_len, get_md_count(orig_io, pt_node->md_len));

	if(!lb_count) {
		lb_count = 1;
	}

	if(lb + lb_count >= pt_node->offset_start) {
		_pt_complete_io(bdev_io, SPDK_BDEV_IO_STATUS_FAILED, cb_arg);
		return;
	}

	request = calloc(1, sizeof(*request));
	if (request == NULL) {
		SPDK_ERRLOG("No memory,ERROR on bdev_io submission!\n");
		spdk_bdev_io_complete(orig_io, SPDK_BDEV_IO_STATUS_FAILED);
		spdk_bdev_free_io(bdev_io);	
		return;
	}
	request->buf = spdk_zmalloc(orig_io->bdev->blocklen * lb_count, 2 * 1024 * 1024, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!request->buf) {
		SPDK_ERRLOG("malloc_md_buf spdk_zmalloc() failed\n");
		spdk_bdev_io_complete(orig_io, SPDK_BDEV_IO_STATUS_FAILED);
		spdk_bdev_free_io(bdev_io);	
		return;
	}
	request->bdev_io = orig_io;	

	spdk_spin_lock(&pt_node->used_lock);
	memcpy(pt_node->malloc_md_buf + get_md_offset(orig_io, pt_node->md_len), orig_io->u.bdev.md_buf, get_md_count(orig_io, pt_node->md_len));
	memcpy(request->buf, pt_node->malloc_md_buf + (lb * orig_io->bdev->blocklen), (lb_count * orig_io->bdev->blocklen));
	spdk_spin_unlock(&pt_node->used_lock);
	struct pt_io_channel *channel = spdk_io_channel_get_ctx(pt_node->md_channel);
	rc = spdk_bdev_write_blocks(pt_node->base_desc, channel->base_ch,
		        request->buf, lb * pt_node->multiplier, lb_count * pt_node->multiplier,
		       _pt_complete_io6, request);

	if (rc != 0) {
		if (request->buf) {
			spdk_free(request->buf);
			request->buf = NULL;
		}	
		free(request);
		SPDK_ERRLOG("reading md blocks, ERROR on bdev_io submission!\n");
		spdk_bdev_io_complete(orig_io, SPDK_BDEV_IO_STATUS_FAILED);		
	}
	spdk_bdev_free_io(bdev_io);	
}


static void
_pt_complete_io4(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	struct passthru_bdev_io *io_ctx = (struct passthru_bdev_io *)orig_io->driver_ctx;
	struct vbdev_passthru *pt_node = SPDK_CONTAINEROF(orig_io->bdev, struct vbdev_passthru, pt_bdev);	


	/* We setup this value in the submission routine, just showing here that it is
	 * passed back to us.
	 */
	if (io_ctx->test != 0x5a) {
		SPDK_ERRLOG("Error, original IO device_ctx is wrong! 0x%x\n",
			    io_ctx->test);
	}

	if (!success) {
        spdk_bdev_io_complete(orig_io, status);
		spdk_bdev_free_io(bdev_io);
        return;
    }

	if (orig_io->u.bdev.md_buf == NULL) {
		_pt_complete_io(bdev_io, success, cb_arg);
		return;
	}

	memcpy(orig_io->u.bdev.md_buf, pt_node->malloc_md_buf + (get_md_offset(orig_io, pt_node->md_len)), get_md_count(orig_io, pt_node->md_len));

	_pt_complete_io(bdev_io, success, cb_arg);
}

static void
pt_init_ext_io_opts(struct spdk_bdev_io *bdev_io, struct spdk_bdev_ext_io_opts *opts)
{
	memset(opts, 0, sizeof(*opts));
	opts->size = sizeof(*opts);
	opts->memory_domain = bdev_io->u.bdev.memory_domain;
	opts->memory_domain_ctx = bdev_io->u.bdev.memory_domain_ctx;
	opts->metadata = NULL;//bdev_io->u.bdev.md_buf;
}

/* Callback for getting a buf from the bdev pool in the event that the caller passed
 * in NULL, we need to own the buffer so it doesn't get freed by another vbdev module
 * beneath us before we're done with it. That won't happen in this example but it could
 * if this example were used as a template for something more complex.
 */
static void
pt_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	struct vbdev_passthru *pt_node = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_passthru,
					 pt_bdev);
	struct pt_io_channel *pt_ch = spdk_io_channel_get_ctx(ch);
	struct passthru_bdev_io *io_ctx = (struct passthru_bdev_io *)bdev_io->driver_ctx;
	struct spdk_bdev_ext_io_opts io_opts;
	int rc;

	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	
	switch (bdev_io->type) {
		case SPDK_BDEV_IO_TYPE_READ:
		pt_init_ext_io_opts(bdev_io, &io_opts);
			rc = spdk_bdev_readv_blocks_ext(pt_node->base_desc, pt_ch->base_ch, bdev_io->u.bdev.iovs,
							bdev_io->u.bdev.iovcnt, (bdev_io->u.bdev.offset_blocks + pt_node->offset_start) * pt_node->multiplier,
							bdev_io->u.bdev.num_blocks * pt_node->multiplier, _pt_complete_io4,
							bdev_io, &io_opts);
			break;
		case SPDK_BDEV_IO_TYPE_WRITE:
			pt_init_ext_io_opts(bdev_io, &io_opts);
			rc = spdk_bdev_writev_blocks_ext(pt_node->base_desc, pt_ch->base_ch, bdev_io->u.bdev.iovs,
							bdev_io->u.bdev.iovcnt, (bdev_io->u.bdev.offset_blocks + pt_node->offset_start) * pt_node->multiplier,
							bdev_io->u.bdev.num_blocks * pt_node->multiplier, _pt_complete_io2,
							bdev_io, &io_opts);
			break;
	}

	if (rc != 0) {
		if (rc == -ENOMEM) {
			SPDK_ERRLOG("No memory, start to queue io for passthru.\n");
			io_ctx->ch = ch;
			vbdev_passthru_queue_io(bdev_io);
		} else {
			SPDK_ERRLOG("ERROR on bdev_io submission!\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

/* Called when someone above submits IO to this pt vbdev. We're simply passing it on here
 * via SPDK IO calls which in turn allocate another bdev IO and call our cpl callback provided
 * below along with the original bdev_io so that we can complete it once this IO completes.
 */
static void
vbdev_passthru_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_passthru *pt_node = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_passthru, pt_bdev);	
	struct pt_io_channel *pt_ch = spdk_io_channel_get_ctx(ch);
	struct passthru_bdev_io *io_ctx = (struct passthru_bdev_io *)bdev_io->driver_ctx;
	io_ctx->ch = ch;
	int rc = 0;

	/* Setup a per IO context value; we don't do anything with it in the vbdev other
	 * than confirm we get the same thing back in the completion callback just to
	 * demonstrate.
	 */
	io_ctx->test = 0x5a;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, pt_read_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		spdk_bdev_io_get_buf(bdev_io, pt_read_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		rc = spdk_bdev_write_zeroes_blocks(pt_node->base_desc, pt_ch->base_ch,
						   (bdev_io->u.bdev.offset_blocks + pt_node->offset_start) * pt_node->multiplier,
						   bdev_io->u.bdev.num_blocks * pt_node->multiplier,
						   _pt_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = spdk_bdev_unmap_blocks(pt_node->base_desc, pt_ch->base_ch,
					    (bdev_io->u.bdev.offset_blocks + pt_node->offset_start) * pt_node->multiplier,
					    bdev_io->u.bdev.num_blocks * pt_node->multiplier,
					    _pt_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush_blocks(pt_node->base_desc, pt_ch->base_ch,
					    (bdev_io->u.bdev.offset_blocks + pt_node->offset_start) * pt_node->multiplier,
					    bdev_io->u.bdev.num_blocks * pt_node->multiplier,
					    _pt_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		rc = spdk_bdev_reset(pt_node->base_desc, pt_ch->base_ch,
				     _pt_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		rc = spdk_bdev_zcopy_start(pt_node->base_desc, pt_ch->base_ch, NULL, 0,
					   (bdev_io->u.bdev.offset_blocks + pt_node->offset_start) * pt_node->multiplier,
					   bdev_io->u.bdev.num_blocks * pt_node->multiplier, bdev_io->u.bdev.zcopy.populate,
					   _pt_complete_zcopy_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_ABORT:
		rc = spdk_bdev_abort(pt_node->base_desc, pt_ch->base_ch, bdev_io->u.abort.bio_to_abort,
				     _pt_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_COPY:
		rc = spdk_bdev_copy_blocks(pt_node->base_desc, pt_ch->base_ch,
					   (bdev_io->u.bdev.offset_blocks + pt_node->offset_start) * pt_node->multiplier,
					   bdev_io->u.bdev.copy.src_offset_blocks * pt_node->multiplier,
					   bdev_io->u.bdev.num_blocks * pt_node->multiplier,
					   _pt_complete_io, bdev_io);
		break;
	default:
		SPDK_ERRLOG("passthru: unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	if (rc != 0) {
		if (rc == -ENOMEM) {
			SPDK_ERRLOG("No memory, start to queue io for passthru.\n");
			io_ctx->ch = ch;
			vbdev_passthru_queue_io(bdev_io);
		} else {
			SPDK_ERRLOG("ERROR on bdev_io submission!\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

/* We'll just call the base bdev and let it answer however if we were more
 * restrictive for some reason (or less) we could get the response back
 * and modify according to our purposes.
 */
static bool
vbdev_passthru_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct vbdev_passthru *pt_node = (struct vbdev_passthru *)ctx;

	return spdk_bdev_io_type_supported(pt_node->base_bdev, io_type);
}

/* We supplied this as an entry point for upper layers who want to communicate to this
 * bdev.  This is how they get a channel. We are passed the same context we provided when
 * we created our PT vbdev in examine() which, for this bdev, is the address of one of
 * our context nodes. From here we'll ask the SPDK channel code to fill out our channel
 * struct and we'll keep it in our PT node.
 */
static struct spdk_io_channel *
vbdev_passthru_get_io_channel(void *ctx)
{
	struct vbdev_passthru *pt_node = (struct vbdev_passthru *)ctx;
	struct spdk_io_channel *pt_ch = NULL;

	/* The IO channel code will allocate a channel for us which consists of
	 * the SPDK channel structure plus the size of our pt_io_channel struct
	 * that we passed in when we registered our IO device. It will then call
	 * our channel create callback to populate any elements that we need to
	 * update.
	 */
	pt_ch = spdk_get_io_channel(pt_node);

	return pt_ch;
}

/* This is the output for bdev_get_bdevs() for this vbdev */
static int
vbdev_passthru_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct vbdev_passthru *pt_node = (struct vbdev_passthru *)ctx;

	spdk_json_write_name(w, "passthru");
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&pt_node->pt_bdev));
	spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(pt_node->base_bdev));
	spdk_json_write_object_end(w);

	return 0;
}

/* This is used to generate JSON that can configure this module to its current state. */
static int
vbdev_passthru_config_json(struct spdk_json_write_ctx *w)
{
	struct vbdev_passthru *pt_node;

	TAILQ_FOREACH(pt_node, &g_pt_nodes, link) {
		const struct spdk_uuid *uuid = spdk_bdev_get_uuid(&pt_node->pt_bdev);

		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "bdev_passthru_create");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(pt_node->base_bdev));
		spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&pt_node->pt_bdev));
		if (!spdk_uuid_is_null(uuid)) {
			spdk_json_write_named_uuid(w, "uuid", uuid);
		}
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
	return 0;
}

/* We provide this callback for the SPDK channel code to create a channel using
 * the channel struct we provided in our module get_io_channel() entry point. Here
 * we get and save off an underlying base channel of the device below us so that
 * we can communicate with the base bdev on a per channel basis.  If we needed
 * our own poller for this vbdev, we'd register it here.
 */
static int
pt_bdev_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct pt_io_channel *pt_ch = ctx_buf;
	struct vbdev_passthru *pt_node = io_device;

	pt_ch->base_ch = spdk_bdev_get_io_channel(pt_node->base_desc);

	return 0;
}

/* We provide this callback for the SPDK channel code to destroy a channel
 * created with our create callback. We just need to undo anything we did
 * when we created. If this bdev used its own poller, we'd unregister it here.
 */
static void
pt_bdev_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct pt_io_channel *pt_ch = ctx_buf;

	spdk_put_io_channel(pt_ch->base_ch);
}

/* Create the passthru association from the bdev and vbdev name and insert
 * on the global list. */
static int
vbdev_passthru_insert_name(const char *bdev_name, const char *vbdev_name,
			   const struct spdk_uuid *uuid, const uint32_t block_sz, const uint32_t md_sz, const uint32_t mode)
{
	struct bdev_names *name;

	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(vbdev_name, name->vbdev_name) == 0) {
			SPDK_ERRLOG("passthru bdev %s already exists\n", vbdev_name);
			return -EEXIST;
		}
	}

	name = calloc(1, sizeof(struct bdev_names));
	if (!name) {
		SPDK_ERRLOG("could not allocate bdev_names\n");
		return -ENOMEM;
	}
	name->block_sz = block_sz;
	name->md_sz = md_sz;
	name->mode = mode;

	name->bdev_name = strdup(bdev_name);
	if (!name->bdev_name) {
		SPDK_ERRLOG("could not allocate name->bdev_name\n");
		free(name);
		return -ENOMEM;
	}

	name->vbdev_name = strdup(vbdev_name);
	if (!name->vbdev_name) {
		SPDK_ERRLOG("could not allocate name->vbdev_name\n");
		free(name->bdev_name);
		free(name);
		return -ENOMEM;
	}

	spdk_uuid_copy(&name->uuid, uuid);
	TAILQ_INSERT_TAIL(&g_bdev_names, name, link);

	return 0;
}

/* On init, just perform bdev module specific initialization. */
static int
vbdev_passthru_init(void)
{
	return 0;
}

/* Called when the entire module is being torn down. */
static void
vbdev_passthru_finish(void)
{
	struct bdev_names *name;

	while ((name = TAILQ_FIRST(&g_bdev_names))) {
		TAILQ_REMOVE(&g_bdev_names, name, link);
		free(name->bdev_name);
		free(name->vbdev_name);
		free(name);
	}
}

/* During init we'll be asked how much memory we'd like passed to us
 * in bev_io structures as context. Here's where we specify how
 * much context we want per IO.
 */
static int
vbdev_passthru_get_ctx_size(void)
{
	return sizeof(struct passthru_bdev_io);
}

/* Where vbdev_passthru_config_json() is used to generate per module JSON config data, this
 * function is called to output any per bdev specific methods. For the PT module, there are
 * none.
 */
static void
vbdev_passthru_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	/* No config per bdev needed */
}

static int
vbdev_passthru_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size)
{
	struct vbdev_passthru *pt_node = (struct vbdev_passthru *)ctx;

	/* Passthru bdev doesn't work with data buffers, so it supports any memory domain used by base_bdev */
	return spdk_bdev_get_memory_domains(pt_node->base_bdev, domains, array_size);
}

/* When we register our bdev this is how we specify our entry points. */
static const struct spdk_bdev_fn_table vbdev_passthru_fn_table = {
	.destruct		= vbdev_passthru_destruct,
	.submit_request		= vbdev_passthru_submit_request,
	.io_type_supported	= vbdev_passthru_io_type_supported,
	.get_io_channel		= vbdev_passthru_get_io_channel,
	.dump_info_json		= vbdev_passthru_dump_info_json,
	.write_config_json	= vbdev_passthru_write_config_json,
	.get_memory_domains	= vbdev_passthru_get_memory_domains,
};

static void
vbdev_passthru_base_bdev_hotremove_cb(struct spdk_bdev *bdev_find)
{
	struct vbdev_passthru *pt_node, *tmp;

	TAILQ_FOREACH_SAFE(pt_node, &g_pt_nodes, link, tmp) {
		if (bdev_find == pt_node->base_bdev) {
			spdk_bdev_unregister(&pt_node->pt_bdev, NULL, NULL);
		}
	}
}

/* Called when the underlying base bdev triggers asynchronous event such as bdev removal. */
static void
vbdev_passthru_base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
				  void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		vbdev_passthru_base_bdev_hotremove_cb(bdev);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

/* Create and register the passthru vbdev if we find it in our list of bdev names.
 * This can be called either by the examine path or RPC method.
 */
static int
vbdev_passthru_register(const char *bdev_name)
{
	struct bdev_names *name;
	struct vbdev_passthru *pt_node;
	struct spdk_bdev *bdev;
	struct spdk_uuid ns_uuid;
	int rc = 0;

	spdk_uuid_parse(&ns_uuid, BDEV_PASSTHRU_NAMESPACE_UUID);

	/* Check our list of names from config versus this bdev and if
	 * there's a match, create the pt_node & bdev accordingly.
	 */
	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(name->bdev_name, bdev_name) != 0) {
			continue;
		}

		SPDK_NOTICELOG("Match on %s\n", bdev_name);
		pt_node = calloc(1, sizeof(struct vbdev_passthru));
		if (!pt_node) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate pt_node\n");
			break;
		}

		pt_node->pt_bdev.name = strdup(name->vbdev_name);
		if (!pt_node->pt_bdev.name) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate pt_bdev name\n");
			free(pt_node);
			break;
		}
		pt_node->pt_bdev.product_name = "passthru";
		spdk_uuid_copy(&pt_node->pt_bdev.uuid, &name->uuid);

		/* The base bdev that we're attaching to. */
		rc = spdk_bdev_open_ext(bdev_name, true, vbdev_passthru_base_bdev_event_cb,
					NULL, &pt_node->base_desc);
		if (rc) {
			if (rc != -ENODEV) {
				SPDK_ERRLOG("could not open bdev %s\n", bdev_name);
			}
			free(pt_node->pt_bdev.name);
			free(pt_node);
			break;
		}
		SPDK_NOTICELOG("base bdev opened\n");

		bdev = spdk_bdev_desc_get_bdev(pt_node->base_desc);
		pt_node->base_bdev = bdev;

		/* Generate UUID based on namespace UUID + base bdev UUID. */
		rc = spdk_uuid_generate_sha1(&pt_node->pt_bdev.uuid, &ns_uuid,
					     (const char *)&pt_node->base_bdev->uuid, sizeof(struct spdk_uuid));
		if (rc) {
			SPDK_ERRLOG("Unable to generate new UUID for passthru bdev\n");
			spdk_bdev_close(pt_node->base_desc);
			free(pt_node->pt_bdev.name);
			free(pt_node);
			break;
		}

		/* Copy some properties from the underlying base bdev. */
		pt_node->pt_bdev.write_cache = bdev->write_cache;
		pt_node->pt_bdev.required_alignment = bdev->required_alignment;
		pt_node->pt_bdev.optimal_io_boundary = bdev->optimal_io_boundary;		
		if(name->block_sz){
			bool wrong_convert = false;
			if(name->block_sz < bdev->blocklen ){
				SPDK_ERRLOG("Unable to do such convert for block size, it's less than the real one\n");
				wrong_convert = true;
			}
			if(name->block_sz % bdev->blocklen != 0 ){
				SPDK_ERRLOG("Unable to do such convert for block size, it's undividable\n");
				wrong_convert = true;
			}
			if(wrong_convert){
				spdk_bdev_close(pt_node->base_desc);
				free(pt_node->pt_bdev.name);
				free(pt_node);
				break;
			}
			pt_node->multiplier = name->block_sz / bdev->blocklen;
			pt_node->pt_bdev.blocklen = bdev->blocklen * pt_node->multiplier;
			pt_node->pt_bdev.blockcnt = bdev->blockcnt / pt_node->multiplier;
		} else{
			pt_node->pt_bdev.blocklen = bdev->blocklen;
			pt_node->pt_bdev.blockcnt = bdev->blockcnt;
			pt_node->multiplier = 1;
		}

		switch (name->md_sz) {
			case 0:
			case 8:
			case 16:
			case 32:
			case 64:
			case 128:
				break;
			default:
				SPDK_ERRLOG("metadata size %u is not supported\n", name->md_sz);
				spdk_bdev_close(pt_node->base_desc);
				free(pt_node->pt_bdev.name);
				free(pt_node);
				return -EINVAL;
		}

		pt_node->pt_bdev.md_interleave = bdev->md_interleave;
		// pt_node->pt_bdev.md_len = bdev->md_len;
		pt_node->pt_bdev.md_len = name->md_sz ? name->md_sz : bdev->md_len;
		pt_node->md_len = name->md_sz ? name->md_sz : bdev->md_len;
		uint64_t guest = pt_node->pt_bdev.blockcnt * pt_node->pt_bdev.md_len;
		pt_node->offset_start = (guest / pt_node->pt_bdev.blocklen) + ((guest % pt_node->pt_bdev.blocklen) ? 1 : 0);
		// pt_node->offset_start = (guest / bdev->blocklen) + ((guest % bdev->blocklen) ? 1 : 0);
		// pt_node->offset_start = pt_node->offset_start  + ((pt_node->offset_start % pt_node->multiplier) ? pt_node->multiplier - (pt_node->offset_start % pt_node->multiplier) : 0);
		// pt_node->offset_start = (pt_node->offset_start * bdev->blocklen) / pt_node->pt_bdev.blocklen;
		// pt_node->offset_start = 20480;
		pt_node->pt_bdev.blockcnt -= pt_node->offset_start;
		// pt_node->malloc_md_buf = spdk_zmalloc((pt_node->pt_bdev.blockcnt * pt_node->md_len) + pt_node->pt_bdev.blocklen, 2 * 1024 * 1024, NULL,
		// 				    SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

		pt_node->malloc_md_buf = spdk_zmalloc(
					(pt_node->offset_start * pt_node->pt_bdev.blocklen) + pt_node->pt_bdev.blocklen,
					2 * 1024 * 1024,
					NULL,
					SPDK_ENV_LCORE_ID_ANY,
					SPDK_MALLOC_DMA
				);

		// pt_node->malloc_md_buf = spdk_zmalloc(MAX_MD_ALLOC * pt_node->pt_bdev.blocklen, 2 * 1024 * 1024, NULL,
		// 				    SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (!pt_node->malloc_md_buf) {
			SPDK_ERRLOG("malloc_md_buf spdk_zmalloc() failed\n");
			spdk_bdev_close(pt_node->base_desc);
			free(pt_node->pt_bdev.name);
			free(pt_node);
			return -ENOMEM;
		}
		pt_node->pt_bdev.dif_type = bdev->dif_type;
		pt_node->pt_bdev.dif_is_head_of_md = bdev->dif_is_head_of_md;
		pt_node->pt_bdev.dif_check_flags = bdev->dif_check_flags;

		/* This is the context that is passed to us when the bdev
		 * layer calls in so we'll save our pt_bdev node here.
		 */
		pt_node->pt_bdev.ctxt = pt_node;
		pt_node->pt_bdev.fn_table = &vbdev_passthru_fn_table;
		pt_node->pt_bdev.module = &passthru_if;
		spdk_spin_init(&pt_node->used_lock);
		TAILQ_INSERT_TAIL(&g_pt_nodes, pt_node, link);

		spdk_io_device_register(pt_node, pt_bdev_ch_create_cb, pt_bdev_ch_destroy_cb,
					sizeof(struct pt_io_channel),
					name->vbdev_name);
		pt_node->md_channel = spdk_get_io_channel(pt_node);
		SPDK_NOTICELOG("io_device created at: 0x%p\n", pt_node);

		/* Save the thread where the base device is opened */
		pt_node->thread = spdk_get_thread();

		rc = spdk_bdev_module_claim_bdev(bdev, pt_node->base_desc, pt_node->pt_bdev.module);
		if (rc) {
			SPDK_ERRLOG("could not claim bdev %s\n", bdev_name);
			spdk_bdev_close(pt_node->base_desc);
			TAILQ_REMOVE(&g_pt_nodes, pt_node, link);
			spdk_io_device_unregister(pt_node, NULL);
			free(pt_node->pt_bdev.name);
			free(pt_node);
			break;
		}
		SPDK_NOTICELOG("bdev claimed\n");

		rc = spdk_bdev_register(&pt_node->pt_bdev);
		if (rc) {
			SPDK_ERRLOG("could not register pt_bdev\n");
			spdk_bdev_module_release_bdev(&pt_node->pt_bdev);
			spdk_bdev_close(pt_node->base_desc);
			TAILQ_REMOVE(&g_pt_nodes, pt_node, link);
			spdk_io_device_unregister(pt_node, NULL);
			free(pt_node->pt_bdev.name);
			free(pt_node);
			break;
		}
		struct pt_io_channel *channel = spdk_io_channel_get_ctx(pt_node->md_channel);
		pt_node->mode = name->mode;
		if(name->mode){
			rc = spdk_bdev_write_zeroes_blocks(pt_node->base_desc, channel->base_ch, 0,
					 	  pt_node->offset_start * pt_node->multiplier , _pt_complete_io_zero, NULL);
			// memset(pt_node->malloc_md_buf, '9', pt_node->offset_start * pt_node->multiplier);
		}
		else{
			// rc = spdk_bdev_read_blocks(pt_node->base_desc, channel->base_ch, 
			// pt_node->malloc_md_buf, 0, (pt_node->offset_start) * pt_node->multiplier, _pt_complete_io_zero, NULL);

			uint64_t blocks_to_read = pt_node->offset_start;
			uint64_t max_blocks_per_io = 1024 * 4;  // or another value that works
			uint64_t offset = 0;
			while (blocks_to_read > 0) {
				uint64_t blocks_this_io = spdk_min(blocks_to_read, max_blocks_per_io);

				rc = spdk_bdev_read_blocks(
					pt_node->base_desc, 
					channel->base_ch, 
					pt_node->malloc_md_buf + (offset * pt_node->pt_bdev.blocklen), 
					(offset) * pt_node->multiplier, 
					blocks_this_io * pt_node->multiplier, 
					_pt_complete_io_zero, 
					NULL
				);

				if (rc != 0) {
					SPDK_ERRLOG("Error submitting read I/O\n");
					break;
				}

				offset += blocks_this_io;
				blocks_to_read -= blocks_this_io;
			}

		}
		SPDK_NOTICELOG("pt_bdev registered\n");
		SPDK_NOTICELOG("created pt_bdev for: %s\n", name->vbdev_name);
	}

	return rc;
}

/* Create the passthru disk from the given bdev and vbdev name. */
int
bdev_passthru_create_disk(const char *bdev_name, const char *vbdev_name,
			  const struct spdk_uuid *uuid, const uint32_t block_sz, const uint32_t md_sz, const uint32_t mode)
{
	int rc;

	/* Insert the bdev name into our global name list even if it doesn't exist yet,
	 * it may show up soon...
	 */
	rc = vbdev_passthru_insert_name(bdev_name, vbdev_name, uuid, block_sz, md_sz, mode);
	if (rc) {
		return rc;
	}

	rc = vbdev_passthru_register(bdev_name);
	if (rc == -ENODEV) {
		/* This is not an error, we tracked the name above and it still
		 * may show up later.
		 */
		SPDK_NOTICELOG("vbdev creation deferred pending base bdev arrival\n");
		rc = 0;
	}

	return rc;
}

void
bdev_passthru_delete_disk(const char *bdev_name, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct bdev_names *name;
	int rc;

	/* Some cleanup happens in the destruct callback. */
	rc = spdk_bdev_unregister_by_name(bdev_name, &passthru_if, cb_fn, cb_arg);
	if (rc == 0) {
		/* Remove the association (vbdev, bdev) from g_bdev_names. This is required so that the
		 * vbdev does not get re-created if the same bdev is constructed at some other time,
		 * unless the underlying bdev was hot-removed.
		 */
		TAILQ_FOREACH(name, &g_bdev_names, link) {
			if (strcmp(name->vbdev_name, bdev_name) == 0) {
				TAILQ_REMOVE(&g_bdev_names, name, link);
				free(name->bdev_name);
				free(name->vbdev_name);
				free(name);
				break;
			}
		}
	} else {
		cb_fn(cb_arg, rc);
	}
}

/* Because we specified this function in our pt bdev function table when we
 * registered our pt bdev, we'll get this call anytime a new bdev shows up.
 * Here we need to decide if we care about it and if so what to do. We
 * parsed the config file at init so we check the new bdev against the list
 * we built up at that time and if the user configured us to attach to this
 * bdev, here's where we do it.
 */
static void
vbdev_passthru_examine(struct spdk_bdev *bdev)
{
	vbdev_passthru_register(bdev->name);

	spdk_bdev_module_examine_done(&passthru_if);
}

SPDK_LOG_REGISTER_COMPONENT(vbdev_passthru)