// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020, Intel Corporation
 */

#include <linux/arm-smccc.h>
#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/firmware/intel/stratix10-svc-client.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>

#include <uapi/linux/intel_fcs-ioctl.h>

#define RANDOM_NUMBER_SIZE	32
#define FILE_NAME_SIZE		32
#define PS_BUF_SIZE		64
#define SHA384_SIZE		48
#define INVALID_STATUS		0xffffffff
#define INVALID_CID		0xffffffff

#define DEC_MIN_SZ		72
#define DEC_MAX_SZ		32712
#define ENC_MIN_SZ		120
#define ENC_MAX_SZ		32760

#define SUBKEY_CMD_MAX_SZ	4092
#define SUBKEY_RSP_MAX_SZ	820
#define MEASUREMENT_CMD_MAX_SZ	4092
#define MEASUREMENT_RSP_MAX_SZ	4092
#define CERTIFICATE_RSP_MAX_SZ	4096

#define SIGMA_SESSION_ID_ONE	0x1
#define SIGMA_UNKNOWN_SESSION	0xffffffff

#define FCS_REQUEST_TIMEOUT (msecs_to_jiffies(SVC_FCS_REQUEST_TIMEOUT_MS))
#define FCS_COMPLETED_TIMEOUT (msecs_to_jiffies(SVC_COMPLETED_TIMEOUT_MS))

typedef void (*fcs_callback)(struct stratix10_svc_client *client,
			     struct stratix10_svc_cb_data *data);

struct intel_fcs_priv {
	struct stratix10_svc_chan *chan;
	struct stratix10_svc_client client;
	struct completion completion;
	struct mutex lock;
	struct miscdevice miscdev;
	unsigned int status;
	void *kbuf;
	unsigned int size;
	unsigned int cid_low;
	unsigned int cid_high;
};

static void fcs_data_callback(struct stratix10_svc_client *client,
			      struct stratix10_svc_cb_data *data)
{
	struct intel_fcs_priv *priv = client->priv;

	if ((data->status == BIT(SVC_STATUS_OK)) ||
	    (data->status == BIT(SVC_STATUS_COMPLETED))) {
		priv->status = 0;
		priv->kbuf = data->kaddr2;
		priv->size = *((unsigned int *)data->kaddr3);
	} else if (data->status == BIT(SVC_STATUS_ERROR)) {
		priv->status = *((unsigned int *)data->kaddr1);
		dev_err(client->dev, "error, mbox_error=0x%x\n", priv->status);
		priv->kbuf = data->kaddr2;
		priv->size = (data->kaddr3) ?
			*((unsigned int *)data->kaddr3) : 0;
	} else {
		dev_err(client->dev, "rejected, invalid param\n");
		priv->status = -EINVAL;
		priv->kbuf = NULL;
		priv->size = 0;
	}

	complete(&priv->completion);
}

static void fcs_vab_callback(struct stratix10_svc_client *client,
			     struct stratix10_svc_cb_data *data)
{
	struct intel_fcs_priv *priv = client->priv;

	if (data->status == BIT(SVC_STATUS_ERROR)) {
		priv->status = *((unsigned int *)data->kaddr1);
		dev_err(client->dev, "mbox_error=0x%x\n", priv->status);
	} else if (data->status == BIT(SVC_STATUS_BUSY)) {
		priv->status = -ETIMEDOUT;
		dev_err(client->dev, "timeout to get completed status\n");
	} else if (data->status == BIT(SVC_STATUS_INVALID_PARAM)) {
		priv->status = -EINVAL;
		dev_err(client->dev, "request rejected\n");
	} else if (data->status == BIT(SVC_STATUS_OK)) {
		priv->status = 0;
	} else {
		priv->status = -EINVAL;
		dev_err(client->dev, "rejected, invalid param\n");
	}

	complete(&priv->completion);
}

static void fcs_chipid_callback(struct stratix10_svc_client *client,
				struct stratix10_svc_cb_data *data)
{
	struct intel_fcs_priv *priv = client->priv;

	priv->status = data->status;
	if (data->status == BIT(SVC_STATUS_OK)) {
		priv->status = 0;
		priv->cid_low = *((unsigned int *)data->kaddr2);
		priv->cid_high = *((unsigned int *)data->kaddr3);
	} else if (data->status == BIT(SVC_STATUS_ERROR)) {
		priv->status = *((unsigned int *)data->kaddr1);
		dev_err(client->dev, "mbox_error=0x%x\n", priv->status);
	}

	complete(&priv->completion);
}

static void fcs_attestation_callback(struct stratix10_svc_client *client,
				     struct stratix10_svc_cb_data *data)
{
	struct intel_fcs_priv *priv = client->priv;

	priv->status = data->status;
	if (data->status == BIT(SVC_STATUS_OK)) {
		priv->status = 0;
		priv->kbuf = data->kaddr2;
		priv->size = *((unsigned int *)data->kaddr3);
	} else if (data->status == BIT(SVC_STATUS_ERROR)) {
		priv->status = *((unsigned int *)data->kaddr1);
		dev_err(client->dev, "mbox_error=0x%x\n", priv->status);
	}

	complete(&priv->completion);
}

static int fcs_request_service(struct intel_fcs_priv *priv,
			       void *msg, unsigned long timeout)
{
	struct stratix10_svc_client_msg *p_msg =
			(struct stratix10_svc_client_msg *)msg;
	int ret;

	mutex_lock(&priv->lock);
	reinit_completion(&priv->completion);

	ret = stratix10_svc_send(priv->chan, p_msg);
	if (ret)
		return -EINVAL;

	ret = wait_for_completion_timeout(&priv->completion,
							timeout);
	if (!ret) {
		dev_err(priv->client.dev,
			"timeout waiting for SMC call\n");
		ret = -ETIMEDOUT;
	} else
		ret = 0;

	mutex_unlock(&priv->lock);
	return ret;
}

static void fcs_close_services(struct intel_fcs_priv *priv,
			       void *sbuf, void *dbuf)
{
	if (sbuf)
		stratix10_svc_free_memory(priv->chan, sbuf);

	if (dbuf)
		stratix10_svc_free_memory(priv->chan, dbuf);

	stratix10_svc_done(priv->chan);
}

static long fcs_ioctl(struct file *file, unsigned int cmd,
		      unsigned long arg)
{
	struct intel_fcs_dev_ioctl *data;
	struct intel_fcs_priv *priv;
	struct device *dev;
	struct stratix10_svc_client_msg *msg;
	const struct firmware *fw;
	char filename[FILE_NAME_SIZE];
	size_t tsz, rsz, datasz;
	uint32_t sid;
	void *s_buf;
	void *d_buf;
	void *ps_buf;
	unsigned int buf_sz;
	int ret = 0;
	int i;

	priv = container_of(file->private_data, struct intel_fcs_priv, miscdev);
	dev = priv->client.dev;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	msg = devm_kzalloc(dev, sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	switch (cmd) {
	case INTEL_FCS_DEV_VALIDATION_REQUEST:
		if (copy_from_user(data, (void __user *)arg, sizeof(*data))) {
			dev_err(dev, "failure on copy_from_user\n");
			return -EFAULT;
		}

		/* for bitstream */
		dev_dbg(dev, "file_name=%s, status=%d\n",
			 (char *)data->com_paras.s_request.src, data->status);
		scnprintf(filename, FILE_NAME_SIZE, "%s",
				(char *)data->com_paras.s_request.src);
		ret = request_firmware(&fw, filename, priv->client.dev);
		if (ret) {
			dev_err(dev, "error requesting firmware %s\n",
				(char *)data->com_paras.s_request.src);
			return -EFAULT;
		}

		dev_dbg(dev, "FW size=%ld\n", fw->size);
		s_buf = stratix10_svc_allocate_memory(priv->chan, fw->size);
		if (!s_buf) {
			dev_err(dev, "failed to allocate VAB buffer\n");
			release_firmware(fw);
			return -ENOMEM;
		}

		memcpy(s_buf, fw->data, fw->size);

		msg->payload_length = fw->size;
		release_firmware(fw);

		msg->command = COMMAND_FCS_REQUEST_SERVICE;
		msg->payload = s_buf;
		priv->client.receive_cb = fcs_vab_callback;

		ret = fcs_request_service(priv, (void *)msg,
					  FCS_REQUEST_TIMEOUT);
		dev_dbg(dev, "fcs_request_service ret=%d\n", ret);
		if (!ret && !priv->status) {
			/* to query the complete status */
			msg->command = COMMAND_POLL_SERVICE_STATUS;
			priv->client.receive_cb = fcs_data_callback;
			ret = fcs_request_service(priv, (void *)msg,
						  FCS_COMPLETED_TIMEOUT);
			dev_dbg(dev, "fcs_request_service ret=%d\n", ret);
			if (!ret && !priv->status)
				data->status = 0;
			else
				data->status = priv->status;
		} else
			data->status = priv->status;

		if (copy_to_user((void __user *)arg, data, sizeof(*data))) {
			dev_err(dev, "failure on copy_to_user\n");
			fcs_close_services(priv, s_buf, NULL);
			ret = -EFAULT;
		}

		fcs_close_services(priv, s_buf, NULL);
		break;

	case INTEL_FCS_DEV_SEND_CERTIFICATE:
		if (copy_from_user(data, (void __user *)arg, sizeof(*data))) {
			dev_err(dev, "failure on copy_from_user\n");
			return -EFAULT;
		}

		dev_dbg(dev, "Test=%d, Size=%d; Address=0x%p\n",
			data->com_paras.c_request.test.test_word,
			data->com_paras.c_request.size,
			data->com_paras.c_request.addr);

		/* Allocate memory for certificate + test word */
		tsz = sizeof(struct intel_fcs_cert_test_word);
		datasz = data->com_paras.s_request.size + tsz;

		s_buf = stratix10_svc_allocate_memory(priv->chan, datasz);
		if (!s_buf) {
			dev_err(dev, "failed to allocate VAB buffer\n");
			return -ENOMEM;
		}

		ps_buf = stratix10_svc_allocate_memory(priv->chan, PS_BUF_SIZE);
		if (!ps_buf) {
			dev_err(dev, "failed to allocate p-status buf\n");
			stratix10_svc_free_memory(priv->chan, s_buf);
			return -ENOMEM;
		}

		/* Copy the test word */
		memcpy(s_buf, &data->com_paras.c_request.test, tsz);

		/* Copy in the certificate data (skipping over the test word) */
		ret = copy_from_user(s_buf + tsz,
				     data->com_paras.c_request.addr,
				     data->com_paras.s_request.size);
		if (ret) {
			dev_err(dev, "failed copy buf ret=%d\n", ret);
			fcs_close_services(priv, s_buf, ps_buf);
			return -EFAULT;
		}

		msg->payload_length = datasz;
		msg->command = COMMAND_FCS_SEND_CERTIFICATE;
		msg->payload = s_buf;
		priv->client.receive_cb = fcs_vab_callback;

		ret = fcs_request_service(priv, (void *)msg,
					  FCS_REQUEST_TIMEOUT);
		dev_dbg(dev, "fcs_request_service ret=%d\n", ret);
		if (!ret && !priv->status) {
			/* to query the complete status */
			msg->payload = ps_buf;
			msg->payload_length = PS_BUF_SIZE;
			msg->command = COMMAND_POLL_SERVICE_STATUS;
			priv->client.receive_cb = fcs_data_callback;
			ret = fcs_request_service(priv, (void *)msg,
						  FCS_COMPLETED_TIMEOUT);
			dev_dbg(dev, "request service ret=%d\n", ret);
			if (!ret && !priv->status)
				data->status = 0;
			else {
				if (priv->kbuf)
					data->com_paras.c_request.c_status =
						(*(u32 *)priv->kbuf);
				else
					data->com_paras.c_request.c_status =
						INVALID_STATUS;
			}
		} else
			data->status = priv->status;

		if (copy_to_user((void __user *)arg, data, sizeof(*data))) {
			dev_err(dev, "failure on copy_to_user\n");
			fcs_close_services(priv, s_buf, NULL);
			ret = -EFAULT;
		}

		fcs_close_services(priv, s_buf, ps_buf);
		break;

	case INTEL_FCS_DEV_COUNTER_SET_PREAUTHORIZED:
		if (copy_from_user(data, (void __user *)arg, sizeof(*data))) {
			dev_err(dev, "failure on copy_from_user\n");
			return -EFAULT;
		}

		msg->command = COMMAND_FCS_COUNTER_SET_PREAUTHORIZED;
		msg->arg[0] = data->com_paras.i_request.counter_type;
		msg->arg[1] = data->com_paras.i_request.counter_value;
		msg->arg[2] = data->com_paras.i_request.test.test_word;
		priv->client.receive_cb = fcs_vab_callback;

		ret = fcs_request_service(priv, (void *)msg,
					  FCS_REQUEST_TIMEOUT);
		if (ret) {
			dev_err(dev, "failed to send the request,ret=%d\n",
				ret);
			return -EFAULT;
		}

		data->status = priv->status;
		if (copy_to_user((void __user *)arg, data, sizeof(*data))) {
			dev_err(dev, "failure on copy_to_user\n");
			ret = -EFAULT;
		}
		break;

	case INTEL_FCS_DEV_RANDOM_NUMBER_GEN:
		if (copy_from_user(data, (void __user *)arg, sizeof(*data))) {
			dev_err(dev, "failure on copy_from_user\n");
			return -EFAULT;
		}

		s_buf = stratix10_svc_allocate_memory(priv->chan,
						      RANDOM_NUMBER_SIZE);
		if (!s_buf) {
			dev_err(dev, "failed to allocate RNG buffer\n");
			return -ENOMEM;
		}

		msg->command = COMMAND_FCS_RANDOM_NUMBER_GEN;
		msg->payload = s_buf;
		msg->payload_length = RANDOM_NUMBER_SIZE;
		priv->client.receive_cb = fcs_data_callback;

		ret = fcs_request_service(priv, (void *)msg,
					  FCS_REQUEST_TIMEOUT);

		if (!ret && !priv->status) {
			if (!priv->kbuf) {
				dev_err(dev, "failure on kbuf\n");
				fcs_close_services(priv, s_buf, NULL);
				return -EFAULT;
			}

			for (i = 0; i < 8; i++)
				dev_dbg(dev, "output_data[%d]=%d\n", i,
					 *((int *)priv->kbuf + i));

			for (i = 0; i < 8; i++)
				data->com_paras.rn_gen.rndm[i] =
					*((int *)priv->kbuf + i);
			data->status = priv->status;

		} else {
			/* failed to get RNG */
			data->status = priv->status;
		}

		if (copy_to_user((void __user *)arg, data, sizeof(*data))) {
			dev_err(dev, "failure on copy_to_user\n");
			fcs_close_services(priv, s_buf, NULL);
			ret = -EFAULT;
		}

		fcs_close_services(priv, s_buf, NULL);
		break;

	case INTEL_FCS_DEV_GET_PROVISION_DATA:
		if (copy_from_user(data, (void __user *)arg,
				   sizeof(*data))) {
			dev_err(dev, "failure on copy_from_user\n");
			return -EFAULT;
		}

		s_buf = stratix10_svc_allocate_memory(priv->chan,
					data->com_paras.gp_data.size);
		if (!s_buf) {
			dev_err(dev, "failed allocate provision buffer\n");
			return -ENOMEM;
		}

		msg->command = COMMAND_FCS_GET_PROVISION_DATA;
		msg->payload = s_buf;
		msg->payload_length = data->com_paras.gp_data.size;
		priv->client.receive_cb = fcs_data_callback;

		ret = fcs_request_service(priv, (void *)msg,
					  FCS_REQUEST_TIMEOUT);
		if (!ret && !priv->status) {
			if (!priv->kbuf) {
				dev_err(dev, "failure on kbuf\n");
				fcs_close_services(priv, s_buf, NULL);
				return -EFAULT;
			}
			data->com_paras.gp_data.size = priv->size;
			ret = copy_to_user(data->com_paras.gp_data.addr,
					   priv->kbuf, priv->size);
			if (ret) {
				dev_err(dev, "failure on copy_to_user\n");
				fcs_close_services(priv, s_buf, NULL);
				return -EFAULT;
			}
			data->status = 0;
		} else {
			data->com_paras.gp_data.addr = NULL;
			data->com_paras.gp_data.size = 0;
			data->status = priv->status;
		}

		if (copy_to_user((void __user *)arg, data, sizeof(*data))) {
			dev_err(dev, "failure on copy_to_user\n");
			fcs_close_services(priv, s_buf, NULL);
			return -EFAULT;
		}

		fcs_close_services(priv, s_buf, NULL);
		break;

	case INTEL_FCS_DEV_DATA_ENCRYPTION:
		if (copy_from_user(data, (void __user *)arg, sizeof(*data))) {
			dev_err(dev, "failure on copy_from_user\n");
			return -EFAULT;
		}

		if (data->com_paras.d_encryption.src_size < DEC_MIN_SZ ||
		    data->com_paras.d_encryption.src_size > DEC_MAX_SZ) {
			dev_err(dev, "Invalid SDOS Buffer src size:%d\n",
				data->com_paras.d_encryption.src_size);
			return -EFAULT;
		}

		if (data->com_paras.d_encryption.dst_size < ENC_MIN_SZ ||
		    data->com_paras.d_encryption.dst_size > ENC_MAX_SZ) {
			dev_err(dev, "Invalid SDOS Buffer dst size:%d\n",
				data->com_paras.d_encryption.dst_size);
			return -EFAULT;
		}

		/* allocate buffer for both source and destination */
		s_buf = stratix10_svc_allocate_memory(priv->chan,
						      DEC_MAX_SZ);
		if (!s_buf) {
			dev_err(dev, "failed allocate encrypt src buf\n");
			return -ENOMEM;
		}
		d_buf = stratix10_svc_allocate_memory(priv->chan,
						      ENC_MAX_SZ);
		if (!d_buf) {
			dev_err(dev, "failed allocate encrypt dst buf\n");
			stratix10_svc_free_memory(priv->chan, s_buf);
			return -ENOMEM;
		}
		ps_buf = stratix10_svc_allocate_memory(priv->chan, PS_BUF_SIZE);
		if (!ps_buf) {
			dev_err(dev, "failed allocate p-status buffer\n");
			fcs_close_services(priv, s_buf, d_buf);
			return -ENOMEM;
		}
		ret = copy_from_user(s_buf,
				     data->com_paras.d_encryption.src,
				     data->com_paras.d_encryption.src_size);
		if (ret) {
			dev_err(dev, "failure on copy_from_user\n");
			fcs_close_services(priv, ps_buf, NULL);
			fcs_close_services(priv, s_buf, d_buf);
			return -ENOMEM;
		}

		msg->command = COMMAND_FCS_DATA_ENCRYPTION;
		msg->payload = s_buf;
		msg->payload_length =
			data->com_paras.d_encryption.src_size;
		msg->payload_output = d_buf;
		msg->payload_length_output =
			data->com_paras.d_encryption.dst_size;
		priv->client.receive_cb = fcs_vab_callback;

		ret = fcs_request_service(priv, (void *)msg,
					  FCS_REQUEST_TIMEOUT);
		if (!ret && !priv->status) {
			msg->payload = ps_buf;
			msg->payload_length = PS_BUF_SIZE;
			msg->command = COMMAND_POLL_SERVICE_STATUS;

			priv->client.receive_cb = fcs_data_callback;
			ret = fcs_request_service(priv, (void *)msg,
						  FCS_COMPLETED_TIMEOUT);
			dev_dbg(dev, "request service ret=%d\n", ret);

			if (!ret && !priv->status) {
				if (!priv->kbuf) {
					dev_err(dev, "failure on kbuf\n");
					fcs_close_services(priv, ps_buf, NULL);
					fcs_close_services(priv, s_buf, d_buf);
					return -EFAULT;
				}
				buf_sz = *(unsigned int *)priv->kbuf;
				data->com_paras.d_encryption.dst_size = buf_sz;
				data->status = 0;
				ret = copy_to_user(data->com_paras.d_encryption.dst,
						   d_buf, buf_sz);
				if (ret) {
					dev_err(dev, "failure on copy_to_user\n");
					fcs_close_services(priv, ps_buf, NULL);
					fcs_close_services(priv, s_buf, d_buf);
					return -EFAULT;
				}
			} else {
				data->com_paras.d_encryption.dst = NULL;
				data->com_paras.d_encryption.dst_size = 0;
				data->status = priv->status;
			}
		} else {
			data->com_paras.d_encryption.dst = NULL;
			data->com_paras.d_encryption.dst_size = 0;
			data->status = priv->status;
		}

		if (copy_to_user((void __user *)arg, data, sizeof(*data))) {
			dev_err(dev, "failure on copy_to_user\n");
			fcs_close_services(priv, ps_buf, NULL);
			fcs_close_services(priv, s_buf, d_buf);
			ret = -EFAULT;
		}

		fcs_close_services(priv, ps_buf, NULL);
		fcs_close_services(priv, s_buf, d_buf);
		break;

	case INTEL_FCS_DEV_DATA_DECRYPTION:
		if (copy_from_user(data, (void __user *)arg, sizeof(*data))) {
			dev_err(dev, "failure on copy_from_user\n");
			return -EFAULT;
		}

		if (data->com_paras.d_encryption.src_size < ENC_MIN_SZ ||
		    data->com_paras.d_encryption.src_size > ENC_MAX_SZ) {
			dev_err(dev, "Invalid SDOS Buffer src size:%d\n",
				data->com_paras.d_encryption.src_size);
			return -EFAULT;
		}

		if (data->com_paras.d_encryption.dst_size < DEC_MIN_SZ ||
		    data->com_paras.d_encryption.dst_size > DEC_MAX_SZ) {
			dev_err(dev, "Invalid SDOS Buffer dst size:%d\n",
				data->com_paras.d_encryption.dst_size);
			return -EFAULT;
		}

		/* allocate buffer for both source and destination */
		s_buf = stratix10_svc_allocate_memory(priv->chan,
						      ENC_MAX_SZ);
		if (!s_buf) {
			dev_err(dev, "failed allocate decrypt src buf\n");
			return -ENOMEM;
		}
		d_buf = stratix10_svc_allocate_memory(priv->chan,
						      DEC_MAX_SZ);
		if (!d_buf) {
			dev_err(dev, "failed allocate decrypt dst buf\n");
			stratix10_svc_free_memory(priv->chan, s_buf);
			return -ENOMEM;
		}

		ps_buf = stratix10_svc_allocate_memory(priv->chan,
						       PS_BUF_SIZE);
		if (!ps_buf) {
			dev_err(dev, "failed allocate p-status buffer\n");
			fcs_close_services(priv, s_buf, d_buf);
			return -ENOMEM;
		}

		ret = copy_from_user(s_buf,
				     data->com_paras.d_decryption.src,
				     data->com_paras.d_decryption.src_size);
		if (ret) {
			dev_err(dev, "failure on copy_from_user\n");
			fcs_close_services(priv, ps_buf, NULL);
			fcs_close_services(priv, s_buf, d_buf);
			return -EFAULT;
		}

		msg->command = COMMAND_FCS_DATA_DECRYPTION;
		msg->payload = s_buf;
		msg->payload_length =
				data->com_paras.d_decryption.src_size;
		msg->payload_output = d_buf;
		msg->payload_length_output =
				data->com_paras.d_decryption.dst_size;
		priv->client.receive_cb = fcs_vab_callback;

		ret = fcs_request_service(priv, (void *)msg,
					  FCS_REQUEST_TIMEOUT);
		if (!ret && !priv->status) {
			msg->command = COMMAND_POLL_SERVICE_STATUS;
			msg->payload = ps_buf;
			msg->payload_length = PS_BUF_SIZE;
			priv->client.receive_cb = fcs_data_callback;
			ret = fcs_request_service(priv, (void *)msg,
						  FCS_COMPLETED_TIMEOUT);
			dev_dbg(dev, "request service ret=%d\n", ret);
			if (!ret && !priv->status) {
				if (!priv->kbuf) {
					dev_err(dev, "failure on kbuf\n");
					fcs_close_services(priv, ps_buf, NULL);
					fcs_close_services(priv, s_buf, d_buf);
					return -EFAULT;
				}
				buf_sz = *((unsigned int *)priv->kbuf);
				data->com_paras.d_decryption.dst_size = buf_sz;
				data->status = 0;
				ret = copy_to_user(data->com_paras.d_decryption.dst,
						   d_buf, buf_sz);
				if (ret) {
					dev_err(dev, "failure on copy_to_user\n");
					fcs_close_services(priv, ps_buf, NULL);
					fcs_close_services(priv, s_buf, d_buf);
					return -EFAULT;
				}
			} else {
				data->com_paras.d_decryption.dst = NULL;
				data->com_paras.d_decryption.dst_size = 0;
				data->status = priv->status;
			}
		} else {
			data->com_paras.d_decryption.dst = NULL;
			data->com_paras.d_decryption.dst_size = 0;
			data->status = priv->status;
		}

		if (copy_to_user((void __user *)arg, data, sizeof(*data))) {
			dev_err(dev, "failure on copy_to_user\n");
			fcs_close_services(priv, ps_buf, NULL);
			fcs_close_services(priv, s_buf, d_buf);
			ret = -EFAULT;
		}

		fcs_close_services(priv, ps_buf, NULL);
		fcs_close_services(priv, s_buf, d_buf);
		break;

	case INTEL_FCS_DEV_PSGSIGMA_TEARDOWN:
		if (copy_from_user(data, (void __user *)arg, sizeof(*data))) {
			dev_err(dev, "failure on copy_from_user\n");
			return -EFAULT;
		}

		sid = data->com_paras.tdown.sid;
		if ((sid != SIGMA_SESSION_ID_ONE) &&
			(sid != SIGMA_UNKNOWN_SESSION)) {
			dev_err(dev, "Invalid session ID:%d\n", sid);
			return -EFAULT;
		}

		msg->command = COMMAND_FCS_PSGSIGMA_TEARDOWN;
		msg->arg[0] = sid;
		priv->client.receive_cb = fcs_vab_callback;
		ret = fcs_request_service(priv, (void *)msg,
					  FCS_REQUEST_TIMEOUT);
		if (ret) {
			dev_err(dev, "failed to send the request,ret=%d\n",
				ret);
			return -EFAULT;
		}

		data->status = priv->status;
		if (copy_to_user((void __user *)arg, data, sizeof(*data))) {
			dev_err(dev, "failure on copy_to_user\n");
			ret = -EFAULT;
		}
		break;

	case INTEL_FCS_DEV_CHIP_ID:
		msg->command = COMMAND_FCS_GET_CHIP_ID;
		priv->client.receive_cb = fcs_chipid_callback;
		ret = fcs_request_service(priv, (void *)msg,
					  FCS_REQUEST_TIMEOUT);
		if (ret) {
			dev_err(dev, "failed to send the request,ret=%d\n",
				ret);
			return -EFAULT;
		}

		data->status = priv->status;
		data->com_paras.c_id.chip_id_low = priv->cid_low;
		data->com_paras.c_id.chip_id_high = priv->cid_high;
		if (copy_to_user((void __user *)arg, data, sizeof(*data))) {
			dev_err(dev, "failure on copy_to_user\n");
			ret = -EFAULT;
		}
		break;

	case INTEL_FCS_DEV_ATTESTATION_SUBKEY:
		if (copy_from_user(data, (void __user *)arg, sizeof(*data))) {
			dev_err(dev, "failure on copy_from_user\n");
			return -EFAULT;
		}

		if (data->com_paras.subkey.cmd_data_sz > SUBKEY_CMD_MAX_SZ) {
			dev_err(dev, "Invalid subkey CMD size %d\n",
				data->com_paras.subkey.cmd_data_sz);
			return -EFAULT;
		}

		if (data->com_paras.subkey.rsp_data_sz > SUBKEY_RSP_MAX_SZ) {
			dev_err(dev, "Invalid subkey RSP size %d\n",
				data->com_paras.subkey.rsp_data_sz);
			return -EFAULT;
		}

		/* allocate buffer for both soruce and destination */
		rsz = sizeof(struct intel_fcs_attestation_resv_word);
		datasz = data->com_paras.subkey.cmd_data_sz + rsz;

		s_buf = stratix10_svc_allocate_memory(priv->chan,
						      SUBKEY_CMD_MAX_SZ +
						      rsz);
		if (!s_buf) {
			dev_err(dev, "failed allocate subkey CMD buf\n");
			return -ENOMEM;
		}

		d_buf = stratix10_svc_allocate_memory(priv->chan,
						      SUBKEY_RSP_MAX_SZ);
		if (!d_buf) {
			dev_err(dev, "failed allocate subkey RSP buf\n");
			return -ENOMEM;
		}

		/* copy the reserve word first then command payload */
		memcpy(s_buf, &data->com_paras.subkey.resv.resv_word, rsz);
		memcpy(s_buf + rsz, data->com_paras.subkey.cmd_data,
		       data->com_paras.subkey.cmd_data_sz);

		msg->command = COMMAND_FCS_ATTESTATION_SUBKEY;
		msg->payload = s_buf;
		msg->payload_length = datasz;
		msg->payload_output = d_buf;
		msg->payload_length_output = SUBKEY_RSP_MAX_SZ;
		priv->client.receive_cb = fcs_attestation_callback;

		ret = fcs_request_service(priv, (void *)msg,
					  10 * FCS_REQUEST_TIMEOUT);
		if (!ret && !priv->status) {
			if (priv->size > SUBKEY_RSP_MAX_SZ) {
				dev_err(dev,
					"returned size is incorrect\n");
				fcs_close_services(priv, s_buf, d_buf);
				return -EFAULT;
			}

			memcpy(data->com_paras.subkey.rsp_data,
			       priv->kbuf, priv->size);
			data->com_paras.subkey.rsp_data_sz = priv->size;
			data->status = priv->status;

		} else {
			data->com_paras.subkey.rsp_data = NULL;
			data->com_paras.subkey.rsp_data_sz = 0;
			data->status = priv->status;
		}

		if (copy_to_user((void __user *)arg, data, sizeof(*data))) {
			dev_err(dev, "failure on copy_to_user\n");
			return -EFAULT;
		}

		fcs_close_services(priv, s_buf, d_buf);
		break;

	case INTEL_FCS_DEV_ATTESTATION_MEASUREMENT:
		if (copy_from_user(data, (void __user *)arg, sizeof(*data))) {
			dev_err(dev, "failure on copy_from_user\n");
			return -EFAULT;
		}

		if (data->com_paras.measurement.cmd_data_sz > MEASUREMENT_CMD_MAX_SZ) {
			dev_err(dev, "Invalid measurement CMD size %d\n",
				data->com_paras.measurement.cmd_data_sz);
			return -EFAULT;
		}

		if (data->com_paras.measurement.rsp_data_sz > MEASUREMENT_RSP_MAX_SZ) {
			dev_err(dev, "Invalid measurement RSP size %d\n",
				data->com_paras.measurement.rsp_data_sz);
			return -EFAULT;
		}

		/* allocate buffer for both soruce and destination */
		rsz = sizeof(struct intel_fcs_attestation_resv_word);
		datasz = data->com_paras.measurement.cmd_data_sz + rsz;

		s_buf = stratix10_svc_allocate_memory(priv->chan,
						      MEASUREMENT_CMD_MAX_SZ +
						      rsz);
		if (!s_buf) {
			dev_err(dev, "failed allocate measurement CMD buf\n");
			return -ENOMEM;
		}

		d_buf = stratix10_svc_allocate_memory(priv->chan,
						      MEASUREMENT_RSP_MAX_SZ);
		if (!d_buf) {
			dev_err(dev, "failed allocate measurement RSP buf\n");
			return -ENOMEM;
		}

		/* copy the reserve word first then command payload */
		memcpy(s_buf, &data->com_paras.measurement.resv.resv_word, rsz);
		memcpy(s_buf + rsz, data->com_paras.measurement.cmd_data,
		       data->com_paras.measurement.cmd_data_sz);

		msg->command = COMMAND_FCS_ATTESTATION_MEASUREMENTS;
		msg->payload = s_buf;
		msg->payload_length = datasz;
		msg->payload_output = d_buf;
		msg->payload_length_output = MEASUREMENT_RSP_MAX_SZ;
		priv->client.receive_cb = fcs_attestation_callback;

		ret = fcs_request_service(priv, (void *)msg,
					  10 * FCS_REQUEST_TIMEOUT);
		if (!ret && !priv->status) {
			if (priv->size > MEASUREMENT_RSP_MAX_SZ) {
				dev_err(dev,
					"returned size is incorrect\n");
				fcs_close_services(priv, s_buf, d_buf);
				return -EFAULT;
			}

			memcpy(data->com_paras.measurement.rsp_data,
			       priv->kbuf, priv->size);
			data->com_paras.measurement.rsp_data_sz = priv->size;
			data->status = priv->status;
		} else {
			data->com_paras.measurement.rsp_data = NULL;
			data->com_paras.measurement.rsp_data_sz = 0;
			data->status = priv->status;
		}

		if (copy_to_user((void __user *)arg, data, sizeof(*data))) {
			dev_err(dev, "failure on copy_to_user\n");
			ret = -EFAULT;
		}

		fcs_close_services(priv, s_buf, d_buf);
		break;

	case INTEL_FCS_DEV_ATTESTATION_GET_CERTIFICATE:
		if (copy_from_user(data, (void __user *)arg, sizeof(*data))) {
			dev_err(dev, "failure on copy_from_user\n");
			return -EFAULT;
		}

		if (data->com_paras.certificate.rsp_data_sz > CERTIFICATE_RSP_MAX_SZ) {
			dev_err(dev, "Invalid certificate RSP size %d\n",
				data->com_paras.certificate.rsp_data_sz);
			return -EFAULT;
		}

		d_buf = stratix10_svc_allocate_memory(priv->chan,
						      CERTIFICATE_RSP_MAX_SZ);
		if (!d_buf) {
			dev_err(dev, "failed allocate certificate RSP buf\n");
			return -ENOMEM;
		}

		msg->command = COMMAND_FCS_ATTESTATION_CERTIFICATE;
		msg->payload = NULL;
		msg->payload_length = 0;
		msg->payload_output = d_buf;
		msg->payload_length_output = CERTIFICATE_RSP_MAX_SZ;
		msg->arg[0] = data->com_paras.certificate.c_request & 0x000f;
		priv->client.receive_cb = fcs_attestation_callback;

		ret = fcs_request_service(priv, (void *)msg,
					  10 * FCS_REQUEST_TIMEOUT);
		if (!ret && !priv->status) {
			if (priv->size > CERTIFICATE_RSP_MAX_SZ) {
				dev_err(dev,
					"returned size is incorrect\n");
				fcs_close_services(priv, NULL, d_buf);
				return -EFAULT;
			}

			memcpy(data->com_paras.certificate.rsp_data,
			       priv->kbuf, priv->size);
			data->com_paras.certificate.rsp_data_sz = priv->size;
			data->status = priv->status;
		} else {
			data->com_paras.certificate.rsp_data = NULL;
			data->com_paras.certificate.rsp_data_sz = 0;
			data->status = priv->status;
		}

		if (copy_to_user((void __user *)arg, data, sizeof(*data))) {
			dev_err(dev, "failure on copy_to_user\n");
			fcs_close_services(priv, NULL, d_buf);
			ret = -EFAULT;
		}

		fcs_close_services(priv, NULL, d_buf);
		break;

	case INTEL_FCS_DEV_ATTESTATION_CERTIFICATE_RELOAD:
		if (copy_from_user(data, (void __user *)arg, sizeof(*data))) {
			dev_err(dev, "failure on copy_from_user\n");
			return -EFAULT;
		}

		msg->command = COMMAND_FCS_ATTESTATION_CERTIFICATE_RELOAD;
		msg->arg[0] = data->com_paras.c_reload.c_request & 0x000f;
		priv->client.receive_cb = fcs_vab_callback;
		ret = fcs_request_service(priv, (void *)msg,
					  10 * FCS_REQUEST_TIMEOUT);
		if (ret) {
			dev_err(dev, "failed to send the request,ret=%d\n",
				ret);
			return -EFAULT;
		}

		data->status = priv->status;
		if (copy_to_user((void __user *)arg, data, sizeof(*data))) {
			dev_err(dev, "failure on copy_to_user\n");
			ret = -EFAULT;
		}

		break;

	case INTEL_FCS_DEV_GET_ROM_PATCH_SHA384:
		if (copy_from_user(data, (void __user *)arg, sizeof(*data))) {
			dev_err(dev, "failure on copy_from_user\n");
			return -EFAULT;
		}

		s_buf = stratix10_svc_allocate_memory(priv->chan,
						      SHA384_SIZE);
		if (!s_buf) {
			dev_err(dev, "failed to allocate RNG buffer\n");
			return -ENOMEM;
		}

		msg->command = COMMAND_FCS_GET_ROM_PATCH_SHA384;
		msg->payload = s_buf;
		msg->payload_length = SHA384_SIZE;
		priv->client.receive_cb = fcs_data_callback;

		ret = fcs_request_service(priv, (void *)msg,
					  FCS_REQUEST_TIMEOUT);

		if (!ret && !priv->status) {
			if (!priv->kbuf) {
				dev_err(dev, "failure on kbuf\n");
				fcs_close_services(priv, s_buf, NULL);
				return -EFAULT;
			}

			if (priv->size > SHA384_SIZE) {
				dev_err(dev, "returned size is incorrect\n");
				fcs_close_services(priv, s_buf, NULL);
				ret = -EFAULT;
			}

			for (i = 0; i < 12; i++)
				dev_dbg(dev, "output_data[%d]=%d\n", i,
					 *((int *)priv->kbuf + i));
			for (i = 0; i < 12; i++)
				data->com_paras.sha384.checksum[i] =
					*((int *)priv->kbuf + i);
			data->status = priv->status;

		} else {
			/* failed to get SHA */
			data->status = priv->status;
		}


		if (copy_to_user((void __user *)arg, data, sizeof(*data))) {
			dev_err(dev, "failure on copy_to_user\n");
			fcs_close_services(priv, s_buf, NULL);
			ret = -EFAULT;
		}

		fcs_close_services(priv, s_buf, NULL);
		break;

	default:
		dev_warn(dev, "shouldn't be here [0x%x]\n", cmd);
		break;
	}

	return ret;
}

static int fcs_open(struct inode *inode, struct file *file)
{
	pr_debug("%s\n", __func__);

	return 0;
}

static int fcs_close(struct inode *inode, struct file *file)
{

	pr_debug("%s\n", __func__);

	return 0;
}

static const struct file_operations fcs_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = fcs_ioctl,
	.open = fcs_open,
	.release = fcs_close,
};

static int fcs_driver_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct intel_fcs_priv *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client.dev = dev;
	priv->client.receive_cb = NULL;
	priv->client.priv = priv;
	priv->kbuf = NULL;
	priv->size = 0;
	priv->status = INVALID_STATUS;
	priv->cid_low = INVALID_CID;
	priv->cid_high = INVALID_CID;

	mutex_init(&priv->lock);
	priv->chan = stratix10_svc_request_channel_byname(&priv->client,
							  SVC_CLIENT_FCS);
	if (IS_ERR(priv->chan)) {
		dev_err(dev, "couldn't get service channel %s\n",
			SVC_CLIENT_FCS);
		return PTR_ERR(priv->chan);
	}

	priv->miscdev.minor = MISC_DYNAMIC_MINOR;
	priv->miscdev.name = "fcs";
	priv->miscdev.fops = &fcs_fops;

	init_completion(&priv->completion);

	platform_set_drvdata(pdev, priv);

	ret = misc_register(&priv->miscdev);
	if (ret) {
		dev_err(dev, "can't register on minor=%d\n",
			MISC_DYNAMIC_MINOR);
		return ret;
	}

	return 0;
}

static int fcs_driver_remove(struct platform_device *pdev)
{
	struct intel_fcs_priv *priv = platform_get_drvdata(pdev);

	misc_deregister(&priv->miscdev);
	stratix10_svc_free_channel(priv->chan);

	return 0;
}

static struct platform_driver fcs_driver = {
	.probe = fcs_driver_probe,
	.remove = fcs_driver_remove,
	.driver = {
		.name = "intel-fcs",
	},
};

module_platform_driver(fcs_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Intel FGPA Crypto Services Driver");
MODULE_AUTHOR("Richard Gong <richard.gong@intel.com>");

