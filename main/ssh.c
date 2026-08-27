/* ssh Client Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"

#include "libssh2_config.h"
#include <libssh2.h>
#include <libssh2_sftp.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>

static const char *TAG = "SSH";

extern EventGroupHandle_t xEventGroup;
extern int TASK_FINISH_BIT;

static int waitsocket(int socket_fd, LIBSSH2_SESSION *session)
{
	struct timeval timeout;
	int rc;
	fd_set fd;
	fd_set *writefd = NULL;
	fd_set *readfd = NULL;
	int dir;

	timeout.tv_sec = 10;
	timeout.tv_usec = 0;

	FD_ZERO(&fd);

	FD_SET(socket_fd, &fd);

	/* now make sure we wait in the correct direction */
	dir = libssh2_session_block_directions(session);

	if(dir & LIBSSH2_SESSION_BLOCK_INBOUND)
		readfd = &fd;

	if(dir & LIBSSH2_SESSION_BLOCK_OUTBOUND)
		writefd = &fd;

	rc = select(socket_fd + 1, readfd, writefd, NULL, &timeout);

	return rc;
}

#define BUFSIZE 3200

void ssh_task(void *pvParameters)
{
    char *task_parameter = (char *)pvParameters;
    ESP_LOGI(pcTaskGetTaskName(0), "Start task_parameter=%s", task_parameter);

	// SSH State
	int sock = -1;
	struct sockaddr_in sin;
	LIBSSH2_SESSION *session = NULL;
	LIBSSH2_CHANNEL *channel = NULL;

	ESP_LOGI(TAG, "libssh2_version is %s", LIBSSH2_VERSION);
	int rc = libssh2_init(0);
	if(rc) {
		ESP_LOGE(TAG, "libssh2 initialization failed (%d)", rc);
		goto cleanup;
	}

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if(-1 == sock) {
		ESP_LOGE(TAG, "failed to create socket!");
		goto cleanup;
	}

	// Set socket timeouts to prevent infinite blocking on offline/unreachable host
	struct timeval tv_timeout;
	tv_timeout.tv_sec = 5;
	tv_timeout.tv_usec = 0;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv_timeout, sizeof(tv_timeout));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv_timeout, sizeof(tv_timeout));

	ESP_LOGI(TAG, "CONFIG_SSH_PORT=%d", CONFIG_SSH_PORT);
	sin.sin_family = AF_INET;
	sin.sin_port = htons(CONFIG_SSH_PORT);
	sin.sin_addr.s_addr = inet_addr(CONFIG_SSH_HOST);
	if(connect(sock, (struct sockaddr*)(&sin),
			   sizeof(struct sockaddr_in)) != 0) {
		ESP_LOGE(TAG, "failed to connect to %s:%d (errno=%d)", CONFIG_SSH_HOST, CONFIG_SSH_PORT, errno);
		goto cleanup;
	}

	/* Create a session instance
	 */
	session = libssh2_session_init();
	if(!session) {
		ESP_LOGE(TAG, "failed to session init");
		goto cleanup;
	}

	// Set libssh2 session timeout to 5000 ms
	libssh2_session_set_timeout(session, 5000);

	/* ... start it up. This will trade welcome banners, exchange keys,
	 * and setup crypto, compression, and MAC layers
	 */
	rc = libssh2_session_handshake(session, sock);
	if(rc) {
		ESP_LOGE(TAG, "Failure establishing SSH session: %d", rc);
		goto cleanup;
	}

	/* We could authenticate via password */
	if(libssh2_userauth_password(session, CONFIG_SSH_USER, CONFIG_SSH_PASSWORD)) {
		ESP_LOGE(TAG, "Authentication by password failed.");
		ESP_LOGE(TAG, "Authentication username : [%s].", CONFIG_SSH_USER);
		goto cleanup;
	}

	libssh2_trace(session, LIBSSH2_TRACE_SOCKET);

	/* Exec non-blocking on the remote host */
	while((channel = libssh2_channel_open_session(session)) == NULL &&
		  libssh2_session_last_error(session, NULL, NULL, 0) ==
		  LIBSSH2_ERROR_EAGAIN) {
		if (waitsocket(sock, session) <= 0) break;
	}
	if(channel == NULL) {
		ESP_LOGE(TAG, "libssh2_channel_open_session failed.");
		goto cleanup;
	}

	while((rc = libssh2_channel_exec(channel, task_parameter)) ==
		  LIBSSH2_ERROR_EAGAIN) {
		if (waitsocket(sock, session) <= 0) break;
	}

	if(rc != 0) {
		ESP_LOGE(TAG, "libssh2_channel_exec failed: %d", rc);
		goto cleanup;
	}

	int bytecount = 0;
	for(;;) {
		/* loop until we block */
		int rc_read;
		do {
			char buffer[128];
			rc_read = libssh2_channel_read(channel, buffer, sizeof(buffer) );
			if(rc_read > 0) {
				int i;
				bytecount += rc_read;
				for(i = 0; i < rc_read; ++i)
					fputc(buffer[i], stdout);
			}
			else if(rc_read < 0 && rc_read != LIBSSH2_ERROR_EAGAIN) {
				ESP_LOGI(TAG, "libssh2_channel_read returned %d", rc_read);
				break;
			}
		}
		while(rc_read > 0);

		if(rc_read == LIBSSH2_ERROR_EAGAIN) {
			if (waitsocket(sock, session) <= 0) break;
		}
		else
			break;
	}
	printf("\n");

	int exitcode = 127;
	char *exitsignal = (char *)"none";
	while((rc = libssh2_channel_close(channel)) == LIBSSH2_ERROR_EAGAIN) {
		if (waitsocket(sock, session) <= 0) break;
	}
	if(rc == 0) {
		exitcode = libssh2_channel_get_exit_status(channel);
		libssh2_channel_get_exit_signal(channel, &exitsignal,
										NULL, NULL, NULL, NULL, NULL);
	} else {
		ESP_LOGW(TAG, "libssh2_channel_close returned: %d (remote host may have halted)", rc);
	}

	if(exitsignal)
		ESP_LOGI(TAG, "EXIT: %d, SIGNAL: %s, bytecount: %d", exitcode, exitsignal, bytecount);
	else
		ESP_LOGI(TAG, "EXIT: %d, bytecount: %d", exitcode, bytecount);

cleanup:
	if(channel) {
		libssh2_channel_free(channel);
		channel = NULL;
	}

	if(session) {
		libssh2_session_disconnect(session, "Shutdown");
		libssh2_session_free(session);
		session = NULL;
	}

	if(sock >= 0) {
		close(sock);
		sock = -1;
	}

	libssh2_exit();

	ESP_LOGI(TAG, "[%s] done", task_parameter);
	xEventGroupSetBits( xEventGroup, TASK_FINISH_BIT );
	vTaskDelete( NULL );
}
