#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/util/log.h>

#include "xserver/internal.h"

// MIT-SHM 1.2: apps that draw with the cpu put their images in memory they
// share with the server (SysV segments, or fds) and only send where they
// are, no pixels over the socket. PutImage from a segment, GetImage into
// one, and pixmaps that live in one (what toolkits double buffer in).
// clients are this user's only (see xserver.c), they may map our segments

#define SHM_MINOR 2

static struct xs_shm_segment *find(struct flame_xserver *xserver, uint32_t id) {
	struct xs_shm_segment *segment;
	wl_list_for_each(segment, &xserver->shm_segments, link) {
		if (segment->id == id) {
			return segment;
		}
	}
	return NULL;
}

static struct xs_shm_segment *get_segment(struct xs_client *client, uint32_t id,
		uint16_t minor) {
	struct xs_shm_segment *segment = find(client->xserver, id);
	if (!segment) {
		xs_error(client, XS_SHM_FIRST_ERROR, id, XS_SHM_OPCODE, minor); // BadShmSeg
	}
	return segment;
}

void xs_shm_unref(struct xs_shm_segment *segment) {
	if (--segment->refs > 0) {
		return;
	}
	if (segment->sysv) {
		shmdt(segment->data);
	} else {
		munmap(segment->data, segment->size);
	}
	free(segment);
}

static void detach(struct xs_shm_segment *segment) {
	wl_list_remove(&segment->link);
	wl_list_init(&segment->link);
	xs_shm_unref(segment);
}

void xs_shm_detach_all(struct flame_xserver *xserver, struct xs_client *client) {
	struct xs_shm_segment *segment, *tmp;
	wl_list_for_each_safe(segment, tmp, &xserver->shm_segments, link) {
		if (segment->owner == client) {
			detach(segment);
		}
	}
}

static bool new_id(struct xs_client *client, uint32_t id, uint16_t minor) {
	if (!xs_id_available(client, id)) {
		xs_error(client, XS_ERROR_ID_CHOICE, id, XS_SHM_OPCODE, minor);
		return false;
	}
	return true;
}

static void add(struct xs_client *client, uint32_t id, void *data, size_t size, bool read_only,
		bool sysv) {
	struct xs_shm_segment *segment = calloc(1, sizeof(*segment));
	if (!segment) {
		if (sysv) {
			shmdt(data);
		} else {
			munmap(data, size);
		}
		xs_error(client, XS_ERROR_ALLOC, id, XS_SHM_OPCODE, 0);
		return;
	}
	*segment = (struct xs_shm_segment){
		.id = id,
		.owner = client,
		.data = data,
		.size = size,
		.read_only = read_only,
		.sysv = sysv,
		.refs = 1,
	};
	wl_list_insert(&client->xserver->shm_segments, &segment->link);
}

static void query_version(struct xs_client *client) {
	struct xout out;
	xs_reply_begin(client, &out, 1); // shared pixmaps
	xout16(&out, 1);
	xout16(&out, SHM_MINOR);
	xout16(&out, (uint16_t)getuid());
	xout16(&out, (uint16_t)getgid());
	xout8(&out, 2); // pixmap format: ZPixmap
	xout_zero(&out, 15);
	xs_reply_send(client, &out);
}

// a SysV segment by its id. the kernel checks it's ours to map
static void attach(struct xs_client *client, const struct xin *in) {
	uint32_t id = xin32(in, 4);
	int shmid = (int)xin32(in, 8);
	bool read_only = xin8(in, 12);
	if (!new_id(client, id, 1)) {
		return;
	}
	struct shmid_ds ds;
	if (shmctl(shmid, IPC_STAT, &ds) < 0 || (ds.shm_perm.uid != getuid()
			&& ds.shm_perm.cuid != getuid())) {
		xs_error(client, XS_ERROR_ACCESS, id, XS_SHM_OPCODE, 1);
		return;
	}
	void *data = shmat(shmid, NULL, read_only ? SHM_RDONLY : 0);
	if (data == (void *)-1) {
		xs_error(client, XS_ERROR_ACCESS, id, XS_SHM_OPCODE, 1);
		return;
	}
	add(client, id, data, ds.shm_segsz, read_only, true);
}

// a segment the client hands over as an fd (memfd)
static void attach_fd(struct xs_client *client, const struct xin *in) {
	uint32_t id = xin32(in, 4);
	bool read_only = xin8(in, 8);
	int fd = xs_client_take_fd(client);
	if (fd < 0) {
		xs_error(client, XS_ERROR_VALUE, id, XS_SHM_OPCODE, 6);
		return;
	}
	struct stat st;
	void *data = MAP_FAILED;
	if (new_id(client, id, 6) && fstat(fd, &st) == 0 && st.st_size > 0) {
		data = mmap(NULL, (size_t)st.st_size, PROT_READ | (read_only ? 0 : PROT_WRITE),
			MAP_SHARED, fd, 0);
		if (data == MAP_FAILED) {
			xs_error(client, XS_ERROR_ACCESS, id, XS_SHM_OPCODE, 6);
		}
	}
	close(fd);
	if (data != MAP_FAILED) {
		add(client, id, data, (size_t)st.st_size, read_only, false);
	}
}

// the server makes the segment and hands the client an fd for it
static void create_segment(struct xs_client *client, const struct xin *in) {
	uint32_t id = xin32(in, 4), size = xin32(in, 8);
	bool read_only = xin8(in, 12);
	if (!new_id(client, id, 7)) {
		return;
	}
	int fd = memfd_create("flamewm-xshm", MFD_CLOEXEC);
	if (fd < 0 || size == 0 || ftruncate(fd, size) < 0) {
		if (fd >= 0) {
			close(fd);
		}
		xs_error(client, XS_ERROR_ALLOC, id, XS_SHM_OPCODE, 7);
		return;
	}
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		xs_error(client, XS_ERROR_ALLOC, id, XS_SHM_OPCODE, 7);
		return;
	}
	add(client, id, data, size, read_only, false);
	struct xout out;
	xs_client_send_fd(client, fd);
	xs_reply_begin(client, &out, 1); // one fd
	xout_zero(&out, 24);
	xs_reply_send(client, &out);
}

// bytes past offset that rows of stride need, inside the segment
static bool fits(const struct xs_shm_segment *segment, uint32_t offset, size_t stride,
		int height) {
	size_t need = stride * (size_t)height;
	return offset <= segment->size && need <= segment->size - offset;
}

static void put_image(struct xs_client *client, const struct xin *in) {
	uint32_t drawable = xin32(in, 4), gc = xin32(in, 8);
	int total_w = xin16(in, 12), total_h = xin16(in, 14);
	int src_x = xin16(in, 16), src_y = xin16(in, 18), src_w = xin16(in, 20), src_h = xin16(in, 22);
	int dst_x = xin16s(in, 24), dst_y = xin16s(in, 26);
	uint8_t depth = xin8(in, 28), format = xin8(in, 29);
	bool send_event = xin8(in, 30);
	uint32_t id = xin32(in, 32), offset = xin32(in, 36);
	struct xs_shm_segment *segment = get_segment(client, id, 3);
	if (!segment) {
		return;
	}
	size_t stride = format == 2 ? xs_image_stride(depth, total_w) : xs_image_stride(1, total_w);
	if (!fits(segment, offset, stride, total_h)) {
		xs_error(client, XS_ERROR_VALUE, offset, XS_SHM_OPCODE, 3);
		return;
	}
	if (src_x + src_w > total_w || src_y + src_h > total_h) {
		xs_error(client, XS_ERROR_VALUE, 0, XS_SHM_OPCODE, 3);
		return;
	}
	xs_draw_put_image(client, drawable, gc, format, depth, total_w, total_h, 0, src_x, src_y,
		src_w, src_h, dst_x, dst_y, segment->data + offset, stride, segment->size - offset,
		XS_SHM_OPCODE, 3);
	if (send_event) {
		// done reading: the app may write the next image there
		struct xs_event event = {0};
		xs_event_put8(&event, 0, XS_SHM_FIRST_EVENT);
		xs_event_put32(&event, 4, drawable);
		xs_event_put16(&event, 8, 3);
		xs_event_put8(&event, 10, XS_SHM_OPCODE);
		xs_event_put32(&event, 12, id);
		xs_event_put32(&event, 16, offset);
		xs_event_send(client, &event);
	}
}

static void get_image(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t drawable = xin32(in, 4);
	int x = xin16s(in, 8), y = xin16s(in, 10), w = xin16(in, 12), h = xin16(in, 14);
	uint32_t id = xin32(in, 24), offset = xin32(in, 28);
	struct xs_shm_segment *segment = get_segment(client, id, 4);
	if (!segment) {
		return;
	}
	if (segment->read_only) {
		xs_error(client, XS_ERROR_ACCESS, id, XS_SHM_OPCODE, 4);
		return;
	}
	struct xs_window *window = xserver_window(xserver, drawable);
	struct xs_pixmap *pixmap = window ? NULL : xs_pixmap_find(xserver, drawable);
	size_t stride = xs_image_stride(window ? window->depth : pixmap ? pixmap->depth : 24, w);
	if (!fits(segment, offset, stride, h)) {
		xs_error(client, XS_ERROR_VALUE, offset, XS_SHM_OPCODE, 4);
		return;
	}
	uint8_t depth;
	uint32_t visual;
	if (!xs_draw_get_image(client, drawable, x, y, w, h, segment->data + offset, stride, &depth,
			&visual, XS_SHM_OPCODE, 4)) {
		return;
	}
	struct xout out;
	xs_reply_begin(client, &out, depth);
	xout32(&out, visual);
	xout32(&out, (uint32_t)(stride * (size_t)h));
	xout_zero(&out, 16);
	xs_reply_send(client, &out);
}

static void create_pixmap(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t id = xin32(in, 4), drawable = xin32(in, 8);
	int w = xin16(in, 12), h = xin16(in, 14);
	uint8_t depth = xin8(in, 16);
	uint32_t seg = xin32(in, 20), offset = xin32(in, 24);
	if (!xserver_window(xserver, drawable) && !xs_pixmap_find(xserver, drawable)) {
		xs_error(client, XS_ERROR_DRAWABLE, drawable, XS_SHM_OPCODE, 5);
		return;
	}
	struct xs_shm_segment *segment = get_segment(client, seg, 5);
	if (!segment) {
		return;
	}
	// drawing goes into its memory: not into memory we may only read
	if (segment->read_only) {
		xs_error(client, XS_ERROR_ACCESS, seg, XS_SHM_OPCODE, 5);
		return;
	}
	size_t stride = xs_image_stride(depth, w);
	if (!fits(segment, offset, stride, h)) {
		xs_error(client, XS_ERROR_VALUE, offset, XS_SHM_OPCODE, 5);
		return;
	}
	xs_draw_create_pixmap(client, id, depth, w, h, segment->data + offset, stride, segment,
		XS_SHM_OPCODE, 5);
}

void xs_shm_request(struct xs_client *client, const struct xin *in) {
	uint8_t minor = xin8(in, 1);
	switch (minor) {
	case 0:
		query_version(client);
		break;
	case 1:
		attach(client, in);
		break;
	case 2: { // Detach
		struct xs_shm_segment *segment = get_segment(client, xin32(in, 4), 2);
		if (segment) {
			detach(segment);
		}
		break;
	}
	case 3:
		put_image(client, in);
		break;
	case 4:
		get_image(client, in);
		break;
	case 5:
		create_pixmap(client, in);
		break;
	case 6:
		attach_fd(client, in);
		break;
	case 7:
		create_segment(client, in);
		break;
	default:
		xs_error(client, XS_ERROR_REQUEST, 0, XS_SHM_OPCODE, minor);
		break;
	}
}
