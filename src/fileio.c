#include "fileio.h"

#include <stdio.h>

int file_replace(const char *tmp, const char *path)
{
	FILE *probe;

	if (tmp == NULL || path == NULL) return -1;

	if (rename(tmp, path) == 0) return 0;

	/*
	 * MSVCRT refuses to rename onto an existing file, and the host test build
	 * runs against it; on the device the first rename normally succeeds. Only
	 * take the destination out of the way once the replacement is known to be
	 * there, so a failed rename cannot destroy a good file for nothing.
	 */
	probe = fopen(tmp, "rb");
	if (probe == NULL) return -1;
	fclose(probe);

	remove(path);

	/*
	 * If this still fails both files are on disk. Leave the temporary alone:
	 * it holds the only complete copy of the data at this point.
	 */
	return rename(tmp, path) == 0 ? 0 : -1;
}
