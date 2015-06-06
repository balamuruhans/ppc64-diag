/* Copyright (C) 2012 IBM Corporation */

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <scsi/scsi.h>
#include <scsi/sg.h>

#include "encl_util.h"
#include "diag_encl.h"
#include "bluehawk.h"

#define ES_STATUS_STRING_MAXLEN		32
#define EVENT_DESC_SIZE			512

#define FRU_NUMBER_LEN			8
#define SERIAL_NUMBER_LEN		12

/* SRN Format :
 *	for SAS : 2667-xxx
 */

/* SAS SRN */
#define SAS_SRN			0x2667

/* SAS SES Reported Fail Indicator */
#define CRIT_PS			0x125
#define CRIT_FAN		0x135
#define CRIT_ESM		0x155
#define CRIT_EN			0x175
#define DEVICE_CONFIG_ERROR	0x201
#define ENCLOSURE_OPEN_FAILURE	0x202
#define ENQUIRY_DATA_FAIL	0x203
#define MEDIA_BAY		0x210
#define VOLTAGE_THRESHOLD	0x239
#define PS_TEMP_THRESHOLD	0x145
#define TEMP_THRESHOLD		0x246

/* Build SRN */
#define SRN_SIZE	16
#define build_srn(srn, element) \
	snprintf(srn, SRN_SIZE, "%03X-%03X", SAS_SRN, element)

static struct element_descriptor_page *edp;	/* for power supply VPD */

static int poked_leds;

static int
read_page2_from_file(struct bluehawk_diag_page2 *pg, const char *path,
						int complain_if_missing)
{
	FILE *f;

	f = fopen(path, "r");
	if (!f) {
		if (complain_if_missing || errno != ENOENT)
			perror(path);
		return -1;
	}
	if (fread(pg, sizeof(*pg), 1, f) != 1) {
		perror(path);
		fclose(f);
		return -2;
	}
	fclose(f);
	return 0;
}

static int
write_page2_to_file(const struct bluehawk_diag_page2 *pg, const char *path)
{
	FILE *f;

	f = fopen(path, "w");
	if (!f) {
		perror(path);
		return -1;
	}
	if (fwrite(pg, sizeof(*pg), 1, f) != 1) {
		perror(path);
		fclose(f);
		return -2;
	}
	fclose(f);
	return 0;
}

#define CHK_IDENT_LED(s) if ((s)->ident) printf(" | IDENT_LED")
#define CHK_FAULT_LED(s) if ((s)->fail) printf(" | FAULT_LED")

static int
status_is_valid(enum element_status_code sc,
				enum element_status_code valid_codes[])
{
	enum element_status_code *v;
	for (v = valid_codes; *v < ES_EOL; v++)
		if (sc == *v)
			return 1;
	return 0;
}

static const char *
status_string(enum element_status_code sc,
				enum element_status_code valid_codes[])
{
	static char invalid_msg[40];	/* So we're not reentrant. */
	if (!status_is_valid(sc, valid_codes)) {
		snprintf(invalid_msg, 40, "(UNEXPECTED_STATUS_CODE=%u)", sc);
		return invalid_msg;
	}
	switch (sc) {
	default:
	case ES_UNSUPPORTED:
		return "UNSUPPORTED";
	case ES_OK:
		return "ok";
	case ES_CRITICAL:
		return "CRITICAL_FAULT";
	case ES_NONCRITICAL:
		return "NON_CRITICAL_FAULT";
	case ES_UNRECOVERABLE:
		return "UNRECOVERABLE_FAULT";
	case ES_NOT_INSTALLED:
		return "(empty)";
	case ES_UNKNOWN:
		return "UNKNOWN";
	case ES_NOT_AVAILABLE:
		return "NOT_AVAILABLE";
	case ES_NO_ACCESS_ALLOWED:
		return "NO_ACCESS_ALLOWED";
	}
	/*NOTREACHED*/
}

static void
print_drive_status(struct disk_status *s)
{
	enum element_status_code sc =
				(enum element_status_code) s->byte0.status;
	static enum element_status_code valid_codes[] = {
		ES_OK, ES_CRITICAL, ES_NONCRITICAL, ES_NOT_INSTALLED, ES_EOL
	};

	printf("%s", status_string(sc, valid_codes));

	if (s->ready_to_insert)
		printf(" | INSERT");
	if (s->rmv)
		printf(" | REMOVE");
	if (s->app_client_bypassed_a)
		printf(" | APP_CLIENT_BYPASSED_A");
	if (s->app_client_bypassed_b)
		printf(" | APP_CLIENT_BYPASSED_B");
	if (s->bypassed_a)
		printf(" | BYPASSED_A");
	if (s->bypassed_b)
		printf(" | BYPASSED_B");
	CHK_IDENT_LED(s);
	CHK_FAULT_LED(s);
	printf("\n");
}

static void
print_power_supply_status(struct power_supply_status *s)
{
	enum element_status_code sc =
				(enum element_status_code) s->byte0.status;
	static enum element_status_code valid_codes[] = {
		ES_OK, ES_CRITICAL, ES_NONCRITICAL, ES_NOT_INSTALLED, ES_EOL
	};

	printf("%s", status_string(sc, valid_codes));

	if (s->dc_fail)
		printf(" | DC_FAIL");
	if (s->dc_over_voltage)
		printf(" | DC_OVER_VOLTAGE");
	if (s->dc_under_voltage)
		printf(" | DC_UNDER_VOLTAGE");
	if (s->dc_over_current)
		printf(" | DC_OVER_CURRENT");
	if (s->ac_fail)
		printf(" | AC_FAIL");
	CHK_IDENT_LED(s);
	CHK_FAULT_LED(s);
	printf("\n");
}

static void
print_voltage_sensor_status(struct voltage_sensor_status *s)
{
	enum element_status_code sc =
				(enum element_status_code) s->byte0.status;
	static enum element_status_code valid_codes[] = {
		ES_OK, ES_CRITICAL, ES_NONCRITICAL, ES_NOT_INSTALLED,
		ES_UNKNOWN, ES_EOL
	};

	printf("%s", status_string(sc, valid_codes));

	if (s->warn_over)
		printf(" | NON_CRITICAL_OVER_VOLTAGE");
	if (s->warn_under)
		printf(" | NON_CRITICAL_UNDER_VOLTAGE");
	if (s->crit_over)
		printf(" | CRITICAL_OVER_VOLTAGE");
	if (s->crit_under)
		printf(" | CRITICAL_UNDER_VOLTAGE");
	if (cmd_opts.verbose)
		/* between +327.67 to -327.68
		 */
		printf(" | VOLTAGE = %.2f volts", s->voltage / 100.0);
	printf("\n");
}

static void
print_fan_status(struct fan_status *s)
{
	const char *speed[] = {
		"Fan at lowest speed",
		"Fan at 1-16% of highest speed",
		"Fan at 17-33% of highest speed",
		"Fan at 34-49% of highest speed",
		"Fan at 50-66% of highest speed",
		"Fan at 67-83% of highest speed",
		"Fan at 84-99% of highest speed",
		"Fan at highest speed"
	};

	enum element_status_code sc =
				(enum element_status_code) s->byte0.status;
	static enum element_status_code valid_codes[] = {
		ES_OK, ES_CRITICAL, ES_NONCRITICAL, ES_NOT_INSTALLED,
		ES_UNKNOWN, ES_EOL
	};

	printf("%s", status_string(sc, valid_codes));

	CHK_IDENT_LED(s);
	CHK_FAULT_LED(s);
	if (cmd_opts.verbose)
		printf(" | %s", speed[s->speed_code]);
	printf("\n");
}

static void
print_temp_sensor_status(struct temperature_sensor_status *s)
{
	enum element_status_code sc =
				(enum element_status_code) s->byte0.status;
	static enum element_status_code valid_codes[] = {
		ES_OK, ES_CRITICAL, ES_NONCRITICAL, ES_NOT_INSTALLED,
		ES_UNKNOWN, ES_EOL
	};

	printf("%s", status_string(sc, valid_codes));

	if (s->ot_failure)
		printf(" | OVER_TEMPERATURE_FAILURE");
	if (s->ot_warning)
		printf(" | OVER_TEMPERATURE_WARNING");
	if (cmd_opts.verbose)
		/* between -19 and +235 degrees Celsius */
		printf(" | TEMPERATURE = %dC", s->temperature - 20);
	printf("\n");
}

static void
print_enclosure_status(struct enclosure_status *s)
{
	enum element_status_code sc =
				(enum element_status_code) s->byte0.status;
	/* Note: Deviation from spec V0.7
	 *	 Spec author says below are valid state
	 */
	static enum element_status_code valid_codes[] = {
		ES_OK, ES_CRITICAL, ES_NONCRITICAL, ES_EOL };

	printf("%s", status_string(sc, valid_codes));

	if (s->failure_requested)
		printf(" | FAILURE_REQUESTED");
	CHK_IDENT_LED(s);
	CHK_FAULT_LED(s);
	printf("\n");
}

static void
print_esm_status(struct esm_status *s)
{
	enum element_status_code sc =
				(enum element_status_code) s->byte0.status;
	static enum element_status_code valid_codes[] = {
		ES_OK, ES_CRITICAL, ES_NOT_INSTALLED, ES_EOL
	};

	printf("%s", status_string(sc, valid_codes));

	CHK_IDENT_LED(s);
	CHK_FAULT_LED(s);
	printf("\n");
}

static void
print_sas_connector_status(struct sas_connector_status *s)
{
	enum element_status_code sc =
				(enum element_status_code) s->byte0.status;
	static enum element_status_code valid_codes[] = {
		ES_OK, ES_NONCRITICAL, ES_NOT_INSTALLED, ES_EOL
	};

	printf("%s", status_string(sc, valid_codes));

	CHK_IDENT_LED(s);
	CHK_FAULT_LED(s);
	printf("\n");
}

static void
print_scc_controller_status(struct scc_controller_element_status *s)
{
	enum element_status_code sc =
				(enum element_status_code) s->byte0.status;
	static enum element_status_code valid_codes[] = {
		ES_OK, ES_CRITICAL, ES_NOT_INSTALLED, ES_NOT_AVAILABLE, ES_EOL
	};

	printf("%s", status_string(sc, valid_codes));

	if (s->report)
		printf(" | REPORT");
	CHK_IDENT_LED(s);
	CHK_FAULT_LED(s);
	printf("\n");
}

static void
print_midplane_status(struct midplane_status *s)
{
	enum element_status_code sc =
				(enum element_status_code) s->byte0.status;
	static enum element_status_code valid_codes[] = {
		ES_OK, ES_CRITICAL, ES_EOL
	};

	printf("%s", status_string(sc, valid_codes));

	CHK_IDENT_LED(s);
	CHK_FAULT_LED(s);
	printf("\n");
}

/* Helper functions for reporting faults to servicelog start here. */

static char *
strzcpy(char *dest, const char *src, size_t n)
{
	memcpy(dest, src, n);
	dest[n] = '\0';
	return dest;
}

/*
 * Factor new status into the composite status cur.  A missing element
 * (ES_NOT_INSTALLED) is ignored.  A non-critical status is less severe
 * than critical.  Otherwise assume that an increasing value of
 * element_status_code indicates an increasing severity.  Return the more
 * severe of new and cur.
 */
enum element_status_code
worse_element_status(enum element_status_code cur, enum element_status_code new)
{
	if (new == ES_OK || new == ES_NOT_INSTALLED)
		return cur;
	if ((cur == ES_OK || cur == ES_NONCRITICAL) && new > ES_OK)
		return new;
	return cur;
}

/*
 * Calculate the composite status for the nel elements starting at
 * address first_element.  We exploit the fact that every status element
 * is 4 bytes and starts with an element_status_byte0 struct.
 */
enum element_status_code
composite_status(const void *first_element, int nel)
{
	int i;
	const char *el = (const char *)first_element;
	enum element_status_code s = ES_OK;
	const struct element_status_byte0 *new_byte0;

	for (i = 0; i < nel; i++, el += 4) {
		new_byte0 = (const struct element_status_byte0 *) el;
		s = worse_element_status(s,
				(enum element_status_code) new_byte0->status);
	}
	return s;
}

static int
status_worsened(enum element_status_code old, enum element_status_code new)
{
	return (worse_element_status(old, new) != old);
}

/*
 * b is the address of a status byte 0 in dp (i.e., the status page we just
 * read from the SES).  If prev_dp has been populated, compare the old and
 * new status, and return 1 if the new status is worse, 0 otherwise.  If
 * prev_dp isn't valid, return 1.
 */
static int
element_status_reportable(const struct element_status_byte0 *new,
			  const char * const dp, const char * const prev_dp)
{
	ptrdiff_t offset;
	struct element_status_byte0 *old;

	if (!prev_dp)
		return 1;
	offset = ((char *) new) - dp;
	old = (struct element_status_byte0 *) (prev_dp + offset);
	return status_worsened((enum element_status_code) old->status,
				(enum element_status_code) new->status);
}

/*
 * If the status byte indicates a fault that needs to be reported, return
 * the appropriate servicelog status and start the description text
 * accordingly.  Else return 0.
 */
static uint8_t
svclog_status(enum element_status_code sc, char *crit)
{
	if (sc == ES_CRITICAL) {
		strncpy(crit, "Critical", ES_STATUS_STRING_MAXLEN - 1);
		crit[ES_STATUS_STRING_MAXLEN - 1] = '\0';
		return SL_SEV_ERROR;
	} else if (sc == ES_NONCRITICAL) {
		strncpy(crit, "Non-critical", ES_STATUS_STRING_MAXLEN - 1);
		crit[ES_STATUS_STRING_MAXLEN - 1] = '\0';
		return SL_SEV_WARNING;
	} else
		return 0;
}

static uint8_t
svclog_element_status(struct element_status_byte0 *b, const char * const dp,
		      const char * const prev_dp, char *crit)
{
	if (!element_status_reportable(b, dp, prev_dp))
		return 0;
	return svclog_status(b->status, crit);
}

/*
 * Like element_status_reportable(), except we return 1 if the status of any
 * of the nel elements has worsened.
 */
static int
composite_status_reportable(const void *first_element, const char * const dp,
			    const char * const prev_dp, int nel)
{
	int i;
	const char *el = (const char *) first_element;

	if (!prev_dp)
		return 1;
	for (i = 0; i < nel; i++, el += 4) {
		if (element_status_reportable(
				(const struct element_status_byte0 *) el,
				(char *) dp, (char *) prev_dp))
			return 1;
	}
	return 0;
}

static uint8_t
svclog_composite_status(const void *first_element,  const char * const dp,
			const char * const prev_dp, int nel, char *crit)
{
	if (!composite_status_reportable(first_element, dp, prev_dp, nel))
		return 0;
	return svclog_status(composite_status(first_element, nel), crit);
}

/* Add a callout with just the location code */
static void
add_location_callout(struct sl_callout **callouts, char *location)
{
	add_callout(callouts, 'M', 0, NULL, location, NULL, NULL, NULL);
}

/* Create a callout for power supply i (i = 0 or 1). */
static int
create_ps_callout(struct sl_callout **callouts, char *location,
						unsigned int i, int fd)
{
	char fru_number[FRU_NUMBER_LEN + 1];
	char serial_number[SERIAL_NUMBER_LEN + 1];
	int rc;
	struct power_supply_descriptor *ps_vpd[2];

	if (fd < 0) {
		add_location_callout(callouts, location);
		return 0;
	}

	edp = calloc(1, sizeof(struct element_descriptor_page));
	if (!edp) {
		fprintf(stderr, "Failed to allocate memory to "
			"hold page containing VPD for PS (pg 07h).\n");
		return 1;
	}

	rc = get_diagnostic_page(fd, RECEIVE_DIAGNOSTIC, 7, edp,
				 sizeof(struct element_descriptor_page));
	if (rc) {
		add_location_callout(callouts, location);
		goto out;
	}

	ps_vpd[0] = &(edp->ps0_vpd);
	ps_vpd[1] = &(edp->ps1_vpd);
	strzcpy(fru_number, ps_vpd[i]->fru_number, FRU_NUMBER_LEN);
	strzcpy(serial_number, ps_vpd[i]->serial_number, SERIAL_NUMBER_LEN);
	add_callout(callouts, 'M', 0, NULL,
		    location, fru_number, serial_number, NULL);

out:
	free(edp);
	return 0;
}

static void
add_callout_from_vpd_page(struct sl_callout **callouts, char *location,
							struct vpd_page *vpd)
{
	char fru_number[8+1];
	char serial_number[12+1];
	char ccin[4+1];

	strzcpy(fru_number, vpd->fru_number, 8);
	strzcpy(serial_number, vpd->serial_number, 12);
	strzcpy(ccin, vpd->model_number, 4);
	add_callout(callouts, 'M', 0, NULL, location, fru_number, serial_number,
									ccin);
}

/* 1 = have VPD for a warhawk; -1 = failed to get it. */
static int have_wh_vpd;
static struct vpd_page whp;	/* for warhawk VPD */

/*
 * The fru_label should be "P1-C1" or "P1-C2" (without the terminating null).
 * i is 0 or 1.
 */
static int
wh_location_match(int i, const char *fru_label)
{
	return ('0'+i+1 == fru_label[4]);
}

/*
 * Create a callout for warhawk i (left=0, right=1). VPD page 1 contains VPD
 * for only one of the warhawks.  If it's the wrong one, just do without the
 * VPD.
 *
 * TODO: Figure out how to get VPD for the other warhawk by inquiring via a
 * different sg device.
 */
static void
create_wh_callout(struct sl_callout **callouts, char *location, unsigned int i,
									int fd)
{
	if (fd < 0)
		have_wh_vpd = -1;
	if (!have_wh_vpd) {
		int result = get_diagnostic_page(fd, INQUIRY, 1, &whp,
								sizeof(whp));
		if (result == 0)
			have_wh_vpd = 1;
		else
			have_wh_vpd = -1;
	}
	if (have_wh_vpd == 1 && wh_location_match(i, whp.fru_label))
		add_callout_from_vpd_page(callouts, location, &whp);
	else
		add_location_callout(callouts, location);
}

/* midplane callout, with VPD from page 5 */
static void
create_mp_callout(struct sl_callout **callouts, char *location, int fd)
{
	struct vpd_page mp;
	int result = -1;

	if (fd >= 0)
		result = get_diagnostic_page(fd, INQUIRY, 5, &mp, sizeof(mp));
	if (result == 0)
		add_callout_from_vpd_page(callouts, location, &mp);
	else
		add_location_callout(callouts, location);
}

static int
report_faults_to_svclog(struct dev_vpd *vpd,
			struct bluehawk_diag_page2 *dp, int fd)
{
	char location[LOCATION_LENGTH], *loc_suffix;
	char description[EVENT_DESC_SIZE], crit[ES_STATUS_STRING_MAXLEN];
	char srn[SRN_SIZE];
	unsigned int i;
	int sev, loc_suffix_size;
	char run_diag_encl[] = "  Run diag_encl for more detailed status,"
		" and refer to the system service documentation for guidance.";
	char ref_svc_doc[] =
		"  Refer to the system service documentation for guidance.";
	struct sl_callout *callouts;
	const char *left_right[] = { "left", "right" };
	struct bluehawk_diag_page2 *prev_dp = NULL;	/* for -c */

	have_wh_vpd = 0;
	strncpy(location, vpd->location, LOCATION_LENGTH - 1);
	location[LOCATION_LENGTH - 1] = '\0';
	loc_suffix_size = LOCATION_LENGTH - strlen(location);
	loc_suffix = location + strlen(location);

	if (cmd_opts.cmp_prev) {
		prev_dp = calloc(1, sizeof(struct bluehawk_diag_page2));
		if (!prev_dp) {
			fprintf(stderr, "Failed to allocate memory to hold "
				"prev. status diagnostics page 02 results.\n");
			return 1;
		}

		if (read_page2_from_file(prev_dp, cmd_opts.prev_path, 0) != 0) {
			free(prev_dp);
			prev_dp = NULL;
		}
	}

	/* disk drives */
	for (i = 0; i < NR_DISKS_PER_BLUEHAWK; i++) {
		sev = svclog_element_status(&(dp->disk_status[i].byte0),
					(char *) dp, (char *) prev_dp, crit);
		if (sev == 0)
			continue;
		snprintf(description, EVENT_DESC_SIZE,
			 "%s fault in RAID enclosure disk %u.%s",
			 crit, i + 1, run_diag_encl);
		snprintf(loc_suffix, loc_suffix_size, "-P1-D%u", i+1);
		callouts = NULL;
		/* VPD for disk drives is not available from the SES. */
		add_location_callout(&callouts, location);
		servevent("none", sev, description, vpd, callouts);
	}

	/* power supplies */
	for (i = 0; i < 2; i++) {
		sev = svclog_element_status(&(dp->ps_status[i].byte0),
					(char *) dp, (char *) prev_dp, crit);
		if (sev == 0)
			continue;
		snprintf(description, EVENT_DESC_SIZE,
			 "%s fault in %s power supply in RAID enclosure.%s",
			 crit, left_right[i], run_diag_encl);
		snprintf(loc_suffix, loc_suffix_size, "-P1-E%u", i+1);
		build_srn(srn, CRIT_PS);
		callouts = NULL;
		if (create_ps_callout(&callouts, location, i, fd))
			goto err_out;
		servevent(srn, sev, description, vpd, callouts);
	}

	/* voltage sensors */
	for (i = 0; i < 2; i++) {
		sev = svclog_composite_status(&(dp->voltage_sensor_sets[i]),
			(char *) dp, (char *) prev_dp, 2, crit);
		if (sev == 0)
			continue;
		snprintf(description, EVENT_DESC_SIZE,
			 "%s fault associated with %s power supply in RAID "
			 "enclosure: voltage sensor(s) reporting voltage(s) "
			 "out of range.%s", crit, left_right[i], run_diag_encl);
		snprintf(loc_suffix, loc_suffix_size, "-P1-E%u", i+1);
		build_srn(srn, VOLTAGE_THRESHOLD);
		callouts = NULL;
		if (create_ps_callout(&callouts, location, i, fd))
			goto err_out;
		servevent(srn, sev, description, vpd, callouts);
	}

	/* power-supply fans -- lump with power supplies, not fan assemblies */
	for (i = 0; i < 2; i++) {
		sev = svclog_element_status(
			&(dp->fan_sets[i].power_supply.byte0),
			(char *) dp, (char *) prev_dp, crit);
		if (sev == 0)
			continue;
		snprintf(description, EVENT_DESC_SIZE,
			 "%s fault in fan for %s power supply in RAID "
			 "enclosure.%s", crit, left_right[i], run_diag_encl);
		snprintf(loc_suffix, loc_suffix_size, "-P1-E%u", i+1);
		build_srn(srn, CRIT_PS);
		callouts = NULL;
		if (create_ps_callout(&callouts, location, i, fd))
			goto err_out;
		servevent(srn, sev, description, vpd, callouts);
	}

	/* fan assemblies */
	for (i = 0; i < 2; i++) {
		/* 4 fans for each fan assembly */
		sev = svclog_composite_status(&(dp->fan_sets[i].fan_element),
				(char *) dp, (char *) prev_dp, 4, crit);
		if (sev == 0)
			continue;
		snprintf(description, EVENT_DESC_SIZE,
			 "%s fault in %s fan assembly in RAID enclosure.%s",
			 crit, left_right[i], run_diag_encl);
		snprintf(loc_suffix, loc_suffix_size, "-P1-C%u-A1", i+1);
		build_srn(srn, CRIT_FAN);
		callouts = NULL;
		/* VPD for fan assemblies is not available from the SES. */
		add_location_callout(&callouts, location);
		servevent(srn, sev, description, vpd, callouts);
	}

	/* power-supply temperature sensors -- lump with power supplies */
	for (i = 0; i < 2; i++) {
		/* 2 sensors for each power supply */
		sev = svclog_composite_status(
			&(dp->temp_sensor_sets[i].power_supply),
			(char *) dp, (char *) prev_dp, 2, crit);
		if (sev == 0)
			continue;
		snprintf(description, EVENT_DESC_SIZE,
			 "%s fault associated with %s power supply in RAID "
			 "enclosure: temperature sensor(s) reporting "
			 "temperature(s) out of range.%s",
			 crit, left_right[i], run_diag_encl);
		snprintf(loc_suffix, loc_suffix_size, "-P1-E%u", i+1);
		build_srn(srn, PS_TEMP_THRESHOLD);
		callouts = NULL;
		if (create_ps_callout(&callouts, location, i, fd))
			goto err_out;
		servevent(srn, sev, description, vpd, callouts);
	}

	/* temp sensors, except for those associated with power supplies */
	for (i = 0; i < 2; i++) {
		/* 5 sensors: croc, ppc, expander, 2*ambient */
		sev = svclog_composite_status(&(dp->temp_sensor_sets[i]),
			(char *) dp, (char *) prev_dp, 5, crit);
		if (sev == 0)
			continue;
		snprintf(description, EVENT_DESC_SIZE,
			 "%s fault associated with %s side of RAID "
			 "enclosure: temperature sensor(s) reporting "
			 "temperature(s) out of range.%s",
			 crit, left_right[i], run_diag_encl);
		/* Not the power supply, so assume the warhawk. */
		snprintf(loc_suffix, loc_suffix_size, "-P1-C%u", i+1);
		build_srn(srn, TEMP_THRESHOLD);
		callouts = NULL;
		create_wh_callout(&callouts, location, i, fd);
		servevent(srn, sev, description, vpd, callouts);
	}

	/* ERM/ESM electronics */
	for (i = 0; i < 2; i++) {
		sev = svclog_element_status(&(dp->esm_status[i].byte0),
					(char *) dp, (char *) prev_dp, crit);
		if (sev == 0)
			continue;
		snprintf(description, EVENT_DESC_SIZE,
			 "%s electronics fault in %s Enclosure RAID Module.%s",
			 crit, left_right[i], ref_svc_doc);
		snprintf(loc_suffix, loc_suffix_size, "-P1-C%u", i+1);
		build_srn(srn, CRIT_ESM);
		callouts = NULL;
		create_wh_callout(&callouts, location, i, fd);
		servevent(srn, sev, description, vpd, callouts);
	}

	/* SAS connectors */
	for (i = 0; i < 4; i++) {
		unsigned int t = i%2 + 1, lr = i/2;
		sev = svclog_element_status(
				&(dp->sas_connector_status[i].byte0),
				(char *) dp, (char *) prev_dp, crit);
		if (sev == 0)
			continue;
		snprintf(description, EVENT_DESC_SIZE,
			 "%s fault in SAS connector T%u of %s RAID Enclosure"
			 " Module.%s", crit, t, left_right[lr], ref_svc_doc);
		snprintf(loc_suffix, loc_suffix_size, "-P1-C%u-T%u", lr+1, t);
		callouts = NULL;
		/* No VPD for SAS connectors in the SES. */
		add_location_callout(&callouts, location);
		servevent("none", sev, description, vpd, callouts);
	}

	/* PCIe controllers */
	for (i = 0; i < 2; i++) {
		sev = svclog_element_status(
			&(dp->scc_controller_status[i].byte0),
			(char *) dp, (char *) prev_dp, crit);
		if (sev == 0)
			continue;
		snprintf(description, EVENT_DESC_SIZE,
			 "%s fault in PCIe controller for %s RAID Enclosure "
			 "Module.%s", crit, left_right[i], ref_svc_doc);
		snprintf(loc_suffix, loc_suffix_size, "-P1-C%u-T3", i+1);
		callouts = NULL;
		/* No VPD for PCIe controllers in the SES. */
		add_location_callout(&callouts, location);
		servevent("none", sev, description, vpd, callouts);
	}

	/* midplane */
	sev = svclog_element_status(&(dp->midplane_element_status.byte0),
				(char *) dp, (char *) prev_dp, crit);
	if (sev != 0) {
		snprintf(description, EVENT_DESC_SIZE,
			 "%s fault in midplane of RAID enclosure.%s",
			 crit, ref_svc_doc);
		strncpy(loc_suffix, "-P1", loc_suffix_size - 1);
		loc_suffix[loc_suffix_size - 1] = '\0';
		callouts = NULL;
		create_mp_callout(&callouts, location, fd);
		servevent("none", sev, description, vpd, callouts);
	}

	if (prev_dp)
		free(prev_dp);

	return write_page2_to_file(dp, cmd_opts.prev_path);

err_out:
	if (prev_dp)
		free(prev_dp);
	return 1;
}

/*
 * If the indicated status element reports a fault, turn on the fault component
 * of the LED if it's not already on.  Keep the identify LED element unchanged.
 */
#define FAULT_LED(ctrl_element, status_element) \
do { \
	enum element_status_code sc = dp->status_element.byte0.status; \
	if (!dp->status_element.fail && \
			(sc == ES_CRITICAL || sc == ES_NONCRITICAL || \
			 sc == ES_UNRECOVERABLE)) { \
		ctrl_page->ctrl_element.common_ctrl.select = 1; \
		ctrl_page->ctrl_element.rqst_fail = 1; \
		ctrl_page->ctrl_element.rqst_ident = dp->status_element.ident; \
		poked_leds++; \
	} \
} while (0)

static int
turn_on_fault_leds(struct bluehawk_diag_page2 *dp, int fd)
{
	int i;
	struct bluehawk_ctrl_page2 *ctrl_page;

	poked_leds = 0;

	ctrl_page = calloc(1, sizeof(struct bluehawk_ctrl_page2));
	if (!ctrl_page) {
		fprintf(stderr, "Failed to allocate memory to hold "
				"control diagnostics page 02.\n");
		return 1;
	}

	/* disk drives */
	for (i = 0; i < NR_DISKS_PER_BLUEHAWK; i++)
		FAULT_LED(disk_ctrl[i], disk_status[i]);

	/* power supplies */
	for (i = 0; i < 2; i++)
		FAULT_LED(ps_ctrl[i], ps_status[i]);

	/* No LEDs for voltage sensors */

	/* fan assemblies */
	for (i = 0; i < 2; i++) {
		enum element_status_code sc =
				composite_status(&(dp->fan_sets[i]), 5);
		if (sc != ES_OK && sc != ES_NOT_INSTALLED)
			FAULT_LED(fan_sets[i].fan_element[0],
						fan_sets[i].fan_element[0]);
	}

	/* No LEDs for temperature sensors */

	/* ERM/ESM electronics */
	for (i = 0; i < 2; i++)
		FAULT_LED(esm_ctrl[i], esm_status[i]);

	/* SAS connectors */
	for (i = 0; i < 4; i++)
		FAULT_LED(sas_connector_ctrl[i], sas_connector_status[i]);

	/* PCIe controllers */
	for (i = 0; i < 2; i++)
		FAULT_LED(scc_controller_ctrl[i], scc_controller_status[i]);

	/* midplane */
	FAULT_LED(midplane_element_ctrl, midplane_element_status);

	if (poked_leds) {
		int result;

		ctrl_page->page_code = 2;
		ctrl_page->page_length = sizeof(struct bluehawk_ctrl_page2) - 4;
		ctrl_page->generation_code = 0;
		result = do_ses_cmd(fd, SEND_DIAGNOSTIC, 0, 0x10, 6,
				SG_DXFER_TO_DEV, ctrl_page,
				sizeof(struct bluehawk_ctrl_page2));
		if (result != 0) {
			perror("ioctl - SEND_DIAGNOSTIC");
			fprintf(stderr, "result = %d\n", result);
			fprintf(stderr, "failed to set LED(s) via SES\n");
			free(ctrl_page);
			return -1;
		}
	}

	free(ctrl_page);

	return 0;
}

/* @return 0 for success, 1 for failure */
int
diag_bluehawk(int fd, struct dev_vpd *vpd)
{
	int i;
	static const char *power_supply_names[] = {
		"PS0 (Left)",
		"PS1 (Right)"
	};
	static const char *fan_set_names[] = {
		"Left Fan Assembly",
		"Right Fan Assembly",
	};
	static const char *temp_sensor_set_names[] = { "Left", "Right" };
	static const char *esm_names[] = { "Left", "Right" };
	static const char *sas_connector_names[] = {
		"Left - T1",
		"Left - T2",
		"Right - T1",
		"Right - T2"
	};
	static const char *scc_controller_names[] = { "Left", "Right" };

	int rc;
	struct bluehawk_diag_page2 *dp;

	dp = calloc(1, sizeof(struct bluehawk_diag_page2));
	if (!dp) {
		fprintf(stderr, "Failed to allocate memory to hold "
			"current status diagnostics page 02 results.\n");
		return 1;
	}

	if (cmd_opts.fake_path) {
		rc = read_page2_from_file(dp, cmd_opts.fake_path, 1);
		fd = -1;
	} else
		rc = get_diagnostic_page(fd, RECEIVE_DIAGNOSTIC, 2,
			(void *)dp, (int) sizeof(struct bluehawk_diag_page2));
	if (rc != 0) {
		fprintf(stderr, "Failed to read SES diagnostic page; "
				"cannot report status.\n");
		goto err_out;
	}

	printf("  Overall Status:    ");
	if (dp->crit) {
		printf("CRITICAL_FAULT");
		if (dp->non_crit)
			printf(" | NON_CRITICAL_FAULT");
	} else if (dp->non_crit)
		printf("NON_CRITICAL_FAULT");
	else
		printf("ok");

	printf("\n\n  Drive Status\n");
	for (i = 0; i < NR_DISKS_PER_BLUEHAWK; i++) {
		struct disk_status *ds = &(dp->disk_status[i]);
		printf("    Disk %02d (Slot %02d): ", i+1,
				ds->byte1.element_status.slot_address);
		print_drive_status(ds);
	}

	printf("\n  Power Supply Status\n");
	for (i = 0; i < 2; i++) {
		printf("    %s:  ", power_supply_names[i]);
		print_power_supply_status(&(dp->ps_status[i]));
		printf("      12V:  ");
		print_voltage_sensor_status(
				&(dp->voltage_sensor_sets[i].sensor_12V));
		printf("      3.3VA:  ");
		print_voltage_sensor_status(
				&(dp->voltage_sensor_sets[i].sensor_3_3VA));
	}

	printf("\n  Fan Status\n");
	for (i = 0; i < 2; i++) {
		int j;
		printf("    %s:\n", fan_set_names[i]);
		printf("      Power Supply:  ");
		print_fan_status(&(dp->fan_sets[i].power_supply));
		for (j = 0; j < 4; j++) {
			printf("      Fan Element %d:  ", j);
			print_fan_status(&(dp->fan_sets[i].fan_element[j]));
		}
	}

	printf("\n  Temperature Sensors\n");
	for (i = 0; i < 2; i++) {
		int j;
		struct temperature_sensor_set *tss = &(dp->temp_sensor_sets[i]);
		printf("    %s:\n", temp_sensor_set_names[i]);
		printf("      CRoC:  ");
		print_temp_sensor_status(&tss->croc);
		printf("      PPC:  ");
		print_temp_sensor_status(&tss->ppc);
		printf("      Expander:  ");
		print_temp_sensor_status(&tss->expander);
		for (j = 0; j < 2; j++) {
			printf("      Ambient %d:  ", j);
			print_temp_sensor_status(&tss->ambient[j]);
		}
		for (j = 0; j < 2; j++) {
			printf("      Power Supply %d:  ", j);
			print_temp_sensor_status(&tss->power_supply[j]);
		}
	}

	printf("\n  Enclosure Status:  ");
	print_enclosure_status(&(dp->enclosure_element_status));

	printf("\n  ERM Electronics Status\n");
	for (i = 0; i < 2; i++) {
		printf("    %s:  ", esm_names[i]);
		print_esm_status(&(dp->esm_status[i]));
	}

	printf("\n  SAS Connector Status\n");
	for (i = 0; i < 4; i++) {
		printf("    %s:  ", sas_connector_names[i]);
		print_sas_connector_status(&(dp->sas_connector_status[i]));
	}

	printf("\n  PCIe Controller Status\n");
	for (i = 0; i < 2; i++) {
		printf("    %s:  ", scc_controller_names[i]);
		print_scc_controller_status(&(dp->scc_controller_status[i]));
	}

	printf("\n  Midplane Status:  ");
	print_midplane_status(&(dp->midplane_element_status));

	if (cmd_opts.verbose) {
		printf("\n\nRaw diagnostic page:\n");
		print_raw_data(stdout, (char *) dp,
				sizeof(struct bluehawk_diag_page2));
	}

	/*
	 * Report faults to servicelog, and turn on LEDs as appropriate.
	 * LED status reported previously may not be accurate after we
	 * do this, but the alternative is to report faults first and then
	 * read the diagnostic page a second time.  And unfortunately, the
	 * changes to LED settings don't always show up immediately in
	 * the next query of the SES.
	 */
	if (cmd_opts.serv_event) {
		rc = report_faults_to_svclog(vpd, dp, fd);
		if (rc != 0)
			goto err_out;
	}

	/* -l is not supported for fake path */
	if (fd != -1 && cmd_opts.leds)
		rc = turn_on_fault_leds(dp, fd);

err_out:
	free(dp);
	return (rc != 0);
}
