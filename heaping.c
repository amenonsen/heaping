/*
 * heaping -- Ping a list of addresses forever
 * http://github.com/amenonsen/heaping
 *
 * Copyright 2011 Abhijit Menon-Sen <ams@toroid.org>
 *
 * You may use, modify, or redistribute this program freely, but please
 * retain the copyright notice, and clearly identify modified versions
 * as being different from the original.
 *
 * There is no warranty.
 */

#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static pid_t pid;
static int killed;
static int child_died;

void pong(int);
void ping(struct in_addr *, int, long);
uint16_t icmp_checksum(uint16_t *, int);
void describe(unsigned char *, int, struct sockaddr_in *);
int ms_between(struct timeval *, struct timeval *);

void sigchld_handler(int sig)
{
    child_died = 1;
}

void kill_handler(int sig)
{
    killed = 1;
}

int main(int ac, char *av[])
{
    int i, j, raw;
    long int n = 0;
    struct in_addr * hosts;
    struct sigaction sa;

    i = 1;

    while (ac > i && *av[i] == '-') {
        if (strcmp(av[i], "-n") == 0) {
            char *end = 0;

            i++;
            n = strtol(av[i], &end, 10);
            if (n <= 0 || *end != '\0') {
                fprintf(
                    stderr, "Couldn't parse '%s' as a positive number\n", av[i]
                );
                exit(-1);
            }
        }
        else {
            fprintf(stderr, "Unrecognised option: '%s'", av[i]);
            exit(-1);
        }

        i++;
    }

    if (ac <= i) {
        fprintf(stderr, "Usage: heaping [-n NNN] <ip> [ip ...]\n");
        exit(0);
    }

    hosts = malloc((ac-i+1) * sizeof(struct in_addr));
    if (!hosts) {
        fprintf(stderr, "Couldn't malloc in_addr[%d]\n", ac);
        exit(-1);
    }

    j = 0;
    while (i < ac) {
        if (inet_aton(av[i], hosts+j) == 0) {
            fprintf(stderr, "Couldn't parse '%s' as IP address\n", av[i]);
            exit(-1);
        }
        i++; j++;
    }
    hosts[j].s_addr = INADDR_NONE;

    raw = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (raw < 0) {
        perror("socket(SOCK_RAW)");
        exit(-1);
    }

    setlinebuf(stdout);
    setlinebuf(stderr);

    sigemptyset(&sa.sa_mask);
    sa.sa_handler = kill_handler;

    /* We don't want our child process to restart syscalls when we're
       trying to interrupt it. */

    sa.sa_flags = SA_RESETHAND;
    if (sigaction(SIGTERM, &sa, NULL) == -1 ||
        sigaction(SIGINT, &sa, NULL) == -1)
    {
        perror("sigaction(SIGTERM/INT)");
        exit(-1);
    }

    /* SIGCHLD doesn't need to interrupt any syscalls. */

    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction(SIGCHLD)");
        exit(-1);
    }

    /* Run ping in the parent (i.e. this) process, and receive results
       in the child process. */

    pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(-1);
    }
    else if (pid) {
        ping(hosts, raw, n);
    }
    else {
        pong(raw);
    }

    return 0;
}

void ping(struct in_addr *hosts, int raw, long num)
{
    /* We send echo requests with the 8-byte ICMP header and a 16-byte
     * (on x86_64) struct timeval. */

    int sz = 24;
    unsigned char pkt[24];

    struct icmp *icp = (struct icmp *)pkt;
    struct timeval *tp = (struct timeval *)(pkt+8);

    int n, seq;
    struct timezone tz;
    struct sockaddr_in to;

    /* Fill in the parts of the packet that never change. */

    memset((char *)&to, 0, sizeof(to));
    to.sin_family = AF_INET;

    icp->icmp_type = ICMP_ECHO;
    icp->icmp_code = 0;
    icp->icmp_id = htons(pid & 0xFFFF);

    /* Fill any space after the timestamp with data. */

    n = 8 + sizeof(struct timeval);
    while (n < sz) {
        *(pkt+n) = (char)n;
        n++;
    }

    /* Send one echo to every host every 10s, forever. */

    seq = 0;
    while (!child_died && !killed && (num == 0 || seq < num)) {
        int i = 0;
        struct timeval a, b;

        icp->icmp_seq = htons(seq);
        printf("meta: new cycle (seq=%d)\n", seq);
        seq++;

        gettimeofday(&a, &tz);

        while (hosts[i].s_addr != INADDR_NONE) {
            int n;

            gettimeofday(tp, &tz);
            icp->icmp_cksum = icmp_checksum((uint16_t *)pkt, sz);

            to.sin_addr.s_addr = hosts[i].s_addr;
            n = sendto(raw, pkt, sz, 0, (struct sockaddr *)&to,
                       sizeof(struct sockaddr));
            if (n < 0) {
                printf("sendto(%s): %s\n", inet_ntoa(hosts[i]), strerror(errno));
            }

            i++;
        }

        gettimeofday(&b, &tz);

        printf("meta: sent %d pings in %d ms\n", i, ms_between(&b, &a));

        sleep(10);
    }

    if (!child_died) {
        kill(pid, SIGTERM);
    }
}


void pong(int raw)
{
    unsigned char pkt[64];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    pid = getpid();
    while (!killed) {
        int n = recvfrom(raw, pkt, sizeof(pkt), 0,
                         (struct sockaddr *)&from, &fromlen);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            perror("recvfrom");
        }
        else if (n > 0) {
            describe(pkt, n, &from);
        }
    }
}


void describe(unsigned char *pkt, int len, struct sockaddr_in *from)
{
    struct ip *ip = (struct ip *)pkt;
    struct icmp *icp;
    int hl;

    hl = ip->ip_hl << 2;
    if (len < hl + ICMP_MINLEN)
        return;

    icp = (struct icmp *)(pkt+hl);
    if (icp->icmp_type == ICMP_ECHOREPLY &&
        icp->icmp_id == htons(pid & 0xFFFF))
    {
        struct timezone tz;
        struct timeval tv;
        struct timeval *then =
            (struct timeval *)&icp->icmp_data[0];

        gettimeofday(&tv, &tz);

        printf("%s: %d ms (seq=%d)\n", inet_ntoa(from->sin_addr),
               ms_between(&tv, then), ntohs(icp->icmp_seq));
    }

    else if (icp->icmp_type == ICMP_UNREACH) {
        struct ip *oldip = (struct ip *)&icp->icmp_data[0];
        struct icmp *oldicp;
        int ohl;

        ohl = oldip->ip_hl << 2;
        if (len < hl + 8 + ohl + ICMP_MINLEN)
            return;

        oldicp = (struct icmp *)(pkt+hl+8+ohl);
        if (oldicp->icmp_type == ICMP_ECHO &&
            oldicp->icmp_id == htons(pid & 0xFFFF))
        {
            printf("%s: unreachable\n", inet_ntoa(oldip->ip_dst));
        }
    }
}

uint16_t icmp_checksum(uint16_t *w, int len)
{
    uint32_t sum = 0;

    /* Add type and code, but skip the checksum. */
    sum += *w;
    len -= 4;
    w += 2;

    while (len > 1) {
        sum += *w++;
        len -= 2;
    }

    if (len == 1) {
        sum += *(unsigned char *)w;
    }

    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);

    return ~sum;
}

int ms_between(struct timeval *new, struct timeval *old)
{
    int sec = 0, usec = 0;

    sec = new->tv_sec;
    usec = new->tv_usec - old->tv_usec;
    if (usec < 0) {
        usec += 1000*1000;
        sec--;
    }
    sec -= old->tv_sec;

    return sec*1000+(usec/1000);
}
