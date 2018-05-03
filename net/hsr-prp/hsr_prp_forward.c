/* Copyright 2011-2014 Autronica Fire and Security AS
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * Author(s):
 *	2011-2014 Arvid Brodin, arvid.brodin@alten.se
 */

#include "hsr_prp_forward.h"
#include <linux/types.h>
#include <linux/skbuff.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include "hsr_prp_main.h"
#include "hsr_prp_framereg.h"

struct hsr_prp_node;

struct hsr_prp_frame_info {
	struct sk_buff *skb_std;
	struct sk_buff *skb_hsr;
	struct hsr_prp_port *port_rcv;
	struct hsr_prp_node *node_src;
	u16 sequence_nr;
	bool is_supervision;
	bool is_vlan;
	bool is_local_dest;
	bool is_local_exclusive;
};


/* The uses I can see for these HSR supervision frames are:
 * 1) Use the frames that are sent after node initialization ("HSR_TLV.Type =
 *    22") to reset any sequence_nr counters belonging to that node. Useful if
 *    the other node's counter has been reset for some reason.
 *    --
 *    Or not - resetting the counter and bridging the frame would create a
 *    loop, unfortunately.
 *
 * 2) Use the LifeCheck frames to detect ring breaks. I.e. if no LifeCheck
 *    frame is received from a particular node, we know something is wrong.
 *    We just register these (as with normal frames) and throw them away.
 *
 * 3) Allow different MAC addresses for the two slave interfaces, using the
 *    mac_address_a field.
 */
static bool is_supervision_frame(struct hsr_prp_priv *priv, struct sk_buff *skb)
{
	struct ethhdr *eth_hdr;
	struct hsr_prp_sup_tag *hsr_sup_tag;
	struct hsrv1_ethhdr_sp *hsr_v1_hdr;

	WARN_ON_ONCE(!skb_mac_header_was_set(skb));
	eth_hdr = (struct ethhdr *)skb_mac_header(skb);

	/* Correct addr? */
	if (!ether_addr_equal(eth_hdr->h_dest,
			      priv->sup_multicast_addr))
		return false;

	/* Correct ether type?. */
	if (!(eth_hdr->h_proto == htons(ETH_P_PRP) ||
	      eth_hdr->h_proto == htons(ETH_P_HSR)))
		return false;

	/* Get the supervision header from correct location. */
	if (eth_hdr->h_proto == htons(ETH_P_HSR)) { /* Okay HSRv1. */
		hsr_v1_hdr = (struct hsrv1_ethhdr_sp *)skb_mac_header(skb);
		if (hsr_v1_hdr->hsr.encap_proto != htons(ETH_P_PRP))
			return false;

		hsr_sup_tag = &hsr_v1_hdr->hsr_sup;
	} else {
		hsr_sup_tag = &((struct hsrv0_ethhdr_sp *)
				skb_mac_header(skb))->hsr_sup;
	}

	if (hsr_sup_tag->HSR_TLV_type != HSR_TLV_ANNOUNCE &&
	    hsr_sup_tag->HSR_TLV_type != HSR_TLV_LIFE_CHECK)
		return false;
	if (hsr_sup_tag->HSR_TLV_length != 12 &&
	    hsr_sup_tag->HSR_TLV_length != sizeof(struct hsr_prp_sup_payload))
		return false;

	return true;
}

static struct sk_buff *create_stripped_skb(struct sk_buff *skb_in,
					   struct hsr_prp_frame_info *frame)
{
	struct sk_buff *skb;
	int copylen;
	unsigned char *dst, *src;

	skb_pull(skb_in, HSR_PRP_HLEN);
	skb = __pskb_copy(skb_in,
			  skb_headroom(skb_in) - HSR_PRP_HLEN, GFP_ATOMIC);
	skb_push(skb_in, HSR_PRP_HLEN);
	if (skb == NULL)
		return NULL;

	skb_reset_mac_header(skb);

	if (skb->ip_summed == CHECKSUM_PARTIAL)
		skb->csum_start -= HSR_PRP_HLEN;

	copylen = 2 * ETH_ALEN;
	if (frame->is_vlan)
		copylen += VLAN_HLEN;
	src = skb_mac_header(skb_in);
	dst = skb_mac_header(skb);
	memcpy(dst, src, copylen);

	skb->protocol = eth_hdr(skb)->h_proto;
	return skb;
}

static struct sk_buff *frame_get_stripped_skb(struct hsr_prp_frame_info *frame,
					      struct hsr_prp_port *port)
{
	if (!frame->skb_std)
		frame->skb_std = create_stripped_skb(frame->skb_hsr, frame);
	return skb_clone(frame->skb_std, GFP_ATOMIC);
}

static void hsr_fill_tag(struct sk_buff *skb, struct hsr_prp_frame_info *frame,
			 struct hsr_prp_port *port, u8 proto_ver)
{
	struct hsr_ethhdr *hsr_ethhdr;
	int lane_id;
	int lsdu_size;

	if (port->type == HSR_PRP_PT_SLAVE_A)
		lane_id = 0;
	else
		lane_id = 1;

	lsdu_size = skb->len - 14;
	if (frame->is_vlan)
		lsdu_size -= 4;

	hsr_ethhdr = (struct hsr_ethhdr *)skb_mac_header(skb);

	set_hsr_tag_path(&hsr_ethhdr->hsr_tag, lane_id);
	set_hsr_tag_LSDU_size(&hsr_ethhdr->hsr_tag, lsdu_size);
	hsr_ethhdr->hsr_tag.sequence_nr = htons(frame->sequence_nr);
	hsr_ethhdr->hsr_tag.encap_proto = hsr_ethhdr->ethhdr.h_proto;
	hsr_ethhdr->ethhdr.h_proto = htons(proto_ver ?
					   ETH_P_HSR : ETH_P_PRP);
}

static struct sk_buff *create_tagged_skb(struct sk_buff *skb_o,
					 struct hsr_prp_frame_info *frame,
					 struct hsr_prp_port *port)
{
	int movelen;
	unsigned char *dst, *src;
	struct sk_buff *skb;

	/* Create the new skb with enough headroom to fit the HSR tag */
	skb = __pskb_copy(skb_o, skb_headroom(skb_o) + HSR_PRP_HLEN,
			  GFP_ATOMIC);
	if (!skb)
		return NULL;
	skb_reset_mac_header(skb);

	if (skb->ip_summed == CHECKSUM_PARTIAL)
		skb->csum_start += HSR_PRP_HLEN;

	movelen = ETH_HLEN;
	if (frame->is_vlan)
		movelen += VLAN_HLEN;

	src = skb_mac_header(skb);
	dst = skb_push(skb, HSR_PRP_HLEN);
	memmove(dst, src, movelen);
	skb_reset_mac_header(skb);

	hsr_fill_tag(skb, frame, port, port->priv->prot_ver);

	return skb;
}

/* If the original frame was an HSR tagged frame, just clone it to be sent
 * unchanged. Otherwise, create a private frame especially tagged for 'port'.
 */
static struct sk_buff *frame_get_tagged_skb(struct hsr_prp_frame_info *frame,
					    struct hsr_prp_port *port)
{
	if (frame->skb_hsr)
		return skb_clone(frame->skb_hsr, GFP_ATOMIC);

	if (port->type != HSR_PRP_PT_SLAVE_A &&
	    port->type != HSR_PRP_PT_SLAVE_B) {
		WARN_ONCE(1,
			  "HSR: Bug: trying to create a tagged frame for a non slave port");
		return NULL;
	}

	return create_tagged_skb(frame->skb_std, frame, port);
}

static void hsr_prp_deliver_master(struct sk_buff *skb,
				   struct hsr_prp_node *node_src,
				   struct hsr_prp_port *port)
{
	struct net_device *dev = port->dev;
	bool was_multicast_frame;
	int res;

	was_multicast_frame = (skb->pkt_type == PACKET_MULTICAST);
	/* For LRE offloaded case, assume same MAC address is on both
	 * interfaces of the remote node and hence no need to substitute
	 * the source MAC address.
	 */
	if (!port->priv->rx_offloaded)
		hsr_addr_subst_source(node_src, skb);

	skb_pull(skb, ETH_HLEN);
	res = netif_rx(skb);
	if (res == NET_RX_DROP) {
		dev->stats.rx_dropped++;
	} else {
		dev->stats.rx_packets++;
		dev->stats.rx_bytes += skb->len;
		if (was_multicast_frame)
			dev->stats.multicast++;
	}
}

static int hsr_prp_xmit(struct sk_buff *skb, struct hsr_prp_port *port,
			struct hsr_prp_frame_info *frame)
{
	if (!port->priv->rx_offloaded &&
	    frame->port_rcv->type == HSR_PRP_PT_MASTER) {
		hsr_addr_subst_dest(frame->node_src, skb, port);

		/* Address substitution (IEC62439-3 pp 26, 50): replace mac
		 * address of outgoing frame with that of the outgoing slave's.
		 */
		ether_addr_copy(eth_hdr(skb)->h_source, port->dev->dev_addr);
	}
	return dev_queue_xmit(skb);
}

/* Forward the frame through all devices except:
 * - Back through the receiving device
 * - If it's a HSR frame: through a device where it has passed before
 * - To the local HSR master only if the frame is directly addressed to it, or
 *   a non-supervision multicast or broadcast frame.
 *
 * HSR slave devices should insert a HSR tag into the frame, or forward the
 * frame unchanged if it's already tagged. Interlink devices should strip HSR
 * tags if they're of the non-HSR type (but only after duplicate discard). The
 * master device always strips HSR tags.
 */
static void hsr_prp_forward_do(struct hsr_prp_frame_info *frame)
{
	struct hsr_prp_port *port;
	struct sk_buff *skb;

	hsr_prp_for_each_port(frame->port_rcv->priv, port) {
		/* Don't send frame back the way it came */
		if (port == frame->port_rcv)
			continue;

		/* Don't deliver locally unless we should */
		if (port->type == HSR_PRP_PT_MASTER && !frame->is_local_dest)
			continue;

		/* Deliver frames directly addressed to us to master only */
		if (port->type != HSR_PRP_PT_MASTER &&
		    frame->is_local_exclusive)
			continue;

		/* Don't send frame over port where it has been sent before
		 * if not rx offloaded
		 */
		if (!port->priv->rx_offloaded &&
		    hsr_register_frame_out(port, frame->node_src,
					   frame->sequence_nr))
			continue;

		/* In LRE offloaded case, don't expect supervision frames from
		 * slave ports for host as they get processed at the h/w or
		 * firmware
		 */
		if (frame->is_supervision &&
		    port->type == HSR_PRP_PT_MASTER &&
		    !port->priv->rx_offloaded) {
			hsr_prp_handle_sup_frame(frame->skb_hsr,
						 frame->node_src,
						 frame->port_rcv);
			continue;
		}

		/* if L2 forward is offloaded, don't forward frame
		 * across slaves
		 */
		if (port->priv->l2_fwd_offloaded &&
		    ((frame->port_rcv->type == HSR_PRP_PT_SLAVE_A &&
		    port->type ==  HSR_PRP_PT_SLAVE_B) ||
		    (frame->port_rcv->type == HSR_PRP_PT_SLAVE_B &&
		    port->type ==  HSR_PRP_PT_SLAVE_A)))
			continue;

		if (port->type != HSR_PRP_PT_MASTER)
			skb = frame_get_tagged_skb(frame, port);
		else
			skb = frame_get_stripped_skb(frame, port);
		if (!skb) {
			/* FIXME: Record the dropped frame? */
			continue;
		}

		skb->dev = port->dev;
		if (port->type == HSR_PRP_PT_MASTER)
			hsr_prp_deliver_master(skb, frame->node_src, port);
		else
			hsr_prp_xmit(skb, port, frame);
	}
}

static void check_local_dest(struct hsr_prp_priv *priv, struct sk_buff *skb,
			     struct hsr_prp_frame_info *frame)
{
	if (hsr_prp_addr_is_self(priv, eth_hdr(skb)->h_dest)) {
		frame->is_local_exclusive = true;
		skb->pkt_type = PACKET_HOST;
	} else {
		frame->is_local_exclusive = false;
	}

	if ((skb->pkt_type == PACKET_HOST) ||
	    (skb->pkt_type == PACKET_MULTICAST) ||
	    (skb->pkt_type == PACKET_BROADCAST)) {
		frame->is_local_dest = true;
	} else {
		frame->is_local_dest = false;
	}
}

static int hsr_prp_fill_frame_info(struct hsr_prp_frame_info *frame,
				   struct sk_buff *skb,
				   struct hsr_prp_port *port)
{
	struct ethhdr *ethhdr;
	unsigned long irqflags;
	struct hsr_prp_priv *priv = port->priv;

	frame->is_supervision = is_supervision_frame(priv, skb);
	if (frame->is_supervision && priv->rx_offloaded &&
	    port->type != HSR_PRP_PT_MASTER) {
		WARN_ONCE(1,
			  "HSR: unexpected rx supervisor frame when offloaded");
		return -1;
	}

	/* For Offloaded case, there is no need for node list since
	 * firmware/hardware implements LRE function.
	 */
	if (!priv->rx_offloaded) {
		frame->node_src = hsr_prp_get_node(port, skb,
						   frame->is_supervision);
		/* Unknown node and !is_supervision, or no mem */
		if (!frame->node_src)
			return -1;
	}

	ethhdr = (struct ethhdr *)skb_mac_header(skb);
	frame->is_vlan = false;
	if (ethhdr->h_proto == htons(ETH_P_8021Q)) {
		frame->is_vlan = true;
		/* FIXME: */
		WARN_ONCE(1, "HSR: VLAN not yet supported");
	}
	if (ethhdr->h_proto == htons(ETH_P_PRP)
			|| ethhdr->h_proto == htons(ETH_P_HSR)) {
		frame->skb_std = NULL;
		frame->skb_hsr = skb;
		frame->sequence_nr = hsr_get_skb_sequence_nr(skb);
	} else {
		frame->skb_std = skb;
		frame->skb_hsr = NULL;
		/* Sequence nr for the master node */
		spin_lock_irqsave(&priv->seqnr_lock, irqflags);
		frame->sequence_nr = priv->sequence_nr;
		priv->sequence_nr++;
		spin_unlock_irqrestore(&priv->seqnr_lock, irqflags);
	}

	frame->port_rcv = port;
	check_local_dest(priv, skb, frame);

	return 0;
}

/* Must be called holding rcu read lock (because of the port parameter) */
void hsr_prp_forward_skb(struct sk_buff *skb, struct hsr_prp_port *port)
{
	struct hsr_prp_frame_info frame;

	if (skb_mac_header(skb) != skb->data) {
		WARN_ONCE(1, "%s:%d: Malformed frame (port_src %s)\n",
			  __FILE__, __LINE__, port->dev->name);
		goto out_drop;
	}

	if (hsr_prp_fill_frame_info(&frame, skb, port) < 0)
		goto out_drop;
	/* No need to register frame when rx offload is supported */
	if (!port->priv->rx_offloaded)
		hsr_register_frame_in(frame.node_src, port, frame.sequence_nr);

	hsr_prp_forward_do(&frame);

	if (frame.skb_hsr)
		kfree_skb(frame.skb_hsr);
	if (frame.skb_std)
		kfree_skb(frame.skb_std);
	return;

out_drop:
	port->dev->stats.tx_dropped++;
	kfree_skb(skb);
}
