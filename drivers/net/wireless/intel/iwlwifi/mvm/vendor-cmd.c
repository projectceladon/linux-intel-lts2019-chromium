/******************************************************************************
 *
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * GPL LICENSE SUMMARY
 *
 * Copyright(c) 2012 - 2014 Intel Corporation. All rights reserved.
 * Copyright(c) 2013 - 2015 Intel Mobile Communications GmbH
 * Copyright(c) 2016 - 2017 Intel Deutschland GmbH
 * Copyright(c) 2018        Intel Corporation
 * Copyright (C) 2019 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * The full GNU General Public License is included in this distribution
 * in the file called COPYING.
 *
 * Contact Information:
 *  Intel Linux Wireless <linuxwifi@intel.com>
 * Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497
 *
 * BSD LICENSE
 *
 * Copyright(c) 2012 - 2014 Intel Corporation. All rights reserved.
 * Copyright(c) 2013 - 2015 Intel Mobile Communications GmbH
 * Copyright(c) 2016 - 2017 Intel Deutschland GmbH
 * Copyright(c) 2018        Intel Corporation
 * Copyright (C) 2019 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name Intel Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *****************************************************************************/
#include <linux/etherdevice.h>
#include <linux/utsname.h>
#include <linux/version.h>
#include <net/mac80211.h>
#include <net/netlink.h>
#include "mvm.h"

#include "iwl-vendor-cmd.h"

#include "iwl-io.h"
#include "iwl-prph.h"

static const struct nla_policy
iwl_mvm_vendor_attr_policy[NUM_IWL_MVM_VENDOR_ATTR] = {
	[IWL_MVM_VENDOR_ATTR_FW_VER] = { .type = NLA_STRING, .len = 50 },
	[IWL_MVM_VENDOR_ATTR_DRV_VER] = { .type = NLA_STRING, .len = 50 },
};

static int iwl_mvm_vendor_get_fw_version(struct wiphy *wiphy,
				   struct wireless_dev *wdev,
				   const void *data, int data_len)
{
	int err = 0;
	struct sk_buff *skb;
	struct ieee80211_hw *hw = wiphy_to_ieee80211_hw(wiphy);
	struct iwl_mvm *mvm = IWL_MAC80211_GET_MVM(hw);
	const struct iwl_fw *fw = mvm->fw;

	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, sizeof(fw->fw_version));
        if (!skb)
                return -ENOMEM;
        if (nla_put_string(skb, IWL_MVM_VENDOR_ATTR_FW_VER, fw->fw_version)) {
                kfree_skb(skb);
                return -ENOBUFS;
	}

	return cfg80211_vendor_cmd_reply(skb);
}

static int iwl_mvm_vendor_get_drv_version(struct wiphy *wiphy,
				   struct wireless_dev *wdev,
				   const void *data, int data_len)
{
	int err = 0;
	struct sk_buff *skb;
	struct ieee80211_hw *hw = wiphy_to_ieee80211_hw(wiphy);
	struct iwl_mvm *mvm = IWL_MAC80211_GET_MVM(hw);
	const struct iwl_fw *fw = mvm->fw;

	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, sizeof(utsname()->release));
        if (!skb)
                return -ENOMEM;
        if (nla_put_string(skb, IWL_MVM_VENDOR_ATTR_DRV_VER, utsname()->release)) {
                kfree_skb(skb);
                return -ENOBUFS;
	}

	return cfg80211_vendor_cmd_reply(skb);
}

static const struct wiphy_vendor_command iwl_mvm_vendor_commands[] = {
	{
		.info = {
			.vendor_id = INTEL_OUI,
			.subcmd = IWL_MVM_VENDOR_CMD_GET_FW_VERSION,
		},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV |
			 WIPHY_VENDOR_CMD_NEED_RUNNING,
		.doit = iwl_mvm_vendor_get_fw_version,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,3,0)
		.policy = iwl_mvm_vendor_attr_policy,
		.maxattr = MAX_IWL_MVM_VENDOR_ATTR,
#endif
	},
	{
		.info = {
			.vendor_id = INTEL_OUI,
			.subcmd = IWL_MVM_VENDOR_CMD_GET_DRV_VERSION,
		},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV |
			 WIPHY_VENDOR_CMD_NEED_RUNNING,
		.doit = iwl_mvm_vendor_get_drv_version,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,3,0)
		.policy = iwl_mvm_vendor_attr_policy,
		.maxattr = MAX_IWL_MVM_VENDOR_ATTR,
#endif
	},
};

void iwl_mvm_set_wiphy_vendor_commands(struct wiphy *wiphy)
{
	wiphy->vendor_commands = iwl_mvm_vendor_commands;
	wiphy->n_vendor_commands = ARRAY_SIZE(iwl_mvm_vendor_commands);
	wiphy->vendor_events = NULL;;
	wiphy->n_vendor_events = 0;
}
