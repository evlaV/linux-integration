// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gaming Input Protocol security message driver for Xbox One/Series controllers
 *
 * This file is based on files from the xone project
 * - https://github.com/dlundqvist/xone/blob/master/auth/auth.c
 * - https://github.com/dlundqvist/xone/blob/master/auth/crypto.c
 *
 * Copyright (C) 2023 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
 * Copyright (c) 2026 Valve Software
 */

#include <linux/scatterlist.h>
#include <linux/version.h>
#include <crypto/hash.h>
#include <crypto/sha2.h>
#include <crypto/akcipher.h>
#include <crypto/kpp.h>
#include <crypto/ecdh.h>

#include "gip.h"

/* trailer is required for v1 clients */
#define GIP_SECURITY_TRAILER_LEN 8
#define GIP_SECURITY_CERTIFICATE_MAX_LEN 1024
#define GIP_SECURITY_ENCRYPTED_PMS_LEN 256
#define GIP_SECURITY_TRANSCRIPT_LEN 32
#define GIP_SECURITY_SESSION_KEY_LEN 16

#define GIP_SECURITY_ECDH_SECRET_LEN 32

enum gip_security_context {
	GIP_SECURITY_CTX_HANDSHAKE = 0x00,
	GIP_SECURITY_CTX_CONTROL = 0x01,
};

enum gip_security_command_handshake {
	GIP_SECURITY_CMD_HOST_HELLO = 0x01,
	GIP_SECURITY_CMD_CLIENT_HELLO = 0x02,
	GIP_SECURITY_CMD_CLIENT_CERTIFICATE = 0x03,
	GIP_SECURITY_CMD_HOST_SECRET = 0x05,
	GIP_SECURITY_CMD_HOST_FINISH = 0x07,
	GIP_SECURITY_CMD_CLIENT_FINISH = 0x08,

	GIP_SECURITY2_CMD_HOST_HELLO = 0x21,
	GIP_SECURITY2_CMD_CLIENT_HELLO = 0x22,
	GIP_SECURITY2_CMD_CLIENT_CERTIFICATE = 0x23,
	GIP_SECURITY2_CMD_CLIENT_PUBKEY = 0x24,
	GIP_SECURITY2_CMD_HOST_PUBKEY = 0x25,
	GIP_SECURITY2_CMD_HOST_FINISH = 0x26,
	GIP_SECURITY2_CMD_CLIENT_FINISH = 0x27,
};

enum gip_security_command_control {
	GIP_SECURITY_CTRL_COMPLETE = 0x00,
	GIP_SECURITY_CTRL_RESET = 0x01,
};

enum gip_security_option {
	GIP_SECURITY_OPT_ACKNOWLEDGE = BIT(0),
	GIP_SECURITY_OPT_REQUEST = BIT(1),
	GIP_SECURITY_OPT_FROM_HOST = BIT(6),
	GIP_SECURITY_OPT_FROM_CLIENT = BIT(6) | BIT(7),
};

struct gip_security_header_handshake {
	u8 context;
	u8 options;
	u8 error;
	u8 command;
	__be16 length;
} __packed;

struct gip_security_header_data {
	u8 command;
	u8 version;
	__be16 length;
} __packed;

struct gip_security_header_full {
	struct gip_security_header_handshake handshake;
	struct gip_security_header_data data;
} __packed;

struct gip_security_header_control {
	u8 context;
	u8 control;
} __packed;

struct gip_security_request {
	struct gip_security_header_handshake header;

	u8 trailer[GIP_SECURITY_TRAILER_LEN];
} __packed;

struct gip_security_host_hello {
	struct gip_security_header_full header;

	u8 random[GIP_SECURITY_RANDOM_LEN];
	u8 unknown1[4];
	u8 unknown2[4];

	u8 trailer[GIP_SECURITY_TRAILER_LEN];
} __packed;

struct gip_security_host_secret {
	struct gip_security_header_full header;

	u8 encrypted_pms[GIP_SECURITY_ENCRYPTED_PMS_LEN];

	u8 trailer[GIP_SECURITY_TRAILER_LEN];
} __packed;

struct gip_security_host_finish {
	struct gip_security_header_full header;

	u8 transcript[GIP_SECURITY_TRANSCRIPT_LEN];

	u8 trailer[GIP_SECURITY_TRAILER_LEN];
} __packed;

struct gip_security_client_hello {
	u8 random[GIP_SECURITY_RANDOM_LEN];
	u8 unknown[48];
} __packed;

struct gip_security_client_finish {
	u8 transcript[GIP_SECURITY_TRANSCRIPT_LEN];
	u8 unknown[32];
} __packed;

struct gip_security2_host_hello {
	struct gip_security_header_full header;

	u8 random[GIP_SECURITY_RANDOM_LEN];
	u8 unknown[4];

	u8 trailer[GIP_SECURITY_TRAILER_LEN];
} __packed;

struct gip_security2_host_pubkey {
	struct gip_security_header_full header;

	u8 pubkey[GIP_SECURITY2_PUBKEY_LEN];

	u8 trailer[GIP_SECURITY_TRAILER_LEN];
} __packed;

struct gip_security2_host_finish {
	struct gip_security_header_full header;

	u8 transcript[GIP_SECURITY_TRANSCRIPT_LEN];

	u8 trailer[GIP_SECURITY_TRAILER_LEN];
} __packed;

struct gip_security2_client_hello {
	u8 random[GIP_SECURITY_RANDOM_LEN];
	u8 unknown1[108];
	u8 unknown2[32];
} __packed;

struct gip_security2_client_cert {
	char header[4];
	u8 unknown1[136];
	char chip[32];
	char revision[20];
	u8 unknown2[576];
} __packed;

struct gip_security2_client_pubkey {
	u8 pubkey[GIP_SECURITY2_PUBKEY_LEN];
	u8 unknown[64];
} __packed;

struct gip_security2_client_finish {
	u8 transcript[GIP_SECURITY_TRANSCRIPT_LEN];
	u8 unknown[32];
} __packed;

static struct shash_desc *gip_security_alloc_shash(const char *alg)
{
	struct crypto_shash *tfm;
	struct shash_desc *desc;

	tfm = crypto_alloc_shash(alg, 0, 0);
	if (IS_ERR(tfm))
		return ERR_CAST(tfm);

	desc = kzalloc(sizeof(*desc) + crypto_shash_descsize(tfm), GFP_KERNEL);
	if (!desc) {
		crypto_free_shash(tfm);
		return ERR_PTR(-ENOMEM);
	}

	desc->tfm = tfm;
	crypto_shash_init(desc);

	return desc;
}

static int gip_security_get_transcript(struct shash_desc *desc, void *transcript)
{
	void *state = kzalloc(crypto_shash_descsize(desc->tfm), GFP_KERNEL);
	int err;

	err = crypto_shash_export(desc, state);
	if (err)
		goto get_transcript_error;

	err = crypto_shash_final(desc, transcript);
	if (err)
		goto get_transcript_error;

	err = crypto_shash_import(desc, state);

get_transcript_error:
	kfree(state);
	return err;
}

static int gip_security_compute_prf(struct shash_desc *desc, const char *label,
	u8 *key, int key_len, u8 *seed, int seed_len, u8 *out, int out_len)
{
	u8 hash[SHA256_DIGEST_SIZE], hash_out[SHA256_DIGEST_SIZE];
	int err;

	err = crypto_shash_setkey(desc->tfm, key, key_len);
	if (err)
		return err;

	crypto_shash_init(desc);
	crypto_shash_update(desc, label, strlen(label));
	crypto_shash_update(desc, seed, seed_len);
	crypto_shash_final(desc, hash);

	while (out_len > 0) {
		crypto_shash_init(desc);
		crypto_shash_update(desc, hash, sizeof(hash));
		crypto_shash_update(desc, label, strlen(label));
		crypto_shash_update(desc, seed, seed_len);
		crypto_shash_final(desc, hash_out);

		memcpy(out, hash_out, min_t(int, out_len, sizeof(hash)));
		out += sizeof(hash);
		out_len -= sizeof(hash);

		crypto_shash_digest(desc, hash, sizeof(hash), hash);
	}

	return 0;
}

static int gip_security_encrypt_rsa(u8 *key, int key_len, u8 *in, int in_len, u8 *out, int out_len)
{
	struct crypto_akcipher *tfm;
	int err;

	tfm = crypto_alloc_akcipher("pkcs1pad(rsa)", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);

	err = crypto_akcipher_set_pub_key(tfm, key, key_len);
	if (err)
		goto err_free_tfm;

	err = crypto_akcipher_sync_encrypt(tfm, in, in_len, out, out_len);

err_free_tfm:
	crypto_free_akcipher(tfm);

	return err;
}

static int gip_security_ecdh_get_pubkey(struct crypto_kpp *tfm, u8 *out, int len)
{
	struct kpp_request *req;
	struct scatterlist dest;
	struct ecdh key = {};
	DECLARE_CRYPTO_WAIT(wait);
	void *privkey, *pubkey;
	unsigned int privkey_len;
	int err = 0;

	privkey_len = crypto_ecdh_key_len(&key);
	privkey = kzalloc(privkey_len, GFP_KERNEL);
	if (!privkey)
		return -ENOMEM;

	pubkey = kzalloc(len, GFP_KERNEL);
	if (!pubkey) {
		err = -ENOMEM;
		goto err_free_privkey;
	}

	/* generate private key */
	err = crypto_ecdh_encode_key(privkey, privkey_len, &key);
	if (err)
		goto err_free_pubkey;

	err = crypto_kpp_set_secret(tfm, privkey, privkey_len);
	if (err)
		goto err_free_pubkey;

	req = kpp_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		err = -ENOMEM;
		goto err_free_pubkey;
	}

	sg_init_one(&dest, pubkey, len);

	kpp_request_set_input(req, NULL, 0);
	kpp_request_set_output(req, &dest, len);
	kpp_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				 crypto_req_done, &wait);
	err = crypto_wait_req(crypto_kpp_generate_public_key(req), &wait);
	if (!err)
		memcpy(out, pubkey, len);

	kpp_request_free(req);

err_free_pubkey:
	kfree(pubkey);
err_free_privkey:
	kfree(privkey);

	return err;
}

static int gip_security_ecdh_get_secret(struct crypto_kpp *tfm, u8 *pubkey,
	int pubkey_len, u8 *secret, int secret_len)
{
	struct kpp_request *req;
	struct scatterlist src, dest;
	DECLARE_CRYPTO_WAIT(wait);
	int err;

	req = kpp_request_alloc(tfm, GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	sg_init_one(&src, pubkey, pubkey_len);
	sg_init_one(&dest, secret, secret_len);

	kpp_request_set_input(req, &src, pubkey_len);
	kpp_request_set_output(req, &dest, secret_len);
	kpp_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				 crypto_req_done, &wait);
	err = crypto_wait_req(crypto_kpp_compute_shared_secret(req), &wait);

	kpp_request_free(req);

	return err;
}

static int gip_security_compute_ecdh(u8 *pubkey_in, u8 *pubkey_out, int pubkey_len, u8 *secret_hash)
{
	struct crypto_kpp *tfm_ecdh;
	struct crypto_shash *tfm_sha;
	u8 *secret;
	int err;

	secret = kzalloc(GIP_SECURITY_ECDH_SECRET_LEN, GFP_KERNEL);
	if (!secret)
		return -ENOMEM;

	tfm_ecdh = crypto_alloc_kpp("ecdh-nist-p256", 0, 0);
	if (IS_ERR(tfm_ecdh)) {
		err = PTR_ERR(tfm_ecdh);
		goto err_free_secret;
	}

	tfm_sha = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm_sha)) {
		err = PTR_ERR(tfm_sha);
		goto err_free_ecdh;
	}

	err = gip_security_ecdh_get_pubkey(tfm_ecdh, pubkey_out, pubkey_len);
	if (err)
		goto err_free_sha;

	err = gip_security_ecdh_get_secret(tfm_ecdh, pubkey_in, pubkey_len,
		secret, GIP_SECURITY_ECDH_SECRET_LEN);
	if (err)
		goto err_free_sha;

	crypto_shash_tfm_digest(tfm_sha, secret, GIP_SECURITY_ECDH_SECRET_LEN, secret_hash);

err_free_sha:
	crypto_free_shash(tfm_sha);
err_free_ecdh:
	crypto_free_kpp(tfm_ecdh);
err_free_secret:
	kfree(secret);

	return err;
}

static int gip_security_send_message(struct gip_security *security,
	enum gip_security_command_handshake cmd, void *message, u16 len)
{
	struct gip_attachment *attachment = container_of(security, struct gip_attachment, security);
	struct gip_security_header_full *hdr = message;
	u16 data_len = len - sizeof(hdr->handshake) - GIP_SECURITY_TRAILER_LEN;

	hdr->handshake.context = GIP_SECURITY_CTX_HANDSHAKE;
	hdr->handshake.options = GIP_SECURITY_OPT_ACKNOWLEDGE | GIP_SECURITY_OPT_FROM_HOST;
	hdr->handshake.command = cmd;
	hdr->handshake.length = cpu_to_be16(data_len);

	hdr->data.command = cmd;
	hdr->data.version = cmd >= GIP_SECURITY2_CMD_HOST_HELLO ? 0x02 : 0x01;
	hdr->data.length = cpu_to_be16(data_len - sizeof(hdr->data));

	security->last_sent_command = cmd;
	crypto_shash_update(security->shash_transcript, message + sizeof(hdr->handshake), data_len);

	return gip_send_system_message(attachment, GIP_CMD_SECURITY, GIP_FLAG_ACME, message, len);
}

static int gip_security_send_request(struct gip_security *security,
	enum gip_security_command_handshake cmd, u16 len)
{
	struct gip_attachment *attachment = container_of(security, struct gip_attachment, security);
	struct gip_security_request req = {};
	u16 data_len = len + sizeof(struct gip_security_header_data);

	req.header.context = GIP_SECURITY_CTX_HANDSHAKE;
	req.header.options = GIP_SECURITY_OPT_REQUEST | GIP_SECURITY_OPT_FROM_HOST;
	req.header.command = cmd;
	req.header.length = cpu_to_be16(data_len);

	return gip_send_system_message(attachment, GIP_CMD_SECURITY,
		GIP_FLAG_ACME, &req, sizeof(req));
}

static int gip_security2_send_hello(struct gip_security *security)
{
	struct gip_security2_host_hello message = {};

	get_random_bytes(security->random_host, sizeof(security->random_host));
	memcpy(message.random, security->random_host, sizeof(message.random));

	return gip_security_send_message(security, GIP_SECURITY2_CMD_HOST_HELLO,
		&message, sizeof(message));
}

static int gip_security2_handle_hello(struct gip_security *security, const void *data, u32 len)
{
	const struct gip_security2_client_hello *message = data;

	if (len < sizeof(*message))
		return -EINVAL;

	memcpy(security->random_client, message->random, sizeof(security->random_client));

	return gip_security_send_request(security,
		GIP_SECURITY2_CMD_CLIENT_CERTIFICATE,
		sizeof(struct gip_security2_client_cert));
}

static int gip_security2_handle_certificate(struct gip_security *security,
	const void *data, u32 len)
{
	const struct gip_security2_client_cert *message = data;

	if (len < sizeof(*message))
		return -EINVAL;

	gip_dbg(security,
		"%s: header=%.*s, chip=%.*s, revision=%.*s\n", __func__,
		(int)sizeof(message->header), message->header,
		(int)sizeof(message->chip), message->chip,
		(int)sizeof(message->revision), message->revision);

	return gip_security_send_request(security,
		GIP_SECURITY2_CMD_CLIENT_PUBKEY,
		sizeof(struct gip_security2_client_pubkey));
}

static int gip_security2_handle_pubkey(struct gip_security *security,
	const void *data, u32 len)
{
	const struct gip_security2_client_pubkey *message = data;

	if (len < sizeof(*message))
		return -EINVAL;

	memcpy(security->pubkey_client2, message->pubkey, sizeof(message->pubkey));
	schedule_work(&security->work_exchange_ecdh);

	return 0;
}

static void gip_security2_exchange_ecdh(struct work_struct *work)
{
	struct gip_security *security = container_of(work, typeof(*security), work_exchange_ecdh);
	struct gip_security2_host_pubkey message = {};
	u8 random[GIP_SECURITY_RANDOM_LEN * 2];
	u8 secret[GIP_SECURITY2_SECRET_LEN];
	int err;

	memcpy(random, security->random_host, sizeof(security->random_host));
	memcpy(random + sizeof(security->random_host), security->random_client,
		sizeof(security->random_client));

	err = gip_security_compute_ecdh(security->pubkey_client2,
		message.pubkey, sizeof(message.pubkey), secret);
	if (err) {
		gip_err(security, "%s: compute ECDH failed: %d\n", __func__, err);
		return;
	}

	err = gip_security_compute_prf(security->shash_prf, "Master Secret",
		secret, sizeof(secret), random, sizeof(random),
		security->master_secret, sizeof(security->master_secret));
	if (err) {
		gip_err(security, "%s: compute PRF failed: %d\n", __func__, err);
		return;
	}

	err = gip_security_send_message(security, GIP_SECURITY2_CMD_HOST_PUBKEY,
		&message, sizeof(message));
	if (err)
		gip_err(security, "%s: send message failed: %d\n", __func__, err);
}

static int gip_security_send_hello(struct gip_security *security)
{
	struct gip_security_host_hello message = {};

	get_random_bytes(security->random_host, sizeof(security->random_host));
	memcpy(message.random, security->random_host, sizeof(message.random));

	return gip_security_send_message(security, GIP_SECURITY_CMD_HOST_HELLO,
		&message, sizeof(message));
}

static int gip_security_send_finish(struct gip_security *security,
	enum gip_security_command_handshake cmd)
{
	struct gip_security_host_finish message = {};
	u8 transcript[GIP_SECURITY_TRANSCRIPT_LEN];
	int err;

	err = gip_security_get_transcript(security->shash_transcript, transcript);
	if (err) {
		gip_err(security, "%s: get transcript failed: %d\n", __func__, err);
		return err;
	}

	err = gip_security_compute_prf(security->shash_prf, "Host Finished",
		security->master_secret, sizeof(security->master_secret),
		transcript, sizeof(transcript), message.transcript,
		sizeof(message.transcript));
	if (err) {
		gip_err(security, "%s: compute PRF failed: %d\n", __func__, err);
		return err;
	}

	return gip_security_send_message(security, cmd, &message, sizeof(message));
}

static int gip_security_handle_acknowledge(struct gip_security *security)
{
	switch (security->last_sent_command) {
	case GIP_SECURITY2_CMD_HOST_HELLO:
		return gip_security_send_request(security,
			GIP_SECURITY2_CMD_CLIENT_HELLO,
			sizeof(struct gip_security2_client_hello));
	case GIP_SECURITY2_CMD_HOST_PUBKEY:
		return gip_security_send_finish(security, GIP_SECURITY2_CMD_HOST_FINISH);
	case GIP_SECURITY2_CMD_HOST_FINISH:
		return gip_security_send_request(security,
			GIP_SECURITY2_CMD_CLIENT_FINISH,
			sizeof(struct gip_security2_client_finish));
	case GIP_SECURITY_CMD_HOST_HELLO:
		return gip_security_send_request(security,
			GIP_SECURITY_CMD_CLIENT_HELLO,
			sizeof(struct gip_security_client_hello));
	case GIP_SECURITY_CMD_HOST_SECRET:
		return gip_security_send_finish(security, GIP_SECURITY_CMD_HOST_FINISH);
	case GIP_SECURITY_CMD_HOST_FINISH:
		return gip_security_send_request(security,
			GIP_SECURITY_CMD_CLIENT_FINISH,
			sizeof(struct gip_security_client_finish));
	default:
		return -EPROTO;
	}
}

static int gip_security_handle_hello(struct gip_security *security, const void *data, u32 len)
{
	const struct gip_security_client_hello *message = data;

	if (len < sizeof(*message))
		return -EINVAL;

	memcpy(security->random_client, message->random, sizeof(message->random));

	return gip_security_send_request(security, GIP_SECURITY_CMD_CLIENT_CERTIFICATE,
		GIP_SECURITY_CERTIFICATE_MAX_LEN);
}

static int gip_security_handle_certificate(struct gip_security *security, const void *data, u32 len)
{
	/* ASN.1 SEQUENCE (len = 0x04 + 0x010a) */
	u8 asn1_seq[] = { 0x30, 0x82, 0x01, 0x0a };
	int i;

	if (len > GIP_SECURITY_CERTIFICATE_MAX_LEN)
		return -EINVAL;

	/*
	 * Poor way of extracting a pubkey from an X.509 certificate.
	 * The certificates issued by Microsoft do not comply with RFC 5280.
	 * They have an empty subject and no subjectAltName.
	 * This is explicitly forbidden by section 4.2.1.6 of the RFC.
	 * The kernel's ASN.1 parser will fail when using x509_cert_parse.
	 */
	for (i = 0; i + sizeof(asn1_seq) <= len; i++) {
		if (memcmp(data + i, asn1_seq, sizeof(asn1_seq)))
			continue;

		if (i + GIP_SECURITY_PUBKEY_LEN > len)
			return -EINVAL;

		memcpy(security->pubkey_client, data + i, GIP_SECURITY_PUBKEY_LEN);
		schedule_work(&security->work_exchange_rsa);

		return 0;
	}

	return -EPROTO;
}

static int gip_security_handle_finish(struct gip_security *security, const void *data, u32 len)
{
	const struct gip_security_client_finish *message = data;
	u8 transcript[GIP_SECURITY_TRANSCRIPT_LEN];
	u8 finished[GIP_SECURITY_TRANSCRIPT_LEN];
	int err;

	if (len < sizeof(*message))
		return -EINVAL;

	err = gip_security_get_transcript(security->shash_transcript, transcript);
	if (err) {
		gip_err(security, "%s: get transcript failed: %d\n", __func__, err);
		return err;
	}

	err = gip_security_compute_prf(security->shash_prf, "Device Finished",
		security->master_secret, sizeof(security->master_secret),
		transcript, sizeof(transcript), finished, sizeof(finished));
	if (err) {
		gip_err(security, "%s: compute PRF failed: %d\n", __func__, err);
		return err;
	}

	if (memcmp(message->transcript, finished, sizeof(finished))) {
		gip_err(security, "%s: transcript mismatch\n", __func__);
		return -EPROTO;
	}

	schedule_work(&security->work_complete);

	return 0;
}

static void gip_security_exchange_rsa(struct work_struct *work)
{
	struct gip_security *security = container_of(work, typeof(*security), work_exchange_rsa);
	struct gip_security_host_secret message = {};
	u8 random[GIP_SECURITY_RANDOM_LEN * 2];
	int err;

	memcpy(random, security->random_host, sizeof(security->random_host));
	memcpy(random + sizeof(security->random_host), security->random_client,
		sizeof(security->random_client));

	/* get random premaster secret */
	get_random_bytes(security->pms, sizeof(security->pms));

	err = gip_security_encrypt_rsa(security->pubkey_client,
		sizeof(security->pubkey_client), security->pms,
		sizeof(security->pms), message.encrypted_pms,
		sizeof(message.encrypted_pms));
	if (err) {
		gip_err(security, "%s: encrypt RSA failed: %d\n", __func__, err);
		return;
	}

	err = gip_security_compute_prf(security->shash_prf, "Master Secret",
		security->pms, sizeof(security->pms), random, sizeof(random),
		security->master_secret, sizeof(security->master_secret));
	if (err) {
		gip_err(security, "%s: compute PRF failed: %d\n", __func__, err);
		return;
	}

	err = gip_security_send_message(security, GIP_SECURITY_CMD_HOST_SECRET,
		&message, sizeof(message));
	if (err)
		gip_err(security, "%s: send message failed: %d\n", __func__, err);
}

static void gip_security_complete_handshake(struct work_struct *work)
{
	struct gip_security *security = container_of(work, typeof(*security), work_complete);
	struct gip_attachment *attachment = container_of(security, struct gip_attachment, security);
	struct gip_security_header_control hdr = {
		.context = GIP_SECURITY_CTX_CONTROL,
		.control = GIP_SECURITY_CTRL_COMPLETE,
	};
	u8 random[GIP_SECURITY_RANDOM_LEN * 2];
	u8 key[GIP_SECURITY_SESSION_KEY_LEN];
	int err;

	memcpy(random, security->random_host, sizeof(security->random_host));
	memcpy(random + sizeof(security->random_host), security->random_client,
		sizeof(security->random_client));

	err = gip_security_compute_prf(security->shash_prf,
		"EXPORTER DAWN data channel session key for controller",
		security->master_secret, sizeof(security->master_secret),
		random, sizeof(random), key, sizeof(key));
	if (err) {
		gip_err(security, "%s: compute PRF failed: %d\n", __func__, err);
		return;
	}

	gip_dbg(security, "%s: key=%*phD\n", __func__, (int)sizeof(key), key);

	err = gip_send_system_message(attachment, GIP_CMD_SECURITY,
		0, &hdr, sizeof(hdr));
	if (err)
		gip_err(security, "%s: send complete failed: %d\n", __func__, err);
}

static int gip_security_dispatch_message(struct gip_security *security,
	enum gip_security_command_handshake cmd, const void *data, u32 len)
{
	switch (cmd) {
	case GIP_SECURITY2_CMD_CLIENT_HELLO:
		return gip_security2_handle_hello(security, data, len);
	case GIP_SECURITY2_CMD_CLIENT_CERTIFICATE:
		return gip_security2_handle_certificate(security, data, len);
	case GIP_SECURITY2_CMD_CLIENT_PUBKEY:
		return gip_security2_handle_pubkey(security, data, len);
	case GIP_SECURITY2_CMD_CLIENT_FINISH:
		return gip_security_handle_finish(security, data, len);
	case GIP_SECURITY_CMD_CLIENT_HELLO:
		return gip_security_handle_hello(security, data, len);
	case GIP_SECURITY_CMD_CLIENT_CERTIFICATE:
		return gip_security_handle_certificate(security, data, len);
	case GIP_SECURITY_CMD_CLIENT_FINISH:
		return gip_security_handle_finish(security, data, len);
	default:
		return -EPROTO;
	}
}

int gip_security_handle_message(struct gip_security *security, const void *bytes, int num_bytes)
{
	const struct gip_security_header_handshake *handshake = bytes;
	const struct gip_security_header_full *hdr;
	int err;

	if (num_bytes < sizeof(*handshake))
		return -EINVAL;

	if (handshake->error)
		return -EPROTO;

	if (handshake->options & GIP_SECURITY_OPT_ACKNOWLEDGE) {
		if (handshake->command == 0x01)
			return gip_security_handle_acknowledge(security);

		gip_err(security, "%s: handshake failed: 0x%02x\n",
			__func__, handshake->command);
		return -EPROTO;
	}

	if (num_bytes < sizeof(*hdr))
		return -EINVAL;

	hdr = bytes;
	/* client uses v2 */
	if (handshake->command != hdr->data.command) {
		/* reset transcript hash and restart handshake */
		gip_dbg(security, "%s: protocol upgrade\n", __func__);
		crypto_shash_init(security->shash_transcript);
		return gip_security2_send_hello(security);
	}

	err = gip_security_dispatch_message(security, hdr->data.command,
		bytes + sizeof(*hdr), num_bytes - sizeof(*hdr));
	if (err)
		return err;

	return crypto_shash_update(security->shash_transcript,
		bytes + sizeof(hdr->handshake),
		num_bytes - sizeof(hdr->handshake));
}

void gip_security_release(struct gip_security *security)
{
	if (!security->shash_transcript || !security->shash_prf)
		return;

	cancel_work_sync(&security->work_exchange_rsa);
	cancel_work_sync(&security->work_exchange_ecdh);
	cancel_work_sync(&security->work_complete);

	crypto_free_shash(security->shash_transcript->tfm);
	crypto_free_shash(security->shash_prf->tfm);
	kfree(security->shash_transcript);
	kfree(security->shash_prf);

	security->shash_transcript = NULL;
	security->shash_prf = NULL;
}

int gip_security_start_handshake(struct gip_security *security)
{
	struct shash_desc *shash_transcript, *shash_prf;

	if (!security->shash_transcript) {
		shash_transcript = gip_security_alloc_shash("sha256");
		if (IS_ERR(shash_transcript))
			return PTR_ERR(shash_transcript);

		security->shash_transcript = shash_transcript;
	}

	if (!security->shash_prf) {
		shash_prf = gip_security_alloc_shash("hmac(sha256)");
		if (IS_ERR(shash_prf)) {
			crypto_free_shash(shash_transcript->tfm);
			kfree(shash_transcript);
			security->shash_transcript = NULL;
			return PTR_ERR(shash_prf);
		}

		security->shash_prf = shash_prf;
	}

	INIT_WORK(&security->work_exchange_rsa, gip_security_exchange_rsa);
	INIT_WORK(&security->work_exchange_ecdh, gip_security2_exchange_ecdh);
	INIT_WORK(&security->work_complete, gip_security_complete_handshake);

	return gip_security_send_hello(security);
}

int gip_security_skip_handshake(struct gip_security *security)
{
	struct gip_attachment *attachment = container_of(security, struct gip_attachment, security);
	struct gip_security_header_control hdr = {
		.context = GIP_SECURITY_CTX_CONTROL,
		.control = GIP_SECURITY_CTRL_COMPLETE,
	};

	return gip_send_system_message(attachment, GIP_CMD_SECURITY, 0, &hdr, sizeof(hdr));
}
