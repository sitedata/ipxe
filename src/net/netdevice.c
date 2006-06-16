/*
 * Copyright (C) 2006 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdint.h>
#include <byteswap.h>
#include <string.h>
#include <errno.h>
#include <gpxe/if_ether.h>
#include <gpxe/pkbuff.h>
#include <gpxe/tables.h>
#include <gpxe/process.h>
#include <gpxe/init.h>
#include <gpxe/netdevice.h>

/** @file
 *
 * Network device management
 *
 */

/**
 * Static single instance of a network device
 *
 * The gPXE API is designed to accommodate multiple network devices.
 * However, in the interests of code size, the implementation behind
 * the API supports only a single instance of a network device.
 *
 * No code outside of netdevice.c should ever refer directly to @c
 * static_single_netdev.
 *
 * Callers should always check the return status of alloc_netdev(),
 * register_netdev() etc.  In the current implementation this code
 * will be optimised out by the compiler, so there is no penalty.
 */
struct net_device static_single_netdev;

/** Registered network-layer protocols */
static struct net_protocol net_protocols[0] __table_start ( net_protocols );
static struct net_protocol net_protocols_end[0] __table_end ( net_protocols );

/** Network-layer addresses for @c static_single_netdev */
static struct net_address static_single_netdev_addresses[0]
	__table_start ( sgl_netdev_addresses );
static struct net_address static_single_netdev_addresses_end[0]
	__table_end ( sgl_netdev_addresses );

/** Recevied packet queue */
static LIST_HEAD ( rx_queue );

#warning "Remove this static IP address hack"
#include <ip.h>
#include <gpxe/ip.h>

/**
 * Register network device
 *
 * @v netdev		Network device
 * @ret rc		Return status code
 *
 * Adds the network device to the list of network devices.
 */
int register_netdev ( struct net_device *netdev ) {
	
#warning "Remove this static IP address hack"
	{
		const struct in_addr static_address = { htonl ( 0x0afefe01 ) };
		const struct in_addr static_netmask = { htonl ( 0xffffff00 ) };
		const struct in_addr static_gateway = { INADDR_NONE };
		int rc;
		
		if ( ( rc = add_ipv4_address ( netdev, static_address,
					       static_netmask,
					       static_gateway ) ) != 0 )
			return rc;
	}

	return 0;
}

/**
 * Unregister network device
 *
 * @v netdev		Network device
 *
 * Removes the network device from the list of network devices.
 */
void unregister_netdev ( struct net_device *netdev ) {

#warning "Remove this static IP address hack"
	del_ipv4_address ( netdev );

}

/**
 * Add packet to receive queue
 *
 * @v netdev		Network device
 * @v pkb		Packet buffer
 *
 * The packet is added to the RX queue.  This function takes ownership
 * of the packet buffer.
 */
void netdev_rx ( struct net_device *netdev, struct pk_buff *pkb ) {
	DBG ( "Packet received\n" );
	pkb->ll_protocol = netdev->ll_protocol;
	list_add_tail ( &pkb->list, &rx_queue );
}

/**
 * Identify network protocol
 *
 * @v net_proto		Network-layer protocol, in network-byte order
 * @ret net_protocol	Network-layer protocol, or NULL
 *
 * Identify a network-layer protocol from a protocol number, which
 * must be an ETH_P_XXX constant in network-byte order.
 */
struct net_protocol * find_net_protocol ( uint16_t net_proto ) {
	struct net_protocol *net_protocol;

	for ( net_protocol = net_protocols ; net_protocol < net_protocols_end ;
	      net_protocol++ ) {
		if ( net_protocol->net_proto == net_proto )
			return net_protocol;
	}
	return NULL;
}

/**
 * Identify network device by network-layer address
 *
 * @v net_protocol	Network-layer protocol
 * @v net_addr		Network-layer address
 * @ret netdev		Network device, or NULL
 *
 * Searches through all network devices to find the device with the
 * specified network-layer address.
 *
 * Note that even with a static single network device, this function
 * can still return NULL.
 */
struct net_device *
find_netdev_by_net_addr ( struct net_protocol *net_protocol,
			  void *net_addr ) {
	struct net_address *net_address;
	struct net_device *netdev = &static_single_netdev;
	
	for ( net_address = static_single_netdev_addresses ;
	      net_address < static_single_netdev_addresses_end ;
	      net_address ++ ) {
		if ( ( net_address->net_protocol == net_protocol ) &&
		     ( memcmp ( net_address->net_addr, net_addr,
				net_protocol->net_addr_len ) == 0 ) )
			return netdev;
	}

	return NULL;
}

/**
 * Poll for packet on all network devices
 *
 * @ret True		There are packets present in the receive queue
 * @ret False		There are no packets present in the receive queue
 *
 * Polls all network devices for received packets.  Any received
 * packets will be added to the RX packet queue via netdev_rx().
 */
int net_poll ( void ) {
	struct net_device *netdev = &static_single_netdev;

	DBG ( "Polling network\n" );
	netdev->poll ( netdev );

	return ( ! list_empty ( &rx_queue ) );
}

/**
 * Remove packet from receive queue
 *
 * @ret pkb		Packet buffer, or NULL
 *
 * Removes the first packet from the RX queue and returns it.
 * Ownership of the packet is transferred to the caller.
 */
struct pk_buff * net_rx_dequeue ( void ) {
	struct pk_buff *pkb;

	list_for_each_entry ( pkb, &rx_queue, list ) {
		list_del ( &pkb->list );
		return pkb;
	}
	return NULL;
}

/**
 * Process received packet
 *
 * @v pkb		Packet buffer
 * @ret rc		Return status code
 *
 * Processes a packet received from the network (and, usually, removed
 * from the RX queue by net_rx_dequeue()).  This call takes ownership
 * of the packet buffer.
 */
int net_rx_process ( struct pk_buff *pkb ) {
	struct ll_protocol *ll_protocol;
	struct ll_header llhdr;
	struct net_protocol *net_protocol;
	int rc;

	/* Parse link-layer header */
	ll_protocol = pkb->ll_protocol;
	ll_protocol->parse_llh ( pkb, &llhdr );
	
	/* Identify network-layer protocol */
	net_protocol = find_net_protocol ( llhdr.net_proto );
	if ( ! net_protocol ) {
		DBG ( "Unknown network-layer protocol %x\n",
		      ntohs ( llhdr.net_proto ) );
		free_pkb ( pkb );
		return -EPROTONOSUPPORT;
	}
	pkb->net_protocol = net_protocol;
	
	/* Strip off link-layer header */
#warning "Temporary hack"
	pkb_pull ( pkb, ETH_HLEN );
	
	/* Hand off to network layer */
	if ( ( rc = net_protocol->rx_process ( pkb ) ) != 0 ) {
		DBG ( "Network-layer protocol dropped packet\n" );
		return rc;
	}

	return 0;
}

/**
 * Single-step the network stack
 *
 * @v process		Network stack process
 *
 * This polls all interfaces for any received packets, and processes
 * at most one packet from the RX queue.
 *
 * We avoid processing all received packets, because processing the
 * received packet can trigger transmission of a new packet (e.g. an
 * ARP response).  Since TX completions will be processed as part of
 * the poll operation, it is easy to overflow small TX queues if
 * multiple packets are processed per poll.
 */
static void net_step ( struct process *process ) {
	struct pk_buff *pkb;

	/* Poll for new packets */
	net_poll();

	/* Handle at most one received packet */
	if ( ( pkb = net_rx_dequeue () ) ) {
		net_rx_process ( pkb );
		DBG ( "Processed received packet\n" );
	}

	/* Re-schedule ourself */
	schedule ( process );
}

/** Networking stack process */
static struct process net_process = {
	.step = net_step,
};

/** Initialise the networking stack process */
static void init_net ( void ) {
	schedule ( &net_process );
}

INIT_FN ( INIT_PROCESS, init_net, NULL, NULL );
