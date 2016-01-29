/*

	Tomato Firmware
	Copyright (C) 2006-2009 Jonathan Zarate

*/
#include "rc.h"

int gpio_main(int argc, char *argv[])
{
	const char hex[] = "12345678";
	unsigned long v;
	int bit;
	int i;
	char s[40], *ptr;
	int f;

	if ((argc == 3) && ((strncmp(argv[1], "en", 2) == 0) || (strncmp(argv[1], "di", 2) == 0))) {
		bit = atoi(argv[2]);
		if ((bit >= 0) && (bit <= 32)) {	// Loy 160128, 32 bits gpio.
			bit = 1 << bit;
			{
				gpio_write(bit, argv[1][0] == 'e');
				return 0;
			}
		}
	}
	else if (argc >= 2) {
		if (strncmp(argv[1], "po", 2) == 0) {
			if (argc >= 3)
				bit = atoi(argv[2]);
			else
				bit = 0;
			printf("Enable gpio mask: 0x%04X\n", bit);

			if ((f = gpio_open(bit)) < 0) {
				printf("Failed to open gpio\n");
				return 0;
			}
			while ((v = _gpio_read(f)) != ~0) {
				ptr = s;
				for (i=31; i>=0; i--) { 	// Loy 160128, 32 bits gpio.
					*ptr = (v & (1 << i)) ? hex[i%8] : '.';
					ptr++;
					if (i%8 == 0) {
						*ptr = ' ';
						ptr++;
					}
				}
				*ptr = 0;
				printf("%08lX: %s\n", v, s);
				sleep(1);
			}
			close(f);
			return 0;
		}
	}

	usage_exit(argv[0], "<enable|disable|poll> <pin|[poll_mask]>\n");
}
