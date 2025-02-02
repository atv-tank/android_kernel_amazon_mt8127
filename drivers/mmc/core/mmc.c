/*
 *  linux/drivers/mmc/core/mmc.c
 *
 *  Copyright (C) 2003-2004 Russell King, All Rights Reserved.
 *  Copyright (C) 2005-2007 Pierre Ossman, All Rights Reserved.
 *  MMCv4 support Copyright (C) 2006 Philip Langdale, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/err.h>
#include <linux/slab.h>
#include <linux/stat.h>

#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>

#include "core.h"
#include "bus.h"
#include "mmc_ops.h"
#include "sd_ops.h"

#if (defined(CONFIG_AMAZON_METRICS_LOG) && defined(ENABLE_SAMSUNG_EMMC_METRICS))
#include <linux/metricslog.h>
#define LMK_METRIC_TAG "kernel"
#define METRICS_samsung_data_LEN 128
#endif /* CONFIG_AMAZON_METRICS_LOG */

#ifdef CONFIG_AMAZON_METRICS_LOG
#include <linux/metricslog.h>
#include <linux/vmalloc.h>
#define LMK_METRIC_TAG "kernel"
#define METRICS_LIFETIME_DATA_LEN 128
#define VITALS_LIFETIME_DATA_LEN 256
#define VITALS_DOMAIN "Kernel"
#endif

static const unsigned int tran_exp[] = {
	10000,		100000,		1000000,	10000000,
	0,		0,		0,		0
};

static const unsigned char tran_mant[] = {
	0,	10,	12,	13,	15,	20,	25,	30,
	35,	40,	45,	50,	55,	60,	70,	80,
};

static const unsigned int tacc_exp[] = {
	1,	10,	100,	1000,	10000,	100000,	1000000, 10000000,
};

static const unsigned int tacc_mant[] = {
	0,	10,	12,	13,	15,	20,	25,	30,
	35,	40,	45,	50,	55,	60,	70,	80,
};

#define UNSTUFF_BITS(resp,start,size)					\
	({								\
		const int __size = size;				\
		const u32 __mask = (__size < 32 ? 1 << __size : 0) - 1;	\
		const int __off = 3 - ((start) / 32);			\
		const int __shft = (start) & 31;			\
		u32 __res;						\
									\
		__res = resp[__off] >> __shft;				\
		if (__size + __shft > 32)				\
			__res |= resp[__off-1] << ((32 - __shft) % 32);	\
		__res & __mask;						\
	})

/*
 * Given the decoded CSD structure, decode the raw CID to our CID structure.
 */
static int mmc_decode_cid(struct mmc_card *card)
{
	u32 *resp = card->raw_cid;

	/*
	 * The selection of the format here is based upon published
	 * specs from sandisk and from what people have reported.
	 */
	switch (card->csd.mmca_vsn) {
	case 0: /* MMC v1.0 - v1.2 */
	case 1: /* MMC v1.4 */
		card->cid.manfid	= UNSTUFF_BITS(resp, 104, 24);
		card->cid.prod_name[0]	= UNSTUFF_BITS(resp, 96, 8);
		card->cid.prod_name[1]	= UNSTUFF_BITS(resp, 88, 8);
		card->cid.prod_name[2]	= UNSTUFF_BITS(resp, 80, 8);
		card->cid.prod_name[3]	= UNSTUFF_BITS(resp, 72, 8);
		card->cid.prod_name[4]	= UNSTUFF_BITS(resp, 64, 8);
		card->cid.prod_name[5]	= UNSTUFF_BITS(resp, 56, 8);
		card->cid.prod_name[6]	= UNSTUFF_BITS(resp, 48, 8);
		card->cid.hwrev		= UNSTUFF_BITS(resp, 44, 4);
		card->cid.fwrev		= UNSTUFF_BITS(resp, 40, 4);
		card->cid.serial	= UNSTUFF_BITS(resp, 16, 24);
		card->cid.month		= UNSTUFF_BITS(resp, 12, 4);
		card->cid.year		= UNSTUFF_BITS(resp, 8, 4) + 1997;
		break;

	case 2: /* MMC v2.0 - v2.2 */
	case 3: /* MMC v3.1 - v3.3 */
	case 4: /* MMC v4 */
		card->cid.manfid	= UNSTUFF_BITS(resp, 120, 8);
		card->cid.oemid		= UNSTUFF_BITS(resp, 104, 16);
		card->cid.prod_name[0]	= UNSTUFF_BITS(resp, 96, 8);
		card->cid.prod_name[1]	= UNSTUFF_BITS(resp, 88, 8);
		card->cid.prod_name[2]	= UNSTUFF_BITS(resp, 80, 8);
		card->cid.prod_name[3]	= UNSTUFF_BITS(resp, 72, 8);
		card->cid.prod_name[4]	= UNSTUFF_BITS(resp, 64, 8);
		card->cid.prod_name[5]	= UNSTUFF_BITS(resp, 56, 8);
		card->cid.prv		= UNSTUFF_BITS(resp, 48, 8);
		card->cid.serial	= UNSTUFF_BITS(resp, 16, 32);
		card->cid.month		= UNSTUFF_BITS(resp, 12, 4);
		card->cid.year		= UNSTUFF_BITS(resp, 8, 4) + 1997;
		break;

	default:
		pr_err("%s: card has unknown MMCA version %d\n",
			mmc_hostname(card->host), card->csd.mmca_vsn);
		return -EINVAL;
	}

	return 0;
}

static void mmc_set_erase_size(struct mmc_card *card)
{
	if (card->ext_csd.erase_group_def & 1)
		card->erase_size = card->ext_csd.hc_erase_size;
	else
		card->erase_size = card->csd.erase_size;

	mmc_init_erase(card);
}

/*
 * Given a 128-bit response, decode to our card CSD structure.
 */
static int mmc_decode_csd(struct mmc_card *card)
{
	struct mmc_csd *csd = &card->csd;
	unsigned int e, m, a, b;
	u32 *resp = card->raw_csd;

	/*
	 * We only understand CSD structure v1.1 and v1.2.
	 * v1.2 has extra information in bits 15, 11 and 10.
	 * We also support eMMC v4.4 & v4.41.
	 */
	csd->structure = UNSTUFF_BITS(resp, 126, 2);
	if (csd->structure == 0) {
		pr_err("%s: unrecognised CSD structure version %d\n",
			mmc_hostname(card->host), csd->structure);
		return -EINVAL;
	}

	csd->mmca_vsn	 = UNSTUFF_BITS(resp, 122, 4);
	m = UNSTUFF_BITS(resp, 115, 4);
	e = UNSTUFF_BITS(resp, 112, 3);
	csd->tacc_ns	 = (tacc_exp[e] * tacc_mant[m] + 9) / 10;
	csd->tacc_clks	 = UNSTUFF_BITS(resp, 104, 8) * 100;

	m = UNSTUFF_BITS(resp, 99, 4);
	e = UNSTUFF_BITS(resp, 96, 3);
	csd->max_dtr	  = tran_exp[e] * tran_mant[m];
	csd->cmdclass	  = UNSTUFF_BITS(resp, 84, 12);

	e = UNSTUFF_BITS(resp, 47, 3);
	m = UNSTUFF_BITS(resp, 62, 12);
	csd->capacity	  = (1 + m) << (e + 2);

	csd->read_blkbits = UNSTUFF_BITS(resp, 80, 4);
	csd->read_partial = UNSTUFF_BITS(resp, 79, 1);
	csd->write_misalign = UNSTUFF_BITS(resp, 78, 1);
	csd->read_misalign = UNSTUFF_BITS(resp, 77, 1);
	csd->r2w_factor = UNSTUFF_BITS(resp, 26, 3);
	csd->write_blkbits = UNSTUFF_BITS(resp, 22, 4);
	csd->write_partial = UNSTUFF_BITS(resp, 21, 1);

	if (csd->write_blkbits >= 9) {
		a = UNSTUFF_BITS(resp, 42, 5);
		b = UNSTUFF_BITS(resp, 37, 5);
		csd->erase_size = (a + 1) * (b + 1);
		csd->erase_size <<= csd->write_blkbits - 9;
	}

	return 0;
}

/*
 * Read extended CSD.
 */
static int mmc_get_ext_csd(struct mmc_card *card, u8 **new_ext_csd)
{
	int err;
	u8 *ext_csd;

	BUG_ON(!card);
	BUG_ON(!new_ext_csd);

	*new_ext_csd = NULL;

	if (card->csd.mmca_vsn < CSD_SPEC_VER_4)
		return 0;

	/*
	 * As the ext_csd is so large and mostly unused, we don't store the
	 * raw block in mmc_card.
	 */
	ext_csd = kmalloc(512, GFP_KERNEL);
	if (!ext_csd) {
		pr_err("%s: could not allocate a buffer to "
			"receive the ext_csd.\n", mmc_hostname(card->host));
		return -ENOMEM;
	}

	err = mmc_send_ext_csd(card, ext_csd);
	if (err) {
		kfree(ext_csd);
		*new_ext_csd = NULL;

		/* If the host or the card can't do the switch,
		 * fail more gracefully. */
		if ((err != -EINVAL)
		 && (err != -ENOSYS)
		 && (err != -EFAULT))
			return err;

		/*
		 * High capacity cards should have this "magic" size
		 * stored in their CSD.
		 */
		if (card->csd.capacity == (4096 * 512)) {
			pr_err("%s: unable to read EXT_CSD "
				"on a possible high capacity card. "
				"Card will be ignored.\n",
				mmc_hostname(card->host));
		} else {
			pr_warning("%s: unable to read "
				"EXT_CSD, performance might "
				"suffer.\n",
				mmc_hostname(card->host));
			err = 0;
		}
	} else
		*new_ext_csd = ext_csd;

	return err;
}

static void mmc_select_card_type(struct mmc_card *card)
{
	struct mmc_host *host = card->host;
	u8 card_type = card->ext_csd.raw_card_type & EXT_CSD_CARD_TYPE_MASK;
	u32 caps = host->caps, caps2 = host->caps2;
	unsigned int hs_max_dtr = 0;

	if (card_type & EXT_CSD_CARD_TYPE_26)
		hs_max_dtr = MMC_HIGH_26_MAX_DTR;

	if (caps & MMC_CAP_MMC_HIGHSPEED &&
			card_type & EXT_CSD_CARD_TYPE_52)
		hs_max_dtr = MMC_HIGH_52_MAX_DTR;

	if ((caps & MMC_CAP_1_8V_DDR &&
			card_type & EXT_CSD_CARD_TYPE_DDR_1_8V) ||
	    (caps & MMC_CAP_1_2V_DDR &&
			card_type & EXT_CSD_CARD_TYPE_DDR_1_2V))
		hs_max_dtr = MMC_HIGH_DDR_MAX_DTR;

	if ((caps2 & MMC_CAP2_HS200_1_8V_SDR &&
			card_type & EXT_CSD_CARD_TYPE_HS200_1_8V) ||
	    (caps2 & MMC_CAP2_HS200_1_2V_SDR &&
			card_type & EXT_CSD_CARD_TYPE_HS200_1_2V))
		hs_max_dtr = MMC_HS200_MAX_DTR;
#ifdef CONFIG_EMMC_50_FEATURE
	if ((caps2 & MMC_CAP2_HS400_1_8V_DDR &&
			card_type & EXT_CSD_CARD_TYPE_HS400_1_8V) ||
	    (caps2 & MMC_CAP2_HS400_1_2V_DDR &&
			card_type & EXT_CSD_CARD_TYPE_HS400_1_2V))
		hs_max_dtr = MMC_HS400_MAX_DTR;
#endif
	card->ext_csd.hs_max_dtr = hs_max_dtr;
	card->ext_csd.card_type = card_type;
}

/*
 * Decode extended CSD.
 */
#define VENDOR_SAMSUNG  (0x15)
static int mmc_read_ext_csd(struct mmc_card *card, u8 *ext_csd)
{
	int err = 0, idx;
	unsigned int part_size;
	u8 hc_erase_grp_sz = 0, hc_wp_grp_sz = 0;

	BUG_ON(!card);

	if (!ext_csd)
		return 0;

	/* Version is coded in the CSD_STRUCTURE byte in the EXT_CSD register */
	card->ext_csd.raw_ext_csd_structure = ext_csd[EXT_CSD_STRUCTURE];
	if (card->csd.structure == 3) {
		if (card->ext_csd.raw_ext_csd_structure > 2) {
			pr_err("%s: unrecognised EXT_CSD structure "
				"version %d\n", mmc_hostname(card->host),
					card->ext_csd.raw_ext_csd_structure);
			err = -EINVAL;
			goto out;
		}
	}

	card->ext_csd.rev = ext_csd[EXT_CSD_REV];
	if (card->ext_csd.rev > 8) {
		pr_err("%s: unrecognised EXT_CSD revision %d\n",
			mmc_hostname(card->host), card->ext_csd.rev);
		err = -EINVAL;
		goto out;
	}
	card->ext_csd.raw_firmware_version = ext_csd[EXT_CSD_FIRMWARE_VERSION + 0] <<0 |
					ext_csd[EXT_CSD_FIRMWARE_VERSION + 1] <<8 |
					ext_csd[EXT_CSD_FIRMWARE_VERSION + 2] <<16 |
					ext_csd[EXT_CSD_FIRMWARE_VERSION + 3] <<24 |
					ext_csd[EXT_CSD_FIRMWARE_VERSION + 4] <<32 |
					ext_csd[EXT_CSD_FIRMWARE_VERSION + 5] <<40 |
					ext_csd[EXT_CSD_FIRMWARE_VERSION + 6] <<48 |
					ext_csd[EXT_CSD_FIRMWARE_VERSION + 7] <<56;
	card->ext_csd.raw_sectors[0] = ext_csd[EXT_CSD_SEC_CNT + 0];
	card->ext_csd.raw_sectors[1] = ext_csd[EXT_CSD_SEC_CNT + 1];
	card->ext_csd.raw_sectors[2] = ext_csd[EXT_CSD_SEC_CNT + 2];
	card->ext_csd.raw_sectors[3] = ext_csd[EXT_CSD_SEC_CNT + 3];
	if (card->ext_csd.rev >= 2) {
		card->ext_csd.sectors =
			ext_csd[EXT_CSD_SEC_CNT + 0] << 0 |
			ext_csd[EXT_CSD_SEC_CNT + 1] << 8 |
			ext_csd[EXT_CSD_SEC_CNT + 2] << 16 |
			ext_csd[EXT_CSD_SEC_CNT + 3] << 24;

		/* Cards with density > 2GiB are sector addressed */
		if (card->ext_csd.sectors > (2u * 1024 * 1024 * 1024) / 512)
			mmc_card_set_blockaddr(card);
	}

	card->ext_csd.raw_card_type = ext_csd[EXT_CSD_CARD_TYPE];
	mmc_select_card_type(card);

	card->ext_csd.raw_s_a_timeout = ext_csd[EXT_CSD_S_A_TIMEOUT];
	card->ext_csd.raw_erase_timeout_mult =
		ext_csd[EXT_CSD_ERASE_TIMEOUT_MULT];
	card->ext_csd.raw_hc_erase_grp_size =
		ext_csd[EXT_CSD_HC_ERASE_GRP_SIZE];
	card->ext_csd.raw_dev_lifetime_est_a =
		ext_csd[EXT_CSD_DEV_LIFETIME_EST_A];
	card->ext_csd.raw_dev_lifetime_est_b =
		ext_csd[EXT_CSD_DEV_LIFETIME_EST_B];
	if (card->ext_csd.rev >= 3) {
		u8 sa_shift = ext_csd[EXT_CSD_S_A_TIMEOUT];
		card->ext_csd.part_config = ext_csd[EXT_CSD_PART_CONFIG];

		/* EXT_CSD value is in units of 10ms, but we store in ms */
		card->ext_csd.part_time = 10 * ext_csd[EXT_CSD_PART_SWITCH_TIME];

		/* Sleep / awake timeout in 100ns units */
		if (sa_shift > 0 && sa_shift <= 0x17)
			card->ext_csd.sa_timeout =
					1 << ext_csd[EXT_CSD_S_A_TIMEOUT];
		card->ext_csd.erase_group_def =
			ext_csd[EXT_CSD_ERASE_GROUP_DEF];
		card->ext_csd.hc_erase_timeout = 300 *
			ext_csd[EXT_CSD_ERASE_TIMEOUT_MULT];
		card->ext_csd.hc_erase_size =
			ext_csd[EXT_CSD_HC_ERASE_GRP_SIZE] << 10;

		card->ext_csd.rel_sectors = ext_csd[EXT_CSD_REL_WR_SEC_C];

		/*
		 * There are two boot regions of equal size, defined in
		 * multiples of 128K.
		 */
		if (ext_csd[EXT_CSD_BOOT_MULT] && mmc_boot_partition_access(card->host)) {
			for (idx = 0; idx < MMC_NUM_BOOT_PARTITION; idx++) {
				part_size = ext_csd[EXT_CSD_BOOT_MULT] << 17;
				mmc_part_add(card, part_size,
					EXT_CSD_PART_CONFIG_ACC_BOOT0 + idx,
					"boot%d", idx, true,
					MMC_BLK_DATA_AREA_BOOT);
			}
		}
	}

	card->ext_csd.raw_hc_erase_gap_size =
		ext_csd[EXT_CSD_HC_WP_GRP_SIZE];
	card->ext_csd.raw_sec_trim_mult =
		ext_csd[EXT_CSD_SEC_TRIM_MULT];
	card->ext_csd.raw_sec_erase_mult =
		ext_csd[EXT_CSD_SEC_ERASE_MULT];
	card->ext_csd.raw_sec_feature_support =
		ext_csd[EXT_CSD_SEC_FEATURE_SUPPORT];
	card->ext_csd.raw_trim_mult =
		ext_csd[EXT_CSD_TRIM_MULT];
	card->ext_csd.raw_partition_support = ext_csd[EXT_CSD_PARTITION_SUPPORT];
	if (card->ext_csd.rev >= 4) {
		/*
		 * Enhanced area feature support -- check whether the eMMC
		 * card has the Enhanced area enabled.  If so, export enhanced
		 * area offset and size to user by adding sysfs interface.
		 */
		if ((ext_csd[EXT_CSD_PARTITION_SUPPORT] & 0x2) &&
		    (ext_csd[EXT_CSD_PARTITION_ATTRIBUTE] & 0x1)) {
			hc_erase_grp_sz =
				ext_csd[EXT_CSD_HC_ERASE_GRP_SIZE];
			hc_wp_grp_sz =
				ext_csd[EXT_CSD_HC_WP_GRP_SIZE];

			card->ext_csd.enhanced_area_en = 1;
			/*
			 * calculate the enhanced data area offset, in bytes
			 */
			card->ext_csd.enhanced_area_offset =
				(ext_csd[139] << 24) + (ext_csd[138] << 16) +
				(ext_csd[137] << 8) + ext_csd[136];
			if (mmc_card_blockaddr(card))
				card->ext_csd.enhanced_area_offset <<= 9;
			/*
			 * calculate the enhanced data area size, in kilobytes
			 */
			card->ext_csd.enhanced_area_size =
				(ext_csd[142] << 16) + (ext_csd[141] << 8) +
				ext_csd[140];
			card->ext_csd.enhanced_area_size *=
				(size_t)(hc_erase_grp_sz * hc_wp_grp_sz);
			card->ext_csd.enhanced_area_size <<= 9;
		} else {
			/*
			 * If the enhanced area is not enabled, disable these
			 * device attributes.
			 */
			card->ext_csd.enhanced_area_offset = -EINVAL;
			card->ext_csd.enhanced_area_size = -EINVAL;
		}

		/*
		 * General purpose partition feature support --
		 * If ext_csd has the size of general purpose partitions,
		 * set size, part_cfg, partition name in mmc_part.
		 */
		if (ext_csd[EXT_CSD_PARTITION_SUPPORT] &
			EXT_CSD_PART_SUPPORT_PART_EN) {
			if (card->ext_csd.enhanced_area_en != 1) {
				hc_erase_grp_sz =
					ext_csd[EXT_CSD_HC_ERASE_GRP_SIZE];
				hc_wp_grp_sz =
					ext_csd[EXT_CSD_HC_WP_GRP_SIZE];

				card->ext_csd.enhanced_area_en = 1;
			}

			for (idx = 0; idx < MMC_NUM_GP_PARTITION; idx++) {
				if (!ext_csd[EXT_CSD_GP_SIZE_MULT + idx * 3] &&
				!ext_csd[EXT_CSD_GP_SIZE_MULT + idx * 3 + 1] &&
				!ext_csd[EXT_CSD_GP_SIZE_MULT + idx * 3 + 2])
					continue;
				part_size =
				(ext_csd[EXT_CSD_GP_SIZE_MULT + idx * 3 + 2]
					<< 16) +
				(ext_csd[EXT_CSD_GP_SIZE_MULT + idx * 3 + 1]
					<< 8) +
				ext_csd[EXT_CSD_GP_SIZE_MULT + idx * 3];
				part_size *= (size_t)(hc_erase_grp_sz *
					hc_wp_grp_sz);
				mmc_part_add(card, part_size << 19,
					EXT_CSD_PART_CONFIG_ACC_GP0 + idx,
					"gp%d", idx, false,
					MMC_BLK_DATA_AREA_GP);
			}
		}
		card->ext_csd.sec_trim_mult =
			ext_csd[EXT_CSD_SEC_TRIM_MULT];
		card->ext_csd.sec_erase_mult =
			ext_csd[EXT_CSD_SEC_ERASE_MULT];
		card->ext_csd.sec_feature_support =
			ext_csd[EXT_CSD_SEC_FEATURE_SUPPORT];
		card->ext_csd.trim_timeout = 300 *
			ext_csd[EXT_CSD_TRIM_MULT];

		/*
		 * Note that the call to mmc_part_add above defaults to read
		 * only. If this default assumption is changed, the call must
		 * take into account the value of boot_locked below.
		 */
		card->ext_csd.boot_ro_lock = ext_csd[EXT_CSD_BOOT_WP];
		card->ext_csd.boot_ro_lockable = true;
	}

	if (card->ext_csd.rev >= 5) {
		/* check whether the eMMC card supports BKOPS */
		if (ext_csd[EXT_CSD_BKOPS_SUPPORT] & 0x1) {
			card->ext_csd.bkops = 1;
			card->ext_csd.bkops_en = ext_csd[EXT_CSD_BKOPS_EN];
			card->ext_csd.raw_bkops_status =
				ext_csd[EXT_CSD_BKOPS_STATUS];
			if (!card->ext_csd.bkops_en)
				pr_info("%s: BKOPS_EN bit is not set\n",
					mmc_hostname(card->host));
		}

		/* check whether the eMMC card supports HPI */
		if (ext_csd[EXT_CSD_HPI_FEATURES] & 0x1) {
			card->ext_csd.hpi = 1;
			if (ext_csd[EXT_CSD_HPI_FEATURES] & 0x2)
				card->ext_csd.hpi_cmd =	MMC_STOP_TRANSMISSION;
			else
				card->ext_csd.hpi_cmd = MMC_SEND_STATUS;
			/*
			 * Indicate the maximum timeout to close
			 * a command interrupted by HPI
			 */
			card->ext_csd.out_of_int_time =
				ext_csd[EXT_CSD_OUT_OF_INTERRUPT_TIME] * 10;
		}

		card->ext_csd.rel_param = ext_csd[EXT_CSD_WR_REL_PARAM];
		card->ext_csd.rst_n_function = ext_csd[EXT_CSD_RST_N_FUNCTION];

		/*
		 * RPMB regions are defined in multiples of 128K.
		 */
		card->ext_csd.raw_rpmb_size_mult = ext_csd[EXT_CSD_RPMB_MULT];
		if (ext_csd[EXT_CSD_RPMB_MULT] && mmc_host_cmd23(card->host)) {
			mmc_part_add(card, ext_csd[EXT_CSD_RPMB_MULT] << 17,
				EXT_CSD_PART_CONFIG_ACC_RPMB,
				"rpmb", 0, false,
				MMC_BLK_DATA_AREA_RPMB);
		}
	}

	card->ext_csd.raw_erased_mem_count = ext_csd[EXT_CSD_ERASED_MEM_CONT];
	if (ext_csd[EXT_CSD_ERASED_MEM_CONT])
		card->erased_byte = 0xFF;
	else
		card->erased_byte = 0x0;

        /* for samsung emmc4.41 plus spec */
        if ((card->cid.manfid == VENDOR_SAMSUNG) && 
            (card->ext_csd.rev == 5)             && 
            (1 == (0x1 & ext_csd[EXT_CSD_SAMSUNG_FEATURE]))){
            printk("set to support discard\n");
            card->ext_csd.feature_support |= MMC_DISCARD_FEATURE;
        }

	/* eMMC v4.5 or later */
	if (card->ext_csd.rev >= 6) {
		card->ext_csd.feature_support |= MMC_DISCARD_FEATURE;

		card->ext_csd.generic_cmd6_time = 10 *
			ext_csd[EXT_CSD_GENERIC_CMD6_TIME];
		card->ext_csd.power_off_longtime = 10 *
			ext_csd[EXT_CSD_POWER_OFF_LONG_TIME];

		card->ext_csd.cache_size =
			ext_csd[EXT_CSD_CACHE_SIZE + 0] << 0 |
			ext_csd[EXT_CSD_CACHE_SIZE + 1] << 8 |
			ext_csd[EXT_CSD_CACHE_SIZE + 2] << 16 |
			ext_csd[EXT_CSD_CACHE_SIZE + 3] << 24;

		if (ext_csd[EXT_CSD_DATA_SECTOR_SIZE] == 1)
			card->ext_csd.data_sector_size = 4096;
		else
			card->ext_csd.data_sector_size = 512;

		if ((ext_csd[EXT_CSD_DATA_TAG_SUPPORT] & 1) &&
		    (ext_csd[EXT_CSD_TAG_UNIT_SIZE] <= 8)) {
			card->ext_csd.data_tag_unit_size =
			((unsigned int) 1 << ext_csd[EXT_CSD_TAG_UNIT_SIZE]) *
			(card->ext_csd.data_sector_size);
		} else {
			card->ext_csd.data_tag_unit_size = 0;
		}

		card->ext_csd.max_packed_writes =
			ext_csd[EXT_CSD_MAX_PACKED_WRITES];
		card->ext_csd.max_packed_reads =
			ext_csd[EXT_CSD_MAX_PACKED_READS];
	} else {
		card->ext_csd.data_sector_size = 512;
	}

out:
	return err;
}

static inline void mmc_free_ext_csd(u8 *ext_csd)
{
	kfree(ext_csd);
}


static int mmc_compare_ext_csds(struct mmc_card *card, unsigned bus_width)
{
	u8 *bw_ext_csd;
	int err;

	if (bus_width == MMC_BUS_WIDTH_1)
		return 0;

	err = mmc_get_ext_csd(card, &bw_ext_csd);

	if (err || bw_ext_csd == NULL) {
		err = -EINVAL;
		goto out;
	}

	/* only compare read only fields */
	err = !((card->ext_csd.raw_partition_support ==
			bw_ext_csd[EXT_CSD_PARTITION_SUPPORT]) &&
		(card->ext_csd.raw_erased_mem_count ==
			bw_ext_csd[EXT_CSD_ERASED_MEM_CONT]) &&
		(card->ext_csd.rev ==
			bw_ext_csd[EXT_CSD_REV]) &&
		(card->ext_csd.raw_ext_csd_structure ==
			bw_ext_csd[EXT_CSD_STRUCTURE]) &&
		(card->ext_csd.raw_card_type ==
			bw_ext_csd[EXT_CSD_CARD_TYPE]) &&
		(card->ext_csd.raw_s_a_timeout ==
			bw_ext_csd[EXT_CSD_S_A_TIMEOUT]) &&
		(card->ext_csd.raw_hc_erase_gap_size ==
			bw_ext_csd[EXT_CSD_HC_WP_GRP_SIZE]) &&
		(card->ext_csd.raw_erase_timeout_mult ==
			bw_ext_csd[EXT_CSD_ERASE_TIMEOUT_MULT]) &&
		(card->ext_csd.raw_hc_erase_grp_size ==
			bw_ext_csd[EXT_CSD_HC_ERASE_GRP_SIZE]) &&
		(card->ext_csd.raw_sec_trim_mult ==
			bw_ext_csd[EXT_CSD_SEC_TRIM_MULT]) &&
		(card->ext_csd.raw_sec_erase_mult ==
			bw_ext_csd[EXT_CSD_SEC_ERASE_MULT]) &&
		(card->ext_csd.raw_sec_feature_support ==
			bw_ext_csd[EXT_CSD_SEC_FEATURE_SUPPORT]) &&
		(card->ext_csd.raw_trim_mult ==
			bw_ext_csd[EXT_CSD_TRIM_MULT]) &&
		(card->ext_csd.raw_sectors[0] ==
			bw_ext_csd[EXT_CSD_SEC_CNT + 0]) &&
		(card->ext_csd.raw_sectors[1] ==
			bw_ext_csd[EXT_CSD_SEC_CNT + 1]) &&
		(card->ext_csd.raw_sectors[2] ==
			bw_ext_csd[EXT_CSD_SEC_CNT + 2]) &&
		(card->ext_csd.raw_sectors[3] ==
			bw_ext_csd[EXT_CSD_SEC_CNT + 3]));
	if (err)
		err = -EINVAL;

out:
	mmc_free_ext_csd(bw_ext_csd);
	return err;
}

#ifdef CONFIG_MMC_SAMSUNG_SMART
static ssize_t mmc_samsung_smart(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct mmc_card *card = mmc_dev_to_card(dev);

	if (card->quirks & MMC_QUIRK_SAMSUNG_SMART)
		return mmc_samsung_smart_handle(card, buf);

	/* There is no information available for this card. */
	return 0;
}
static DEVICE_ATTR(samsung_smart, S_IRUGO, mmc_samsung_smart, NULL);
#endif /* CONFIG_MMC_SAMSUNG_SMART */

MMC_DEV_ATTR(cid, "%08x%08x%08x%08x\n", card->raw_cid[0], card->raw_cid[1],
	card->raw_cid[2], card->raw_cid[3]);
MMC_DEV_ATTR(csd, "%08x%08x%08x%08x\n", card->raw_csd[0], card->raw_csd[1],
	card->raw_csd[2], card->raw_csd[3]);
MMC_DEV_ATTR(date, "%02d/%04d\n", card->cid.month, card->cid.year);
MMC_DEV_ATTR(erase_size, "%u\n", card->erase_size << 9);
MMC_DEV_ATTR(preferred_erase_size, "%u\n", card->pref_erase << 9);
MMC_DEV_ATTR(fwrev, "0x%x\n", card->cid.fwrev);
MMC_DEV_ATTR(hwrev, "0x%x\n", card->cid.hwrev);
MMC_DEV_ATTR(manfid, "0x%06x\n", card->cid.manfid);
MMC_DEV_ATTR(name, "%s\n", card->cid.prod_name);
MMC_DEV_ATTR(oemid, "0x%04x\n", card->cid.oemid);
MMC_DEV_ATTR(prv, "0x%x\n", card->cid.prv);
MMC_DEV_ATTR(serial, "0x%08x\n", card->cid.serial);
MMC_DEV_ATTR(enhanced_area_offset, "%llu\n",
		card->ext_csd.enhanced_area_offset);
MMC_DEV_ATTR(enhanced_area_size, "%u\n", card->ext_csd.enhanced_area_size);
MMC_DEV_ATTR(raw_rpmb_size_mult, "%#x\n", card->ext_csd.raw_rpmb_size_mult);
MMC_DEV_ATTR(rel_sectors, "%#x\n", card->ext_csd.rel_sectors);


static int mmc_can_ext_csd(struct mmc_card *card)
{
	return (card && card->csd.mmca_vsn > CSD_SPEC_VER_3);
}

#ifdef CONFIG_AMAZON_METRICS_LOG
static void mmc_metrics(struct mmc_card *card)
{
	char *buf;
	buf = vmalloc(VITALS_LIFETIME_DATA_LEN * sizeof(char));
	if(buf){
		snprintf(buf, METRICS_LIFETIME_DATA_LEN,
			"emmc:info:est_life_time_type_a_%x=1;CT;1:NR", card->ext_csd.raw_dev_lifetime_est_a);
		log_to_metrics(ANDROID_LOG_INFO, LMK_METRIC_TAG, buf);

		snprintf(buf, METRICS_LIFETIME_DATA_LEN,
			"emmc:info:est_life_time_type_b_%x=1;CT;1:NR", card->ext_csd.raw_dev_lifetime_est_b);
		log_to_metrics(ANDROID_LOG_INFO, LMK_METRIC_TAG, buf);

		if(card->ext_csd.raw_dev_lifetime_est_a == 0xb || card->ext_csd.raw_dev_lifetime_est_b == 0xb) {
			snprintf(buf, METRICS_LIFETIME_DATA_LEN, "lk1nigk5:vyot/2/0c330411::status=expired;SY,_deviceId=;SY:");
			log_to_metrics(ANDROID_LOG_INFO, LMK_METRIC_TAG, buf);
		}

		snprintf(buf, VITALS_LIFETIME_DATA_LEN,
			"SYSTEM_BSP_DIAG:emmc_health:fgtracking=false;DV;1,key=0x%x;DV;1,Timer=1.0;TI;1,unit=count;DV;1,"
			"metadata=0x%x!{\"d\"#{\"ManfID\"#\"0x%x\"$\"LifetimeTypeA\"#\"0x%x\"$\"LifetimeTypeB\"#\"0x%x\"}};DV;1:HI",
			card->ext_csd.raw_dev_lifetime_est_b, card->ext_csd.raw_dev_lifetime_est_a, card->cid.manfid,
			card->ext_csd.raw_dev_lifetime_est_a, card->ext_csd.raw_dev_lifetime_est_b);
		log_to_vitals(ANDROID_LOG_INFO, VITALS_DOMAIN, buf);

                pr_info("%s", buf);

		vfree(buf);
	} else {
		printk("allocate metrics buf error for emmc");
	}
}
#endif

static ssize_t mmc_life_time_est_a_show(struct device *dev,
                                        struct device_attribute *attr,
                                        char *buf)
{
	struct mmc_card *card = mmc_dev_to_card(dev);
	u8 *ext_csd = NULL;
	int err;

	mmc_claim_host(card->host);

	if (!mmc_can_ext_csd(card)) {
		mmc_release_host(card->host);
		return 0;
	}

	err = mmc_get_ext_csd(card, &ext_csd);
	if (err) {
		if (ext_csd)
			kfree(ext_csd);

		mmc_release_host(card->host);
		return err;
	}

	card->ext_csd.raw_dev_lifetime_est_a =
		ext_csd[EXT_CSD_DEV_LIFETIME_EST_A];

	kfree(ext_csd);
	mmc_release_host(card->host);

	return sprintf(buf, "0x%02x\n",
		       card->ext_csd.raw_dev_lifetime_est_a);
}
static DEVICE_ATTR(lifetime_est_a, S_IRUGO, mmc_life_time_est_a_show, NULL);

static ssize_t mmc_life_time_est_b_show(struct device *dev,
                                        struct device_attribute *attr,
                                        char *buf)
{
	struct mmc_card *card = mmc_dev_to_card(dev);
	u8 *ext_csd = NULL;
	int err;

	mmc_claim_host(card->host);

	if (!mmc_can_ext_csd(card)) {
		mmc_release_host(card->host);
		return 0;
	}

	err = mmc_get_ext_csd(card, &ext_csd);
	if (err) {
		if (ext_csd)
			kfree(ext_csd);

		mmc_release_host(card->host);
		return err;
	}

	card->ext_csd.raw_dev_lifetime_est_b =
		ext_csd[EXT_CSD_DEV_LIFETIME_EST_B];

	kfree(ext_csd);
	mmc_release_host(card->host);

	return sprintf(buf, "0x%02x\n",
		       card->ext_csd.raw_dev_lifetime_est_b);
}
static DEVICE_ATTR(lifetime_est_b, S_IRUGO, mmc_life_time_est_b_show, NULL);

static ssize_t mmc_life_time_show(struct device *dev,
                                  struct device_attribute *attr,
                                  char *buf)
{
	struct mmc_card *card = mmc_dev_to_card(dev);
	u8 *ext_csd = NULL;
	int err;

	mmc_claim_host(card->host);

	if (!mmc_can_ext_csd(card)) {
		mmc_release_host(card->host);
		return 0;
	}

	err = mmc_get_ext_csd(card, &ext_csd);
	if (err) {
		if (ext_csd)
			kfree(ext_csd);

		mmc_release_host(card->host);
		return err;
	}

	card->ext_csd.raw_dev_lifetime_est_a =
		ext_csd[EXT_CSD_DEV_LIFETIME_EST_A];
	card->ext_csd.raw_dev_lifetime_est_b =
		ext_csd[EXT_CSD_DEV_LIFETIME_EST_B];

#ifdef CONFIG_AMAZON_METRICS_LOG
	mmc_metrics(card);
#endif
	kfree(ext_csd);
	mmc_release_host(card->host);

	return sprintf(buf, "0x%02x 0x%02x\n",
		       card->ext_csd.raw_dev_lifetime_est_a,
		       card->ext_csd.raw_dev_lifetime_est_b);
}
static DEVICE_ATTR(life_time, S_IRUGO, mmc_life_time_show, NULL);

static struct attribute *mmc_std_attrs[] = {
	&dev_attr_cid.attr,
	&dev_attr_csd.attr,
	&dev_attr_date.attr,
	&dev_attr_erase_size.attr,
	&dev_attr_preferred_erase_size.attr,
	&dev_attr_fwrev.attr,
	&dev_attr_hwrev.attr,
	&dev_attr_manfid.attr,
	&dev_attr_name.attr,
	&dev_attr_oemid.attr,
	&dev_attr_prv.attr,
	&dev_attr_serial.attr,
	&dev_attr_enhanced_area_offset.attr,
	&dev_attr_enhanced_area_size.attr,
	&dev_attr_raw_rpmb_size_mult.attr,
	&dev_attr_rel_sectors.attr,
#ifdef CONFIG_MMC_SAMSUNG_SMART
	&dev_attr_samsung_smart.attr,
#endif /* CONFIG_MMC_SAMSUNG_SMART */
	&dev_attr_lifetime_est_a.attr,
	&dev_attr_lifetime_est_b.attr,
	&dev_attr_life_time.attr,
	NULL,
};

static struct attribute_group mmc_std_attr_group = {
	.attrs = mmc_std_attrs,
};

static const struct attribute_group *mmc_attr_groups[] = {
	&mmc_std_attr_group,
	NULL,
};

static struct device_type mmc_type = {
	.groups = mmc_attr_groups,
};

/*
 * Select the PowerClass for the current bus width
 * If power class is defined for 4/8 bit bus in the
 * extended CSD register, select it by executing the
 * mmc_switch command.
 */
static int mmc_select_powerclass(struct mmc_card *card,
		unsigned int bus_width, u8 *ext_csd)
{
	int err = 0;
	unsigned int pwrclass_val;
	unsigned int index = 0;
	struct mmc_host *host;

	BUG_ON(!card);

	host = card->host;
	BUG_ON(!host);

	if (ext_csd == NULL)
		return 0;

	/* Power class selection is supported for versions >= 4.0 */
	if (card->csd.mmca_vsn < CSD_SPEC_VER_4)
		return 0;

	/* Power class values are defined only for 4/8 bit bus */
	if (bus_width == EXT_CSD_BUS_WIDTH_1)
		return 0;

	switch (1 << host->ios.vdd) {
	case MMC_VDD_165_195:
		if (host->ios.clock <= 26000000)
			index = EXT_CSD_PWR_CL_26_195;
		else if	(host->ios.clock <= 52000000)
			index = (bus_width <= EXT_CSD_BUS_WIDTH_8) ?
				EXT_CSD_PWR_CL_52_195 :
				EXT_CSD_PWR_CL_DDR_52_195;
		else if (host->ios.clock <= 200000000)
			index = EXT_CSD_PWR_CL_200_195;
		break;
	case MMC_VDD_27_28:
	case MMC_VDD_28_29:
	case MMC_VDD_29_30:
	case MMC_VDD_30_31:
	case MMC_VDD_31_32:
	case MMC_VDD_32_33:
	case MMC_VDD_33_34:
	case MMC_VDD_34_35:
	case MMC_VDD_35_36:
		if (host->ios.clock <= 26000000)
			index = EXT_CSD_PWR_CL_26_360;
		else if	(host->ios.clock <= 52000000)
			index = (bus_width <= EXT_CSD_BUS_WIDTH_8) ?
				EXT_CSD_PWR_CL_52_360 :
				EXT_CSD_PWR_CL_DDR_52_360;
		else if (host->ios.clock <= 200000000)
			index = EXT_CSD_PWR_CL_200_360;
		break;
	default:
		pr_warning("%s: Voltage range not supported "
			   "for power class.\n", mmc_hostname(host));
		return -EINVAL;
	}

	pwrclass_val = ext_csd[index];

	if (bus_width & (EXT_CSD_BUS_WIDTH_8 | EXT_CSD_DDR_BUS_WIDTH_8))
		pwrclass_val = (pwrclass_val & EXT_CSD_PWR_CL_8BIT_MASK) >>
				EXT_CSD_PWR_CL_8BIT_SHIFT;
	else
		pwrclass_val = (pwrclass_val & EXT_CSD_PWR_CL_4BIT_MASK) >>
				EXT_CSD_PWR_CL_4BIT_SHIFT;

	/* If the power class is different from the default value */
	if (pwrclass_val > 0) {
		err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
				 EXT_CSD_POWER_CLASS,
				 pwrclass_val,
				 card->ext_csd.generic_cmd6_time);
	}

	return err;
}
#ifdef CONFIG_EMMC_50_FEATURE
/*
 * Selects the desired buswidth and switch to the HS400 mode
 * if bus width set without error
 */
static int mmc_select_hs400(struct mmc_card *card)
{
	int err = -EINVAL;
	struct mmc_host *host;

	BUG_ON(!card);

	host = card->host;

	if (card->ext_csd.card_type & EXT_CSD_CARD_TYPE_HS400_1_2V &&
			host->caps2 & MMC_CAP2_HS400_1_2V_DDR)
		err = __mmc_set_signal_voltage(host, MMC_SIGNAL_VOLTAGE_120);

	if (err && card->ext_csd.card_type & EXT_CSD_CARD_TYPE_HS400_1_8V &&
			host->caps2 & MMC_CAP2_HS400_1_8V_DDR)
		err = __mmc_set_signal_voltage(host, MMC_SIGNAL_VOLTAGE_180);

	/* If fails try again during next card power cycle */
	if (err)
		goto err;

	/* switch to High speed mode  */
	err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
				 EXT_CSD_HS_TIMING, 1, 0);

	if(err)
		goto err;

	/* switch to DDR50 mode */
	err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
				 EXT_CSD_BUS_WIDTH,
                 EXT_CSD_DDR_BUS_WIDTH_8,
				 card->ext_csd.generic_cmd6_time);
	if (err)
		goto err;

	mmc_set_bus_width(card->host, MMC_BUS_WIDTH_8);

	
	/* switch to HS400 mode if bus width set successfully */
	err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
				 EXT_CSD_HS_TIMING, 3, 0);
err:
    printk("[%s]: switch to HS400 speed mode, err=%d\n", __func__, err); 
	return err;
}
#endif

/*
 * Selects the desired buswidth and switch to the HS200 mode
 * if bus width set without error
 */
static int mmc_select_hs200(struct mmc_card *card)
{
	int idx, err = -EINVAL;
	struct mmc_host *host;
	static unsigned ext_csd_bits[] = {
		EXT_CSD_BUS_WIDTH_4,
		EXT_CSD_BUS_WIDTH_8,
	};
	static unsigned bus_widths[] = {
		MMC_BUS_WIDTH_4,
		MMC_BUS_WIDTH_8,
	};

	BUG_ON(!card);

	host = card->host;

	if (card->ext_csd.card_type & EXT_CSD_CARD_TYPE_HS200_1_2V &&
			host->caps2 & MMC_CAP2_HS200_1_2V_SDR)
		err = __mmc_set_signal_voltage(host, MMC_SIGNAL_VOLTAGE_120);

	if (err && card->ext_csd.card_type & EXT_CSD_CARD_TYPE_HS200_1_8V &&
			host->caps2 & MMC_CAP2_HS200_1_8V_SDR)
		err = __mmc_set_signal_voltage(host, MMC_SIGNAL_VOLTAGE_180);

	/* If fails try again during next card power cycle */
	if (err)
		goto err;
#ifdef CONFIG_EMMC_50_FEATURE
	/* switch to High speed mode  */
	if (card->ext_csd.card_type & EXT_CSD_CARD_TYPE_HS400_1_2V ||
        card->ext_csd.card_type & EXT_CSD_CARD_TYPE_HS400_1_8V){
	    err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
				 EXT_CSD_HS_TIMING, 1, 0);

	    if(err)
		     goto err;
    }
#endif
	idx = (host->caps & MMC_CAP_8_BIT_DATA) ? 1 : 0;

	/*
	 * Unlike SD, MMC cards dont have a configuration register to notify
	 * supported bus width. So bus test command should be run to identify
	 * the supported bus width or compare the ext csd values of current
	 * bus width and ext csd values of 1 bit mode read earlier.
	 */
	for (; idx >= 0; idx--) {

		/*
		 * Host is capable of 8bit transfer, then switch
		 * the device to work in 8bit transfer mode. If the
		 * mmc switch command returns error then switch to
		 * 4bit transfer mode. On success set the corresponding
		 * bus width on the host.
		 */
		err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
				 EXT_CSD_BUS_WIDTH,
				 ext_csd_bits[idx],
				 card->ext_csd.generic_cmd6_time);
		if (err)
			continue;

		mmc_set_bus_width(card->host, bus_widths[idx]);

		if (!(host->caps & MMC_CAP_BUS_WIDTH_TEST))
			err = mmc_compare_ext_csds(card, bus_widths[idx]);
		else
			err = mmc_bus_test(card, bus_widths[idx]);
		if (!err)
			break;
	}

	/* switch to HS200 mode if bus width set successfully */
	if (!err)
		err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
				 EXT_CSD_HS_TIMING, 2, 0);
err:
    printk("[%s]: switch to HS200 speed mode, err=%d\n", __func__, err); 
	return err;
}

#if (defined(CONFIG_AMAZON_METRICS_LOG) && defined(ENABLE_SAMSUNG_EMMC_METRICS))
static int emmcmetrics_read(struct mmc_host *host)
{
	int err = -1;
	int ret = 0;
	char logcatsamsung_data[METRICS_samsung_data_LEN];
	u32 *samsung_report;
	unsigned int *samsung_data;
	struct mmc_card *card;
	unsigned int minReservedBlocks;
	unsigned int minReservedBlocksQuantified;
	unsigned int maxEraseCountMLC;
	unsigned int maxEraseCountMLCQuantified;
	unsigned int avgEraseCountMLC;
	unsigned int avgEraseCountMLCQuantified;
	unsigned int maxEraseCountSLC;
	unsigned int maxEraseCountSLCQuantified;
	unsigned int avgEraseCountSLC;
	unsigned int avgEraseCountSLCQuantified;

	card = host->card;

	pr_info("Trying to read Samsung eMMC stats\n");

	samsung_report = kmalloc(512, GFP_KERNEL);
	if (!samsung_report) {
		pr_err("Failed to alloc memory for Smart Report\n");
		ret = -ENOMEM;
		goto fail;
	}

	if (card->quirks & MMC_QUIRK_SAMSUNG_SMART)
		err = mmc_samsung_report(card, (u8 *)samsung_report);

	if (!err) {
		int i;

		samsung_data = (unsigned int *)samsung_report;
		pr_info("Samsung data [20 dwords]:");
		for (i = 0; i < 10; ++i)
			pr_info(" %08X", samsung_data[i]);
		pr_info("\n");
		for (i = 10; i < 20; ++i)
			pr_info(" %08X", samsung_data[i]);
		pr_info("\n");
		for (i = 20; i < 30; ++i)
			pr_info(" %08X", samsung_data[i]);
		pr_info("\n");
		for (i = 30; i < 36; ++i)
			pr_info(" %08X", samsung_data[i]);
		pr_info("\n");

	} else {
		pr_info("...Read Samsung eMMC stats failed...");
		ret = -1;
		goto fail_read;
	}

	minReservedBlocks = samsung_data[7];          /*bank0*/
	if (minReservedBlocks > samsung_data[10] && samsung_data[10] > 0)
		minReservedBlocks = samsung_data[10]; /*bank1*/
	if (minReservedBlocks > samsung_data[13] && samsung_data[13] > 0)
		minReservedBlocks = samsung_data[13]; /*bank2*/
	if (minReservedBlocks > samsung_data[16] && samsung_data[16] > 0)
		minReservedBlocks = samsung_data[16]; /*bank3*/

	minReservedBlocksQuantified =     /*6 bands with 2^n gap*/
	(minReservedBlocks >= 32) ? 5 :
	(minReservedBlocks >= 16) ? 4 :
	(minReservedBlocks >=  8) ? 3 :
	(minReservedBlocks >=  4) ? 2 :
	(minReservedBlocks >=  2) ? 1 : 0;

	/*report minReservedBlocksQuantified as metrics*/
	snprintf(logcatsamsung_data, METRICS_samsung_data_LEN,
		"emmc:info:minReservedBlocksQuantified_%d=1;CT;1:NR",
		minReservedBlocksQuantified);
	log_to_metrics(ANDROID_LOG_INFO, LMK_METRIC_TAG, logcatsamsung_data);

	card->minReservedBlocks = minReservedBlocks;

	maxEraseCountMLC = samsung_data[33];
	maxEraseCountMLCQuantified =     /*6 bands in linear gap*/
	(maxEraseCountMLC >= 3000) ? 5 :
	(maxEraseCountMLC >= 2500) ? 4 :
	(maxEraseCountMLC >= 2000) ? 3 :
	(maxEraseCountMLC >= 1500) ? 2 :
	(maxEraseCountMLC >= 100) ? 1 : 0;

	/*report maxEraseCountMLCQuantified as metrics*/
	snprintf(logcatsamsung_data, METRICS_samsung_data_LEN,
		"emmc:info:maxEraseCountMLCQuantified_%d=1;CT;1:NR",
		maxEraseCountMLCQuantified);
	log_to_metrics(ANDROID_LOG_INFO, LMK_METRIC_TAG, logcatsamsung_data);

	card->maxEraseCountMLC = maxEraseCountMLC;

	avgEraseCountMLC = samsung_data[35];
	avgEraseCountMLCQuantified =    /*6 bands in linear gap*/
	(avgEraseCountMLC >= 1500) ? 5 :
	(avgEraseCountMLC >= 1250) ? 4 :
	(avgEraseCountMLC >= 1000) ? 3 :
	(avgEraseCountMLC >= 750) ? 2 :
	(avgEraseCountMLC >= 50) ? 1 : 0;

	/*report avgEraseCountMLCQuantified as metrics*/
	snprintf(logcatsamsung_data, METRICS_samsung_data_LEN,
		"emmc:info:avgEraseCountMLCQuantified_%d=1;CT;1:NR",
		avgEraseCountMLCQuantified);
	log_to_metrics(ANDROID_LOG_INFO, LMK_METRIC_TAG, logcatsamsung_data);

	card->avgEraseCountMLC = avgEraseCountMLC;

	maxEraseCountSLC = samsung_data[30];
	maxEraseCountSLCQuantified =     /*6 bands in linear gap*/
	(maxEraseCountSLC >= 2100) ? 5 :
	(maxEraseCountSLC >= 1750) ? 4 :
	(maxEraseCountSLC >= 1400) ? 3 :
	(maxEraseCountSLC >= 1050) ? 2 :
	(maxEraseCountSLC >= 70) ? 1 : 0;

	/*report maxEraseCountSLCQuantified as metrics*/
	snprintf(logcatsamsung_data, METRICS_samsung_data_LEN,
		"emmc:info:maxEraseCountSLCQuantified_%d=1;CT;1:NR",
		maxEraseCountSLCQuantified);
	log_to_metrics(ANDROID_LOG_INFO, LMK_METRIC_TAG, logcatsamsung_data);

	card->maxEraseCountSLC = maxEraseCountSLC;

	avgEraseCountSLC = samsung_data[32];
	avgEraseCountSLCQuantified =     /*6 bands in linear gap*/
	(avgEraseCountSLC >= 1050) ? 5 :
	(avgEraseCountSLC >= 875) ? 4 :
	(avgEraseCountSLC >= 700) ? 3 :
	(avgEraseCountSLC >= 525) ? 2 :
	(avgEraseCountSLC >= 35) ? 1 : 0;

	/*report avgEraseCountSLCQuantified as metrics*/
	snprintf(logcatsamsung_data, METRICS_samsung_data_LEN,
		"emmc:info:avgEraseCountSLCQuantified_%d=1;CT;1:NR",
		avgEraseCountSLCQuantified);
	log_to_metrics(ANDROID_LOG_INFO, LMK_METRIC_TAG, logcatsamsung_data);

	card->avgEraseCountSLC = avgEraseCountSLC;

fail_read:
	kfree(samsung_report);

fail:
	return ret;
}

/*
 * Internal work. Work to output metrics at some later point.
 */
void mmc_host_metrics_work(struct work_struct *work)
{
	struct mmc_host *host = container_of(work, struct mmc_host,
						metrics_delay_work.work);
	emmcmetrics_read(host);
}

static void metrics_delaywork_queue(struct mmc_host *host)
{
	/* delay 5 seconds to output metrics */
	queue_delayed_work(system_nrt_wq, &host->metrics_delay_work,
				msecs_to_jiffies(5000));
}
#endif /* CONFIG_AMAZON_METRICS_LOG */

/*
 * Handle the detection and initialisation of a card.
 *
 * In the case of a resume, "oldcard" will contain the card
 * we're trying to reinitialise.
 */
static int mmc_init_card(struct mmc_host *host, u32 ocr,
	struct mmc_card *oldcard)
{
	struct mmc_card *card;
	int err, ddr = 0;
	u32 cid[4];
	unsigned int max_dtr;
	u32 rocr;
	u8 *ext_csd = NULL;

	BUG_ON(!host);
	WARN_ON(!host->claimed);

	/* Set correct bus mode for MMC before attempting init */
	if (!mmc_host_is_spi(host))
		mmc_set_bus_mode(host, MMC_BUSMODE_OPENDRAIN);

	/*
	 * Since we're changing the OCR value, we seem to
	 * need to tell some cards to go back to the idle
	 * state.  We wait 1ms to give cards time to
	 * respond.
	 * mmc_go_idle is needed for eMMC that are asleep
	 */
	mmc_go_idle(host);

	/* The extra bit indicates that we support high capacity */
	err = mmc_send_op_cond(host, ocr | (1 << 30), &rocr);
	if (err)
		goto err;

	/*
	 * For SPI, enable CRC as appropriate.
	 */
	if (mmc_host_is_spi(host)) {
		err = mmc_spi_set_crc(host, use_spi_crc);
		if (err)
			goto err;
	}

	/*
	 * Fetch CID from card.
	 */
	if (mmc_host_is_spi(host))
		err = mmc_send_cid(host, cid);
	else
		err = mmc_all_send_cid(host, cid);
	if (err)
		goto err;

	if (oldcard) {
		if (memcmp((void *)cid, (void *)oldcard->raw_cid, sizeof(cid)) != 0) {
			err = -ENOENT;
			goto err;
		}

		card = oldcard;
	} else {
		/*
		 * Allocate card structure.
		 */
		card = mmc_alloc_card(host, &mmc_type);
		if (IS_ERR(card)) {
			err = PTR_ERR(card);
			goto err;
		}

		card->type = MMC_TYPE_MMC;
		card->rca = 1;
		memcpy(card->raw_cid, cid, sizeof(card->raw_cid));
	}

	/*
	 * For native busses:  set card RCA and quit open drain mode.
	 */
	if (!mmc_host_is_spi(host)) {
		err = mmc_set_relative_addr(card);
		if (err)
			goto free_card;

		mmc_set_bus_mode(host, MMC_BUSMODE_PUSHPULL);
	}

	if (!oldcard) {
		/*
		 * Fetch CSD from card.
		 */
		err = mmc_send_csd(card, card->raw_csd);
		if (err)
			goto free_card;
		err = mmc_decode_csd(card);
		if (err)
			goto free_card;
		err = mmc_decode_cid(card);
		if (err)
			goto free_card;
	}

	/*
	 * Select card, as all following commands rely on that.
	 */
	if (!mmc_host_is_spi(host)) {
		err = mmc_select_card(card);
		if (err)
			goto free_card;
	}

	if (!oldcard) {
		/*
		 * Fetch and process extended CSD.
		 */

		err = mmc_get_ext_csd(card, &ext_csd);
		if (err)
			goto free_card;
		err = mmc_read_ext_csd(card, ext_csd);
		if (err)
			goto free_card;

		/* If doing byte addressing, check if required to do sector
		 * addressing.  Handle the case of <2GB cards needing sector
		 * addressing.  See section 8.1 JEDEC Standard JED84-A441;
		 * ocr register has bit 30 set for sector addressing.
		 */
		if (!(mmc_card_blockaddr(card)) && (rocr & (1<<30)))
			mmc_card_set_blockaddr(card);

		/* Erase size depends on CSD and Extended CSD */
		mmc_set_erase_size(card);
	}
	/*add to print emmc vendor*/
	switch(card->cid.manfid){
	case 0x15:
		pr_info("[%s]: EMMC Vendor: SAMSUNG\n", __func__);
		break;
	case 0x13:
		pr_info("[%s]: EMMC Vendor: MICRON\n", __func__);
		break;
	case 0x90:
		pr_info("[%s]: EMMC Vendor: HYNIX\n", __func__);
		break;
	default:
		pr_info("[%s]: Unknown EMMC Vendor, manfid is 0X%x\n", __func__, card->cid.manfid);
		break;
	}

	if (card->csd.mmca_vsn >= CSD_SPEC_VER_2) {
		card->cid.prod_name[6] = '\0';
		pr_info("[%s]: Product Name:%s\n", __func__, card->cid.prod_name);
	}else{
		card->cid.prod_name[7] = '\0';
		pr_info("[%s]: Product Name:%s\n", __func__, card->cid.prod_name);
	}
	pr_info("[%s]: Firmware Version:%llx\n", __func__, card->ext_csd.raw_firmware_version);
	pr_info("[%s]: Device life time estimation type A:%x, life time estimation type B:%x\n", __func__,
					card->ext_csd.raw_dev_lifetime_est_a, card->ext_csd.raw_dev_lifetime_est_b);

	/*
	 * If enhanced_area_en is TRUE, host needs to enable ERASE_GRP_DEF
	 * bit.  This bit will be lost every time after a reset or power off.
	 */
	if (card->ext_csd.enhanced_area_en ||
	    (card->ext_csd.rev >= 3 && (host->caps2 & MMC_CAP2_HC_ERASE_SZ))) {
		err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
				 EXT_CSD_ERASE_GROUP_DEF, 1,
				 card->ext_csd.generic_cmd6_time);

		if (err && err != -EBADMSG)
			goto free_card;

		if (err) {
			err = 0;
			/*
			 * Just disable enhanced area off & sz
			 * will try to enable ERASE_GROUP_DEF
			 * during next time reinit
			 */
			card->ext_csd.enhanced_area_offset = -EINVAL;
			card->ext_csd.enhanced_area_size = -EINVAL;
		} else {
			card->ext_csd.erase_group_def = 1;
			/*
			 * enable ERASE_GRP_DEF successfully.
			 * This will affect the erase size, so
			 * here need to reset erase size
			 */
			mmc_set_erase_size(card);
		}
	}

	/*
	 * Ensure eMMC user default partition is enabled
	 */
	if (card->ext_csd.part_config & EXT_CSD_PART_CONFIG_ACC_MASK) {
		card->ext_csd.part_config &= ~EXT_CSD_PART_CONFIG_ACC_MASK;
		err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL, EXT_CSD_PART_CONFIG,
				 card->ext_csd.part_config,
				 card->ext_csd.part_time);
		if (err && err != -EBADMSG)
			goto free_card;
	}

	/*
	 * If the host supports the power_off_notify capability then
	 * set the notification byte in the ext_csd register of device
	 */
	if ((host->caps2 & MMC_CAP2_POWEROFF_NOTIFY) && (card->ext_csd.rev >= 6) && (card->quirks & MMC_QUIRK_PON)){
		err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
				 EXT_CSD_POWER_OFF_NOTIFICATION,
				 EXT_CSD_POWER_ON,
				 card->ext_csd.generic_cmd6_time);
		if (err && err != -EBADMSG)
			goto free_card;

		/*
		 * The err can be -EBADMSG or 0,
		 * so check for success and update the flag
		 */
		if (!err)
			card->ext_csd.power_off_notification = EXT_CSD_POWER_ON;
	}

	/*
	 * Activate high speed (if supported)
	 */
	if (card->ext_csd.hs_max_dtr != 0) {
		err = 0;
#ifdef CONFIG_EMMC_50_FEATURE
        if(card->ext_csd.hs_max_dtr >200000000 &&
           host->caps2 & MMC_CAP2_HS400)
           err = mmc_select_hs400(card); 
		else
#endif
		if (card->ext_csd.hs_max_dtr > 52000000 &&
		    host->caps2 & MMC_CAP2_HS200)
			err = mmc_select_hs200(card);
		else if	(host->caps & MMC_CAP_MMC_HIGHSPEED)
			err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
					 EXT_CSD_HS_TIMING, 1,
					 card->ext_csd.generic_cmd6_time);

		if (err && err != -EBADMSG)
			goto free_card;

		if (err) {
			pr_warning("%s: switch to highspeed failed\n",
			       mmc_hostname(card->host));
			err = 0;
		} else {
#ifdef CONFIG_EMMC_50_FEATURE
			if(card->ext_csd.hs_max_dtr >200000000 &&
			    host->caps2 & MMC_CAP2_HS400){
			    mmc_card_set_hs400(card); 
				mmc_set_timing(card->host, MMC_TIMING_MMC_HS400); 
			}else
#endif

			if (card->ext_csd.hs_max_dtr > 52000000 &&
			    host->caps2 & MMC_CAP2_HS200) {
				mmc_card_set_hs200(card);
				mmc_set_timing(card->host,
					       MMC_TIMING_MMC_HS200);
			} else {
				mmc_card_set_highspeed(card);
				mmc_set_timing(card->host, MMC_TIMING_MMC_HS);
			}
		}
	}

	/*
	 * Compute bus speed.
	 */
	max_dtr = (unsigned int)-1;
	if (mmc_card_highspeed(card) || 
#ifdef CONFIG_EMMC_50_FEATURE
        mmc_card_hs400(card) || 
#endif
        mmc_card_hs200(card)) {
		if (max_dtr > card->ext_csd.hs_max_dtr)
			max_dtr = card->ext_csd.hs_max_dtr;
		if (mmc_card_highspeed(card) && (max_dtr > 52000000))
			max_dtr = 52000000;
	} else if (max_dtr > card->csd.max_dtr) {
		max_dtr = card->csd.max_dtr;
	}

	mmc_set_clock(host, max_dtr);

	/*
	 * Indicate DDR mode (if supported).
	 */
	if (mmc_card_highspeed(card)) {
		if ((card->ext_csd.card_type & EXT_CSD_CARD_TYPE_DDR_1_8V)
			&& ((host->caps & (MMC_CAP_1_8V_DDR |
			     MMC_CAP_UHS_DDR50))
				== (MMC_CAP_1_8V_DDR | MMC_CAP_UHS_DDR50)))
				ddr = MMC_1_8V_DDR_MODE;
		else if ((card->ext_csd.card_type & EXT_CSD_CARD_TYPE_DDR_1_2V)
			&& ((host->caps & (MMC_CAP_1_2V_DDR |
			     MMC_CAP_UHS_DDR50))
				== (MMC_CAP_1_2V_DDR | MMC_CAP_UHS_DDR50)))
				ddr = MMC_1_2V_DDR_MODE;
	}

#ifdef CONFIG_EMMC_50_FEATURE
	/*
	 * Indicate HS400 SDR mode (if supported).
	 */

    if(mmc_card_hs400(card)){
        err = mmc_select_powerclass(card, EXT_CSD_DDR_BUS_WIDTH_8, ext_csd); 
		if (err)
			pr_warning("%s: power class selection to 8bit DDR failed\n", mmc_hostname(card->host));
        
    }
#endif
	/*
	 * Indicate HS200 SDR mode (if supported).
	 */
	if (mmc_card_hs200(card)) {
		u32 ext_csd_bits;
		u32 bus_width = card->host->ios.bus_width;

		/*
		 * For devices supporting HS200 mode, the bus width has
		 * to be set before executing the tuning function. If
		 * set before tuning, then device will respond with CRC
		 * errors for responses on CMD line. So for HS200 the
		 * sequence will be
		 * 1. set bus width 4bit / 8 bit (1 bit not supported)
		 * 2. switch to HS200 mode
		 * 3. set the clock to > 52Mhz <=200MHz and
		 * 4. execute tuning for HS200
		 */
		if ((host->caps2 & MMC_CAP2_HS200) &&
		    card->host->ops->execute_tuning) {
			mmc_host_clk_hold(card->host);
			err = card->host->ops->execute_tuning(card->host,
				MMC_SEND_TUNING_BLOCK_HS200);
			mmc_host_clk_release(card->host);
		}
		if (err) {
			pr_warning("%s: tuning execution failed\n",
				   mmc_hostname(card->host));
			goto err;
		}

		ext_csd_bits = (bus_width == MMC_BUS_WIDTH_8) ?
				EXT_CSD_BUS_WIDTH_8 : EXT_CSD_BUS_WIDTH_4;
		err = mmc_select_powerclass(card, ext_csd_bits, ext_csd);
		if (err)
			pr_warning("%s: power class selection to bus width %d"
				   " failed\n", mmc_hostname(card->host),
				   1 << bus_width);
	}

	/*
	 * Activate wide bus and DDR (if supported).
	 */
	if (!mmc_card_hs200(card) &&
#ifdef CONFIG_EMMC_50_FEATURE
        !mmc_card_hs400(card) &&
#endif
	    (card->csd.mmca_vsn >= CSD_SPEC_VER_4) &&
	    (host->caps & (MMC_CAP_4_BIT_DATA | MMC_CAP_8_BIT_DATA))) {
		static unsigned ext_csd_bits[][2] = {
			{ EXT_CSD_BUS_WIDTH_8, EXT_CSD_DDR_BUS_WIDTH_8 },
			{ EXT_CSD_BUS_WIDTH_4, EXT_CSD_DDR_BUS_WIDTH_4 },
			{ EXT_CSD_BUS_WIDTH_1, EXT_CSD_BUS_WIDTH_1 },
		};
		static unsigned bus_widths[] = {
			MMC_BUS_WIDTH_8,
			MMC_BUS_WIDTH_4,
			MMC_BUS_WIDTH_1
		};
		unsigned idx, bus_width = 0;

		if (host->caps & MMC_CAP_8_BIT_DATA)
			idx = 0;
		else
			idx = 1;
		for (; idx < ARRAY_SIZE(bus_widths); idx++) {
			bus_width = bus_widths[idx];
			if (bus_width == MMC_BUS_WIDTH_1)
				ddr = 0; /* no DDR for 1-bit width */
			err = mmc_select_powerclass(card, ext_csd_bits[idx][0],
						    ext_csd);
			if (err)
				pr_warning("%s: power class selection to "
					   "bus width %d failed\n",
					   mmc_hostname(card->host),
					   1 << bus_width);

			err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
					 EXT_CSD_BUS_WIDTH,
					 ext_csd_bits[idx][0],
					 card->ext_csd.generic_cmd6_time);
			if (!err) {
				mmc_set_bus_width(card->host, bus_width);

				/*
				 * If controller can't handle bus width test,
				 * compare ext_csd previously read in 1 bit mode
				 * against ext_csd at new bus width
				 */
				if (!(host->caps & MMC_CAP_BUS_WIDTH_TEST))
					err = mmc_compare_ext_csds(card,
						bus_width);
				else
					err = mmc_bus_test(card, bus_width);
				if (!err)
					break;
			}
		}

		if (!err && ddr) {
			err = mmc_select_powerclass(card, ext_csd_bits[idx][1],
						    ext_csd);
			if (err)
				pr_warning("%s: power class selection to "
					   "bus width %d ddr %d failed\n",
					   mmc_hostname(card->host),
					   1 << bus_width, ddr);

			err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
					 EXT_CSD_BUS_WIDTH,
					 ext_csd_bits[idx][1],
					 card->ext_csd.generic_cmd6_time);
		}
		if (err) {
			pr_warning("%s: switch to bus width %d ddr %d "
				"failed\n", mmc_hostname(card->host),
				1 << bus_width, ddr);
			goto free_card;
		} else if (ddr) {
			/*
			 * eMMC cards can support 3.3V to 1.2V i/o (vccq)
			 * signaling.
			 *
			 * EXT_CSD_CARD_TYPE_DDR_1_8V means 3.3V or 1.8V vccq.
			 *
			 * 1.8V vccq at 3.3V core voltage (vcc) is not required
			 * in the JEDEC spec for DDR.
			 *
			 * Do not force change in vccq since we are obviously
			 * working and no change to vccq is needed.
			 *
			 * WARNING: eMMC rules are NOT the same as SD DDR
			 */
			if (ddr == MMC_1_2V_DDR_MODE) {
				err = __mmc_set_signal_voltage(host,
					MMC_SIGNAL_VOLTAGE_120);
				if (err)
					goto err;
			}
			mmc_card_set_ddr_mode(card);
			mmc_set_timing(card->host, MMC_TIMING_UHS_DDR50);
			mmc_set_bus_width(card->host, bus_width);
		}
	}

	/*
	 * Enable HPI feature (if supported)
	 */
	if (card->ext_csd.hpi) {
		err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
				EXT_CSD_HPI_MGMT, 1,
				card->ext_csd.generic_cmd6_time);
		if (err && err != -EBADMSG)
			goto free_card;
		if (err) {
			pr_warning("%s: Enabling HPI failed\n",
				   mmc_hostname(card->host));
			err = 0;
		} else
			card->ext_csd.hpi_en = 1;
	}

	/*
	 * If cache size is higher than 0, this indicates
	 * the existence of cache and it can be turned on.
	 */
#ifndef CONFIG_MTK_EMMC_CACHE
	//the quirks is intialized after mmc_add_card(), and failed to disable cache by !(card->quirks & MMC_QUIRK_DISABLE_CACHE) && 
	if ((host->caps2 & MMC_CAP2_CACHE_CTRL) &&
         (card->ext_csd.cache_size > 0)) { 
		err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
				EXT_CSD_CACHE_CTRL, 1,
				card->ext_csd.generic_cmd6_time);
		if (err && err != -EBADMSG)
			goto free_card;

		/*
		 * Only if no error, cache is turned on successfully.
		 */
		if (err) {
			pr_warning("%s: Cache is supported, "
					"but failed to turn on (%d)\n",
					mmc_hostname(card->host), err);
			card->ext_csd.cache_ctrl = 0;
			err = 0;
		} else {
			card->ext_csd.cache_ctrl = 1;
		}
	}
#endif
	/*
	 * The mandatory minimum values are defined for packed command.
	 * read: 5, write: 3
	 */
	if (card->ext_csd.max_packed_writes >= 3 &&
	    card->ext_csd.max_packed_reads >= 5 &&
	    host->caps2 & MMC_CAP2_PACKED_CMD) {
		err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
				EXT_CSD_EXP_EVENTS_CTRL,
				EXT_CSD_PACKED_EVENT_EN,
				card->ext_csd.generic_cmd6_time);
		if (err && err != -EBADMSG)
			goto free_card;
		if (err) {
			pr_warn("%s: Enabling packed event failed\n",
				mmc_hostname(card->host));
			card->ext_csd.packed_event_en = 0;
			err = 0;
		} else {
			card->ext_csd.packed_event_en = 1;
		}
	}

	if (!oldcard)
		host->card = card;

#ifdef CONFIG_MTK_EMMC_SUPPORT_OTP 
    /* enable hc erase grp size */
    printk("switch to hc erase grp size\n");
    err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
            EXT_CSD_ERASE_GROUP_DEF, 1, 0);
    card->ext_csd.erase_group_def = 1;
#endif

	mmc_free_ext_csd(ext_csd);
	return 0;

free_card:
	if (!oldcard)
		mmc_remove_card(card);
err:
	mmc_free_ext_csd(ext_csd);

	return err;
}

static int mmc_can_poweroff_notify(const struct mmc_card *card)
{
	return card &&
		mmc_card_mmc(card) &&
		(card->ext_csd.power_off_notification == EXT_CSD_POWER_ON);
}

static int mmc_poweroff_notify(struct mmc_card *card, unsigned int notify_type)
{
	unsigned int timeout = card->ext_csd.generic_cmd6_time;
	int err;

	/* Use EXT_CSD_POWER_OFF_SHORT as default notification type. */
	if (notify_type == EXT_CSD_POWER_OFF_LONG)
		timeout = card->ext_csd.power_off_longtime;

	err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
			 EXT_CSD_POWER_OFF_NOTIFICATION,
			 notify_type, timeout);
	if (err)
		pr_err("%s: Power Off Notification timed out, %u\n",
		       mmc_hostname(card->host), timeout);

	/* Disable the power off notification after the switch operation. */
	card->ext_csd.power_off_notification = EXT_CSD_NO_POWER_NOTIFICATION;

	return err;
}

/*
 * Host is being removed. Free up the current card.
 */
static void mmc_remove(struct mmc_host *host)
{
	BUG_ON(!host);
	BUG_ON(!host->card);

	mmc_remove_card(host->card);
	host->card = NULL;
}

/*
 * Card detection - card is alive.
 */
static int mmc_alive(struct mmc_host *host)
{
	return mmc_send_status(host->card, NULL);
}

/*
 * Card detection callback from host.
 */
static void mmc_detect(struct mmc_host *host)
{
	int err;

	BUG_ON(!host);
	BUG_ON(!host->card);

	mmc_claim_host(host);

	/*
	 * Just check if our card has been removed.
	 */
	err = _mmc_detect_card_removed(host);

	mmc_release_host(host);

	if (err) {
		mmc_remove(host);

		mmc_claim_host(host);
		mmc_detach_bus(host);
		mmc_power_off(host);
		mmc_release_host(host);
	}
}

/*
 * Suspend callback from host.
 */

#define LINUX_34_DEBUG   (1)
static int mmc_suspend(struct mmc_host *host)
{
	int err = 0;

	BUG_ON(!host);
	BUG_ON(!host->card);

	mmc_claim_host(host);

	err = mmc_cache_ctrl(host, 0);
	if (err)
		goto out;

	if (mmc_can_poweroff_notify(host->card))
		err = mmc_poweroff_notify(host->card, EXT_CSD_POWER_OFF_SHORT);
#if (1 == LINUX_34_DEBUG)
	else if (mmc_card_can_sleep(host) && mmc_card_keep_power(host)) {
		err = mmc_card_sleep(host);
		if (!err)
			mmc_card_set_sleep(host->card);
	} 

#else
	else if (mmc_card_can_sleep(host) && mmc_card_keep_power(host))
		err = mmc_card_sleep(host);
#endif
	else if (!mmc_host_is_spi(host))
		err = mmc_deselect_cards(host);
		
#ifdef CONFIG_EMMC_50_FEATURE
	host->card->state &= ~(MMC_STATE_HIGHSPEED | MMC_STATE_HIGHSPEED_200 | MMC_STATE_HIGHSPEED_400);
#else
	host->card->state &= ~(MMC_STATE_HIGHSPEED | MMC_STATE_HIGHSPEED_200);
#endif

out:
	mmc_release_host(host);
	return err;
}

/*
 * Resume callback from host.
 *
 * This function tries to determine if the same card is still present
 * and, if so, restore all state to it.
 */
static int mmc_resume(struct mmc_host *host)
{
	int err;

	BUG_ON(!host);
	BUG_ON(!host->card);

#if (1 == LINUX_34_DEBUG)

	mmc_claim_host(host);
	if (mmc_card_is_sleep(host->card)) {
		err = mmc_card_awake(host);
		mmc_card_clr_sleep(host->card);
	} else
		err = mmc_init_card(host, host->ocr, host->card);

#ifdef CONFIG_MTK_EMMC_CACHE
    //do enable the cache feature when eMMC is resumed by wake up. 
    if(!err)
	    mmc_cache_ctrl(host, 1);
#endif

	mmc_release_host(host);

#else

	mmc_claim_host(host);
	err = mmc_init_card(host, host->ocr, host->card);
	mmc_release_host(host);

#endif
	/* 
	 * emmc resume fail is a critical issue, and kernel info should better be dump out
	 */
	if(err){
		printk(KERN_ERR "[%s]: fatal error, emmc resume failed, err=%d\n", __func__, err);
		BUG_ON(err);
	}
	
	return err;
}

static int mmc_power_restore(struct mmc_host *host)
{
	int ret;
	
#ifdef CONFIG_EMMC_50_FEATURE
	host->card->state &= ~(MMC_STATE_HIGHSPEED | MMC_STATE_HIGHSPEED_200 | MMC_STATE_HIGHSPEED_400);
#else
	host->card->state &= ~(MMC_STATE_HIGHSPEED | MMC_STATE_HIGHSPEED_200);
#endif
	mmc_claim_host(host);
	ret = mmc_init_card(host, host->ocr, host->card);
	mmc_release_host(host);

	return ret;
}

static int mmc_sleep(struct mmc_host *host)
{
	struct mmc_card *card = host->card;
	int err = -ENOSYS;

	if (card && card->ext_csd.rev >= 3) {
		err = mmc_card_sleepawake(host, 1);
		if (err < 0)
			pr_debug("%s: Error %d while putting card into sleep",
				 mmc_hostname(host), err);
	}

	return err;
}

static int mmc_awake(struct mmc_host *host)
{
	struct mmc_card *card = host->card;
	int err = -ENOSYS;

	if (card && card->ext_csd.rev >= 3) {
		err = mmc_card_sleepawake(host, 0);
		if (err < 0)
			pr_debug("%s: Error %d while awaking sleeping card",
				 mmc_hostname(host), err);
	}

	return err;
}

static const struct mmc_bus_ops mmc_ops = {
	.awake = mmc_awake,
	.sleep = mmc_sleep,
	.remove = mmc_remove,
	.detect = mmc_detect,
	.suspend = NULL,
	.resume = NULL,
	.power_restore = mmc_power_restore,
	.alive = mmc_alive,
};

static const struct mmc_bus_ops mmc_ops_unsafe = {
	.awake = mmc_awake,
	.sleep = mmc_sleep,
	.remove = mmc_remove,
	.detect = mmc_detect,
	.suspend = mmc_suspend,
	.resume = mmc_resume,
	.power_restore = mmc_power_restore,
	.alive = mmc_alive,
};

static void mmc_attach_bus_ops(struct mmc_host *host)
{
	const struct mmc_bus_ops *bus_ops;

	if (!mmc_card_is_removable(host))
		bus_ops = &mmc_ops_unsafe;
	else
		bus_ops = &mmc_ops;
	mmc_attach_bus(host, bus_ops);
}

/*
 * Starting point for MMC card init.
 */
int mmc_attach_mmc(struct mmc_host *host)
{
	int err;
	int err_pon;
	u32 ocr;

	BUG_ON(!host);
	WARN_ON(!host->claimed);

	/* Set correct bus mode for MMC before attempting attach */
	if (!mmc_host_is_spi(host))
		mmc_set_bus_mode(host, MMC_BUSMODE_OPENDRAIN);

	err = mmc_send_op_cond(host, 0, &ocr);
	if (err)
		return err;

	mmc_attach_bus_ops(host);
	if (host->ocr_avail_mmc)
		host->ocr_avail = host->ocr_avail_mmc;

	/*
	 * We need to get OCR a different way for SPI.
	 */
	if (mmc_host_is_spi(host)) {
		err = mmc_spi_read_ocr(host, 1, &ocr);
		if (err)
			goto err;
	}

	/*
	 * Sanity check the voltages that the card claims to
	 * support.
	 */
	if (ocr & 0x7F) {
		pr_warning("%s: card claims to support voltages "
		       "below the defined range. These will be ignored.\n",
		       mmc_hostname(host));
		ocr &= ~0x7F;
	}

	host->ocr = mmc_select_voltage(host, ocr);

	/*
	 * Can we support the voltage of the card?
	 */
	if (!host->ocr) {
		err = -EINVAL;
		goto err;
	}

	/*
	 * Detect and init the card.
	 */
	err = mmc_init_card(host, host->ocr, NULL);
	if (err)
		goto err;

#if (defined(CONFIG_AMAZON_METRICS_LOG) && defined(ENABLE_SAMSUNG_EMMC_METRICS))
	metrics_delaywork_queue(host);
#endif /* CONFIG_AMAZON_METRICS_LOG */

	mmc_release_host(host);
	err = mmc_add_card(host->card);
 
	if ((host->caps2 & MMC_CAP2_POWEROFF_NOTIFY) && (host->card->ext_csd.rev >= 6) && (host->card->quirks & MMC_QUIRK_PON))
	{
		if (host->card->ext_csd.rev >= 6) {
			err_pon = mmc_switch(host->card, EXT_CSD_CMD_SET_NORMAL,
					 EXT_CSD_POWER_OFF_NOTIFICATION,
					 EXT_CSD_POWER_ON,
					 host->card->ext_csd.generic_cmd6_time);
			if (err_pon && err_pon != -EBADMSG)
				printk(KERN_ERR "mmc_switch error %d",err_pon);

			if (!err_pon)
			{
				host->card->ext_csd.power_off_notification = EXT_CSD_POWER_ON;
			}
		}
		
	}

	mmc_claim_host(host);

#ifdef CONFIG_MTK_EMMC_SUPPORT
	host->card_init_complete(host);
#endif

	if (err)
		goto remove_card;

	return 0;

remove_card:
	mmc_release_host(host);
	mmc_remove_card(host->card);
	mmc_claim_host(host);
	host->card = NULL;
err:
	mmc_detach_bus(host);

	pr_err("%s: error %d whilst initialising MMC card\n",
		mmc_hostname(host), err);

	return err;
}
