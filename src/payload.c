/*
 * General protocol-agnostic payload-based sample fetches and ACLs
 *
 * Copyright 2000-2013 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <haproxy/acl.h>
#include <haproxy/api.h>
#include <haproxy/arg.h>
#include <haproxy/channel.h>
#include <haproxy/connection.h>
#include <haproxy/htx.h>
#include <haproxy/net_helper.h>
#include <haproxy/pattern.h>
#include <haproxy/payload.h>
#include <haproxy/sample.h>
#include <haproxy/stconn.h>
#include <haproxy/tools.h>

#ifdef USE_ECH
#include <haproxy/log.h>
#include <haproxy/ech.h>
#endif


/************************************************************************/
/*       All supported sample fetch functions must be declared here     */
/************************************************************************/

enum client_hello_status {
	CLIENTHELLO_ERR_OK = 0,
	CLIENTHELLO_ERR_UNAVAIL = 1,
	CLIENTHELLO_ERR_TOO_SHORT = 2,
};

enum client_hello_type {
	CLIENTHELLO_EXTENSIONS,
	CLIENTHELLO_CIPHERSUITE,
};

/* Extract information presented in a TLS client hello handshake message.
 * The format of the message is the following (cf RFC5246 + RFC6066) :
 * TLS frame :
 *   - uint8  type                            = 0x16   (Handshake)
 *   - uint16 version                        >= 0x0301 (TLSv1)
 *   - uint16 length                                   (frame length)
 *   - TLS handshake :
 *     - uint8  msg_type                      = 0x01   (ClientHello)
 *     - uint24 length                                 (handshake message length)
 *     - ClientHello :
 *       - uint16 client_version             >= 0x0301 (TLSv1)
 *       - uint8 Random[32]                  (4 first ones are timestamp)
 *       - SessionID :
 *         - uint8 session_id_len (0..32)              (SessionID len in bytes)
 *         - uint8 session_id[session_id_len]
 *       - CipherSuite :
 *         - uint16 cipher_len               >= 2      (Cipher length in bytes)
 *         - uint16 ciphers[cipher_len/2]
 *       - CompressionMethod :
 *         - uint8 compression_len           >= 1      (# of supported methods)
 *         - uint8 compression_methods[compression_len]
 *       - optional client_extension_len               (in bytes)
 *       - optional sequence of ClientHelloExtensions  (as many bytes as above):
 *         - uint16 extension_type            = 0 for server_name
 *         - uint16 extension_len
 *         - opaque extension_data[extension_len]
 *           - uint16 server_name_list_len             (# of bytes here)
 *           - opaque server_names[server_name_list_len bytes]
 *             - uint8 name_type              = 0 for host_name
 *             - uint16 name_len
 *             - opaque hostname[name_len bytes]
 */
static int
smp_client_hello_parse( struct sample *smp, enum client_hello_type type, unsigned char **ch_data, int *len)
{
	int hs_len, ext_len, bleft;
	struct channel *chn;
	unsigned char *data;

	if (!smp->strm)
		goto not_ssl_hello;

	/* meaningless for HTX buffers */
	if (IS_HTX_STRM(smp->strm))
		goto not_ssl_hello;

	chn = ((smp->opt & SMP_OPT_DIR) == SMP_OPT_DIR_RES) ? &smp->strm->res : &smp->strm->req;


	bleft = ci_data(chn);
	data = (unsigned char *)ci_head(chn);

	/* Check for SSL/TLS Handshake */
	if (!bleft)
		goto too_short;
	if (*data != 0x16)
		goto not_ssl_hello;

	/* Check for SSLv3 or later (SSL version >= 3.0) in the record layer*/
	if (bleft < 3)
		goto too_short;
	if (data[1] < 0x03)
		goto not_ssl_hello;

	if (bleft < 5)
		goto too_short;
	hs_len = (data[3] << 8) + data[4];
	if (hs_len < 1 + 3 + 2 + 32 + 1 + 2 + 2 + 1 + 1 + 2 + 2)
		goto not_ssl_hello; /* too short to have an extension */

	data += 5; /* enter TLS handshake */
	bleft -= 5;

	/* Check for a complete client hello starting at <data> */
	if (bleft < 1)
		goto too_short;
	if (data[0] != 0x01) /* msg_type = Client Hello */
		goto not_ssl_hello;

	/* Check the Hello's length */
	if (bleft < 4)
		goto too_short;
	hs_len = (data[1] << 16) + (data[2] << 8) + data[3];
	if (hs_len < 2 + 32 + 1 + 2 + 2 + 1 + 1 + 2 + 2)
		goto not_ssl_hello; /* too short to have an extension */

	/* We want the full handshake here */
	if (bleft < hs_len)
		goto too_short;

	data += 4;
	/* Start of the ClientHello message */
	if (data[0] < 0x03 || data[1] < 0x01) /* TLSv1 minimum */
		goto not_ssl_hello;

	ext_len = data[34]; /* session_id_len */
	if (ext_len > 32 || ext_len > (hs_len - 35)) /* check for correct session_id len */
		goto not_ssl_hello;

	/* Jump to cipher suite */
	hs_len -= 35 + ext_len;
	data   += 35 + ext_len;

	if (hs_len < 4 ||                               /* minimum one cipher */
	    (ext_len = (data[0] << 8) + data[1]) < 2 || /* minimum 2 bytes for a cipher */
	    ext_len > hs_len)
		goto not_ssl_hello;

	/* Jump to the compression methods. For fetching cipher list this processing is not required. */
	if (type == CLIENTHELLO_EXTENSIONS)
		goto parse_extn;
	else
		goto parse_cipher;

parse_extn:
	hs_len -= 2 + ext_len;
	data   += 2 + ext_len;

	if (hs_len < 2 ||                       /* minimum one compression method */
	    data[0] < 1 || data[0] > hs_len)    /* minimum 1 bytes for a method */
		goto not_ssl_hello;

	/* Jump to the extensions */
	hs_len -= 1 + data[0];
	data   += 1 + data[0];

	if (hs_len < 2 ||                      /* minimum one extension list length */
	    (ext_len = (data[0] << 8) + data[1]) > hs_len - 2) /* list too long */
		goto not_ssl_hello;

	hs_len = ext_len; /* limit ourselves to the extension length */
	data += 2;

	*len = hs_len;
	*ch_data = data;
	return CLIENTHELLO_ERR_OK;

parse_cipher:
	*len = ext_len;
	*ch_data = data;
	return CLIENTHELLO_ERR_OK;

not_ssl_hello:
	return CLIENTHELLO_ERR_UNAVAIL;

too_short:
	return CLIENTHELLO_ERR_TOO_SHORT;
}

/* wait for more data as long as possible, then return TRUE. This should be
 * used with content inspection.
 */
static int
smp_fetch_wait_end(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	if (!(smp->opt & SMP_OPT_FINAL)) {
		smp->flags |= SMP_F_MAY_CHANGE;
		return 0;
	}
	smp->data.type = SMP_T_BOOL;
	smp->data.u.sint = 1;
	return 1;
}

/* return the number of bytes in the request buffer */
static int
smp_fetch_len(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	if (smp->strm) {
		struct channel *chn = ((smp->opt & SMP_OPT_DIR) == SMP_OPT_DIR_RES) ? &smp->strm->res : &smp->strm->req;

		/* Not accurate but kept for backward compatibility purpose */
		if (IS_HTX_STRM(smp->strm)) {
			struct htx *htx = htxbuf(&chn->buf);
			smp->data.u.sint = htx->data - co_data(chn);
		}
		else
			smp->data.u.sint = ci_data(chn);
	}
	else if (obj_type(smp->sess->origin) == OBJ_TYPE_CHECK) {
		struct check *check = __objt_check(smp->sess->origin);

		/* Not accurate but kept for backward compatibility purpose */
		smp->data.u.sint = ((check->sc && IS_HTX_SC(check->sc)) ? (htxbuf(&check->bi))->data: b_data(&check->bi));
	}
	else
		return 0;

	smp->data.type = SMP_T_SINT;
	smp->flags = SMP_F_VOLATILE | SMP_F_MAY_CHANGE;
	return 1;
}

/* Returns 0 if the client didn't send a SessionTicket Extension
 * Returns 1 if the client sent SessionTicket Extension
 * Returns 2 if the client also sent non-zero length SessionTicket
 * Returns SMP_T_SINT data type
 */
static int
smp_fetch_req_ssl_st_ext(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	enum client_hello_status status;
	int hs_len;
	unsigned char *data;

	status = smp_client_hello_parse(smp, CLIENTHELLO_EXTENSIONS, &data, &hs_len);
	if (status == CLIENTHELLO_ERR_UNAVAIL)
		goto not_ssl_hello;
	else if (status == CLIENTHELLO_ERR_TOO_SHORT)
		goto too_short;


	while (hs_len >= 4) {
		int ext_type, ext_len;

		ext_type = (data[0] << 8) + data[1];
		ext_len  = (data[2] << 8) + data[3];

		if (ext_len > hs_len - 4) /* Extension too long */
			goto not_ssl_hello;

		/* SesstionTicket extension */
		if (ext_type == 35) {
			smp->data.type = SMP_T_SINT;
			/* SessionTicket also present */
			if (ext_len > 0)
				smp->data.u.sint = 2;
			/* SessionTicket absent */
			else
				smp->data.u.sint = 1;
			smp->flags = SMP_F_VOLATILE;
			return 1;
		}

		hs_len -= 4 + ext_len;
		data   += 4 + ext_len;
	}
	/* SessionTicket Extension not found */
	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = 0;
	smp->flags = SMP_F_VOLATILE;
	return 1;

 too_short:
	smp->flags = SMP_F_MAY_CHANGE;

 not_ssl_hello:
	return 0;
}

/* Returns TRUE if the client sent Supported Elliptic Curves Extension (0x000a)
 * Mainly used to detect if client supports ECC cipher suites.
 */
static int
smp_fetch_req_ssl_ec_ext(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	enum client_hello_status status;
	int hs_len;
	unsigned char *data;

	status = smp_client_hello_parse(smp, CLIENTHELLO_EXTENSIONS, &data, &hs_len);
	if (status == CLIENTHELLO_ERR_UNAVAIL)
		goto not_ssl_hello;
	else if (status == CLIENTHELLO_ERR_TOO_SHORT)
		goto too_short;

	while (hs_len >= 4) {
		int ext_type, ext_len;

		ext_type = (data[0] << 8) + data[1];
		ext_len  = (data[2] << 8) + data[3];

		if (ext_len > hs_len - 4) /* Extension too long */
			goto not_ssl_hello;

		/* Elliptic curves extension */
		if (ext_type == 10) {
			smp->data.type = SMP_T_BOOL;
			smp->data.u.sint = 1;
			smp->flags = SMP_F_VOLATILE;
			return 1;
		}

		hs_len -= 4 + ext_len;
		data   += 4 + ext_len;
	}
	/* server name not found */
	goto not_ssl_hello;

 too_short:
	smp->flags = SMP_F_MAY_CHANGE;

 not_ssl_hello:

	return 0;
}
/* returns the type of SSL hello message (mainly used to detect an SSL hello) */
static int
smp_fetch_ssl_hello_type(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	int hs_len;
	int hs_type, bleft;
	struct channel *chn;
	const unsigned char *data;

	if (!smp->strm)
		goto not_ssl_hello;

	/* meaningless for HTX buffers */
	if (IS_HTX_STRM(smp->strm))
		goto not_ssl_hello;

	chn = ((smp->opt & SMP_OPT_DIR) == SMP_OPT_DIR_RES) ? &smp->strm->res : &smp->strm->req;
	bleft = ci_data(chn);
	data = (const unsigned char *)ci_head(chn);

	if (!bleft)
		goto too_short;

	if ((*data >= 0x14 && *data <= 0x17) || (*data == 0xFF)) {
		/* SSLv3 header format */
		if (bleft < 9)
			goto too_short;

		/* ssl version 3 */
		if ((data[1] << 16) + data[2] < 0x00030000)
			goto not_ssl_hello;

		/* ssl message len must present handshake type and len */
		if ((data[3] << 8) + data[4] < 4)
			goto not_ssl_hello;

		/* format introduced with SSLv3 */

		hs_type = (int)data[5];
		hs_len = ( data[6] << 16 ) + ( data[7] << 8 ) + data[8];

		/* not a full handshake */
		if (bleft < (9 + hs_len))
			goto too_short;

	}
	else {
		goto not_ssl_hello;
	}

	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = hs_type;
	smp->flags = SMP_F_VOLATILE;

	return 1;

 too_short:
	smp->flags = SMP_F_MAY_CHANGE;

 not_ssl_hello:

	return 0;
}

/* Return the version of the SSL protocol in the request. It supports both
 * SSLv3 (TLSv1) header format for any message, and SSLv2 header format for
 * the hello message. The SSLv3 format is described in RFC 2246 p49, and the
 * SSLv2 format is described here, and completed p67 of RFC 2246 :
 *    http://wp.netscape.com/eng/security/SSL_2.html
 *
 * Note: this decoder only works with non-wrapping data.
 */
static int
smp_fetch_req_ssl_ver(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	int version, bleft, msg_len;
	const unsigned char *data;
	struct channel *req;

	if (!smp->strm)
		goto not_ssl;

	/* meaningless for HTX buffers */
	if (IS_HTX_STRM(smp->strm))
		goto not_ssl;

	req = &smp->strm->req;
	msg_len = 0;
	bleft = ci_data(req);
	if (!bleft)
		goto too_short;

	data = (const unsigned char *)ci_head(req);
	if ((*data >= 0x14 && *data <= 0x17) || (*data == 0xFF)) {
		/* SSLv3 header format */
		if (bleft < 11)
			goto too_short;

		version = (data[1] << 16) + data[2]; /* record layer version: major, minor */
		msg_len = (data[3] <<  8) + data[4]; /* record length */

		/* format introduced with SSLv3 */
		if (version < 0x00030000)
			goto not_ssl;

		/* message length between 6 and 2^14 + 2048 */
		if (msg_len < 6 || msg_len > ((1<<14) + 2048))
			goto not_ssl;

		bleft -= 5; data += 5;

		/* return the client hello client version, not the record layer version */
		version = (data[4] << 16) + data[5]; /* client hello version: major, minor */
	} else {
		/* SSLv2 header format, only supported for hello (msg type 1) */
		int rlen, plen, cilen, silen, chlen;

		if (*data & 0x80) {
			if (bleft < 3)
				goto too_short;
			/* short header format : 15 bits for length */
			rlen = ((data[0] & 0x7F) << 8) | data[1];
			plen = 0;
			bleft -= 2; data += 2;
		} else {
			if (bleft < 4)
				goto too_short;
			/* long header format : 14 bits for length + pad length */
			rlen = ((data[0] & 0x3F) << 8) | data[1];
			plen = data[2];
			bleft -= 3; data += 3;
		}

		if (*data != 0x01)
			goto not_ssl;
		bleft--; data++;

		if (bleft < 8)
			goto too_short;
		version = (data[0] << 16) + data[1]; /* version: major, minor */
		cilen   = (data[2] <<  8) + data[3]; /* cipher len, multiple of 3 */
		silen   = (data[4] <<  8) + data[5]; /* session_id_len: 0 or 16 */
		chlen   = (data[6] <<  8) + data[7]; /* 16<=challenge length<=32 */

		bleft -= 8; data += 8;
		if (cilen % 3 != 0)
			goto not_ssl;
		if (silen && silen != 16)
			goto not_ssl;
		if (chlen < 16 || chlen > 32)
			goto not_ssl;
		if (rlen != 9 + cilen + silen + chlen)
			goto not_ssl;

		/* focus on the remaining data length */
		msg_len = cilen + silen + chlen + plen;
	}
	/* We could recursively check that the buffer ends exactly on an SSL
	 * fragment boundary and that a possible next segment is still SSL,
	 * but that's a bit pointless. However, we could still check that
	 * all the part of the request which fits in a buffer is already
	 * there.
	 */
	if (msg_len > channel_recv_limit(req) + b_orig(&req->buf) - ci_head(req))
		msg_len = channel_recv_limit(req) + b_orig(&req->buf) - ci_head(req);

	if (bleft < msg_len)
		goto too_short;

	/* OK that's enough. We have at least the whole message, and we have
	 * the protocol version.
	 */
	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = version;
	smp->flags = SMP_F_VOLATILE;
	return 1;

 too_short:
	smp->flags = SMP_F_MAY_CHANGE;
 not_ssl:
	return 0;
}

/*
 * Extract the ciphers that may be presented in a TLS client hello handshake message.
 */
static int
smp_fetch_ssl_cipherlist(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	enum client_hello_status status;
	int hs_len;
	unsigned char *data;

	status = smp_client_hello_parse(smp, CLIENTHELLO_CIPHERSUITE, &data, &hs_len);
	if (status == CLIENTHELLO_ERR_UNAVAIL)
		goto not_ssl_hello;
	else if (status == CLIENTHELLO_ERR_TOO_SHORT)
		goto too_short;

	smp->data.type = SMP_T_BIN;
	smp->data.u.str.area = (char *)data + 2;
	smp->data.u.str.data = hs_len;
	smp->flags = SMP_F_VOLATILE | SMP_F_CONST;

	return 1;

too_short:
	smp->flags = SMP_F_MAY_CHANGE;

not_ssl_hello:

	return 0;
}

/* Extract the supported group that may be presented in a TLS client hello handshake
 * message.
 */
static int
smp_fetch_ssl_supported_groups(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	enum client_hello_status status;
	int hs_len;
	unsigned char *data;

	status = smp_client_hello_parse(smp, CLIENTHELLO_EXTENSIONS, &data, &hs_len);
	if (status == CLIENTHELLO_ERR_UNAVAIL)
		goto not_ssl_hello;
	else if (status == CLIENTHELLO_ERR_TOO_SHORT)
		goto too_short;

	while (hs_len >= 4) {
		int ext_type, ext_len, grp_len;

		ext_type = (data[0] << 8) + data[1]; /* Extension type */
		ext_len  = (data[2] << 8) + data[3]; /* Extension length */

		if (ext_len > hs_len - 4) /* Extension too long */
			goto not_ssl_hello;

		if (ext_type == 10) { /* Supported groups extension type ID is 10dec */
			if (ext_len < 2)  /* need at least one entry of 2 bytes in the list length */
				goto not_ssl_hello;

			grp_len = (data[4] << 8) + data[5]; /* Supported group list length */
			if (grp_len < 2 || grp_len > hs_len - 6)
				goto not_ssl_hello; /* at least 2 bytes per supported group */

			smp->data.type = SMP_T_BIN;
			smp->data.u.str.area = (char *)data + 6;
			smp->data.u.str.data = grp_len;
			smp->flags = SMP_F_VOL_SESS | SMP_F_CONST;

			return 1;

		}
		hs_len -= 4 + ext_len;
		data   += 4 + ext_len;
	}
	/* supported groups not found */
	goto not_ssl_hello;

too_short:
	smp->flags = SMP_F_MAY_CHANGE;

not_ssl_hello:

	return 0;
}

/* Extract the signature algorithms that may be presented in a TLS client hello
 * handshake message.
 */
static int
smp_fetch_ssl_sigalgs(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	enum client_hello_status status;
	int hs_len;
	unsigned char *data;

	status = smp_client_hello_parse(smp, CLIENTHELLO_EXTENSIONS, &data, &hs_len);
	if (status == CLIENTHELLO_ERR_UNAVAIL)
		goto not_ssl_hello;
	else if (status == CLIENTHELLO_ERR_TOO_SHORT)
		goto too_short;

	while (hs_len >= 4) {
		int ext_type, ext_len, sigalg_len;

		ext_type = (data[0] << 8) + data[1]; /* Extension type */
		ext_len  = (data[2] << 8) + data[3]; /* Extension length */

		if (ext_len > hs_len - 4) /* Extension too long */
			goto not_ssl_hello;

		if (ext_type == 13) { /* Sigalgs extension type ID is 13dec */
			if (ext_len < 2) /* need at least one entry of 2 bytes in the list length */
				goto not_ssl_hello;

			sigalg_len = (data[4] << 8) + data[5]; /* Sigalgs list length */
			if (sigalg_len < 2 || sigalg_len > hs_len - 6)
				goto not_ssl_hello; /* at least 2 bytes per sigalg */

			smp->data.type = SMP_T_BIN;
			smp->data.u.str.area = (char *)data + 6;
			smp->data.u.str.data = sigalg_len;
			smp->flags = SMP_F_VOLATILE | SMP_F_CONST;

			return 1;

		}
		hs_len -= 4 + ext_len;
		data   += 4 + ext_len;
	}
	/* sigalgs not found */
	goto not_ssl_hello;

too_short:
	smp->flags = SMP_F_MAY_CHANGE;

not_ssl_hello:

	return 0;
}

/*
 * Extract the key shares that may be presented in a TLS client hello handshake message.
*/
static int
smp_fetch_ssl_keyshare_groups(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	int readPosition, numberOfKeyshares;
	struct buffer *smp_trash = NULL;
	unsigned char *data;
	unsigned char *dataPointer;
	enum client_hello_status status;
	int hs_len;


	status = smp_client_hello_parse(smp, CLIENTHELLO_EXTENSIONS, &data, &hs_len);
	if (status == CLIENTHELLO_ERR_UNAVAIL)
		goto not_ssl_hello;
	else if (status == CLIENTHELLO_ERR_TOO_SHORT)
		goto too_short;

	while (hs_len >= 4) {
		int ext_type, ext_len, keyshare_len;

		ext_type = (data[0] << 8) + data[1]; /* Extension type */
		ext_len  = (data[2] << 8) + data[3]; /* Extension length */

		if (ext_len > hs_len - 4) /* Extension too long */
			goto not_ssl_hello;

		if (ext_type == 51) { /* Keyshare extension type ID is 51dec */
			if (ext_len < 2) /* need at least one entry of 2 bytes in the list length */
				goto not_ssl_hello;

			keyshare_len = (data[4] << 8) + data[5]; /* Client keyshare length */
			if (keyshare_len < 2 || keyshare_len > hs_len - 6)
				goto not_ssl_hello; /* at least 2 bytes per keyshare */
			dataPointer = data + 6; /* start of keyshare entries */
			readPosition = 0;
			numberOfKeyshares = 0;
			smp_trash = get_trash_chunk();
			while (readPosition < keyshare_len) {
				/* Get the binary value of the keyshare group and move the offset to the end of the related keyshare */
				memmove(b_orig(smp_trash) + (2*numberOfKeyshares), &dataPointer[readPosition], 2);
				numberOfKeyshares++;
				readPosition += ((int)dataPointer[readPosition+2] << 8) + (int)dataPointer[readPosition+3] + 4;
			}
			smp->data.type = SMP_T_BIN;
			smp->data.u.str.area = smp_trash->area;
			smp->data.u.str.data = 2*numberOfKeyshares;
			smp->flags = SMP_F_VOLATILE | SMP_F_CONST;

			return 1;
		}
		hs_len -= 4 + ext_len;
		data   += 4 + ext_len;
	}
	/* keyshare groups not found */
	goto not_ssl_hello;

too_short:
	smp->flags = SMP_F_MAY_CHANGE;
not_ssl_hello:
	return 0;
}

#ifdef USE_ECH
static int
payload_attempt_split_ech(const struct arg *args,
                          struct sample *smp,
                          const char *kw,
                          void *private)
{
	int bleft;
	struct channel *chn;
	unsigned char *data;
    int decrypted_ok=0;
    unsigned char *newdata = NULL;
    size_t newlen=1024;
    int srv=0;
    ech_state_t *ech_state = NULL;
    struct stconn *sc = NULL;
#undef ECHDOLOG
#ifdef ECHDOLOG
    /* next two just for logging */
    struct stream *s = NULL;
    struct proxy *frontend = NULL;
#endif

    /*
     * Do some initial checks to be sure we have an entire CH
     * before attempting ECH decryption 
     */
	if (!smp->strm)
		goto not_ssl_hello;
	/* meaningless for HTX buffers */
	if (IS_HTX_STRM(smp->strm))
		goto not_ssl_hello;
	chn = ((smp->opt & SMP_OPT_DIR) == SMP_OPT_DIR_RES)
           ? &smp->strm->res : &smp->strm->req;
	bleft = ci_data(chn);
	data = (unsigned char *)ci_head(chn);

    sc = chn_prod(chn);
#ifdef ECHDOLOG
    s = __sc_strm(sc);
    frontend = strm_fe(s);
#endif

    if (smp->ctx.a[0] != NULL) {
        ech_state = (ech_state_t*) smp->ctx.a[0];
        if (ech_state->calls > 0
            && ech_state->inner_sni != NULL) {
            /* we did ECH decrypt already so no need again */
            ech_state->calls++;
            /* switch on inner SNI */
            smp->data.type = SMP_T_STR;
            smp->data.u.str.area = ech_state->inner_sni;
            smp->data.u.str.data = strlen(ech_state->inner_sni);
            smp->flags = SMP_F_VOLATILE | SMP_F_CONST;
            return 1;
        }
    }

    ech_state = (ech_state_t*) OPENSSL_zalloc(sizeof(ech_state_t));
    if (ech_state == NULL)
        goto not_ssl_hello;
    ech_state->ctx = smp->px->tcp_req.ech_ctx;
    smp->ctx.a[0] = (void *)ech_state;
#ifdef ECHDOLOG
    send_log(frontend, LOG_INFO, "Will attempt split-mode ECH decryption.");
#endif

    srv = attempt_split_ech(ech_state,
                            data, bleft,
                            &decrypted_ok,
                            &newdata, &newlen);
    if (srv == 0) {
#ifdef ECHDOLOG
        send_log(frontend, LOG_INFO, "Split-mode ECH decryption call failed");
#endif
        goto not_ssl_hello;
    }
    if (decrypted_ok) {
        ech_state->calls++;
        /* switch on inner SNI */
        smp->data.type = SMP_T_STR;
        smp->data.u.str.area = ech_state->inner_sni;
        smp->data.u.str.data = (ech_state->inner_sni ?
                                    strlen(ech_state->inner_sni)
                                    : 0);
        smp->flags = SMP_F_CONST;
#ifdef ECHDOLOG
        send_log(frontend, LOG_INFO, "Split-mode ECH decryption succeeded.");
#endif
        /* Move the inner CH onto the channel */
        channel_erase(chn);
        ci_putblk(chn,(char*)newdata,newlen);
        /* store ECH state in case of HRR */
        sc->ech_state = ech_state;
        OPENSSL_free(newdata);
        return 1;
    }
#ifdef ECHDOLOG
     else {
        send_log(frontend, LOG_INFO, "Split-mode ECH decryption failed.");
    }
#endif

 too_short:
	smp->flags = SMP_F_MAY_CHANGE;

 not_ssl_hello:

	return 0;
}
#endif

/* Try to extract the Server Name Indication that may be presented in a TLS
 * client hello handshake message. The format of the message is the following
 * (cf RFC5246 + RFC6066) :
 * TLS frame :
 *   - uint8  type                            = 0x16   (Handshake)
 *   - uint16 version                        >= 0x0301 (TLSv1)
 *   - uint16 length                                   (frame length)
 *   - TLS handshake :
 *     - uint8  msg_type                      = 0x01   (ClientHello)
 *     - uint24 length                                 (handshake message length)
 *     - ClientHello :
 *       - uint16 client_version             >= 0x0301 (TLSv1)
 *       - uint8 Random[32]                  (4 first ones are timestamp)
 *       - SessionID :
 *         - uint8 session_id_len (0..32)              (SessionID len in bytes)
 *         - uint8 session_id[session_id_len]
 *       - CipherSuite :
 *         - uint16 cipher_len               >= 2      (Cipher length in bytes)
 *         - uint16 ciphers[cipher_len/2]
 *       - CompressionMethod :
 *         - uint8 compression_len           >= 1      (# of supported methods)
 *         - uint8 compression_methods[compression_len]
 *       - optional client_extension_len               (in bytes)
 *       - optional sequence of ClientHelloExtensions  (as many bytes as above):
 *         - uint16 extension_type            = 0 for server_name
 *         - uint16 extension_len
 *         - opaque extension_data[extension_len]
 *           - uint16 server_name_list_len             (# of bytes here)
 *           - opaque server_names[server_name_list_len bytes]
 *             - uint8 name_type              = 0 for host_name
 *             - uint16 name_len
 *             - opaque hostname[name_len bytes]
 */
static int
smp_fetch_ssl_hello_sni(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	int hs_len, ext_len, bleft;
	struct channel *chn;
	unsigned char *data;

	if (!smp->strm)
		goto not_ssl_hello;

#ifdef USE_ECH
    /*
     * If we configured ECH, then attempt decryption.
     * Even when ECH decryption worked, this may be called
     * twice (or more), e.g. if we have two backends, one for 
     * the public_name and one for the inner SNI.
     * Even if called multiple times, things should be ok
     * though, as the additional calls won't work given the
     * inner CH will have replaced the outer CH after the
     * first call, but no harm should ensue. (The overhead
     * should also be ok, as we won't do crypto after the
     * 1st ECH decryption since the inner CH will flag
     * that it is an inner CH.)
     * side note: those multiple calls confused me a lot;-)
     * TODO: consider a malformed inner CH, e.g., doesn't
     * have the is-inner flag in the CH extension.
     */
    if (smp->px && smp->px->tcp_req.ech_ctx && smp->ctx.a[0] == NULL
        && payload_attempt_split_ech(args, smp, kw, private) == 1) {
            return 1;
    }
#endif

	/* meaningless for HTX buffers */
	if (IS_HTX_STRM(smp->strm))
		goto not_ssl_hello;

	chn = ((smp->opt & SMP_OPT_DIR) == SMP_OPT_DIR_RES) ? &smp->strm->res : &smp->strm->req;
	bleft = ci_data(chn);
	data = (unsigned char *)ci_head(chn);

	/* Check for SSL/TLS Handshake */
	if (!bleft)
		goto too_short;
	if (*data != 0x16)
		goto not_ssl_hello;

	/* Check for SSLv3 or later (SSL version >= 3.0) in the record layer*/
	if (bleft < 3)
		goto too_short;
	if (data[1] < 0x03)
		goto not_ssl_hello;

	if (bleft < 5)
		goto too_short;
	hs_len = (data[3] << 8) + data[4];
	if (hs_len < 1 + 3 + 2 + 32 + 1 + 2 + 2 + 1 + 1 + 2 + 2)
		goto not_ssl_hello; /* too short to have an extension */

	data += 5; /* enter TLS handshake */
	bleft -= 5;

	/* Check for a complete client hello starting at <data> */
	if (bleft < 1)
		goto too_short;
	if (data[0] != 0x01) /* msg_type = Client Hello */
		goto not_ssl_hello;

	/* Check the Hello's length */
	if (bleft < 4)
		goto too_short;
	hs_len = (data[1] << 16) + (data[2] << 8) + data[3];
	if (hs_len < 2 + 32 + 1 + 2 + 2 + 1 + 1 + 2 + 2)
		goto not_ssl_hello; /* too short to have an extension */

	/* We want the full handshake here */
	if (bleft < hs_len)
		goto too_short;

	data += 4;
	/* Start of the ClientHello message */
	if (data[0] < 0x03 || data[1] < 0x01) /* TLSv1 minimum */
		goto not_ssl_hello;

	ext_len = data[34]; /* session_id_len */
	if (ext_len > 32 || ext_len > (hs_len - 35)) /* check for correct session_id len */
		goto not_ssl_hello;

	/* Jump to cipher suite */
	hs_len -= 35 + ext_len;
	data   += 35 + ext_len;

	if (hs_len < 4 ||                               /* minimum one cipher */
	    (ext_len = (data[0] << 8) + data[1]) < 2 || /* minimum 2 bytes for a cipher */
	    ext_len > hs_len)
		goto not_ssl_hello;

	/* Jump to the compression methods */
	hs_len -= 2 + ext_len;
	data   += 2 + ext_len;

	if (hs_len < 2 ||                       /* minimum one compression method */
	    data[0] < 1 || data[0] > hs_len)    /* minimum 1 bytes for a method */
		goto not_ssl_hello;

	/* Jump to the extensions */
	hs_len -= 1 + data[0];
	data   += 1 + data[0];

	if (hs_len < 2 ||                       /* minimum one extension list length */
	    (ext_len = (data[0] << 8) + data[1]) > hs_len - 2) /* list too long */
		goto not_ssl_hello;

	hs_len = ext_len; /* limit ourselves to the extension length */
	data += 2;

	while (hs_len >= 4) {
		int ext_type, name_type, srv_len, name_len;

		ext_type = (data[0] << 8) + data[1];
		ext_len  = (data[2] << 8) + data[3];

		if (ext_len > hs_len - 4) /* Extension too long */
			goto not_ssl_hello;

		if (ext_type == 0) { /* Server name */
			if (ext_len < 2) /* need one list length */
				goto not_ssl_hello;

			srv_len = (data[4] << 8) + data[5];
			if (srv_len < 4 || srv_len > hs_len - 6)
				goto not_ssl_hello; /* at least 4 bytes per server name */

			name_type = data[6];
			name_len = (data[7] << 8) + data[8];

			if (name_type == 0) { /* hostname */
				smp->data.type = SMP_T_STR;
				smp->data.u.str.area = (char *)data + 9;
				smp->data.u.str.data = name_len;
				smp->flags = SMP_F_VOLATILE | SMP_F_CONST;
				return 1;
			}
		}
		hs_len -= 4 + ext_len;
		data   += 4 + ext_len;
	}
	/* server name not found */
	goto not_ssl_hello;

 too_short:
	smp->flags = SMP_F_MAY_CHANGE;

 not_ssl_hello:

	return 0;
}

/* Try to extract the Application-Layer Protocol Negotiation (ALPN) protocol
 * names that may be presented in a TLS client hello handshake message. As the
 * message presents a list of protocol names in descending order of preference,
 * it may return iteratively. The format of the message is the following
 * (cf RFC5246 + RFC7301) :
 * TLS frame :
 *   - uint8  type                            = 0x16   (Handshake)
 *   - uint16 version                        >= 0x0301 (TLSv1)
 *   - uint16 length                                   (frame length)
 *   - TLS handshake :
 *     - uint8  msg_type                      = 0x01   (ClientHello)
 *     - uint24 length                                 (handshake message length)
 *     - ClientHello :
 *       - uint16 client_version             >= 0x0301 (TLSv1)
 *       - uint8 Random[32]                  (4 first ones are timestamp)
 *       - SessionID :
 *         - uint8 session_id_len (0..32)              (SessionID len in bytes)
 *         - uint8 session_id[session_id_len]
 *       - CipherSuite :
 *         - uint16 cipher_len               >= 2      (Cipher length in bytes)
 *         - uint16 ciphers[cipher_len/2]
 *       - CompressionMethod :
 *         - uint8 compression_len           >= 1      (# of supported methods)
 *         - uint8 compression_methods[compression_len]
 *       - optional client_extension_len               (in bytes)
 *       - optional sequence of ClientHelloExtensions  (as many bytes as above):
 *         - uint16 extension_type            = 16 for application_layer_protocol_negotiation
 *         - uint16 extension_len
 *         - opaque extension_data[extension_len]
 *           - uint16 protocol_names_len               (# of bytes here)
 *           - opaque protocol_names[protocol_names_len bytes]
 *             - uint8 name_len
 *             - opaque protocol_name[name_len bytes]
 */
static int
smp_fetch_ssl_hello_alpn(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	int hs_len, ext_len, bleft;
	struct channel *chn;
	unsigned char *data;

	if (!smp->strm)
		goto not_ssl_hello;

	/* meaningless for HTX buffers */
	if (IS_HTX_STRM(smp->strm))
		goto not_ssl_hello;

	chn = ((smp->opt & SMP_OPT_DIR) == SMP_OPT_DIR_RES) ? &smp->strm->res : &smp->strm->req;
	bleft = ci_data(chn);
	data = (unsigned char *)ci_head(chn);

	/* Check for SSL/TLS Handshake */
	if (!bleft)
		goto too_short;
	if (*data != 0x16)
		goto not_ssl_hello;

	/* Check for SSLv3 or later (SSL version >= 3.0) in the record layer*/
	if (bleft < 3)
		goto too_short;
	if (data[1] < 0x03)
		goto not_ssl_hello;

	if (bleft < 5)
		goto too_short;
	hs_len = (data[3] << 8) + data[4];
	if (hs_len < 1 + 3 + 2 + 32 + 1 + 2 + 2 + 1 + 1 + 2 + 2)
		goto not_ssl_hello; /* too short to have an extension */

	data += 5; /* enter TLS handshake */
	bleft -= 5;

	/* Check for a complete client hello starting at <data> */
	if (bleft < 1)
		goto too_short;
	if (data[0] != 0x01) /* msg_type = Client Hello */
		goto not_ssl_hello;

	/* Check the Hello's length */
	if (bleft < 4)
		goto too_short;
	hs_len = (data[1] << 16) + (data[2] << 8) + data[3];
	if (hs_len < 2 + 32 + 1 + 2 + 2 + 1 + 1 + 2 + 2)
		goto not_ssl_hello; /* too short to have an extension */

	/* We want the full handshake here */
	if (bleft < hs_len)
		goto too_short;

	data += 4;
	/* Start of the ClientHello message */
	if (data[0] < 0x03 || data[1] < 0x01) /* TLSv1 minimum */
		goto not_ssl_hello;

	ext_len = data[34]; /* session_id_len */
	if (ext_len > 32 || ext_len > (hs_len - 35)) /* check for correct session_id len */
		goto not_ssl_hello;

	/* Jump to cipher suite */
	hs_len -= 35 + ext_len;
	data   += 35 + ext_len;

	if (hs_len < 4 ||                               /* minimum one cipher */
	    (ext_len = (data[0] << 8) + data[1]) < 2 || /* minimum 2 bytes for a cipher */
	    ext_len > hs_len)
		goto not_ssl_hello;

	/* Jump to the compression methods */
	hs_len -= 2 + ext_len;
	data   += 2 + ext_len;

	if (hs_len < 2 ||                       /* minimum one compression method */
	    data[0] < 1 || data[0] > hs_len)    /* minimum 1 bytes for a method */
		goto not_ssl_hello;

	/* Jump to the extensions */
	hs_len -= 1 + data[0];
	data   += 1 + data[0];

	if (hs_len < 2 ||                       /* minimum one extension list length */
	    (ext_len = (data[0] << 8) + data[1]) > hs_len - 2) /* list too long */
		goto not_ssl_hello;

	hs_len = ext_len; /* limit ourselves to the extension length */
	data += 2;

	while (hs_len >= 4) {
		int ext_type, name_len, name_offset;

		ext_type = (data[0] << 8) + data[1];
		ext_len  = (data[2] << 8) + data[3];

		if (ext_len > hs_len - 4) /* Extension too long */
			goto not_ssl_hello;

		if (ext_type == 16) { /* ALPN */
			if (ext_len < 3) /* one list length [uint16] + at least one name length [uint8] */
				goto not_ssl_hello;

			/* Name cursor in ctx, must begin after protocol_names_len */
			name_offset = smp->ctx.i < 6 ? 6 : smp->ctx.i;
			name_len = data[name_offset];

			if (name_len + name_offset - 3 > ext_len)
				goto not_ssl_hello;

			smp->data.type = SMP_T_STR;
			smp->data.u.str.area = (char *)data + name_offset + 1; /* +1 to skip name_len */
			smp->data.u.str.data = name_len;
			smp->flags = SMP_F_VOLATILE | SMP_F_CONST;

			/* May have more protocol names remaining */
			if (name_len + name_offset - 3 < ext_len) {
				smp->ctx.i = name_offset + name_len + 1;
				smp->flags |= SMP_F_NOT_LAST;
			}

			return 1;
		}

		hs_len -= 4 + ext_len;
		data   += 4 + ext_len;
	}
	/* alpn not found */
	goto not_ssl_hello;

 too_short:
	smp->flags = SMP_F_MAY_CHANGE;

 not_ssl_hello:

	return 0;
}

/* Fetch the request RDP cookie identified in <cname>:<clen>, or any cookie if
 * <clen> is empty (cname is then ignored). It returns the data into sample <smp>
 * of type SMP_T_CSTR. Note: this decoder only works with non-wrapping data.
 */
int
fetch_rdp_cookie_name(struct stream *s, struct sample *smp, const char *cname, int clen)
{
	int bleft;
	const unsigned char *data;

	smp->flags = SMP_F_CONST;
	smp->data.type = SMP_T_STR;

	bleft = ci_data(&s->req);
	if (bleft <= 11)
		goto too_short;

	data = (const unsigned char *)ci_head(&s->req) + 11;
	bleft -= 11;

	if (bleft <= 7)
		goto too_short;

	if (strncasecmp((const char *)data, "Cookie:", 7) != 0)
		goto not_cookie;

	data += 7;
	bleft -= 7;

	while (bleft > 0 && *data == ' ') {
		data++;
		bleft--;
	}

	if (clen) {
		if (bleft <= clen)
			goto too_short;

		if ((data[clen] != '=') ||
		    strncasecmp(cname, (const char *)data, clen) != 0)
			goto not_cookie;

		data += clen + 1;
		bleft -= clen + 1;
	} else {
		while (bleft > 0 && *data != '=') {
			if (*data == '\r' || *data == '\n')
				goto not_cookie;
			data++;
			bleft--;
		}

		if (bleft < 1)
			goto too_short;

		if (*data != '=')
			goto not_cookie;

		data++;
		bleft--;
	}

	/* data points to cookie value */
	smp->data.u.str.area = (char *)data;
	smp->data.u.str.data = 0;

	while (bleft > 0 && *data != '\r') {
		data++;
		bleft--;
	}

	if (bleft < 2)
		goto too_short;

	if (data[0] != '\r' || data[1] != '\n')
		goto not_cookie;

	smp->data.u.str.data = (char *)data - smp->data.u.str.area;
	smp->flags = SMP_F_VOLATILE | SMP_F_CONST;
	return 1;

 too_short:
	smp->flags = SMP_F_MAY_CHANGE | SMP_F_CONST;
 not_cookie:
	return 0;
}

/* Fetch the request RDP cookie identified in the args, or any cookie if no arg
 * is passed. It is usable both for ACL and for samples. Note: this decoder
 * only works with non-wrapping data. Accepts either 0 or 1 argument. Argument
 * is a string (cookie name), other types will lead to undefined behaviour. The
 * returned sample has type SMP_T_CSTR.
 */
int
smp_fetch_rdp_cookie(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	if (!smp->strm)
		return 0;

	/* meaningless for HTX buffers */
	if (IS_HTX_STRM(smp->strm))
		return 0;

	return fetch_rdp_cookie_name(smp->strm, smp,
				     args ? args->data.str.area : NULL,
				     args ? args->data.str.data : 0);
}

/* returns either 1 or 0 depending on whether an RDP cookie is found or not */
static int
smp_fetch_rdp_cookie_cnt(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	int ret;

	ret = smp_fetch_rdp_cookie(args, smp, kw, private);

	if (smp->flags & SMP_F_MAY_CHANGE)
		return 0;

	smp->flags = SMP_F_VOLATILE;
	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = ret;
	return 1;
}

/* extracts part of a payload with offset and length at a given position */
static int
smp_fetch_payload_lv(const struct arg *arg_p, struct sample *smp, const char *kw, void *private)
{
	unsigned int len_offset = arg_p[0].data.sint;
	unsigned int len_size = arg_p[1].data.sint;
	unsigned int buf_offset;
	unsigned int buf_size = 0;
	struct channel *chn = NULL;
	char *head = NULL;
	size_t max, data;
	int i;

	/* Format is (len offset, len size, buf offset) or (len offset, len size) */
	/* by default buf offset == len offset + len size */
	/* buf offset could be absolute or relative to len offset + len size if prefixed by + or - */

	if (smp->strm) {
		/* meaningless for HTX buffers */
		if (IS_HTX_STRM(smp->strm))
			return 0;
		chn = ((smp->opt & SMP_OPT_DIR) == SMP_OPT_DIR_RES) ? &smp->strm->res : &smp->strm->req;
		head = ci_head(chn);
		data = ci_data(chn);
	}
	else if (obj_type(smp->sess->origin) == OBJ_TYPE_CHECK) {
		struct check *check = __objt_check(smp->sess->origin);

		/* meaningless for HTX buffers */
		if (check->sc && IS_HTX_SC(check->sc))
			return 0;
		head = b_head(&check->bi);
		data = b_data(&check->bi);
	}
	max = global.tune.bufsize;
	if (!head)
		goto too_short;

	if (len_offset + len_size > data)
		goto too_short;

	for (i = 0; i < len_size; i++) {
		buf_size = (buf_size << 8) + ((unsigned char *)head)[i + len_offset];
	}

	/* buf offset may be implicit, absolute or relative. If the LSB
	 * is set, then the offset is relative otherwise it is absolute.
	 */
	buf_offset = len_offset + len_size;
	if (arg_p[2].type == ARGT_SINT) {
		if (arg_p[2].data.sint & 1)
			buf_offset += arg_p[2].data.sint >> 1;
		else
			buf_offset = arg_p[2].data.sint >> 1;
	}

	if (!buf_size || buf_size > max || buf_offset + buf_size > max) {
		/* will never match */
		smp->flags = 0;
		return 0;
	}

	if (buf_offset + buf_size > data)
		goto too_short;

	/* init chunk as read only */
	smp->data.type = SMP_T_BIN;
	smp->flags = SMP_F_VOLATILE | SMP_F_CONST;
	chunk_initlen(&smp->data.u.str, head + buf_offset, 0, buf_size);
	return 1;

 too_short:
	smp->flags = SMP_F_MAY_CHANGE | SMP_F_CONST;
	return 0;
}

/* extracts some payload at a fixed position and length */
static int
smp_fetch_payload(const struct arg *arg_p, struct sample *smp, const char *kw, void *private)
{
	unsigned int buf_offset = arg_p[0].data.sint;
	unsigned int buf_size = arg_p[1].data.sint;
	struct channel *chn = NULL;
	char *head = NULL;
	size_t max, data;

	if (smp->strm) {
		/* meaningless for HTX buffers */
		if (IS_HTX_STRM(smp->strm))
			return 0;
		chn = ((smp->opt & SMP_OPT_DIR) == SMP_OPT_DIR_RES) ? &smp->strm->res : &smp->strm->req;
		head = ci_head(chn);
		data = ci_data(chn);
	}
	else if (obj_type(smp->sess->origin) == OBJ_TYPE_CHECK) {
		struct check *check = __objt_check(smp->sess->origin);

		/* meaningless for HTX buffers */
		if (check->sc && IS_HTX_SC(check->sc))
			return 0;
		head = b_head(&check->bi);
		data = b_data(&check->bi);
	}
	max = global.tune.bufsize;
	if (!head)
		goto too_short;

	if (buf_size > max || buf_offset + buf_size > max) {
		/* will never match */
		smp->flags = 0;
		return 0;
	}
	if (buf_offset + buf_size > data)
		goto too_short;

	/* init chunk as read only */
	smp->data.type = SMP_T_BIN;
	smp->flags = SMP_F_VOLATILE | SMP_F_CONST;
	chunk_initlen(&smp->data.u.str, head + buf_offset, 0, buf_size ? buf_size : (data - buf_offset));

	if (!buf_size && chn && channel_may_recv(chn) && !channel_input_closed(chn))
		smp->flags |= SMP_F_MAY_CHANGE;

	return 1;

  too_short:
	smp->flags = SMP_F_MAY_CHANGE | SMP_F_CONST;
	return 0;
}

/* This function is used to validate the arguments passed to a "payload_lv" fetch
 * keyword. This keyword allows two positive integers and an optional signed one,
 * with the second one being strictly positive and the third one being greater than
 * the opposite of the two others if negative. It is assumed that the types are
 * already the correct ones. Returns 0 on error, non-zero if OK. If <err_msg> is
 * not NULL, it will be filled with a pointer to an error message in case of
 * error, that the caller is responsible for freeing. The initial location must
 * either be freeable or NULL.
 *
 * Note that offset2 is stored with SINT type, but its not directly usable as is.
 * The value is contained in the 63 MSB and the LSB is used as a flag for marking
 * the "relative" property of the value.
 */
int val_payload_lv(struct arg *arg, char **err_msg)
{
	int relative = 0;
	const char *str;

	if (arg[0].data.sint < 0) {
		memprintf(err_msg, "payload offset1 must be positive");
		return 0;
	}

	if (!arg[1].data.sint) {
		memprintf(err_msg, "payload length must be > 0");
		return 0;
	}

	if (arg[2].type == ARGT_STR && arg[2].data.str.data > 0) {
		long long int i;

		if (arg[2].data.str.area[0] == '+' || arg[2].data.str.area[0] == '-')
			relative = 1;
		str = arg[2].data.str.area;
		i = read_int64(&str, str + arg[2].data.str.data);
		if (*str != '\0') {
			memprintf(err_msg, "payload offset2 is not a number");
			return 0;
		}
		chunk_destroy(&arg[2].data.str);
		arg[2].type = ARGT_SINT;
		arg[2].data.sint = i;

		if (arg[0].data.sint + arg[1].data.sint + arg[2].data.sint < 0) {
			memprintf(err_msg, "payload offset2 too negative");
			return 0;
		}
		if (relative)
			arg[2].data.sint = ( arg[2].data.sint << 1 ) + 1;
	}
	return 1;
}

/* extracts the parameter value of a distcc token */
static int
smp_fetch_distcc_param(const struct arg *arg_p, struct sample *smp, const char *kw, void *private)
{
	unsigned int match_tok = arg_p[0].data.sint;
	unsigned int match_occ = arg_p[1].data.sint;
	unsigned int token;
	unsigned int param;
	unsigned int body;
	unsigned int ofs;
	unsigned int occ;
	struct channel *chn;
	int i;

	/* Format is (token[,occ]). occ starts at 1. */

	if (!smp->strm)
		return 0;

	/* meaningless for HTX buffers */
	if (IS_HTX_STRM(smp->strm))
		return 0;

	chn = ((smp->opt & SMP_OPT_DIR) == SMP_OPT_DIR_RES) ? &smp->strm->res : &smp->strm->req;

	ofs = 0; occ = 0;
	while (1) {
		if (ofs + 12 > ci_data(chn)) {
			/* not there yet but could it at least fit ? */
			if (!chn->buf.size)
				goto too_short;

			if (ofs + 12 <= channel_recv_limit(chn) + b_orig(&chn->buf) - ci_head(chn))
				goto too_short;

			goto no_match;
		}

		token = read_n32(ci_head(chn) + ofs);
		ofs += 4;

		for (i = param = 0; i < 8; i++) {
			int c = hex2i(ci_head(chn)[ofs + i]);

			if (c < 0)
				goto no_match;
			param = (param << 4) + c;
		}
		ofs += 8;

		/* these tokens don't have a body */
		if (token != 0x41524743 /* ARGC */ && token != 0x44495354 /* DIST */ &&
		    token != 0x4E46494C /* NFIL */ && token != 0x53544154 /* STAT */ &&
		    token != 0x444F4E45 /* DONE */)
			body = param;
		else
			body = 0;

		if (token == match_tok) {
			occ++;
			if (!match_occ || match_occ == occ) {
				/* found */
				smp->data.type = SMP_T_SINT;
				smp->data.u.sint = param;
				smp->flags = SMP_F_VOLATILE | SMP_F_CONST;
				return 1;
			}
		}
		ofs += body;
	}

 too_short:
	smp->flags = SMP_F_MAY_CHANGE | SMP_F_CONST;
	return 0;
 no_match:
	/* will never match (end of buffer, or bad contents) */
	smp->flags = 0;
	return 0;

}

/* extracts the (possibly truncated) body of a distcc token */
static int
smp_fetch_distcc_body(const struct arg *arg_p, struct sample *smp, const char *kw, void *private)
{
	unsigned int match_tok = arg_p[0].data.sint;
	unsigned int match_occ = arg_p[1].data.sint;
	unsigned int token;
	unsigned int param;
	unsigned int ofs;
	unsigned int occ;
	unsigned int body;
	struct channel *chn;
	int i;

	/* Format is (token[,occ]). occ starts at 1. */

	if (!smp->strm)
		return 0;

	/* meaningless for HTX buffers */
	if (IS_HTX_STRM(smp->strm))
		return 0;

	chn = ((smp->opt & SMP_OPT_DIR) == SMP_OPT_DIR_RES) ? &smp->strm->res : &smp->strm->req;

	ofs = 0; occ = 0;
	while (1) {
		if (ofs + 12 > ci_data(chn)) {
			if (!chn->buf.size)
				goto too_short;

			if (ofs + 12 <= channel_recv_limit(chn) + b_orig(&chn->buf) - ci_head(chn))
				goto too_short;

			goto no_match;
		}

		token = read_n32(ci_head(chn) + ofs);
		ofs += 4;

		for (i = param = 0; i < 8; i++) {
			int c = hex2i(ci_head(chn)[ofs + i]);

			if (c < 0)
				goto no_match;
			param = (param << 4) + c;
		}
		ofs += 8;

		/* these tokens don't have a body */
		if (token != 0x41524743 /* ARGC */ && token != 0x44495354 /* DIST */ &&
		    token != 0x4E46494C /* NFIL */ && token != 0x53544154 /* STAT */ &&
		    token != 0x444F4E45 /* DONE */)
			body = param;
		else
			body = 0;

		if (token == match_tok) {
			occ++;
			if (!match_occ || match_occ == occ) {
				/* found */

				smp->data.type = SMP_T_BIN;
				smp->flags = SMP_F_VOLATILE | SMP_F_CONST;

				if (ofs + body > ci_head(chn) - b_orig(&chn->buf) + ci_data(chn)) {
					/* incomplete body */

					if (ofs + body > channel_recv_limit(chn) + b_orig(&chn->buf) - ci_head(chn)) {
						/* truncate it to whatever will fit */
						smp->flags |= SMP_F_MAY_CHANGE;
						body = channel_recv_limit(chn) + b_orig(&chn->buf) - ci_head(chn) - ofs;
					}
				}

				chunk_initlen(&smp->data.u.str, ci_head(chn) + ofs, 0, body);
				return 1;
			}
		}
		ofs += body;
	}

 too_short:
	smp->flags = SMP_F_MAY_CHANGE | SMP_F_CONST;
	return 0;
 no_match:
	/* will never match (end of buffer, or bad contents) */
	smp->flags = 0;
	return 0;

}

/* This function is used to validate the arguments passed to a "distcc_param" or
 * "distcc_body" sample fetch keyword. They take a mandatory token name of exactly
 * 4 characters, followed by an optional occurrence number starting at 1. It is
 * assumed that the types are already the correct ones. Returns 0 on error, non-
 * zero if OK. If <err_msg> is not NULL, it will be filled with a pointer to an
 * error message in case of error, that the caller is responsible for freeing.
 * The initial location must either be freeable or NULL.
 */
int val_distcc(struct arg *arg, char **err_msg)
{
	unsigned int token;

	if (arg[0].data.str.data != 4) {
		memprintf(err_msg, "token name must be exactly 4 characters");
		return 0;
	}

	/* convert the token name to an unsigned int (one byte per character,
	 * big endian format).
	 */
	token = (arg[0].data.str.area[0] << 24) + (arg[0].data.str.area[1] << 16) +
		(arg[0].data.str.area[2] << 8) + (arg[0].data.str.area[3] << 0);

	chunk_destroy(&arg[0].data.str);
	arg[0].type      = ARGT_SINT;
	arg[0].data.sint = token;

	if (arg[1].type != ARGT_SINT) {
		arg[1].type      = ARGT_SINT;
		arg[1].data.sint = 0;
	}
	return 1;
}

/************************************************************************/
/*      All supported sample and ACL keywords must be declared here.    */
/************************************************************************/

/* Note: must not be declared <const> as its list will be overwritten.
 * Note: fetches that may return multiple types should be declared using the
 * appropriate pseudo-type. If not available it must be declared as the lowest
 * common denominator, the type that can be casted into all other ones.
 */
static struct sample_fetch_kw_list smp_kws = {ILH, {
	{ "distcc_body",         smp_fetch_distcc_body,    ARG2(1,STR,SINT),       val_distcc,     SMP_T_BIN,  SMP_USE_L6REQ|SMP_USE_L6RES },
	{ "distcc_param",        smp_fetch_distcc_param,   ARG2(1,STR,SINT),       val_distcc,     SMP_T_SINT, SMP_USE_L6REQ|SMP_USE_L6RES },
	{ "payload",             smp_fetch_payload,        ARG2(2,SINT,SINT),      NULL,           SMP_T_BIN,  SMP_USE_L6REQ|SMP_USE_L6RES },
	{ "payload_lv",          smp_fetch_payload_lv,     ARG3(2,SINT,SINT,STR),  val_payload_lv, SMP_T_BIN,  SMP_USE_L6REQ|SMP_USE_L6RES },
	{ "rdp_cookie",          smp_fetch_rdp_cookie,     ARG1(0,STR),            NULL,           SMP_T_STR,  SMP_USE_L6REQ },
	{ "rdp_cookie_cnt",      smp_fetch_rdp_cookie_cnt, ARG1(0,STR),            NULL,           SMP_T_SINT, SMP_USE_L6REQ },
	{ "rep_ssl_hello_type",  smp_fetch_ssl_hello_type, 0,                      NULL,           SMP_T_SINT, SMP_USE_L6RES },
	{ "req_len",             smp_fetch_len,            0,                      NULL,           SMP_T_SINT, SMP_USE_L6REQ },
	{ "req_ssl_hello_type",  smp_fetch_ssl_hello_type, 0,                      NULL,           SMP_T_SINT, SMP_USE_L6REQ },
	{ "req_ssl_sni",         smp_fetch_ssl_hello_sni,  0,                      NULL,           SMP_T_STR,  SMP_USE_L6REQ },
	{ "req_ssl_ver",         smp_fetch_req_ssl_ver,    0,                      NULL,           SMP_T_SINT, SMP_USE_L6REQ },

	{ "req.len",             smp_fetch_len,            0,                      NULL,           SMP_T_SINT, SMP_USE_L6REQ },
	{ "req.payload",         smp_fetch_payload,        ARG2(2,SINT,SINT),      NULL,           SMP_T_BIN,  SMP_USE_L6REQ },
	{ "req.payload_lv",      smp_fetch_payload_lv,     ARG3(2,SINT,SINT,STR),  val_payload_lv, SMP_T_BIN,  SMP_USE_L6REQ },
	{ "req.rdp_cookie",      smp_fetch_rdp_cookie,     ARG1(0,STR),            NULL,           SMP_T_STR,  SMP_USE_L6REQ },
	{ "req.rdp_cookie_cnt",  smp_fetch_rdp_cookie_cnt, ARG1(0,STR),            NULL,           SMP_T_SINT, SMP_USE_L6REQ },
	{ "req.ssl_ec_ext",      smp_fetch_req_ssl_ec_ext, 0,                      NULL,           SMP_T_BOOL, SMP_USE_L6REQ },
	{ "req.ssl_st_ext",      smp_fetch_req_ssl_st_ext, 0,                      NULL,           SMP_T_SINT, SMP_USE_L6REQ },
	{ "req.ssl_hello_type",  smp_fetch_ssl_hello_type, 0,                      NULL,           SMP_T_SINT, SMP_USE_L6REQ },
	{ "req.ssl_sni",         smp_fetch_ssl_hello_sni,  0,                      NULL,           SMP_T_STR,  SMP_USE_L6REQ },
	{ "req.ssl_cipherlist",        smp_fetch_ssl_cipherlist,       0,          NULL,           SMP_T_BIN,  SMP_USE_L6REQ|SMP_USE_L4CLI|SMP_USE_L5CLI|SMP_USE_FTEND },
	{ "req.ssl_supported_groups",  smp_fetch_ssl_supported_groups, 0,          NULL,           SMP_T_BIN,  SMP_USE_L6REQ|SMP_USE_L4CLI|SMP_USE_L5CLI|SMP_USE_FTEND },
	{ "req.ssl_sigalgs",           smp_fetch_ssl_sigalgs,          0,          NULL,           SMP_T_BIN,  SMP_USE_L6REQ|SMP_USE_L4CLI|SMP_USE_L5CLI|SMP_USE_FTEND },
	{ "req.ssl_keyshare_groups",   smp_fetch_ssl_keyshare_groups,  0,          NULL,           SMP_T_BIN,  SMP_USE_L6REQ|SMP_USE_L4CLI|SMP_USE_L5CLI|SMP_USE_FTEND },
	{ "req.ssl_alpn",        smp_fetch_ssl_hello_alpn, 0,                      NULL,           SMP_T_STR,  SMP_USE_L6REQ },
	{ "req.ssl_ver",         smp_fetch_req_ssl_ver,    0,                      NULL,           SMP_T_SINT, SMP_USE_L6REQ },
	{ "res.len",             smp_fetch_len,            0,                      NULL,           SMP_T_SINT, SMP_USE_L6RES },
	{ "res.payload",         smp_fetch_payload,        ARG2(2,SINT,SINT),      NULL,           SMP_T_BIN,  SMP_USE_L6RES },
	{ "res.payload_lv",      smp_fetch_payload_lv,     ARG3(2,SINT,SINT,STR),  val_payload_lv, SMP_T_BIN,  SMP_USE_L6RES },
	{ "res.ssl_hello_type",  smp_fetch_ssl_hello_type, 0,                      NULL,           SMP_T_SINT, SMP_USE_L6RES },
	{ "wait_end",            smp_fetch_wait_end,       0,                      NULL,           SMP_T_BOOL, SMP_USE_INTRN },
	{ /* END */ },
}};

INITCALL1(STG_REGISTER, sample_register_fetches, &smp_kws);

/* Note: must not be declared <const> as its list will be overwritten.
 * Please take care of keeping this list alphabetically sorted.
 */
static struct acl_kw_list acl_kws = {ILH, {
	{ "payload",            "req.payload",        PAT_MATCH_BIN },
	{ "payload_lv",         "req.payload_lv",     PAT_MATCH_BIN },
	{ "req_rdp_cookie",     "req.rdp_cookie",     PAT_MATCH_STR },
	{ "req_rdp_cookie_cnt", "req.rdp_cookie_cnt", PAT_MATCH_INT },
	{ "req_ssl_sni",        "req.ssl_sni",        PAT_MATCH_STR },
	{ "req_ssl_ver",        "req.ssl_ver",        PAT_MATCH_INT, pat_parse_dotted_ver },
	{ "req.ssl_ver",        "req.ssl_ver",        PAT_MATCH_INT, pat_parse_dotted_ver },
	{ /* END */ },
}};

INITCALL1(STG_REGISTER, acl_register_keywords, &acl_kws);

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
