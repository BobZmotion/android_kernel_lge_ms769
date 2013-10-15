/* 
 * arch/arm/mach-omap2/lge/p940/p940_cmdline.c
 *
 * Copyright (C) 2011 LGE, Inc
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/ctype.h>
#include <linux/string.h>
#include <asm/setup.h>

struct cmdline_parameter {
	char *key;
	char *value; /* If you want to remove key, set as "" */
	int new;     /* if you add new parameter, set as 1 */
};

#define NO_CONSOLE_SUSPEND

static struct cmdline_parameter cmdline_parameters[] __initdata = {
	{"mem", "1G@0x80000000", 0},
};

#ifdef NO_CONSOLE_SUSPEND
static void __init no_console_suspend(char *s) {
	char *p = strstr(s, "console");
	if (p) {
		printk("%s: %s\n", __func__, p);
		strlcat(s, " no_console_suspend", COMMAND_LINE_SIZE);
	}
}
#endif

static char *matchstr(const char *s1, const char *s2)
{
	char *p, *p2;
	bool matched;

	p = s1;
	do {
		p = strstr(p, s2);
		if (p) {
			matched = false;
			if (p == s1)
				matched = true;
			else if (isspace(*(p-1)))
				matched = true;

			if (matched) {
				p2 = p + strlen(s2);
				if (isspace(*p2) || *p2 == '=')
					return p;
			}
			p += strlen(s2);
		}
	} while (p);

	return NULL;
}

static void __init fill_whitespace(char *s)
{
	char *p;

	for (p = s; !isspace(*p) && *p != '\0'; ++p)
		*p = ' ';
}

void __init lge_manipulate_cmdline(char *default_command_line)
{
	char *s, *p, *p2;
	int i;
	int ws;

	s = default_command_line;

	/* remove parameters */
	for (i = 0; i < ARRAY_SIZE(cmdline_parameters); i++) {
		p = s;
		do {
			p = matchstr(p, cmdline_parameters[i].key);
			if (p)
				fill_whitespace(p);
		} while(p);
	}

	/* trim the whitespace */
	p = p2 = s;
	ws = 0; /* whitespace */
	for (i = 0; i < COMMAND_LINE_SIZE; i++) {
		if (!isspace(p[i])) {
			if (ws) {
				ws = 0;
				*p2++ = ' ';
			}
			*p2++ = p[i];
		}
		else {
			ws = 1;
		}
	}
	*p2 = '\0';

	/* add parameters */
	for (i = 0; i < ARRAY_SIZE(cmdline_parameters); i++) {
		/*                                                                       */
		static char param[COMMAND_LINE_SIZE];
		memset(param, 0, COMMAND_LINE_SIZE);
		if (strlen(cmdline_parameters[i].value) ||
				cmdline_parameters[i].new) {
			strcat(param, cmdline_parameters[i].key);
			if (strlen(cmdline_parameters[i].value)) {
				strcat(param, "=");
				strcat(param, cmdline_parameters[i].value);
			}
		}
		strlcat(s, " ", COMMAND_LINE_SIZE);
		strlcat(s, param, COMMAND_LINE_SIZE);
	}

#ifdef NO_CONSOLE_SUSPEND
	no_console_suspend(s);
#endif
}
