/* vi: set sw=4 ts=4: */
/* dhcpd.h */
#ifndef UDHCP_DHCPD_H
#define UDHCP_DHCPD_H 1

PUSH_AND_SET_FUNCTION_VISIBILITY_TO_HIDDEN

/************************************/
/* Defaults _you_ may want to tweak */
/************************************/

/* the period of time the client is allowed to use that address */
#define LEASE_TIME              (60*60*24*10) /* 10 days of seconds */
#define LEASES_FILE		CONFIG_DHCPD_LEASES_FILE

/* where to find the DHCP server configuration file */
#define DHCPD_CONF_FILE         "/etc/udhcpd.conf"

struct option_set {
	uint8_t *data;
	struct option_set *next;
};

struct static_lease {
	struct static_lease *next;
	uint32_t nip;
	uint8_t mac[6];
};

struct server_config_t {
	uint32_t server_nip;            /* Our IP, in network order */
#if ENABLE_FEATURE_UDHCP_PORT
	uint16_t port;
#endif
	/* start,end are in host order: we need to compare start <= ip <= end */
	uint32_t start_ip;              /* Start address of leases, in host order */
	uint32_t end_ip;                /* End of leases, in host order */
	struct option_set *options;     /* List of DHCP options loaded from the config file */
	char *interface;                /* The name of the interface to use */
	int ifindex;                    /* Index number of the interface to use */
	uint8_t arp[6];                 /* Our arp address */
	uint32_t lease;	                /* lease time in seconds (host order) */
	uint32_t max_leases;            /* maximum number of leases (including reserved address) */
	uint32_t auto_time;             /* how long should udhcpd wait before writing a config file.
	                                 * if this is zero, it will only write one on SIGUSR1 */
	uint32_t decline_time;          /* how long an address is reserved if a client returns a
	                                 * decline message */
	uint32_t conflict_time;         /* how long an arp conflict offender is leased for */
	uint32_t offer_time;            /* how long an offered address is reserved */
	uint32_t min_lease;             /* minimum lease time a client can request */
	uint32_t siaddr_nip;                /* next server bootp option */
	char *lease_file;
	char *pidfile;
	char *notify_file;              /* What to run whenever leases are written */
	char *sname;                    /* bootp server name */
	char *boot_file;                /* bootp boot file option */
	struct static_lease *static_leases; /* List of ip/mac pairs to assign static leases */
};

#define server_config (*(struct server_config_t*)&bb_common_bufsiz1)
/* client_config sits in 2nd half of bb_common_bufsiz1 */

#if ENABLE_FEATURE_UDHCP_PORT
#define SERVER_PORT (server_config.port)
#else
#define SERVER_PORT 67
#endif


/*** leases.h ***/

typedef uint32_t leasetime_t;
typedef int32_t signed_leasetime_t;

struct dhcpOfferedAddr {
	uint8_t lease_mac16[16];
	/* "nip": IP in network order */
	uint32_t lease_nip;
	/* Unix time when lease expires. Kept in memory in host order.
	 * When written to file, converted to network order
	 * and adjusted (current time subtracted) */
	leasetime_t expires;
	uint8_t hostname[20]; /* (size is a multiply of 4) */
};

extern struct dhcpOfferedAddr *leases;

struct dhcpOfferedAddr *add_lease(
		const uint8_t *chaddr, uint32_t yiaddr,
		leasetime_t leasetime, uint8_t *hostname
		) FAST_FUNC;
int lease_expired(struct dhcpOfferedAddr *lease) FAST_FUNC;
struct dhcpOfferedAddr *find_lease_by_chaddr(const uint8_t *chaddr) FAST_FUNC;
struct dhcpOfferedAddr *find_lease_by_yiaddr(uint32_t yiaddr) FAST_FUNC;
uint32_t find_free_or_expired_address(const uint8_t *chaddr) FAST_FUNC;


/*** static_leases.h ***/

/* Config file parser will pass static lease info to this function
 * which will add it to a data structure that can be searched later */
void add_static_lease(struct static_lease **st_lease_pp, uint8_t *mac, uint32_t nip) FAST_FUNC;
/* Find static lease IP by mac */
uint32_t get_static_nip_by_mac(struct static_lease *st_lease, void *arg) FAST_FUNC;
/* Check to see if an IP is reserved as a static IP */
int is_nip_reserved(struct static_lease *st_lease, uint32_t nip) FAST_FUNC;
/* Print out static leases just to check what's going on (debug code) */
void print_static_leases(struct static_lease **st_lease_pp) FAST_FUNC;


/*** serverpacket.h ***/

int send_offer(struct dhcpMessage *oldpacket) FAST_FUNC;
int send_NAK(struct dhcpMessage *oldpacket) FAST_FUNC;
int send_ACK(struct dhcpMessage *oldpacket, uint32_t yiaddr) FAST_FUNC;
int send_inform(struct dhcpMessage *oldpacket) FAST_FUNC;


/*** files.h ***/

void read_config(const char *file) FAST_FUNC;
void write_leases(void) FAST_FUNC;
void read_leases(const char *file) FAST_FUNC;
struct option_set *find_option(struct option_set *opt_list, uint8_t code) FAST_FUNC;


POP_SAVED_FUNCTION_VISIBILITY

#endif
