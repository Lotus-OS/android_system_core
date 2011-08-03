/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* NOTICE: This is a clean room re-implementation of libnl */

#include <errno.h>
#include "netlink/netlink.h"
#include "netlink/msg.h"
#include "netlink/attr.h"
#include "netlink-types.h"

/* Return payload of string attribute. */
char *nla_get_string(struct nlattr *nla)
{
	return (char *) nla_data(nla);
}

/* Return payload of 16 bit integer attribute. */
uint16_t nla_get_u16(struct nlattr *nla)
{
	return *((uint16_t *) nla_data(nla));
}

/* Return payload of 32 bit integer attribute. */
uint32_t nla_get_u32(struct nlattr *nla)
{
	return *((uint32_t *) nla_data(nla));
}

/* Return value of 8 bit integer attribute. */
uint8_t nla_get_u8(struct nlattr *nla)
{
	return *((uint8_t *) nla_data(nla));
}

/* Return payload of uint64_t attribute. */
uint64_t nla_get_u64(struct nlattr *nla)
{
	uint64_t tmp;
	nla_memcpy(&tmp, nla, sizeof(tmp));
	return tmp;
}

/* Head of payload */
void *nla_data(const struct nlattr *nla)
{
	return (void *) ((char *) nla + NLA_HDRLEN);
}

/* Return length of the payload . */
int nla_len(const struct nlattr *nla)
{
	return nla->nla_len - NLA_HDRLEN;
}

/* Start a new level of nested attributes. */
struct nlattr *nla_nest_start(struct nl_msg *msg, int attrtype)
{
	if (!nla_put(msg, attrtype, 0, NULL)) {
		/* Get ref to last (nested start) attr	*/
		int padding;
		struct nlattr *nla;

		padding = nlmsg_padlen(nlmsg_datalen(nlmsg_hdr(msg)));
		nla = (struct nlattr *) \
			((char *) nlmsg_tail(msg->nm_nlh) - padding);
		return nla;

	} else
		return NULL;

}

/* Finalize nesting of attributes. */
int nla_nest_end(struct nl_msg *msg, struct nlattr *start)
{
	struct nlattr *container;

	/* Adjust nested attribute container size */
	container = (unsigned char *) start - sizeof(struct nlattr);
	container->nla_len = (unsigned char *) \
		nlmsg_tail(nlmsg_hdr(msg)) - (unsigned char *)container;

	/* Fix attribute size */
	start->nla_len = (unsigned char *) \
		nlmsg_tail(nlmsg_hdr(msg)) - (unsigned char *)start;

	return 0;
}

/* Return next attribute in a stream of attributes. */
struct nlattr *nla_next(const struct nlattr *nla, int *remaining)
{
	struct nlattr *next_nla = NULL;
	if (nla->nla_len >= sizeof(struct nlattr) &&
	   nla->nla_len <= *remaining){
		next_nla = (struct nlattr *) \
			((char *) nla + NLA_ALIGN(nla->nla_len));
		*remaining = *remaining - NLA_ALIGN(nla->nla_len);
	}

	return next_nla;

}

/* Check if the attribute header and payload can be accessed safely. */
int nla_ok(const struct nlattr *nla, int remaining)
{
	return remaining > 0 &&
		nla->nla_len >= sizeof(struct nlattr) &&
		sizeof(struct nlattr) <= (unsigned int) remaining &&
		nla->nla_len <= remaining;
}

/* Create attribute index based on a stream of attributes. */
/* NOTE: Policy not used ! */
int nla_parse(struct nlattr *tb[], int maxtype, struct nlattr *head,
	int len, struct nla_policy *policy)
{
	struct nlattr *pos;
	int rem;

	/* First clear table */
	memset(tb, 0, (maxtype+1) * sizeof(struct nlattr *));

	nla_for_each_attr(pos, head, len, rem) {
		const int type = nla_type(pos);

		if (type <= maxtype)
			tb[type] = pos;

	}

	return 0;
}


/* Create attribute index based on nested attribute. */
int nla_parse_nested(struct nlattr *tb[], int maxtype,
		struct nlattr *nla, struct nla_policy *policy)
{
	return nla_parse(tb, maxtype, nla_data(nla), nla_len(nla), policy);
}


/* Add a unspecific attribute to netlink message. */
int nla_put(struct nl_msg *msg, int attrtype, int datalen, const void *data)
{
	struct nlattr *nla;

	/* Reserve space and init nla header */
	nla = nla_reserve(msg, attrtype, datalen);
	if (nla) {
		memcpy(nla_data(nla), data, datalen);
		return 0;
	}

	return -EINVAL;

}


/* Add nested attributes to netlink message. */
/* Takes the attributes found in the nested message and appends them
 * to the message msg nested in a container of the type attrtype. The
 * nested message may not have a family specific header */
int nla_put_nested(struct nl_msg *msg, int attrtype, struct nl_msg *nested)
{
	int rc = -1;
	const int NO_HEADER = 0;

	rc = nla_put(
		msg,
		attrtype,
		nlmsg_attrlen(nlmsg_hdr(nested), NO_HEADER),
		(const void *) nlmsg_attrdata(nlmsg_hdr(nested), NO_HEADER)
		);
	return rc;

}

/* Return type of the attribute. */
int nla_type(const struct nlattr *nla)
{
	return (int) nla->nla_type;
}

/* Reserves room for an attribute in specified netlink message and fills
 * in the attribute header (type,length). Return NULL if insufficient space */
struct nlattr *nla_reserve(struct nl_msg * msg, int attrtype, int data_len)
{

	struct nlattr *nla;
	const unsigned int NEW_SIZE = \
		msg->nm_nlh->nlmsg_len + NLA_ALIGN(NLA_HDRLEN + data_len);

	/* Check enough space for attribute */
	if (NEW_SIZE <= msg->nm_size) {
		const int fam_hdrlen = msg->nm_nlh->nlmsg_len - NLMSG_HDRLEN;
		msg->nm_nlh->nlmsg_len = NEW_SIZE;
		nla = nlmsg_attrdata(msg->nm_nlh, fam_hdrlen);
		nla->nla_type = attrtype;
		nla->nla_len = NLA_HDRLEN + data_len;
	} else
		goto fail;

	return nla;
fail:
	return NULL;

}

/* Copy attribute payload to another memory area. */
int nla_memcpy(void *dest, struct nlattr *src, int count)
{
	int rc;
	void *ret_dest = memcpy(dest, nla_data(src), count);
	if (!ret_dest)
		return count;
	else
		return 0;
}


