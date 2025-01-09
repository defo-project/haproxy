/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef _HAPROXY_ECH_H
# define _HAPROXY_ECH_H
#ifdef USE_ECH

#include <openssl/ech.h>
#include <haproxy/ech-t.h>

/* define this for additional logging of split-mode ECH */
#define ECHDOLOG

int load_echkeys(SSL_CTX *ctx, char *dirname, int *loaded);
int conn_get_ech_status(struct connection *conn, struct buffer *buf);
int conn_get_ech_outer_sni(struct connection *conn, struct buffer *buf);
int attempt_split_ech(ech_state_t *ech_state,
                      unsigned char *data, size_t bleft,
                      int *dec_ok,
                      unsigned char **newdata, size_t *newlen);
void ech_state_free(ech_state_t *st);

int load_echkeys(SSL_CTX *ctx, char *dirname, int *loaded);

# endif /* USE_ECH */
#endif /* _HAPROXY_ECH_H */
