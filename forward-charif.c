/*
 * This program forwards UDP packets between two netmap ports.
 * Only UDP packets with a destination port specified
 * by command-line option are forwarded, while all the other ones are
 * dropped. If port 0 is specified, all packets are forwarded.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <net/if.h>
#include <stdint.h>
#include <net/netmap.h>
#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>
#include <netinet/ether.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <netinet/tcp.h>

static int stop = 0;
static unsigned long long fwd = 0;
static unsigned long long tot = 0;

static void
sigint_handler(int signum)
{
    stop = 1;
}

static int
rx_ready(struct nm_desc *nmd)
{
    unsigned int ri;

    for (ri = nmd->first_rx_ring; ri <= nmd->last_rx_ring; ri ++) {
            struct netmap_ring *ring;

            ring = NETMAP_RXRING(nmd->nifp, ri);
            if (nm_ring_space(ring)) {
                return 1; /* there is something to read */
            }
    }

    return 0;
}

static inline int
pkt_select(const char *buf, int udp_port)
{
    struct ether_header *ethh;
    struct iphdr *iph;
    struct udphdr *udph;

    if (udp_port == 0) {
        return 1; /* no filter */
    }

    ethh = (struct ether_header *)buf;
    if (ethh->ether_type != htons(ETHERTYPE_IP)) {
        /* Filter out non-IP traffic. */
        return 0;
    }
    iph = (struct iphdr *)(ethh + 1);
    if (iph->protocol != IPPROTO_UDP) {
        /* Filter out non-UDP traffic. */
        return 0;
    }
    udph = (struct udphdr *)(iph + 1);

    /* Match the destination port. */
    if (udph->dest != htons(udp_port)) {
        return 0;
    }

    return 1;
}


static int
main_loop(const char *netmap_port_one, const char *netmap_port_two, int udp_port)
{
    struct nm_desc *nmd_one;
    struct nm_desc *nmd_two;
    int zerocopy;

    nmd_one = nm_open(netmap_port_one, NULL, 0, NULL);
    if (nmd_one == NULL) {
        if (!errno) {
            printf("Failed to nm_open(%s): not a netmap port\n",
                   netmap_port_one);
        } else {
            printf("Failed to nm_open(%s): %s\n", netmap_port_one,
                   strerror(errno));
        }
        return -1;
    }

    nmd_two = nm_open(netmap_port_two, NULL, NM_OPEN_NO_MMAP, nmd_one);
    if (nmd_two == NULL) {
        if (!errno) {
            printf("Failed to nm_open(%s): not a netmap port\n",
                   netmap_port_two);
        } else {
            printf("Failed to nm_open(%s): %s\n", netmap_port_two,
                   strerror(errno));
        }
        return -1;
    }

    /* Check if we can do zerocopy. */
    zerocopy = (nmd_one->mem == nmd_two->mem);
    printf("zerocopy %sabled\n", zerocopy ? "en" : "dis");
	struct pollfd pfd[2];
	struct netmap_ring *ring_one;
	struct netmap_ring *ring_two;
	struct netmap_if *nifp_one = nmd_one->nifp;
	struct netmap_if *nifp_two = nmd_two->nifp;

	int counter = 0;
	int gcounter = 0;
	int timeout = 1000;
	
    while (!stop) {

	pfd[0].fd = nmd_one->fd;
	pfd[1].fd = nmd_two->fd;

	pfd[0].events = 0;
	pfd[1].events = 0;

	if(rx_ready(nmd_one)){
		pfd[1].events |= POLLOUT;
	}
	else{
		pfd[0].events |= POLLIN;
	}
	int p = poll(pfd, 2, timeout);

	if(p==-1){
		printf("Error on Poll = %d \n", p );	
	}

	int irx = nmd_one->first_rx_ring, itx = nmd_two->first_tx_ring;
	
	while(irx <= nmd_one->last_rx_ring && itx <= nmd_two->last_tx_ring ){

		ring_one = NETMAP_RXRING(nifp_one, irx);
		ring_two = NETMAP_TXRING(nifp_two, itx);
		int jrx = ring_one->head, jtx = ring_two->head;
		
		while(jrx != ring_one->tail && jtx != ring_two->tail ){
			
			void *bufRx = NETMAP_BUF(ring_one, ring_one->slot[jrx].buf_idx);
			void *bufTx = NETMAP_BUF(ring_two, ring_two->slot[jtx].buf_idx);
			if(pkt_select(bufRx, udp_port)){
				memcpy(bufRx, bufTx, ring_one->slot[jrx].len);
				counter++;
			}
			gcounter++;	
			jrx = nm_ring_next(ring_one,jrx);
			jtx = nm_ring_next(ring_two,jtx);
		}	
		if(jrx == ring_one->tail){
			irx++;
		}	
		if(jtx == ring_one->tail){
			itx++;
		}
		ring_one->head = ring_one->cur = jrx;
		ring_two->head = ring_two->cur = jtx;
			
	}
	printf("\rExpected %d Count: %d Total: %d", gcounter/6, counter, gcounter);

	

    }

    nm_close(nmd_one);
    nm_close(nmd_two);

    printf("\nTotal processed packets: %llu\n", gcounter);
    printf("Forwarded packets      : %llu\n", counter);

    return 0;
}

static void
usage(char **argv)
{
    printf("usage: %s [-h] [-p UDP_PORT] [-i NETMAP_PORT_ONE] "
           "[-i NETMAP_PORT_TWO]\n", argv[0]);
    exit(EXIT_SUCCESS);
}

int
main(int argc, char **argv)
{
    const char *netmap_port_one = NULL;
    const char *netmap_port_two = NULL;
    int udp_port = 0; /* zero means select everything */
    struct sigaction sa;
    int opt;
    int ret;

    while ((opt = getopt(argc, argv, "hi:p:")) != -1) {
        switch (opt) {
            case 'h':
                usage(argv);
                return 0;

            case 'i':
                if (netmap_port_one == NULL) {
                    netmap_port_one = optarg;
                } else if (netmap_port_two == NULL) {
                    netmap_port_two = optarg;
                }
                break;

            case 'p':
                udp_port = atoi(optarg);
                if (udp_port < 0 || udp_port >= 65535) {
                    printf("    invalid UDP port %s\n", optarg);
                    usage(argv);
                }
                break;

            default:
                printf("    unrecognized option '-%c'\n", opt);
                usage(argv);
                return -1;
        }
    }

    if (netmap_port_one == NULL) {
        printf("    missing netmap port #1\n");
        usage(argv);
    }

    if (netmap_port_two == NULL) {
        printf("    missing netmap port #2\n");
        usage(argv);
    }

    /* Register Ctrl-C handler. */
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ret = sigaction(SIGINT, &sa, NULL);
    if (ret) {
        perror("sigaction(SIGINT)");
        exit(EXIT_FAILURE);
    }
    (void)rx_ready;

    printf("Port one: %s\n", netmap_port_one);
    printf("Port two: %s\n", netmap_port_two);
    printf("UDP port: %d\n", udp_port);

    main_loop(netmap_port_one, netmap_port_two, udp_port);

    return 0;
}
