/*
 * Copyright (C) 2012, 2015, 2016 IBM Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <scsi/scsi.h>
#include <scsi/scsi_ioctl.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>

#include "encl_util.h"
#include "encl_led.h"
#include "bluehawk.h"

enum bh_component_type {
	BHC_ENCLOSURE,
	BHC_MIDPLANE,
	BHC_DISK,
	BHC_POWER_SUPPLY,
	BHC_ERM,
	BHC_PCI_CONTROLLER,
	BHC_SAS_CONNECTOR,
	BHC_FAN_ASSEMBLY
};

static int
decode_component_loc(const char *loc, enum bh_component_type *type,
		     unsigned int *index)
{
	unsigned int n, n2;
	char g;		/* catch trailing garbage */

	if (!loc || !strcmp(loc, "-")) {
		*type = BHC_ENCLOSURE;
		*index = 0;
	} else if (!strcmp(loc, "P1")) {
		*type = BHC_MIDPLANE;
		*index = 0;
	} else if (sscanf(loc, "P1-D%u%c", &n, &g) == 1) {
		element_check_range(n, 1, 30, loc);
		*type = BHC_DISK;
		*index = n - 1;
	} else if (sscanf(loc, "P1-C%u%c", &n, &g) == 1) {
		element_check_range(n, 1, 2, loc);
		*type = BHC_ERM;
		*index = n-1;
	} else if (sscanf(loc, "P1-E%u%c", &n, &g) == 1) {
		element_check_range(n, 1, 2, loc);
		*type = BHC_POWER_SUPPLY;
		*index = n-1;
	} else if (sscanf(loc, "P1-C%u-T%u%c", &n, &n2, &g) == 2) {
		element_check_range(n, 1, 2, loc);
		if (n2 == 3) {
			*type = BHC_PCI_CONTROLLER;
			*index = n-1;
		} else {
			element_check_range(n2, 1, 2, loc);
			*type = BHC_SAS_CONNECTOR;
			*index = (n-1)*2 + (n2-1);
		}
	} else if (sscanf(loc, "P1-C%u-A%u%c", &n, &n2, &g) == 2) {
		element_check_range(n, 1, 2, loc);
		element_check_range(n2, 1, 1, loc);
		*type = BHC_FAN_ASSEMBLY;
		*index = n-1;
	} else {
		fprintf(stderr, "%s: unrecognized location code: %s\n",
							progname, loc);
		return -1;
	}
	return 0;
}

static const char *on_off_string[] = { "off", "on" };

static void
report_component(struct bluehawk_diag_page2 *dp, int fault, int ident,
		 enum bh_component_type type, unsigned int i, int verbose)
{
	char loc_code[COMP_LOC_CODE];
	char desc[COMP_DESC_SIZE];
	static const char *left_right[] = { "left", "right" };

	switch (type) {
	case BHC_ENCLOSURE:
		REPORT_COMPONENT(dp, enclosure_element_status, fault, ident,
						"-", "enclosure", verbose);
		break;
	case BHC_MIDPLANE:
		REPORT_COMPONENT(dp, midplane_element_status, fault, ident,
						"P1", "midplane", verbose);
		break;
	case BHC_DISK:
		snprintf(loc_code, COMP_LOC_CODE, "P1-D%u", i+1);
		snprintf(desc, COMP_DESC_SIZE, "disk %u", i+1);
		REPORT_COMPONENT(dp, disk_status[i], fault, ident, loc_code,
							desc, verbose);
		break;
	case BHC_POWER_SUPPLY:
		snprintf(loc_code, COMP_LOC_CODE, "P1-E%u", i+1);
		snprintf(desc, COMP_DESC_SIZE,
			 "%s power supply", left_right[i]);
		REPORT_COMPONENT(dp, ps_status[i], fault, ident, loc_code,
							desc, verbose);
		break;
	case BHC_ERM:
		snprintf(loc_code, COMP_LOC_CODE, "P1-C%u", i+1);
		snprintf(desc, COMP_DESC_SIZE,
			 "%s Enclosure RAID Module", left_right[i]);
		REPORT_COMPONENT(dp, esm_status[i], fault, ident, loc_code,
							desc, verbose);
		break;
	case BHC_PCI_CONTROLLER:
		snprintf(loc_code, COMP_LOC_CODE, "P1-C%u-T3", i+1);
		snprintf(desc, COMP_DESC_SIZE,
			 "%s PCIe controller", left_right[i]);
		REPORT_COMPONENT(dp, scc_controller_status[i], fault, ident,
						loc_code, desc, verbose);
		break;
	case BHC_SAS_CONNECTOR:
		snprintf(loc_code, COMP_LOC_CODE,
			 "P1-C%u-T%u", (i/2)+1, (i%2)+1);
		snprintf(desc, COMP_DESC_SIZE,
			 "%s SAS connector T%d", left_right[i/2], (i%2)+1);
		REPORT_COMPONENT(dp, sas_connector_status[i], fault, ident,
						loc_code, desc, verbose);
		break;
	case BHC_FAN_ASSEMBLY:
		snprintf(loc_code, COMP_LOC_CODE, "P1-C%u-A1", i+1);
		snprintf(desc, COMP_DESC_SIZE, "%s fan assembly", left_right[i]);
		REPORT_COMPONENT(dp, fan_sets[i].fan_element[0], fault, ident,
						loc_code, desc, verbose);
		break;
	default:
		fprintf(stderr,
			"%s internal error: unexpected component type %u\n",
			progname, type);
		exit(3);
	}
}

static void
report_component_from_ses(struct bluehawk_diag_page2 *dp,
			  enum bh_component_type type, unsigned int i,
			  int verbose)
{
	report_component(dp, LED_SAME, LED_SAME, type, i, verbose);
}

int
bluehawk_list_leds(const char *enclosure, const char *component, int verbose)
{
	int fd, rc;
	struct bluehawk_diag_page2 dp;

	fd = open_sg_device(enclosure);
	if (fd < 0)
		return -1;
	rc = get_diagnostic_page(fd, RECEIVE_DIAGNOSTIC, 2, &dp, sizeof(dp));
	if (rc != 0) {
		fprintf(stderr,
			"%s: cannot read diagnostic page from SES for %s\n",
			progname, enclosure);
		close(fd);
		return -1;
	}

	printf("fault ident location  description\n");

	if (component) {
		unsigned int cindex;
		enum bh_component_type ctype;
		rc = decode_component_loc(component, &ctype, &cindex);
		if (rc != 0) {
			close(fd);
			return -1;
		}

		report_component_from_ses(&dp, ctype, cindex, verbose);
	} else {
		unsigned int i;

		report_component_from_ses(&dp, BHC_ENCLOSURE, 0, verbose);
		report_component_from_ses(&dp, BHC_MIDPLANE, 0, verbose);
		for (i = 0; i < NR_DISKS_PER_BLUEHAWK; i++)
			report_component_from_ses(&dp, BHC_DISK, i, verbose);
		for (i = 0; i < 2; i++)
			report_component_from_ses(&dp, BHC_POWER_SUPPLY, i,
								verbose);
		for (i = 0; i < 2; i++)
			report_component_from_ses(&dp, BHC_ERM, i, verbose);
		for (i = 0; i < 2; i++)
			report_component_from_ses(&dp, BHC_PCI_CONTROLLER, i,
								verbose);
		for (i = 0; i < 4; i++)
			report_component_from_ses(&dp, BHC_SAS_CONNECTOR, i,
								verbose);
		for (i = 0; i < 2; i++)
			report_component_from_ses(&dp, BHC_FAN_ASSEMBLY, i,
								verbose);
	}

	close(fd);
	return 0;
}

int
bluehawk_set_led(const char *enclosure, const char *component, int fault,
		 int ident, int verbose)
{
	int fd, rc;
	unsigned int index;
	enum bh_component_type type;
	struct bluehawk_diag_page2 dp;
	struct bluehawk_ctrl_page2 cp;

	fd = open_sg_device(enclosure);
	if (fd < 0)
		return -1;

	rc = decode_component_loc(component, &type, &index);
	if (rc != 0)
		return -1;

	if (fault == LED_SAME || ident == LED_SAME) {
		memset(&dp, 0, sizeof(dp));
		rc = get_diagnostic_page(fd, RECEIVE_DIAGNOSTIC, 2, &dp,
								sizeof(dp));
		if (rc != 0) {
			fprintf(stderr, "%s: cannot read diagnostic page"
				" from SES for %s\n", progname, enclosure);
			return -1;
		}
	}

	memset(&cp, 0, sizeof(cp));

	switch (type) {
	case BHC_ENCLOSURE:
		if (fault == LED_ON) {
			fprintf(stderr, "%s: Cannot directly enable enclosure"
					" fault indicator\n", enclosure);
			return -1;
		}
		SET_LED(&cp, &dp, fault, ident, enclosure_element_ctrl,
						enclosure_element_status);
		break;
	case BHC_MIDPLANE:
		SET_LED(&cp, &dp, fault, ident, midplane_element_ctrl,
						midplane_element_status);
		break;
	case BHC_DISK:
		SET_LED(&cp, &dp, fault, ident, disk_ctrl[index],
						disk_status[index]);
		break;
	case BHC_POWER_SUPPLY:
		SET_LED(&cp, &dp, fault, ident, ps_ctrl[index],
						ps_status[index]);
		break;
	case BHC_ERM:
		SET_LED(&cp, &dp, fault, ident, esm_ctrl[index],
						esm_status[index]);
		break;
	case BHC_PCI_CONTROLLER:
		SET_LED(&cp, &dp, fault, ident, scc_controller_ctrl[index],
						scc_controller_status[index]);
		break;
	case BHC_SAS_CONNECTOR:
		SET_LED(&cp, &dp, fault, ident, sas_connector_ctrl[index],
						sas_connector_status[index]);
		break;
	case BHC_FAN_ASSEMBLY:
		SET_LED(&cp, &dp, fault, ident, fan_sets[index].fan_element[0],
						fan_sets[index].fan_element[0]);
		break;
	default:
		fprintf(stderr,
			"%s internal error: unexpected component type %u\n",
			progname, type);
		exit(3);
	}

	cp.page_code = 2;
	/* Convert host byte order to network byte order */
	cp.page_length = htons(sizeof(cp) - 4);
	cp.generation_code = 0;

	rc = do_ses_cmd(fd, SEND_DIAGNOSTIC, 0, 0x10,  6, SG_DXFER_TO_DEV, &cp,
								sizeof(cp));
	if (rc != 0) {
		fprintf(stderr, "%s: failed to set LED(s) via SES for %s.\n",
						progname, enclosure);
		exit(2);
	}

	if (verbose)
		report_component(&dp, fault, ident, type, index, verbose);
	return 0;
}
