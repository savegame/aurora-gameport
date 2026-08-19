/*
 * sensorfw_rawdump.c -- diagnostic tool: dump raw bytes of sensorfw
 * data frames without interpreting them, to verify/derive the actual
 * on-wire struct layout for a given sensorfwd build. Not part of the
 * game; keep for future protocol debugging (docs/sensorfw.md).
 *
 * Usage: sensorfw_rawdump [sensor_id] [frame_count]
 */

#include "sensorfw_client.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main( int argc, char **argv )
{
	const char *sensor_id = argc > 1 ? argv[1] : "accelerometersensor";
	int max_frames = argc > 2 ? atoi( argv[2] ) : 10;

	sensorfw_client_t *client = sensorfw_client_open( sensor_id );
	if( !client )
		return 1;

	sensorfw_client_set_interval_ms( client, 100 );
	if( !sensorfw_client_start( client ) )
	{
		sensorfw_client_close( client );
		return 1;
	}

	int fd = sensorfw_client_fd( client );
	int frames = 0;

	while( frames < max_frames )
	{
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		if( poll( &pfd, 1, 3000 ) <= 0 )
		{
			fprintf( stderr, "timeout waiting for data\n" );
			break;
		}

		uint32_t count = 0;
		size_t got = 0;
		while( got < sizeof( count ) )
		{
			ssize_t n = read( fd, (unsigned char *)&count + got, sizeof( count ) - got );
			if( n > 0 ) { got += (size_t)n; continue; }
			if( n < 0 && ( errno == EAGAIN || errno == EWOULDBLOCK ) )
			{
				struct pollfd pfd2 = { .fd = fd, .events = POLLIN };
				poll( &pfd2, 1, 1000 );
				continue;
			}
			fprintf( stderr, "short header read: %zd (errno=%d)\n", n, errno );
			goto done;
		}
		printf( "frame %d: count=%u\n", frames, count );

		unsigned char buf[4096];
		size_t want = (size_t)count * 24; /* upper bound guess, just for the raw dump */
		if( want > sizeof( buf ) )
			want = sizeof( buf );
		size_t have = 0;
		while( have < want )
		{
			ssize_t r = read( fd, buf + have, want - have );
			if( r > 0 ) { have += (size_t)r; continue; }
			if( r < 0 && ( errno == EAGAIN || errno == EWOULDBLOCK ) )
			{
				struct pollfd pfd2 = { .fd = fd, .events = POLLIN };
				poll( &pfd2, 1, 1000 );
				continue;
			}
			break;
		}

		for( size_t i = 0; i < have; ++i )
		{
			printf( "%02x ", buf[i] );
			if( ( i + 1 ) % 24 == 0 )
				printf( "\n" );
		}
		printf( "\n(%zu bytes read for count=%u)\n\n", have, count );

		++frames;
	}

done:
	sensorfw_client_close( client );
	return 0;
}
