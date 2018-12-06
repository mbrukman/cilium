/*
 *  Copyright (C) 2016-2018 Authors of Cilium
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include <node_config.h>
#include <lxc_config.h>

#define EVENT_SOURCE LXC_ID

#include <bpf/api.h>

#include <stdint.h>
#include <stdio.h>

#include <linux/icmpv6.h>

#include "lib/utils.h"
#include "lib/common.h"
#include "lib/maps.h"
#include "lib/arp.h"
#include "lib/ipv6.h"
#include "lib/ipv4.h"
#include "lib/icmp6.h"
#include "lib/eth.h"
#include "lib/dbg.h"
#include "lib/l3.h"
#include "lib/lxc.h"
#include "lib/nat46.h"
#include "lib/policy.h"
#include "lib/lb.h"
#include "lib/drop.h"
#include "lib/dbg.h"
#include "lib/trace.h"
#include "lib/csum.h"
#include "lib/conntrack.h"
#include "lib/encap.h"

#define POLICY_ID ((LXC_ID << 16) | SECLABEL)

#ifdef HAVE_LRU_MAP_TYPE
#define CT_MAP_TYPE BPF_MAP_TYPE_LRU_HASH
#else
#define CT_MAP_TYPE BPF_MAP_TYPE_HASH
#endif

struct bpf_elf_map __section_maps CT_MAP_TCP6 = {
	.type		= CT_MAP_TYPE,
	.size_key	= sizeof(struct ipv6_ct_tuple),
	.size_value	= sizeof(struct ct_entry),
	.pinning	= PIN_GLOBAL_NS,
	.max_elem	= CT_MAP_SIZE_TCP,
};

struct bpf_elf_map __section_maps CT_MAP_ANY6 = {
	.type		= CT_MAP_TYPE,
	.size_key	= sizeof(struct ipv6_ct_tuple),
	.size_value	= sizeof(struct ct_entry),
	.pinning	= PIN_GLOBAL_NS,
	.max_elem	= CT_MAP_SIZE_ANY,
};

struct bpf_elf_map __section_maps CT_MAP_TCP4 = {
	.type		= CT_MAP_TYPE,
	.size_key	= sizeof(struct ipv4_ct_tuple),
	.size_value	= sizeof(struct ct_entry),
	.pinning	= PIN_GLOBAL_NS,
	.max_elem	= CT_MAP_SIZE_TCP,
};

struct bpf_elf_map __section_maps CT_MAP_ANY4 = {
	.type		= CT_MAP_TYPE,
	.size_key	= sizeof(struct ipv4_ct_tuple),
	.size_value	= sizeof(struct ct_entry),
	.pinning	= PIN_GLOBAL_NS,
	.max_elem	= CT_MAP_SIZE_ANY,
};

static inline struct bpf_elf_map *
get_ct_map6(struct ipv6_ct_tuple *tuple)
{
	if (tuple->nexthdr == IPPROTO_TCP) {
		return &CT_MAP_TCP6;
	}
	return &CT_MAP_ANY6;
}

static inline struct bpf_elf_map *
get_ct_map4(struct ipv4_ct_tuple *tuple)
{
	if (tuple->nexthdr == IPPROTO_TCP) {
		return &CT_MAP_TCP4;
	}
	return &CT_MAP_ANY4;
}

static inline int ipv6_l3_from_lxc(struct __sk_buff *skb,
				   struct ipv6_ct_tuple *tuple, int l3_off,
				   struct ipv6hdr *ip6, __u32 *dstID)
{
	union macaddr router_mac = NODE_MAC;
	int ret, verdict, l4_off, forwarding_reason, hdrlen;
	struct csum_offset csum_off = {};
	struct endpoint_info *ep;
	struct lb6_service *svc;
	struct lb6_key key = {};
	struct ct_state ct_state_new = {};
	struct ct_state ct_state = {};
	void *data, *data_end;
	union v6addr *daddr, orig_dip;
	__u32 tunnel_endpoint = 0;
	__u32 monitor = 0;

	if (unlikely(!is_valid_lxc_src_ip(ip6)))
		return DROP_INVALID_SIP;

	ipv6_addr_copy(&tuple->daddr, (union v6addr *) &ip6->daddr);
	ipv6_addr_copy(&tuple->saddr, (union v6addr *) &ip6->saddr);

	hdrlen = ipv6_hdrlen(skb, l3_off, &tuple->nexthdr);
	if (hdrlen < 0)
		return hdrlen;

	l4_off = l3_off + hdrlen;

	ret = lb6_extract_key(skb, tuple, l4_off, &key, &csum_off, CT_EGRESS);
	if (IS_ERR(ret)) {
		if (ret == DROP_UNKNOWN_L4)
			goto skip_service_lookup;
		else
			return ret;
	}

	ct_state_new.orig_dport = key.dport;

	/*
	 * Check if the destination address is among the address that should be
	 * load balanced. This operation is performed before we go through the
	 * connection tracker to allow storing the reverse nat index in the CT
	 * entry for destination endpoints where we can't encode the state in the
	 * address.
	 */
	if ((svc = lb6_lookup_service(skb, &key)) != NULL) {
		ret = lb6_local(get_ct_map6(tuple), skb, l3_off, l4_off,
				&csum_off, &key, tuple, svc, &ct_state_new);
		if (IS_ERR(ret))
			return ret;
	}

skip_service_lookup:
	/* The verifier wants to see this assignment here in case the above goto
	 * skip_service_lookup is hit. However, in the case the packet
	 * is _not_ TCP or UDP we should not be using proxy logic anyways. For
	 * correctness it must be below the service handler in case the service
	 * logic re-writes the tuple daddr. In "theory" however the assignment
	 * should be OK to move above goto label.
	 */
	ipv6_addr_copy(&orig_dip, (union v6addr *) &tuple->daddr);


	/* WARNING: ip6 offset check invalidated, revalidate before use */

	/* Pass all outgoing packets through conntrack. This will create an
	 * entry to allow reverse packets and return set cb[CB_POLICY] to
	 * POLICY_SKIP if the packet is a reply packet to an existing
	 * incoming connection. */
	ret = ct_lookup6(get_ct_map6(tuple), tuple, skb, l4_off, CT_EGRESS,
			 &ct_state, &monitor);
	if (ret < 0) {
		relax_verifier();
		return ret;
	}

	forwarding_reason = ret;

	if (!revalidate_data(skb, &data, &data_end, &ip6))
		return DROP_INVALID;

	/* Determine the destination category for policy fallback. */
	if (1) {
		struct remote_endpoint_info *info;

		info = lookup_ip6_remote_endpoint(&orig_dip);
		if (info != NULL && info->sec_label) {
			*dstID = info->sec_label;
			tunnel_endpoint = info->tunnel_endpoint;
		} else {
			*dstID = WORLD_ID;
		}

		cilium_dbg(skb, info ? DBG_IP_ID_MAP_SUCCEED6 : DBG_IP_ID_MAP_FAILED6,
			   orig_dip.p4, *dstID);
	}

	/* If the packet is in the establishing direction and it's destined
	 * within the cluster, it must match policy or be dropped. If it's
	 * bound for the host/outside, perform the CIDR policy check. */
	verdict = policy_can_egress6(skb, tuple, *dstID,
				     ipv6_ct_tuple_get_daddr(tuple));
	if (ret != CT_REPLY && ret != CT_RELATED && verdict < 0) {
		/* If the connection was previously known and packet is now
		 * denied, remove the connection tracking entry */
		if (ret == CT_ESTABLISHED)
			ct_delete6(get_ct_map6(tuple), tuple, skb);

		return verdict;
	}

	switch (ret) {
	case CT_NEW:
		/* New connection implies that rev_nat_index remains untouched
		 * to the index provided by the loadbalancer (if it applied).
		 * Create a CT entry which allows to track replies and to
		 * reverse NAT.
		 */
		ct_state_new.src_sec_id = SECLABEL;
		ret = ct_create6(get_ct_map6(tuple), tuple, skb, CT_EGRESS, &ct_state_new);
		if (IS_ERR(ret))
			return ret;
		monitor = TRACE_PAYLOAD_LEN;
		break;

	case CT_ESTABLISHED:
		break;

	case CT_RELATED:
	case CT_REPLY:
		policy_mark_skip(skb);

		if (ct_state.rev_nat_index) {
			ret = lb6_rev_nat(skb, l4_off, &csum_off,
					  ct_state.rev_nat_index, tuple, 0);
			if (IS_ERR(ret))
				return ret;

			/* A reverse translate packet is always allowed except for delivery
			 * on the local node in which case this marking is cleared again. */
			policy_mark_skip(skb);
		}
		break;

	default:
		return DROP_UNKNOWN_CT;
	}

	if (!revalidate_data(skb, &data, &data_end, &ip6))
		return DROP_INVALID;

	daddr = (union v6addr *)&ip6->daddr;

	/* Lookup IPv6 address, this will return a match if:
	 *  - The destination IP address belongs to a local endpoint managed by
	 *    cilium
	 *  - The destination IP address is an IP address associated with the
	 *    host itself.
	 */
	if ((ep = lookup_ip6_endpoint(ip6)) != NULL) {
		if (ep->flags & ENDPOINT_F_HOST) {
#ifdef HOST_IFINDEX
			goto to_host;
#else
			return DROP_HOST_UNREACHABLE;
#endif
		}

		policy_clear_mark(skb);
		return ipv6_local_delivery(skb, l3_off, l4_off, SECLABEL, ip6, tuple->nexthdr, ep, METRIC_EGRESS);
	}

	/* The packet goes to a peer not managed by this agent instance */
	goto pass_to_stack;

to_host:
	if (1) {
		union macaddr host_mac = HOST_IFINDEX_MAC;

		cilium_dbg(skb, DBG_TO_HOST, is_policy_skip(skb), 0);

		ret = ipv6_l3(skb, l3_off, (__u8 *) &router_mac.addr, (__u8 *) &host_mac.addr, METRIC_EGRESS);
		if (ret != TC_ACT_OK)
			return ret;

		send_trace_notify(skb, TRACE_TO_HOST, SECLABEL, HOST_ID, 0,
				  HOST_IFINDEX, forwarding_reason, monitor);

		cilium_dbg_capture(skb, DBG_CAPTURE_DELIVERY, HOST_IFINDEX);
		//return redirect(HOST_IFINDEX, 0);
		return TC_ACT_OK;
	}

pass_to_stack:
	cilium_dbg(skb, DBG_TO_STACK, 0, 0);

	ret = ipv6_l3(skb, l3_off, NULL, (__u8 *) &router_mac.addr, METRIC_EGRESS);
	if (unlikely(ret != TC_ACT_OK))
		return ret;

	if (ipv6_store_flowlabel(skb, l3_off, SECLABEL_NB) < 0)
		return DROP_WRITE_ERROR;

	send_trace_notify(skb, TRACE_TO_STACK, SECLABEL, *dstID, 0, 0,
			  forwarding_reason, monitor);

	cilium_dbg_capture(skb, DBG_CAPTURE_DELIVERY, 0);
	return TC_ACT_OK;
}

static inline int __inline__ handle_ipv6(struct __sk_buff *skb, __u32 *dstID)
{
	struct ipv6_ct_tuple tuple = {};
	void *data, *data_end;
	struct ipv6hdr *ip6;
	int ret;

	if (!revalidate_data(skb, &data, &data_end, &ip6))
		return DROP_INVALID;

	/* Handle special ICMPv6 messages. This includes echo requests to the
	 * logical router address, neighbour advertisements to the router.
	 * All remaining packets are subjected to forwarding into the container.
	 */
	if (unlikely(ip6->nexthdr == IPPROTO_ICMPV6)) {
		if (data + sizeof(*ip6) + ETH_HLEN + sizeof(struct icmp6hdr) > data_end) {
			return DROP_INVALID;
		}

		ret = icmp6_handle(skb, ETH_HLEN, ip6, METRIC_EGRESS);
		if (IS_ERR(ret))
			return ret;
	}

	/* Perform L3 action on the frame */
	tuple.nexthdr = ip6->nexthdr;
	return ipv6_l3_from_lxc(skb, &tuple, ETH_HLEN, ip6, dstID);
}

__section_tail(CILIUM_MAP_CALLS, CILIUM_CALL_IPV6_FROM_LXC) int tail_handle_ipv6(struct __sk_buff *skb)
{
	__u32 dstID = 0;
	int ret = handle_ipv6(skb, &dstID);

	if (IS_ERR(ret)) {
		relax_verifier();
		return send_drop_notify(skb, SECLABEL, dstID, 0, 0, ret, TC_ACT_SHOT,
		                        METRIC_EGRESS);
	}

	return ret;
}

#ifdef LXC_IPV4

static inline int handle_ipv4_from_lxc(struct __sk_buff *skb, __u32 *dstID)
{
	struct ipv4_ct_tuple tuple = {};
	union macaddr router_mac = NODE_MAC;
	void *data, *data_end;
	struct iphdr *ip4;
	int ret, verdict, l3_off = ETH_HLEN, l4_off, forwarding_reason;
	struct csum_offset csum_off = {};
	struct endpoint_info *ep;
	struct lb4_service *svc;
	struct lb4_key key = {};
	struct ct_state ct_state_new = {};
	struct ct_state ct_state = {};
	__be32 orig_dip;
	__u32 tunnel_endpoint = 0;
	__u32 monitor = 0;

	if (!revalidate_data(skb, &data, &data_end, &ip4))
		return DROP_INVALID;

	tuple.nexthdr = ip4->protocol;

	if (unlikely(!is_valid_lxc_src_ipv4(ip4)))
		return DROP_INVALID_SIP;

	tuple.daddr = ip4->daddr;
	tuple.saddr = ip4->saddr;

	l4_off = l3_off + ipv4_hdrlen(ip4);

	ret = lb4_extract_key(skb, &tuple, l4_off, &key, &csum_off, CT_EGRESS);
	if (IS_ERR(ret)) {
		if (ret == DROP_UNKNOWN_L4)
			goto skip_service_lookup;
		else
			return ret;
	}

	ct_state_new.orig_dport = key.dport;
#ifdef ENABLE_IPV4
	if ((svc = lb4_lookup_service(skb, &key)) != NULL) {
		ret = lb4_local(get_ct_map4(&tuple), skb, l3_off, l4_off, &csum_off,
				&key, &tuple, svc, &ct_state_new, ip4->saddr);
		if (IS_ERR(ret))
			return ret;
	}
#endif
skip_service_lookup:
	/* The verifier wants to see this assignment here in case the above goto
	 * skip_service_lookup is hit. However, in the case the packet
	 * is _not_ TCP or UDP we should not be using proxy logic anyways. For
	 * correctness it must be below the service handler in case the service
	 * logic re-writes the tuple daddr. In "theory" however the assignment
	 * should be OK to move above goto label.
	 */
	orig_dip = tuple.daddr;

	/* WARNING: ip4 offset check invalidated, revalidate before use */

	/* Pass all outgoing packets through conntrack. This will create an
	 * entry to allow reverse packets and return set cb[CB_POLICY] to
	 * POLICY_SKIP if the packet is a reply packet to an existing
	 * incoming connection. */
	ret = ct_lookup4(get_ct_map4(&tuple), &tuple, skb, l4_off, CT_EGRESS,
			 &ct_state, &monitor);
	if (ret < 0)
		return ret;

	forwarding_reason = ret;

	/* Determine the destination category for policy fallback. */
	if (1) {
		struct remote_endpoint_info *info;

		info = lookup_ip4_remote_endpoint(orig_dip);
		if (info != NULL && info->sec_label) {
			*dstID = info->sec_label;
			tunnel_endpoint = info->tunnel_endpoint;
		} else {
			*dstID = WORLD_ID;
		}

		cilium_dbg(skb, info ? DBG_IP_ID_MAP_SUCCEED4 : DBG_IP_ID_MAP_FAILED4,
			   orig_dip, *dstID);
	}

	/* If the packet is in the establishing direction and it's destined
	 * within the cluster, it must match policy or be dropped. If it's
	 * bound for the host/outside, perform the CIDR policy check. */
	verdict = policy_can_egress4(skb, &tuple, *dstID, ipv4_ct_tuple_get_daddr(&tuple));
	if (ret != CT_REPLY && ret != CT_RELATED && verdict < 0) {
		/* If the connection was previously known and packet is now
		 * denied, remove the connection tracking entry */
		if (ret == CT_ESTABLISHED)
			ct_delete4(get_ct_map4(&tuple), &tuple, skb);

		return verdict;
	}

	switch (ret) {
	case CT_NEW:
		/* New connection implies that rev_nat_index remains untouched
		 * to the index provided by the loadbalancer (if it applied).
		 * Create a CT entry which allows to track replies and to
		 * reverse NAT.
		 */
		ct_state_new.src_sec_id = SECLABEL;
		ret = ct_create4(get_ct_map4(&tuple), &tuple, skb, CT_EGRESS,
				 &ct_state_new);
		if (IS_ERR(ret))
			return ret;
		break;

	case CT_ESTABLISHED:
		break;

	case CT_RELATED:
	case CT_REPLY:
		policy_mark_skip(skb);

		if (ct_state.rev_nat_index) {
			ret = lb4_rev_nat(skb, l3_off, l4_off, &csum_off,
					  &ct_state, &tuple, 0);
			if (IS_ERR(ret)) {
				relax_verifier();
				return ret;
			}
		}
		break;

	default:
		return DROP_UNKNOWN_CT;
	}

	/* After L4 write in port mapping: revalidate for direct packet access */
	if (!revalidate_data(skb, &data, &data_end, &ip4))
		return DROP_INVALID;

	orig_dip = ip4->daddr;

	/* Lookup IPv4 address, this will return a match if:
	 *  - The destination IP address belongs to a local endpoint managed by
	 *    cilium
	 *  - The destination IP address is an IP address associated with the
	 *    host itself.
	 */
	if ((ep = lookup_ip4_endpoint(ip4)) != NULL) {
		if (ep->flags & ENDPOINT_F_HOST) {
#ifdef HOST_IFINDEX
			goto to_host;
#else
			return DROP_HOST_UNREACHABLE;
#endif
		}
		policy_clear_mark(skb);
		return ipv4_local_delivery(skb, l3_off, l4_off, SECLABEL, ip4, ep, METRIC_EGRESS);
	}

	goto pass_to_stack;

to_host:
	if (1) {
		union macaddr host_mac = HOST_IFINDEX_MAC;

		cilium_dbg(skb, DBG_TO_HOST, is_policy_skip(skb), 0);

		ret = ipv4_l3(skb, l3_off, (__u8 *) &router_mac.addr, (__u8 *) &host_mac.addr, ip4);
		if (ret != TC_ACT_OK)
			return ret;

		send_trace_notify(skb, TRACE_TO_HOST, SECLABEL, HOST_ID, 0, HOST_IFINDEX,
				  forwarding_reason, monitor);

		cilium_dbg_capture(skb, DBG_CAPTURE_DELIVERY, HOST_IFINDEX);
		//return redirect(HOST_IFINDEX, 0);
		return TC_ACT_OK;
	}

pass_to_stack:
	cilium_dbg(skb, DBG_TO_STACK, 0, 0);

	ret = ipv4_l3(skb, l3_off, NULL, (__u8 *) &router_mac.addr, ip4);
	if (unlikely(ret != TC_ACT_OK))
		return ret;

	/* FIXME: We can't store the security context anywhere here so all
	 * packets to other nodes will look like they come from an outside
	 * network.
	 */

	send_trace_notify(skb, TRACE_TO_STACK, SECLABEL, *dstID, 0, 0,
			  forwarding_reason, monitor);

	cilium_dbg_capture(skb, DBG_CAPTURE_DELIVERY, 0);
	return TC_ACT_OK;
}

__section_tail(CILIUM_MAP_CALLS, CILIUM_CALL_IPV4_FROM_LXC) int tail_handle_ipv4(struct __sk_buff *skb)
{
	__u32 dstID = 0;
	int ret = handle_ipv4_from_lxc(skb, &dstID);

	if (IS_ERR(ret))
		return send_drop_notify(skb, SECLABEL, dstID, 0, 0, ret, TC_ACT_SHOT,
		                        METRIC_EGRESS);

	return ret;
}

#endif

static inline int __inline__
ipv6_policy(struct __sk_buff *skb, int ifindex, __u32 src_label, int *forwarding_reason, struct ep_config *cfg)
{
	struct ipv6_ct_tuple tuple = {};
	void *data, *data_end;
	struct ipv6hdr *ip6;
	struct csum_offset csum_off = {};
	int ret, l4_off, verdict, hdrlen;
	struct ct_state ct_state = {};
	struct ct_state ct_state_new = {};
	union v6addr orig_dip = {};
	__u32 monitor = 0;

	if (!revalidate_data(skb, &data, &data_end, &ip6))
		return DROP_INVALID;

	policy_clear_mark(skb);
	tuple.nexthdr = ip6->nexthdr;

	ipv6_addr_copy(&tuple.daddr, (union v6addr *) &ip6->daddr);
	ipv6_addr_copy(&tuple.saddr, (union v6addr *) &ip6->saddr);
	ipv6_addr_copy(&orig_dip, (union v6addr *) &ip6->daddr);

	hdrlen = ipv6_hdrlen(skb, ETH_HLEN, &tuple.nexthdr);
	if (hdrlen < 0)
		return hdrlen;

	l4_off = ETH_HLEN + hdrlen;
	csum_l4_offset_and_flags(tuple.nexthdr, &csum_off);

	/* derive reverse NAT index and zero it. */
	ct_state_new.rev_nat_index = ip6->daddr.s6_addr32[3] & 0xFFFF;
	if (ct_state_new.rev_nat_index) {
		union v6addr dip;

		ipv6_addr_copy(&dip, (union v6addr *) &ip6->daddr);
		dip.p4 &= ~0xFFFF;
		ret = ipv6_store_daddr(skb, dip.addr, ETH_HLEN);
		if (IS_ERR(ret))
			return DROP_WRITE_ERROR;

		if (csum_off.offset) {
			__u32 zero_nat = 0;
			__be32 sum = csum_diff(&ct_state_new.rev_nat_index, 4, &zero_nat, 4, 0);
			if (csum_l4_replace(skb, l4_off, &csum_off, 0, sum, BPF_F_PSEUDO_HDR) < 0)
				return DROP_CSUM_L4;
		}
	}

	ret = ct_lookup6(get_ct_map6(&tuple), &tuple, skb, l4_off, CT_INGRESS,
			 &ct_state, &monitor);
	if (ret < 0)
		return ret;

	*forwarding_reason = ret;

	if (unlikely(ct_state.rev_nat_index)) {
		int ret2;

		ret2 = lb6_rev_nat(skb, l4_off, &csum_off,
				   ct_state.rev_nat_index, &tuple, 0);
		if (IS_ERR(ret2))
			return ret2;
	}

	if (!(cfg->flags & EP_F_SKIP_POLICY_INGRESS))
		verdict = policy_can_access_ingress(skb, src_label, tuple.dport,
				tuple.nexthdr, sizeof(tuple.saddr),
				&tuple.saddr, false);
	else
		verdict = TC_ACT_OK;

	/* Reply packets and related packets are allowed, all others must be
	 * permitted by policy */
	if (ret != CT_REPLY && ret != CT_RELATED && verdict < 0) {
		/* If the connection was previously known and packet is now
		 * denied, remove the connection tracking entry */
		if (ret == CT_ESTABLISHED)
			ct_delete6(get_ct_map6(&tuple), &tuple, skb);

		return verdict;
	}

	if (ret == CT_NEW) {
		ct_state_new.orig_dport = tuple.dport;
		ct_state_new.src_sec_id = src_label;
		ret = ct_create6(get_ct_map6(&tuple), &tuple, skb, CT_INGRESS, &ct_state_new);
		if (IS_ERR(ret))
			return ret;

		/* NOTE: tuple has been invalidated after this */
	}

	//ifindex = skb->cb[CB_IFINDEX];
	//if (ifindex)
	//	return redirect(ifindex, 0);

	return TC_ACT_OK;
}

__section_tail(CILIUM_MAP_CALLS, CILIUM_CALL_IPV6_TO_LXC) int tail_ipv6_policy(struct __sk_buff *skb) {
	int ret, ifindex = skb->cb[CB_IFINDEX];
	__u32 src_label = skb->cb[CB_SRC_LABEL];
	int forwarding_reason = 0;

	struct ep_config *cfg = lookup_ep_config();

	if (cfg)
		ret = ipv6_policy(skb, ifindex, src_label, &forwarding_reason, cfg);
	else
		ret = DROP_NO_CONFIG;

	if (IS_ERR(ret))
		return send_drop_notify(skb, src_label, SECLABEL, LXC_ID,
					ifindex, ret, TC_ACT_SHOT, METRIC_INGRESS);

	return ret;
}

#ifdef LXC_IPV4
static inline int __inline__
ipv4_policy(struct __sk_buff *skb, int ifindex, __u32 src_label, int *forwarding_reason, struct ep_config *cfg)
{
	struct ipv4_ct_tuple tuple = {};
	void *data, *data_end;
	struct iphdr *ip4;
	struct csum_offset csum_off = {};
	int ret, verdict, l4_off;
	struct ct_state ct_state = {};
	struct ct_state ct_state_new = {};
	__be32 orig_dip, orig_sip;
	bool is_fragment = false;
	__u32 monitor = 0;

	if (!revalidate_data(skb, &data, &data_end, &ip4))
		return DROP_INVALID;

	policy_clear_mark(skb);
	tuple.nexthdr = ip4->protocol;

	tuple.daddr = ip4->daddr;
	tuple.saddr = ip4->saddr;
	orig_dip = ip4->daddr;
	orig_sip = ip4->saddr;

	l4_off = ETH_HLEN + ipv4_hdrlen(ip4);
	csum_l4_offset_and_flags(tuple.nexthdr, &csum_off);
	is_fragment = ipv4_is_fragment(ip4);

	ret = ct_lookup4(get_ct_map4(&tuple), &tuple, skb, l4_off, CT_INGRESS, &ct_state,
			 &monitor);
	if (ret < 0)
		return ret;

	*forwarding_reason = ret;
	if (unlikely(ret == CT_REPLY && ct_state.rev_nat_index &&
		     !ct_state.loopback)) {
		int ret2;

		ret2 = lb4_rev_nat(skb, ETH_HLEN, l4_off, &csum_off,
				   &ct_state, &tuple,
				   REV_NAT_F_TUPLE_SADDR);
		if (IS_ERR(ret2))
			return ret2;
	}

	if (!(cfg->flags & EP_F_SKIP_POLICY_INGRESS))
		verdict = policy_can_access_ingress(skb, src_label, tuple.dport,
						    tuple.nexthdr,
						    sizeof(orig_sip),
						    &orig_sip, is_fragment);
	else
		verdict = TC_ACT_OK;

	/* Reply packets and related packets are allowed, all others must be
	 * permitted by policy */
	if (ret != CT_REPLY && ret != CT_RELATED && verdict < 0) {
		/* If the connection was previously known and packet is now
		 * denied, remove the connection tracking entry */
		if (ret == CT_ESTABLISHED)
			ct_delete4(get_ct_map4(&tuple), &tuple, skb);

		return verdict;
	}

	if (ret == CT_NEW) {
		ct_state_new.orig_dport = tuple.dport;
		ct_state_new.src_sec_id = src_label;
		ret = ct_create4(get_ct_map4(&tuple), &tuple, skb, CT_INGRESS, &ct_state_new);
		if (IS_ERR(ret))
			return ret;

		/* NOTE: tuple has been invalidated after this */
	}

	//ifindex = skb->cb[CB_IFINDEX];
	//if (ifindex)
	//	return redirect(ifindex, 0);

	return TC_ACT_OK;
}

__section_tail(CILIUM_MAP_CALLS, CILIUM_CALL_IPV4_TO_LXC) int tail_ipv4_policy(struct __sk_buff *skb) {
	struct ep_config *cfg = lookup_ep_config();
	int ret, ifindex = skb->cb[CB_IFINDEX];
	__u32 src_label = skb->cb[CB_SRC_LABEL];
	int forwarding_reason = 0;

	if (cfg)
		ret = ipv4_policy(skb, ifindex, src_label, &forwarding_reason, cfg);
	else
		ret = DROP_NO_CONFIG;
	if (IS_ERR(ret))
		return send_drop_notify(skb, src_label, SECLABEL, LXC_ID,
					ifindex, ret, TC_ACT_SHOT, METRIC_INGRESS);

	return ret;
}

#endif

/* Handle policy decisions as the packet makes its way towards the endpoint.
 * Previously, the packet may have come from another local endpoint, another
 * endpoint in the cluster, or from the big blue room (as identified by the
 * contents of skb->cb[CB_SRC_LABEL]). Determine whether the traffic may be
 * passed into the endpoint or if it needs further inspection by a userspace
 * proxy.
 */
__section_tail(CILIUM_MAP_POLICY, LXC_ID) int handle_policy(struct __sk_buff *skb)
{
	int ret, ifindex = skb->cb[CB_IFINDEX];
	__u32 src_label = skb->cb[CB_SRC_LABEL];

	switch (skb->protocol) {
	case bpf_htons(ETH_P_IPV6):
		ep_tail_call(skb, CILIUM_CALL_IPV6_TO_LXC);
		ret = DROP_MISSED_TAIL_CALL;
		break;

#ifdef LXC_IPV4
	case bpf_htons(ETH_P_IP):
		ep_tail_call(skb, CILIUM_CALL_IPV4_TO_LXC);
		ret = DROP_MISSED_TAIL_CALL;
		break;
#endif

	default:
		ret = DROP_UNKNOWN_L3;
		break;
	}

	if (IS_ERR(ret))
		return send_drop_notify(skb, src_label, SECLABEL, LXC_ID,
					ifindex, ret, TC_ACT_SHOT, METRIC_INGRESS);

	return ret;
}

__section("entry") int handle_egress(struct __sk_buff *skb)
{
	int ret;

	bpf_clear_cb(skb);

	send_trace_notify(skb, TRACE_FROM_LXC, SECLABEL, 0, 0, 0, 0,
			  TRACE_PAYLOAD_LEN);

	switch (skb->protocol) {
	case bpf_htons(ETH_P_IPV6):
		ep_tail_call(skb, CILIUM_CALL_IPV6_FROM_LXC);
		ret = DROP_MISSED_TAIL_CALL;
		break;

#ifdef LXC_IPV4
	case bpf_htons(ETH_P_IP):
		ep_tail_call(skb, CILIUM_CALL_IPV4_FROM_LXC);
		ret = DROP_MISSED_TAIL_CALL;
		break;
#endif

	default:
		ret = DROP_UNKNOWN_L3;
	}

	if (IS_ERR(ret))
		return send_drop_notify(skb, SECLABEL, 0, 0, 0, ret, TC_ACT_SHOT,
					METRIC_EGRESS);
	return ret;
}

BPF_LICENSE("GPL");