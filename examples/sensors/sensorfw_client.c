/*
 * sensorfw_client.c -- see sensorfw_client.h for the "why" and usage.
 *
 * Wire protocol summary (full derivation in docs/sensorfw.md):
 *
 *   1. D-Bus (system bus), service com.nokia.SensorService:
 *        /SensorManager  local.SensorManager
 *          loadPlugin(s: id) -> b                       [MUST be called
 *            first -- requestSensor() on an unloaded id fails with
 *            "requested sensor id '...' not registered", verified
 *            on-device]
 *          requestSensor(s: id, x: pid) -> i sessionId
 *          releaseSensor(s: id, i: sessionId, x: pid) -> b
 *        /SensorManager/<id>  local.<Type>Sensor  (created only after
 *            loadPlugin; confirmed via introspection for
 *            local.AccelerometerSensor / local.GyroscopeSensor)
 *          start(i: sessionId)
 *          stop(i: sessionId)
 *          setInterval(i: sessionId, i: value_ms)
 *
 *   2. Data socket: AF_UNIX SOCK_STREAM to /run/sensord.sock (a
 *      plain filesystem-path unix socket, world-writable -- verified
 *      `ls -la /run/sensord.sock` on-device). Handshake:
 *        client -> connect()
 *        server -> writes 1 tag byte ('\n') right after accept()
 *        client -> writes sizeof(int32) raw bytes: the sessionId
 *        client -> reads 1 byte (consumes the tag)
 *      From then on the socket carries sample frames, each:
 *        [uint32 count][count * 24 raw bytes]
 *      The 24-byte element is documented upstream as TimedXyzData
 *      (datatypes/genericdata.h) = { uint64 timestamp_us; float x, y,
 *      z; }, but that does NOT match what actually arrives on the
 *      wire: a raw byte dump (sensorfw_rawdump.c) shows
 *      { uint64 timestamp_us; int32 x, y, z; int32 reserved; } --
 *      four int32 fields, not three floats + padding. See
 *      sensorfw_wire_xyz_t below and docs/sensorfw.md for the full
 *      derivation, including the raw bytes captured.
 *
 * Build:  pkg-config --cflags --libs dbus-1
 */

#define _POSIX_C_SOURCE 200809L /* strdup() */

#include "sensorfw_client.h"

#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define SENSORFW_SERVICE       "com.nokia.SensorService"
#define SENSORFW_OBJECT_PATH   "/SensorManager"
#define SENSORFW_MANAGER_IFACE "local.SensorManager"
#define SENSORFW_SOCKET_PATH   "/run/sensord.sock"
#define SENSORFW_CALL_TIMEOUT_MS 2000

/* Samples per frame sensord will realistically ever send in one
 * write() with the default bufferSize=1 this client uses; guards
 * against a corrupt/desynced stream making us allocate wildly (the
 * original Qt SocketReader uses the same kind of sanity cap, at
 * 1000). */
#define SENSORFW_MAX_BATCH 64

typedef struct
{
	uint64_t timestamp_us;
	int32_t  x;
	int32_t  y;
	int32_t  z;
	int32_t  reserved; /* meaning unknown -- see comment below */
} sensorfw_wire_xyz_t;

/* This layout was derived empirically with sensorfw_rawdump.c and
 * does NOT match the upstream sailfishos/sensorfw sources
 * (github.com/sailfishos/sensorfw, datatypes/genericdata.h), where
 * TimedXyzData is { quint64 timestamp_; float x_, y_, z_; } --
 * 8+4+4+4=20 bytes padded to 24 (trailing padding, only 3 fields).
 * What actually arrives on the wire instead is FOUR raw int32 fields
 * after the timestamp (still 24 bytes total, no padding -- every
 * byte is real data):
 *   accelerometersensor: x,y small, z landing near 1000 with the
 *                         device flat, 4th field a session-constant.
 *   gyroscopesensor:      x,y,z small noise at rest, 4th field a
 *                         (different) session-constant.
 * z landing near 1000 while flat strongly suggests raw milli-g units
 * for the accelerometer (not SI m/s^2, not floating point g). The
 * 4th field's meaning is unconfirmed (constant per session/sensor
 * type, differs between accelerometer and gyroscope -- looks like a
 * per-type status/range/accuracy word, not per-sample data) and is
 * ignored by this client. This matches the D-Bus "xyz" property's
 * "(tiii)" signature (timestamp + 3 int32) seen in introspection,
 * which also contradicts the upstream source's "float->double"
 * marshalling comment -- i.e. the underlying type differs from
 * upstream, not just the D-Bus wire encoding. Re-verify with
 * sensorfw_rawdump.c if this ever needs to run against a different
 * sensorfw build. */
_Static_assert( sizeof( sensorfw_wire_xyz_t ) == 24, "TimedXyzData wire layout mismatch" );

typedef struct
{
	const char *id;
	const char *iface;
	bool        verified_on_device;
} sensorfw_iface_entry_t;

static const sensorfw_iface_entry_t SENSORFW_IFACES[] =
{
	/* Verified via `dbus-send --system ... Introspect` on-device. */
	{ "accelerometersensor", "local.AccelerometerSensor", true },
	{ "gyroscopesensor",     "local.GyroscopeSensor",     true },

	/* NOT verified on-device -- inferred only from sensorfw's
	 * qt-api naming convention (magnetometersensor_i.h /
	 * local.MagnetometerSensor etc). Re-check with dbus-send
	 * introspection on /SensorManager/<id> before relying on
	 * these; sensorfw_client_open() will still work as long as
	 * the guess is right, since it uses the exact same D-Bus
	 * calls, but a wrong interface name fails loudly (method
	 * call errors), not silently. */
	{ "magnetometersensor",  "local.MagnetometerSensor",  false },
	{ "orientationsensor",   "local.OrientationSensor",   false },
	{ "rotationsensor",      "local.RotationSensor",      false },
};

struct sensorfw_client
{
	DBusConnection *bus;
	char           *sensor_id;
	const char     *iface;
	char            object_path[128];
	int             fd;
	int32_t         session_id;
	int64_t         pid;
	bool            started;

	/* pump() frame-reassembly state */
	uint8_t  hdr_buf[sizeof( uint32_t )];
	size_t   hdr_have;
	uint32_t pending_count;
	size_t   payload_cap;   /* 0 == currently expecting a header */
	size_t   payload_have;
	uint8_t  payload_buf[SENSORFW_MAX_BATCH * sizeof( sensorfw_wire_xyz_t )];
};

/* ------------------------------------------------------------------------- */

static const char *sensorfw_iface_for_id( const char *id )
{
	for( size_t i = 0; i < sizeof( SENSORFW_IFACES ) / sizeof( SENSORFW_IFACES[0] ); ++i )
	{
		if( strcmp( SENSORFW_IFACES[i].id, id ) == 0 )
		{
			if( !SENSORFW_IFACES[i].verified_on_device )
				fprintf( stderr, "sensorfw_client: interface for '%s' is not verified "
				         "on-device -- double-check with dbus-send introspection\n", id );
			return SENSORFW_IFACES[i].iface;
		}
	}
	return NULL;
}

static DBusMessage *sensorfw_call_sync( DBusConnection *bus, const char *path, const char *iface,
                                         const char *method, DBusError *err, int first_arg_type, ... )
{
	DBusMessage *msg = dbus_message_new_method_call( SENSORFW_SERVICE, path, iface, method );
	if( !msg )
		return NULL;

	if( first_arg_type != DBUS_TYPE_INVALID )
	{
		va_list ap;
		va_start( ap, first_arg_type );
		dbus_bool_t ok = dbus_message_append_args_valist( msg, first_arg_type, ap );
		va_end( ap );
		if( !ok )
		{
			dbus_message_unref( msg );
			return NULL;
		}
	}

	DBusMessage *reply = dbus_connection_send_with_reply_and_block( bus, msg, SENSORFW_CALL_TIMEOUT_MS, err );
	dbus_message_unref( msg );
	return reply;
}

/* ------------------------------------------------------------------------- */

sensorfw_client_t *sensorfw_client_open( const char *sensor_id )
{
	if( !sensor_id )
		return NULL;

	const char *iface = sensorfw_iface_for_id( sensor_id );
	if( !iface )
	{
		fprintf( stderr, "sensorfw_client: unknown sensor id '%s' "
		         "(add it to SENSORFW_IFACES in sensorfw_client.c)\n", sensor_id );
		return NULL;
	}

	DBusError err;
	dbus_error_init( &err );

	DBusConnection *bus = dbus_bus_get( DBUS_BUS_SYSTEM, &err );
	if( !bus )
	{
		fprintf( stderr, "sensorfw_client: system bus unreachable: %s\n", err.message );
		dbus_error_free( &err );
		return NULL;
	}
	dbus_connection_set_exit_on_disconnect( bus, FALSE );

	/* sensord only exposes /SensorManager/<id> after the plugin has
	 * been loaded at least once -- requestSensor() alone fails with
	 * "requested sensor id '<id>' not registered" (verified
	 * on-device). */
	DBusMessage *reply = sensorfw_call_sync( bus, SENSORFW_OBJECT_PATH, SENSORFW_MANAGER_IFACE,
	                                          "loadPlugin", &err,
	                                          DBUS_TYPE_STRING, &sensor_id, DBUS_TYPE_INVALID );
	if( !reply )
	{
		fprintf( stderr, "sensorfw_client: loadPlugin('%s') failed: %s\n", sensor_id, err.message );
		dbus_error_free( &err );
		dbus_connection_unref( bus );
		return NULL;
	}
	dbus_message_unref( reply );

	dbus_int64_t pid = (dbus_int64_t)getpid();
	reply = sensorfw_call_sync( bus, SENSORFW_OBJECT_PATH, SENSORFW_MANAGER_IFACE,
	                             "requestSensor", &err,
	                             DBUS_TYPE_STRING, &sensor_id, DBUS_TYPE_INT64, &pid, DBUS_TYPE_INVALID );
	if( !reply )
	{
		fprintf( stderr, "sensorfw_client: requestSensor('%s') failed: %s\n", sensor_id, err.message );
		dbus_error_free( &err );
		dbus_connection_unref( bus );
		return NULL;
	}

	dbus_int32_t session_id = -1;
	if( !dbus_message_get_args( reply, &err, DBUS_TYPE_INT32, &session_id, DBUS_TYPE_INVALID )
	    || session_id < 0 )
	{
		fprintf( stderr, "sensorfw_client: requestSensor('%s') returned no session (id=%d)%s%s\n",
		         sensor_id, (int)session_id, err.message ? ": " : "", err.message ? err.message : "" );
		dbus_error_free( &err );
		dbus_message_unref( reply );
		dbus_connection_unref( bus );
		return NULL;
	}
	dbus_message_unref( reply );

	/* -- data socket handshake -- */
	int fd = socket( AF_UNIX, SOCK_STREAM, 0 );
	if( fd < 0 )
	{
		perror( "sensorfw_client: socket" );
		goto fail_release;
	}

	struct sockaddr_un addr;
	memset( &addr, 0, sizeof( addr ) );
	addr.sun_family = AF_UNIX;
	snprintf( addr.sun_path, sizeof( addr.sun_path ), "%s", SENSORFW_SOCKET_PATH );

	if( connect( fd, (struct sockaddr *)&addr, sizeof( addr ) ) != 0 )
	{
		perror( "sensorfw_client: connect(" SENSORFW_SOCKET_PATH ")" );
		close( fd );
		goto fail_release;
	}

	int32_t wire_session_id = session_id;
	if( write( fd, &wire_session_id, sizeof( wire_session_id ) ) != (ssize_t)sizeof( wire_session_id ) )
	{
		perror( "sensorfw_client: write(session id)" );
		close( fd );
		goto fail_release;
	}

	char tag;
	if( read( fd, &tag, 1 ) != 1 )
	{
		perror( "sensorfw_client: read(handshake tag)" );
		close( fd );
		goto fail_release;
	}

	int flags = fcntl( fd, F_GETFL, 0 );
	fcntl( fd, F_SETFL, flags | O_NONBLOCK );

	sensorfw_client_t *client = calloc( 1, sizeof( *client ) );
	client->bus = bus;
	client->sensor_id = strdup( sensor_id );
	client->iface = iface;
	snprintf( client->object_path, sizeof( client->object_path ), "%s/%s", SENSORFW_OBJECT_PATH, sensor_id );
	client->fd = fd;
	client->session_id = session_id;
	client->pid = pid;
	return client;

fail_release:
	{
		DBusError err2;
		dbus_error_init( &err2 );
		DBusMessage *r = sensorfw_call_sync( bus, SENSORFW_OBJECT_PATH, SENSORFW_MANAGER_IFACE,
		                                      "releaseSensor", &err2,
		                                      DBUS_TYPE_STRING, &sensor_id,
		                                      DBUS_TYPE_INT32, &session_id,
		                                      DBUS_TYPE_INT64, &pid, DBUS_TYPE_INVALID );
		if( r )
			dbus_message_unref( r );
		dbus_error_free( &err2 );
	}
	dbus_connection_unref( bus );
	return NULL;
}

void sensorfw_client_close( sensorfw_client_t *client )
{
	if( !client )
		return;

	if( client->started )
		sensorfw_client_stop( client );

	DBusError err;
	dbus_error_init( &err );
	DBusMessage *reply = sensorfw_call_sync( client->bus, SENSORFW_OBJECT_PATH, SENSORFW_MANAGER_IFACE,
	                                          "releaseSensor", &err,
	                                          DBUS_TYPE_STRING, &client->sensor_id,
	                                          DBUS_TYPE_INT32, &client->session_id,
	                                          DBUS_TYPE_INT64, &client->pid, DBUS_TYPE_INVALID );
	if( reply )
		dbus_message_unref( reply );
	else
		fprintf( stderr, "sensorfw_client: releaseSensor('%s') failed: %s\n", client->sensor_id, err.message );
	dbus_error_free( &err );

	close( client->fd );
	dbus_connection_unref( client->bus );
	free( client->sensor_id );
	free( client );
}

bool sensorfw_client_set_interval_ms( sensorfw_client_t *client, int interval_ms )
{
	if( !client )
		return false;

	DBusError err;
	dbus_error_init( &err );
	dbus_int32_t session_id = client->session_id;
	dbus_int32_t value = interval_ms;
	DBusMessage *reply = sensorfw_call_sync( client->bus, client->object_path, client->iface,
	                                          "setInterval", &err,
	                                          DBUS_TYPE_INT32, &session_id,
	                                          DBUS_TYPE_INT32, &value, DBUS_TYPE_INVALID );
	if( !reply )
	{
		fprintf( stderr, "sensorfw_client: setInterval(%d) failed: %s\n", interval_ms, err.message );
		dbus_error_free( &err );
		return false;
	}
	dbus_message_unref( reply );
	return true;
}

bool sensorfw_client_start( sensorfw_client_t *client )
{
	if( !client )
		return false;

	DBusError err;
	dbus_error_init( &err );
	dbus_int32_t session_id = client->session_id;
	DBusMessage *reply = sensorfw_call_sync( client->bus, client->object_path, client->iface,
	                                          "start", &err,
	                                          DBUS_TYPE_INT32, &session_id, DBUS_TYPE_INVALID );
	if( !reply )
	{
		fprintf( stderr, "sensorfw_client: start() failed: %s\n", err.message );
		dbus_error_free( &err );
		return false;
	}
	dbus_message_unref( reply );
	client->started = true;
	return true;
}

bool sensorfw_client_stop( sensorfw_client_t *client )
{
	if( !client )
		return false;

	DBusError err;
	dbus_error_init( &err );
	dbus_int32_t session_id = client->session_id;
	DBusMessage *reply = sensorfw_call_sync( client->bus, client->object_path, client->iface,
	                                          "stop", &err,
	                                          DBUS_TYPE_INT32, &session_id, DBUS_TYPE_INVALID );
	if( !reply )
	{
		fprintf( stderr, "sensorfw_client: stop() failed: %s\n", err.message );
		dbus_error_free( &err );
		return false;
	}
	dbus_message_unref( reply );
	client->started = false;
	return true;
}

int sensorfw_client_fd( const sensorfw_client_t *client )
{
	return client ? client->fd : -1;
}

bool sensorfw_client_pump( sensorfw_client_t *client, sensorfw_sample_cb cb, void *user_data )
{
	if( !client )
		return false;

	for( ;; )
	{
		if( client->payload_cap == 0 )
		{
			ssize_t n = recv( client->fd, client->hdr_buf + client->hdr_have,
			                   sizeof( client->hdr_buf ) - client->hdr_have, 0 );
			if( n == 0 )
				return false; /* sensord closed the session */
			if( n < 0 )
			{
				if( errno == EAGAIN || errno == EWOULDBLOCK )
					return true;
				perror( "sensorfw_client: recv(header)" );
				return false;
			}
			client->hdr_have += (size_t)n;
			if( client->hdr_have < sizeof( client->hdr_buf ) )
				continue;

			uint32_t count;
			memcpy( &count, client->hdr_buf, sizeof( count ) );
			client->hdr_have = 0;

			if( count == 0 || count > SENSORFW_MAX_BATCH )
			{
				fprintf( stderr, "sensorfw_client: implausible batch count %u -- protocol desync\n",
				         (unsigned)count );
				return false;
			}

			client->pending_count = count;
			client->payload_cap = (size_t)count * sizeof( sensorfw_wire_xyz_t );
			client->payload_have = 0;
			continue;
		}

		ssize_t n = recv( client->fd, client->payload_buf + client->payload_have,
		                   client->payload_cap - client->payload_have, 0 );
		if( n == 0 )
			return false;
		if( n < 0 )
		{
			if( errno == EAGAIN || errno == EWOULDBLOCK )
				return true;
			perror( "sensorfw_client: recv(payload)" );
			return false;
		}
		client->payload_have += (size_t)n;
		if( client->payload_have < client->payload_cap )
			continue;

		if( cb )
		{
			for( uint32_t i = 0; i < client->pending_count; ++i )
			{
				sensorfw_wire_xyz_t s;
				memcpy( &s, client->payload_buf + (size_t)i * sizeof( s ), sizeof( s ) );
				cb( user_data, s.timestamp_us, s.x, s.y, s.z );
			}
		}
		client->payload_cap = 0;
		client->payload_have = 0;
		client->pending_count = 0;
	}
}
