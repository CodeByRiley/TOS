#ifndef TOS_NETSURF_ZLIB_H
#define TOS_NETSURF_ZLIB_H

#include <stdio.h>

typedef unsigned char Bytef;
typedef unsigned int uInt;
typedef void *voidpf;

typedef struct z_stream_s {
	Bytef *next_in;
	uInt avail_in;
	Bytef *next_out;
	uInt avail_out;
	voidpf zalloc;
	voidpf zfree;
	voidpf opaque;
} z_stream;

typedef z_stream *z_streamp;
typedef FILE *gzFile;

#define Z_NULL NULL
#define Z_NO_FLUSH 0
#define Z_OK 0
#define Z_STREAM_END 1
#define Z_DATA_ERROR (-3)
#define MAX_WBITS 15

int inflateInit2(z_streamp stream, int window_bits);
int inflate(z_streamp stream, int flush);
int inflateEnd(z_streamp stream);
gzFile gzopen(const char *path, const char *mode);
char *gzgets(gzFile file, char *buffer, int length);
int gzclose(gzFile file);

#endif
