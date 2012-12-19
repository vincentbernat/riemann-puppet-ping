/*	$OpenBSD: check_icmp.c,v 1.33 2012/09/19 09:49:24 reyk Exp $	*/

/*
 * Copyright (c) 2006,2012 Pierre-Yves Ritschard <pyr@spootnik.org>
 * Copyright (c) 2012 Exoscale
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <net/if.h>
#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/icmp6.h>
#include <arpa/inet.h>

#include <err.h>
#include <fcntl.h>
#include <limits.h>
#include <event.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#define ICMP_BUF_SIZE 64

struct ping {
	u_int16_t		 id;
	u_int32_t		 lastid;
	int			 send;
	int			 recv;
	struct ping_res		*head;
	struct timeval		 timeout;
	struct timeval		 start;
	struct timeval		 tv;
	struct event_base	*base;
	struct event		 send_ev;
	struct event		 recv_ev;
};

struct ping_res {
	struct sockaddr_storage	 ss;
#define F_SENT			 0x01
#define F_DONE			 0x02
#define F_UNREACHABLE		 0x04
#define	F_UP			 0x08
	u_int32_t		 id;
	u_int8_t		 flags;
	double			 latency;
	void			*data;
	struct ping_res		*next;
};

struct ping	*ping_new(int);
void		 ping_schedule(struct ping *, const char *, void *);
void		 ping_run(struct ping *);
struct ping_res	*ping_first(struct ping *);
struct ping_res	*ping_next(struct ping_res *);
void		*ping_data(struct ping_res *);
u_int8_t	 ping_info(struct ping_res *, double *);
void		 ping_free(struct ping *, void (*free_cb)(void *));

int		 ping_all_done(struct ping *);
void		 ping_done(struct ping *);

void		 ping_send(int, short, void *);
void		 ping_recv(int, short, void *);
void		 ping_next_timeout(struct timeval *, struct timeval *,
				   struct timeval *);
int		 in_cksum(u_short *, int);


/* from log.c */
void	 log_init(const char *, int);
void	 log_warn(const char *, ...);
void	 log_warnx(const char *, ...);
void	 log_info(const char *, ...);
void	 log_debug(const char *, ...);
void	 fatal(const char *);
void	 fatalx(const char *);

struct ping_res *
ping_first(struct ping *ping)
{
	return ping->head;
}

struct ping_res *
ping_next(struct ping_res *res)
{
	return res->next;
}

void
ping_free(struct ping *ping, void (*free_fn)(void *))
{
	struct ping_res	*current;
	struct ping_res	*next;

	current = ping->head;

	while (current != NULL) {
		next = current->next;
		if (free_fn)
			free_fn(current->data);
		free(current);
		current = next;
	}
}

void *
ping_data(struct ping_res *res)
{
	return res->data;
}

u_int8_t
ping_info(struct ping_res *res, double *latency)
{
	memcpy(latency, &res->latency, sizeof(*latency));
	return res->flags;
}

struct ping *
ping_new(int timeout)
{
	int		 flags;
	struct ping	*ping;

	if ((ping = calloc(1, sizeof(*ping))) == NULL)
		err(1, "calloc for ping");
	
	if ((ping->send = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)) == -1) {
		err(1, "cannot create icmp send socket");
	}
	if ((flags = fcntl(ping->send, F_GETFL, 0)) == -1) {
		close(ping->send);
		err(1, "fcntl F_GETFL in icmp_init for send");
	}
	flags |= O_NONBLOCK;
	if (fcntl(ping->send, F_SETFL, &flags) == -1) {
		close(ping->send);
		err(1, "fcntl F_SETFL in icmp_init for send");
	}

	if ((ping->recv = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)) == -1) {
		close(ping->send);
		err(1, "cannot create icmp recv socket");
	}
	if ((flags = fcntl(ping->recv, F_GETFL, 0)) == -1) {
		close(ping->send);
		close(ping->recv);
		err(1, "fcntl F_GETFL in icmp_init for recv");
	}
	flags |= O_NONBLOCK;
	if (fcntl(ping->recv, F_SETFL, &flags) == -1) {
		close(ping->send);
		close(ping->recv);
		err(1, "fcntl F_SETFL in icmp_init for recv");
	}
	
	ping->timeout.tv_sec = timeout;
	ping->timeout.tv_usec = 0;
	ping->id = getpid() & 0xffff;
	return ping;
}

void
ping_schedule(struct ping *ping,  const char *hostname, void *data)
{
	int		 e;
	struct ping_res	*res;
	struct addrinfo	*ai, hints;

	if ((res = calloc(1, sizeof(*res))) == NULL) {
		err(1, "calloc ping_res");
	}

	res->data = data;

	bzero(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_flags = 0;
	hints.ai_protocol = 0;

	if ((e = getaddrinfo(hostname, NULL, &hints, &ai)) != 0) {
		res->flags |= F_UNREACHABLE;
		log_warnx("resolver failed: %s", gai_strerror(e));
		goto out;
	}

	if (ai == NULL) {
		res->flags |= F_UNREACHABLE;
		log_warnx("host not found: %s", hostname);
		goto out;
	}

	log_debug("storing resolved address");
	memcpy(&res->ss, ai->ai_addr, ai->ai_addrlen);
	res->id = ping->lastid++; /* wraps around */
	freeaddrinfo(ai);

 out:
	/* add this new ping_res to our list */
	res->next = ping->head;
	ping->head = res;
}

void
ping_run(struct ping *ping)
{
	ping->base = event_base_new();

	if (gettimeofday(&ping->start, NULL) == -1)
		err(1, "gettimeofday");
	
	event_base_once(ping->base, ping->send,
			EV_WRITE|EV_TIMEOUT, ping_send,
			ping, &ping->timeout);
	event_base_once(ping->base, ping->recv,
			EV_READ|EV_TIMEOUT, ping_recv,
			ping, &ping->timeout);
	event_base_loop(ping->base, 0);
	event_base_free(ping->base);
}

int
ping_all_done(struct ping *ping)
{
	struct ping_res	*res;

	for (res = ping->head;
	     res != NULL && res->flags & (F_UNREACHABLE|F_DONE);
	     res = res->next)
		;
	return (res == NULL);
}

void
ping_done(struct ping *ping)
{
	struct timeval tv;

	tv.tv_sec = 0;
	tv.tv_usec = 0;
	event_loopexit(&tv);
}

void
ping_send(int s, short event, void *arg)
{
	struct ping	*ping = arg;
	struct ping_res	*res;
	struct icmp	*icp;
	struct sockaddr	*to;
	struct timeval	 tv;
	ssize_t		 r;
	u_char		 packet[ICMP_BUF_SIZE];
	socklen_t	 slen;
	int		 i = 0;
	u_int32_t	 id;

	if (event == EV_TIMEOUT)
		ping_done(ping);

	bzero(&packet, sizeof(packet));
	icp = (struct icmp *)packet;

	icp->icmp_type = ICMP_ECHO;
	icp->icmp_code = 0;
	icp->icmp_id = htons(ping->id);
	icp->icmp_cksum = 0;
	slen = sizeof(struct sockaddr_in);

	for (res = ping->head; res != NULL; res = res->next) {
		if (res->flags & (F_SENT | F_UNREACHABLE))
			continue;
		i++;
		to = (struct sockaddr *)&res->ss;
		id = htonl(res->id);
		
		icp->icmp_seq = htons(i);
		icp->icmp_cksum = 0;
		icp->icmp_mask = id;
		icp->icmp_cksum = in_cksum((u_short *)icp, sizeof(packet));

		r = sendto(s, packet, sizeof(packet), 0, to, slen);
		if (r == -1) {
			if (errno == EAGAIN || errno == EINTR)
				goto retry;
			res->flags |= F_SENT|F_DONE;
		} else if (r != sizeof(packet))
			goto retry;
		res->flags |= F_SENT;
	}
	return;
 retry:
	ping_next_timeout(&ping->start, &ping->timeout, &tv);
	event_base_once(ping->base, ping->send, EV_TIMEOUT|EV_READ,
			ping_send, ping, &tv);
}

void
ping_recv(int s, short event, void *arg)
{
	struct ping		*ping = arg;
	u_char			 packet[ICMP_BUF_SIZE];
	socklen_t		 slen;
	struct sockaddr_storage	 ss;
	struct icmp		*icp;
	struct timeval		 tv;
	u_int16_t		 icpid;
	struct ping_res		*res;
	ssize_t			 r;
	u_int32_t		 id;

	if (event == EV_TIMEOUT) {
		ping_done(ping);
		return;
	}
	
	bzero(&packet, sizeof(packet));
	bzero(&ss, sizeof(ss));
	slen = sizeof(ss);

	r = recvfrom(s, packet, sizeof(packet), 0,
	      (struct sockaddr *)&ss, &slen);

	if (r == -1 || r != ICMP_BUF_SIZE) {
		log_warnx("short read on icmp packet");
		goto retry;
	}

	icp = (struct icmp *)(packet + sizeof(struct ip));
	icpid = ntohs(icp->icmp_id);
	id = icp->icmp_mask;

	if (icpid != ping->id)
		goto retry;
	id = ntohl(id);

	for (res = ping->head;
	     res != NULL && res->id != id;
	     res = res->next)
		;
	if (res == NULL) {
		log_warnx("stray icmp packet received");
		goto retry;
	}

	if (memcmp(&res->ss, &ss, sizeof(struct sockaddr_in)) != 0) {
		log_warnx("spoofed icmp packet?");
		goto retry;
	}

	res->flags |= (F_DONE|F_UP);

	if (gettimeofday(&tv, NULL) == -1)
		err(1, "gettimeofday");

	timersub(&tv, &ping->start, &tv);
	res->latency = ((tv.tv_sec & 0xffffffff) * 1000000000.0 
			+ (tv.tv_usec & 0xffffffff)) * 0.001;
	
	if (ping_all_done(ping))
		return;

 retry:
	ping_next_timeout(&ping->start, &ping->timeout, &tv);
	event_base_once(ping->base, ping->recv, EV_TIMEOUT|EV_READ,
			ping_recv, ping, &tv);
}

void
ping_next_timeout(struct timeval *start, struct timeval *timeout,
		  struct timeval *new_timeout)
{
	
	struct timeval tv_next, tv_now, tv_zero;

	bzero(new_timeout, sizeof(*new_timeout));

	if (gettimeofday(&tv_now, NULL) == -1)
		fatal("event_again: gettimeofday");

	/* new_timeout = timeout - (now - start) */
	bcopy(timeout, &tv_next, sizeof(tv_next));
	timersub(&tv_now, start, &tv_now);
	timersub(&tv_next, &tv_now, new_timeout);

	bzero(&tv_zero, sizeof(tv_zero));
	if (timercmp(&tv_zero, new_timeout, >))
		bcopy(&tv_zero, new_timeout, sizeof(*new_timeout));
}

/* in_cksum from ping.c --
 *	Checksum routine for Internet Protocol family headers (C Version)
 *
 * Copyright (c) 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Mike Muuss.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
int
in_cksum(u_short *addr, int len)
{
	int nleft = len;
	u_short *w = addr;
	int sum = 0;
	u_short answer = 0;

	/*
	 * Our algorithm is simple, using a 32 bit accumulator (sum), we add
	 * sequential 16 bit words to it, and at the end, fold back all the
	 * carry bits from the top 16 bits into the lower 16 bits.
	 */
	while (nleft > 1)  {
		sum += *w++;
		nleft -= 2;
	}

	/* mop up an odd byte, if necessary */
	if (nleft == 1) {
		*(u_char *)(&answer) = *(u_char *)w ;
		sum += answer;
	}

	/* add back carry outs from top 16 bits to low 16 bits */
	sum = (sum >> 16) + (sum & 0xffff);	/* add hi 16 to low 16 */
	sum += (sum >> 16);			/* add carry */
	answer = ~sum;				/* truncate to 16 bits */

	return (answer);
}
