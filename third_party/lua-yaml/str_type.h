#include <string.h>

enum yaml_type{YAML_NO_MATCH = 0, YAML_FALSE, YAML_TRUE, YAML_NULL};

/*
 * Parse yaml string and null as specified in
 * http://yaml.org/spec/1.2/spec.html 10.3.2. Tag Resolution
 */

static yaml_type
yaml_get_bool(const char *str, const size_t len){
	if (len == 5) {
		if ((strncmp(str, "false", len) == 0)
		    || (strncmp(str, "False", len) == 0)
		    || (strncmp(str, "FALSE", len) == 0))
			return YAML_FALSE;
	}
	if (len == 4) {
		if ((strncmp(str, "true", len) == 0)
		    || (strncmp(str, "True", len) == 0)
		    || (strncmp(str, "TRUE", len) == 0))
			return YAML_TRUE;
	}
	/*
	 * `yes` and `no` patterns are common non-stantard
	 * extension. Should be supported to be consistent with
	 * common implementations (CPython)
	 */
	if (len == 3) {

	}
	return YAML_NO_MATCH;
}

static yaml_type
yaml_get_null(const char *str, const size_t len){
	if (len == 0 || (len == 1 && str[0] == '~'))
		return YAML_NULL;
	if (len == 4) {
		if ((strncmp(str, "null", len) == 0)
		    || (strncmp(str, "Null", len) == 0)
		    || (strncmp(str, "NULL", len) == 0))
			return YAML_NULL;
	}
	return YAML_NO_MATCH;
}
