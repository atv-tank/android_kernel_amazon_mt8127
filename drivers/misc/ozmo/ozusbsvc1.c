/* -----------------------------------------------------------------------------
 * Copyright (c) 2011 Ozmo Inc
 * Released under the GNU General Public License Version 2 (GPLv2).
 *
 * This file implements the protocol specific parts of the USB service for a PD.
 * -----------------------------------------------------------------------------
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/netdevice.h>
#include <linux/errno.h>
#include <linux/input.h>
#include <asm/unaligned.h>
#include "ozprotocol.h"
#include "ozeltbuf.h"
#include "ozpd.h"
#include "ozproto.h"
#include "ozusbif.h"
#include "ozhcd.h"
#include "oztrace.h"
#include "ozusbsvc.h"
/*------------------------------------------------------------------------------
 */
#define MAX_ISOC_FIXED_DATA	(253-sizeof(struct oz_isoc_fixed))

/*------------------------------------------------------------------------------
 * Context: softirq
 */
static void oz_usb_setup_elt_completion_callback(struct oz_pd *pd, long context)
{
	struct oz_usb_ctx *ctx;
	spin_lock_bh(&pd->app_lock[OZ_APPID_USB-1]);
	ctx = (struct oz_usb_ctx *)pd->app_ctx[OZ_APPID_USB-1];
	if (ctx) {
		u8 req_id = (u8)context;
		oz_hcd_mark_urb_submitted(ctx->hport, 0, req_id);
	}
	spin_unlock_bh(&pd->app_lock[OZ_APPID_USB-1]);
}
/*------------------------------------------------------------------------------
 * Context: softirq
 */
static int oz_usb_submit_elt(struct oz_elt_buf *eb, struct oz_elt_info *ei,
	struct oz_usb_ctx *usb_ctx, u8 strid, u8 isoc)
{
	int ret;
	struct oz_elt *elt = (struct oz_elt *)ei->data;
	struct oz_app_hdr *app_hdr = (struct oz_app_hdr *)(elt+1);
	elt->type = OZ_ELT_APP_DATA;
	ei->app_id = OZ_APPID_USB;
	ei->length = elt->length + sizeof(struct oz_elt);
	app_hdr->app_id = OZ_APPID_USB;
	spin_lock_bh(&eb->lock);
	if (isoc == 0) {
		app_hdr->elt_seq_num = usb_ctx->tx_seq_num++;
		if (usb_ctx->tx_seq_num == 0)
			usb_ctx->tx_seq_num = 1;
	}
	ret = oz_queue_elt_info(eb, isoc, strid, ei);
	if (ret)
		oz_elt_info_free(eb, ei);
	spin_unlock_bh(&eb->lock);
	return ret;
}
/*------------------------------------------------------------------------------
 * Context: softirq
 */
int oz_usb_get_desc_req(void *hpd, u8 req_id, u8 req_type, u8 desc_type,
	u8 index, u16 windex, int offset, int len)
{
	struct oz_usb_ctx *usb_ctx = (struct oz_usb_ctx *)hpd;
	struct oz_pd *pd = usb_ctx->pd;
	struct oz_elt *elt;
	struct oz_get_desc_req *body;
	struct oz_elt_buf *eb = &pd->elt_buff;
	struct oz_elt_info *ei = oz_elt_info_alloc(&pd->elt_buff);
	if (len > 200)
		len = 200;
	if (ei == 0)
		return -1;
	ei->callback = oz_usb_setup_elt_completion_callback;
	ei->context = req_id;
	elt = (struct oz_elt *)ei->data;
	elt->length = sizeof(struct oz_get_desc_req);
	body = (struct oz_get_desc_req *)(elt+1);
	body->type = OZ_GET_DESC_REQ;
	body->req_id = req_id;
	put_unaligned(cpu_to_le16(offset), &body->offset);
	put_unaligned(cpu_to_le16(len), &body->size);
	body->req_type = req_type;
	body->desc_type = desc_type;
	body->w_index = windex;
	body->index = index;
	return oz_usb_submit_elt(eb, ei, usb_ctx, 0, 0);
}
/*------------------------------------------------------------------------------
 * Context: tasklet
 */
static int oz_usb_set_config_req(void *hpd, u8 req_id, u8 index)
{
	struct oz_usb_ctx *usb_ctx = (struct oz_usb_ctx *)hpd;
	struct oz_pd *pd = usb_ctx->pd;
	struct oz_elt *elt;
	struct oz_elt_buf *eb = &pd->elt_buff;
	struct oz_elt_info *ei = oz_elt_info_alloc(&pd->elt_buff);
	struct oz_set_config_req *body;
	if (ei == 0)
		return -1;
	ei->callback = oz_usb_setup_elt_completion_callback;
	ei->context = req_id;
	elt = (struct oz_elt *)ei->data;
	elt->length = sizeof(struct oz_set_config_req);
	body = (struct oz_set_config_req *)(elt+1);
	body->type = OZ_SET_CONFIG_REQ;
	body->req_id = req_id;
	body->index = index;
	return oz_usb_submit_elt(eb, ei, usb_ctx, 0, 0);
}
/*------------------------------------------------------------------------------
 * Context: tasklet
 */
static int oz_usb_set_interface_req(void *hpd, u8 req_id, u8 index, u8 alt)
{
	struct oz_usb_ctx *usb_ctx = (struct oz_usb_ctx *)hpd;
	struct oz_pd *pd = usb_ctx->pd;
	struct oz_elt *elt;
	struct oz_elt_buf *eb = &pd->elt_buff;
	struct oz_elt_info *ei = oz_elt_info_alloc(&pd->elt_buff);
	struct oz_set_interface_req *body;
	if (ei == 0)
		return -1;
	ei->callback = oz_usb_setup_elt_completion_callback;
	ei->context = req_id;
	elt = (struct oz_elt *)ei->data;
	elt->length = sizeof(struct oz_set_interface_req);
	body = (struct oz_set_interface_req *)(elt+1);
	body->type = OZ_SET_INTERFACE_REQ;
	body->req_id = req_id;
	body->index = index;
	body->alternative = alt;
	return oz_usb_submit_elt(eb, ei, usb_ctx, 0, 0);
}
/*------------------------------------------------------------------------------
 * Context: tasklet
 */
static int oz_usb_set_clear_feature_req(void *hpd, u8 req_id, u8 type,
			u8 recipient, u8 index, __le16 feature)
{
	struct oz_usb_ctx *usb_ctx = (struct oz_usb_ctx *)hpd;
	struct oz_pd *pd = usb_ctx->pd;
	struct oz_elt *elt;
	struct oz_elt_buf *eb = &pd->elt_buff;
	struct oz_elt_info *ei = oz_elt_info_alloc(&pd->elt_buff);
	struct oz_feature_req *body;
	if (ei == 0)
		return -1;
	elt = (struct oz_elt *)ei->data;
	ei->callback = oz_usb_setup_elt_completion_callback;
	ei->context = req_id;
	elt->length = sizeof(struct oz_feature_req);
	body = (struct oz_feature_req *)(elt+1);
	body->type = type;
	body->req_id = req_id;
	body->recipient = recipient;
	body->index = index;
	put_unaligned(feature, &body->feature);
	return oz_usb_submit_elt(eb, ei, usb_ctx, 0, 0);
}
/*------------------------------------------------------------------------------
 * Context: tasklet
 */
static int oz_usb_vendor_class_req(void *hpd, u8 req_id, u8 req_type,
	u8 request, __le16 value, __le16 index, const u8 *data, int data_len)
{
	struct oz_usb_ctx *usb_ctx = (struct oz_usb_ctx *)hpd;
	struct oz_pd *pd = usb_ctx->pd;
	struct oz_elt *elt;
	struct oz_elt_buf *eb = &pd->elt_buff;
	struct oz_elt_info *ei = oz_elt_info_alloc(&pd->elt_buff);
	struct oz_vendor_class_req *body;
	if (ei == 0)
		return -1;
	ei->callback = oz_usb_setup_elt_completion_callback;
	ei->context = req_id;
	elt = (struct oz_elt *)ei->data;
	elt->length = sizeof(struct oz_vendor_class_req) - 1 + data_len;
	body = (struct oz_vendor_class_req *)(elt+1);
	body->type = OZ_VENDOR_CLASS_REQ;
	body->req_id = req_id;
	body->req_type = req_type;
	body->request = request;
	put_unaligned(value, &body->value);
	put_unaligned(index, &body->index);
	if (data_len)
		memcpy(body->data, data, data_len);
	return oz_usb_submit_elt(eb, ei, usb_ctx, 0, 0);
}
/*------------------------------------------------------------------------------
 * Context: tasklet
 */
int oz_usb_control_req(void *hpd, u8 req_id, struct usb_ctrlrequest *setup,
			const u8 *data, int data_len)
{
	unsigned wvalue = le16_to_cpu(setup->wValue);
	unsigned windex = le16_to_cpu(setup->wIndex);
	unsigned wlength = le16_to_cpu(setup->wLength);
	int rc = 0;

	if ((setup->bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD) {
		switch (setup->bRequest) {
		case USB_REQ_GET_DESCRIPTOR:
			rc = oz_usb_get_desc_req(hpd, req_id,
				setup->bRequestType, (u8)(wvalue>>8),
				(u8)wvalue, setup->wIndex, 0, wlength);
			break;
		case USB_REQ_SET_CONFIGURATION:
			rc = oz_usb_set_config_req(hpd, req_id, (u8)wvalue);
			break;
		case USB_REQ_SET_INTERFACE: {
				u8 if_num = (u8)windex;
				u8 alt = (u8)wvalue;
				rc = oz_usb_set_interface_req(hpd, req_id,
					if_num, alt);
			}
			break;
		case USB_REQ_SET_FEATURE:
			rc = oz_usb_set_clear_feature_req(hpd, req_id,
				OZ_SET_FEATURE_REQ,
				setup->bRequestType & 0xf, (u8)windex,
				setup->wValue);
			break;
		case USB_REQ_CLEAR_FEATURE:
			rc = oz_usb_set_clear_feature_req(hpd, req_id,
				OZ_CLEAR_FEATURE_REQ,
				setup->bRequestType & 0xf,
				(u8)windex, setup->wValue);
			break;
		}
	} else {
		rc = oz_usb_vendor_class_req(hpd, req_id, setup->bRequestType,
			setup->bRequest, setup->wValue, setup->wIndex,
			data, data_len);
	}
	return rc;
}
/*------------------------------------------------------------------------------
 * Context: softirq
 */
int oz_usb_send_isoc(void *hpd, u8 ep_num, struct urb *urb)
{
	struct oz_usb_ctx *usb_ctx = (struct oz_usb_ctx *)hpd;
	struct oz_pd *pd = usb_ctx->pd;
	struct oz_elt_buf *eb;
	int i;
	int hdr_size;
	u8 *data;
	struct usb_iso_packet_descriptor *desc;

	if (pd->mode & OZ_F_ISOC_NO_ELTS) {
		urb->actual_length = 0;
		for (i = 0; i < urb->number_of_packets; i++) {
			u8 *data;
			desc = &urb->iso_frame_desc[i];
			data = ((u8 *)urb->transfer_buffer)+desc->offset;
			desc->status = 0;
			desc->actual_length = desc->length;
			urb->actual_length += desc->length;
			oz_send_isoc_unit(pd, ep_num, data, desc->length);
		}
		return 0;
	}

	hdr_size = sizeof(struct oz_isoc_fixed) - 1;
	eb = &pd->elt_buff;
	i = 0;
	while (i < urb->number_of_packets) {
		struct oz_elt_info *ei = oz_elt_info_alloc(eb);
		struct oz_elt *elt;
		struct oz_isoc_fixed *body;
		int unit_count;
		int unit_size;
		int rem;
		if (ei == 0)
			return -1;
		rem = MAX_ISOC_FIXED_DATA;
		elt = (struct oz_elt *)ei->data;
		body = (struct oz_isoc_fixed *)(elt + 1);
		body->type = OZ_USB_ENDPOINT_DATA;
		body->endpoint = ep_num;
		body->format = OZ_DATA_F_ISOC_FIXED;
		unit_size = urb->iso_frame_desc[i].length;
		body->unit_size = (u8)unit_size;
		data = ((u8 *)(elt+1)) + hdr_size;
		unit_count = 0;
		while (i < urb->number_of_packets) {
			desc = &urb->iso_frame_desc[i];
			if ((unit_size == desc->length) &&
				(desc->length <= rem)) {
				memcpy(data, ((u8 *)urb->transfer_buffer) +
					desc->offset, unit_size);
				data += unit_size;
				rem -= unit_size;
				unit_count++;
				desc->status = 0;
				desc->actual_length = desc->length;
				i++;
			} else {
				break;
			}
		}
		elt->length = hdr_size + MAX_ISOC_FIXED_DATA - rem;
		/* Store the number of units in body->frame_number for the
		 * moment. This field will be correctly determined before
		 * the element is sent. */
		body->frame_number = (u8)unit_count;
		oz_usb_submit_elt(eb, ei, usb_ctx, ep_num,
			pd->mode & OZ_F_ISOC_ANYTIME);
	}
	return 0;
}

/*------------------------------------------------------------------------------
 * Context: softirq-serialized
 */
void oz_usb_handle_ep_data(struct oz_usb_ctx *usb_ctx,
	struct oz_usb_hdr *usb_hdr, int len)
{
	struct oz_data *data_hdr = (struct oz_data *)usb_hdr;
	switch (data_hdr->format) {
	case OZ_DATA_F_MULTIPLE_FIXED: {
			struct oz_multiple_fixed *body =
				(struct oz_multiple_fixed *)data_hdr;
			u8 *data = body->data;
			unsigned int n;
			if (!body->unit_size ||
				len < sizeof(struct oz_multiple_fixed) - 1)
				break;
			n = (len - (sizeof(struct oz_multiple_fixed) - 1))
				/ body->unit_size;
			while (n--) {
				oz_hcd_data_ind(usb_ctx->hport, body->endpoint,
					data, body->unit_size);
				/*
				 * Get battery level from the last byte of HID report.
				 * abc123 attach the battery level to the end of HID report.
				 * But abc123 sends an independent report for battery level.
				 * Dumping the HID report, the fist byte of reports containing
				 * batter level is 0x01 or 0x03, according to this obersation,
				 * only update battery level when receive such HID report starting
				 * with 0x01 and 0x03. Changes are needed in abc123/abc123 FW side
				 * to exhibite consistent behavior between abc123 and abc123,
				 * attaching the battery level to the end of each HID report.
				 */
				if (((unsigned) *data == 0x01) || ((unsigned) *data == 0x03))
					usb_ctx->pd->battery_level = (unsigned) *(data + body->unit_size - 1);

				data += body->unit_size;
			}
		}
		break;
	case OZ_DATA_F_ISOC_FIXED: {
			struct oz_isoc_fixed *body =
				(struct oz_isoc_fixed *)data_hdr;
			int data_len = len-sizeof(struct oz_isoc_fixed)+1;
			int unit_size = body->unit_size;
			u8 frame_number = body->frame_number;
			u8 *data = body->data;
			int count;
			int i;
			if (!unit_size)
				break;
			count = data_len/unit_size;
			/*
			 * FIXME: I didn't see the code block process HID report.
			 *        should update battery level, if HID report is processed here.
			 */
			for (i = 0; i < count; i++) {
				oz_hcd_isoc_frame(usb_ctx->hport,
					body->endpoint,
					frame_number, data, unit_size);
				data += unit_size;
				frame_number++;
			}
		}
		break;
	}

}
/*------------------------------------------------------------------------------
 * This is called when the PD has received a USB element. The type of element
 * is determined and is then passed to an appropriate handler function.
 * Context: softirq-serialized
 */
void oz_usb_rx(struct oz_pd *pd, struct oz_elt *elt)
{
	struct oz_usb_hdr *usb_hdr = (struct oz_usb_hdr *)(elt + 1);
	struct oz_usb_ctx *usb_ctx;

	spin_lock_bh(&pd->app_lock[OZ_APPID_USB-1]);
	usb_ctx = (struct oz_usb_ctx *)pd->app_ctx[OZ_APPID_USB-1];
	if (usb_ctx)
		oz_usb_get(usb_ctx);
	spin_unlock_bh(&pd->app_lock[OZ_APPID_USB-1]);
	if (usb_ctx == 0)
		return; /* Context has gone so nothing to do. */
	if (usb_ctx->stopped)
		goto done;
	/* If sequence number is non-zero then check it is not a duplicate.
	 * Zero sequence numbers are always accepted.
	 */
	if (usb_hdr->elt_seq_num != 0) {
		if (((usb_ctx->rx_seq_num - usb_hdr->elt_seq_num) & 0x80) == 0)
			/* Reject duplicate element. */
			oz_trace_msg(M, "USB seq overlap %02X %02X\n",
					usb_ctx->rx_seq_num,
					usb_hdr->elt_seq_num);
	}
	usb_ctx->rx_seq_num = usb_hdr->elt_seq_num;
	switch (usb_hdr->type) {
	case OZ_GET_DESC_RSP: {
			struct oz_get_desc_rsp *body =
				(struct oz_get_desc_rsp *)usb_hdr;
			u16 offs, total_size;
			u8 data_len;

			if (elt->length < sizeof(struct oz_get_desc_rsp) - 1)
				break;
			data_len = elt->length -
					(sizeof(struct oz_get_desc_rsp) - 1);
			offs = le16_to_cpu(get_unaligned(&body->offset));
			total_size =
				le16_to_cpu(get_unaligned(&body->total_size));
			oz_hcd_get_desc_cnf(usb_ctx->hport, body->req_id,
					body->rcode, body->data,
					data_len, offs, total_size);
		}
		break;
	case OZ_SET_CONFIG_RSP: {
			struct oz_set_config_rsp *body =
				(struct oz_set_config_rsp *)usb_hdr;
			oz_hcd_control_cnf(usb_ctx->hport, body->req_id,
				body->rcode, 0, 0);
		}
		break;
	case OZ_SET_INTERFACE_RSP: {
			struct oz_set_interface_rsp *body =
				(struct oz_set_interface_rsp *)usb_hdr;
			oz_hcd_control_cnf(usb_ctx->hport,
				body->req_id, body->rcode, 0, 0);
		}
		break;
	case OZ_VENDOR_CLASS_RSP: {
			struct oz_vendor_class_rsp *body =
				(struct oz_vendor_class_rsp *)usb_hdr;
			oz_hcd_control_cnf(usb_ctx->hport, body->req_id,
				body->rcode, body->data, elt->length-
				sizeof(struct oz_vendor_class_rsp)+1);
		}
		break;
	case OZ_USB_ENDPOINT_DATA:
		oz_usb_handle_ep_data(usb_ctx, usb_hdr, elt->length);
		break;
	}
done:
	oz_usb_put(usb_ctx);
}
/*------------------------------------------------------------------------------
 * Context: softirq, process
 */
void oz_usb_farewell(struct oz_pd *pd, u8 ep_num, u8 *data, u8 len)
{
	struct oz_usb_ctx *usb_ctx;
	spin_lock_bh(&pd->app_lock[OZ_APPID_USB-1]);
	usb_ctx = (struct oz_usb_ctx *)pd->app_ctx[OZ_APPID_USB-1];
	if (usb_ctx)
		oz_usb_get(usb_ctx);
	spin_unlock_bh(&pd->app_lock[OZ_APPID_USB-1]);
	if (usb_ctx == 0)
		return; /* Context has gone so nothing to do. */
	if (!usb_ctx->stopped) {
		oz_trace("Farewell indicated ep = 0x%x\n", ep_num);
		oz_hcd_data_ind(usb_ctx->hport, ep_num, data, len);
	}
	oz_usb_put(usb_ctx);
}
/*------------------------------------------------------------------------------
 * Context: softirq or process
 */
u8 oz_get_up_max_buffer_units(void *hpd)
{
	struct oz_usb_ctx *usb_ctx = (struct oz_usb_ctx *)hpd;
	oz_trace_msg(M, "max_buffer_units:%d\n", usb_ctx->pd->up_audio_buf);
	return usb_ctx->pd->up_audio_buf;
}
