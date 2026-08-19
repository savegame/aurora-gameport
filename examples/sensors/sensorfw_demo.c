/*
 * sensorfw_demo.c -- manual smoke test for sensorfw_client.
 *
 * Prints accelerometer or gyroscope samples to stdout for a few
 * seconds. Cross-compile for the target device and run it there to
 * confirm the protocol (see docs/sensorfw.md); not part of the game
 * build.
 *
 * Usage: sensorfw_demo [accelerometersensor|gyroscopesensor] [seconds]
 */

#define _POSIX_C_SOURCE 200809L /* clock_gettime() */

#include "sensorfw_client.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void print_sample( void *user_data, uint64_t ts_us, float x, float y, float z )
{
	(void)user_data;
	printf( "t=%12llu  x=%8.4f  y=%8.4f  z=%8.4f\n",
	        (unsigned long long)ts_us, (double)x, (double)y, (double)z );
}

static uint64_t now_ms( void )
{
	struct timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

int main( int argc, char **argv )
{
	const char *type_name = argc > 1 ? argv[1] : sensorfw_sensor_type_name( SENSORFW_SENSOR_ACCELEROMETER );
	int seconds = argc > 2 ? atoi( argv[2] ) : 5;

	sensorfw_sensor_type_t type;
	if( !sensorfw_sensor_type_from_name( type_name, &type ) )
	{
		fprintf( stderr, "'%s' is not a known sensor type (accelerometersensor, gyroscopesensor)\n", type_name );
		return 1;
	}

	sensorfw_sensor_type_t available[SENSORFW_SENSOR_TYPE_COUNT];
	int n = sensorfw_client_list_available( available, SENSORFW_SENSOR_TYPE_COUNT );
	bool present = false;
	for( int i = 0; i < n; ++i )
		if( available[i] == type )
			present = true;
	if( !present )
	{
		fprintf( stderr, "'%s' is not among the sensors sensorfwd reports for this device\n", type_name );
		return 1;
	}

	sensorfw_client_t *client = sensorfw_client_open( type );
	if( !client )
	{
		fprintf( stderr, "failed to open '%s'\n", type_name );
		return 1;
	}

	sensorfw_client_set_interval_ms( client, 20 ); /* ~50 Hz */
	if( !sensorfw_client_start( client ) )
	{
		sensorfw_client_close( client );
		return 1;
	}

	printf( "reading '%s' for %d s (fd=%d) ...\n", type_name, seconds, sensorfw_client_fd( client ) );

	uint64_t deadline = now_ms() + (uint64_t)seconds * 1000;
	while( now_ms() < deadline )
	{
		struct pollfd pfd = { .fd = sensorfw_client_fd( client ), .events = POLLIN };
		poll( &pfd, 1, 200 );

		if( !sensorfw_client_pump( client, print_sample, NULL ) )
		{
			fprintf( stderr, "session lost\n" );
			break;
		}
	}

	sensorfw_client_close( client );
	return 0;
}
