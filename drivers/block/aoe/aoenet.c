/* Copyright (c) 2004 Coraid, Inc.  See COPYING for GPL terms. */
/*
 * aoenet.c
 * Ethernet portion of AoE driver
 */

#include <linux/hdreg.h>
#include <linux/blkdev.h>
#include <linux/netdevice.h>
#include "aoe.h"

#define NECODES 5

static char *aoe_errlist[] =
{
	"no such error",
	"unrecognized command code",
	"bad argument parameter",
	"device unavailable",
	"config string present",
	"unsupported version"
};

enum {
	IFLISTSZ = 1024,
};

static char aoe_iflist[IFLISTSZ];

int
is_aoe_netif(struct net_device *ifp)
{
	register char *p, *q;
	register int len;

	if (aoe_iflist[0] == '\0')
		return 1;

	for (p = aoe_iflist; *p; p = q + strspn(q, WHITESPACE)) {
		q = p + strcspn(p, WHITESPACE);
		if (q != p)
			len = q - p;
		else
			len = strlen(p); /* last token in aoe_iflist */

		if (strlen(ifp->name) == len && !strncmp(ifp->name, p, len))
			return 1;
		if (q == p)
			break;
	}

	return 0;
}

int
set_aoe_iflist(char *str)
{
	int len = strlen(str);

	if (len >= IFLISTSZ)
		return -EINVAL;

	strcpy(aoe_iflist, str);
	return 0;
}

u64
mac_addr(char addr[6])
{
	u64 n = 0;
	char *p = (char *) &n;

	memcpy(p + 2, addr, 6);	/* (sizeof addr != 6) */

	return __be64_to_cpu(n);
}

static struct sk_buff *
skb_check(struct sk_buff *skb)
{
	if (skb_is_nonlinear(skb))
	if ((skb = skb_share_check(skb, GFP_ATOMIC)))
	if (skb_linearize(skb, GFP_ATOMIC) < 0) {
		dev_kfree_skb(skb);
		return NULL;
	}
	return skb;
}

void
aoenet_xmit(struct sk_buff *sl)
{
	struct sk_buff *skb;

	while ((skb = sl)) {
		sl = sl->next;
		skb->next = skb->prev = NULL;
		dev_queue_xmit(skb);
	}
}

/* 
 * (1) i have no idea if this is redundant, but i can't figure why
 * the ifp is passed in if it is.
 *
 * (2) len doesn't include the header by default.  I want this. 
 */
static int
aoenet_rcv(struct sk_buff *skb, struct net_device *ifp, struct packet_type *pt)
{
	struct aoe_hdr *h;
	ulong n;

	skb = skb_check(skb);
	if (!skb)
		return 0;

	skb->dev = ifp;	/* (1) */

	if (!is_aoe_netif(ifp))
		goto exit;

	skb->len += ETH_HLEN;	/* (2) */

	h = (struct aoe_hdr *) skb->mac.raw;
	n = __be32_to_cpu(*((u32 *) h->tag));
	if ((h->verfl & AOEFL_RSP) == 0 || (n & 1<<31))
		goto exit;

	if (h->verfl & AOEFL_ERR) {
		n = h->err;
		if (n > NECODES)
			n = 0;
		printk(KERN_CRIT "aoe: aoenet_rcv: error packet from %d.%d; "
			"ecode=%d '%s'\n",
		       __be16_to_cpu(*((u16 *) h->major)), h->minor, 
			h->err, aoe_errlist[n]);
		goto exit;
	}

	switch (h->cmd) {
	case AOECMD_ATA:
		aoecmd_ata_rsp(skb);
		break;
	case AOECMD_CFG:
		aoecmd_cfg_rsp(skb);
		break;
	default:
		printk(KERN_INFO "aoe: aoenet_rcv: unknown cmd %d\n", h->cmd);
	}
exit:
	dev_kfree_skb(skb);
	return 0;
}

static struct packet_type aoe_pt = {
	.type = __constant_htons(ETH_P_AOE),
	.func = aoenet_rcv,
};

int __init
aoenet_init(void)
{
	dev_add_pack(&aoe_pt);
	return 0;
}

void __exit
aoenet_exit(void)
{
	dev_remove_pack(&aoe_pt);
}

