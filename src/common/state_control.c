/*****************************************************************************\
 *  state_control.c - state control common functions
 *****************************************************************************
 *  Copyright (C) 2017 SchedMD LLC.
 *  Written by Alejandro Sanchez <alex@schedmd.com>
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include <ctype.h>
#include <limits.h>	/* For LONG_MAX */
#include "src/common/proc_args.h"
#include "src/common/state_control.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/common/working_cluster.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

extern char *state_control_watts_to_str(uint32_t watts)
{
	char *str = NULL;

	if ((watts == NO_VAL) || (watts == 0))
		xstrcat(str, "n/a");
	else if (watts == INFINITE)
		xstrcat(str, "INFINITE");
	else if ((watts % 1000000) == 0)
		xstrfmtcat(str, "%uM", watts / 1000000);
	else if ((watts % 1000) == 0)
		xstrfmtcat(str, "%uK", watts / 1000);
	else
		xstrfmtcat(str, "%u", watts);

	return str;
}

extern uint32_t state_control_parse_resv_watts(char *watts_str,
					       resv_desc_msg_t *resv_msg_ptr,
					       char **err_msg)
{
	resv_msg_ptr->resv_watts = 0;
	char *end_ptr = NULL;

	if (!xstrcasecmp(watts_str, "n/a") || !xstrcasecmp(watts_str, "none"))
		return SLURM_SUCCESS;
	if (!xstrcasecmp(watts_str, "INFINITE")) {
		resv_msg_ptr->resv_watts = INFINITE;
		return SLURM_SUCCESS;
	}
	resv_msg_ptr->resv_watts = (uint32_t)strtoul(watts_str, &end_ptr, 10);
	if ((end_ptr[0] == 'k') || (end_ptr[0] == 'K')) {
		resv_msg_ptr->resv_watts *= 1000;
	} else if ((end_ptr[0] == 'm') || (end_ptr[0] == 'M')) {
		resv_msg_ptr->resv_watts *= 1000000;
	} else if (end_ptr[0] != '\0') {
		if (err_msg)
			xstrfmtcat(*err_msg, "Invalid Watts value: %s",
				   watts_str);
		resv_msg_ptr->resv_watts = NO_VAL;
		return SLURM_ERROR;
	}
	return SLURM_SUCCESS;
}

extern int state_control_configured_tres(char *type)
{
	int i, cc;
	int rc = SLURM_ERROR;
	assoc_mgr_info_request_msg_t req;
	assoc_mgr_info_msg_t *msg = NULL;

	memset(&req, 0, sizeof(assoc_mgr_info_request_msg_t));
	cc = slurm_load_assoc_mgr_info(&req, &msg);
	if (cc != SLURM_SUCCESS) {
		slurm_perror("slurm_load_assoc_mgr_info error");
		goto cleanup;
	}

	for (i = 0; i < msg->tres_cnt; ++i) {
		if (!xstrcasecmp(msg->tres_names[i], type)) {
			rc = SLURM_SUCCESS;
			goto cleanup;
		}
	}

cleanup:
	slurm_free_assoc_mgr_info_msg(msg);
	return rc;
}

extern int state_control_parse_resv_tres(char *val,
					 resv_desc_msg_t *resv_msg_ptr,
					 uint32_t *res_free_flags,
					 char **err_msg)
{
	int i, len;
	char *tres_bb = NULL, *tres_license = NULL,
		*tres_corecnt = NULL, *tres_nodecnt = NULL,
		*token, *type = NULL, *saveptr1 = NULL,
		*value_str = NULL, *name = NULL, *compound = NULL,
		*tmp = NULL;
	bool discard, first;

	token = strtok_r(val, ",", &saveptr1);
	while (token) {

		compound = strtok_r(token, "=", &value_str);

		if (!compound || !value_str || !*value_str) {
			xstrfmtcat(*err_msg, "invalid TRES '%s'", token);
			goto error;
		}

		if (strchr(compound, '/')) {
			tmp = xstrdup(compound);
			type = strtok_r(tmp, "/", &name);
		} else
			type = compound;

		if (state_control_configured_tres(compound) != SLURM_SUCCESS) {
			xstrfmtcat(*err_msg,
				   "couldn't identify configured TRES '%s'",
				   compound);
			goto error;
		}

		if (!xstrcasecmp(type, "license")) {
			if (tres_license && tres_license[0] != '\0')
				xstrcatchar(tres_license, ',');
			xstrfmtcat(tres_license, "%s:%s", name, value_str);
			token = strtok_r(NULL, ",", &saveptr1);

		} else if (xstrcasecmp(type, "bb") == 0) {
			if (tres_bb && tres_bb[0] != '\0')
				xstrcatchar(tres_bb, ',');
			xstrfmtcat(tres_bb, "%s:%s", name, value_str);
			token = strtok_r(NULL, ",", &saveptr1);

		} else if (xstrcasecmp(type, "cpu") == 0) {
			first = true;
			discard = false;
			do {
				len = strlen(value_str);
				for (i = 0; i < len; i++) {
					if (!isdigit(value_str[i])) {
						if (first) {
							xstrfmtcat(*err_msg,
								   "invalid TRES cpu value '%s'",
								   value_str);
							goto error;
						} else
							discard = true;
						break;
					}
				}
				first = false;
				if (!discard) {
					if (tres_corecnt && tres_corecnt[0]
					    != '\0')
						xstrcatchar(tres_corecnt, ',');
					xstrcat(tres_corecnt, value_str);

					token = strtok_r(NULL, ",", &saveptr1);
					value_str = token;
				}
			} while (!discard && token);

		} else if (xstrcasecmp(type, "node") == 0) {
			if (tres_nodecnt && tres_nodecnt[0] != '\0')
				xstrcatchar(tres_nodecnt, ',');
			xstrcat(tres_nodecnt, value_str);
			token = strtok_r(NULL, ",", &saveptr1);
		} else {
			xstrfmtcat(*err_msg, "TRES type '%s' not supported with reservations",
				   compound);
			goto error;
		}

	}

	if (tres_corecnt && tres_corecnt[0] != '\0') {
		/* only have this on a cons_tres machine */
		resv_msg_ptr->core_cnt = slurm_atoul(tres_corecnt);
		xfree(tres_corecnt);
	}

	if (tres_nodecnt && tres_nodecnt[0] != '\0') {
		char *leftover = NULL;
		resv_msg_ptr->node_cnt = str_to_nodes(tres_nodecnt, &leftover);
		if (!xstring_is_whitespace(leftover)) {
			xstrfmtcat(*err_msg, "\"%s\" is not a valid node count",
				   tres_nodecnt);
			xfree(tres_nodecnt);
			goto error;
		}
		xfree(tres_nodecnt);
	}

	if (tres_license && tres_license[0] != '\0') {
		resv_msg_ptr->licenses = tres_license;
		*res_free_flags |= RESV_FREE_STR_TRES_LIC;
	}

	if (tres_bb && tres_bb[0] != '\0') {
		resv_msg_ptr->burst_buffer = tres_bb;
		*res_free_flags |= RESV_FREE_STR_TRES_BB;
	}

	xfree(tmp);
	return SLURM_SUCCESS;

error:
	xfree(tmp);
	xfree(tres_nodecnt);
	xfree(tres_corecnt);
	return SLURM_ERROR;
}
