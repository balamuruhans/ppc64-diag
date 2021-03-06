#include <stdlib.h>
#include <stdio.h>

#include "bluehawk.h"
#include "encl_common.h"
#include "encl_util.h"
#include "test_utils.h"

extern struct bluehawk_diag_page2 healthy_page;

/* healthy pg2 for bluehawk */
int
main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s pathname\n", argv[0]);
		exit(1);
	}

	convert_htons(&healthy_page.page_length);
	convert_htons(&healthy_page.overall_voltage_status.voltage);
	convert_htons(&healthy_page.voltage_sensor_sets[0].sensor_12V.voltage);
	convert_htons(&healthy_page.voltage_sensor_sets[0].sensor_3_3VA.voltage);
	convert_htons(&healthy_page.voltage_sensor_sets[1].sensor_12V.voltage);
	convert_htons(&healthy_page.voltage_sensor_sets[1].sensor_3_3VA.voltage);

	if (write_page2_to_file(argv[1],
		 &healthy_page, sizeof(healthy_page)) != 0)
		exit(2);
	exit(0);
}
