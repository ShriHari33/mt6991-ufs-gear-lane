// SPDX-License-Identifier: GPL-2.0
/*
 * gear_lane sysfs attribute for ufs_mtk_config_gear_lane()
 * (02_ufs_mtk_config_gear_lane.c).
 *
 * Where: drivers/ufs/ufs-mediatek-sysfs.c, above
 *        "static DEVICE_ATTR_RW(skip_blocktag);"
 * Then:  add "&dev_attr_gear_lane.attr," to ufs_mtk_sysfs_attrs[] in the
 *        same file. That group is created on hba->dev, so the node lands
 *        next to the other ufshci attributes.
 *
 * Usage: echo "3 1" > /sys/bus/platform/devices/16810000.ufshci/gear_lane
 *        cat /sys/bus/platform/devices/16810000.ufshci/gear_lane
 *
 * Not compiled.
 */

/* ---8<--- paste below this line --- */
static ssize_t gear_lane_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct ufs_hba *hba = dev_get_drvdata(dev);

	return sysfs_emit(buf, "gear rx/tx %u/%u lane rx/tx %u/%u\n",
			  hba->pwr_info.gear_rx, hba->pwr_info.gear_tx,
			  hba->pwr_info.lane_rx, hba->pwr_info.lane_tx);
}

static ssize_t gear_lane_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct ufs_hba *hba = dev_get_drvdata(dev);
	u32 gear, lane;
	int err;

	if (sscanf(buf, "%u %u", &gear, &lane) != 2)
		return -EINVAL;

	err = ufs_mtk_config_gear_lane(hba, gear, lane);

	return err ? err : count;
}

static DEVICE_ATTR_RW(gear_lane);
