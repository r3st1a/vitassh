/*
 * The one file-system move that has to be identical everywhere: putting a fully
 * written temporary file in place of the real one.
 */
#ifndef VITASSH_FILEIO_H
#define VITASSH_FILEIO_H

/*
 * Renames tmp onto path, replacing whatever is there. Returns 0 on success and
 * -1 if the move could not be completed; on failure both files are left on disk
 * (the caller decides whether the temporary is worth removing).
 */
int file_replace(const char *tmp, const char *path);

#endif /* VITASSH_FILEIO_H */
