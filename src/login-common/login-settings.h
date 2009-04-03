#ifndef __LOGIN_SETTINGS_H
#define __LOGIN_SETTINGS_H

struct login_settings {
	const char *login_dir;
	bool login_chroot;
	const char *login_trusted_networks;
	const char *login_greeting;
	const char *login_log_format_elements, *login_log_format;

	bool login_process_per_connection;
	const char *capability_string;

	const char *ssl;
	const char *ssl_ca_file;
	const char *ssl_cert_file;
	const char *ssl_key_file;
	const char *ssl_key_password;
	const char *ssl_parameters_file;
	const char *ssl_cipher_list;
	const char *ssl_cert_username_field;
	bool ssl_verify_client_cert;
	bool ssl_require_client_cert;
	bool ssl_username_from_cert;
	bool verbose_ssl;

	bool disable_plaintext_auth;
	bool verbose_auth;
	bool auth_debug;
	bool verbose_proctitle;

	unsigned int login_max_connections;

	/* generated: */
	const char *const *log_format_elements_split;
};

struct login_settings *login_settings_read(void);

#endif
