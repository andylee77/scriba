/* Driver: print flash_layout_json for each file, then try every buffer size
 * up to the full length to check the -1 path never overflows. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "flash_layout.h"

int main(int argc, char **argv)
{
	static struct fl_layout l;
	static char out[16384];

	for (int i = 1; i < argc; i++) {
		FILE *f = fopen(argv[i], "rb");
		if (!f) { perror(argv[i]); continue; }
		fseek(f, 0, SEEK_END);
		long len = ftell(f);
		fseek(f, 0, SEEK_SET);
		unsigned char *buf = malloc(len ? len : 1);
		if (fread(buf, 1, len, f) != (size_t)len) { perror("read"); return 1; }
		fclose(f);
		flash_layout_analyze(buf, len, 0, &l);
		int r = flash_layout_json(&l, out, sizeof(out));
		printf("%s\n", out);
		if (r < 0 || (size_t)r != strlen(out)) { fprintf(stderr, "bad length %d\n", r); return 1; }
		for (int n = 1; n <= r + 1; n++) {
			char *small = malloc(n);
			int k = flash_layout_json(&l, small, n);
			if (n <= r ? (k != -1 || small[0]) : (k != r || strcmp(small, out))) {
				fprintf(stderr, "%s: n=%d gave %d\n", argv[i], n, k);
				return 1;
			}
			free(small);
		}
		free(buf);
	}
	return 0;
}
