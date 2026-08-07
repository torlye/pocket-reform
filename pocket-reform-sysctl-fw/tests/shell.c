#include "sysctl.h"
#include <stdio.h>
#include "../inc/cli.h"

/* stubs */

void turn_som_power_off()
{
	printf("[cli_stub] turn_som_power_off()");
}
void turn_som_power_on()
{
	printf("[cli_stub] turn_som_power_on()");
}

/* end stubs */

void cli_test(char *str)
{
	printf("> %s", str);
	int len = strlen(str);
	for (int i = 0; i < len; i++) {
		cli_char(str[i]);
	}
	printf("%s\n\n", cli_get_out());
	cli_reset_out();
}

int main(int argc, char **argv)
{
	cli_init();
	cli_test("()\n");
	cli_test("(h)\n");
	cli_test("(poweroff)\n");
	cli_test("(vars 0)\n");
	cli_test("vars 2\n");
	cli_test("vars 2 1\n");
	cli_test("vars 10\n");
	cli_test("1p\n");
	cli_test("(set hello 12345678)\n");
	cli_test("vars\n");
	cli_test("hello\n");
	cli_test("(hello)\n");
	cli_test("(set hello #ffaabbcc)\n");
	cli_test("vars\n");
	cli_test("hello\n");
	return 0;
}
