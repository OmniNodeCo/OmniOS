/*
 * OmniOS — os/gui/netinfo.h
 *
 * Network status for the desktop (taskbar icon, Quick Settings) and
 * Settings > Network & internet: the wired adapter OmniOS uses, its IPv4
 * address, gateway, DNS servers and traffic. Read straight from the
 * kernel (/proc/net/route, /sys/class/net, SIOCGIFADDR) and
 * /etc/resolv.conf, which udhcpc writes.
 */
#ifndef OMNI_OS_GUI_NETINFO_H
#define OMNI_OS_GUI_NETINFO_H

enum { NET_NO_ADAPTER, NET_UNPLUGGED, NET_CONNECTING, NET_LOCAL, NET_ONLINE };

struct omni_netinfo {
    int  state;                 /* NET_*                                   */
    char ifname[16];            /* "eth0"                                  */
    char ip[16], mask[16];      /* IPv4 address and subnet mask            */
    char gateway[16];           /* default route                           */
    char dns[48];               /* "10.0.2.3, 1.1.1.1"                     */
    char mac[20];
    int  speed;                 /* link speed in Mb/s, 0 = unknown         */
    unsigned long long rx, tx;  /* bytes received / sent                   */
};

/* read the current status; returns 0 */
int omni_net_read(struct omni_netinfo *n);
/* "Connected", "Network cable unplugged", ... */
const char *omni_net_state_text(int state);

#endif /* OMNI_OS_GUI_NETINFO_H */
