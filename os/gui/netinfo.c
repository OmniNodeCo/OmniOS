/*
 * OmniOS — os/gui/netinfo.c
 *
 * Network status (see netinfo.h).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <dirent.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "netinfo.h"

static int read_sys(const char *ifc, const char *leaf, char *out, size_t n)
{
    char p[128];
    FILE *f;
    snprintf(p, sizeof(p), "/sys/class/net/%s/%s", ifc, leaf);
    out[0] = '\0';
    if (!(f = fopen(p, "r")))
        return -1;
    if (!fgets(out, (int)n, f))
        out[0] = '\0';
    fclose(f);
    out[strcspn(out, "\n")] = '\0';
    return out[0] ? 0 : -1;
}

static int is_adapter(const char *ifc)
{
    char p[128];
    snprintf(p, sizeof(p), "/sys/class/net/%s/device", ifc);
    return access(p, F_OK) == 0;                /* hardware, not lo/tun/veth */
}

/* the interface holding the default route (and its gateway), 0 if none */
static int default_route(char *ifc, size_t n, char *gw, size_t gn)
{
    char line[256], name[32];
    unsigned long dst, via, flags;
    FILE *f = fopen("/proc/net/route", "r");
    int found = 0;

    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%31s %lx %lx %lx", name, &dst, &via, &flags) == 4 &&
            dst == 0 && (flags & 1)) {          /* RTF_UP */
            struct in_addr a;
            a.s_addr = (in_addr_t)via;           /* printed in kernel order */
            snprintf(ifc, n, "%s", name);
            inet_ntop(AF_INET, &a, gw, (socklen_t)gn);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

/* first hardware adapter (else any interface but lo), 0 if none */
static int pick_adapter(char *ifc, size_t n)
{
    DIR *d = opendir("/sys/class/net");
    struct dirent *e;
    char any[32] = "";
    int found = 0;

    if (!d)
        return 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strcmp(e->d_name, "lo") == 0)
            continue;
        if (is_adapter(e->d_name)) {
            if (!found || strcmp(e->d_name, ifc) < 0)
                snprintf(ifc, n, "%s", e->d_name);  /* eth0 before eth1 */
            found = 1;
        } else if (!any[0]) {
            snprintf(any, sizeof(any), "%s", e->d_name);
        }
    }
    closedir(d);
    if (!found && any[0]) {
        snprintf(ifc, n, "%s", any);
        found = 1;
    }
    return found;
}

static void addr_of(int fd, const char *ifc, unsigned long req, char *out, size_t n)
{
    struct ifreq ifr;
    out[0] = '\0';
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifc);
    ifr.ifr_addr.sa_family = AF_INET;
    if (ioctl(fd, req, &ifr) == 0)
        inet_ntop(AF_INET, &((struct sockaddr_in *)(void *)&ifr.ifr_addr)->sin_addr,
                  out, (socklen_t)n);
}

static void read_dns(char *out, size_t n)
{
    char line[160], ip[64];
    FILE *f = fopen("/etc/resolv.conf", "r");
    out[0] = '\0';
    if (!f)
        return;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "nameserver %63s", ip) == 1 &&
            strlen(out) + strlen(ip) + 3 < n) {
            if (out[0])
                strcat(out, ", ");
            strcat(out, ip);
        }
    fclose(f);
}

int omni_net_read(struct omni_netinfo *n)
{
    char v[64];
    int fd, online;

    memset(n, 0, sizeof(*n));
    online = default_route(n->ifname, sizeof(n->ifname), n->gateway, sizeof(n->gateway));
    if (!online && !pick_adapter(n->ifname, sizeof(n->ifname))) {
        n->state = NET_NO_ADAPTER;
        return 0;
    }
    read_sys(n->ifname, "address", n->mac, sizeof(n->mac));
    if (read_sys(n->ifname, "speed", v, sizeof(v)) == 0 && atoi(v) > 0)
        n->speed = atoi(v);
    if (read_sys(n->ifname, "statistics/rx_bytes", v, sizeof(v)) == 0)
        n->rx = strtoull(v, NULL, 10);
    if (read_sys(n->ifname, "statistics/tx_bytes", v, sizeof(v)) == 0)
        n->tx = strtoull(v, NULL, 10);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        addr_of(fd, n->ifname, SIOCGIFADDR, n->ip, sizeof(n->ip));
        addr_of(fd, n->ifname, SIOCGIFNETMASK, n->mask, sizeof(n->mask));
        close(fd);
    }
    if (strcmp(n->ip, "0.0.0.0") == 0)
        n->ip[0] = '\0';                         /* udhcpc's "deconfig" */
    read_dns(n->dns, sizeof(n->dns));

    if (online)
        n->state = NET_ONLINE;
    else if (n->ip[0])
        n->state = NET_LOCAL;                    /* an address, no gateway */
    else if (read_sys(n->ifname, "carrier", v, sizeof(v)) == 0 && v[0] == '0')
        n->state = NET_UNPLUGGED;
    else
        n->state = NET_CONNECTING;               /* DHCP still asking */
    return 0;
}

const char *omni_net_state_text(int state)
{
    switch (state) {
    case NET_ONLINE:     return "Connected";
    case NET_LOCAL:      return "Connected, no internet";
    case NET_CONNECTING: return "Connecting...";
    case NET_UNPLUGGED:  return "Network cable unplugged";
    default:             return "No network adapter";
    }
}
