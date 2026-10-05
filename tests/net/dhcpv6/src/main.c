/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/net/dummy.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>

#include "../../../subsys/net/lib/dhcpv6/dhcpv6.c"
#include "../../../subsys/net/ip/icmpv6.h"

static struct net_in6_addr test_addr = { { { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
					 0, 0, 0, 0, 0, 0, 0, 0x1 } } };
static struct net_in6_addr test_prefix = { { { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
					   0, 0, 0, 0, 0, 0, 0, 0 } } };
static uint8_t test_prefix_len = 64;
static uint8_t test_preference;
static struct net_dhcpv6_duid_storage test_serverid;
static struct net_mgmt_event_callback net_mgmt_cb;

typedef void (*test_dhcpv6_pkt_fn_t)(struct net_if *iface,
				     struct net_pkt *pkt);

typedef int (*test_dhcpv6_options_fn_t)(struct net_if *iface,
					struct net_pkt *pkt,
					enum dhcpv6_msg_type msg_type);

struct test_dhcpv6_context {
	uint8_t mac[sizeof(struct net_eth_addr)];
	struct net_if *iface;
	test_dhcpv6_pkt_fn_t test_fn;
	struct k_sem tx_sem;
	struct k_sem exchange_complete_sem;
	bool reset_dhcpv6;
};

struct test_dhcpv6_context test_ctx;

static void test_iface_init(struct net_if *iface)
{
	struct test_dhcpv6_context *ctx = net_if_get_device(iface)->data;

	/* Generate and assign MAC. */
	/* 00-00-5E-00-53-xx Documentation RFC 7042 */
	ctx->mac[0] = 0x00;
	ctx->mac[1] = 0x00;
	ctx->mac[2] = 0x5E;
	ctx->mac[3] = 0x00;
	ctx->mac[4] = 0x53;
	ctx->mac[5] = 0x00;

	net_if_set_link_addr(iface, ctx->mac, sizeof(ctx->mac), NET_LINK_ETHERNET);
}

static bool is_dhcpv6_client_msg(struct net_pkt *pkt)
{
	struct net_pkt_cursor backup;
	struct net_ipv6_hdr ip_hdr;
	struct net_udp_hdr udp_hdr;
	bool ret = false;

	net_pkt_cursor_backup(pkt, &backup);
	net_pkt_cursor_init(pkt);

	if (net_pkt_read(pkt, &ip_hdr, sizeof(ip_hdr)) == 0 &&
	    ip_hdr.nexthdr == NET_IPPROTO_UDP &&
	    net_pkt_read(pkt, &udp_hdr, sizeof(udp_hdr)) == 0 &&
	    udp_hdr.dst_port == net_htons(DHCPV6_SERVER_PORT)) {
		ret = true;
	}

	net_pkt_cursor_restore(pkt, &backup);

	return ret;
}

static int test_send(const struct device *dev, struct net_pkt *pkt)
{
	struct test_dhcpv6_context *ctx = dev->data;

	/* Neighbor Discovery traffic is not of interest. */
	if (!is_dhcpv6_client_msg(pkt)) {
		return 0;
	}

	if (ctx->test_fn != NULL) {
		ctx->test_fn(net_pkt_iface(pkt), pkt);
	}

	k_sem_give(&ctx->tx_sem);

	return 0;
}

static struct dummy_api test_if_api = {
	.iface_api.init = test_iface_init,
	.send = test_send,
};

NET_DEVICE_INIT(test_dhcpv6, "test_dhcpv6", NULL, NULL, &test_ctx, NULL,
		CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &test_if_api,
		DUMMY_L2, NET_L2_GET_CTX_TYPE(DUMMY_L2), NET_IPV6_MTU);

static void set_dhcpv6_test_fn(test_dhcpv6_pkt_fn_t test_fn)
{
	test_ctx.test_fn = test_fn;
}

static void set_test_addr_on_iface(struct net_if *iface)
{
	memcpy(&test_ctx.iface->config.dhcpv6.addr, &test_addr,
	       sizeof(test_ctx.iface->config.dhcpv6.addr));
	memcpy(&test_ctx.iface->config.dhcpv6.prefix, &test_prefix,
	       sizeof(test_ctx.iface->config.dhcpv6.prefix));
	test_ctx.iface->config.dhcpv6.prefix_len = test_prefix_len;
}

static void clear_test_addr_on_iface(struct net_if *iface)
{
	memset(&test_ctx.iface->config.dhcpv6.addr, 0,
	       sizeof(test_ctx.iface->config.dhcpv6.addr));
	memset(&test_ctx.iface->config.dhcpv6.prefix, 0,
	       sizeof(test_ctx.iface->config.dhcpv6.prefix));
	test_ctx.iface->config.dhcpv6.prefix_len = 0;
}

static void generate_fake_server_duid(void)
{
	struct net_dhcpv6_duid_storage *serverid = &test_serverid;
	struct dhcpv6_duid_ll *duid_ll =
				(struct dhcpv6_duid_ll *)&serverid->duid.buf;
	uint8_t fake_mac[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };

	memset(serverid, 0, sizeof(*serverid));

	UNALIGNED_PUT(net_htons(DHCPV6_DUID_TYPE_LL),
		      UNALIGNED_MEMBER_ADDR(serverid, duid.type));
	UNALIGNED_PUT(net_htons(DHCPV6_HARDWARE_ETHERNET_TYPE),
		      UNALIGNED_MEMBER_ADDR(duid_ll, hw_type));
	memcpy(duid_ll->ll_addr, fake_mac, sizeof(fake_mac));

	serverid->length = DHCPV6_DUID_LL_HEADER_SIZE + sizeof(fake_mac);
}

static void set_fake_server_duid(struct net_if *iface)
{
	memcpy(&iface->config.dhcpv6.serverid, &test_serverid,
	       sizeof(test_serverid));
}

#define TEST_MSG_SIZE 256

static struct net_pkt *test_dhcpv6_create_message(
		struct net_if *iface, enum dhcpv6_msg_type msg_type,
		test_dhcpv6_options_fn_t set_options_fn)
{
	struct net_in6_addr *local_addr;
	struct net_in6_addr peer_addr;
	struct net_pkt *pkt;

	local_addr = net_if_ipv6_get_ll(iface, NET_ADDR_ANY_STATE);
	if (local_addr == NULL) {
		return NULL;
	}

	/* Create a peer address from my address but invert the last byte
	 * so that the address is not the same. This is needed as we drop
	 * the packet if source address is our own address.
	 */
	memcpy(&peer_addr, local_addr, sizeof(peer_addr));
	peer_addr.s6_addr[15] = ~peer_addr.s6_addr[15];

	pkt = net_pkt_alloc_with_buffer(iface, TEST_MSG_SIZE, NET_AF_INET6,
					NET_IPPROTO_UDP, K_FOREVER);
	if (pkt == NULL) {
		return NULL;
	}

	if (net_ipv6_create(pkt, &peer_addr, local_addr) < 0 ||
	    net_udp_create(pkt, net_htons(DHCPV6_SERVER_PORT),
			   net_htons(DHCPV6_CLIENT_PORT)) < 0) {
		goto fail;
	}

	dhcpv6_generate_tid(iface);

	if (dhcpv6_add_header(pkt, msg_type, iface->config.dhcpv6.tid) < 0) {
		goto fail;
	}

	if (set_options_fn(iface, pkt, msg_type) < 0) {
		goto fail;
	}

	net_pkt_cursor_init(pkt);
	net_ipv6_finalize(pkt, NET_IPPROTO_UDP);
	net_pkt_cursor_init(pkt);

	return pkt;

fail:
	net_pkt_unref(pkt);

	return NULL;
}

static void evt_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			struct net_if *iface)
{
	ARG_UNUSED(cb);

	if (mgmt_event == NET_EVENT_IF_UP) {
		struct net_in6_addr lladdr;

		net_ipv6_addr_create_iid(&lladdr, net_if_get_link_addr(test_ctx.iface));
		(void)net_if_ipv6_addr_add(test_ctx.iface, &lladdr, NET_ADDR_AUTOCONF, 0);
	}
}

static void *dhcpv6_tests_setup(void)
{
	struct net_in6_addr lladdr;

	test_ctx.iface = net_if_get_first_by_type(&NET_L2_GET_NAME(DUMMY));

	net_ipv6_addr_create_iid(&lladdr, net_if_get_link_addr(test_ctx.iface));
	(void)net_if_ipv6_addr_add(test_ctx.iface, &lladdr, NET_ADDR_AUTOCONF, 0);

	k_sem_init(&test_ctx.tx_sem, 0, 1);
	k_sem_init(&test_ctx.exchange_complete_sem, 0, 1);

	generate_fake_server_duid();

	net_mgmt_init_event_callback(&net_mgmt_cb, evt_handler, NET_EVENT_IF_UP);
	net_mgmt_add_event_callback(&net_mgmt_cb);

	return NULL;
}

static void dhcpv6_tests_before(void *fixture)
{
	ARG_UNUSED(fixture);

	test_ctx.reset_dhcpv6 = false;

	set_dhcpv6_test_fn(NULL);
	k_sem_reset(&test_ctx.tx_sem);
	k_sem_reset(&test_ctx.exchange_complete_sem);

	memset(&test_ctx.iface->config.dhcpv6, 0,
	       sizeof(test_ctx.iface->config.dhcpv6));

	dhcpv6_generate_client_duid(test_ctx.iface);
	test_ctx.iface->config.dhcpv6.state = NET_DHCPV6_DISABLED;
	test_ctx.iface->config.dhcpv6.addr_iaid = 10;
	test_ctx.iface->config.dhcpv6.prefix_iaid = 20;
	test_ctx.iface->config.dhcpv6.exchange_start = k_uptime_get();
	test_ctx.iface->config.dhcpv6.params = (struct net_dhcpv6_params){
		.request_addr = true,
		.request_prefix = true,
	};

	test_preference = 100;

	net_if_ipv6_addr_rm(test_ctx.iface, &test_addr);
	net_if_ipv6_prefix_rm(test_ctx.iface, &test_prefix, test_prefix_len);
}

static void dhcpv6_tests_after(void *fixture)
{
	ARG_UNUSED(fixture);

	set_dhcpv6_test_fn(NULL);

	if (test_ctx.reset_dhcpv6) {
		net_dhcpv6_stop(test_ctx.iface);
	}
}

static void verify_dhcpv6_header(struct net_if *iface, struct net_pkt *pkt,
				 enum dhcpv6_msg_type msg_type)
{
	uint8_t tid[DHCPV6_TID_SIZE];
	uint8_t type;
	int ret;

	(void)net_pkt_skip(pkt, NET_IPV6UDPH_LEN);

	ret = net_pkt_read_u8(pkt, &type);
	zassert_ok(ret, "DHCPv6 header incomplete (type)");
	zassert_equal(type, msg_type, "Invalid message type");

	ret = net_pkt_read(pkt, tid, sizeof(tid));
	zassert_ok(ret, "DHCPv6 header incomplete (tid)");
	zassert_mem_equal(tid, iface->config.dhcpv6.tid, sizeof(tid),
			  "Transaction ID doesn't match ID of the current exchange");
}

static void verify_dhcpv6_clientid(struct net_if *iface, struct net_pkt *pkt)
{
	struct net_dhcpv6_duid_storage duid;
	int ret;

	ret = dhcpv6_find_clientid(pkt, &duid);
	zassert_ok(ret, "Missing Client ID option");
	zassert_equal(duid.length, iface->config.dhcpv6.clientid.length,
		      "Invalid Client ID length");
	zassert_mem_equal(&duid.duid, &iface->config.dhcpv6.clientid.duid,
			  duid.length, "Invalid Client ID value");
}

static void verify_dhcpv6_serverid(struct net_if *iface, struct net_pkt *pkt)
{
	struct net_dhcpv6_duid_storage duid;
	int ret;

	ret = dhcpv6_find_serverid(pkt, &duid);
	zassert_ok(ret, "Missing Server ID option");
	zassert_equal(duid.length, iface->config.dhcpv6.serverid.length,
		      "Invalid Server ID length");
	zassert_mem_equal(&duid.duid, &iface->config.dhcpv6.serverid.duid,
			  duid.length, "Invalid Server ID value");
}

static void verify_dhcpv6_no_serverid(struct net_if *iface, struct net_pkt *pkt)
{
	struct net_dhcpv6_duid_storage duid;
	int ret;

	ret = dhcpv6_find_serverid(pkt, &duid);
	zassert_not_equal(ret, 0, "Server ID option should not be present");
}

static void verify_dhcpv6_elapsed_time(struct net_if *iface, struct net_pkt *pkt,
				       uint16_t min_accepted, uint16_t max_accepted)
{
	struct net_pkt_cursor backup;
	uint16_t elapsed_time;
	uint16_t length;
	int ret;

	net_pkt_cursor_backup(pkt, &backup);

	ret = dhcpv6_find_option(pkt, DHCPV6_OPTION_CODE_ELAPSED_TIME, &length);
	zassert_ok(ret, "Missing Elapsed time option");
	zassert_equal(length, sizeof(uint16_t), "Invalid Elapsed time length");

	ret = net_pkt_read_be16(pkt, &elapsed_time);
	zassert_ok(ret, "Failed to read Elapsed time option");
	zassert_between_inclusive(elapsed_time, min_accepted, max_accepted,
				  "Elapsed time not in accepted range");

	net_pkt_cursor_restore(pkt, &backup);
}

static void verify_dhcpv6_ia_na(struct net_if *iface, struct net_pkt *pkt,
				struct net_in6_addr *addr)
{
	struct dhcpv6_ia_na ia_na;
	int ret;

	ret = dhcpv6_find_ia_na(pkt, &ia_na);
	zassert_ok(ret, "Missing IA NA option");
	zassert_equal(ia_na.iaid, iface->config.dhcpv6.addr_iaid,
		      "Incorrect IA NA IAID");
	zassert_equal(ia_na.t1, 0, "T1 should be set to 0 by the client");
	zassert_equal(ia_na.t2, 0, "T2 should be set to 0 by the client");

	if (addr == NULL) {
		zassert_equal(ia_na.iaaddr.status, DHCPV6_STATUS_NO_ADDR_AVAIL,
			      "Address should not be present");
		return;
	}

	zassert_equal(ia_na.iaaddr.status, DHCPV6_STATUS_SUCCESS, "Invalid status");
	zassert_equal(ia_na.iaaddr.preferred_lifetime, 0,
		      "Preferred lifetime should be set to 0 by the client");
	zassert_equal(ia_na.iaaddr.valid_lifetime, 0,
		      "Valid lifetime should be set to 0 by the client");
	zassert_mem_equal(&ia_na.iaaddr.addr, addr, sizeof(ia_na.iaaddr.addr),
			  "Incorrect address");
}

static void verify_dhcpv6_ia_pd(struct net_if *iface, struct net_pkt *pkt,
				struct net_in6_addr *prefix, uint8_t prefix_len)
{
	struct dhcpv6_ia_pd ia_pd;
	int ret;

	ret = dhcpv6_find_ia_pd(pkt, &ia_pd);
	zassert_ok(ret, "Missing IA PD option");
		zassert_equal(ia_pd.iaid, iface->config.dhcpv6.prefix_iaid,
		      "Incorrect IA PD IAID");
	zassert_equal(ia_pd.t1, 0, "T1 should be set to 0 by the client");
	zassert_equal(ia_pd.t2, 0, "T2 should be set to 0 by the client");

	if (prefix == NULL) {
		zassert_equal(ia_pd.iaprefix.status, DHCPV6_STATUS_NO_PREFIX_AVAIL,
			      "Prefix should not be present");
		return;
	}

	zassert_equal(ia_pd.iaprefix.status, DHCPV6_STATUS_SUCCESS, "Invalid status");
	zassert_equal(ia_pd.iaprefix.preferred_lifetime, 0,
		      "Preferred lifetime should be set to 0 by the client");
	zassert_equal(ia_pd.iaprefix.valid_lifetime, 0,
		      "Valid lifetime should be set to 0 by the client");
	zassert_equal(ia_pd.iaprefix.prefix_len, prefix_len,
		      "Incorrect prefix length");
	zassert_mem_equal(&ia_pd.iaprefix.prefix, prefix,
			  sizeof(ia_pd.iaprefix.prefix), "Incorrect prefix");
}

static void verify_dhcpv6_no_reconfigure_accept(struct net_if *iface,
						struct net_pkt *pkt)
{
	struct net_pkt_cursor backup;
	uint16_t length;
	int ret;

	net_pkt_cursor_backup(pkt, &backup);

	ret = dhcpv6_find_option(pkt, DHCPV6_OPTION_CODE_RECONF_ACCEPT, &length);
	zassert_not_equal(ret, 0, "Reconfigure accept option should not be present");

	net_pkt_cursor_restore(pkt, &backup);
}

static void verify_dhcpv6_oro(struct net_pkt *pkt, uint16_t code)
{
	struct net_pkt_cursor backup;
	uint16_t length;
	uint16_t oro;
	int ret;

	net_pkt_cursor_backup(pkt, &backup);

	ret = dhcpv6_find_option(pkt, DHCPV6_OPTION_CODE_ORO, &length);
	zassert_ok(ret, "ORO option not found");
	zassert_true(length >= sizeof(uint16_t) && length % sizeof(uint16_t) == 0,
		     "Invalid ORO length");

	while (length >= sizeof(uint16_t)) {
		ret = net_pkt_read_be16(pkt, &oro);
		zassert_ok(ret, "ORO read error");
		length -= sizeof(uint16_t);

		if (oro == code) {
			break;
		}
	}

	zassert_equal(oro, code, "No option %u request present", code);

	net_pkt_cursor_restore(pkt, &backup);
}

static void verify_dhcpv6_oro_sol_max_rt(struct net_if *iface,
					 struct net_pkt *pkt)
{
	verify_dhcpv6_oro(pkt, DHCPV6_OPTION_CODE_SOL_MAX_RT);
}

static void verify_solicit_message(struct net_if *iface, struct net_pkt *pkt)
{
	/* Verify header */
	verify_dhcpv6_header(iface, pkt, DHCPV6_MSG_TYPE_SOLICIT);

	/* Verify options */
	verify_dhcpv6_clientid(iface, pkt);
	verify_dhcpv6_no_serverid(iface, pkt);
	verify_dhcpv6_elapsed_time(iface, pkt, 0, 10);
	verify_dhcpv6_ia_na(iface, pkt, NULL);
	verify_dhcpv6_ia_pd(iface, pkt, NULL, 0);
	verify_dhcpv6_no_reconfigure_accept(iface, pkt);
	verify_dhcpv6_oro_sol_max_rt(iface, pkt);
}

/* Verify that outgoing DHCPv6 Solicit has a valid format and includes all
 * mandatory options.
 */
ZTEST(dhcpv6_tests, test_solicit_message_format)
{
	int ret;

	set_dhcpv6_test_fn(verify_solicit_message);

	ret = dhcpv6_send_solicit(test_ctx.iface);
	zassert_ok(ret, "dhcpv6_send_solicit failed");

	ret = k_sem_take(&test_ctx.tx_sem, K_SECONDS(1));
	zassert_ok(ret, "Packet not transmitted");
}

static void verify_request_message(struct net_if *iface, struct net_pkt *pkt)
{
	/* Verify header */
	verify_dhcpv6_header(iface, pkt, DHCPV6_MSG_TYPE_REQUEST);

	/* Verify options */
	verify_dhcpv6_clientid(iface, pkt);
	verify_dhcpv6_serverid(iface, pkt);
	verify_dhcpv6_elapsed_time(iface, pkt, 0, 10);
	verify_dhcpv6_ia_na(iface, pkt, NULL);
	verify_dhcpv6_ia_pd(iface, pkt, NULL, 0);
	verify_dhcpv6_no_reconfigure_accept(iface, pkt);
	verify_dhcpv6_oro_sol_max_rt(iface, pkt);
}

/* Verify that outgoing DHCPv6 Request has a valid format and includes all
 * mandatory options.
 */
ZTEST(dhcpv6_tests, test_request_message_format)
{
	int ret;

	set_fake_server_duid(test_ctx.iface);
	set_dhcpv6_test_fn(verify_request_message);

	ret = dhcpv6_send_request(test_ctx.iface);
	zassert_ok(ret, "dhcpv6_send_request failed");

	ret = k_sem_take(&test_ctx.tx_sem, K_SECONDS(1));
	zassert_ok(ret, "Packet not transmitted");
}

static void verify_confirm_message(struct net_if *iface, struct net_pkt *pkt)
{
	/* Verify header */
	verify_dhcpv6_header(iface, pkt, DHCPV6_MSG_TYPE_CONFIRM);

	/* Verify options */
	verify_dhcpv6_clientid(iface, pkt);
	verify_dhcpv6_no_serverid(iface, pkt);
	verify_dhcpv6_elapsed_time(iface, pkt, 0, 10);
	verify_dhcpv6_ia_na(iface, pkt, &test_addr);
}

/* Verify that outgoing DHCPv6 Confirm has a valid format and includes all
 * mandatory options.
 */
ZTEST(dhcpv6_tests, test_confirm_message_format)
{
	int ret;

	set_test_addr_on_iface(test_ctx.iface);
	set_dhcpv6_test_fn(verify_confirm_message);

	ret = dhcpv6_send_confirm(test_ctx.iface);
	zassert_ok(ret, "dhcpv6_send_confirm failed");

	ret = k_sem_take(&test_ctx.tx_sem, K_SECONDS(1));
	zassert_ok(ret, "Packet not transmitted");
}

void verify_renew_message(struct net_if *iface, struct net_pkt *pkt)
{
	/* Verify header */
	verify_dhcpv6_header(iface, pkt, DHCPV6_MSG_TYPE_RENEW);

	/* Verify options */
	verify_dhcpv6_clientid(iface, pkt);
	verify_dhcpv6_serverid(iface, pkt);
	verify_dhcpv6_elapsed_time(iface, pkt, 0, 10);
	verify_dhcpv6_ia_na(iface, pkt, &test_addr);
	verify_dhcpv6_ia_pd(iface, pkt, &test_prefix, test_prefix_len);
	verify_dhcpv6_oro_sol_max_rt(iface, pkt);
}

/* Verify that outgoing DHCPv6 Renew has a valid format and includes all
 * mandatory options.
 */
ZTEST(dhcpv6_tests, test_renew_message_format)
{
	int ret;

	set_test_addr_on_iface(test_ctx.iface);
	set_fake_server_duid(test_ctx.iface);
	set_dhcpv6_test_fn(verify_renew_message);

	ret = dhcpv6_send_renew(test_ctx.iface);
	zassert_ok(ret, "dhcpv6_send_renew failed");

	ret = k_sem_take(&test_ctx.tx_sem, K_SECONDS(1));
	zassert_ok(ret, "Packet not transmitted");
}

static void verify_rebind_message(struct net_if *iface, struct net_pkt *pkt)
{
	/* Verify header */
	verify_dhcpv6_header(iface, pkt, DHCPV6_MSG_TYPE_REBIND);

	/* Verify options */
	verify_dhcpv6_clientid(iface, pkt);
	verify_dhcpv6_no_serverid(iface, pkt);
	verify_dhcpv6_elapsed_time(iface, pkt, 0, 10);
	verify_dhcpv6_ia_na(iface, pkt, &test_addr);
	verify_dhcpv6_ia_pd(iface, pkt, &test_prefix, test_prefix_len);
	verify_dhcpv6_oro_sol_max_rt(iface, pkt);
}

/* Verify that outgoing DHCPv6 Rebind has a valid format and includes all
 * mandatory options.
 */
ZTEST(dhcpv6_tests, test_rebind_message_format)
{
	int ret;

	set_test_addr_on_iface(test_ctx.iface);
	set_dhcpv6_test_fn(verify_rebind_message);

	ret = dhcpv6_send_rebind(test_ctx.iface);
	zassert_ok(ret, "dhcpv6_send_rebind failed");

	ret = k_sem_take(&test_ctx.tx_sem, K_SECONDS(1));
	zassert_ok(ret, "Packet not transmitted");
}

static int set_generic_client_options(struct net_if *iface, struct net_pkt *pkt,
				      enum dhcpv6_msg_type msg_type)
{
	int ret;

	/* Simulate a minimum subset of valid options */
	ret = dhcpv6_add_option_clientid(pkt, &iface->config.dhcpv6.clientid);
	if (ret < 0) {
		return ret;
	}

	if (msg_type == DHCPV6_MSG_TYPE_REQUEST ||
	    msg_type == DHCPV6_MSG_TYPE_RENEW ||
	    msg_type == DHCPV6_MSG_TYPE_RELEASE ||
	    msg_type == DHCPV6_MSG_TYPE_DECLINE) {
		ret = dhcpv6_add_option_serverid(pkt, &test_serverid);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

/* Verify that DHCPv6 client rejects all messages other than Advertise, Reply
 * and Reconfigure.
 */
ZTEST(dhcpv6_tests, test_input_reject_client_initiated_messages)
{

	enum dhcpv6_msg_type type;
	enum net_verdict result;
	struct net_pkt *pkt;

	test_ctx.iface->config.dhcpv6.state = NET_DHCPV6_INIT;

	for (type = DHCPV6_MSG_TYPE_SOLICIT;
	     type <= DHCPV6_MSG_TYPE_RELAY_REPL; type++) {
		if (type == DHCPV6_MSG_TYPE_ADVERTISE ||
		    type == DHCPV6_MSG_TYPE_REPLY ||
		    type == DHCPV6_MSG_TYPE_RECONFIGURE) {
			continue;
		}

		pkt = test_dhcpv6_create_message(test_ctx.iface, type,
						 set_generic_client_options);
		zassert_not_null(pkt, "Failed to create fake pkt");

		result = net_ipv6_input(pkt);
		zassert_equal(result, NET_DROP, "Should've drop the message");

		net_pkt_unref(pkt);
	}
}

static int set_advertise_options(struct net_if *iface, struct net_pkt *pkt,
				 enum dhcpv6_msg_type msg_type)
{
	struct dhcpv6_ia_na test_ia_na = {
		.iaid = iface->config.dhcpv6.addr_iaid,
		.t1 = 60,
		.t2 = 120,
		.iaaddr.addr = test_addr,
		.iaaddr.preferred_lifetime = 120,
		.iaaddr.valid_lifetime = 240,
	};
	struct dhcpv6_ia_pd test_ia_pd = {
		.iaid = iface->config.dhcpv6.prefix_iaid,
		.t1 = 60,
		.t2 = 120,
		.iaprefix.prefix = test_prefix,
		.iaprefix.prefix_len = test_prefix_len,
		.iaprefix.preferred_lifetime = 120,
		.iaprefix.valid_lifetime = 240,
	};
	int ret;

	ret = dhcpv6_add_option_clientid(pkt, &iface->config.dhcpv6.clientid);
	if (ret < 0) {
		return ret;
	}

	ret = dhcpv6_add_option_serverid(pkt, &test_serverid);
	if (ret < 0) {
		return ret;
	}

	if (test_ctx.iface->config.dhcpv6.params.request_addr) {
		ret = dhcpv6_add_option_ia_na(pkt, &test_ia_na, true);
		if (ret < 0) {
			return ret;
		}
	}

	if (test_ctx.iface->config.dhcpv6.params.request_prefix) {
		ret = dhcpv6_add_option_ia_pd(pkt, &test_ia_pd, true);
		if (ret < 0) {
			return ret;
		}
	}

	/* Server specific options */
	ret = dhcpv6_add_option_header(pkt, DHCPV6_OPTION_CODE_PREFERENCE,
				       DHCPV6_OPTION_PREFERENCE_SIZE);
	if (ret < 0) {
		return ret;
	}

	ret = net_pkt_write_u8(pkt, test_preference);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

/* Verify that DHCPv6 client only accepts Advertise messages in Soliciting state */
ZTEST(dhcpv6_tests, test_input_advertise)
{
	enum net_verdict result;
	struct net_pkt *pkt;
	enum net_dhcpv6_state state;

	for (state = NET_DHCPV6_DISABLED; state <= NET_DHCPV6_BOUND; state++) {
		test_ctx.iface->config.dhcpv6.state = state;

		pkt = test_dhcpv6_create_message(test_ctx.iface,
						 DHCPV6_MSG_TYPE_ADVERTISE,
						 set_advertise_options);
		zassert_not_null(pkt, "Failed to create pkt");

		result = net_ipv6_input(pkt);

		switch (state) {
		case NET_DHCPV6_SOLICITING:
			zassert_equal(result, NET_OK, "Message should've been processed");

			/* Verify that Advertise actually updated DHPCv6 context. */
			zassert_equal(test_ctx.iface->config.dhcpv6.server_preference,
				      test_preference, "Preference not set");
			zassert_equal(test_ctx.iface->config.dhcpv6.serverid.length,
				      test_serverid.length, "Invalid Server ID length");
			zassert_mem_equal(&test_ctx.iface->config.dhcpv6.serverid.duid,
					  &test_serverid.duid, test_serverid.length,
					  "Invalid Server ID value");

			break;
		default:
			zassert_equal(result, NET_DROP, "Should've drop the message");
			break;

		}

		net_pkt_unref(pkt);
	}
}

static int set_reply_options(struct net_if *iface, struct net_pkt *pkt,
			     enum dhcpv6_msg_type msg_type)
{
	struct dhcpv6_ia_na test_ia_na = {
		.iaid = iface->config.dhcpv6.addr_iaid,
		.t1 = 60,
		.t2 = 120,
		.iaaddr.addr = test_addr,
		.iaaddr.preferred_lifetime = 120,
		.iaaddr.valid_lifetime = 240,
	};
	struct dhcpv6_ia_pd test_ia_pd = {
		.iaid = iface->config.dhcpv6.prefix_iaid,
		.t1 = 60,
		.t2 = 120,
		.iaprefix.prefix = test_prefix,
		.iaprefix.prefix_len = test_prefix_len,
		.iaprefix.preferred_lifetime = 120,
		.iaprefix.valid_lifetime = 240,
	};
	int ret;

	ret = dhcpv6_add_option_clientid(pkt, &iface->config.dhcpv6.clientid);
	if (ret < 0) {
		return ret;
	}

	ret = dhcpv6_add_option_serverid(pkt, &test_serverid);
	if (ret < 0) {
		return ret;
	}

	if (iface->config.dhcpv6.state == NET_DHCPV6_CONFIRMING) {
		ret = dhcpv6_add_option_header(
			pkt, DHCPV6_OPTION_CODE_STATUS_CODE,
			DHCPV6_OPTION_STATUS_CODE_HEADER_SIZE);
		if (ret < 0) {
			return ret;
		}

		ret = net_pkt_write_be16(pkt, DHCPV6_STATUS_SUCCESS);
		if (ret < 0) {
			return ret;
		}

		return 0;
	}

	ret = dhcpv6_add_option_ia_na(pkt, &test_ia_na, true);
	if (ret < 0) {
		return ret;
	}

	ret = dhcpv6_add_option_ia_pd(pkt, &test_ia_pd, true);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

/* Verify that DHCPv6 client accepts Reply messages in Requesting, Confirming,
 * Renewing and Rebinding states
 */
ZTEST(dhcpv6_tests, test_input_reply)
{
	enum net_verdict result;
	struct net_pkt *pkt;
	enum net_dhcpv6_state state;

	for (state = NET_DHCPV6_DISABLED; state <= NET_DHCPV6_BOUND; state++) {
		test_ctx.iface->config.dhcpv6.state = state;

		set_fake_server_duid(test_ctx.iface);
		clear_test_addr_on_iface(test_ctx.iface);

		pkt = test_dhcpv6_create_message(test_ctx.iface,
						 DHCPV6_MSG_TYPE_REPLY,
						 set_reply_options);
		zassert_not_null(pkt, "Failed to create pkt");

		result = net_ipv6_input(pkt);

		switch (state) {
		case NET_DHCPV6_CONFIRMING:
		case NET_DHCPV6_REQUESTING:
		case NET_DHCPV6_RENEWING:
		case NET_DHCPV6_REBINDING:
		case NET_DHCPV6_INFO_REQUESTING:
			zassert_equal(result, NET_OK, "Message should've been processed");

			/* Confirm and Information-request are exceptions, as
			 * they do not update address on an interface.
			 */
			if (state == NET_DHCPV6_CONFIRMING ||
			    state == NET_DHCPV6_INFO_REQUESTING) {
				break;
			}

			/* Verify that Reply actually updated DHPCv6 context. */
			zassert_mem_equal(&test_ctx.iface->config.dhcpv6.addr,
					  &test_addr, sizeof(test_addr),
					  "Invalid address (state %s)",
					  net_dhcpv6_state_name(state));
			zassert_mem_equal(&test_ctx.iface->config.dhcpv6.prefix,
					  &test_prefix, sizeof(test_prefix),
					  "Invalid prefix (state %s)",
					  net_dhcpv6_state_name(state));
			zassert_equal(test_ctx.iface->config.dhcpv6.prefix_len,
				      test_prefix_len, "Invalid prefix len (state %s)",
				      net_dhcpv6_state_name(state));

			break;
		default:
			zassert_equal(result, NET_DROP, "Should've drop the message");
			break;

		}

		net_pkt_unref(pkt);
	}
}

static void test_solicit_expect_request_send_reply(struct net_if *iface,
						   struct net_pkt *pkt)
{
	struct net_pkt *reply;
	int result;

	/* Verify header */
	verify_dhcpv6_header(iface, pkt, DHCPV6_MSG_TYPE_REQUEST);

	/* Verify options */
	verify_dhcpv6_clientid(iface, pkt);
	verify_dhcpv6_serverid(iface, pkt);
	verify_dhcpv6_ia_na(iface, pkt, NULL);
	verify_dhcpv6_ia_pd(iface, pkt, NULL, 0);

	/* Verify client state */
	zassert_equal(iface->config.dhcpv6.state, NET_DHCPV6_REQUESTING,
		      "Invalid state");

	/* Reply with Reply message */
	reply = test_dhcpv6_create_message(test_ctx.iface,
					   DHCPV6_MSG_TYPE_REPLY,
					   set_reply_options);
	zassert_not_null(reply, "Failed to create pkt");

	result = net_ipv6_input(reply);
	zassert_equal(result, NET_OK, "Message should've been processed");

	/* Verify client state */
	zassert_equal(iface->config.dhcpv6.state, NET_DHCPV6_BOUND,
		      "Invalid state");
	zassert_mem_equal(&test_ctx.iface->config.dhcpv6.addr,
			  &test_addr, sizeof(test_addr), "Invalid address");
	zassert_mem_equal(&test_ctx.iface->config.dhcpv6.prefix,
			  &test_prefix, sizeof(test_prefix), "Invalid prefix");
	zassert_equal(test_ctx.iface->config.dhcpv6.prefix_len,
		      test_prefix_len, "Invalid prefix len");

	k_sem_give(&test_ctx.exchange_complete_sem);
}

static void test_solicit_expect_solicit_send_advertise(struct net_if *iface,
						       struct net_pkt *pkt)
{
	struct net_pkt *reply;
	int result;

	/* Verify header */
	verify_dhcpv6_header(iface, pkt, DHCPV6_MSG_TYPE_SOLICIT);

	/* Verify options */
	verify_dhcpv6_clientid(iface, pkt);
	verify_dhcpv6_ia_na(iface, pkt, NULL);
	verify_dhcpv6_ia_pd(iface, pkt, NULL, 0);

	/* Verify client state */
	zassert_equal(iface->config.dhcpv6.state, NET_DHCPV6_SOLICITING,
		      "Invalid state");
	zassert_equal(iface->config.dhcpv6.server_preference, -1,
		      "Invalid initial preference");

	/* Update next expected packet handler */
	set_dhcpv6_test_fn(test_solicit_expect_request_send_reply);

	/* Reply with Advertise message */
	reply = test_dhcpv6_create_message(test_ctx.iface,
					   DHCPV6_MSG_TYPE_ADVERTISE,
					   set_advertise_options);
	zassert_not_null(reply, "Failed to create pkt");

	result = net_ipv6_input(reply);
	zassert_equal(result, NET_OK, "Message should've been processed");

	/* Verify client state */
	zassert_equal(iface->config.dhcpv6.state, NET_DHCPV6_SOLICITING,
		      "Invalid state");
	zassert_equal(iface->config.dhcpv6.server_preference, test_preference,
		      "Invalid initial preference");
	zassert_equal(test_serverid.length, iface->config.dhcpv6.serverid.length,
		      "Invalid Server ID length");
	zassert_mem_equal(&test_serverid.duid, &iface->config.dhcpv6.serverid.duid,
			  test_serverid.length, "Invalid Server ID value");
}

/* Verify that DHCPv6 client can handle standard exchange (Solicit/Request) */
ZTEST(dhcpv6_tests, test_solicit_exchange)
{
	struct net_dhcpv6_params params = {
		.request_addr = true,
		.request_prefix = true,
	};
	struct net_if_ipv6_prefix *prefix;
	struct net_if_addr *addr;
	int ret;

	test_ctx.reset_dhcpv6 = true;
	memset(&test_ctx.iface->config.dhcpv6, 0,
	       sizeof(test_ctx.iface->config.dhcpv6));

	set_dhcpv6_test_fn(test_solicit_expect_solicit_send_advertise);

	net_dhcpv6_start(test_ctx.iface, &params);

	ret = k_sem_take(&test_ctx.exchange_complete_sem, K_SECONDS(3));
	zassert_ok(ret, "Exchange not completed in required time");

	addr = net_if_ipv6_addr_lookup_by_iface(test_ctx.iface, &test_addr);
	prefix = net_if_ipv6_prefix_lookup(test_ctx.iface, &test_prefix,
					   test_prefix_len);
	zassert_not_null(addr, "Address not configured on the interface");
	zassert_not_null(prefix, "Prefix not configured on the interface");
}

static void expect_request_send_reply(struct net_if *iface, struct net_pkt *pkt)
{
	struct net_pkt *reply;
	int result;

	verify_dhcpv6_header(iface, pkt, DHCPV6_MSG_TYPE_REQUEST);
	set_dhcpv6_test_fn(NULL);

	/* Reply with Reply message */
	reply = test_dhcpv6_create_message(test_ctx.iface,
					   DHCPV6_MSG_TYPE_REPLY,
					   set_reply_options);
	zassert_not_null(reply, "Failed to create pkt");

	result = net_ipv6_input(reply);
	zassert_equal(result, NET_OK, "Message should've been processed");

	k_sem_give(&test_ctx.exchange_complete_sem);
}

static void expect_solicit_send_advertise(struct net_if *iface, struct net_pkt *pkt)
{
	struct net_pkt *reply;
	int result;

	verify_dhcpv6_header(iface, pkt, DHCPV6_MSG_TYPE_SOLICIT);
	set_dhcpv6_test_fn(expect_request_send_reply);

	/* Reply with Advertise message */
	reply = test_dhcpv6_create_message(test_ctx.iface,
					   DHCPV6_MSG_TYPE_ADVERTISE,
					   set_advertise_options);
	zassert_not_null(reply, "Failed to create pkt");

	result = net_ipv6_input(reply);
	zassert_equal(result, NET_OK, "Message should've been processed");
}

static void test_dhcpv6_start_and_enter_bound(struct net_dhcpv6_params *params)
{
	int ret;

	/* Set maximum preference to speed up the process. */
	test_preference = DHCPV6_MAX_SERVER_PREFERENCE;

	set_dhcpv6_test_fn(expect_solicit_send_advertise);
	net_dhcpv6_start(test_ctx.iface, params);

	ret = k_sem_take(&test_ctx.exchange_complete_sem, K_SECONDS(3));
	zassert_ok(ret, "Exchange not completed in required time");
	zassert_equal(test_ctx.iface->config.dhcpv6.state, NET_DHCPV6_BOUND,
		      "Invalid state");
}

static void test_confirm_expect_confirm_send_reply(struct net_if *iface,
						   struct net_pkt *pkt)
{
	struct net_pkt *reply;
	int result;

	/* Verify header */
	verify_dhcpv6_header(iface, pkt, DHCPV6_MSG_TYPE_CONFIRM);

	/* Verify options */
	verify_dhcpv6_clientid(iface, pkt);
	verify_dhcpv6_ia_na(iface, pkt, &test_addr);

	/* Verify client state */
	zassert_equal(iface->config.dhcpv6.state, NET_DHCPV6_CONFIRMING,
		      "Invalid state");

	set_dhcpv6_test_fn(NULL);

	/* Reply with Advertise message */
	reply = test_dhcpv6_create_message(test_ctx.iface,
					   DHCPV6_MSG_TYPE_REPLY,
					   set_reply_options);
	zassert_not_null(reply, "Failed to create pkt");

	result = net_ipv6_input(reply);
	zassert_equal(result, NET_OK, "Message should've been processed");

	/* Verify client state */
	zassert_equal(iface->config.dhcpv6.state, NET_DHCPV6_BOUND,
		      "Invalid state");
	zassert_equal(test_serverid.length, iface->config.dhcpv6.serverid.length,
		      "Invalid Server ID length");
	zassert_mem_equal(&test_serverid.duid, &iface->config.dhcpv6.serverid.duid,
			  test_serverid.length, "Invalid Server ID value");

	k_sem_give(&test_ctx.exchange_complete_sem);
}

/* Verify that DHCPv6 client starts with Confirm when interface goes down and
 * up again (no prefix).
 */
ZTEST(dhcpv6_tests, test_confirm_exchange_after_iface_down)
{
	struct net_dhcpv6_params params = {
		.request_addr = true,
		.request_prefix = false,
	};
	struct net_if_addr *addr;
	int ret;

	test_ctx.reset_dhcpv6 = true;
	memset(&test_ctx.iface->config.dhcpv6, 0,
	       sizeof(test_ctx.iface->config.dhcpv6));

	test_dhcpv6_start_and_enter_bound(&params);
	set_dhcpv6_test_fn(test_confirm_expect_confirm_send_reply);

	net_if_down(test_ctx.iface);
	net_if_up(test_ctx.iface);

	ret = k_sem_take(&test_ctx.exchange_complete_sem, K_SECONDS(3));
	zassert_ok(ret, "Exchange not completed in required time");

	addr = net_if_ipv6_addr_lookup_by_iface(test_ctx.iface, &test_addr);
	zassert_not_null(addr, "Address not configured on the interface");
}

static void test_rebind_expect_rebind_send_reply(struct net_if *iface,
						 struct net_pkt *pkt)
{
	struct net_pkt *reply;
	int result;

	/* Verify header */
	verify_dhcpv6_header(iface, pkt, DHCPV6_MSG_TYPE_REBIND);

	/* Verify options */
	verify_dhcpv6_clientid(iface, pkt);
	verify_dhcpv6_ia_na(iface, pkt, &test_addr);
	verify_dhcpv6_ia_pd(iface, pkt, &test_prefix, test_prefix_len);

	/* Verify client state */
	zassert_equal(iface->config.dhcpv6.state, NET_DHCPV6_REBINDING,
		      "Invalid state");

	set_dhcpv6_test_fn(NULL);

	/* Reply with Advertise message */
	reply = test_dhcpv6_create_message(test_ctx.iface,
					   DHCPV6_MSG_TYPE_REPLY,
					   set_reply_options);
	zassert_not_null(reply, "Failed to create pkt");

	result = net_ipv6_input(reply);
	zassert_equal(result, NET_OK, "Message should've been processed");

	/* Verify client state */
	zassert_equal(iface->config.dhcpv6.state, NET_DHCPV6_BOUND,
		      "Invalid state");
	zassert_equal(test_serverid.length, iface->config.dhcpv6.serverid.length,
		      "Invalid Server ID length");
	zassert_mem_equal(&test_serverid.duid, &iface->config.dhcpv6.serverid.duid,
			  test_serverid.length, "Invalid Server ID value");

	k_sem_give(&test_ctx.exchange_complete_sem);
}

/* Verify that DHCPv6 client starts with Rebind when interface goes down and
 * up again (w/ prefix).
 */
ZTEST(dhcpv6_tests, test_rebind_exchange_after_iface_down)
{
	struct net_dhcpv6_params params = {
		.request_addr = true,
		.request_prefix = true,
	};
	struct net_if_ipv6_prefix *prefix;
	struct net_if_addr *addr;
	int ret;

	test_ctx.reset_dhcpv6 = true;
	memset(&test_ctx.iface->config.dhcpv6, 0,
	       sizeof(test_ctx.iface->config.dhcpv6));

	test_dhcpv6_start_and_enter_bound(&params);
	set_dhcpv6_test_fn(test_rebind_expect_rebind_send_reply);

	net_if_down(test_ctx.iface);
	net_if_up(test_ctx.iface);

	ret = k_sem_take(&test_ctx.exchange_complete_sem, K_SECONDS(3));
	zassert_ok(ret, "Exchange not completed in required time");

	addr = net_if_ipv6_addr_lookup_by_iface(test_ctx.iface, &test_addr);
	prefix = net_if_ipv6_prefix_lookup(test_ctx.iface, &test_prefix,
					   test_prefix_len);
	zassert_not_null(addr, "Address not configured on the interface");
	zassert_not_null(prefix, "Prefix not configured on the interface");
}

static void test_renew_expect_renew_send_reply(struct net_if *iface,
					       struct net_pkt *pkt)
{
	struct net_pkt *reply;
	int result;

	/* Verify header */
	verify_dhcpv6_header(iface, pkt, DHCPV6_MSG_TYPE_RENEW);

	/* Verify options */
	verify_dhcpv6_clientid(iface, pkt);
	verify_dhcpv6_serverid(iface, pkt);
	verify_dhcpv6_ia_na(iface, pkt, &test_addr);
	verify_dhcpv6_ia_pd(iface, pkt, &test_prefix, test_prefix_len);

	/* Verify client state */
	zassert_equal(iface->config.dhcpv6.state, NET_DHCPV6_RENEWING,
		      "Invalid state");

	set_dhcpv6_test_fn(NULL);

	/* Reply with Advertise message */
	reply = test_dhcpv6_create_message(test_ctx.iface,
					   DHCPV6_MSG_TYPE_REPLY,
					   set_reply_options);
	zassert_not_null(reply, "Failed to create pkt");

	result = net_ipv6_input(reply);
	zassert_equal(result, NET_OK, "Message should've been processed");

	/* Verify client state */
	zassert_equal(iface->config.dhcpv6.state, NET_DHCPV6_BOUND,
		      "Invalid state");
	zassert_equal(test_serverid.length, iface->config.dhcpv6.serverid.length,
		      "Invalid Server ID length");
	zassert_mem_equal(&test_serverid.duid, &iface->config.dhcpv6.serverid.duid,
			  test_serverid.length, "Invalid Server ID value");

	k_sem_give(&test_ctx.exchange_complete_sem);
}

/* Verify that DHCPv6 client proceeds with Renew when T1 timeout expires. */
ZTEST(dhcpv6_tests, test_renew_exchange_after_t1)
{
	struct net_dhcpv6_params params = {
		.request_addr = true,
		.request_prefix = true,
	};
	struct net_if_ipv6_prefix *prefix;
	struct net_if_addr *addr;
	int ret;

	test_ctx.reset_dhcpv6 = true;
	memset(&test_ctx.iface->config.dhcpv6, 0,
	       sizeof(test_ctx.iface->config.dhcpv6));

	test_dhcpv6_start_and_enter_bound(&params);
	set_dhcpv6_test_fn(test_renew_expect_renew_send_reply);

	/* Simulate T1 timeout */
	test_ctx.iface->config.dhcpv6.t1 = k_uptime_get();
	test_ctx.iface->config.dhcpv6.timeout = test_ctx.iface->config.dhcpv6.t1;
	dhcpv6_reschedule();

	ret = k_sem_take(&test_ctx.exchange_complete_sem, K_SECONDS(3));
	zassert_ok(ret, "Exchange not completed in required time");

	addr = net_if_ipv6_addr_lookup_by_iface(test_ctx.iface, &test_addr);
	prefix = net_if_ipv6_prefix_lookup(test_ctx.iface, &test_prefix,
					   test_prefix_len);
	zassert_not_null(addr, "Address not configured on the interface");
	zassert_not_null(prefix, "Prefix not configured on the interface");
}

/* Verify that DHCPv6 client proceeds with Rebind when T2 timeout expires. */
ZTEST(dhcpv6_tests, test_rebind_exchange_after_t2)
{
	struct net_dhcpv6_params params = {
		.request_addr = true,
		.request_prefix = true,
	};
	struct net_if_ipv6_prefix *prefix;
	struct net_if_addr *addr;
	int ret;

	test_ctx.reset_dhcpv6 = true;
	memset(&test_ctx.iface->config.dhcpv6, 0,
	       sizeof(test_ctx.iface->config.dhcpv6));

	test_dhcpv6_start_and_enter_bound(&params);
	set_dhcpv6_test_fn(NULL);

	/* Simulate T1 timeout */
	test_ctx.iface->config.dhcpv6.t1 = k_uptime_get();
	test_ctx.iface->config.dhcpv6.timeout = test_ctx.iface->config.dhcpv6.t1;
	dhcpv6_reschedule();

	/* Give a state machine a chance to run, we ignore Renew message. */
	k_msleep(10);

	set_dhcpv6_test_fn(test_rebind_expect_rebind_send_reply);

	/* Simulate T2 timeout */
	test_ctx.iface->config.dhcpv6.t2 = k_uptime_get();
	test_ctx.iface->config.dhcpv6.timeout = test_ctx.iface->config.dhcpv6.t2;
	dhcpv6_reschedule();

	ret = k_sem_take(&test_ctx.exchange_complete_sem, K_SECONDS(3));
	zassert_ok(ret, "Exchange not completed in required time");

	addr = net_if_ipv6_addr_lookup_by_iface(test_ctx.iface, &test_addr);
	prefix = net_if_ipv6_prefix_lookup(test_ctx.iface, &test_prefix,
					   test_prefix_len);
	zassert_not_null(addr, "Address not configured on the interface");
	zassert_not_null(prefix, "Prefix not configured on the interface");
}

static void verify_information_request_message(struct net_if *iface,
					       struct net_pkt *pkt)
{
	struct dhcpv6_ia_na ia_na;
	struct dhcpv6_ia_pd ia_pd;

	/* Verify header */
	verify_dhcpv6_header(iface, pkt, DHCPV6_MSG_TYPE_INFORMATION_REQUEST);

	/* Verify options */
	verify_dhcpv6_clientid(iface, pkt);
	verify_dhcpv6_no_serverid(iface, pkt);
	verify_dhcpv6_elapsed_time(iface, pkt, 0, 10);
	verify_dhcpv6_oro(pkt, DHCPV6_OPTION_CODE_INF_MAX_RT);
	verify_dhcpv6_oro(pkt, DHCPV6_OPTION_CODE_INFORMATION_REFRESH_TIME);
	zassert_not_equal(dhcpv6_find_ia_na(pkt, &ia_na), 0,
			  "IA NA option should not be present");
	zassert_not_equal(dhcpv6_find_ia_pd(pkt, &ia_pd), 0,
			  "IA PD option should not be present");
}

/* Verify that outgoing DHCPv6 Information-request has a valid format and
 * includes all mandatory options.
 */
ZTEST(dhcpv6_tests, test_information_request_message_format)
{
	int ret;

	set_dhcpv6_test_fn(verify_information_request_message);

	ret = dhcpv6_send_information_request(test_ctx.iface);
	zassert_ok(ret, "dhcpv6_send_information_request failed");

	ret = k_sem_take(&test_ctx.tx_sem, K_SECONDS(1));
	zassert_ok(ret, "Packet not transmitted");
}

/* Information Refresh Time sent in the Reply, 0 means option not included. */
static uint32_t test_info_refresh_time;
static struct net_in6_addr test_dns_server = { { { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
						   0, 0, 0, 0, 0, 0, 0, 0x53 } } };

static void verify_dns_server_configured(void)
{
#if defined(CONFIG_NET_DHCPV6_OPTION_DNS_ADDRESS)
	struct dns_resolve_context *ctx = dns_resolve_get_default();

	for (int i = 0; i < ARRAY_SIZE(ctx->servers); i++) {
		struct net_sockaddr_in6 *addr = net_sin6(&ctx->servers[i].dns_server);

		if (ctx->servers[i].source == DNS_SOURCE_DHCPV6 &&
		    addr->sin6_family == NET_AF_INET6 &&
		    net_ipv6_addr_cmp(&addr->sin6_addr, &test_dns_server)) {
			return;
		}
	}

	zassert_unreachable("DNS server not configured");
#endif
}

static int set_info_reply_options(struct net_if *iface, struct net_pkt *pkt,
				  enum dhcpv6_msg_type msg_type)
{
	int ret;

	ret = dhcpv6_add_option_clientid(pkt, &iface->config.dhcpv6.clientid);
	if (ret < 0) {
		return ret;
	}

	ret = dhcpv6_add_option_serverid(pkt, &test_serverid);
	if (ret < 0) {
		return ret;
	}

	ret = dhcpv6_add_option_header(pkt, DHCPV6_OPTION_CODE_OPTION_DNS_SERVERS,
				       sizeof(test_dns_server));
	if (ret < 0) {
		return ret;
	}

	ret = net_pkt_write(pkt, &test_dns_server, sizeof(test_dns_server));
	if (ret < 0) {
		return ret;
	}

	if (test_info_refresh_time == 0) {
		return 0;
	}

	ret = dhcpv6_add_option_header(pkt, DHCPV6_OPTION_CODE_INFORMATION_REFRESH_TIME,
				       DHCPV6_OPTION_INFORMATION_REFRESH_TIME_SIZE);
	if (ret < 0) {
		return ret;
	}

	return net_pkt_write_be32(pkt, test_info_refresh_time);
}

static void expect_info_request_send_reply(struct net_if *iface,
					   struct net_pkt *pkt)
{
	struct net_pkt *reply;
	int result;

	verify_information_request_message(iface, pkt);
	zassert_equal(iface->config.dhcpv6.state, NET_DHCPV6_INFO_REQUESTING,
		      "Invalid state");

	set_dhcpv6_test_fn(NULL);

	reply = test_dhcpv6_create_message(test_ctx.iface,
					   DHCPV6_MSG_TYPE_REPLY,
					   set_info_reply_options);
	zassert_not_null(reply, "Failed to create pkt");

	result = net_ipv6_input(reply);
	zassert_equal(result, NET_OK, "Message should've been processed");

	zassert_equal(iface->config.dhcpv6.state, NET_DHCPV6_BOUND,
		      "Invalid state");

	k_sem_give(&test_ctx.exchange_complete_sem);
}

/* Verify that DHCPv6 client in stateless mode performs Information-request
 * exchange, and repeats it when the information refresh time expires.
 */
ZTEST(dhcpv6_tests, test_info_request_exchange)
{
	struct net_dhcpv6_params params = {
		.request_addr = false,
		.request_prefix = false,
	};
	int64_t start;
	int ret;

	test_ctx.reset_dhcpv6 = true;
	memset(&test_ctx.iface->config.dhcpv6, 0,
	       sizeof(test_ctx.iface->config.dhcpv6));
	test_info_refresh_time = 3600;

	start = k_uptime_get();
	set_dhcpv6_test_fn(expect_info_request_send_reply);
	net_dhcpv6_start(test_ctx.iface, &params);

	ret = k_sem_take(&test_ctx.exchange_complete_sem, K_SECONDS(3));
	zassert_ok(ret, "Exchange not completed in required time");
	zassert_between_inclusive(test_ctx.iface->config.dhcpv6.t1,
				  start + 3600 * MSEC_PER_SEC,
				  k_uptime_get() + 3600 * MSEC_PER_SEC,
				  "Invalid refresh deadline");
	zassert_is_null(net_if_ipv6_addr_lookup_by_iface(test_ctx.iface, &test_addr),
			"No address should be configured");
	verify_dns_server_configured();

	/* Simulate information refresh time expiry. */
	set_dhcpv6_test_fn(expect_info_request_send_reply);
	test_ctx.iface->config.dhcpv6.t1 = k_uptime_get();
	test_ctx.iface->config.dhcpv6.timeout = test_ctx.iface->config.dhcpv6.t1;
	dhcpv6_reschedule();

	ret = k_sem_take(&test_ctx.exchange_complete_sem, K_SECONDS(3));
	zassert_ok(ret, "Refresh exchange not completed in required time");
}

static void test_info_reply_refresh_time(uint32_t refresh_time, uint64_t expected)
{
	struct net_pkt *pkt;
	enum net_verdict result;
	int64_t before = k_uptime_get();

	test_info_refresh_time = refresh_time;
	test_ctx.iface->config.dhcpv6.state = NET_DHCPV6_INFO_REQUESTING;

	pkt = test_dhcpv6_create_message(test_ctx.iface, DHCPV6_MSG_TYPE_REPLY,
					 set_info_reply_options);
	zassert_not_null(pkt, "Failed to create pkt");

	result = net_ipv6_input(pkt);
	zassert_equal(result, NET_OK, "Message should've been processed");
	zassert_equal(test_ctx.iface->config.dhcpv6.state, NET_DHCPV6_BOUND,
		      "Invalid state");

	if (expected == UINT64_MAX) {
		zassert_equal(test_ctx.iface->config.dhcpv6.t1, UINT64_MAX,
			      "Refresh should never happen");
	} else {
		zassert_between_inclusive(test_ctx.iface->config.dhcpv6.t1,
					  before + expected * MSEC_PER_SEC,
					  k_uptime_get() + expected * MSEC_PER_SEC,
					  "Invalid refresh deadline (%u)", refresh_time);
	}

	net_pkt_unref(pkt);
}

/* Verify Information Refresh Time handling (RFC 8415, ch. 21.23). */
ZTEST(dhcpv6_tests, test_info_reply_refresh_time)
{
	test_ctx.iface->config.dhcpv6.params = (struct net_dhcpv6_params){ 0 };

	/* Option absent, default is used. */
	test_info_reply_refresh_time(0, DHCPV6_IRT_DEFAULT);
	/* Too small, minimum is used. */
	test_info_reply_refresh_time(10, DHCPV6_IRT_MINIMUM);
	test_info_reply_refresh_time(7200, 7200);
	test_info_reply_refresh_time(DHCPV6_INFINITY, UINT64_MAX);
}

static void test_send_ra(struct net_if *iface, uint8_t flags)
{
	struct net_icmpv6_ra_hdr ra_hdr = { .flags = flags };
	struct net_in6_addr *local_addr;
	struct net_in6_addr peer_addr;
	struct net_pkt *pkt;
	int ret;

	local_addr = net_if_ipv6_get_ll(iface, NET_ADDR_ANY_STATE);
	zassert_not_null(local_addr, "No link-local address");

	memcpy(&peer_addr, local_addr, sizeof(peer_addr));
	peer_addr.s6_addr[15] = ~peer_addr.s6_addr[15];

	pkt = net_pkt_alloc_with_buffer(iface, sizeof(struct net_icmp_hdr) + sizeof(ra_hdr),
					NET_AF_INET6, NET_IPPROTO_ICMPV6, K_FOREVER);
	zassert_not_null(pkt, "Failed to allocate pkt");

	net_pkt_set_ipv6_hop_limit(pkt, NET_IPV6_ND_HOP_LIMIT);

	ret = net_ipv6_create(pkt, &peer_addr, local_addr);
	zassert_ok(ret, "Failed to create IPv6 header");
	ret = net_icmpv6_create(pkt, NET_ICMPV6_RA, 0);
	zassert_ok(ret, "Failed to create ICMPv6 header");
	ret = net_pkt_write(pkt, &ra_hdr, sizeof(ra_hdr));
	zassert_ok(ret, "Failed to write RA header");

	net_pkt_cursor_init(pkt);
	net_ipv6_finalize(pkt, NET_IPPROTO_ICMPV6);
	net_pkt_cursor_init(pkt);

	ret = net_recv_data(iface, pkt);
	zassert_ok(ret, "Failed to receive RA");

	/* Let the RX path process the RA. */
	k_msleep(50);
}

static void test_ra_before(void)
{
	test_ctx.reset_dhcpv6 = true;
	memset(&test_ctx.iface->config.dhcpv6, 0,
	       sizeof(test_ctx.iface->config.dhcpv6));
	test_info_refresh_time = 3600;
}

/* Verify that Router Advertisement with O flag starts stateless DHCPv6. */
ZTEST(dhcpv6_tests, test_ra_other_flag_starts_stateless)
{
	int ret;

	Z_TEST_SKIP_IFNDEF(CONFIG_NET_DHCPV6_START_ON_RA);
	test_ra_before();

	set_dhcpv6_test_fn(expect_info_request_send_reply);
	test_send_ra(test_ctx.iface, NET_ICMPV6_RA_HDR_FLAG_OTHER);

	ret = k_sem_take(&test_ctx.exchange_complete_sem, K_SECONDS(3));
	zassert_ok(ret, "Exchange not completed in required time");
	zassert_true(test_ctx.iface->config.dhcpv6.auto_started,
		     "Client should be marked as started by RA");
}

/* Verify that Router Advertisement with M flag starts stateful DHCPv6. */
ZTEST(dhcpv6_tests, test_ra_managed_flag_starts_stateful)
{
	int ret;

	Z_TEST_SKIP_IFNDEF(CONFIG_NET_DHCPV6_START_ON_RA);
	test_ra_before();

	/* Set maximum preference to speed up the process. */
	test_preference = DHCPV6_MAX_SERVER_PREFERENCE;

	set_dhcpv6_test_fn(expect_solicit_send_advertise);
	test_send_ra(test_ctx.iface,
		     NET_ICMPV6_RA_HDR_FLAG_MANAGED | NET_ICMPV6_RA_HDR_FLAG_OTHER);

	ret = k_sem_take(&test_ctx.exchange_complete_sem, K_SECONDS(3));
	zassert_ok(ret, "Exchange not completed in required time");
	zassert_equal(test_ctx.iface->config.dhcpv6.state, NET_DHCPV6_BOUND,
		      "Invalid state");
	zassert_true(test_ctx.iface->config.dhcpv6.params.request_addr,
		     "Address should be requested");
	zassert_false(test_ctx.iface->config.dhcpv6.params.request_prefix,
		      "Prefix should not be requested");
	zassert_not_null(net_if_ipv6_addr_lookup_by_iface(test_ctx.iface, &test_addr),
			 "Address not configured on the interface");
}

/* Verify that Router Advertisement without M/O flags does not start DHCPv6. */
ZTEST(dhcpv6_tests, test_ra_no_flags_no_start)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_NET_DHCPV6_START_ON_RA);
	test_ra_before();

	test_send_ra(test_ctx.iface, 0);

	zassert_equal(test_ctx.iface->config.dhcpv6.state, NET_DHCPV6_DISABLED,
		      "DHCPv6 should not be started");
}

/* Verify that explicit stop disables the automatic start. */
ZTEST(dhcpv6_tests, test_ra_no_start_after_stop)
{
	struct net_dhcpv6_params params = { 0 };
	int ret;

	Z_TEST_SKIP_IFNDEF(CONFIG_NET_DHCPV6_START_ON_RA);
	test_ra_before();

	net_dhcpv6_stop(test_ctx.iface);
	test_send_ra(test_ctx.iface, NET_ICMPV6_RA_HDR_FLAG_OTHER);

	zassert_equal(test_ctx.iface->config.dhcpv6.state, NET_DHCPV6_DISABLED,
		      "DHCPv6 should not be started after explicit stop");

	/* Explicit start enables it again. */
	set_dhcpv6_test_fn(expect_info_request_send_reply);
	net_dhcpv6_start(test_ctx.iface, &params);

	ret = k_sem_take(&test_ctx.exchange_complete_sem, K_SECONDS(3));
	zassert_ok(ret, "Exchange not completed in required time");
	zassert_false(test_ctx.iface->config.dhcpv6.auto_started,
		      "Client should not be marked as started by RA");
}

/* Verify that explicit start replaces the client started by RA. */
ZTEST(dhcpv6_tests, test_ra_explicit_start_replaces_auto)
{
	struct net_dhcpv6_params params = {
		.request_addr = true,
		.request_prefix = true,
	};

	Z_TEST_SKIP_IFNDEF(CONFIG_NET_DHCPV6_START_ON_RA);
	test_ra_before();

	test_send_ra(test_ctx.iface, NET_ICMPV6_RA_HDR_FLAG_OTHER);
	zassert_not_equal(test_ctx.iface->config.dhcpv6.state, NET_DHCPV6_DISABLED,
			  "DHCPv6 should be started by RA");

	test_dhcpv6_start_and_enter_bound(&params);
	zassert_false(test_ctx.iface->config.dhcpv6.auto_started,
		      "Client should not be marked as started by RA");
	zassert_not_null(net_if_ipv6_addr_lookup_by_iface(test_ctx.iface, &test_addr),
			 "Address not configured on the interface");
}

/* Verify that the client started by RA stops when the interface goes down,
 * and is started again by the next RA.
 */
ZTEST(dhcpv6_tests, test_ra_auto_stops_on_iface_down)
{
	int ret;

	Z_TEST_SKIP_IFNDEF(CONFIG_NET_DHCPV6_START_ON_RA);
	test_ra_before();

	test_send_ra(test_ctx.iface, NET_ICMPV6_RA_HDR_FLAG_OTHER);
	zassert_not_equal(test_ctx.iface->config.dhcpv6.state, NET_DHCPV6_DISABLED,
			  "DHCPv6 should be started by RA");

	ret = net_if_down(test_ctx.iface);
	zassert_ok(ret, "Failed to bring iface down");
	k_msleep(50);

	zassert_equal(test_ctx.iface->config.dhcpv6.state, NET_DHCPV6_DISABLED,
		      "DHCPv6 should be stopped");

	ret = net_if_up(test_ctx.iface);
	zassert_ok(ret, "Failed to bring iface up");
	k_msleep(50);

	zassert_equal(test_ctx.iface->config.dhcpv6.state, NET_DHCPV6_DISABLED,
		      "DHCPv6 should wait for RA");

	set_dhcpv6_test_fn(expect_info_request_send_reply);
	test_send_ra(test_ctx.iface, NET_ICMPV6_RA_HDR_FLAG_OTHER);

	ret = k_sem_take(&test_ctx.exchange_complete_sem, K_SECONDS(3));
	zassert_ok(ret, "Exchange not completed in required time");
}

ZTEST_SUITE(dhcpv6_tests, NULL, dhcpv6_tests_setup, dhcpv6_tests_before,
	    dhcpv6_tests_after, NULL);
