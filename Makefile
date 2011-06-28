# On Linux, one needs to define _POSIX_SOURCE or _XOPEN_SOURCE to get a
# declaration for kill(2), but on FreeBSD, defining _POSIX_SOURCE makes
# sys/types.h fail to define u_char/u_int etc., needed by netinet/ip.h.
# So _XOPEN_SOURCE it is, since that works in both places. Ironically,
# we also need _BSD_SOURCE (on Linux) for inet_aton.

CFLAGS = -D_BSD_SOURCE -D_XOPEN_SOURCE -Wall -ansi -pedantic

heaping: heaping.c

clean:
	rm heaping
