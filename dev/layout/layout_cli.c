/* Standalone driver for flash_layout.c: print the layout of each file. */
#include <stdio.h>
#include <stdlib.h>
#include "flash_layout.h"

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		FILE *f = fopen(argv[i], "rb");
		if (!f) { perror(argv[i]); continue; }
		fseek(f, 0, SEEK_END);
		long len = ftell(f);
		fseek(f, 0, SEEK_SET);
		unsigned char *buf = malloc(len ? len : 1);
		if (fread(buf, 1, len, f) != (size_t)len) { perror("read"); return 1; }
		fclose(f);
		printf("=== %s (%ld bytes)\n", argv[i], len);
		flash_layout_report(buf, len, 0);
		if (len > 0x800000) {           /* same image on a chip of known size */
			printf("--- as read from a 16 MB chip:\n");
			flash_layout_report(buf, len, 16UL * 1024 * 1024);
			printf("--- loaded for an 8 MB chip:\n");
			flash_layout_report(buf, len, 8UL * 1024 * 1024);
		}
		free(buf);
	}
	return 0;
}
