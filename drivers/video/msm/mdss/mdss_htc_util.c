/* Copyright (c) 2009-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/debugfs.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/debug_display.h>
#include "mdss_htc_util.h"
#include "mdss_dsi.h"
#include "mdss_mdp.h"

struct attribute_status htc_attr_status[] = {
	{"cabc_level_ctl", 1, 1, 1},
	{"mdss_pp_hue", 0, 0, 0},
	{"pp_pcc", 0, 0, 0},
	{"sre_level", 0, 0, 0},
	{"limit_brightness", MDSS_MAX_BL_BRIGHTNESS, MDSS_MAX_BL_BRIGHTNESS, MDSS_MAX_BL_BRIGHTNESS},
};

int dspp_pcc_mode_cnt;
static struct mdss_dspp_pcc_mode *dspp_pcc_mode;

static struct delayed_work dimming_work;

struct msm_fb_data_type *mfd_instance;
#define DEBUG_BUF   2048
#define MIN_COUNT   9
#define DCS_MAX_CNT   128

static char debug_buf[DEBUG_BUF];
struct mdss_dsi_ctrl_pdata *ctrl_instance = NULL;
static char dcs_cmds[DCS_MAX_CNT];
static char *tmp;
static struct dsi_cmd_desc debug_cmd = {
	{DTYPE_DCS_LWRITE, 1, 0, 0, 1, 1}, dcs_cmds
};
static char dsi_rbuf[4];
static void dsi_read_cb(int len)
{
	unsigned *lp;

	lp = (uint32_t *)dsi_rbuf;
	PR_DISP_INFO("%s: data=0x%x len=%d\n", __func__,*lp, len);
}
static ssize_t dsi_cmd_write(
	struct file *file,
	const char __user *buff,
	size_t count,
	loff_t *ppos)
{
	u32 type = 0, value = 0;
	int cnt, i;
	struct dcs_cmd_req cmdreq;

	if (count >= sizeof(debug_buf) || count < MIN_COUNT)
		return -EFAULT;

	if (copy_from_user(debug_buf, buff, count))
		return -EFAULT;

	if (!ctrl_instance)
		return count;

	
	debug_buf[count] = 0;

	
	cnt = (count) / 3 - 1;
	debug_cmd.dchdr.dlen = cnt;

	
	sscanf(debug_buf, "%x", &type);

	if (type == DTYPE_DCS_LWRITE)
		debug_cmd.dchdr.dtype = DTYPE_DCS_LWRITE;
	else if (type == DTYPE_GEN_LWRITE)
		debug_cmd.dchdr.dtype = DTYPE_GEN_LWRITE;
	else if (type == DTYPE_DCS_READ)
		debug_cmd.dchdr.dtype = DTYPE_DCS_READ;
	else
		return -EFAULT;

	PR_DISP_INFO("%s: cnt=%d, type=0x%x\n", __func__, cnt, type);

	
	for (i = 0; i < cnt; i++) {
		if (i >= DCS_MAX_CNT) {
			PR_DISP_INFO("%s: DCS command count over DCS_MAX_CNT, Skip these commands.\n", __func__);
			break;
		}
		tmp = debug_buf + (3 * (i + 1));
		sscanf(tmp, "%x", &value);
		dcs_cmds[i] = value;
		PR_DISP_INFO("%s: value=0x%x\n", __func__, dcs_cmds[i]);
	}

	memset(&cmdreq, 0, sizeof(cmdreq));
	memset(&dsi_rbuf, 0, sizeof(dsi_rbuf));

	if (type == DTYPE_DCS_READ){
		cmdreq.cmds = &debug_cmd;
		cmdreq.cmds_cnt = 1;
		cmdreq.flags = CMD_REQ_COMMIT | CMD_REQ_RX;
		cmdreq.rlen = 4;
		cmdreq.rbuf = dsi_rbuf;
		cmdreq.cb = dsi_read_cb; 

		mdss_dsi_cmdlist_put(ctrl_instance, &cmdreq);
	} else {
		cmdreq.cmds = &debug_cmd;
		cmdreq.cmds_cnt = 1;
		cmdreq.flags = CMD_REQ_COMMIT | CMD_CLK_CTRL;
		cmdreq.rlen = 0;
		cmdreq.cb = NULL;
		mdss_dsi_cmdlist_put(ctrl_instance, &cmdreq);
		PR_DISP_INFO("%s %ld\n", __func__, count);
	}
	return count;
}

static const struct file_operations dsi_cmd_fops = {
	.write = dsi_cmd_write,
};

static struct kobject *android_disp_kobj;
static char panel_name[MDSS_MAX_PANEL_LEN] = {0};
static ssize_t disp_vendor_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t ret = 0;
	ret = snprintf(buf, PAGE_SIZE, "%s\n", panel_name);
	return ret;
}
static DEVICE_ATTR(vendor, S_IRUGO, disp_vendor_show, NULL);

char *disp_vendor(void){
	return panel_name;
}
EXPORT_SYMBOL(disp_vendor);

void htc_panel_info(const char *panel)
{
	android_disp_kobj = kobject_create_and_add("android_display", NULL);
	if (!android_disp_kobj) {
		PR_DISP_ERR("%s: subsystem register failed\n", __func__);
		return ;
	}
	if (sysfs_create_file(android_disp_kobj, &dev_attr_vendor.attr)) {
		PR_DISP_ERR("Fail to create sysfs file (vendor)\n");
		return ;
	}
	strlcpy(panel_name, panel, MDSS_MAX_PANEL_LEN);
}

void htc_debugfs_init(struct msm_fb_data_type *mfd)
{
	struct mdss_panel_data *pdata;
	struct dentry *dent = debugfs_create_dir("htc_debug", NULL);

	PR_DISP_INFO("%s\n", __func__);

	pdata = dev_get_platdata(&mfd->pdev->dev);

	ctrl_instance = container_of(pdata, struct mdss_dsi_ctrl_pdata,
						panel_data);

	if (IS_ERR(dent)) {
		pr_err(KERN_ERR "%s(%d): debugfs_create_dir fail, error %ld\n",
			__FILE__, __LINE__, PTR_ERR(dent));
		return;
	}

	if (debugfs_create_file("dsi_cmd", 0644, dent, 0, &dsi_cmd_fops)
			== NULL) {
		pr_err(KERN_ERR "%s(%d): debugfs_create_file: index fail\n",
			__FILE__, __LINE__);
		return;
	}
	return;
}

static unsigned backlightvalue = 0;
static unsigned dua_backlightvalue = 0;
static ssize_t camera_bl_show(struct device *dev,
        struct device_attribute *attr, char *buf)
{
	ssize_t ret =0;
	ret = scnprintf(buf, PAGE_SIZE, "%s%u\n%s%u\n", "BL_CAM_MIN=", backlightvalue, "BL_CAM_DUA_MIN=", dua_backlightvalue);
	return ret;
}

static ssize_t attrs_show(struct device *dev,
        struct device_attribute *attr, char *buf)
{
	ssize_t ret = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(htc_attr_status); i++) {
		if (!strcmp(attr->attr.name, htc_attr_status[i].title)) {
			ret = scnprintf(buf, PAGE_SIZE, "%d\n", htc_attr_status[i].cur_value);
			break;
		}
	}

	return ret;
}

static ssize_t attr_store(struct device *dev, struct device_attribute *attr,
        const char *buf, size_t count)
{
	unsigned long res;
	int rc, i;

	rc = strict_strtoul(buf, 10, &res);
	if (rc) {
		pr_err("invalid parameter, %s %d\n", buf, rc);
		count = -EINVAL;
		goto err_out;
	}

	for (i = 0; i < ARRAY_SIZE(htc_attr_status); i++) {
		if (!strcmp(attr->attr.name, htc_attr_status[i].title)) {
			htc_attr_status[i].req_value = res;
			break;
		}
	}

err_out:
	return count;
}


#define SLEEPMS_OFFSET(strlen) (strlen+1) 
#define CMDLEN_OFFSET(strlen)  (SLEEPMS_OFFSET(strlen)+sizeof(const __be32))
#define CMD_OFFSET(strlen)     (CMDLEN_OFFSET(strlen)+sizeof(const __be32))

static struct __dsi_cmd_map{
	char *cmdtype_str;
	int  cmdtype_strlen;
	int  dtype;
} dsi_cmd_map[] = {
	{ "DTYPE_DCS_WRITE", 0, DTYPE_DCS_WRITE },
	{ "DTYPE_DCS_WRITE1", 0, DTYPE_DCS_WRITE1 },
	{ "DTYPE_DCS_LWRITE", 0, DTYPE_DCS_LWRITE },
	{ "DTYPE_GEN_WRITE", 0, DTYPE_GEN_WRITE },
	{ "DTYPE_GEN_WRITE1", 0, DTYPE_GEN_WRITE1 },
	{ "DTYPE_GEN_WRITE2", 0, DTYPE_GEN_WRITE2 },
	{ "DTYPE_GEN_LWRITE", 0, DTYPE_GEN_LWRITE },
	{ NULL, 0, 0 }
};

int htc_mdss_dsi_parse_dcs_cmds(struct device_node *np,
		struct dsi_panel_cmds *pcmds, char *cmd_key, char *link_key)
{
	const char *data;
	int blen = 0, len = 0;
	char *buf;
	struct property *prop;
	struct dsi_ctrl_hdr *pdchdr;
	int i, cnt;
	int curcmdtype;

	i = 0;
	while (dsi_cmd_map[i].cmdtype_str) {
		if (!dsi_cmd_map[i].cmdtype_strlen) {
			dsi_cmd_map[i].cmdtype_strlen = strlen(dsi_cmd_map[i].cmdtype_str);
		}
		i++;
	}

	prop = of_find_property( np, cmd_key, &len);
	if (!prop || !len || !(prop->length) || !(prop->value)) {
		PR_DISP_ERR("%s: failed, key=%s  [%d : %d : %p]\n", __func__, cmd_key,
			len, (prop ? prop->length : -1), (prop ? prop->value : 0) );
		
		return -ENOMEM;
	}

	data = prop->value;
	blen = 0;
	cnt = 0;
	while (len > 0) {
		curcmdtype = 0;
		while (dsi_cmd_map[curcmdtype].cmdtype_strlen) {
			if( !strncmp( data, dsi_cmd_map[curcmdtype].cmdtype_str,
						dsi_cmd_map[curcmdtype].cmdtype_strlen ) &&
				data[dsi_cmd_map[curcmdtype].cmdtype_strlen] == '\0' )
				break;
			curcmdtype++;
		};
		if( !dsi_cmd_map[curcmdtype].cmdtype_strlen ) 
			break;

		i = be32_to_cpup((__be32 *)&data[CMDLEN_OFFSET(dsi_cmd_map[curcmdtype].cmdtype_strlen)]);
		blen += i;
		cnt++;

		data = data + CMD_OFFSET(dsi_cmd_map[curcmdtype].cmdtype_strlen) + i;
		len = len - CMD_OFFSET(dsi_cmd_map[curcmdtype].cmdtype_strlen) - i;
	}

	if(len || !cnt || !blen){
		PR_DISP_ERR("%s: failed, key[%s] : %d cmds, remain=%d bytes \n", __func__, cmd_key, cnt, len);
		return -ENOMEM;
	}

	i = (sizeof(char)*blen+sizeof(struct dsi_ctrl_hdr)*cnt);
	buf = kzalloc( i, GFP_KERNEL);
	if (!buf){
		PR_DISP_ERR("%s: create dsi ctrl oom failed \n", __func__);
		return -ENOMEM;
	}

	pcmds->cmds = kzalloc(cnt * sizeof(struct dsi_cmd_desc), GFP_KERNEL);
	if (!pcmds->cmds){
		PR_DISP_ERR("%s: create dsi commands oom failed \n", __func__);
		goto exit_free;
	}

	pcmds->cmd_cnt = cnt;
	pcmds->buf = buf;
	pcmds->blen = i;
	data = prop->value;
	for(i=0; i<cnt; i++){
		pdchdr = &pcmds->cmds[i].dchdr;

		curcmdtype = 0;
		while(dsi_cmd_map[curcmdtype].cmdtype_strlen){
			if( !strncmp( data, dsi_cmd_map[curcmdtype].cmdtype_str,
						dsi_cmd_map[curcmdtype].cmdtype_strlen ) &&
				data[dsi_cmd_map[curcmdtype].cmdtype_strlen] == '\0' ){
				pdchdr->dtype = dsi_cmd_map[curcmdtype].dtype;
				break;
			}
			curcmdtype ++;
		}

		pdchdr->last = 0x01;
		pdchdr->vc = 0x00;
		pdchdr->ack = 0x00;
		pdchdr->wait = be32_to_cpup((__be32 *)&data[SLEEPMS_OFFSET(dsi_cmd_map[curcmdtype].cmdtype_strlen)]) & 0xff;
		pdchdr->dlen = be32_to_cpup((__be32 *)&data[CMDLEN_OFFSET(dsi_cmd_map[curcmdtype].cmdtype_strlen)]);
		memcpy( buf, pdchdr, sizeof(struct dsi_ctrl_hdr) );
		buf += sizeof(struct dsi_ctrl_hdr);
		memcpy( buf, &data[CMD_OFFSET(dsi_cmd_map[curcmdtype].cmdtype_strlen)], pdchdr->dlen);
		pcmds->cmds[i].payload = buf;
		buf += pdchdr->dlen;
		data = data + CMD_OFFSET(dsi_cmd_map[curcmdtype].cmdtype_strlen) + pdchdr->dlen;
	}

	data = of_get_property(np, link_key, NULL);
	if (data) {
		if (!strncmp(data, "dsi_hs_mode", 11))
			pcmds->link_state = DSI_HS_MODE;
		else
			pcmds->link_state = DSI_LP_MODE;
	} else {
		pcmds->link_state = DSI_HS_MODE;
	}
	pr_debug("%s: dcs_cmd=%x len=%d, cmd_cnt=%d link_state=%d\n", __func__,
		pcmds->buf[0], pcmds->blen, pcmds->cmd_cnt, pcmds->link_state);

	return 0;

exit_free:
	kfree(buf);
	return -ENOMEM;
}

int mdss_mdp_parse_dt_dspp_pcc_setting(struct platform_device *pdev)
{
	struct mdss_dspp_pcc_config *pcc;
	struct device_node *of_node = NULL, *pcc_node = NULL;
	const u32 *pp_pcc_arr;
	u32 pp_pcc_config_len = 0;
	int mode_index = 0, rc = 0;
	char pcc_str[] = "htc,mdss-pp-pcc-settings";
	int pcc_str_size = strlen(pcc_str);

	of_node = pdev->dev.of_node;
	dspp_pcc_mode_cnt = 0;

	for_each_child_of_node(of_node, pcc_node) {
		if (!strncmp(pcc_node->name, pcc_str, pcc_str_size))
			++dspp_pcc_mode_cnt;
	}
	if (dspp_pcc_mode_cnt == 0) {
		pr_debug("%s: no htc,mdss-pp-pcc-settings node\n", __func__);
		return 0;
	} else {
		pr_info("%s: dspp_pcc_setting found. count=%d\n", __func__, dspp_pcc_mode_cnt);
	}
	dspp_pcc_mode = devm_kzalloc(&pdev->dev, sizeof(*dspp_pcc_mode) * dspp_pcc_mode_cnt, GFP_KERNEL);

	if(!dspp_pcc_mode)
		return -ENOMEM;

	for_each_child_of_node(of_node, pcc_node) {
		if (!strncmp(pcc_node->name, pcc_str, pcc_str_size)) {
			const char *st = NULL;
			int i = 0;

			dspp_pcc_mode[mode_index].dspp_pcc_config_cnt = 0;
			
			rc = of_property_read_string(pcc_node,
				"htc,pcc-mode", &st);
			if (rc) {
				pr_err("%s: error reading name. rc=%d, skip this node\n",
					__func__, rc);
				dspp_pcc_mode_cnt--;
				continue;
			}

			scnprintf(dspp_pcc_mode[mode_index].mode_name,
				ARRAY_SIZE(dspp_pcc_mode[mode_index].mode_name),
				"%s", st);

			
			of_property_read_u32(pcc_node, "htc,pcc-enable", &dspp_pcc_mode[mode_index].pcc_enable);

			
			pp_pcc_arr = of_get_property(pcc_node, "htc,pcc-configs", &pp_pcc_config_len);

			if (!pp_pcc_arr) {
				pr_debug("%s: Not found htc,pcc-configs node\n", __func__);
				pp_pcc_config_len = 0;
			} else if (pp_pcc_config_len % (2 * sizeof(u32))) {
				pr_err("%s: htc,pcc-configs property size error. size=%d\n", __func__, pp_pcc_config_len);
				pp_pcc_config_len = 0;
			}

			pp_pcc_config_len /= 2 * sizeof(u32);
			if (pp_pcc_config_len) {
				
				dspp_pcc_mode[mode_index].dspp_pcc_config = devm_kzalloc(&pdev->dev,
					sizeof(*dspp_pcc_mode[mode_index].dspp_pcc_config) * pp_pcc_config_len, GFP_KERNEL);
				pcc = dspp_pcc_mode[mode_index].dspp_pcc_config;
				if (!pcc)
					return -ENOMEM;

				for (i = 0; i < pp_pcc_config_len * 2; i += 2) {
					pcc->reg_offset = be32_to_cpu(pp_pcc_arr[i]);
					pcc->val = be32_to_cpu(pp_pcc_arr[i + 1]);
					pr_debug("reg: 0x%04x=0x%08x\n", pcc->reg_offset, pcc->val);
					pcc++;
				}
				dspp_pcc_mode[mode_index].dspp_pcc_config_cnt = pp_pcc_config_len;
			}
			++mode_index;
		}
	}
	return 0;
}

static ssize_t pcc_attrs_show(struct device *dev,
        struct device_attribute *attr, char *buf)
{
	ssize_t ret = 0;
	u32 cur_val = htc_attr_status[PP_PCC_INDEX].cur_value;

	if (cur_val < dspp_pcc_mode_cnt)
		ret = scnprintf(buf, MAX_MODE_NAME_SIZE, "%s\n", dspp_pcc_mode[cur_val].mode_name);

	return ret;
}

static ssize_t pcc_attr_store(struct device *dev, struct device_attribute *attr,
        const char *buf, size_t count)
{
	int i, name_size;

	for (i = 0; i < dspp_pcc_mode_cnt; i++) {
		name_size = strlen(dspp_pcc_mode[i].mode_name);
		if (!strncmp(buf, dspp_pcc_mode[i].mode_name, name_size)) {
			htc_attr_status[PP_PCC_INDEX].req_value = i;
			break;
		}
	}
	return count;
}

static DEVICE_ATTR(backlight_info, S_IRUGO, camera_bl_show, NULL);
static DEVICE_ATTR(cabc_level_ctl, S_IRUGO | S_IWUSR, attrs_show, attr_store);
static DEVICE_ATTR(mdss_pp_hue, S_IRUGO | S_IWUSR, attrs_show, attr_store);
static DEVICE_ATTR(pp_pcc, S_IRUGO | S_IWUSR, pcc_attrs_show, pcc_attr_store);
static DEVICE_ATTR(sre_level, S_IRUGO | S_IWUSR, attrs_show, attr_store);
static DEVICE_ATTR(limit_brightness, S_IRUGO | S_IWUSR, attrs_show, attr_store);
static struct attribute *htc_extend_attrs[] = {
	&dev_attr_backlight_info.attr,
	&dev_attr_cabc_level_ctl.attr,
	&dev_attr_mdss_pp_hue.attr,
	&dev_attr_sre_level.attr,
	&dev_attr_limit_brightness.attr,
	NULL,
};

static struct attribute_group htc_extend_attr_group = {
	.attrs = htc_extend_attrs,
};

void htc_register_attrs(struct kobject *led_kobj, struct msm_fb_data_type *mfd)
{
	int rc;
	struct mdss_panel_info *panel_info = mfd->panel_info;

	rc = sysfs_create_group(led_kobj, &htc_extend_attr_group);
	if (rc)
		pr_err("sysfs group creation failed, rc=%d\n", rc);

	if (dspp_pcc_mode_cnt > 0) {
		rc = sysfs_create_file(&mfd->fbi->dev->kobj, &dev_attr_pp_pcc.attr);
		if (rc)
			pr_err("sysfs creation pp_pcc failed, rc=%d\n", rc);
	}
	
	htc_attr_status[HUE_INDEX].req_value = panel_info->mdss_pp_hue;

	
	htc_attr_status[PP_PCC_INDEX].req_value = 0;

	return;
}

void htc_reset_status(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(htc_attr_status); i++) {
		htc_attr_status[i].cur_value = htc_attr_status[i].def_value;
	}

	return;
}

void htc_register_camera_bkl(int level, int dua_level)
{
	backlightvalue = level;
	dua_backlightvalue = dua_level;
}

void htc_set_cabc(struct msm_fb_data_type *mfd, bool skip_check)
{
	struct mdss_panel_data *pdata;
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;
	struct dcs_cmd_req cmdreq;

	pdata = dev_get_platdata(&mfd->pdev->dev);

	ctrl_pdata = container_of(pdata, struct mdss_dsi_ctrl_pdata,
					panel_data);

	if (htc_attr_status[CABC_INDEX].req_value > 2)
		return;

	if (!ctrl_pdata->cabc_off_cmds.cmds)
		return;

	if (!ctrl_pdata->cabc_ui_cmds.cmds)
		return;

	if (!ctrl_pdata->cabc_video_cmds.cmds)
		return;

	if (!skip_check && (htc_attr_status[CABC_INDEX].req_value == htc_attr_status[CABC_INDEX].cur_value))
		return;

	memset(&cmdreq, 0, sizeof(cmdreq));

	if (htc_attr_status[CABC_INDEX].req_value == 0) {
		cmdreq.cmds = ctrl_pdata->cabc_off_cmds.cmds;
		cmdreq.cmds_cnt = ctrl_pdata->cabc_off_cmds.cmd_cnt;
	} else if (htc_attr_status[CABC_INDEX].req_value == 1) {
		cmdreq.cmds = ctrl_pdata->cabc_ui_cmds.cmds;
		cmdreq.cmds_cnt = ctrl_pdata->cabc_ui_cmds.cmd_cnt;
	} else if (htc_attr_status[CABC_INDEX].req_value == 2) {
		cmdreq.cmds = ctrl_pdata->cabc_video_cmds.cmds;
		cmdreq.cmds_cnt = ctrl_pdata->cabc_video_cmds.cmd_cnt;
	} else {
		cmdreq.cmds = ctrl_pdata->cabc_ui_cmds.cmds;
		cmdreq.cmds_cnt = ctrl_pdata->cabc_ui_cmds.cmd_cnt;
	}

	cmdreq.flags = CMD_REQ_COMMIT | CMD_CLK_CTRL;
	cmdreq.rlen = 0;
	cmdreq.cb = NULL;

	mdss_dsi_cmdlist_put(ctrl_pdata, &cmdreq);

	htc_attr_status[CABC_INDEX].cur_value = htc_attr_status[CABC_INDEX].req_value;
	PR_DISP_INFO("%s cabc mode=%d\n", __func__, htc_attr_status[CABC_INDEX].cur_value);
	return;
}

bool htc_set_sre(struct msm_fb_data_type *mfd)
{
	struct mdss_panel_data *pdata;
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;
	struct dcs_cmd_req cmdreq;
	static bool sre_state = false;

	pdata = dev_get_platdata(&mfd->pdev->dev);

	ctrl_pdata = container_of(pdata, struct mdss_dsi_ctrl_pdata,
					panel_data);

	if (!ctrl_pdata->sre_off_cmds.cmds)
		return false;

	if (!ctrl_pdata->sre_on_cmds.cmds)
		return false;

	if (!ctrl_pdata->sre_ebi_value)
		return false;

	if (htc_attr_status[SRE_INDEX].req_value == htc_attr_status[SRE_INDEX].cur_value)
		return false;

	if (sre_state && htc_attr_status[SRE_INDEX].req_value < ctrl_pdata->sre_ebi_value) {
		sre_state = false;
	} else if (!sre_state && htc_attr_status[SRE_INDEX].req_value >= ctrl_pdata->sre_ebi_value) {
		sre_state = true;
	} else {
		htc_attr_status[SRE_INDEX].cur_value = htc_attr_status[SRE_INDEX].req_value;
		return false;
	}

	memset(&cmdreq, 0, sizeof(cmdreq));

	
	if (!sre_state) {
		cmdreq.cmds = ctrl_pdata->sre_off_cmds.cmds;
		cmdreq.cmds_cnt = ctrl_pdata->sre_off_cmds.cmd_cnt;
	} else {
		cmdreq.cmds = ctrl_pdata->sre_on_cmds.cmds;
		cmdreq.cmds_cnt = ctrl_pdata->sre_on_cmds.cmd_cnt;
	}

	cmdreq.flags = CMD_REQ_COMMIT | CMD_CLK_CTRL;
	cmdreq.rlen = 0;
	cmdreq.cb = NULL;

	mdss_dsi_cmdlist_put(ctrl_pdata, &cmdreq);

	htc_attr_status[SRE_INDEX].cur_value = htc_attr_status[SRE_INDEX].req_value;
	PR_DISP_INFO("%s sre mode=%d\n", __func__, htc_attr_status[SRE_INDEX].cur_value);
	return true;
}

void htc_set_limit_brightness(struct msm_fb_data_type *mfd)
{
	struct mdss_panel_info *panel_info = mfd->panel_info;
	int bl_lvl = 0;
	int nits_lvl = 0;
	if (htc_attr_status[LIM_BRI_INDEX].req_value > MDSS_MAX_BL_BRIGHTNESS)
		return;

	if (htc_attr_status[LIM_BRI_INDEX].req_value == htc_attr_status[LIM_BRI_INDEX].cur_value)
		return;

	panel_info->brightness_max = htc_attr_status[LIM_BRI_INDEX].req_value;

	bl_lvl = htc_backlight_transfer_bl_brightness(panel_info->brightness_max, panel_info, true);
	if (bl_lvl < 0) {
		MDSS_BRIGHT_TO_BL(bl_lvl, panel_info->brightness_max, mfd->panel_info->bl_max,
							MDSS_MAX_BL_BRIGHTNESS);
	}

	nits_lvl = htc_backlight_bl_to_nits(bl_lvl, panel_info);
	if (nits_lvl < 0) {
		
		panel_info->nits_bl_table.max_nits = 0;
	} else {
		panel_info->nits_bl_table.max_nits = nits_lvl;
	}

	if (bl_lvl < mfd->bl_level) {
		
		mfd->bl_updated = false;
		mfd->unset_bl_level = bl_lvl;
	}

	htc_attr_status[LIM_BRI_INDEX].cur_value = htc_attr_status[LIM_BRI_INDEX].req_value;
	PR_DISP_INFO("%s limit_brightness = %d, nits = %d\n", __func__, panel_info->brightness_max, panel_info->nits_bl_table.max_nits);
	return;
}

static void dimming_do_work(struct work_struct *work)
{
	struct mdss_panel_data *pdata;
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;
	struct dcs_cmd_req cmdreq;

	pdata = dev_get_platdata(&mfd_instance->pdev->dev);

	ctrl_pdata = container_of(pdata, struct mdss_dsi_ctrl_pdata,
					panel_data);

	memset(&cmdreq, 0, sizeof(cmdreq));

	cmdreq.cmds = ctrl_pdata->dimming_on_cmds.cmds;
	cmdreq.cmds_cnt = ctrl_pdata->dimming_on_cmds.cmd_cnt;
	cmdreq.flags = CMD_REQ_COMMIT | CMD_CLK_CTRL;
	cmdreq.rlen = 0;
	cmdreq.cb = NULL;

	mdss_dsi_cmdlist_put(ctrl_pdata, &cmdreq);

	PR_DISP_INFO("dimming on\n");
}

void htc_dimming_on(struct msm_fb_data_type *mfd)
{
	struct mdss_panel_data *pdata;
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;

	pdata = dev_get_platdata(&mfd->pdev->dev);

	ctrl_pdata = container_of(pdata, struct mdss_dsi_ctrl_pdata,
					panel_data);

	if (!ctrl_pdata->dimming_on_cmds.cmds)
		return;

	mfd_instance = mfd;

	INIT_DELAYED_WORK(&dimming_work, dimming_do_work);

	schedule_delayed_work(&dimming_work, msecs_to_jiffies(1000));
	return;
}

void htc_dimming_off(void)
{
	
	cancel_delayed_work_sync(&dimming_work);
}

#define HUE_MAX   4096
void htc_set_pp_pa(struct mdss_mdp_ctl *ctl)
{
	struct mdss_data_type *mdata;
	struct mdss_mdp_mixer *mixer;
	u32 opmode = 0;
	char __iomem *basel;

	
	if (htc_attr_status[HUE_INDEX].req_value == htc_attr_status[HUE_INDEX].cur_value)
		return;

	if (htc_attr_status[HUE_INDEX].req_value >= HUE_MAX)
		return;

	mdata = mdss_mdp_get_mdata();
	mixer = mdata->mixer_intf;

	basel = mixer->dspp_base;

	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON);


	opmode |= (1 << 20); 
	writel_relaxed(opmode, basel + MDSS_MDP_REG_DSPP_OP_MODE);

	ctl->flush_bits |= BIT(13);

	wmb();
	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF);

	htc_attr_status[HUE_INDEX].cur_value = htc_attr_status[HUE_INDEX].req_value;
	PR_DISP_INFO("%s pp_hue = 0x%x\n", __func__, htc_attr_status[HUE_INDEX].req_value);
}

void htc_set_pp_pcc(struct mdss_mdp_ctl *ctl)
{
	struct mdss_data_type *mdata;
	struct mdss_mdp_mixer *mixer;
	struct mdss_dspp_pcc_config *pcc;
	u32 opmode = 0, req_val = 0;
	char __iomem *basel;
	int i;
	req_val = htc_attr_status[PP_PCC_INDEX].req_value;

	
	if (req_val == htc_attr_status[PP_PCC_INDEX].cur_value)
		return;

	if ((req_val >= dspp_pcc_mode_cnt) || (req_val < 0)) {
		pr_err("%s:req_val = %d was invalid\n", __func__, req_val);
		return;
	}

	mdata = mdss_mdp_get_mdata();
	if(!mdata) {
		pr_err("%s:mdss_mdp_get_mdata was NULL\n", __func__);
		return;
	}

	mixer = mdata->mixer_intf;
	if(!mixer) {
		pr_err("%s:mdata->mixer_intf was NULL\n", __func__);
		return;
	}

	basel = mixer->dspp_base;

	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON);

	if(dspp_pcc_mode[req_val].pcc_enable) {
		opmode |= MDSS_MDP_DSPP_OP_PCC_EN; 
		pcc = dspp_pcc_mode[req_val].dspp_pcc_config;
		if (pcc)
			for (i = 0; i < dspp_pcc_mode[req_val].dspp_pcc_config_cnt; i++) {
				pr_debug("%s: pcc->val = %d\n", __func__, pcc->val);
				pcc++;
			}
	} else {
		opmode &= ~MDSS_MDP_DSPP_OP_PCC_EN; 
	}

	writel_relaxed(opmode, basel + MDSS_MDP_REG_DSPP_OP_MODE);

	ctl->flush_bits |= BIT(13);

	wmb();
	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF);

	htc_attr_status[PP_PCC_INDEX].cur_value = req_val;
	PR_DISP_INFO("%s pp_pcc mode = 0x%x\n", __func__, req_val);
}

int htc_backlight_transfer_bl_brightness(int val, struct mdss_panel_info *panel_info, bool brightness_to_bl)
{
	unsigned int result;
	int index = 0;
	u16 *val_table;
	u16 *ret_table;
	struct htc_backlight1_table *brt_bl_table = &panel_info->brt_bl_table;
	int size = brt_bl_table->size;

	
	if(!size || !brt_bl_table->brt_data || !brt_bl_table->bl_data)
		return -ENOENT;

	if (brightness_to_bl) {
		val_table = brt_bl_table->brt_data;
		ret_table = brt_bl_table->bl_data;
	} else {
		val_table = brt_bl_table->bl_data;
		ret_table = brt_bl_table->brt_data;
	}

	if (val <= 0){
		result = 0;
	} else if (val < val_table[0]) {
		
		result = ret_table[0];
	} else if (val >= val_table[size - 1]) {
		
		result = ret_table[size - 1];
	} else {
		
		result = val;
		for(index = 0; index < size - 1; index++){
			if (val >= val_table[index] && val <= val_table[index + 1]) {
				int x0 = val_table[index];
				int y0 = ret_table[index];
				int x1 = val_table[index + 1];
				int y1 = ret_table[index + 1];

				if (x0 == x1)
					result = y0;
				else
					result = y0 + (y1 - y0) * (val - x0) / (x1 - x0);

				break;
			}
		}
	}

	pr_info("%s: mode=%d, %d transfer to %d\n",
		__func__, brightness_to_bl, val, result);

	return result;
}

int htc_backlight_bl_to_nits(int val, struct mdss_panel_info *panel_info)
{
	int index = 0, remainder = 0, max_index = 0;
	unsigned int nits, code1, code2;
	int scale;
	struct htc_backlight2_table *nits_bl_table = &panel_info->nits_bl_table;

	scale = nits_bl_table->scale;
	max_index = nits_bl_table->size;
	if (!scale || !max_index || !nits_bl_table->data)
		return -ENOENT;

	if (val <= 0)
		return 0;


	for (index = 1; index < max_index; index++) {
		if (val <= nits_bl_table->data[index])
			break;
	}

	code1 = nits_bl_table->data[index - 1];
	code2 = nits_bl_table->data[index];
	remainder = (code2 - val) * scale / (code2 - code1);

	nits = index * scale - remainder;

	pr_info("%s: bl=%d, nits=%d", __func__, val, nits);
	return nits;
}

static void print_bl_log_suppressed(int val, int index, unsigned int code, int scale)
{
	bool print_log = false;
	unsigned long flags;
	static unsigned long timeout = 0;
	static int suppress_count = 0;
	static int suppress_nits = 0;
	static int prev_val = 0;
	static DEFINE_SPINLOCK(lock);

	spin_lock_irqsave(&lock, flags);

	
	print_log = time_after(jiffies, timeout);
	print_log |= !prev_val || ((prev_val / scale) != index);
	print_log |= !val;

	if (print_log || !timeout) {
		if (suppress_count) {
			pr_info("%s: nits=%d, bl=%d, suppress %d logs, last value=(%d)", __func__, val, code, suppress_count, suppress_nits);
			suppress_count = 0;
		} else {
			pr_info("%s: nits=%d, bl=%d", __func__, val, code);
		}
		
		timeout = jiffies + msecs_to_jiffies(500);
		prev_val = val;
	} else {
		suppress_nits = val;
		suppress_count++;
		pr_debug("%s: nits=%d, bl=%d \n", __func__, val, code);
	}
	spin_unlock_irqrestore(&lock, flags);
}

int htc_backlight_nits_to_bl(int val, struct mdss_panel_info *panel_info)
{
	int index = 0, remainder = 0, max_index = 0;
	unsigned int code, code1, code2;
	int scale;

	struct htc_backlight2_table *nits_bl_table = &panel_info->nits_bl_table;

	scale = nits_bl_table->scale;
	max_index = nits_bl_table->size;

	if (!scale || !max_index || !nits_bl_table->data)
		return -ENOENT;

	index = val / scale;
	remainder = val % scale;

	
	if (index >= max_index) {
		index = max_index;
		remainder = 0;
	}

	if (remainder != 0 ) {
		code1 = nits_bl_table->data[index];
		code2 = nits_bl_table->data[index + 1];

		
		code = (code2 - code1) * remainder / scale + code1;
	} else {
		
		code = nits_bl_table->data[index];
	}

	print_bl_log_suppressed(val, index, code, scale);

	return code;
}

