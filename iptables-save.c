/* Code to save the iptables state, in human readable-form. */
/* Authors: Paul 'Rusty' Russel <rusty@linuxcare.com.au> and
 * 	    Harald Welte <laforge@gnumonks.org>
 */
#include <getopt.h>
#include <sys/errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <time.h>
#include "libiptc/libiptc.h"
#include "iptables.h"

static int binary = 0, counters = 0;

static struct option options[] = {
	{ "binary", 0, 0, 'b' },
	{ "counters", 0, 0, 'c' },
	{ "dump", 0, 0, 'd' },
	{ "table", 1, 0, 't' },
	{ 0 }
};

#define IP_PARTS_NATIVE(n)			\
(unsigned int)((n)>>24)&0xFF,			\
(unsigned int)((n)>>16)&0xFF,			\
(unsigned int)((n)>>8)&0xFF,			\
(unsigned int)((n)&0xFF)

#define IP_PARTS(n) IP_PARTS_NATIVE(ntohl(n))

/* This assumes that mask is contiguous, and byte-bounded. */
static void
print_iface(char letter, const char *iface, const unsigned char *mask,
	    int invert)
{
	unsigned int i;

	if (mask[0] == 0)
		return;

	printf("-%c %s", letter, invert ? "! " : "");

	for (i = 0; i < IFNAMSIZ; i++) {
		if (mask[i] != 0) {
			if (iface[i] != '\0')
				printf("%c", iface[i]);
		} else {
			if (iface[i] != '\0')
				printf("+");
			break;
		}
	}

	printf(" ");
}

/* These are hardcoded backups in iptables.c, so they are safe */
struct pprot {
	char *name;
	u_int8_t num;
};

/* FIXME: why don't we use /etc/services ? */
static const struct pprot chain_protos[] = {
	{ "tcp", IPPROTO_TCP },
	{ "udp", IPPROTO_UDP },
	{ "icmp", IPPROTO_ICMP },
};

static void print_proto(u_int16_t proto, int invert)
{
	if (proto) {
		unsigned int i;
		const char *invertstr = invert ? "! " : "";

		for (i = 0; i < sizeof(chain_protos)/sizeof(struct pprot); i++)
			if (chain_protos[i].num == proto) {
				printf("-p %s%s ",
				       invertstr, chain_protos[i].name);
				return;
			}

		printf("-p %s%u ", invertstr, proto);
	}
}

#if 0
static int non_zero(const void *ptr, size_t size)
{
	unsigned int i;

	for (i = 0; i < size; i++)
		if (((char *)ptr)[i])
			return 0;

	return 1;
}
#endif

static int print_match(const struct ipt_entry_match *e,
			const struct ipt_ip *ip)
{
	struct iptables_match *match
		= find_match(e->u.user.name, TRY_LOAD);

	if (match) {
		printf("-m %s ", e->u.user.name);

		/* some matches don't provide a save function */
		if (match->save)
			match->save(ip, e);
	} else {
		if (e->u.match_size) {
			fprintf(stderr,
				"Can't find library for match `%s'\n",
				e->u.user.name);
			exit(1);
		}
	}
	return 0;
}

/* print a given ip including mask if neccessary */
static void print_ip(char *prefix, u_int32_t ip, u_int32_t mask, int invert)
{
	if (!mask && !ip)
		return;

	printf("%s %s%u.%u.%u.%u",
		prefix,
		invert ? "! " : "",
		IP_PARTS(ip));

	if (mask != 0xffffffff) 
		printf("/%u.%u.%u.%u ", IP_PARTS(mask));
	else
		printf(" ");
}

/* We want this to be readable, so only print out neccessary fields.
 * Because that's the kind of world I want to live in.  */
static void print_rule(const struct ipt_entry *e, 
		iptc_handle_t *h, int counters)
{
	struct ipt_entry_target *t;

	/* print counters */
	if (counters)
		printf("[%llu:%llu] ", e->counters.pcnt, e->counters.bcnt);

	/* Print IP part. */
	print_ip("-s", e->ip.src.s_addr,e->ip.smsk.s_addr,
			e->ip.invflags & IPT_INV_SRCIP);	

	print_ip("-d", e->ip.dst.s_addr, e->ip.dmsk.s_addr,
			e->ip.invflags & IPT_INV_DSTIP);

	print_iface('i', e->ip.iniface, e->ip.iniface_mask,
		    e->ip.invflags & IPT_INV_VIA_IN);

	print_iface('o', e->ip.outiface, e->ip.outiface_mask,
		    e->ip.invflags & IPT_INV_VIA_OUT);

	print_proto(e->ip.proto, e->ip.invflags & IPT_INV_PROTO);

	if (e->ip.flags & IPT_F_FRAG)
		printf("%s-f ",
		       e->ip.invflags & IPT_INV_FRAG ? "! " : "");

	/* Print matchinfo part */
	if (e->target_offset) {
		IPT_MATCH_ITERATE(e, print_match, &e->ip);
	}

	/* Print target name */	
	printf("-j %s ", iptc_get_target(e, h));

	/* Print targinfo part */
	t = ipt_get_target((struct ipt_entry *)e);
	if (t->u.user.name[0]) {
		struct iptables_target *target
			= find_target(t->u.user.name, TRY_LOAD);

		if (target)
			target->save(&e->ip, t);
		else {
			/* If some bits are non-zero, it implies we *need*
			   to understand it */
			if (t->u.target_size) {
				fprintf(stderr,
					"Can't find library for target `%s'\n",
					t->u.user.name);
				exit(1);
			}
		}
	}
	printf("\n");
}

/* Debugging prototype. */
static int for_each_table(int (*func)(const char *tablename))
{
        int ret = 1;
	FILE *procfile = NULL;
	char tablename[IPT_TABLE_MAXNAMELEN+1];

	procfile = fopen("/proc/net/ip_tables_names", "r");
	if (!procfile)
		return 0;

	while (fgets(tablename, sizeof(tablename), procfile)) {
		if (tablename[strlen(tablename) - 1] != '\n')
			exit_error(OTHER_PROBLEM, 
				   "Badly formed tablename `%s'\n",
				   tablename);
		tablename[strlen(tablename) - 1] = '\0';
		ret &= func(tablename);
	}

	return ret;
}
	

static int do_output(const char *tablename)
{
	iptc_handle_t h;
	const char *chain = NULL;

	if (!tablename)
		return for_each_table(&do_output);

	h = iptc_init(tablename);
	if (!h)
 		exit_error(OTHER_PROBLEM, "Can't initialize: %s\n",
			   iptc_strerror(errno));

	if (!binary) {
		time_t now = time(NULL);

		printf("# Generated by iptables-save v%s on %s",
		       NETFILTER_VERSION, ctime(&now));
		printf("*%s\n", tablename);

		/* Dump out chain names */
		for (chain = iptc_first_chain(&h);
		     chain;
		     chain = iptc_next_chain(&h)) {
			const struct ipt_entry *e;

			printf(":%s ", chain);
			if (iptc_builtin(chain, h)) {
				struct ipt_counters count;
				printf("%s ",
				       iptc_get_policy(chain, &count, &h));
				printf("[%llu:%llu]\n", count.pcnt, count.bcnt);
			} else {
				printf("- [0:0]\n");
			}

			/* Dump out rules */
			e = iptc_first_rule(chain, &h);
			while(e) {
				print_rule(e, &h, counters);
				e = iptc_next_rule(e, &h);
			}
		}

		now = time(NULL);
		printf("COMMIT\n");
		printf("# Completed on %s", ctime(&now));
	} else {
		/* Binary, huh?  OK. */
		exit_error(OTHER_PROBLEM, "Binary NYI\n");
	}

	return 1;
}

/* Format:
 * :Chain name POLICY packets bytes
 * rule
 */
int main(int argc, char *argv[])
{
	const char *tablename = NULL;
	int c;

	program_name = "iptables-save";
	program_version = NETFILTER_VERSION;

	while ((c = getopt_long(argc, argv, "bc", options, NULL)) != -1) {
		switch (c) {
		case 'b':
			binary = 1;
			break;

		case 'c':
			counters = 1;
			break;

		case 't':
			/* Select specific table. */
			tablename = optarg;
			break;
		case 'd':
			do_output(tablename);
			exit(0);
		}
	}

	if (optind < argc) {
		fprintf(stderr, "Unknown arguments found on commandline");
		exit(1);
	}

	return !do_output(tablename);
}
