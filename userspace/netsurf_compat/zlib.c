#include "zlib.h"

int inflateInit2(z_streamp stream, int window_bits)
{
	(void)stream;
	(void)window_bits;
	return Z_DATA_ERROR;
}

int inflate(z_streamp stream, int flush)
{
	(void)stream;
	(void)flush;
	return Z_DATA_ERROR;
}

int inflateEnd(z_streamp stream)
{
	(void)stream;
	return Z_OK;
}

gzFile gzopen(const char *path, const char *mode)
{
	return fopen(path, mode);
}

char *gzgets(gzFile file, char *buffer, int length)
{
	return fgets(buffer, length, file);
}

int gzclose(gzFile file)
{
	return fclose(file);
}
