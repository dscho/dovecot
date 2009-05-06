#ifndef MASTER_SERVICE_H
#define MASTER_SERVICE_H

#include "network.h"

enum master_service_flags {
	/* stdin/stdout already contains a client which we want to serve */
	MASTER_SERVICE_FLAG_STD_CLIENT		= 0x01,
	/* this process is currently running standalone without a master */
	MASTER_SERVICE_FLAG_STANDALONE		= 0x02,
	/* Log to stderr instead of the configured log file */
	MASTER_SERVICE_FLAG_LOG_TO_STDERR	= 0x04
};

struct master_service_connection {
	int fd;
	int listen_fd;

	struct ip_addr remote_ip;
	unsigned int remote_port;

	bool ssl;
};

typedef void
master_service_connection_callback_t(const struct master_service_connection *conn);

struct master_service;

const char *master_service_getopt_string(void);

/* Start service initialization. */
struct master_service *
master_service_init(const char *name, enum master_service_flags flags,
		    int argc, char *argv[]);
/* Parser command line option. Returns TRUE if processed. */
bool master_service_parse_option(struct master_service *service,
				 int opt, const char *arg);
/* Finish service initialization. The caller should drop privileges
   before calling this. */
void master_service_init_finish(struct master_service *service);

/* Clean environment from everything except TZ, USER and optionally HOME. */
void master_service_env_clean(bool preserve_home);

/* Initialize logging. */
void master_service_init_log(struct master_service *service, const char *prefix,
			     unsigned int max_lines_per_sec);

/* Set maximum number of clients we can handle. Default is given by master. */
void master_service_set_client_limit(struct master_service *service,
				     unsigned int client_limit);
/* Returns the maximum number of clients we can handle. */
unsigned int master_service_get_client_limit(struct master_service *service);

/* Set maximum number of client connections we will handle before shutting
   down. */
void master_service_set_service_count(struct master_service *service,
				      unsigned int count);
/* Returns the number of client connections we will handle before shutting
   down. The value is decreased only after connection has been closed. */
unsigned int master_service_get_service_count(struct master_service *service);
/* Return the number of listener sockets. */
unsigned int master_service_get_socket_count(struct master_service *service);

/* Returns configuration file path. */
const char *master_service_get_config_path(struct master_service *service);
/* Returns PACKAGE_VERSION or NULL if version_ignore=yes. This function is
   useful mostly as parameter to module_dir_load(). */
const char *master_service_get_version_string(struct master_service *service);

/* Start the service. Blocks until finished */
void master_service_run(struct master_service *service,
			master_service_connection_callback_t *callback);
/* Stop a running service. */
void master_service_stop(struct master_service *service);

/* Send command to anvil process, if we have fd to it. */
void master_service_anvil_send(struct master_service *service, const char *cmd);
/* Call whenever a client connection is destroyed. */
void master_service_client_connection_destroyed(struct master_service *service);

/* Deinitialize the service. */
void master_service_deinit(struct master_service **service);

#endif
