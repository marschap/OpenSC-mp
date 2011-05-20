/*
 * card-openpgp.c: Support for OpenPGP card
 *
 * Copyright (C) 2003  Olaf Kirch <okir@suse.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * Specifications:
 * http://www.g10code.de/docs/openpgp-card-1.1.pdf
 * http://www.g10code.de/docs/openpgp-card-2.0.pdf
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "internal.h"
#include "asn1.h"
#include "cardctl.h"
#include "errors.h"

static struct sc_atr_table pgp_atrs[] = {
	{ "3b:fa:13:00:ff:81:31:80:45:00:31:c1:73:c0:01:00:00:90:00:b1", NULL, "OpenPGP card v1.0/1.1", SC_CARD_TYPE_OPENPGP_V1, 0, NULL },
	{ "3b:da:18:ff:81:b1:fe:75:1f:03:00:31:c5:73:c0:01:40:00:90:00:0c", NULL, "CryptoStick v1.2 (OpenPGP v2.0)", SC_CARD_TYPE_OPENPGP_V2, 0, NULL },
	{ NULL, NULL, NULL, 0, 0, NULL }
};

static struct sc_card_operations *iso_ops;
static struct sc_card_operations pgp_ops;
static struct sc_card_driver pgp_drv = {
	"OpenPGP card",
	"openpgp",
	&pgp_ops,
	NULL, 0, NULL
};

/*
 * The OpenPGP card doesn't have a file system, instead everything
 * is stored in data objects that are accessed through GET/PUT.
 *
 * However, much inside OpenSC's pkcs15 implementation is based on
 * the assumption that we have a file system. So we fake one here.
 *
 * Selecting the MF causes us to select the OpenPGP AID.
 *
 * Everything else is mapped to "file" IDs.
 */
struct blob {
	struct blob *	next;	/* pointer to next sibling */
	struct blob *	parent;	/* pointer to parent */
	struct do_info *info;

	sc_file_t *	file;
	unsigned int	id;
	int		status;

	unsigned char *	data;
	unsigned int	len;
	struct blob *	files;	/* pointer to 1st child */
};

struct do_info {
	unsigned int	id;
	unsigned int	constructed : 1;
	unsigned int	size;
	int		(*get_fn)(sc_card_t *, unsigned int, u8 *, size_t);
	int		(*put_fn)(sc_card_t *, unsigned int, const u8 *, size_t);
};

static int		pgp_finish(sc_card_t *card);
static void		pgp_iterate_blobs(struct blob *, int, void (*func)());

static struct blob *	pgp_new_blob(struct blob *, unsigned int, int,
				struct do_info *);
static void		pgp_free_blob(struct blob *);
static int		pgp_get_pubkey(sc_card_t *, unsigned int,
				u8 *, size_t);
static int		pgp_get_pubkey_pem(sc_card_t *, unsigned int,
				u8 *, size_t);

static struct do_info		pgp_objects[] = {
      {	0x004f,		0, 0,	sc_get_data,	sc_put_data	},
      {	0x005e,		0, 0,	sc_get_data,	sc_put_data	},
      {	0x0065,		1, 0,	sc_get_data,	sc_put_data	},
      {	0x006e,		1, 0,	sc_get_data,	sc_put_data	},
      {	0x007a,		1, 0,	sc_get_data,	sc_put_data	},
      {	0x00c4,		0, 0,	sc_get_data,	sc_put_data	},
      {	0x0101,		0, 0,	sc_get_data,	sc_put_data	},
      {	0x0102,		0, 0,	sc_get_data,	sc_put_data	},
      {	0x5f50,		0, 0,	sc_get_data,	sc_put_data	},
      {	0x5f52,		0, 0,	sc_get_data,	sc_put_data	},
      {	0x7f21,		1, 0,	sc_get_data,	sc_put_data	},
      { 0xb600,		1, 0,	pgp_get_pubkey,	NULL		},
      { 0xb800,		1, 0,	pgp_get_pubkey,	NULL		},
      { 0xa400,		1, 0,	pgp_get_pubkey,	NULL		},
      { 0xb601,		0, 0,	pgp_get_pubkey_pem,NULL		},
      { 0xb801,		0, 0,	pgp_get_pubkey_pem,NULL		},
      { 0xa401,		0, 0,	pgp_get_pubkey_pem,NULL		},

      { 0, 0, 0, NULL, NULL },
};

#define DRVDATA(card)        ((struct pgp_priv_data *) ((card)->drv_data))
struct pgp_priv_data {
	struct blob *		mf;
	struct blob *		current;

	sc_security_env_t	sec_env;
};


static int
pgp_match_card(sc_card_t *card)
{
	int i;

	i = _sc_match_atr(card, pgp_atrs, &card->type);
	if (i >= 0) {
		card->name = pgp_atrs[i].name;
		return 1;
	}
	return 0;
}

static int
pgp_init(sc_card_t *card)
{
        struct pgp_priv_data *priv;
        unsigned long	flags;
	sc_path_t	aid;
	sc_file_t	*file = NULL;
	struct do_info	*info;
	int		r;
	struct blob 	*child;

	priv = calloc (1, sizeof *priv);
	if (!priv)
		return SC_ERROR_OUT_OF_MEMORY;
	priv->mf = calloc(1, sizeof(struct blob));
	if (!priv->mf) {
		free(priv);
		return SC_ERROR_OUT_OF_MEMORY;
	}

	card->drv_data = priv;
	card->cla = 0x00;

	/* Is this correct? */
	/* OpenPGP card spec 1.1 & 2.0, section 2.1 */
        flags = SC_ALGORITHM_RSA_RAW;
	/* OpenPGP card spec 1.1 & 2.0, section 7.2.9 & 7.2.10 */
        flags |= SC_ALGORITHM_RSA_PAD_PKCS1;
        flags |= SC_ALGORITHM_RSA_HASH_NONE;

	/* Is this correct? */
        _sc_card_add_rsa_alg(card, 512, flags, 0);
        _sc_card_add_rsa_alg(card, 768, flags, 0);
        _sc_card_add_rsa_alg(card, 1024, flags, 0);
	if (card->type == SC_CARD_TYPE_OPENPGP_V2)
		_sc_card_add_rsa_alg(card, 2048, flags, 0);

	/* select application "OpenPGP" */
	sc_format_path("D276:0001:2401", &aid);
	aid.type = SC_PATH_TYPE_DF_NAME;
	if ((r = iso_ops->select_file(card, &aid, &file)) < 0) {
		free(priv->mf);
		free(priv);
		card->drv_data = NULL;
		return r;
	}

	/* kludge: get card's serial number from manufacturer ID + serial number */
	if (file && file->namelen == 16) {
		/* OpenPGP card spec 1.1 & 2.0, section 4.2.1 & 4.1.2.1 */
		memcpy(card->serialnr.value, file->name + 8, 6);
		card->serialnr.len = 6;
	}

	sc_format_path("3f00", &file->path);
	file->type = SC_FILE_TYPE_DF;
	file->id = 0x3f00;

	priv->mf->file = file;
	priv->mf->id = 0x3F00;

	priv->current = priv->mf;

	/* Populate MF - add all blobs listed in the pgp_objects table. */
	for (info = pgp_objects; info->id > 0; info++) {
		child = pgp_new_blob(priv->mf, info->id,
			  	info->constructed? SC_FILE_TYPE_DF
					  	 : SC_FILE_TYPE_WORKING_EF,
				info);
		/* catch out of memory condition */
		if (child == NULL)
			break;
	}

	/* treat out of memory condition */
	if (child == NULL) {
		pgp_finish(card);
		return SC_ERROR_OUT_OF_MEMORY;
	}

	/* update card capabilities from ATR */
	if (card->atr.len > 0) {
		unsigned char *hist_bytes = card->atr.value;
		size_t len = card->atr.len;
		size_t i = 0;

		while ((i < len) && (hist_bytes[i] != 0x73))
			i++;

		/* bit 0x40 in byte 3 of TL 0x73 means "extended Le/Lc" */
		if ((hist_bytes[i] == 0x73) && (len > i+3) &&
		    (hist_bytes[i+3] & 0x40))
			card->caps |= SC_CARD_CAP_APDU_EXT;
	}

	return SC_SUCCESS;
}

static int
pgp_finish(sc_card_t *card)
{
        struct pgp_priv_data *priv;

        if (card == NULL)
                return SC_SUCCESS;
	priv = DRVDATA (card);

	/* delete fake file hierarchy */
	pgp_iterate_blobs(priv->mf, 99, pgp_free_blob);

	free(priv);
	card->drv_data = NULL;
	return SC_SUCCESS;
}

static int
pgp_set_blob(struct blob *blob, const u8 *data, size_t len)
{
	if (blob->data)
		free(blob->data);
	blob->data = NULL;
	blob->len    = 0;
	blob->status = 0;

	if (len > 0) {
		void *tmp = malloc(len);

		if (tmp == NULL)
			return SC_ERROR_OUT_OF_MEMORY;

		blob->data = tmp;
		blob->len  = len;
		memcpy(blob->data, data, len);
	}

	if (blob->file)
		blob->file->size = len;

	return SC_SUCCESS;
}

static struct blob *
pgp_new_blob(struct blob *parent, unsigned int file_id,
		int file_type, struct do_info *info)
{
	sc_file_t	*file = sc_file_new();
	struct blob	*blob, **p;

	if ((blob = calloc(1, sizeof(*blob))) != NULL) {
		blob->parent = parent;
		blob->id     = file_id;
		blob->file   = file;
		blob->info   = info;

		file->type   = file_type;
		file->path   = parent->file->path;
		file->ef_structure = SC_FILE_EF_TRANSPARENT;
		sc_append_file_id(&file->path, file_id);

		for (p = &parent->files; *p; p = &(*p)->next)
			;
		*p = blob;
	}

	return blob;
}

static void
pgp_free_blob(struct blob *blob)
{
	if (blob) {
		if (blob->file)
			sc_file_free(blob->file);
		if (blob->data)
			free(blob->data);
		free(blob);
	}
}


static void
pgp_iterate_blobs(struct blob *blob, int level, void (*func)())
{
	if (blob) {
		if (level > 0) {
			struct blob *child = blob->files;

			while (child != NULL) {
				struct blob *next = child->next;

				pgp_iterate_blobs(child, level-1, func);
				child = next;
			}
		}
		func(blob);
	}
}

static int
pgp_read_blob(sc_card_t *card, struct blob *blob)
{
	unsigned char	buffer[2048];
	size_t		buf_len = (card->caps & SC_CARD_CAP_APDU_EXT)
				  ? sizeof(buffer) : 256;
	int		r;

	if (blob->data != NULL)
		return SC_SUCCESS;
	if (blob->info == NULL)
		return blob->status;

	r = blob->info->get_fn(card, blob->id, buffer, buf_len);

	if (r < 0) {	/* an error occurred */
		blob->status = r;
		return r;
	}

	return pgp_set_blob(blob, buffer, r);
}

/*
 * Enumerate contents of a data blob.
 * The OpenPGP card has a TLV encoding according ASN.1 BER-encoding rules.
 */
static int
pgp_enumerate_blob(sc_card_t *card, struct blob *blob)
{
	const u8	*in;
	int		r;

	if (blob->files != NULL)
		return SC_SUCCESS;

	if ((r = pgp_read_blob(card, blob)) < 0)
		return r;

	in = blob->data;

	while (blob->len > (in - blob->data)) {
		unsigned int	cla, tag, tmptag;
		unsigned int	type = SC_FILE_TYPE_WORKING_EF;
		size_t		len;
		const u8	*data = in;
		struct blob	*new;

		r = sc_asn1_read_tag(&data, blob->len - (in - blob->data),
					&cla, &tag, &len);
		if (r < 0) {
			sc_debug(card->ctx, SC_LOG_DEBUG_NORMAL,
				 "Unexpected end of contents\n");
			return SC_ERROR_OBJECT_NOT_VALID;
		}

		/* create fake file system hierarchy by
		 * using constructed DOs as DF */
		if (cla & SC_ASN1_TAG_CONSTRUCTED)
			type = SC_FILE_TYPE_DF;

		/* undo ASN1's split of tag & class */
		for (tmptag = tag; tmptag > 0x0FF; tmptag >>= 8) {
			cla <<= 8;
		}
		tag |= cla;

		if ((new = pgp_new_blob(blob, tag, type, NULL)) == NULL)
			return SC_ERROR_OUT_OF_MEMORY;
		pgp_set_blob(new, data, len);
		in = data + len;
	}

	return SC_SUCCESS;
}

static int
pgp_get_blob(sc_card_t *card, struct blob *blob, unsigned int id,
		struct blob **ret)
{
	struct blob		*child;
	int			r;

	if ((r = pgp_enumerate_blob(card, blob)) < 0)
		return r;

	for (child = blob->files; child; child = child->next) {
		if (child->id == id)
			break;
	}

	if (child != NULL) {
		(void) pgp_read_blob(card, child);
		*ret = child;
		return SC_SUCCESS;
	}

	return SC_ERROR_FILE_NOT_FOUND;
}

static int
pgp_select_file(sc_card_t *card, const sc_path_t *path, sc_file_t **ret)
{
	struct pgp_priv_data *priv = DRVDATA(card);
	struct blob	*blob;
	sc_path_t	path_copy;
	unsigned int	n;
	int		r;

	memset(&path_copy, 0, sizeof(path_copy));

	if (path->type == SC_PATH_TYPE_DF_NAME)
		return iso_ops->select_file(card, path, ret);
	if (path->type != SC_PATH_TYPE_PATH)
		return SC_ERROR_INVALID_ARGUMENTS;
	if (path->len < 2 || (path->len & 1))
		return SC_ERROR_INVALID_ARGUMENTS;
	if (!memcmp(path->value, "\x3f\x00", 2)) {
		memcpy(path_copy.value, path->value + 2, path->len - 2);
		path_copy.len = path->len - 2;
		path = &path_copy;
	}

	blob = priv->mf;
	for (n = 0; n < path->len; n += 2) {
		r = pgp_get_blob(card, blob,
				(path->value[n] << 8) | path->value[n+1],
				&blob);
		if (r < 0) {
			priv->current = NULL;
			return r;
		}
	}

	priv->current = blob;

	if (ret)
		sc_file_dup(ret, blob->file);
	return SC_SUCCESS;
}

static int
pgp_list_files(sc_card_t *card, u8 *buf, size_t buflen)
{
	struct pgp_priv_data *priv = DRVDATA(card);
	struct blob	*blob;
	unsigned int	k;
	int		r;

	blob = priv->current;
	if (blob->file->type != SC_FILE_TYPE_DF)
		return SC_ERROR_OBJECT_NOT_VALID;

	if ((r = pgp_enumerate_blob(card, blob)) < 0)
		return r;

	for (k = 0, blob = blob->files; blob; blob = blob->next) {
		if (k + 2 > buflen)
			break;
		buf[k++] = blob->id >> 8;
		buf[k++] = blob->id;
	}

	return k;
}

static int
pgp_read_binary(sc_card_t *card, unsigned int idx,
		u8 *buf, size_t count, unsigned long flags)
{
	struct pgp_priv_data *priv = DRVDATA(card);
	struct blob	*blob;
	int		r;

	if ((blob = priv->current) == NULL)
		return SC_ERROR_FILE_NOT_FOUND;

	if (blob->file->type != SC_FILE_TYPE_WORKING_EF)
		return SC_ERROR_FILE_NOT_FOUND;

	if ((r = pgp_read_blob(card, blob)) < 0)
		return r;

	if (idx > blob->len)
		return SC_ERROR_INCORRECT_PARAMETERS;

	if (idx + count > blob->len)
		count = blob->len - idx;

	memcpy(buf, blob->data + idx, count);
	return count;
}

static int
pgp_write_binary(sc_card_t *card, unsigned int idx,
		const u8 *buf, size_t count, unsigned long flags)
{
	return SC_ERROR_NOT_SUPPORTED;
}

static int
pgp_get_pubkey(sc_card_t *card, unsigned int tag, u8 *buf, size_t buf_len)
{
	sc_apdu_t	apdu;
	u8		idbuf[2];
	int		r;

	sc_debug(card->ctx, SC_LOG_DEBUG_NORMAL, "called, tag=%04x\n", tag);

	idbuf[0] = tag >> 8;
	idbuf[1] = tag;

	sc_format_apdu(card, &apdu, SC_APDU_CASE_4, 0x47, 0x81, 0);
	apdu.lc = 2;
	apdu.data = idbuf;
	apdu.datalen = 2;
	apdu.le = ((buf_len >= 256) && !(card->caps & SC_CARD_CAP_APDU_EXT)) ? 256 : buf_len;
	apdu.resp = buf;
	apdu.resplen = buf_len;

	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "Card returned error");

	return apdu.resplen;
}

static int
pgp_get_pubkey_pem(sc_card_t *card, unsigned int tag, u8 *buf, size_t buf_len)
{
	struct pgp_priv_data *priv = DRVDATA(card);
	struct blob	*blob, *mod_blob, *exp_blob;
	sc_pkcs15_pubkey_t pubkey;
	u8		*data;
	size_t		len;
	int		r;

	sc_debug(card->ctx, SC_LOG_DEBUG_NORMAL, "called, tag=%04x\n", tag);
	
	if ((r = pgp_get_blob(card, priv->mf, tag & 0xFFFE, &blob)) < 0
	 || (r = pgp_get_blob(card, blob, 0x7F49, &blob)) < 0
	 || (r = pgp_get_blob(card, blob, 0x0081, &mod_blob)) < 0
	 || (r = pgp_get_blob(card, blob, 0x0082, &exp_blob)) < 0
	 || (r = pgp_read_blob(card, mod_blob)) < 0
	 || (r = pgp_read_blob(card, exp_blob)) < 0)
		return r;

	memset(&pubkey, 0, sizeof(pubkey));
	pubkey.algorithm = SC_ALGORITHM_RSA;
	pubkey.u.rsa.modulus.data  = mod_blob->data;
	pubkey.u.rsa.modulus.len   = mod_blob->len;
	pubkey.u.rsa.exponent.data = exp_blob->data;
	pubkey.u.rsa.exponent.len  = exp_blob->len;

	if ((r = sc_pkcs15_encode_pubkey(card->ctx, &pubkey, &data, &len)) < 0)
		return r;

	if (len > buf_len)
		len = buf_len;
	memcpy(buf, data, len);
	free(data);
	return len;
}

static int
pgp_get_data(sc_card_t *card, unsigned int tag, u8 *buf, size_t buf_len)
{
	sc_apdu_t	apdu;
	int		r;

	sc_format_apdu(card, &apdu, SC_APDU_CASE_2, 0xCA, tag >> 8, tag);
	apdu.le = ((buf_len >= 256) && !(card->caps & SC_CARD_CAP_APDU_EXT)) ? 256 : buf_len;
	apdu.resp = buf;
	apdu.resplen = buf_len;

	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "Card returned error");

	return apdu.resplen;
}

static int
pgp_put_data(sc_card_t *card, unsigned int tag, const u8 *buf, size_t buf_len)
{
	return SC_ERROR_NOT_SUPPORTED;
}

static int
pgp_pin_cmd(sc_card_t *card, struct sc_pin_cmd_data *data, int *tries_left)
{
	if (data->pin_type != SC_AC_CHV)
		return SC_ERROR_INVALID_ARGUMENTS;

	data->pin_reference |= 0x80;

	return iso_ops->pin_cmd(card, data, tries_left);
}

static int
pgp_set_security_env(sc_card_t *card,
		const sc_security_env_t *env, int se_num)
{
	struct pgp_priv_data *priv = DRVDATA(card);

	if (env->flags & SC_SEC_ENV_ALG_PRESENT) {
		if (env->algorithm != SC_ALGORITHM_RSA)
			return SC_ERROR_INVALID_ARGUMENTS;
	}
	if (!(env->flags & SC_SEC_ENV_KEY_REF_PRESENT)
	 || env->key_ref_len != 1)
		return SC_ERROR_INVALID_ARGUMENTS;
	if (env->flags & SC_SEC_ENV_FILE_REF_PRESENT)
		return SC_ERROR_INVALID_ARGUMENTS;

	switch (env->operation) {
	case SC_SEC_OPERATION_SIGN:
		if (env->key_ref[0] != 0x00
		 && env->key_ref[0] != 0x02) {
		 	sc_debug(card->ctx, SC_LOG_DEBUG_NORMAL,
				"Key reference not compatible with "
				"requested usage\n");
			return SC_ERROR_NOT_SUPPORTED;
		}
		break;
	case SC_SEC_OPERATION_DECIPHER:
		if (env->key_ref[0] != 0x01) {
		 	sc_debug(card->ctx, SC_LOG_DEBUG_NORMAL,
				"Key reference not compatible with "
				"requested usage\n");
			return SC_ERROR_NOT_SUPPORTED;
		}
		break;
	default:
		return SC_ERROR_INVALID_ARGUMENTS;
	}

	priv->sec_env = *env;
	return SC_SUCCESS;
}

static int
pgp_compute_signature(sc_card_t *card, const u8 *data,
                size_t data_len, u8 * out, size_t outlen)
{
	struct pgp_priv_data	*priv = DRVDATA(card);
	sc_security_env_t	*env = &priv->sec_env;
	sc_apdu_t		apdu;
	int			r;

	if (env->operation != SC_SEC_OPERATION_SIGN)
		return SC_ERROR_INVALID_ARGUMENTS;

	switch (env->key_ref[0]) {
	case 0x00: /* signature key */
		/* PSO SIGNATURE */
		sc_format_apdu(card, &apdu, SC_APDU_CASE_4, 0x2A, 0x9E, 0x9A);
		break;
	case 0x02: /* authentication key */
		/* INTERNAL AUTHENTICATE */
		sc_format_apdu(card, &apdu, SC_APDU_CASE_4, 0x88, 0, 0);
		break;
	case 0x01:
		sc_debug(card->ctx, SC_LOG_DEBUG_NORMAL,
			"Invalid key reference (decipher only key)\n");
		return SC_ERROR_INVALID_ARGUMENTS;
	default:
		sc_debug(card->ctx, SC_LOG_DEBUG_NORMAL, "Invalid key reference 0x%02x\n",
				env->key_ref[0]);
		return SC_ERROR_INVALID_ARGUMENTS;
	}

	apdu.lc = data_len;
	apdu.data = data;
	apdu.datalen = data_len;
	apdu.le = ((outlen >= 256) && !(card->caps & SC_CARD_CAP_APDU_EXT)) ? 256 : outlen;
	apdu.resp    = out;
	apdu.resplen = outlen;

	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "Card returned error");

	return apdu.resplen;
}

static int
pgp_decipher(sc_card_t *card, const u8 *in, size_t inlen,
		u8 *out, size_t outlen)
{
	struct pgp_priv_data	*priv = DRVDATA(card);
	sc_security_env_t	*env = &priv->sec_env;
	sc_apdu_t	apdu;
	u8		*temp = NULL;
	int		r;

	/* There's some funny padding indicator that must be
	 * prepended... hmm. */
	if (!(temp = malloc(inlen + 1)))
		return SC_ERROR_OUT_OF_MEMORY;
	temp[0] = '\0';
	memcpy(temp + 1, in, inlen);
	in = temp;
	inlen += 1;

	if (env->operation != SC_SEC_OPERATION_DECIPHER) {
		free(temp);
		return SC_ERROR_INVALID_ARGUMENTS;
	}

	switch (env->key_ref[0]) {
	case 0x01: /* Decryption key */
		/* PSO DECIPHER */
		sc_format_apdu(card, &apdu, SC_APDU_CASE_4, 0x2A, 0x80, 0x86);
		break;
	case 0x00: /* signature key */
	case 0x02: /* authentication key */
		sc_debug(card->ctx, SC_LOG_DEBUG_NORMAL,
			"Invalid key reference (signature only key)\n");
		free(temp);
		return SC_ERROR_INVALID_ARGUMENTS;
	default:
		sc_debug(card->ctx, SC_LOG_DEBUG_NORMAL, "Invalid key reference 0x%02x\n",
				env->key_ref[0]);
		free(temp);
		return SC_ERROR_INVALID_ARGUMENTS;
	}

	apdu.lc = inlen;
	apdu.data = in;
	apdu.datalen = inlen;
	apdu.le = ((outlen >= 256) && !(card->caps & SC_CARD_CAP_APDU_EXT)) ? 256 : outlen;
	apdu.resp = out;
	apdu.resplen = outlen;

	r = sc_transmit_apdu(card, &apdu);
	free(temp);

	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "Card returned error");

	return apdu.resplen;
}

static int pgp_card_ctl(sc_card_t *card, unsigned long cmd, void *ptr)
{
	struct pgp_priv_data	*priv = DRVDATA(card);

	switch(cmd) {
	case SC_CARDCTL_GET_SERIALNR:
		memmove((sc_serial_number_t *) ptr, &card->serialnr, sizeof(card->serialnr));
		return SC_SUCCESS;
		break;
	}

	SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_NORMAL, SC_ERROR_NOT_SUPPORTED);
}

/* Driver binding stuff */
static struct sc_card_driver *
sc_get_driver(void)
{
	struct sc_card_driver *iso_drv = sc_get_iso7816_driver();

	iso_ops = iso_drv->ops;

	pgp_ops = *iso_ops;
	pgp_ops.match_card	= pgp_match_card;
	pgp_ops.init		= pgp_init;
	pgp_ops.finish		= pgp_finish;
	pgp_ops.select_file	= pgp_select_file;
	pgp_ops.list_files	= pgp_list_files;
	pgp_ops.read_binary	= pgp_read_binary;
	pgp_ops.write_binary	= pgp_write_binary;
	pgp_ops.pin_cmd		= pgp_pin_cmd;
	pgp_ops.get_data	= pgp_get_data;
	pgp_ops.put_data	= pgp_put_data;
	pgp_ops.set_security_env= pgp_set_security_env;
	pgp_ops.compute_signature= pgp_compute_signature;
	pgp_ops.decipher	= pgp_decipher;
	pgp_ops.card_ctl	= pgp_card_ctl;

	return &pgp_drv;
}

struct sc_card_driver *
sc_get_openpgp_driver(void)
{
	return sc_get_driver();
}
