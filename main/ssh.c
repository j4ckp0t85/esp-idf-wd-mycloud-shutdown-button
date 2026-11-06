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
#include "esp_timer.h"
#include "ssh.h"

#include "libssh2_config.h"
#include <libssh2.h>
#include <libssh2_sftp.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>

static const char *TAG = "SSH";

#define SSH_STAGE_TIMEOUT_MS 30000
#define SSH_CONNECT_TIMEOUT_MS 10000
#define SSH_OUTPUT_TIMEOUT_MS 5000

static int waitsocket(int socket_fd, LIBSSH2_SESSION *session, int64_t deadline)
{
	int64_t remaining = deadline - esp_timer_get_time();
	if (remaining <= 0) return LIBSSH2_ERROR_TIMEOUT;
	struct timeval timeout = {
		.tv_sec = remaining / 1000000,
		.tv_usec = remaining % 1000000
	};
	fd_set readfds;
	fd_set writefds;
	FD_ZERO(&readfds);
	FD_ZERO(&writefds);
	int directions = session ? libssh2_session_block_directions(session) : LIBSSH2_SESSION_BLOCK_OUTBOUND;
	if (directions & LIBSSH2_SESSION_BLOCK_OUTBOUND) FD_SET(socket_fd, &writefds);
	if ((directions & LIBSSH2_SESSION_BLOCK_INBOUND) || directions == 0) FD_SET(socket_fd, &readfds);
	int ready = select(socket_fd + 1, &readfds, &writefds, NULL, &timeout);
	if (ready > 0 || (ready < 0 && errno == EINTR)) return 0;
	return ready == 0 ? LIBSSH2_ERROR_TIMEOUT : LIBSSH2_ERROR_SOCKET_RECV;
}

static int64_t start_stage(const char *stage, int attempt)
{
	ESP_LOGI(TAG, "Attempt %d/2: %s started", attempt, stage);
	return esp_timer_get_time();
}

static void finish_stage(const char *stage, int64_t started, int rc, LIBSSH2_SESSION *session)
{
	long long elapsed = (esp_timer_get_time() - started) / 1000;
	if (rc == 0) {
		ESP_LOGI(TAG, "%s completed in %lld ms", stage, elapsed);
	} else {
		char *message = NULL;
		if (session) libssh2_session_last_error(session, &message, NULL, 0);
		ESP_LOGE(TAG, "%s failed after %lld ms: rc=%d, detail=%s", stage, elapsed, rc,
			message ? message : "unavailable");
	}
}

static EventBits_t ssh_attempt(const char *command, int attempt, int *retryable)
{
	int sock = -1;
	int rc = 0;
	LIBSSH2_SESSION *session = NULL;
	LIBSSH2_CHANNEL *channel = NULL;
	EventBits_t result = SSH_FAILED_BIT;
	*retryable = 1;
	int64_t started = start_stage("TCP connection", attempt);
	int64_t deadline = started + (int64_t)SSH_CONNECT_TIMEOUT_MS * 1000;
	struct sockaddr_in address = {0};
	address.sin_family = AF_INET;
	address.sin_port = htons(CONFIG_SSH_PORT);
	address.sin_addr.s_addr = inet_addr(CONFIG_SSH_HOST);
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		ESP_LOGE(TAG, "Socket creation failed: errno=%d", errno);
		goto cleanup;
	}
	int flags = fcntl(sock, F_GETFL, 0);
	if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
		ESP_LOGE(TAG, "Non-blocking socket setup failed: errno=%d", errno);
		goto cleanup;
	}
	rc = connect(sock, (struct sockaddr *)&address, sizeof(address));
	if (rc < 0 && errno == EINPROGRESS) {
		rc = waitsocket(sock, NULL, deadline);
		if (rc == 0) {
			int socket_error = 0;
			socklen_t length = sizeof(socket_error);
			if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &socket_error, &length) < 0 || socket_error != 0) {
				ESP_LOGE(TAG, "TCP connection failed: socket_error=%d, errno=%d", socket_error, errno);
				rc = LIBSSH2_ERROR_SOCKET_RECV;
			}
		}
	}
	finish_stage("TCP connection", started, rc, NULL);
	if (rc != 0) goto cleanup;

	session = libssh2_session_init();
	if (!session) {
		ESP_LOGE(TAG, "Session allocation failed");
		goto cleanup;
	}
	libssh2_session_set_blocking(session, 0);
	libssh2_session_set_timeout(session, SSH_STAGE_TIMEOUT_MS);

	started = start_stage("SSH handshake", attempt);
	deadline = started + (int64_t)SSH_STAGE_TIMEOUT_MS * 1000;
	while ((rc = libssh2_session_handshake(session, sock)) == LIBSSH2_ERROR_EAGAIN) {
		rc = waitsocket(sock, session, deadline);
		if (rc != 0) break;
	}
	finish_stage("SSH handshake", started, rc, session);
	if (rc != 0) goto cleanup;

	started = start_stage("Authentication", attempt);
	deadline = started + (int64_t)SSH_STAGE_TIMEOUT_MS * 1000;
	while ((rc = libssh2_userauth_password(session, CONFIG_SSH_USER, CONFIG_SSH_PASSWORD)) == LIBSSH2_ERROR_EAGAIN) {
		rc = waitsocket(sock, session, deadline);
		if (rc != 0) break;
	}
	finish_stage("Authentication", started, rc, session);
	if (rc != 0) {
		if (rc == LIBSSH2_ERROR_AUTHENTICATION_FAILED) *retryable = 0;
		goto cleanup;
	}

	started = start_stage("Channel opening", attempt);
	deadline = started + (int64_t)SSH_STAGE_TIMEOUT_MS * 1000;
	while ((channel = libssh2_channel_open_session(session)) == NULL) {
		rc = libssh2_session_last_error(session, NULL, NULL, 0);
		if (rc != LIBSSH2_ERROR_EAGAIN) break;
		rc = waitsocket(sock, session, deadline);
		if (rc != 0) break;
	}
	if (channel) rc = 0;
	finish_stage("Channel opening", started, rc, session);
	if (!channel) goto cleanup;

	*retryable = 0;
	result = SSH_UNKNOWN_BIT;
	started = start_stage("Command acknowledgement", attempt);
	deadline = started + (int64_t)SSH_STAGE_TIMEOUT_MS * 1000;
	while ((rc = libssh2_channel_exec(channel, command)) == LIBSSH2_ERROR_EAGAIN) {
		rc = waitsocket(sock, session, deadline);
		if (rc != 0) break;
	}
	finish_stage("Command acknowledgement", started, rc, session);
	if (rc != 0) {
		if (rc == LIBSSH2_ERROR_CHANNEL_REQUEST_DENIED) result = SSH_FAILED_BIT;
		ESP_LOGW(TAG, "Command will NOT be resent; inspect NAS power state before another press");
		goto cleanup;
	}
	result = SSH_ACCEPTED_BIT;
	ESP_LOGI(TAG, "NAS acknowledged the command; physical shutdown is not yet verified");

	deadline = esp_timer_get_time() + (int64_t)SSH_OUTPUT_TIMEOUT_MS * 1000;
	while (esp_timer_get_time() < deadline) {
		int received = 0;
		for (int stream = 0; stream < 2; ++stream) {
			char buffer[128];
			int bytes = libssh2_channel_read_ex(channel, stream, buffer, sizeof(buffer));
			if (bytes > 0) {
				fwrite(buffer, 1, bytes, stream == 0 ? stdout : stderr);
				received = 1;
			} else if (bytes < 0 && bytes != LIBSSH2_ERROR_EAGAIN) {
				ESP_LOGW(TAG, "Output stream %d ended: rc=%d (NAS may be shutting down)", stream, bytes);
				goto cleanup;
			}
		}
		if (libssh2_channel_eof(channel)) break;
		if (!received && waitsocket(sock, session, deadline) != 0) break;
	}
	if (libssh2_channel_eof(channel)) {
		while ((rc = libssh2_channel_wait_closed(channel)) == LIBSSH2_ERROR_EAGAIN) {
			rc = waitsocket(sock, session, deadline);
			if (rc != 0) break;
		}
		if (rc == 0) {
			int exitcode = libssh2_channel_get_exit_status(channel);
			char *signal = NULL;
			libssh2_channel_get_exit_signal(channel, &signal, NULL, NULL, NULL, NULL, NULL);
			ESP_LOGI(TAG, "Remote shell exit=%d, signal=%s", exitcode, signal ? signal : "none");
			if (exitcode != 0 || signal) result = SSH_FAILED_BIT;
			if (signal) libssh2_free(session, signal);
		} else {
			ESP_LOGW(TAG, "No final shell status: rc=%d; command was acknowledged", rc);
		}
	} else {
		ESP_LOGW(TAG, "No final shell status within %d ms; command was acknowledged", SSH_OUTPUT_TIMEOUT_MS);
	}

cleanup:
	if (sock >= 0) close(sock);
	if (session) {
		int free_rc = libssh2_session_free(session);
		if (free_rc != 0) ESP_LOGW(TAG, "Session cleanup returned %d", free_rc);
	}
	return result;
}

void ssh_task(void *pvParameters)
{
	const char *command = (const char *)pvParameters;
	EventBits_t result = SSH_FAILED_BIT;
	ESP_LOGI(TAG, "libssh2=%s, host=%s:%d, stage timeout=%d ms", LIBSSH2_VERSION,
		CONFIG_SSH_HOST, CONFIG_SSH_PORT, SSH_STAGE_TIMEOUT_MS);
	int rc = libssh2_init(0);
	if (rc == 0) {
		for (int attempt = 1; attempt <= 2; ++attempt) {
			int retryable = 0;
			result = ssh_attempt(command, attempt, &retryable);
			if (result != SSH_FAILED_BIT || !retryable || attempt == 2) break;
			ESP_LOGW(TAG, "Failure before command execution; retrying once with a fresh connection");
			vTaskDelay(pdMS_TO_TICKS(1000));
		}
		libssh2_exit();
	} else {
		ESP_LOGE(TAG, "libssh2 initialization failed: %d", rc);
	}
	ESP_LOGI(TAG, "RESULT: %s", result == SSH_ACCEPTED_BIT ? "COMMAND_ACKNOWLEDGED" :
		result == SSH_UNKNOWN_BIT ? "OUTCOME_UNKNOWN_DO_NOT_RETRY" : "FAILED");
	xEventGroupSetBits(xEventGroup, SSH_FINISH_BIT | result);
	vTaskDelete(NULL);
}
