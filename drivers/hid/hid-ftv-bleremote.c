/*
 * HID driver for Lab126 Bluetooth LE Remote
 *
 * Copyright (C) 2017 Amazon Lab126.
 *
 * Author:
 *	Muhaiyadeen Habibullah <habibulm@amazon.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/input/mt.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <asm/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>
#include <linux/version.h>
#if IS_ENABLED(CONFIG_AMAZON_MINERVA_METRICS_LOG) || IS_ENABLED(CONFIG_AMAZON_METRICS_LOG)
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/metricslog.h>
#endif
#include "hid-ids.h"
#define ftv_remote_log(...) pr_info("snd_atvr: " __VA_ARGS__)

/* BLE Remote Reports */
#define KEY_REPORT_ID          0x01
#define CONSUMER_REPORT_ID     0x02
#define LAB126_REPORT_ID       0xEF
#define OPUS_AUDIO_REPORT_ID   0xF0
#define AUDIO_CONFIG_REPORT_ID 0xF1
#define AUDIO_STATE_REPORT_ID  0xF2
#define DIAG_REPORT_ID         0xF3
#define ADPCM_AUDIO_REPORT_ID  0xF4
#define GAME_CTLR_VOICE_REPORT_ID 0xF5

#define PARTNER_KEY_USAGE      0x01
#define ABS_VOLUME_USAGE       0xE0
#define REL_HWHEEL_USAGE      0x238

#define MAX_BUFFER_STREAM_SIZE    (5*1024)

/* To enable Throughput Testing, Uncomment and set hid_debug = 1 in hid-core.c */
/* #define TPUT_TESTING */

/* Keys */
#define SEARCH_KEY	0x221
#define CUSTOM_KEY1	0xA1
#define CUSTOM_KEY2	0xA2
#define CUSTOM_KEY3	0xA3
#define CUSTOM_KEY4	0xA4
#define CONSUMER_PRESET_1_KEY 0x27e
#define CONSUMER_PRESET_2_KEY 0x27f
#define CONSUMER_AUTO_PAIR_KEY 0x280
#define CONSUMER_SETTINGS_KEY  0x33
#define CONSUMER_METRIC_VIRTUAL_KEY 0x230

#define CUSTOM_APP1	0xFA /* Prime Video - Dont change*/
#define CUSTOM_APP2	0xF9 /* Netflix - Dont Change*/
#define CUSTOM_APP3	0xFB
#define CUSTOM_APP4	0xFD
#define CONSUMER_PRESET_1 0x27e
#define CONSUMER_PRESET_2 0x27f
#define CONSUMER_AUTO_PAIR 0x280
#define CONSUMER_SETTINGS  0x2ee
#define CONSUMER_METRIC_VIRTUAL 0x2ff /* KEY_MAX */

#if IS_ENABLED(CONFIG_AMAZON_MINERVA_METRICS_LOG)
#define VENDOR_BT_REMOTE_GROUP_ID     "2999trpq"
#define VENDOR_BT_REMOTE_SCHEMA_ID    "b9p5/2/04330400"
#endif

/* Debug feature to trace audio packets being received */
#define DEBUG_AUDIO_RECEPTION 1

/* Debug feature to trace HID reports we see */
#define DEBUG_HID_RAW_INPUT 0

/* report id & state */
static unsigned char audio_start[] = { AUDIO_STATE_REPORT_ID, 0x01};
static unsigned char audio_stop[] = { AUDIO_STATE_REPORT_ID, 0x00};

struct audio_ring_buffer {
	unsigned char *audio_data_start;     /* data buffer  */
	unsigned char *audio_data_end; /* end of data buffer */
	unsigned int buffer_count;
	unsigned int underrun_count;
	unsigned short buffer_loop;
	unsigned char *write_buffer;       /* pointer to head */
	unsigned char *read_buffer;       /* pointer to tail */
};

struct ftv_remote_device {
	unsigned short voice_active;
	struct hid_device *hdev;
	unsigned short audio_state_started;
#if IS_ENABLED(CONFIG_AMAZON_MINERVA_METRICS_LOG) || IS_ENABLED(CONFIG_AMAZON_METRICS_LOG)
	ktime_t voice_key_down_time;
	ktime_t voice_key_up_time;
	ktime_t voice_start_time;
	ktime_t voice_stop_time;
#endif
};

struct ftv_remote_drvdata {
	struct input_dev *input;
};

static struct miscdevice bleremote_dev_node;
static struct audio_ring_buffer raw_audio_buffer_stream;
static struct ftv_remote_device bleremote_dev;
struct mutex audio_rw_lock;

#ifdef TPUT_TESTING
/* Throughput testing */
#define TPUT_MEASUREMENT_TIMEOUT 10

static unsigned long tput_bytes_received;
static unsigned long tput_avg;
unsigned short test_started;
static unsigned short count;
struct timer_list tput_timer;
#endif

/* Function Declaration */
static int audio_buffer_stream_init(void);
void audio_buffer_stream_write(const void *raw_input_buffer, unsigned int size);
static void process_opus_audio_data(unsigned char *raw_input, unsigned int size);
static void process_adpcm_audio_data(unsigned char *raw_input, unsigned int size);

#if IS_ENABLED(CONFIG_AMAZON_MINERVA_METRICS_LOG) || IS_ENABLED(CONFIG_AMAZON_METRICS_LOG)
#define METRICS_META_DATA_MAX_LEN 256
static const char * voice_latency_labels[] = {
	"0-100ms",
	"100-200ms",
	"200-300ms",
	"300-400ms",
	"400-500ms",
	"500-600ms",
	"600-700ms",
	"700-800ms",
	"800-900ms",
	"900-1000ms",
	"1000-1100ms",
	"1100-1200ms",
	"1200-1300ms",
	"1300-1400ms",
	"1400-1500ms",
	">1500ms",
};
static const char * get_voice_latency_label(int delta) {
	int index = delta / 100;
	if (index < 0)
		index = 0;
	else if (index > 15)
		index = 15;
	return voice_latency_labels[index];
}
#endif

#ifdef TPUT_TESTING
void calculate_tput(unsigned long data)
{
	dbg_hid("ftvremote: calculate_tput bytes_received: %lu\n",
		tput_bytes_received);
	if (bleremote_dev.hdev != NULL) {
		if (test_started) {
		/* stop the test */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 15, 0))
			hid_hw_output_report(bleremote_dev.hdev, audio_stop, sizeof(audio_stop));
#else
			bleremote_dev.hdev->hid_output_raw_report(bleremote_dev.hdev,
							audio_stop,
							sizeof(audio_stop),
							HID_OUTPUT_REPORT);
#endif
			test_started = 0;
			tput_avg += tput_bytes_received;
			dbg_hid("ftvremote: Current Throughput (10s): %lu.%lu Kbps\n ",
			((tput_bytes_received*8)/(TPUT_MEASUREMENT_TIMEOUT*1000)),
			((tput_bytes_received * 8)%1000));
			if (count == 100) {
				dbg_hid("ftvremote: Avg Throughput (1000s): %lu.%lu Kbps\n",
				((tput_avg * 8)/(TPUT_MEASUREMENT_TIMEOUT*1000*count)),
				((tput_bytes_received*8)%(count*1000)));
				count = 0;
				tput_avg = 0;
			}
			tput_bytes_received = 0;
		} else {
		test_started = 1;
		count++;
		tput_bytes_received = 0;
		/* start the test */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 15, 0))
		hid_hw_output_report(bleremote_dev.hdev, audio_start, sizeof(audio_start));
#else
		bleremote_dev.hdev->hid_output_raw_report(bleremote_dev.hdev,
							audio_start,
							sizeof(audio_start),
							HID_OUTPUT_REPORT);
#endif
		}
		tput_timer.expires = jiffies + msecs_to_jiffies(10000);
		add_timer(&tput_timer);
	}
}
#endif


static int audio_buffer_stream_init(void)
{
	dbg_hid("ftvremote: audio_buffer_stream_init\n ");
	raw_audio_buffer_stream.audio_data_start = kzalloc(MAX_BUFFER_STREAM_SIZE, GFP_KERNEL);
	if (!raw_audio_buffer_stream.audio_data_start)
		return -ENOMEM;

	raw_audio_buffer_stream.audio_data_end = raw_audio_buffer_stream.audio_data_start +
						(MAX_BUFFER_STREAM_SIZE);
	raw_audio_buffer_stream.buffer_count = 0; /* used to detect overflow */
	raw_audio_buffer_stream.underrun_count = 0;
	raw_audio_buffer_stream.buffer_loop = 0;
	raw_audio_buffer_stream.write_buffer = raw_audio_buffer_stream.audio_data_start;
	raw_audio_buffer_stream.read_buffer = raw_audio_buffer_stream.audio_data_start;
	return 0;
}

void audio_buffer_stream_reset(void)
{
	dbg_hid("ftvremote: audio_buffer_stream_reset\n ");
	mutex_lock(&audio_rw_lock);
	raw_audio_buffer_stream.buffer_count = 0;
	raw_audio_buffer_stream.underrun_count = 0;
	raw_audio_buffer_stream.buffer_loop = 0;
	raw_audio_buffer_stream.write_buffer = raw_audio_buffer_stream.audio_data_start;
	raw_audio_buffer_stream.read_buffer = raw_audio_buffer_stream.audio_data_start;
	mutex_unlock(&audio_rw_lock);
}

/* VARIABLE_SIZE buffer implementation */
void audio_buffer_stream_write(const void *raw_input_buffer, unsigned int size)
{
	unsigned int copy_size = 0, write_size = 0;
	dbg_hid("ftvremote: write ENTER Buf_cnt: %u, CurWriteBuf : %p\n",
		raw_audio_buffer_stream.buffer_count,
		(void *)raw_audio_buffer_stream.write_buffer);
	write_size = size;

	/* check for buffer wrap */
	if (raw_audio_buffer_stream.audio_data_end <=
		(raw_audio_buffer_stream.write_buffer + size)) {

		copy_size = raw_audio_buffer_stream.audio_data_end -
			raw_audio_buffer_stream.write_buffer;

		memset(raw_audio_buffer_stream.write_buffer, 0, copy_size);
		memcpy(raw_audio_buffer_stream.write_buffer, raw_input_buffer, copy_size);

		/*precond: looped once and read ptr is ahead of write ptr. then write ptr
		shall not overwrite read ptr before looping again. So move read ptr
		head copy size will reach end of buffer stream and decrement buffer
		counter for overwritten buffers.
		*/
		if ((raw_audio_buffer_stream.buffer_loop == 1) &&
		(raw_audio_buffer_stream.write_buffer <= raw_audio_buffer_stream.read_buffer)
		&& ((raw_audio_buffer_stream.write_buffer + copy_size) >
		raw_audio_buffer_stream.read_buffer)) {
			raw_audio_buffer_stream.buffer_count -=
				(raw_audio_buffer_stream.audio_data_end -
				raw_audio_buffer_stream.read_buffer);
			raw_audio_buffer_stream.read_buffer = raw_audio_buffer_stream.audio_data_start;
		}

		raw_audio_buffer_stream.write_buffer = raw_audio_buffer_stream.audio_data_start;
		raw_audio_buffer_stream.buffer_count += copy_size;
		write_size = size - copy_size;
		raw_audio_buffer_stream.buffer_loop = 1;
	}

	/* copy the remaining bytes if any */
	if (write_size) {
		memset(raw_audio_buffer_stream.write_buffer, 0, write_size);
		memcpy(raw_audio_buffer_stream.write_buffer, raw_input_buffer+copy_size, write_size);
		raw_audio_buffer_stream.write_buffer += write_size;
		raw_audio_buffer_stream.buffer_count += write_size;
	}

	if (raw_audio_buffer_stream.buffer_count > MAX_BUFFER_STREAM_SIZE)
		raw_audio_buffer_stream.buffer_count = MAX_BUFFER_STREAM_SIZE;

	/* if write pointer looped back then it shouldnt move ahead of read ptr after looping.
	* so move read ptr and set the length */
	if ((raw_audio_buffer_stream.buffer_loop) &&
	(raw_audio_buffer_stream.write_buffer >= raw_audio_buffer_stream.read_buffer)) {
		raw_audio_buffer_stream.read_buffer = raw_audio_buffer_stream.write_buffer;
	}

	dbg_hid("ftvremote: write exit CurBufcnt: %u, Curwrite_addr: %p, read_buffer:%p\n",
		raw_audio_buffer_stream.buffer_count, (void *)raw_audio_buffer_stream.write_buffer,
		(void *)raw_audio_buffer_stream.read_buffer);
}

static unsigned int  bleremote_audio_buffer_stream_read(struct file *file, char __user *buffer,
				unsigned int count, loff_t *ppos)
{
	unsigned int read_size, current_read_size, bytes_sent = 0;
	mutex_lock(&audio_rw_lock);
	read_size = count;

	if (raw_audio_buffer_stream.buffer_count == 0) {
		/*pr_warn("%s: Buffer Underrun packet loss\n",
			__func__);*/
		raw_audio_buffer_stream.underrun_count++;
		goto unlock;
	}

	dbg_hid("ftvremote: Read ENTER CurBufcnt: %u, Curwrite_addr: %p, read_buffer:%p\n",
		raw_audio_buffer_stream.buffer_count, (void *)raw_audio_buffer_stream.write_buffer,
		(void *)raw_audio_buffer_stream.read_buffer);

	if (raw_audio_buffer_stream.buffer_count < read_size)
		read_size = raw_audio_buffer_stream.buffer_count;

	if ((raw_audio_buffer_stream.read_buffer + read_size) >
	raw_audio_buffer_stream.audio_data_end) {
		current_read_size = raw_audio_buffer_stream.audio_data_end -
		raw_audio_buffer_stream.read_buffer;

		if (copy_to_user(buffer, raw_audio_buffer_stream.read_buffer, current_read_size)) {
			bytes_sent = -EFAULT;
			goto unlock;
		}
		bytes_sent = current_read_size;
		raw_audio_buffer_stream.read_buffer = raw_audio_buffer_stream.audio_data_start;
	} else {
		if (copy_to_user(buffer, raw_audio_buffer_stream.read_buffer, read_size)) {
			bytes_sent = -EFAULT;
			goto unlock;
		}
		bytes_sent = read_size;
		raw_audio_buffer_stream.read_buffer += bytes_sent;
	}
	raw_audio_buffer_stream.buffer_count -= bytes_sent;

	/* Resetting the loop if looped and all the data is read. This will let readbuffer to
	move fwd in stream_write */
	if ((raw_audio_buffer_stream.buffer_loop) &&
	(raw_audio_buffer_stream.buffer_count == 0)) {
		raw_audio_buffer_stream.buffer_loop = 0;
	}

unlock:

	dbg_hid("ftvremote:read EXIT CurBufcnt: %u, Curwrite_addr: %p, read_buffer:%p bytes_Sent:%u\n",
		raw_audio_buffer_stream.buffer_count, (void *)raw_audio_buffer_stream.write_buffer,
		(void *)raw_audio_buffer_stream.read_buffer, bytes_sent);
	mutex_unlock(&audio_rw_lock);
	return bytes_sent;
}


static int bleremote_audio_dev_open(struct inode *inode, struct file *file)
{
	dbg_hid("ftvremote: bleremote_audio_dev_open\n");
#if IS_ENABLED(CONFIG_AMAZON_MINERVA_METRICS_LOG) || IS_ENABLED(CONFIG_AMAZON_METRICS_LOG)
	bleremote_dev.voice_start_time = ktime_get();
#endif
	return 0;
}

static int bleremote_audio_dev_close(struct inode *inode, struct file *file)
{
	dbg_hid("ftvremote: bleremote_audio_dev_close\n");
#if IS_ENABLED(CONFIG_AMAZON_MINERVA_METRICS_LOG) || IS_ENABLED(CONFIG_AMAZON_METRICS_LOG)
	{
		char dimensions[METRICS_META_DATA_MAX_LEN];
		int pressed_time;
		int record_time;
		int start_delay;
		int stop_delay;

		bleremote_dev.voice_stop_time = ktime_get();
		pressed_time = (int)ktime_to_ms(ktime_sub(bleremote_dev.voice_key_up_time,
			bleremote_dev.voice_key_down_time));
		record_time = (int)ktime_to_ms(ktime_sub(bleremote_dev.voice_stop_time,
			bleremote_dev.voice_start_time));
		start_delay = (int)ktime_to_ms(ktime_sub(bleremote_dev.voice_start_time,
			bleremote_dev.voice_key_down_time));
		stop_delay = (int)ktime_to_ms(ktime_sub(bleremote_dev.voice_stop_time,
			bleremote_dev.voice_key_up_time));
#if IS_ENABLED(CONFIG_AMAZON_MINERVA_METRICS_LOG)
		snprintf(dimensions, METRICS_META_DATA_MAX_LEN,
				"\"%s\"#\"%s\"$\"%s\"#\"%s\"$\"%s\"#\"%s\"$\"%s\"#\"%s\"",
				"voice_key_pressed_time",
				get_voice_latency_label(pressed_time),
				"voice_data_record_time",
				get_voice_latency_label(record_time),
				"voice_record_start_delay",
				get_voice_latency_label(start_delay),
				"voice_record_stop_delay",
				get_voice_latency_label(stop_delay));
		log_counter_to_vitals_v2(ANDROID_LOG_INFO,
				VENDOR_BT_REMOTE_GROUP_ID,
				VENDOR_BT_REMOTE_SCHEMA_ID, "Kernel",
				"remote-wireless", "bt-ble-voicesearch",
				"voice-started", 1, "count",
				NULL, VITALS_NORMAL, dimensions, NULL);
#else
		snprintf(dimensions, METRICS_META_DATA_MAX_LEN,
				"!{\"d\"#{\"%s\"#\"%s\"$\"%s\"#\"%s\"$\"%s\"#\"%s\"$\"%s\"#\"%s\"}}",
				"voice_key_pressed_time", get_voice_latency_label(pressed_time),
				"voice_data_record_time", get_voice_latency_label(record_time),
				"voice_record_start_delay", get_voice_latency_label(start_delay),
				"voice_record_stop_delay", get_voice_latency_label(stop_delay));
		log_counter_to_vitals(ANDROID_LOG_INFO, "Kernel",
				"remote-wireless", "bt-ble-voicesearch",
				"voice-started", 1, "count",
				dimensions, VITALS_NORMAL);
#endif
	}
#endif
	return 0;
}


static const struct file_operations bleremote_audio_fops = {
	.owner = THIS_MODULE,
	.open = bleremote_audio_dev_open,
	.llseek = no_llseek,
	.read = bleremote_audio_buffer_stream_read,
	.release = bleremote_audio_dev_close,
};


static void process_opus_audio_data(unsigned char *raw_input, unsigned int size)
{
	mutex_lock(&audio_rw_lock);
	audio_buffer_stream_write((raw_input + 1), (size - 1));
	mutex_unlock(&audio_rw_lock);
	#ifdef TPUT_TESTING
	/* Avoid header incase if its present */
	if (size > 3)
		tput_bytes_received += (size - 1);
	#endif
}

static void process_adpcm_audio_data(unsigned char *raw_input, unsigned int size)
{
	dbg_hid("ftvremote: process_adpcm_audio_data Size %d\n", size);
}

static const __u32 amazon_ble_game_controller[] =
{
    BT_DEVICE_ID_LAB126_abc123
};

static int is_amazon_ble_game_controller(struct hid_device *hdev) {
	int found = 0;

	if (hdev->vendor == BT_VENDOR_ID_LAB126) {
		__u32 product = hdev->product;
		int i = 0;
		int list_size = sizeof(amazon_ble_game_controller) / sizeof(__u32);
		for (i = 0; i < list_size; i++) {
			if (product == amazon_ble_game_controller[i]) {
				found = 1;
				break;
			}
		}
	}

	return found;
}

/*
 *  Deal with input raw event
 *  REPORT_ID:		DESCRIPTION:
 *	0x01			Keyboard
 *	0x240			Audio Data
 *	0x242			Audio State
 *  0x243			Diag
 *	0x09			Lab126 Keyboard : handle Shop key
 */

static int ftv_remote_raw_event(struct hid_device *hdev, struct hid_report *report,
	u8 *data, int size)
{
	unsigned short *keycode;
	int i;
	struct ftv_remote_drvdata *remote_drvdata = hid_get_drvdata(hdev);
	dbg_hid("ftvremote: %s\n", __func__);
#if (DEBUG_HID_RAW_INPUT == 1)
	pr_info("%s: report->id = 0x%x, size = %d\n",
		__func__, report->id, size);
	if (size < 20) {
		int i;
		for (i = 1; i < size; i++)
			pr_info("data[%d] = 0x%02x\n", i, data[i]);
	}
#endif

	switch (report->id) {
	case OPUS_AUDIO_REPORT_ID:
		process_opus_audio_data((unsigned char *)data, (unsigned int) size);
		return 1;
	break;

	case ADPCM_AUDIO_REPORT_ID:
		process_adpcm_audio_data((unsigned char *)data, (unsigned int) size);
		return 1;
	break;

	case KEY_REPORT_ID:
	break;

	case CONSUMER_REPORT_ID:
		/* for BLE Game Controller this report ID is Usage (AC Home), ignore! */
		if (is_amazon_ble_game_controller(hdev))
			break;
		/* fallthrough */
	case GAME_CTLR_VOICE_REPORT_ID: /* specially added for BLE Game Controller */
		keycode = (unsigned short *)&data[1];
		dbg_hid("ftvremote: ftv_remote_raw_event consumer keycode: %d\n", *keycode);
		for (i = 1; i < size; i++)
			dbg_hid("ftvremote: data[%d] = 0x%02x\n", i, data[i]);

		switch (*keycode) {
		case SEARCH_KEY:
			if (bleremote_dev.voice_active == true)
			return 1;
			else {
				int ret;
				audio_buffer_stream_reset();
				bleremote_dev.voice_active = true;
				bleremote_dev.hdev = hdev;
#if IS_ENABLED(CONFIG_AMAZON_MINERVA_METRICS_LOG) || IS_ENABLED(CONFIG_AMAZON_METRICS_LOG)
				bleremote_dev.voice_key_down_time = ktime_get();
#endif
				dbg_hid("ftvremote: ftv_remote_raw_event voice active: TRUE device %p\n",
					bleremote_dev.hdev);


				mutex_lock(&audio_rw_lock);

			#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 15, 0))
				ret = hid_hw_output_report(bleremote_dev.hdev, audio_start, sizeof(audio_start));
			#else
				ret = bleremote_dev.hdev->hid_output_raw_report(bleremote_dev.hdev,
					audio_start, sizeof(audio_start), HID_OUTPUT_REPORT);
			#endif
				if (ret < 0)
					dbg_hid("ftvremote:Audio Start Output report failed\n");
				else
					bleremote_dev.audio_state_started = true;

				mutex_unlock(&audio_rw_lock);
			}
		break;

		case 0x00:
			if ((bleremote_dev.voice_active == true) && (bleremote_dev.hdev == hdev)) {
				int ret;

				mutex_lock(&audio_rw_lock);

			#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 15, 0))
				ret = hid_hw_output_report(bleremote_dev.hdev, audio_stop, sizeof(audio_stop));
			#else
				ret = bleremote_dev.hdev->hid_output_raw_report(bleremote_dev.hdev,
					audio_stop, sizeof(audio_stop), HID_OUTPUT_REPORT);
			#endif
				if (ret < 0)
					dbg_hid("ftvremote:Audio Start Output report failed\n");
				else
					bleremote_dev.audio_state_started = false;

				mutex_unlock(&audio_rw_lock);

				dbg_hid("ftvremote: ftv_remote_raw_event voice active: FALSE device %p\n",
					bleremote_dev.hdev);
				pr_warn("%s: Buffer Underrun packet loss count %u\n",
					__func__, raw_audio_buffer_stream.underrun_count);

				bleremote_dev.voice_active = false;
				bleremote_dev.hdev = NULL;
#if IS_ENABLED(CONFIG_AMAZON_MINERVA_METRICS_LOG) || IS_ENABLED(CONFIG_AMAZON_METRICS_LOG)
				bleremote_dev.voice_key_up_time = ktime_get();

				if (!bleremote_dev.audio_state_started) {
					char dimensions[METRICS_META_DATA_MAX_LEN];
					int pressed_time = (int)ktime_to_ms(ktime_sub(bleremote_dev.voice_key_up_time,
						bleremote_dev.voice_key_down_time));
#if IS_ENABLED(CONFIG_AMAZON_MINERVA_METRICS_LOG)
					snprintf(dimensions, METRICS_META_DATA_MAX_LEN,
						 "\"%s\"#\"%s\"", "voice_key_pressed_time",
						 get_voice_latency_label(pressed_time));
					log_counter_to_vitals_v2(ANDROID_LOG_INFO,
						VENDOR_BT_REMOTE_GROUP_ID,
						VENDOR_BT_REMOTE_SCHEMA_ID,
						"Kernel",
						"remote-wireless", "bt-ble-voicesearch",
						"voice-not-started", 1, "count",
						NULL, VITALS_NORMAL, dimensions, NULL);
#else
					snprintf(dimensions, METRICS_META_DATA_MAX_LEN,
						 "!{\"d\"#{\"%s\"#\"%s\"}}", "voice_key_pressed_time",
						 get_voice_latency_label(pressed_time));
					log_counter_to_vitals(ANDROID_LOG_INFO,  "Kernel",
						"remote-wireless", "bt-ble-voicesearch",
						"voice-not-started", 1, "count",
						dimensions, VITALS_NORMAL);
#endif
				}
#endif
			}
		break;
		}
	break;

	case AUDIO_STATE_REPORT_ID:
		return 1;
	break;

	case DIAG_REPORT_ID:
		return 1;
	break;

	case LAB126_REPORT_ID:
		keycode = (unsigned short *)&data[1];
		dbg_hid("ftvremote: ftv_remote_raw_event lab126 keycode: %d\n", *keycode);
		switch (*keycode) {
		case CUSTOM_KEY1:
			input_report_key(remote_drvdata->input, CUSTOM_APP1, 1);
			input_sync(remote_drvdata->input);
			input_report_key(remote_drvdata->input, CUSTOM_APP1, 0);
			input_sync(remote_drvdata->input);
		break;

		case CUSTOM_KEY2:
			input_report_key(remote_drvdata->input, CUSTOM_APP2, 1);
			input_sync(remote_drvdata->input);
			input_report_key(remote_drvdata->input, CUSTOM_APP2, 0);
			input_sync(remote_drvdata->input);
		break;

		case CUSTOM_KEY3:
			input_report_key(remote_drvdata->input, CUSTOM_APP3, 1);
			input_sync(remote_drvdata->input);
			input_report_key(remote_drvdata->input, CUSTOM_APP3, 0);
			input_sync(remote_drvdata->input);
		break;

		case CUSTOM_KEY4:
			input_report_key(remote_drvdata->input, CUSTOM_APP4, 1);
			input_sync(remote_drvdata->input);
			input_report_key(remote_drvdata->input, CUSTOM_APP4, 0);
			input_sync(remote_drvdata->input);
		break;
		}
		return 1;
	break;

	default:
	break;
	}
	/* let the event through for regular input processing */
	return 0;
}

static int ftv_remote_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret;
	struct ftv_remote_drvdata *remote_drvdata;
    /* since vendor/product id filter doesn't work yet, because
     * Bluedroid is unable to get the vendor/product id, we
     * have to filter on name
     */
	pr_info("%s: hdev->name = %s, vendor_id = %d, product_id = %d\n",
		__func__, hdev->name, hdev->vendor, hdev->product);

	remote_drvdata = kmalloc(sizeof(*remote_drvdata), GFP_KERNEL | __GFP_ZERO);
	if (remote_drvdata == NULL) {
		hid_err(hdev, "can't alloc remote_drvdata descriptor\n");
		return -ENOMEM;
	}

	hid_set_drvdata(hdev, remote_drvdata);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid parse failed\n");
		goto err_parse;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret) {
		hid_err(hdev, "hw start failed\n");
		goto err_start;
	}

	if (!remote_drvdata->input) {
		hid_err(hdev, "ftv remote  input not registered\n");
		ret = -ENOMEM;
		goto err_start;
	}

	bleremote_dev.voice_active = false;
	bleremote_dev.audio_state_started = false;

#ifdef TPUT_TESTING
	bleremote_dev.hdev = hdev;
	init_timer(&tput_timer);
	tput_timer.function = calculate_tput;
	tput_timer.expires = jiffies + msecs_to_jiffies(10000);
	tput_timer.data = 0;
	dbg_hid("ftvremote: ftv_remote_probe calculate_tput timer started\n");
	add_timer(&tput_timer);
#endif

	return 0;

err_start:
err_parse:
	return ret;
}

static int ftv_remote_input_mapping(struct hid_device *hdev,
		struct hid_input *hi, struct hid_field *field,
		struct hid_usage *usage, unsigned long **bit, int *max)
{
	struct input_dev *input = hi->input;
	struct ftv_remote_drvdata *remote_drvdata = hid_get_drvdata(hdev);

	dbg_hid("%s: Usage page = 0x%x, Usage id = 0x%x\n", __func__,
				  (usage->hid & HID_USAGE_PAGE) >> 4, usage->hid & HID_USAGE);

	if (!remote_drvdata->input)
		remote_drvdata->input = hi->input;

	switch (usage->hid & HID_USAGE_PAGE) {
	case HID_UP_CUSTOM:	/* 0x00ff */
		switch (usage->hid & HID_USAGE) {
		case PARTNER_KEY_USAGE:
			set_bit(EV_KEY, input->evbit);
			set_bit(EV_SYN, input->evbit);
			set_bit(CUSTOM_APP1, input->keybit);
			set_bit(CUSTOM_APP2, input->keybit);
			set_bit(CUSTOM_APP3, input->keybit);
			set_bit(CUSTOM_APP4, input->keybit);
		break;

		default:
		break;
		}

	case HID_UP_CONSUMER:    /* 0x00c0 */
		switch (usage->hid & HID_USAGE) {
		case ABS_VOLUME_USAGE:
		case REL_HWHEEL_USAGE:
			dbg_hid("Ignore ABS_VOLUME and REL_HWHEEL\n");
			return -1;
		case CONSUMER_PRESET_1_KEY:
			hid_map_usage_clear(hi, usage, bit, max, EV_KEY, CONSUMER_PRESET_1);
			return 1;
		case CONSUMER_PRESET_2_KEY:
			hid_map_usage_clear(hi, usage, bit, max, EV_KEY, CONSUMER_PRESET_2);
			return 1;
		case CONSUMER_AUTO_PAIR_KEY:
			hid_map_usage_clear(hi, usage, bit, max, EV_KEY, CONSUMER_AUTO_PAIR);
			return 1;
		case CONSUMER_SETTINGS_KEY:
			hid_map_usage_clear(hi, usage, bit, max, EV_KEY, CONSUMER_SETTINGS);
			return 1;
		case CONSUMER_METRIC_VIRTUAL_KEY:
			hid_map_usage_clear(hi, usage, bit, max, EV_KEY, CONSUMER_METRIC_VIRTUAL);
			return 1;
		default:
			break;
		}
	default:
		break;
	}
	return 0;
}

static void ftv_remote_remove(struct hid_device *hdev)
{
	struct ftv_remote_drvdata *remote_drvdata = hid_get_drvdata(hdev);
	pr_info("%s: hdev->name = %s\n", __func__, hdev->name);

#ifdef TPUT_TESTING
	del_timer(&tput_timer);
#endif
	if (bleremote_dev.hdev == hdev) {
		bleremote_dev.hdev = NULL;
		bleremote_dev.voice_active = false;
		bleremote_dev.audio_state_started = false;
	}
	hid_hw_stop(hdev);

	if (NULL != remote_drvdata)
		kfree(remote_drvdata);
}


static const struct hid_device_id ftv_remote_devices[] = {
	{HID_BLUETOOTH_DEVICE(BT_VENDOR_ID_LAB126, BT_DEVICE_ID_LAB126_abi123)},
	{HID_USB_DEVICE(BT_VENDOR_ID_LAB126, BT_DEVICE_ID_LAB126_abi123)},
	{HID_BLUETOOTH_DEVICE(BT_VENDOR_ID_LAB126, BT_DEVICE_ID_LAB126_abj123)},
	{HID_USB_DEVICE(BT_VENDOR_ID_LAB126, BT_DEVICE_ID_LAB126_abj123)},
	{HID_BLUETOOTH_DEVICE(BT_VENDOR_ID_LAB126, BT_DEVICE_ID_LAB126_abk123)},
	{HID_USB_DEVICE(BT_VENDOR_ID_LAB126, BT_DEVICE_ID_LAB126_abk123)},
	{HID_BLUETOOTH_DEVICE(BT_VENDOR_ID_LAB126, BT_DEVICE_ID_LAB126_abc123)},
	{HID_USB_DEVICE(BT_VENDOR_ID_LAB126, BT_DEVICE_ID_LAB126_abc123)},
	{HID_BLUETOOTH_DEVICE(BT_VENDOR_ID_LAB126, BT_DEVICE_ID_LAB126_abo123)},
	{HID_USB_DEVICE(BT_VENDOR_ID_LAB126, BT_DEVICE_ID_LAB126_abo123)},
	{HID_BLUETOOTH_DEVICE(BT_VENDOR_ID_LAB126, BT_DEVICE_ID_LAB126_aby123)},
	{HID_USB_DEVICE(BT_VENDOR_ID_LAB126, BT_DEVICE_ID_LAB126_aby123)},
	{HID_BLUETOOTH_DEVICE(BT_VENDOR_ID_LAB126, BT_DEVICE_ID_LAB126_abu123)},
	{HID_USB_DEVICE(BT_VENDOR_ID_LAB126, BT_DEVICE_ID_LAB126_abu123)},
	{HID_BLUETOOTH_DEVICE(BT_VENDOR_ID_LAB126, BT_DEVICE_ID_LAB126_abt123)},
	{HID_USB_DEVICE(BT_VENDOR_ID_LAB126, BT_DEVICE_ID_LAB126_abt123)},
	{HID_BLUETOOTH_DEVICE(BT_VENDOR_ID_LAB126, BT_DEVICE_ID_LAB126_abs123)},
	{HID_USB_DEVICE(BT_VENDOR_ID_LAB126, BT_DEVICE_ID_LAB126_abs123)},
	{ }
};
MODULE_DEVICE_TABLE(hid, ftv_remote_devices);

static struct hid_driver ftv_remote_driver = {
	.name = "FireTV remote",
	.id_table = ftv_remote_devices,
	.raw_event = ftv_remote_raw_event,
	.probe = ftv_remote_probe,
	.remove = ftv_remote_remove,
	.input_mapping = ftv_remote_input_mapping,
};

static int __init ftv_remote_init(void)
{
	int ret;

	ret = hid_register_driver(&ftv_remote_driver);
	if (ret) {
		pr_err("%s: can't register Fire TV Remote driver\n", __func__);
		return ret;
	}

	mutex_init(&audio_rw_lock);
	bleremote_dev_node.minor = MISC_DYNAMIC_MINOR;
	bleremote_dev_node.name = "bleremote_audio";
	bleremote_dev_node.fops = &bleremote_audio_fops;
	ret = misc_register(&bleremote_dev_node);
	if (ret) {
		pr_err("%s: failed to create ble remote misc device %d\n",
			__func__, ret);
		hid_unregister_driver(&ftv_remote_driver);
		return ret;
	} else {
		pr_info("%s: succeeded creating misc device %s\n",
			__func__, bleremote_dev_node.name);
	}

	ret = audio_buffer_stream_init();
	if (ret) {
		pr_err("%s: failed to initialise the audio buffer %d\n",
			__func__, ret);
		misc_deregister(&bleremote_dev_node);
		hid_unregister_driver(&ftv_remote_driver);
	}

	return ret;
}

static void __exit ftv_remote_exit(void)
{
	dbg_hid("ftvremote: %s\n", __func__);

	misc_deregister(&bleremote_dev_node);
	hid_unregister_driver(&ftv_remote_driver);

}

module_init(ftv_remote_init);
module_exit(ftv_remote_exit);
