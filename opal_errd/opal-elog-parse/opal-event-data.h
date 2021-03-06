#ifndef _H_OPAL_EVENTS_DATA
#define _H_OPAL_EVENTS_DATA

#include <stdint.h>

struct generic_desc{
	uint8_t id;
	const char *desc;
};

const char *get_event_desc(uint8_t id);

const char *get_subsystem_name(uint8_t id);

const char *get_severity_desc(uint8_t id);

const char *get_creator_name(uint8_t id);

const char *get_event_scope(uint8_t id);

const char *get_elog_desc(uint8_t id);

const char *get_fru_priority_desc(uint8_t id);

const char *get_fru_component_desc(uint8_t id);

const char *get_ep_event_desc(uint8_t id);

const char *get_lr_res_desc(uint8_t id);

const char *get_ie_type_desc(uint8_t id);

const char *get_ie_scope_desc(uint8_t id);

const char *get_ie_subtype_desc(uint8_t id);

const char *get_dh_type_desc(uint8_t id);
#endif /* _H_OPAL_EVENTS_DATA */
