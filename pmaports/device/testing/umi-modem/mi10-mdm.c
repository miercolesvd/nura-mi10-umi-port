// SPDX-License-Identifier: GPL-2.0
/*
 * mi10-mdm: boot the Mi 10's SDX55 modem without Android's mdm_helper and ks,
 * on the stock 4.19 kernel or on mainline. From mi-10-hotspot.
 *
 * What the kernel expects from userspace (drivers/esoc, drivers/bus/mhi):
 *  - a "request engine" registered on /dev/esoc-0 (ESOC_REG_REQ_ENG);
 *  - someone holding /dev/subsys_esoc0 open: opening it powers the modem up
 *    (subsystem_get) and blocks until boot is confirmed; closing it powers
 *    the modem down;
 *  - on ESOC_REQ_IMG: the modem is out of reset. PCIe2 is not enumerated at
 *    boot on this board (qcom,boot-option = 1); the kernel brings the link
 *    up during power-on, and we only enumerate it if the modem is still not
 *    on the bus. The MHI driver then loads sdx55m/sbl1.mbn by itself, and
 *    SBL asks for the rest of its images over Sahara on the SAHARA channel.
 *    We serve those from a map file (image id -> file or partition), then
 *    send ESOC_IMG_XFER_DONE, and ESOC_BOOT_DONE once MDM2AP_STATUS is high;
 *  - on ESOC_REQ_DEBUG (the modem crashed and offers RAM dumps): we take no
 *    dumps and answer ESOC_DEBUG_DONE, so the kernel restarts it.
 *
 * The modem keeps its NV (IMEI, calibration, settings) in the phone's
 * mdm1m9kefs1..3 partitions: it receives them as Sahara images at boot, and
 * in mission mode it "syncs" them back over the EFS channel (Sahara memory
 * debug: a region table, then reads). We save those regions to a RAM
 * directory and never write to the phone's storage: partitions and files
 * are only ever opened read-only.
 *
 * On mainline (-M) there is no esoc: the MHI PCI driver loads SBL over BHI
 * by itself, and the SAHARA and EFS channels are /dev/mhi_sahara and
 * /dev/mhi_efs (mhi_sahara_cdev). We serve SBL whenever /dev/mhi_sahara
 * exists; it goes away when the modem reaches mission mode and comes back
 * if the modem restarts.
 *
 * Usage: mi10-mdm -m MAP [-e EFS_DIR] [-l LINK_INFO]    (stock esoc kernel)
 *        mi10-mdm -M -m MAP [-e EFS_DIR]                (mainline)
 *   MAP lines: "<image id> <path>"; '#' starts a comment.
 * Test hooks (no esoc): -S FD serves image loading on an open fd,
 *   -E FD serves EFS sync on an open fd.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

/* include/uapi/linux/esoc_ctrl.h of the device kernel */
#define ESOC_CODE		0xCC
#define ESOC_WAIT_FOR_REQ	_IOR(ESOC_CODE, 2, unsigned int)
#define ESOC_NOTIFY		_IOW(ESOC_CODE, 3, unsigned int)
#define ESOC_GET_STATUS		_IOR(ESOC_CODE, 4, unsigned int)
#define ESOC_REG_REQ_ENG	_IO(ESOC_CODE, 7)
#define ESOC_SET_BOOT_FAIL_ACT	_IOW(ESOC_CODE, 10, unsigned int)

enum { BOOT_FAIL_ACTION_SHUTDOWN = 2 };
enum {
	ESOC_IMG_XFER_DONE = 1, ESOC_BOOT_DONE, ESOC_BOOT_FAIL,
	ESOC_IMG_XFER_RETRY, ESOC_IMG_XFER_FAIL, ESOC_UPGRADE_AVAILABLE,
	ESOC_DEBUG_DONE, ESOC_DEBUG_FAIL,
};
enum {
	ESOC_REQ_IMG = 1, ESOC_REQ_DEBUG, ESOC_REQ_SHUTDOWN,
	ESOC_REQ_SEND_SHUTDOWN, ESOC_REQ_CRASH_SHUTDOWN,
};

/* Sahara commands */
enum {
	SAHARA_HELLO = 0x1, SAHARA_HELLO_RESP, SAHARA_READ_DATA,
	SAHARA_END_OF_IMAGE, SAHARA_DONE, SAHARA_DONE_RESP, SAHARA_RESET,
	SAHARA_RESET_RESP, SAHARA_MEM_DEBUG, SAHARA_MEM_READ,
	SAHARA_CMD_READY, SAHARA_SWITCH_MODE, SAHARA_EXECUTE,
	SAHARA_EXECUTE_RESP, SAHARA_EXECUTE_DATA, SAHARA_MEM_DEBUG64,
	SAHARA_MEM_READ64, SAHARA_READ_DATA64, SAHARA_RESET_STATE,
};

#define MAX_IMAGES	64
#define PKT_MAX		0x10000
#define IO_TIMEOUT_MS	60000
#define MEM_CHUNK	0x40000
#define REGION_MAX	(64u << 20)

struct image {
	unsigned int id;
	char path[256];
	int fd;
	off_t size;
	uint64_t sent;
};

static struct image images[MAX_IMAGES];
static int n_images;
static const char *efs_dir = "/var/run/mi10-mdm/efs";
static char link_info[64] = "0306_02.01.00";
/* channel device nodes, set in main() */
static char sahara_name[96];
static char efs_name[96];
static int kmsg_fd = -1;

static void logmsg(int to_kmsg, const char *fmt, ...)
{
	char line[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(line) - 2)
		n = sizeof(line) - 2;
	line[n++] = '\n';
	if (write(STDERR_FILENO, line, n) < 0) {
		/* nothing to do */
	}
	if (to_kmsg && kmsg_fd >= 0) {
		char k[540];
		int m = snprintf(k, sizeof(k), "mi10: modem: %.*s", n, line);

		if (write(kmsg_fd, k, m) < 0) {
			/* nothing to do */
		}
	}
}
#define info(...)	logmsg(1, __VA_ARGS__)
#define dbg(...)	logmsg(0, __VA_ARGS__)

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- map file ---------------------------------------------------------- */

static int load_map(const char *file)
{
	char line[512];
	FILE *f = fopen(file, "r");

	if (!f) {
		info("cannot open image map %s: %s", file, strerror(errno));
		return -1;
	}
	while (fgets(line, sizeof(line), f) && n_images < MAX_IMAGES) {
		struct image *im = &images[n_images];
		char *p = strchr(line, '#');

		if (p)
			*p = 0;
		if (sscanf(line, "%u %255s", &im->id, im->path) != 2)
			continue;
		im->fd = -1;
		n_images++;
	}
	fclose(f);
	dbg("image map %s: %d entries", file, n_images);
	return 0;
}

static struct image *find_image(unsigned int id)
{
	for (int i = 0; i < n_images; i++) {
		struct image *im = &images[i];

		if (im->id != id)
			continue;
		if (im->fd < 0) {
			im->fd = open(im->path, O_RDONLY | O_CLOEXEC);
			if (im->fd < 0) {
				info("image %u: cannot open %s: %s", id, im->path, strerror(errno));
				return NULL;
			}
			im->size = lseek(im->fd, 0, SEEK_END);
		}
		return im;
	}
	info("image %u requested, but the map has no file for it", id);
	return NULL;
}

/* ---- channel I/O ------------------------------------------------------- */

static int wait_readable(int fd, int ms)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	int r;

	do
		r = poll(&p, 1, ms);
	while (r < 0 && errno == EINTR);
	if (r == 0)
		errno = ETIMEDOUT;
	return r > 0 ? 0 : -1;
}

/* One packet: the MHI char device returns one transfer per read. */
static ssize_t read_pkt(int fd, void *buf, size_t len, int ms)
{
	ssize_t n;

	if (wait_readable(fd, ms))
		return -1;
	do
		n = read(fd, buf, len);
	while (n < 0 && errno == EINTR);
	if (n == 0)
		errno = EPIPE;
	return n > 0 ? n : -1;
}

static int read_exact(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;

	while (len) {
		ssize_t n = read_pkt(fd, p, len, IO_TIMEOUT_MS);

		if (n < 0)
			return -1;
		p += n;
		len -= n;
	}
	return 0;
}

static int write_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len) {
		ssize_t n = write(fd, p, len);

		if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
			usleep(1000);
			continue;
		}
		if (n <= 0)
			return -1;
		p += n;
		len -= n;
	}
	return 0;
}

static int send_words(int fd, int n, ...)
{
	uint32_t w[16];
	va_list ap;

	va_start(ap, n);
	for (int i = 0; i < n; i++)
		w[i] = va_arg(ap, uint32_t);
	va_end(ap);
	return write_all(fd, w, n * 4);
}

static uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
	return le32(p) | (uint64_t)le32(p + 4) << 32;
}

static void hexdump(const char *what, const uint8_t *p, size_t n)
{
	char s[3 * 48 + 1] = "";

	if (n > 48)
		n = 48;
	for (size_t i = 0; i < n; i++)
		sprintf(s + 3 * i, "%02x ", p[i]);
	info("%s: %s", what, s);
}

static int hello_resp(int fd, const uint8_t *pkt, size_t n)
{
	uint32_t ver = n >= 12 ? le32(pkt + 8) : 0;
	uint32_t mode = n >= 24 ? le32(pkt + 20) : 0;

	dbg("HELLO version %u, mode %u", ver, mode);
	/* cmd, length, version, version compatible, status, mode, reserved[6] */
	return send_words(fd, 12, SAHARA_HELLO_RESP, 0x30, 2, 1, 0, mode,
			  0, 0, 0, 0, 0, 0);
}

static int serve_read(int fd, uint32_t id, uint64_t off, uint64_t len)
{
	static uint8_t buf[1 << 20];
	struct image *im = find_image(id);

	if (!im)
		return -1;
	if (im->sent == 0)
		dbg("image %u: first read (%s, %lld bytes)", id, im->path, (long long)im->size);
	while (len) {
		size_t chunk = len > sizeof(buf) ? sizeof(buf) : len;
		ssize_t n = 0;

		if ((off_t)off < im->size)
			n = pread(im->fd, buf, chunk, off);
		if (n < 0) {
			info("image %u: read error at %llu: %s", id,
			     (unsigned long long)off, strerror(errno));
			return -1;
		}
		if ((size_t)n < chunk) {
			/* beyond the end of the file: pad with zeros */
			memset(buf + n, 0, chunk - n);
		}
		if (write_all(fd, buf, chunk)) {
			info("image %u: channel write failed: %s", id, strerror(errno));
			return -1;
		}
		off += chunk;
		len -= chunk;
		im->sent += chunk;
	}
	return 0;
}

/*
 * Serve the boot images SBL asks for. Returns 0 once the modem reports the
 * whole transfer complete (DONE_RESP status 1).
 */
static int sahara_images(int fd)
{
	static uint8_t pkt[PKT_MAX];
	double t0 = now(), ti = t0;
	int count = 0;

	for (;;) {
		ssize_t n = read_pkt(fd, pkt, sizeof(pkt), IO_TIMEOUT_MS);
		uint32_t cmd;

		if (n < 0) {
			info("Sahara: channel read failed: %s", strerror(errno));
			return -1;
		}
		if (n < 8) {
			hexdump("Sahara: short packet", pkt, n);
			continue;
		}
		cmd = le32(pkt);
		switch (cmd) {
		case SAHARA_HELLO:
			if (hello_resp(fd, pkt, n))
				return -1;
			break;
		case SAHARA_READ_DATA:
			if (n < 20 || serve_read(fd, le32(pkt + 8), le32(pkt + 12), le32(pkt + 16)))
				return -1;
			break;
		case SAHARA_READ_DATA64:
			if (n < 32 || serve_read(fd, (uint32_t)le64(pkt + 8), le64(pkt + 16), le64(pkt + 24)))
				return -1;
			break;
		case SAHARA_END_OF_IMAGE: {
			uint32_t id = n >= 12 ? le32(pkt + 8) : 0;
			uint32_t st = n >= 16 ? le32(pkt + 12) : 0;
			struct image *im = NULL;

			for (int i = 0; i < n_images; i++)
				if (images[i].id == id)
					im = &images[i];
			if (st) {
				info("Sahara: the modem rejected image %u (%s), status %u",
				     id, im ? im->path : "?", st);
				return -1;
			}
			info("image %u loaded (%s, %.1fs)", id,
			     im ? strrchr(im->path, '/') + 1 : "?", now() - ti);
			ti = now();
			count++;
			if (send_words(fd, 2, SAHARA_DONE, 8))
				return -1;
			break;
		}
		case SAHARA_DONE_RESP:
			if (n >= 12 && le32(pkt + 8) == 1) {
				info("all %d images loaded in %.1fs", count, now() - t0);
				return 0;
			}
			break;	/* more images: the next HELLO follows */
		case SAHARA_CMD_READY:
			/* command mode: ask for image transfer instead */
			if (send_words(fd, 3, SAHARA_SWITCH_MODE, 12, 0))
				return -1;
			break;
		case SAHARA_MEM_DEBUG:
		case SAHARA_MEM_DEBUG64:
			info("Sahara: the modem offers a RAM dump instead of booting (it crashed in SBL)");
			send_words(fd, 2, SAHARA_RESET, 8);
			return -1;
		default:
			hexdump("Sahara: unexpected packet", pkt, n);
			break;
		}
	}
}

/* ---- EFS sync ---------------------------------------------------------- */

static int mem_read(int fd, int wide, uint64_t addr, uint8_t *dst, uint64_t len)
{
	while (len) {
		uint64_t n = len > MEM_CHUNK ? MEM_CHUNK : len;
		int r;

		if (wide)
			r = send_words(fd, 6, SAHARA_MEM_READ64, 0x18,
				       (uint32_t)addr, (uint32_t)(addr >> 32),
				       (uint32_t)n, (uint32_t)(n >> 32));
		else
			r = send_words(fd, 4, SAHARA_MEM_READ, 0x10, (uint32_t)addr, (uint32_t)n);
		if (r || read_exact(fd, dst, n))
			return -1;
		addr += n;
		dst += n;
		len -= n;
	}
	return 0;
}

static void safe_name(char *dst, const uint8_t *src, size_t len, int idx)
{
	size_t j = 0;

	for (size_t i = 0; i < len && src[i]; i++) {
		char c = src[i];

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
			dst[j++] = c;
	}
	if (!j)
		j = sprintf(dst, "region%02d.bin", idx);
	dst[j] = 0;
}

/* The modem hands its file system over: read every region into RAM. */
static int efs_regions(int fd, int wide, uint64_t table_addr, uint64_t table_len)
{
	size_t esz = wide ? 64 : 52;
	uint8_t *table;
	int saved = 0;

	if (table_len > 64 * 64 || table_len < esz) {
		info("EFS: odd region table length %llu", (unsigned long long)table_len);
		return -1;
	}
	table = malloc(table_len);
	if (!table || mem_read(fd, wide, table_addr, table, table_len)) {
		info("EFS: cannot read the region table: %s", strerror(errno));
		free(table);
		return -1;
	}
	mkdir(efs_dir, 0700);
	for (size_t i = 0; i + esz <= table_len; i += esz) {
		const uint8_t *e = table + i;
		uint64_t addr = wide ? le64(e + 8) : le32(e + 4);
		uint64_t len = wide ? le64(e + 16) : le32(e + 8);
		const uint8_t *fname = e + (wide ? 44 : 32);
		char name[32], path[512], tmp[520];
		uint8_t *buf;
		int out;

		safe_name(name, fname, 20, (int)(i / esz));
		if (!len)
			continue;
		if (len > REGION_MAX) {
			info("EFS: region %s is %llu bytes, skipped", name, (unsigned long long)len);
			continue;
		}
		buf = malloc(len);
		if (!buf || mem_read(fd, wide, addr, buf, len)) {
			info("EFS: reading region %s failed: %s", name, strerror(errno));
			free(buf);
			free(table);
			return -1;
		}
		snprintf(path, sizeof(path), "%s/%s", efs_dir, name);
		snprintf(tmp, sizeof(tmp), "%s.tmp", path);
		out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
		if (out < 0 || write_all(out, buf, len) || close(out) || rename(tmp, path))
			info("EFS: cannot save %s: %s", path, strerror(errno));
		else
			saved++;
		dbg("EFS: region %s, %llu bytes", name, (unsigned long long)len);
		free(buf);
	}
	free(table);
	return saved;
}

/* Serve EFS sync sessions on an open channel until it fails. */
static void efs_serve(int fd)
{
	static uint8_t pkt[PKT_MAX];
	int syncs = 0;

	for (;;) {
		ssize_t n = read_pkt(fd, pkt, sizeof(pkt), -1);
		uint32_t cmd;

		if (n < 0) {
			info("EFS: channel closed (%s) after %d sync(s)", strerror(errno), syncs);
			return;
		}
		if (n < 8) {
			hexdump("EFS: short packet", pkt, n);
			continue;
		}
		cmd = le32(pkt);
		switch (cmd) {
		case SAHARA_HELLO:
			if (hello_resp(fd, pkt, n))
				return;
			break;
		case SAHARA_MEM_DEBUG:
		case SAHARA_MEM_DEBUG64: {
			int wide = cmd == SAHARA_MEM_DEBUG64;
			uint64_t a = wide ? le64(pkt + 8) : le32(pkt + 8);
			uint64_t l = wide ? le64(pkt + 16) : le32(pkt + 12);
			int saved = efs_regions(fd, wide, a, l);

			if (saved < 0)
				return;
			syncs++;
			/* first sync, and then only now and then, to the kernel log */
			logmsg(syncs == 1 || syncs % 20 == 0,
			       "EFS sync %d: %d region(s) kept in RAM (%s)", syncs, saved, efs_dir);
			/* closing the session is the host's job */
			if (send_words(fd, 2, SAHARA_RESET, 8))
				return;
			break;
		}
		case SAHARA_RESET_RESP:
			break;
		case SAHARA_READ_DATA:
			if (n < 20 || serve_read(fd, le32(pkt + 8), le32(pkt + 12), le32(pkt + 16)))
				return;
			break;
		case SAHARA_READ_DATA64:
			if (n < 32 || serve_read(fd, (uint32_t)le64(pkt + 8), le64(pkt + 16), le64(pkt + 24)))
				return;
			break;
		case SAHARA_END_OF_IMAGE:
			if (send_words(fd, 2, SAHARA_DONE, 8))
				return;
			break;
		case SAHARA_DONE_RESP:
			break;
		default:
			hexdump("EFS: unexpected packet", pkt, n);
			break;
		}
	}
}

/* ---- device nodes and the bus ------------------------------------------ */

/*
 * Open /dev/<name>, (re)making the node from sysfs first: procd's hotplug
 * normally makes it, but after a modem restart a stale node could point at
 * a minor number that now belongs to another channel.
 */
static int open_node(const char *name, int flags)
{
	char path[128], pat[160];
	unsigned int ma, mi;
	struct stat st;
	glob_t g;
	FILE *f;

	snprintf(path, sizeof(path), "/dev/%s", name);
	snprintf(pat, sizeof(pat), "/sys/class/*/%s/dev", name);
	if (glob(pat, 0, NULL, &g) == 0) {
		f = fopen(g.gl_pathv[0], "r");
		if (f && fscanf(f, "%u:%u", &ma, &mi) == 2 &&
		    (stat(path, &st) || st.st_rdev != makedev(ma, mi))) {
			unlink(path);
			mknod(path, S_IFCHR | 0600, makedev(ma, mi));
		}
		if (f)
			fclose(f);
		globfree(&g);
	}
	return open(path, flags | O_CLOEXEC);
}

/*
 * MHI channel devices exist only while the modem is in the matching state
 * (class mhi_uci on the stock kernel, misc on mainline).
 */
static int node_exists(const char *name)
{
	char pat[160];
	glob_t g;
	int found;

	snprintf(pat, sizeof(pat), "/sys/class/*/%s", name);
	found = glob(pat, 0, NULL, &g) == 0;
	if (found)
		globfree(&g);
	return found;
}

static int modem_on_pci(void)
{
	glob_t g;
	int found = 0;

	if (glob("/sys/bus/pci/devices/*/device", 0, NULL, &g))
		return 0;
	for (size_t i = 0; i < g.gl_pathc && !found; i++) {
		FILE *f = fopen(g.gl_pathv[i], "r");
		unsigned int id = 0;

		if (f && fscanf(f, "%x", &id) == 1 && id == 0x0306)
			found = 1;
		if (f)
			fclose(f);
	}
	globfree(&g);
	return found;
}

/* PCIe root complex for the modem: cell-index == domain from link_info */
static int enumerate_pcie(void)
{
	unsigned int domain = 2;
	char want[64] = "";
	glob_t g;
	int done = -1;

	sscanf(link_info, "%*x_%u.", &domain);
	if (glob("/sys/bus/platform/devices/*/enumerate", 0, NULL, &g)) {
		info("no PCIe 'enumerate' attribute in sysfs");
		return -1;
	}
	for (size_t i = 0; i < g.gl_pathc; i++) {
		char ci[256];
		uint8_t be[4];
		int f;

		snprintf(ci, sizeof(ci), "%.*s/of_node/cell-index",
			 (int)(strrchr(g.gl_pathv[i], '/') - g.gl_pathv[i]), g.gl_pathv[i]);
		f = open(ci, O_RDONLY);
		if (f < 0)
			continue;
		if (read(f, be, 4) == 4 &&
		    (unsigned)(be[0] << 24 | be[1] << 16 | be[2] << 8 | be[3]) == domain)
			snprintf(want, sizeof(want), "%s", g.gl_pathv[i]);
		close(f);
	}
	if (!*want && g.gl_pathc)
		info("no PCIe controller with cell-index %u", domain);
	if (*want) {
		int f = open(want, O_WRONLY);

		if (f >= 0 && write(f, "1", 1) == 1)
			done = 0;
		if (f >= 0)
			close(f);
		info("PCIe%u enumeration requested (%s)%s", domain, want, done ? ": write failed" : "");
	}
	globfree(&g);
	return done;
}

static int wait_node(const char *name, int seconds)
{
	for (int i = 0; i < seconds * 10; i++) {
		if (node_exists(name))
			return 0;
		usleep(100000);
	}
	return -1;
}

/* ---- esoc -------------------------------------------------------------- */

/*
 * Note: these ioctls return a positive value on success too
 * (ESOC_WAIT_FOR_REQ returns the number of bytes it read), so only a
 * negative return is a failure.
 */
static int esoc_fd = -1;
/* set while the power thread is inside open(/dev/subsys_esoc0) */
static volatile sig_atomic_t powering;

static void notify(unsigned int what)
{
	if (ioctl(esoc_fd, ESOC_NOTIFY, &what) < 0)
		info("ESOC_NOTIFY %u failed: %s", what, strerror(errno));
}

/*
 * The power thread waits in open() for the boot to be confirmed, and that
 * wait cannot be interrupted: a process exiting mid-boot would hang there
 * forever. Telling the kernel the boot failed ends the wait (the kernel
 * then powers the modem off), so the process can exit. Async-signal-safe.
 */
static void abort_boot(void)
{
	unsigned int v = ESOC_BOOT_FAIL;

	if (powering && esoc_fd >= 0)
		ioctl(esoc_fd, ESOC_NOTIFY, &v);
}

static void die_esoc(const char *what)
{
	info("%s: %s", what, strerror(errno));
	abort_boot();
	exit(1);
}

static int load_images(void)
{
	const char *sahara = sahara_name;
	int fd, ret;

	for (int tries = 0; tries < 2 && !node_exists(sahara); tries++) {
		if (!modem_on_pci())
			enumerate_pcie();
		if (wait_node(sahara, 20) == 0)
			break;
		info("no Sahara channel (%s) after 20 s%s", sahara,
		     modem_on_pci() ? " (modem is on PCIe)" : " (modem not on PCIe)");
	}
	fd = open_node(sahara, O_RDWR);
	if (fd < 0) {
		info("cannot open /dev/%s: %s", sahara, strerror(errno));
		return -1;
	}
	info("serving boot images on /dev/%s", sahara);
	for (int i = 0; i < n_images; i++)
		images[i].sent = 0;
	ret = sahara_images(fd);
	close(fd);
	return ret;
}

static void *efs_thread(void *arg)
{
	const char *efs = efs_name;

	(void)arg;
	for (;;) {
		int fd;

		while (!node_exists(efs))
			sleep(1);
		fd = open_node(efs, O_RDWR);
		if (fd < 0) {
			sleep(1);
			continue;
		}
		info("EFS sync channel open (/dev/%s)", efs);
		efs_serve(fd);
		close(fd);
		sleep(1);
	}
	return NULL;
}

static void *power_thread(void *arg)
{
	int fd;

	(void)arg;
	info("powering the modem up (open /dev/subsys_esoc0)");
	powering = 1;
	fd = open_node("subsys_esoc0", O_RDONLY);
	powering = 0;
	if (fd < 0) {
		info("modem power-up FAILED: %s", strerror(errno));
		exit(1);
	}
	info("modem is up (boot confirmed to the kernel)");
	for (;;)
		pause();	/* holding the fd keeps the modem powered */
	return NULL;
}

static void on_signal(int sig)
{
	(void)sig;
	abort_boot();
	_exit(0);	/* closing subsys_esoc0 powers the modem down */
}

static void run_esoc(void)
{
	unsigned int v = BOOT_FAIL_ACTION_SHUTDOWN;
	pthread_t t;

	esoc_fd = open_node("esoc-0", O_RDWR);
	if (esoc_fd < 0)
		die_esoc("cannot open /dev/esoc-0");
	if (ioctl(esoc_fd, ESOC_SET_BOOT_FAIL_ACT, &v) < 0)
		info("ESOC_SET_BOOT_FAIL_ACT failed: %s", strerror(errno));
	if (ioctl(esoc_fd, ESOC_REG_REQ_ENG) < 0)
		die_esoc("cannot register with esoc (ESOC_REG_REQ_ENG)");
	pthread_create(&t, NULL, efs_thread, NULL);
	pthread_create(&t, NULL, power_thread, NULL);

	for (;;) {
		unsigned int req = 0, st = 0;
		int i;

		if (ioctl(esoc_fd, ESOC_WAIT_FOR_REQ, &req) < 0) {
			if (errno == EINTR)
				continue;
			die_esoc("ESOC_WAIT_FOR_REQ failed");
		}
		switch (req) {
		case ESOC_REQ_IMG:
			info("the modem is out of reset and wants its images");
			if (load_images()) {
				info("image transfer FAILED");
				notify(ESOC_IMG_XFER_FAIL);
				break;
			}
			notify(ESOC_IMG_XFER_DONE);
			/* the kernel gives MDM2AP_STATUS 120 s (MDM2AP_STATUS_TIMEOUT_MS) */
			for (i = 0; i < 1200; i++) {
				if (ioctl(esoc_fd, ESOC_GET_STATUS, &st) >= 0 && st)
					break;
				usleep(100000);
			}
			if (st) {
				info("MDM2AP status is high after %.1f s: boot done", i / 10.0);
				notify(ESOC_BOOT_DONE);
			} else {
				info("MDM2AP status never went high: boot FAILED");
				notify(ESOC_BOOT_FAIL);
			}
			break;
		case ESOC_REQ_DEBUG:
			info("the modem crashed and offers RAM dumps: skipped, restarting it");
			notify(ESOC_DEBUG_DONE);
			break;
		case ESOC_REQ_SHUTDOWN:
		case ESOC_REQ_SEND_SHUTDOWN:
		case ESOC_REQ_CRASH_SHUTDOWN:
			info("shutdown request %u from the kernel", req);
			break;
		default:
			info("unknown esoc request %u", req);
			break;
		}
	}
}

/* ---- mainline: no esoc ------------------------------------------------ */

static void run_mhi(void)
{
	pthread_t t;

	pthread_create(&t, NULL, efs_thread, NULL);
	for (;;) {
		int fd, ret;

		info("waiting for the modem to reach SBL (/dev/%s)", sahara_name);
		while (!node_exists(sahara_name))
			usleep(200000);
		fd = open_node(sahara_name, O_RDWR);
		if (fd < 0) {
			info("cannot open /dev/%s: %s", sahara_name, strerror(errno));
			sleep(1);
			continue;
		}
		info("the modem is in SBL and wants its images (/dev/%s)", sahara_name);
		for (int i = 0; i < n_images; i++)
			images[i].sent = 0;
		ret = sahara_images(fd);
		close(fd);
		info(ret ? "image transfer FAILED" : "images served, waiting for mission mode");
		/* the node goes away once the modem leaves SBL */
		for (int i = 1; node_exists(sahara_name); i++) {
			sleep(1);
			if (i == 30)
				info("the modem is still in SBL 30 s later");
		}
		info("the modem left SBL");
	}
}

int main(int argc, char **argv)
{
	int mainline = 0;
	const char *map = NULL;
	int sahara_fd = -1, efs_fd = -1;
	int opt;

	while ((opt = getopt(argc, argv, "Mm:e:l:S:E:")) != -1) {
		switch (opt) {
		case 'M': mainline = 1; break;
		case 'm': map = optarg; break;
		case 'e': efs_dir = optarg; break;
		case 'l': snprintf(link_info, sizeof(link_info), "%s", optarg); break;
		case 'S': sahara_fd = atoi(optarg); break;
		case 'E': efs_fd = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: %s [-M] -m MAP [-e EFS_DIR] [-l LINK_INFO]\n", argv[0]);
			return 2;
		}
	}
	if (!map || load_map(map))
		return 2;
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);

	if (sahara_fd >= 0)
		return sahara_images(sahara_fd) ? 1 : 0;
	if (efs_fd >= 0) {
		efs_serve(efs_fd);
		return 0;
	}
	kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (mainline) {
		snprintf(sahara_name, sizeof(sahara_name), "mhi_sahara");
		snprintf(efs_name, sizeof(efs_name), "mhi_efs");
		run_mhi();
		return 0;
	}
	snprintf(sahara_name, sizeof(sahara_name), "mhi_%s_pipe_2", link_info);
	snprintf(efs_name, sizeof(efs_name), "mhi_%s_pipe_10", link_info);
	run_esoc();
	return 0;
}
