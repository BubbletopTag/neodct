/*
 * Tests for neodct_mem: /proc/self/status VmRSS/VmHWM parsing used
 * by the serial memory-pressure log.
 */

#include "test_util.h"
#include "../neodct_mem.h"

static const char status_text[] =
	"Name:\tnetsurf-fb\n"
	"Umask:\t0022\n"
	"VmPeak:\t   48620 kB\n"
	"VmSize:\t   45120 kB\n"
	"VmHWM:\t   21340 kB\n"
	"VmRSS:\t   19872 kB\n"
	"VmData:\t   12000 kB\n";

int main(void)
{
	CHECK_INT(neodct_mem_parse_field(status_text, "VmRSS"), 19872);
	CHECK_INT(neodct_mem_parse_field(status_text, "VmHWM"), 21340);
	CHECK_INT(neodct_mem_parse_field(status_text, "VmPeak"), 48620);

	/* missing field yields -1 */
	CHECK_INT(neodct_mem_parse_field(status_text, "VmSwap"), -1);
	CHECK_INT(neodct_mem_parse_field("", "VmRSS"), -1);
	CHECK_INT(neodct_mem_parse_field(NULL, "VmRSS"), -1);

	/* prefix of another field must not match (Vm vs VmRSS) */
	CHECK_INT(neodct_mem_parse_field("XVmRSS:\t1 kB\n", "VmRSS"), -1);

	/* /proc/meminfo format (space padding, no tabs) parses too */
	{
		static const char meminfo[] =
			"MemTotal:         495616 kB\n"
			"MemFree:          401232 kB\n"
			"MemAvailable:     441516 kB\n"
			"SwapTotal:         32768 kB\n"
			"SwapFree:          32768 kB\n";
		CHECK_INT(neodct_mem_parse_field(meminfo, "MemAvailable"),
			  441516);
		CHECK_INT(neodct_mem_parse_field(meminfo, "MemFree"), 401232);
		CHECK_INT(neodct_mem_parse_field(meminfo, "SwapFree"), 32768);
	}

	/* async-signal-safe number formatting for the crash handler */
	{
		char buf[24];

		CHECK_INT(neodct_mem_format_long(buf, sizeof(buf), 0), 1);
		CHECK_STR(buf, "0");
		CHECK_INT(neodct_mem_format_long(buf, sizeof(buf), 11), 2);
		CHECK_STR(buf, "11");
		CHECK_INT(neodct_mem_format_long(buf, sizeof(buf), 19712), 5);
		CHECK_STR(buf, "19712");
		CHECK_INT(neodct_mem_format_long(buf, sizeof(buf), -9), 2);
		CHECK_STR(buf, "-9");
		/* too-small buffer refuses cleanly */
		CHECK_INT(neodct_mem_format_long(buf, 3, 12345), -1);
	}

	TEST_EXIT();
}
