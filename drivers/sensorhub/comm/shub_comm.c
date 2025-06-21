/*
 *  Copyright (C) 2020, Samsung Electronics Co. Ltd. All Rights Reserved.
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
 */

#include "../debug/shub_debug.h"
#include "../sensor/scontext.h"
#include "../sensorhub/shub_device.h"
#include "../sensormanager/shub_sensor.h"
#include "../sensormanager/shub_sensor_manager.h"
#include "../utility/shub_utility.h"
#include "../vendor/shub_vendor.h"
#include "shub_cmd.h"

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mutex.h>
// FIX: Include spinlock header for IRQ-safe locking
#include <linux/spinlock.h>

#define SHUB_CMD_SIZE		64

#define SHUB2AP_BYPASS_DATA	0x37
#define SHUB2AP_LIBRARY_DATA	0x01
#define SHUB2AP_DEBUG_DATA	0x03
#define SHUB2AP_META_DATA	0x05
#define SHUB2AP_GYRO_CAL	0x08
#define SHUB2AP_PROX_THRESH	0x09
#define SHUB2AP_REQ_RESET	0x0A
#define SHUB2AP_MAG_CAL		0x0B
#define SHUB2AP_LOG_DUMP	0x12
#define SHUB2AP_SYSTEM_INFO	0x31
#define SHUB2AP_BIG_DATA	0x51

struct shub_msg {
	u8 cmd;
	u8 type;
	u8 subcmd;
	u16 total_length;
	u16 length;
	u64 timestamp;
	char *buffer;
	bool is_empty_pending_list;
	struct completion *done;
	struct list_head list;
} __attribute__((__packed__));

#define SHUB_MSG_HEADER_SIZE	offsetof(struct shub_msg, buffer)

// FIX: comm_mutex is only used by process-context send functions, so it can remain a mutex.
struct mutex comm_mutex;

// FIX: pending_mutex is accessed by IRQ context (handle_packet) and process context (shub_send_command_wait).
// It MUST be a spinlock to prevent deadlocks.
static DEFINE_SPINLOCK(pending_mutex);

// FIX: rx_msg_mutex is accessed by IRQ context. It MUST be a spinlock.
static DEFINE_SPINLOCK(rx_msg_mutex);

struct list_head pending_list;

unsigned int cnt_timeout;
unsigned int cnt_comm_fail;

char shub_cmd_data[SHUB_CMD_SIZE];
struct shub_msg rx_msg;

static struct shub_msg *make_msg(u8 cmd, u8 type, u8 subcmd, char *send_buf, int send_buf_len)
{
	struct shub_msg *msg = kzalloc(sizeof(*msg), GFP_KERNEL);

	if (!msg) {
		shub_errf("kzalloc error");
		return NULL;
	}

	msg->cmd = cmd;
	msg->type = type;
	msg->subcmd = subcmd;
	msg->length = send_buf_len;
	msg->timestamp = get_current_timestamp();

	if (send_buf != NULL && send_buf_len != 0) {
		msg->buffer = kzalloc(send_buf_len, GFP_KERNEL);
		if (!msg->buffer) {
			kfree(msg);
			msg = NULL;
			shub_errf("kzalloc error");
			return NULL;
		}
		memcpy(msg->buffer, send_buf, send_buf_len);
	}

	return msg;
}

static void clean_msg(struct shub_msg *msg, bool buf_free)
{
	if (buf_free)
		kfree(msg->buffer);
	kfree(msg);
}

static int comm_to_sensorhub(struct shub_msg *msg)
{
	int ret;

	if (!is_shub_working()) {
		shub_errf("sensorhub is not working");
		return -EIO;
	}

	mutex_lock(&comm_mutex);
	memcpy(shub_cmd_data, msg, SHUB_MSG_HEADER_SIZE);
	if (msg->length > 0) {
		memcpy(&shub_cmd_data[SHUB_MSG_HEADER_SIZE], msg->buffer, msg->length);
	} else if (msg->length > (SHUB_CMD_SIZE - SHUB_MSG_HEADER_SIZE)) {
		shub_errf("command size(%d) is over.", msg->length);
		mutex_unlock(&comm_mutex);
		return -EINVAL;
	}

	shub_infof("cmd %d type %d subcmd %d send_buf_len %d ts %llu", msg->cmd, msg->type, msg->subcmd, msg->length,
		   msg->timestamp);

	ret = sensorhub_comms_write(shub_cmd_data, SHUB_CMD_SIZE);
	mutex_unlock(&comm_mutex);

	if (ret < 0) {
		bool is_shub_shutdown = !is_shub_working();

		cnt_comm_fail += (is_shub_shutdown) ? 0 : 1;
		shub_errf("comm write FAILED. cnt_comm_fail %d , shub_down %d ", cnt_comm_fail, is_shub_shutdown);

#ifndef CONFIG_SHUB_MTK
		reset_mcu(RESET_TYPE_KERNEL_COM_FAIL);
#endif
	}

	return ret;
}

int shub_send_command(u8 cmd, u8 type, u8 subcmd, char *send_buf, int send_buf_len)
{
	int ret = 0;
	struct shub_msg *msg = make_msg(cmd, type, subcmd, send_buf, send_buf_len);

	if (msg == NULL)
		return -EINVAL;

	ret = comm_to_sensorhub(msg);
	if (ret < 0)
		shub_errf("comm_to_sensorhub FAILED.");

	clean_msg(msg, true);

	return ret;
}

int shub_send_command_wait(u8 cmd, u8 type, u8 subcmd, int timeout, char *send_buf, int send_buf_len,
			   char **receive_buf, int *receive_buf_len, bool reset)
{
	int ret = 0;
	DECLARE_COMPLETION_ONSTACK(done);
	struct shub_msg *msg;
	unsigned long flags;

	if (cmd != CMD_GETVALUE) {
		shub_errf("invalid command %d", cmd);
		return -EINVAL;
	}

	if (receive_buf == NULL || receive_buf_len == NULL)
		return -EINVAL;

	msg = make_msg(cmd, type, subcmd, send_buf, send_buf_len);
	if (msg == NULL)
		return -EINVAL;

	msg->done = &done;

	// FIX: Use IRQ-safe spinlock
	spin_lock_irqsave(&pending_mutex, flags);
	list_add_tail(&msg->list, &pending_list);
	spin_unlock_irqrestore(&pending_mutex, flags);

	ret = comm_to_sensorhub(msg);
	if (ret < 0) {
		shub_errf("comm_to_sensorhub FAILED.");

		spin_lock_irqsave(&pending_mutex, flags);
		list_del(&msg->list);
		spin_unlock_irqrestore(&pending_mutex, flags);
		goto exit;
	}

	ret = wait_for_completion_timeout(msg->done, msecs_to_jiffies(timeout));

	if (msg->is_empty_pending_list) {
		shub_errf("pending list is empty");
		msg->is_empty_pending_list = false;
		goto exit;
	}

	/* when timeout happen */
	if (!ret) {
		bool is_shub_shutdown = !is_shub_working();

		msg->done = NULL;
		spin_lock_irqsave(&pending_mutex, flags);
		list_del(&msg->list);
		spin_unlock_irqrestore(&pending_mutex, flags);
		cnt_timeout += (is_shub_shutdown) ? 0 : 1;

		shub_errf("timeout(%d %d %d). cnt_timeout %d, shub_down %d", cmd, type, subcmd, cnt_timeout,
			  is_shub_shutdown);
		ret = -EIO;
#ifndef CONFIG_SHUB_MTK
		if (reset)
			reset_mcu(RESET_TYPE_KERNEL_COM_FAIL);
#endif
	} else {
		if (msg->length != 0) {
			*receive_buf = msg->buffer;
			*receive_buf_len = msg->length;
		} else {
			ret = -EINVAL;
		}
	}

exit:
	clean_msg(msg, false);
	return ret;
}

void clean_pending_list(void)
{
	struct shub_msg *msg, *n;
	unsigned long flags;

	shub_infof("");

	spin_lock_irqsave(&pending_mutex, flags);
	list_for_each_entry_safe(msg, n, &pending_list, list) {
		list_del(&msg->list);
		if (msg->done != NULL && !completion_done(msg->done)) {
			msg->is_empty_pending_list = 1;
			complete(msg->done);
		}
	}
	spin_unlock_irqrestore(&pending_mutex, flags);
}

// This function is now safe. The fixes from the commit were good.
static int parse_dataframe(char *dataframe, int frame_len)
{
	int index = 0;
	int ret = 0;
	struct shub_sensor *sensor;

	if (!is_shub_working()) {
		shub_infof("ssp shutdown, do not parse");
		return 0;
	}

	while (index < frame_len) {
		int cmd;
		int reset_type, no_event_type;
		int prev_index = index;

		cmd = dataframe[index++];
		if (index > frame_len) {
			shub_errf("Read past buffer end trying to get cmd\n");
			ret = -EINVAL;
			break;
		}

		switch (cmd) {
		case SHUB2AP_DEBUG_DATA:
			ret = print_mcu_debug(dataframe, &index, frame_len);
			break;
		case SHUB2AP_BYPASS_DATA:
			ret = parsing_bypass_data(dataframe, &index, frame_len);
			break;
		case SHUB2AP_META_DATA:
			ret = parsing_meta_data(dataframe, &index, frame_len);
			break;
		case SHUB2AP_LIBRARY_DATA:
			ret = parsing_scontext_data(dataframe, &index, frame_len);
			break;
		case SHUB2AP_GYRO_CAL:
			sensor = get_sensor(SENSOR_TYPE_GYROSCOPE);
			if (sensor && sensor->funcs && sensor->funcs->parsing_data)
				ret = sensor->funcs->parsing_data(dataframe, &index, frame_len);
			else
				shub_errf("No parser for GYRO_CAL\n");
			break;
		case SHUB2AP_MAG_CAL:
			sensor = get_sensor(SENSOR_TYPE_GEOMAGNETIC_FIELD);
			if (sensor && sensor->funcs && sensor->funcs->parsing_data)
				ret = sensor->funcs->parsing_data(dataframe, &index, frame_len);
			else
				shub_errf("No parser for MAG_CAL\n");
			break;
		case SHUB2AP_SYSTEM_INFO:
			ret = print_system_info(dataframe + index, &index, frame_len);
			break;
		case SHUB2AP_REQ_RESET:
			if (index + 2 <= frame_len) {
				reset_type = dataframe[index++];
				no_event_type = dataframe[index++];
				if (reset_type == HUB_RESET_REQ_NO_EVENT) {
					shub_infof("Hub request reset[0x%x] No Event type %d", reset_type, no_event_type);
					reset_mcu(RESET_TYPE_HUB_NO_EVENT);
				} else if (reset_type == HUB_RESET_REQ_TASK_FAILURE) {
					shub_infof("Hub request reset[0x%x] request task failure", reset_type);
					reset_mcu(RESET_TYPE_HUB_REQ_TASK_FAILURE);
				} else {
					shub_infof("Hub request rest[0x%x] invalid request", reset_type);
				}
			} else {
				shub_errf("parsing error");
				ret = -EINVAL;
			}
			break;
		case SHUB2AP_PROX_THRESH:
			sensor = get_sensor(SENSOR_TYPE_PROXIMITY);
			if (sensor && sensor->funcs && sensor->funcs->parsing_data)
				ret = sensor->funcs->parsing_data(dataframe, &index, frame_len);
			else
				shub_errf("No parser for PROX_THRESH\n");
			break;
		case SHUB2AP_LOG_DUMP:
			ret = save_log_dump(dataframe, &index, frame_len);
			break;
		case SHUB2AP_BIG_DATA:
			ret = parsing_big_data(dataframe, &index, frame_len);
			break;
		default:
			shub_errf("0x%x cmd doesn't support, index = %d\n", cmd, index);
			ret = -EOPNOTSUPP;
			break;
		}

		if (index > frame_len) {
			shub_errf("CRITICAL: Buffer over-read detected after cmd 0x%x. index=%d, frame_len=%d\n",
				  cmd, index, frame_len);
			ret = -EINVAL;
		}

		if (index <= prev_index) {
			shub_errf("CRITICAL: Index did not advance for cmd 0x%x. Possible infinite loop. index=%d\n",
				  cmd, index);
			ret = -EINVAL;
		}

		if (ret < 0)
			break;
	}

	if (ret < 0) {
		shub_errf("Error during dataframe parsing. Dumping buffer.\n");
		print_dataframe(dataframe, frame_len);
	}

	return ret;
}

// FIX: Corrected function to be IRQ-safe
int get_shub_msg_big_buffer(struct shub_msg *msg, char *packet, int packet_size)
{
	int ret = 0;
	unsigned long flags;

	// FIX: Use spinlock instead of mutex
	spin_lock_irqsave(&rx_msg_mutex, flags);

	if (rx_msg.timestamp != msg->timestamp) {
		kfree(rx_msg.buffer);
		memcpy(&rx_msg, msg, SHUB_MSG_HEADER_SIZE);
		rx_msg.length = 0;
		// FIX: Use GFP_ATOMIC for allocation in IRQ context
		rx_msg.buffer = kzalloc(rx_msg.total_length, GFP_ATOMIC);
		if (ZERO_OR_NULL_PTR(rx_msg.buffer)) {
			shub_errf("fail to alloc memory for total buffer(%d %d %d)", msg->cmd, msg->type,
					msg->subcmd);
			ret = -ENOMEM;
			goto msg_alloc_error;
		}
	}

	if (rx_msg.length + msg->length > rx_msg.total_length) {
		shub_errf("length error. cmd %d, type %d, sub_cmd %d, total %d, len %d", rx_msg.cmd,
			rx_msg.type, rx_msg.subcmd, rx_msg.total_length, rx_msg.length + msg->length);
		ret = -EINVAL;
		goto msg_error;
	}

	memcpy(&rx_msg.buffer[rx_msg.length], packet + SHUB_MSG_HEADER_SIZE, msg->length);
	rx_msg.length += msg->length;

	if (rx_msg.length == rx_msg.total_length) {
		msg->length = rx_msg.length;
		msg->buffer = rx_msg.buffer;
		memset(&rx_msg, 0, sizeof(struct shub_msg));
		ret = 0;
	} else {
		// This is an intermediate packet. Indicate that we are still processing.
		ret = 1;
	}

	spin_unlock_irqrestore(&rx_msg_mutex, flags);
	return ret;

msg_error:
	kfree(rx_msg.buffer);
msg_alloc_error:
	memset(&rx_msg, 0, sizeof(struct shub_msg));
	spin_unlock_irqrestore(&rx_msg_mutex, flags);
	return ret;
}

int get_shub_msg_buffer(struct shub_msg *msg, char *packet, int packet_size)
{
	int ret = 0;

	if (msg->total_length != msg->length) {
		ret = get_shub_msg_big_buffer(msg, packet, packet_size);
	} else {
		// FIX: Ensure GFP_ATOMIC is used for all allocations in this path.
		msg->buffer = kzalloc(msg->length, GFP_ATOMIC);
		if (ZERO_OR_NULL_PTR(msg->buffer)) {
			shub_errf("fail to alloc memory for msg buffer(%d %d %d)", msg->cmd, msg->type, msg->subcmd);
			return -ENOMEM;
		}

		memcpy(msg->buffer, &packet[SHUB_MSG_HEADER_SIZE], msg->length);
	}

	return ret;
}

int get_shub_msg(struct shub_msg *msg, char *packet, int packet_size)
{
	int ret = 0;

	if (packet_size < SHUB_MSG_HEADER_SIZE) {
		shub_infof("packet size is small/(%s)", packet);
		return -EINVAL;
	}

	memcpy(msg, packet, SHUB_MSG_HEADER_SIZE);

	if (msg->total_length)
		ret = get_shub_msg_buffer(msg, packet, packet_size);

	return ret;
}

struct shub_msg *get_msg_from_pending_list(struct shub_msg msg)
{
	struct shub_msg *m, *n;
	struct shub_msg *found_msg = NULL;
	unsigned long flags;

	// FIX: Use IRQ-safe spinlock
	spin_lock_irqsave(&pending_mutex, flags);
	if (!list_empty(&pending_list)) {
		list_for_each_entry_safe(m, n, &pending_list, list) {
			if ((m->cmd == msg.cmd) && (m->type == msg.type) && (m->subcmd == msg.subcmd)) {
				list_del(&m->list);
				found_msg = m;
				break;
			}
		}

		if (!found_msg)
			shub_errf("%d %d %d - Not match error", msg.cmd, msg.type, msg.subcmd);
	} else {
		shub_errf("List empty error(%d %d %d)", msg.cmd, msg.type, msg.subcmd);
	}

	spin_unlock_irqrestore(&pending_mutex, flags);

	return found_msg;
}

void handle_packet(char *packet, int packet_size)
{
	struct shub_msg msg;
	int ret;

#ifdef CONFIG_SHUB_DEBUG
	if (check_debug_log_state(SHUB_LOG_DATA_PACKET))
		print_dataframe(packet, packet_size);
#endif

	ret = get_shub_msg(&msg, packet, packet_size);
	// FIX: Handle the case where a large message is still being assembled
	if (ret > 0) {
		// Intermediate packet for a big message was handled. Do nothing.
		return;
	} else if (ret < 0) {
		shub_errf("get_shub_msg failed with error %d\n", ret);
		return;
	}

	// If get_shub_msg returns 0, we now own msg.buffer and must free it.
	if (msg.cmd == CMD_GETVALUE) {
		struct shub_msg *pending_msg;

		pending_msg = get_msg_from_pending_list(msg);
		if (pending_msg) {
			kfree(pending_msg->buffer);
			pending_msg->buffer = msg.buffer; // Transfer ownership
			pending_msg->length = msg.length;

			if (pending_msg->done != NULL && !completion_done(pending_msg->done))
				complete(pending_msg->done);
		} else {
			shub_errf("No pending message for CMD_GETVALUE, freeing buffer.\n");
			kfree(msg.buffer);
		}
	} else if (msg.cmd == CMD_REPORT) {
		ret = parse_dataframe(msg.buffer, msg.length);
		if (ret < 0) {
			shub_errf("parse_dataframe failed with error %d\n", ret);
		}

		kfree(msg.buffer);
	} else {
		shub_errf("Unknown msg_cmd: %d, packet size %d\n", msg.cmd, packet_size);
		print_dataframe(packet, packet_size);
		kfree(msg.buffer);
	}
}

int get_cnt_comm_fail(void)
{
	return cnt_comm_fail;
}

int get_cnt_timeout(void)
{
	return cnt_timeout;
}

void stop_comm_to_hub(void)
{
	clean_pending_list();
}

int init_comm_to_hub(void)
{
	mutex_init(&comm_mutex);
	// FIX: Initialize spinlocks instead of mutexes
	spin_lock_init(&pending_mutex);
	spin_lock_init(&rx_msg_mutex);

	INIT_LIST_HEAD(&pending_list);

	cnt_timeout = 0;
	cnt_comm_fail = 0;

	memset(shub_cmd_data, 0, SHUB_CMD_SIZE);
	return 0;
}

void exit_comm_to_hub(void)
{
	clean_pending_list();
	mutex_destroy(&comm_mutex);
	// No destroy function for statically defined spinlocks
}