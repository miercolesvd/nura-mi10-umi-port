// SPDX-License-Identifier: GPL-2.0
/*
 * mi10-qmi: talk QMI to the Mi 10's SDX55 over QRTR, the way Android's QMI
 * framework does. Over QRTR there is no QMUX and no client ids: every
 * socket is one client, and a service is found through the name service.
 *
 *   mi10-qmi [-t MS] lookup [SERVICE]   servers the name service knows
 *   mi10-qmi [-t MS] info               firmware, mode, SIM, network, signal,
 *                                       stored APN, data format (read-only)
 *   mi10-qmi [-t MS] network            one line: registration, RAT, PLMN, signal
 *   mi10-qmi [-t MS] online             set the modem's operating mode to online
 *   mi10-qmi [-t MS] raw SVC MSG [T:HEX...]  one request; prints the reply's TLVs
 *   mi10-qmi dtr DEV                    raise DTR/RTS on an MHI channel device
 *   mi10-qmi [-t MS] connect [-a APN] [-u USER -p PASS [-A pap|chap|both]]
 *            [-e IFACE] [-m MUX] [-H HOOK]
 *       start a data session and hold it: QMAP on the PCIe data endpoint,
 *       WDS bound to mux MUX; runs HOOK (sh -c) with IP, PREFIX, MASK, GW,
 *       DNS1, DNS2 and MTU in the environment once connected; exits 1 when
 *       the session ends, 0 after SIGTERM (stopping the session first)
 *
 * Nothing identifying (IMEI, IMSI, ICCID, phone number) is ever requested.
 * Message and TLV numbers follow libqmi's service definitions.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <syslog.h>
#include <sys/wait.h>
#include <linux/qrtr.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

enum { QRTR_BYE = 3, QRTR_NEW_SERVER = 4, QRTR_DEL_SERVER = 5, QRTR_NEW_LOOKUP = 10 };
enum { SVC_WDS = 1, SVC_DMS = 2, SVC_NAS = 3, SVC_UIM = 11, SVC_WDA = 0x1a, SVC_DPM = 0x2f };

struct ctrl_pkt {
	uint32_t cmd;
	uint32_t service, instance, node, port;
};

struct svc {
	unsigned int service, version, instance, node, port;
};

static int usage(void);
static int sock = -1;
static struct sockaddr_qrtr ns_sq;
static int timeout_ms = 5000;
static uint16_t txn;
static unsigned int last_error;
static int last_len;			/* TLV bytes in the last reply, also on errors */
static volatile sig_atomic_t stopping;

static int now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int open_sock(void)
{
	socklen_t sl = sizeof(ns_sq);

	sock = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	if (sock < 0 || getsockname(sock, (struct sockaddr *)&ns_sq, &sl) < 0) {
		fprintf(stderr, "qrtr socket: %s\n", strerror(errno));
		return -1;
	}
	ns_sq.sq_port = QRTR_PORT_CTRL;
	txn = getpid() & 0x7fff;
	return 0;
}

/* receive one datagram before the deadline */
static ssize_t recv_until(void *buf, size_t len, struct sockaddr_qrtr *from, int deadline)
{
	for (;;) {
		struct pollfd p = { .fd = sock, .events = POLLIN };
		socklen_t sl = sizeof(*from);
		int left = deadline - now_ms();
		ssize_t n;

		if (left <= 0 || stopping) {
			errno = stopping ? EINTR : ETIMEDOUT;
			return -1;
		}
		if (poll(&p, 1, left) <= 0)
			continue;
		n = recvfrom(sock, buf, len, 0, (struct sockaddr *)from, &sl);
		if (n >= 0 || errno != EINTR)
			return n;
	}
}

/* Servers for SERVICE (0: all) as the name service lists them. */
static int lookup(unsigned int service, struct svc *out, int max)
{
	struct ctrl_pkt pkt = { .cmd = QRTR_NEW_LOOKUP, .service = service };
	int deadline = now_ms() + timeout_ms, n = 0;

	if (sendto(sock, &pkt, sizeof(pkt), 0, (struct sockaddr *)&ns_sq, sizeof(ns_sq)) < 0) {
		fprintf(stderr, "lookup: %s\n", strerror(errno));
		return -1;
	}
	for (;;) {
		struct sockaddr_qrtr from;
		struct ctrl_pkt r;
		ssize_t len = recv_until(&r, sizeof(r), &from, deadline);

		if (len < 0)
			break;
		if (from.sq_port != QRTR_PORT_CTRL || len < (ssize_t)sizeof(r) || r.cmd != QRTR_NEW_SERVER)
			continue;
		if (!r.service && !r.instance && !r.node && !r.port)
			break;		/* end of the list */
		if (n < max)
			out[n++] = (struct svc){ r.service, r.instance & 0xff, r.instance >> 8, r.node, r.port };
	}
	return n;
}

static int find_service(unsigned int service, struct svc *s)
{
	struct svc list[8];
	int n = lookup(service, list, 8);

	if (n <= 0)
		return -1;
	*s = list[0];
	return 0;
}

/*
 * One QMI request. Returns the length of the reply's TLV area in resp, -1
 * on transport failure or timeout, -2 if the modem answered with an error
 * (code in last_error).
 */
static int qmi_req(const struct svc *s, uint16_t msg, const uint8_t *tlv, size_t tlen,
		   uint8_t *resp, size_t rmax)
{
	struct sockaddr_qrtr to = { .sq_family = AF_QIPCRTR, .sq_node = s->node, .sq_port = s->port };
	uint8_t req[1024], buf[8192];
	int deadline = now_ms() + timeout_ms;
	uint16_t id = ++txn;

	if (tlen + 7 > sizeof(req))
		return -1;
	req[0] = 0;			/* request */
	req[1] = id;
	req[2] = id >> 8;
	req[3] = msg;
	req[4] = msg >> 8;
	req[5] = tlen;
	req[6] = tlen >> 8;
	memcpy(req + 7, tlv, tlen);
	if (sendto(sock, req, tlen + 7, 0, (struct sockaddr *)&to, sizeof(to)) < 0)
		return -1;
	for (;;) {
		struct sockaddr_qrtr from;
		ssize_t n = recv_until(buf, sizeof(buf), &from, deadline);
		size_t len;

		if (n < 0)
			return -1;
		if (from.sq_node != s->node || from.sq_port != s->port || n < 7)
			continue;
		if (buf[0] != 2 || (buf[1] | buf[2] << 8) != id || (buf[3] | buf[4] << 8) != msg)
			continue;	/* indication, or another transaction */
		len = buf[5] | buf[6] << 8;
		if (len > (size_t)n - 7)
			len = n - 7;
		if (len > rmax)
			len = rmax;
		memcpy(resp, buf + 7, len);
		last_len = len;
		for (size_t o = 0; o + 3 <= len;) {
			unsigned int t = resp[o], l = resp[o + 1] | resp[o + 2] << 8;

			if (t == 0x02 && l >= 4 && o + 3 + l <= len && (resp[o + 3] | resp[o + 4] << 8)) {
				last_error = resp[o + 5] | resp[o + 6] << 8;
				return -2;
			}
			o += 3 + l;
		}
		return len;
	}
}

static const uint8_t *tlv_get(const uint8_t *b, int len, unsigned int type, unsigned int *tl)
{
	for (int o = 0; o + 3 <= len;) {
		unsigned int t = b[o], l = b[o + 1] | b[o + 2] << 8;

		if (o + 3 + (int)l > len)
			return NULL;
		if (t == type) {
			*tl = l;
			return b + o + 3;
		}
		o += 3 + l;
	}
	return NULL;
}

static const char *name(const char *const *names, size_t n, unsigned int v)
{
	static char buf[16];

	if (v < n && names[v])
		return names[v];
	snprintf(buf, sizeof(buf), "%u", v);
	return buf;
}
#define NAME(arr, v) name(arr, sizeof(arr) / sizeof(arr[0]), v)

static void print_str(const char *key, const uint8_t *p, unsigned int l)
{
	printf("%s=", key);
	for (unsigned int i = 0; i < l && p[i]; i++)
		putchar(p[i] >= 0x20 && p[i] < 0x7f ? p[i] : '?');
	putchar('\n');
}

static void print_failure(const char *key, int r)
{
	if (r == -2)
		printf("%s_error=qmi error %u\n", key, last_error);
	else
		printf("%s_error=%s\n", key, strerror(errno));
}

/* run one request against SERVICE; prints KEY_error on failure */
static int ask(const char *key, unsigned int service, uint16_t msg,
	       const uint8_t *tlv, size_t tlen, uint8_t *resp, size_t rmax)
{
	struct svc s;
	int r;

	if (find_service(service, &s)) {
		printf("%s_error=service %u not registered\n", key, service);
		return -1;
	}
	r = qmi_req(&s, msg, tlv, tlen, resp, rmax);
	if (r < 0)
		print_failure(key, r);
	return r;
}

/* ---- decoders: each takes a reply's TLV area -------------------------- */

static const char *const op_modes[] = {
	"online", "low_power", "factory_test", "offline", "reset",
	"shutting_down", "persistent_low_power", "mode_only_low_power",
};
static const char *const reg_states[] = {
	"not_registered", "registered", "searching", "denied", "unknown",
};
static const char *const attach[] = { "unknown", "attached", "detached" };
static const char *const card_states[] = { "absent", "present", "error" };
static const char *const app_states[] = {
	"unknown", "detected", "pin_required", "puk_required",
	"personalization_check", "pin_blocked", "illegal", "ready",
};
static const char *const pin_states[] = {
	"not_initialized", "enabled_not_verified", "enabled_verified",
	"disabled", "blocked", "permanently_blocked",
};

static const char *radio(int r)
{
	switch (r) {
	case 0: return "none";
	case 1: return "cdma1x";
	case 2: return "evdo";
	case 4: return "gsm";
	case 5: return "umts";
	case 8: return "lte";
	case 9: return "tdscdma";
	case 12: return "5gnr";
	default: return "unknown";
	}
}

static void dec_mode(const uint8_t *b, int len)
{
	unsigned int l;
	const uint8_t *v = tlv_get(b, len, 0x01, &l);

	if (v && l >= 1)
		printf("mode=%s\n", NAME(op_modes, v[0]));
}

/* UIM Get Card Status, TLV 0x10: the first card and its first application */
static void dec_card(const uint8_t *b, int len)
{
	unsigned int l;
	const uint8_t *v = tlv_get(b, len, 0x10, &l), *e;
	unsigned int cards, apps, aid;

	if (!v || l < 9) {
		printf("sim_error=no card status in the reply\n");
		return;
	}
	cards = v[8];
	if (!cards) {
		printf("sim=no_card\n");
		return;
	}
	e = v + l;
	v += 9;				/* card 0 */
	if (v + 6 > e)
		goto short_tlv;
	printf("sim=%s", NAME(card_states, v[0]));
	apps = v[5];
	v += 6;
	if (apps) {
		if (v + 7 > e)
			goto short_tlv;
		aid = v[6];
		if (v + 7 + aid + 7 > e)
			goto short_tlv;
		printf(" app=%s pin1=%s pin1_retries=%u",
		       NAME(app_states, v[1]), NAME(pin_states, v[7 + aid + 1]), v[7 + aid + 2]);
	}
	printf(" cards=%u\n", cards);
	return;
short_tlv:
	printf("\nsim_error=card status shorter than expected\n");
}

/* NAS Get Serving System: TLV 0x01 registration, 0x12 current PLMN */
static void dec_serving(const uint8_t *b, int len, int one_line)
{
	unsigned int l, n;
	const uint8_t *v = tlv_get(b, len, 0x01, &l);

	if (!v || l < 5) {
		printf("network_error=no serving system in the reply\n");
		return;
	}
	n = v[4];
	printf("%s%s", one_line ? "" : "registration=", NAME(reg_states, v[0]));
	if (!one_line)
		printf(" cs=%s ps=%s", NAME(attach, v[1]), NAME(attach, v[2]));
	for (unsigned int i = 0; i < n && 5 + i < l; i++)
		printf("%s%s", i ? "+" : (one_line ? ", " : " radio="), radio((int8_t)v[5 + i]));
	v = tlv_get(b, len, 0x12, &l);
	if (v && l >= 5) {
		unsigned int dl = v[4];

		printf("%s%03u-%02u", one_line ? ", " : " plmn=", v[0] | v[1] << 8, v[2] | v[3] << 8);
		if (dl && 5 + dl <= l) {
			printf(" ");
			for (unsigned int i = 0; i < dl; i++)
				putchar(v[5 + i] >= 0x20 && v[5 + i] < 0x7f ? v[5 + i] : '?');
		}
	}
	if (!one_line)
		putchar('\n');
}

/* NAS Get Signal Info: 0x14 LTE, 0x17 5G NR */
static void dec_signal(const uint8_t *b, int len, int one_line)
{
	unsigned int l;
	const uint8_t *v = tlv_get(b, len, 0x14, &l);

	if (v && l >= 6) {
		int rsrp = (int16_t)(v[2] | v[3] << 8), snr = (int16_t)(v[4] | v[5] << 8);

		if (one_line)
			printf(", RSRP %d dBm", rsrp);
		else
			printf("lte_signal=rssi %d dBm, rsrq %d dB, rsrp %d dBm, snr %.1f dB\n",
			       (int8_t)v[0], (int8_t)v[1], rsrp, snr / 10.0);
	}
	v = tlv_get(b, len, 0x17, &l);
	if (v && l >= 4 && !one_line)
		printf("nr5g_signal=rsrp %d dBm, snr %.1f dB\n",
		       (int16_t)(v[0] | v[1] << 8), (int16_t)(v[2] | v[3] << 8) / 10.0);
}

static void print_tlvs(const char *key, const uint8_t *b, int len)
{
	printf("%s=", key);
	for (int o = 0; o + 3 <= len;) {
		unsigned int t = b[o], l = b[o + 1] | b[o + 2] << 8;

		printf("%s%02x:", o ? " " : "", t);
		for (unsigned int i = 0; i < l && o + 3 + (int)i < len; i++)
			printf("%02x", b[o + 3 + i]);
		o += 3 + l;
	}
	putchar('\n');
}

/* ---- commands ---------------------------------------------------------- */

static int cmd_lookup(unsigned int service)
{
	struct svc list[128];
	int n = lookup(service, list, 128);

	for (int i = 0; i < n; i++)
		printf("%u %u %u %u %u\n", list[i].service, list[i].version,
		       list[i].instance, list[i].node, list[i].port);
	return n > 0 ? 0 : 1;
}

static void wda_format(const char *key, const uint8_t *tlv, size_t tlen)
{
	uint8_t r[2048];
	int n = ask(key, SVC_WDA, 0x0021, tlv, tlen, r, sizeof(r));

	if (n >= 0)
		print_tlvs(key, r, n);
}

static int cmd_info(void)
{
	static const uint8_t profile1[] = { 0x01, 0x02, 0x00, 0x00, 0x01 };	/* 3GPP, index 1 */
	/* endpoint: PCIe (3), interface 4, as the POCO F3 port binds it */
	static const uint8_t ep_pcie4[] = { 0x10, 0x08, 0x00, 3, 0, 0, 0, 4, 0, 0, 0 };
	uint8_t r[4096];
	unsigned int l;
	const uint8_t *v;
	int n;

	if ((n = ask("revision", SVC_DMS, 0x0023, NULL, 0, r, sizeof(r))) >= 0 &&
	    (v = tlv_get(r, n, 0x01, &l)))
		print_str("revision", v, l);
	if ((n = ask("mode", SVC_DMS, 0x002D, NULL, 0, r, sizeof(r))) >= 0)
		dec_mode(r, n);
	if ((n = ask("sim", SVC_UIM, 0x002F, NULL, 0, r, sizeof(r))) >= 0)
		dec_card(r, n);
	if ((n = ask("network", SVC_NAS, 0x0024, NULL, 0, r, sizeof(r))) >= 0)
		dec_serving(r, n, 0);
	if ((n = ask("signal", SVC_NAS, 0x004F, NULL, 0, r, sizeof(r))) >= 0)
		dec_signal(r, n, 0);
	/* the stored APN: name only (TLV 0x14), never the credentials */
	if ((n = ask("apn", SVC_WDS, 0x002B, profile1, sizeof(profile1), r, sizeof(r))) >= 0 &&
	    (v = tlv_get(r, n, 0x14, &l)))
		print_str("apn", v, l);
	wda_format("data_format", NULL, 0);
	wda_format("data_format_pcie4", ep_pcie4, sizeof(ep_pcie4));
	return 0;
}

static int cmd_network(void)
{
	uint8_t r[4096];
	struct svc s;
	int n;

	if (find_service(SVC_NAS, &s)) {
		printf("no network service\n");
		return 1;
	}
	n = qmi_req(&s, 0x0024, NULL, 0, r, sizeof(r));
	if (n < 0) {
		print_failure("network", n);
		return 1;
	}
	dec_serving(r, n, 1);
	n = qmi_req(&s, 0x004F, NULL, 0, r, sizeof(r));
	if (n >= 0)
		dec_signal(r, n, 1);
	putchar('\n');
	return 0;
}

static int cmd_online(void)
{
	static const uint8_t online[] = { 0x01, 0x01, 0x00, 0x00 };
	uint8_t r[256];
	int n = ask("online", SVC_DMS, 0x002E, online, sizeof(online), r, sizeof(r));

	if (n >= 0)
		printf("online=ok\n");
	return n < 0;
}

static int hexval(int c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
	       c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int cmd_raw(int argc, char **argv)
{
	uint8_t tlv[1024], r[8192];
	size_t tl = 0;
	unsigned int service = strtoul(argv[0], NULL, 0), msg = strtoul(argv[1], NULL, 0);
	int n;

	for (int i = 2; i < argc; i++) {
		char *colon = strchr(argv[i], ':');
		size_t start = tl;

		if (!colon || tl + 3 > sizeof(tlv))
			return 2;
		tlv[tl] = strtoul(argv[i], NULL, 16);
		tl += 3;
		for (const char *h = colon + 1; h[0] && h[1] && tl < sizeof(tlv); h += 2) {
			int a = hexval(h[0]), b = hexval(h[1]);

			if (a < 0 || b < 0)
				return 2;
			tlv[tl++] = a << 4 | b;
		}
		tlv[start + 1] = (tl - start - 3) & 0xff;
		tlv[start + 2] = (tl - start - 3) >> 8;
	}
	n = ask("raw", service, msg, tlv, tl, r, sizeof(r));
	if (n < 0)
		return 1;
	print_tlvs("reply", r, n);
	return 0;
}

static int cmd_dtr(const char *dev)
{
	int fd = open(dev, O_RDWR | O_CLOEXEC), bits = TIOCM_DTR | TIOCM_RTS;

	if (fd < 0 || ioctl(fd, TIOCMSET, &bits) < 0) {
		printf("dtr_error=%s\n", strerror(errno));
		return 1;
	}
	close(fd);
	printf("dtr=ok\n");
	return 0;
}

/* ---- data session -------------------------------------------------------- */

static int kmsg_fd = -1;

static void on_term(int sig)
{
	(void)sig;
	stopping = 1;
}

/* to stderr and syslog; with to_kmsg also to the kernel log (screen, pstore) */
static void say(int to_kmsg, const char *fmt, ...)
{
	char line[400];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fprintf(stderr, "%s\n", line);
	syslog(LOG_INFO, "%s", line);
	if (to_kmsg && kmsg_fd >= 0)
		dprintf(kmsg_fd, "mi10: modem: data: %s\n", line);
}

static size_t put_tlv(uint8_t *b, size_t o, unsigned int t, const void *v, size_t l)
{
	b[o] = t;
	b[o + 1] = l;
	b[o + 2] = l >> 8;
	memcpy(b + o + 3, v, l);
	return o + 3 + l;
}

static size_t put_u32(uint8_t *b, size_t o, unsigned int t, uint32_t v)
{
	uint8_t le[4] = { v, v >> 8, v >> 16, v >> 24 };

	return put_tlv(b, o, t, le, 4);
}

static size_t put_ep(uint8_t *b, size_t o, unsigned int t, uint32_t iface)
{
	/* endpoint: QMI_DATA_ENDPOINT_TYPE_PCIE (3), interface number */
	uint8_t ep[8] = { 3, 0, 0, 0, iface, iface >> 8, iface >> 16, iface >> 24 };

	return put_tlv(b, o, t, ep, 8);
}

static uint32_t get_u32(const uint8_t *b, int len, unsigned int t, int *ok)
{
	unsigned int l;
	const uint8_t *v = tlv_get(b, len, t, &l);

	*ok = v && l >= 4;
	return *ok ? (uint32_t)(v[0] | v[1] << 8 | v[2] << 16 | (uint32_t)v[3] << 24) : 0;
}

static void ip4(char *out, uint32_t a)
{
	sprintf(out, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
}

static int prefix_len(uint32_t mask)
{
	int n = 0;

	while (mask & 0x80000000u) {
		n++;
		mask <<= 1;
	}
	return n;
}

/* why a call failed or ended: TLV 0x10 (reason), 0x11 (verbose type, reason) */
static void end_reason(char *out, size_t sz, const uint8_t *b, int len)
{
	unsigned int l;
	const uint8_t *v = tlv_get(b, len, 0x10, &l), *w = tlv_get(b, len, 0x11, &l);

	snprintf(out, sz, "call end reason %d, verbose %d/%d",
		 v ? (int)(v[0] | v[1] << 8) : -1,
		 w ? (int)(w[0] | w[1] << 8) : -1, w ? (int16_t)(w[2] | w[3] << 8) : -1);
}

static int run_hook(const char *hook, char env[][64], int n)
{
	pid_t pid;
	int st;

	if (!hook)
		return 0;
	pid = fork();
	if (pid == 0) {
		for (int i = 0; i < n; i++)
			putenv(env[i]);
		execl("/bin/sh", "sh", "-c", hook, (char *)NULL);
		_exit(127);
	}
	if (pid < 0 || waitpid(pid, &st, 0) < 0)
		return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int cmd_connect(int argc, char **argv)
{
	const char *apn = NULL, *user = NULL, *pass = NULL, *hook = NULL;
	unsigned int iface = 4, mux = 1, auth = 0;
	struct svc wds, wda, dpm, dms;
	uint8_t req[512], r[4096];
	struct sigaction sa = { .sa_handler = on_term };
	char env[8][64], why[96], a[16], m[16], g[16], d1[16], d2[16];
	uint32_t handle = 0, ip, mask, gw, dns1, dns2, mtu;
	int n, ok, have_dns1, have_dns2, have_mtu, have_gw, opt, saved = timeout_ms;
	size_t o;

	optind = 1;
	while ((opt = getopt(argc, argv, "a:u:p:A:e:m:H:")) != -1) {
		switch (opt) {
		case 'a': apn = optarg; break;
		case 'u': user = optarg; break;
		case 'p': pass = optarg; break;
		case 'A': auth = !strcmp(optarg, "pap") ? 1 : !strcmp(optarg, "chap") ? 2 : 3; break;
		case 'e': iface = strtoul(optarg, NULL, 0); break;
		case 'm': mux = strtoul(optarg, NULL, 0); break;
		case 'H': hook = optarg; break;
		default: return usage();
		}
	}
	openlog("mi10-lte", 0, LOG_DAEMON);
	kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	if (find_service(SVC_WDS, &wds) || find_service(SVC_WDA, &wda)) {
		say(0, "the modem's data services are not registered");
		return 1;
	}

	/* operating mode online (the default after boot is low power) */
	if (!find_service(SVC_DMS, &dms)) {
		o = put_tlv(req, 0, 0x01, "\0", 1);
		qmi_req(&dms, 0x002E, req, o, r, sizeof(r));
	}

	/*
	 * Data port: the IP_HW0 MHI channels (100 up, 101 down) behind PCIe
	 * endpoint IFACE. The modem may already have it open; errors only logged.
	 */
	if (!find_service(SVC_DPM, &dpm)) {
		uint8_t port[17] = { 1, 3, 0, 0, 0, iface, 0, 0, 0, 101, 0, 0, 0, 100, 0, 0, 0 };

		o = put_tlv(req, 0, 0x11, port, sizeof(port));
		n = qmi_req(&dpm, 0x0020, req, o, r, sizeof(r));
		say(0, "DPM open port: %s", n >= 0 ? "ok" : n == -2 ? "qmi error" : strerror(errno));
	}

	/* raw IP inside QMAP both ways; the kernel's rmnet does the muxing */
	o = put_u32(req, 0, 0x11, 2);		/* link layer: raw IP */
	o = put_u32(req, o, 0x12, 5);		/* uplink: QMAP */
	o = put_u32(req, o, 0x13, 5);		/* downlink: QMAP */
	o = put_u32(req, o, 0x15, 32);		/* downlink: up to 32 packets... */
	o = put_u32(req, o, 0x16, 31744);	/* ...in 31 KiB (MHI MRU 32 KiB) */
	o = put_ep(req, o, 0x17, iface);
	n = qmi_req(&wda, 0x0020, req, o, r, sizeof(r));
	if (n < 0) {
		say(1, "setting the data format FAILED (%s %u)",
		    n == -2 ? "qmi error" : strerror(errno), n == -2 ? last_error : 0);
		return 1;
	}

	o = put_ep(req, 0, 0x10, iface);
	o = put_tlv(req, o, 0x11, (uint8_t[]){ mux }, 1);
	n = qmi_req(&wds, 0x00A2, req, o, r, sizeof(r));
	if (n < 0) {
		say(1, "binding mux %u FAILED (%s %u)", mux,
		    n == -2 ? "qmi error" : strerror(errno), n == -2 ? last_error : 0);
		return 1;
	}

	o = 0;
	if (apn && *apn)
		o = put_tlv(req, o, 0x14, apn, strlen(apn));
	if (user && *user) {
		o = put_tlv(req, o, 0x16, (uint8_t[]){ auth ? auth : 3 }, 1);
		o = put_tlv(req, o, 0x17, user, strlen(user));
		if (pass)
			o = put_tlv(req, o, 0x18, pass, strlen(pass));
	}
	o = put_tlv(req, o, 0x19, (uint8_t[]){ 4 }, 1);	/* IPv4 */
	say(1, "connecting (APN '%s')", apn && *apn ? apn : "network default");
	timeout_ms = 120000;	/* the network can take a while */
	n = qmi_req(&wds, 0x0020, req, o, r, sizeof(r));
	timeout_ms = saved;
	if (n < 0) {
		if (n == -2)
			end_reason(why, sizeof(why), r, last_len);
		say(1, "connecting FAILED: %s %u%s%s", n == -2 ? "qmi error" : strerror(errno),
		    n == -2 ? last_error : 0, n == -2 ? ", " : "", n == -2 ? why : "");
		return 1;
	}
	handle = get_u32(r, n, 0x01, &ok);

	/* PDP type, DNS, IP address, gateway, MTU, IP family */
	o = put_u32(req, 0, 0x10, 1u << 2 | 1u << 4 | 1u << 8 | 1u << 9 | 1u << 13 | 1u << 15);
	n = qmi_req(&wds, 0x002D, req, o, r, sizeof(r));
	if (n < 0) {
		say(1, "connected, but reading the settings FAILED");
		return 1;
	}
	ip = get_u32(r, n, 0x1E, &ok);
	if (!ok) {
		say(1, "connected, but the modem gave no IPv4 address");
		return 1;
	}
	gw = get_u32(r, n, 0x20, &have_gw);
	mask = get_u32(r, n, 0x21, &ok);
	if (!ok || !mask)
		mask = 0xffffffff;
	dns1 = get_u32(r, n, 0x15, &have_dns1);
	dns2 = get_u32(r, n, 0x16, &have_dns2);
	mtu = get_u32(r, n, 0x29, &have_mtu);
	ip4(a, ip);
	ip4(m, mask);
	ip4(g, have_gw ? gw : 0);
	ip4(d1, have_dns1 ? dns1 : 0);
	ip4(d2, have_dns2 ? dns2 : 0);
	n = 0;
	snprintf(env[n++], 64, "IP=%s", a);
	snprintf(env[n++], 64, "PREFIX=%d", prefix_len(mask));
	snprintf(env[n++], 64, "MASK=%s", m);
	snprintf(env[n++], 64, "GW=%s", have_gw ? g : "");
	snprintf(env[n++], 64, "DNS1=%s", have_dns1 && dns1 ? d1 : "");
	snprintf(env[n++], 64, "DNS2=%s", have_dns2 && dns2 ? d2 : "");
	snprintf(env[n++], 64, "MTU=%u", have_mtu && mtu ? mtu : 1500);
	say(1, "connected: %s/%d, DNS %s %s, MTU %u", a, prefix_len(mask),
	    have_dns1 ? d1 : "-", have_dns2 ? d2 : "-", have_mtu && mtu ? mtu : 1500);
	if (run_hook(hook, env, n))
		say(1, "the connect hook failed");

	/*
	 * Hold the session (it ends when this socket closes) and wait, without
	 * waking up periodically: for the modem's "disconnected" indication, for
	 * the data service going away, or for SIGTERM.
	 */
	for (;;) {
		struct sockaddr_qrtr from;
		struct pollfd p = { .fd = sock, .events = POLLIN };
		socklen_t sl = sizeof(from);
		ssize_t len;

		if (stopping) {
			if (handle) {
				o = put_u32(req, 0, 0x01, handle);
				timeout_ms = 5000;
				qmi_req(&wds, 0x0021, req, o, r, sizeof(r));
			}
			say(1, "disconnected (stopped)");
			return 0;
		}
		if (poll(&p, 1, -1) <= 0)
			continue;
		len = recvfrom(sock, r, sizeof(r), 0, (struct sockaddr *)&from, &sl);
		if (len < 0)
			continue;
		if (from.sq_port == QRTR_PORT_CTRL && len >= (ssize_t)sizeof(struct ctrl_pkt)) {
			struct ctrl_pkt *c = (struct ctrl_pkt *)r;

			if ((c->cmd == QRTR_DEL_SERVER && c->node == wds.node && c->port == wds.port) ||
			    (c->cmd == QRTR_BYE && c->service == wds.node)) {
				say(1, "disconnected: the modem's data service went away");
				return 1;
			}
			continue;
		}
		if (from.sq_node != wds.node || from.sq_port != wds.port || len < 7 ||
		    r[0] != 4 || (r[3] | r[4] << 8) != 0x0022)
			continue;
		{
			int l = r[5] | r[6] << 8;
			unsigned int tl;
			const uint8_t *st = tlv_get(r + 7, l > len - 7 ? len - 7 : l, 0x01, &tl);

			if (!st || tl < 1)
				continue;
			if (st[0] == 1 || (tl >= 2 && st[1])) {
				end_reason(why, sizeof(why), r + 7, l > len - 7 ? len - 7 : l);
				say(1, "disconnected by the network (%s)", why);
				return 1;
			}
		}
	}
}

static int usage(void)
{
	fprintf(stderr, "usage: mi10-qmi [-t MS] lookup [SERVICE] | info | network | online |\n"
		"       raw SERVICE MSG [TLV:HEX...] | dtr DEVICE |\n"
		"       connect [-a APN] [-u USER -p PASS [-A pap|chap|both]] [-e IFACE] [-m MUX] [-H HOOK]\n");
	return 2;
}

int main(int argc, char **argv)
{
	/* by hand: GNU getopt would reorder the subcommand's own options */
	argc--;
	argv++;
	if (argc >= 2 && !strcmp(argv[0], "-t")) {
		timeout_ms = atoi(argv[1]);
		argc -= 2;
		argv += 2;
	}
	if (argc < 1)
		return usage();
	if (!strcmp(argv[0], "dtr"))
		return argc == 2 ? cmd_dtr(argv[1]) : usage();
	if (open_sock())
		return 1;
	if (!strcmp(argv[0], "lookup"))
		return cmd_lookup(argc > 1 ? strtoul(argv[1], NULL, 0) : 0);
	if (!strcmp(argv[0], "info"))
		return cmd_info();
	if (!strcmp(argv[0], "network"))
		return cmd_network();
	if (!strcmp(argv[0], "online"))
		return cmd_online();
	if (!strcmp(argv[0], "connect"))
		return cmd_connect(argc, argv);
	if (!strcmp(argv[0], "raw") && argc >= 3)
		return cmd_raw(argc - 1, argv + 1);
	return usage();
}
