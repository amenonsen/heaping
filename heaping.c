/*
 * heaping -- Ping a list of addresses forever
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
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static pid_t pid;
static int child_died;
static int killed;

uint16_t icmp_checksum(uint16_t *, int);
void describe(unsigned char *, int, struct sockaddr_in *);
void ping(struct in_addr *, int);
void pong(int);

void sigchld_handler(int sig)
{
    child_died = 1;
}

void kill_handler(int sig)
{
    killed = 1;
}

int main(int ac, char *av[]) {
    int n, raw;
    struct in_addr * hosts;

    if (ac < 2) {
        fprintf(stderr, "Usage: heaping <ip> [ip ...]\n");
        exit(0);
    }

    hosts = malloc(ac * sizeof(struct in_addr));
    if (!hosts) {
        fprintf(stderr, "Couldn't malloc in_addr[%d]\n", ac);
        exit(-1);
    }

    n = 1;
    while (n < ac) {
        if (inet_aton(av[n], hosts+n-1) == 0) {
            fprintf(stderr, "Couldn't parse '%s' as IP address\n", av[n]);
            exit(-1);
        }
        n++;
    }
    hosts[ac-1].s_addr = INADDR_NONE;

    raw = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (raw < 0) {
        perror("socket(SOCK_RAW)");
        exit(-1);
    }

    signal(SIGCHLD, sigchld_handler);
    signal(SIGTERM, kill_handler);
    signal(SIGINT, kill_handler);

    pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(-1);
    }
    else if (pid) {
        ping(hosts, raw);
    }
    else {
        pong(raw);
    }

    return -1;
}

void ping(struct in_addr *hosts, int raw)
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
    while (!child_died && !killed) {
        int i = 0;

        icp->icmp_seq = htons(seq);
        printf("meta: new cycle (seq=%d)\n", seq);
        seq++;

        while (hosts[i].s_addr != INADDR_NONE) {
            int n;

            gettimeofday(tp, &tz);
            icp->icmp_cksum = icmp_checksum((uint16_t *)pkt, sz);

            /* What should we do about errors? */
            to.sin_addr.s_addr = hosts[i].s_addr;
            n = sendto(raw, pkt, sz, 0, (struct sockaddr *)&to,
                       sizeof(struct sockaddr));

            i++;
        }

        sleep(10);
    }

    if (killed) {
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
        if (n > 0)
            describe(pkt, n, &from);
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
        int time;
        struct timezone tz;
        struct timeval tv;
        struct timeval *then =
            (struct timeval *)&icp->icmp_data[0];

        gettimeofday(&tv, &tz);

        tv.tv_usec -= then->tv_usec;
        if (tv.tv_usec < 0) {
            tv.tv_sec--;
            tv.tv_usec += 1000000;
        }
        tv.tv_sec -= then->tv_sec;

        time = tv.tv_sec*1000+(tv.tv_usec/1000);

        printf("%s: %d ms (seq=%d)\n", inet_ntoa(from->sin_addr),
               time, ntohs(icp->icmp_seq));
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
