/*
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
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <err.h>
#include <getopt.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "riemann.pb-c.h"

#define PROGNAME		"riemann-puppet-ping"
#define DEFAULT_INTERVAL	30
#define DEFAULT_DELAY		2.0
#define DEFAULT_CONFIG		"/etc/riemann-puppet-ping.conf"
#define DEFAULT_RIEMANN_HOST	"localhost"
#define DEFAULT_RIEMANN_PORT	5555
#define DEFAULT_REPORTS_DIR	"/var/lib/puppet/reports"
#define MAXLEN		        1024
#define MAXTAGS			32

/*
 * riemann-puppet-ping is a health check agent
 * for your puppet nodes.
 *
 * it will simply pull in the list of nodes
 * having reported to puppet at least once and
 * query them through icmp.
 *
 * See https://github.com/exoscale/riemann-puppet-ping
 */

enum state {
	S_OK = 0,
	S_WARNING = 1,
	S_CRITICAL = 2,
	S_UNKNOWN = 3
};

struct msg {
	u_char	*buf;
	int	 len;
};

struct env {
	int	 flags;
	int	 s;                        /* riemann connection */
	int	 interval;                 /* ping interval */
	double	 delay;                    /* ttl = interval + delay */
	int	 tagcount;                 /* number of tags to add */
	char	*tags[MAXTAGS];            /* tags */
	char	 reports_dir[MAXPATHLEN];  /* path to puppet reports */
	char	 host[MAXLEN];             /* riemann host */
	int	 port;			   /* riemann service port */
};

/* from icmp.c */
struct ping;
struct ping_res;
#define F_SENT		0x01
#define F_DONE		0x02
#define F_UNREACHABLE	0x04
#define	F_UP		0x08
struct ping	*ping_new(int);
void		 ping_schedule(struct ping *, const char *, void *);
void		 ping_run(struct ping *);
struct ping_res	*ping_first(struct ping *);
struct ping_res	*ping_next(struct ping_res *);
void		*ping_data(struct ping_res *);
u_int8_t	 ping_info(struct ping_res *, double *);
void		 ping_free(struct ping *, void (*free_cb)(void *));

/* from log.c */
void	 log_init(const char *, int);
void	 log_warn(const char *, ...);
void	 log_warnx(const char *, ...);
void	 log_info(const char *, ...);
void	 log_debug(const char *, ...);
void	 fatal(const char *);
void	 fatalx(const char *);

void	 usage(const char *);
void	 puppet_ping(struct env *, struct msg *);
void     puppet_ping_event(struct env *, struct ping_res *);
void	 riemann_send(struct env *, struct msg *);
void	 event_free(void *);

void
event_free(void *p)
{
	Event	*ev = p;

	free(ev->host);
	free(ev);
}
/*
 * construct a valid riemann event from oping's output
 */
void
puppet_ping_event(struct env *env, struct ping_res *res)
{
	u_int8_t	 flags;
	double		 latency;
	Event		*ev;

	ev = ping_data(res);
	flags = ping_info(res, &latency);

	ev->has_time = 1;
	ev->time = time(NULL);
	ev->service = "ping";
	ev->description = "ping latency";
	ev->n_tags = env->tagcount;
	ev->tags = env->tags;
	ev->has_ttl = 1;
	ev->ttl = env->interval + env->delay;
	ev->has_metric_f = 1;
	ev->metric_f = latency;
	
	if (flags & F_UP) {
		ev->state = "ok";
	} else if (flags & F_SENT) {
		ev->state = "critical";
		ev->description = "ping timed out";
	} else if (flags & F_UNREACHABLE) {
		ev->state = "critical";
		ev->description = "unreachable host";
	}
}

void
puppet_ping(struct env *env, struct msg *m)
{
	int		 i;
	DIR		*dir = NULL;
	struct dirent	*dent = NULL;
	Msg		 msg = MSG__INIT;
	Event		*ev;
	struct ping	*ping;
	struct ping_res	*res;

	if ((ping = ping_new(env->interval / 2)) == NULL) {
		log_warn("cannot create ping object");
	}
	if ((dir = opendir(env->reports_dir)) == NULL) {
		log_warn("cannot open reports dir \"%s\"", env->reports_dir);
		goto out;
	}

	/*
	 * treat all directories as resolvable host names
	 */
	msg.n_events = 0;
	for (dent = readdir(dir); dent != NULL; dent = readdir(dir)) {
		if (dent->d_type == DT_DIR && dent->d_name[0] != '.') {
			if ((ev = calloc(1, sizeof(*ev))) == NULL) {
				err(1, "calloc");
			}
			log_debug("adding host: %s", dent->d_name);
			event__init(ev);
			if ((ev->host = strdup(dent->d_name)) == NULL)
				err(1, "stdup for hostname");
			ping_schedule(ping, dent->d_name, ev);
			msg.n_events++;
		}
	}
	closedir(dir);

	if ((msg.events = calloc(msg.n_events, sizeof(*msg.events))) == NULL) {
		log_warn("cannot allocate event tab");
		goto out;
	}

	for (i = 0, res = ping_first(ping);
	     res != NULL;
	     i++, res = ping_next(res)) {
		msg.events[i] = ping_data(res);
	}

	if (i != msg.n_events)
		errx(1, "inconsistent data structures");

	ping_run(ping);

	for (i = 0, res = ping_first(ping);
	     res != NULL;
	     i++, res = ping_next(res)) {
		puppet_ping_event(env, res);
	}

	if (i != msg.n_events) 
		errx(1, "inconsistent data structures in reply");

	m->len = msg__get_packed_size(&msg);
	m->buf = calloc(1, m->len);
	msg__pack(&msg, m->buf);

 out:
	if (ping)
		ping_free(ping, event_free);
	if (msg.n_events) /* individual events are freed by ping_free */
		free(msg.events);
	return;
}

/*
 * Attempt to get a socket connected to a riemann server
 */
int
riemann_connect(struct env *env)
{
	int			 e;
	struct addrinfo		*res, *ai, hints;
	struct sockaddr_in	*sin;

	if (env->flags & F_UP)
		return 0;

	bzero(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_flags = 0;
	hints.ai_protocol = 0;

	if ((e = getaddrinfo(env->host, NULL, &hints, &res)) != 0) {
		log_warnx("cannot lookup: %s: %s", env->host, gai_strerror(e));
		return -1;
	}

	for (ai = res; ai != NULL; ai = ai->ai_next) {
		if ((env->s = socket(ai->ai_family, ai->ai_socktype,
				     ai->ai_protocol)) == -1)
			continue;
		/* ugly cast to force port to our value */
		sin = (struct sockaddr_in *)ai->ai_addr;
		sin->sin_port = htons(env->port);
		if (connect(env->s, ai->ai_addr, ai->ai_addrlen) != -1)
			break;

		close(env->s);
	}

	freeaddrinfo(res);
	if (ai == NULL)
		return -1;
	return 0;
}

/*
 * Atomically send a riemann event
 */
void
riemann_send(struct env *env, struct msg *msg)
{
	int	nlen;

	if (riemann_connect(env) == -1) {
		log_warnx("could not connect to riemann %s:%s",
			  env->host, env->port);
		free(msg->buf);
		return;
	}

	nlen = htonl(msg->len);

	/* TODO: loop through write */
	if (write(env->s, &nlen, sizeof(nlen)) != sizeof(nlen)) {
		log_warnx("short write for len");
		close(env->s);
		env->flags |= ~F_UP;
		goto out;
	}

	/* TODO: loop through write */
	if (write(env->s, msg->buf, msg->len)  != msg->len) {
		log_warnx("short write for buf");
		close(env->s);
		env->flags |= ~F_UP;
	}

 out:
	free(msg->buf);
	return;
}

/* Bail */
void
usage(const char *progname)
{
	fprintf(stderr, "usage: %s [-d] [-f configfile]\n",
		progname);
	_exit(1);
}

int
main(int argc, char *argv[])
{
	int			 c;
	int			 debug = 0;
	FILE			*fd;
	const char		*progname = argv[0];
	char			*key, *val;
	struct timeval	         start_tv, end_tv, tv;
	size_t			 len, read;
	char			 config[MAXLEN];
	char			 confline[MAXLEN];
	struct env		 env;
	struct msg		 msg;

	bzero(&env, sizeof(env));
	bzero(&msg, sizeof(msg));
	bzero(config, sizeof(config));
	strncpy(config, DEFAULT_CONFIG, strlen(DEFAULT_CONFIG) + 1);

	/* fill in default values */
	env.port = DEFAULT_RIEMANN_PORT;
	env.interval = DEFAULT_INTERVAL;
	env.delay = DEFAULT_DELAY;
	strncpy(env.host, DEFAULT_RIEMANN_HOST,
		strlen(DEFAULT_RIEMANN_HOST) + 1);
	strncpy(env.reports_dir, DEFAULT_REPORTS_DIR,
		strlen(DEFAULT_REPORTS_DIR) + 1);

	/* parse command line opts */
	while ((c = getopt(argc, argv, "df:")) != -1) {
		switch (c) {
		case 'd':
			debug += 1;
			break;
		case 'f':
			if (strlen(optarg) >= MAXLEN)
				errx(1, "config too wide");
			strncpy(config, optarg, strlen(optarg) + 1);
			break;
		default:
			usage(progname);
		}
	}

	argc -= optind;
	argv += optind;
	
	log_init(progname, debug + 1);
	/*
	 * ghetto parser for a simple key = value config file format
	 */
	if ((fd = fopen(config, "r")) == NULL)
		err(1, "cannot open configuration");

	len = 0;
	while ((read = getline(&key, &len, fd)) != -1) {
		if (len >= MAXLEN)
			err(1, "config line too wide");
		strncpy(confline, key, len);

		if ((confline[0] == '#') || (confline[0] == '\n'))
			continue;
		key = confline + strspn(confline, " \t");
		val = confline + strcspn(confline, "=") + 1;
		key[strcspn(key, " \t=")] = '\0';
		val += strspn(val, " \t");
		val[strcspn(val, "\n")] = '\0';

		log_debug("configuration parsed key = %s, val = %s", key, val);

		/*
		 * not doing any boundary check here since we
		 * already asserted that the line length was
		 * not wider than MAXLEN
		 */
		if (strcasecmp(key, "riemann_host") == 0) {
			strncpy(env.host, val, strlen(val) + 1);
		} else if (strcasecmp(key, "riemann_port") == 0) {
			env.port = atoi(val);
		} else if (strcasecmp(key, "interval") == 0) {
			env.interval = atoi(val);
		} else if (strcasecmp(key, "delay") == 0) {
			env.delay = atof(val);
		} else if (strcasecmp(key, "reports_dir") == 0) {
			strncpy(env.reports_dir, val, strlen(val) + 1);
		} else if (strcasecmp(key, "tag") == 0) {
			if (env.tagcount >= MAXTAGS)
				errx(1, "too many tags");
			/*
			 * this is not freed but who cares ?
			 */
			env.tags[env.tagcount++] = strdup(val);
		} else {
			errx(1, "invalid configuration directive: %s", key);
		}
	}
	
	/* sanity checks */
	if (env.interval <= 0)
		usage(progname);

	log_init(PROGNAME, debug);
	log_info("starting " PROGNAME " main loop");

	/* main loop */
	while (1) {
		bzero(&tv, sizeof(tv));
		if (gettimeofday(&start_tv, NULL) == -1)
			err(1, "gettimeofday");
		bzero(&msg, sizeof(msg));
		log_debug("gathering statistics");
		puppet_ping(&env, &msg);
		/*
		 * Set these fields systematically
		 */
		log_debug("sending riemann message");
		riemann_send(&env, &msg);
		if (gettimeofday(&end_tv, NULL) == -1)
			err(1, "gettimeofday");

		tv.tv_sec = env.interval;
		tv.tv_usec = 0;

		/* waitfor = interval - (end - start) */
		timersub(&end_tv, &start_tv, &end_tv);
		timersub(&tv, &end_tv, &tv);

		bzero(&end_tv, sizeof(end_tv));
		log_debug("got wait interval: %ld.%ld",
			  tv.tv_sec, tv.tv_usec);
		if (timercmp(&tv, &end_tv, >))
			select(0, NULL, NULL, NULL, &tv);

	}

	return 0;
}
