#include <sys/types.h>
#include <stdio.h>
#include <netdb.h>
#include <syslog.h>

#include <sys/socket.h>
#include <sys/time.h>

#include <netinet/in.h>

#include <arpa/inet.h>

#define	CONFIG_FILE	"/etc/holed.conf"
#define	DEBUG		if (debug) syslog

char	*config_file = CONFIG_FILE;
int	debug;

extern	char	*optarg;
extern	int	optind, opterr;

main(argc, argv)
int argc;
char **argv;
{
	register	int			i, numread, numready, width;
			fd_set			openfds, readfds;
			struct	sockaddr_in	us, them, *sip, *findcohort();
			struct	timeval		tv;
			struct	hostent		*hp;
			char			buf[BUFSIZ];
			int			s, len, bufsiz, working;

	openlog(argv[0], LOG_PID|LOG_CONS, LOG_DAEMON);

	bufsiz = opterr = 0;
	while ((i = getopt(argc, argv, "b:d")) != EOF) {
		switch (i) {
		case 'b':
			bufsiz = atoi(optarg);
			break;

		case 'd':
			debug++;
			break;

		default:
			syslog(LOG_ERR, "bad option");
			closelog();
			exit(1);
			break;
		}
	}

	if (optind < argc)
		config_file = argv[optind];

	len = sizeof(struct sockaddr_in);
	if (getsockname(0, &us, &len) < 0) {
		syslog(LOG_ERR, "getsockname failed: %m");
		closelog();
		exit(1);
	}

	if (getpeername(0, &them, &len) < 0) {
		syslog(LOG_ERR, "getpeername failed: %m");
		closelog();
		exit(1);
	}

	if ((hp = gethostbyaddr((char *)&them.sin_addr.s_addr,
	    sizeof(them.sin_addr.s_addr), AF_INET)) == (struct hostent *)NULL)
		syslog(LOG_NOTICE,
		    "accepted connection from unregistered host %s on port %d",
			inet_ntoa(them.sin_addr),
			ntohs(us.sin_port));
	else
		syslog(LOG_NOTICE, "accepted connection from %s:%s on port %d",
			hp->h_name,
			inet_ntoa(them.sin_addr),
			ntohs(us.sin_port));

	if ((sip = findcohort(ntohs(us.sin_port), &them.sin_addr)) == NULL) {
		syslog(LOG_ERR, "no cohort for %s/%d",
			inet_ntoa(them.sin_addr), ntohs(us.sin_port));
		closelog();
		exit(1);
	}

	if ((s = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
		syslog(LOG_ERR, "can't create socket: %m");
		closelog();
		exit(1);
	}

	if (bufsiz)
		Set_buffer_size(s, bufsiz);

	us.sin_addr.s_addr = sip->sin_addr.s_addr;
	us.sin_port = sip->sin_port;

	if (connect(s, &us, sizeof(us)) < 0) {
		syslog(LOG_ERR, "can't connect to %s: %m", hp->h_name);
		closelog();
		exit(1);
	}

	working = 2;
	width = s + 1;
	FD_ZERO(&openfds);
	FD_SET(0, &openfds);
	FD_SET(s, &openfds);
	tv.tv_sec = 1800;	/* select() times out in half hour */
	tv.tv_usec = 0;

	while (working) {
		readfds = openfds;
		switch (numready = select(width, &readfds, NULL, NULL, &tv)) {
		case 0:
			syslog(LOG_ERR, "select timed out");
			close(0);
			close(i);
			working = 0;
			break;
		case -1:
			syslog(LOG_ERR, "select failed: %m");
			closelog();
			exit(1);
			break;
		}
		DEBUG(LOG_DEBUG, "select fired, %d ready", numready);

		for (i = 0; i < width && numready; i++) {
			if (FD_ISSET(i, &readfds)) {
				DEBUG(LOG_DEBUG, "fd %d is ready", i);

				numready--;
				if ((numread = read(i, buf, BUFSIZ)) < 0) {
					syslog(LOG_ERR,
						"read failed %s: %m",
						inet_ntoa(i ? us.sin_addr : them.sin_addr));
					closelog();
					exit(1);
				}
				DEBUG(LOG_DEBUG, "read %d bytes", numread);

				if (numread)
					write((i ? 1: s), buf, numread);
				else if (i) {	/* EOF from the socket  */
					DEBUG(LOG_DEBUG, "EOF from socket");
					close(1);
					shutdown(0, 1);
					FD_CLR(s, &openfds);
					working--;
				}
				else {	/* EOF on stdin  */
					DEBUG(LOG_DEBUG, "EOF from stdin");
					shutdown(s, 1);
					FD_CLR(0, &openfds);
					working--;
				}
			}
		}
	}

	syslog(LOG_NOTICE, "exiting");

	closelog();

	exit(0);
}

struct sockaddr_in *
findcohort(port, fap)
unsigned short port;
struct in_addr *fap;
{
	register	char			**cpp;
	static		struct	sockaddr_in	*sip, sin;
			FILE			*fp;
			struct	hostent		*hp;
			struct	servent		*fromsep, *tosep;
			char			buf[BUFSIZ],
						fromservice[BUFSIZ],
						fromhost[BUFSIZ],
						tohost[BUFSIZ],
						toservice[BUFSIZ];

	if ((fromsep = getservbyport(port, "tcp")) == (struct servent *)NULL) {
		/* non-standard incoming port */
		fromsep->s_name = "01234567";
		sprintf(fromsep->s_name, "%d", port);
	}
	DEBUG(LOG_DEBUG, "service is %s", fromsep->s_name);

	if ((fp = fopen(config_file, "r")) == (FILE *)NULL) {
		syslog(LOG_ERR, "cannot open %s", config_file);
		return(NULL);
	}

	sip = (struct sockaddr_in *)NULL;
	while (fgets(buf, BUFSIZ, fp) != (char *)NULL) {
		DEBUG(LOG_DEBUG, "conf line '%s'", buf);

		if (buf[0] == '#' || buf[0] == '\n')
			continue;

		toservice[0] = '\0';
		sscanf(buf, "%s %s %s %s",
			fromservice, fromhost, tohost, toservice);

		/* is this the right service? */
		if (strcmp(fromservice, fromsep->s_name))
			continue;

		/* is this the right fromhost? */
		if ((hp = gethostbyname(fromhost)) == (struct hostent *)NULL) {
			syslog(LOG_ERR, "unknown host: %s", fromhost);
			continue;
		}
		DEBUG(LOG_DEBUG, "remote host is %s", hp->h_name);

		for (cpp = hp->h_addr_list; *cpp; cpp++)
			if (bcmp((char *)&fap->s_addr, *cpp, hp->h_length) == 0)
				break;

		if (*cpp == (char *)NULL) {
			hp = (struct hostent *)NULL;
			continue;
		}
		DEBUG(LOG_DEBUG, "matched remote host");

		/* service and fromhost match */
		if ((hp = gethostbyname(tohost)) == (struct hostent *)NULL) {
			syslog(LOG_ERR, "unknown host: %s", tohost);
			continue;
		}
		bcopy(hp->h_addr_list[0], (char *)&sin.sin_addr.s_addr, hp->h_length);

		sin.sin_port = htons(port);
		if (toservice[0]) {
			if (isdigit(toservice[0]))
				sin.sin_port = htons(atoi(toservice));
			else if ((tosep = getservbyname(toservice, "tcp")) == (struct servent *)NULL) {
				syslog(LOG_ERR, "unknown service: %s", toservice);
				continue;
			}
			else
				sin.sin_port = tosep->s_port;
		}
		sip = &sin;
		syslog(LOG_NOTICE, "bridging to %s:%s:%d",
			hp->h_name, inet_ntoa(sin.sin_addr), ntohs(sin.sin_port));

		break;
	}

	fclose(fp);

	return(sip);
}

/*
 * set all buffers to a specific size
 */
Set_buffer_size(s, bufsiz)
int s, bufsiz;
{
	int	len;

	DEBUG(LOG_DEBUG, "setting buffers to %d", bufsiz);

	len = sizeof(bufsiz);
	if (setsockopt(0, SOL_SOCKET, SO_RCVBUF, (char *)&bufsiz, len) < 0) {
		syslog(LOG_ERR, "setsockopt SO_RCVBUF failed on stdin: %m");
		closelog();
		exit(1);
	}
	if (setsockopt(1, SOL_SOCKET, SO_SNDBUF, (char *)&bufsiz, len) < 0) {
		syslog(LOG_ERR, "setsockopt SO_SNDBUF failed on stdout: %m");
		closelog();
		exit(1);
	}

	if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)&bufsiz, len) < 0) {
		syslog(LOG_ERR,
			"setsockopt SO_RCVBUF failed on outgoing socket: %m");
		closelog();
		exit(1);
	}
	if (setsockopt(s, SOL_SOCKET, SO_SNDBUF, (char *)&bufsiz, len) < 0) {
		syslog(LOG_ERR,
			"setsockopt SO_SNDBUF failed on outgoing socket: %m");
		closelog();
		exit(1);
	}
}
