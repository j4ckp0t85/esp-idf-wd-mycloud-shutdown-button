# esp-idf-ssh-client
ssh client for esp-idf.
This project use [this](https://github.com/libssh2/libssh2) ssh library.
The target is create a physical shutdown button.
In my case to properly shutdown a WD My Coud Gen2 Mirror (my father would only unplug the power supply, the NAS had to rebuild the raid at every consequent startup).

# Software requirements
esp-idf v4.4 or later.
This is because this version supports ESP32-C3.

# Installation

```
git clone https://github.com/nopnop2002/esp-idf-ssh-client
cd esp-idf-ssh-client/
git clone https://github.com/libssh2/libssh2 components/libssh2
cp esp-idf/libssh2_config.h components/libssh2/include
cp esp-idf/CMakeLists.txt components/libssh2
idf.py set-target {esp32/esp32s2/esp32c3}
idf.py menuconfig
idf.py flash
```


# Configuration
You have to set this config value with menuconfig.
- CONFIG_ESP_WIFI_SSID
SSID (network name) to connect to.
- CONFIG_ESP_WIFI_PASSWORD
WiFi password (WPA or WPA2) to use.
- CONFIG_ESP_MAXIMUM_RETRY
Set the Maximum retry to avoid station reconnecting to the AP unlimited when the AP is really inexistent.
- CONFIG_SSH_USER
Username of SSH.
- CONFIG_SSH_PASSWORD
Password of SSH.
- CONFIG_SSH_HOST
IP address of SSH host.
__mDMS name cannot be used.__
- CONFIG_SSH_PORT
TCP port number 22 is used by default.

![config-main](https://user-images.githubusercontent.com/6020549/120054821-3d755500-c06d-11eb-950c-d357d0a9fdef.jpg)
![config-app](https://user-images.githubusercontent.com/6020549/131051283-5614e66d-2c3a-4df7-88f2-863d3bf23b34.jpg)

# ssh command list
ssh command list is defined ssh/command.txt.
```
$ cat ssh/command.txt
uname -a
ls -l
```


# Screen Shot
![ssh-client-1](https://user-images.githubusercontent.com/6020549/120056024-b1ffc200-c074-11eb-8507-1bb566b0cc7c.jpg)

# Shutdown Reliability

Buttons are active-low (wired from the configured input to ground). A falling
edge starts a command after 50 ms debounce; releasing the button does not start
another command. Button events during SSH or result indication are discarded.

SSH uses a non-blocking socket and session. Each TCP connection has a 10-second
deadline; handshake, authentication, channel opening, and command acknowledgement
each have a 30-second deadline. Remote output/status is collected for up to
5 seconds after acknowledgement. Each stage logs its elapsed time on the
115200-baud serial console.

Wi-Fi power saving is disabled (`WIFI_PS_NONE`) to test whether modem sleep
contributes to the observed TCP/SSH delays. This increases ESP32 power consumption.
Each accepted button press logs the access point RSSI (dBm) and channel, or a
warning if access point information is unavailable. SSH deadlines, retry policy,
and the shutdown command remain unchanged for this test.

The button task has an 8 KiB stack to accommodate Wi-Fi diagnostics, logging,
and SSH task creation. Its minimum free stack is logged after each completed
command cycle. Activity outputs are explicitly cleared during GPIO startup.

There is at most one fresh-connection retry after a failure before command
execution. Rejected credentials are not retried. Once execution is attempted,
the command is never automatically resent, even if its acknowledgement times
out: the NAS may already be shutting down. Remaining commands in the file are
not run after a failed or unknown result.

Both configured activity outputs are on while SSH is running, then turn off:
- No blinking: command acknowledged, not proof of physical shutdown.
- Three blinks: failure (including missing command file or task allocation).
- Five blinks: outcome unknown; check the NAS before pressing again.

A zero shell exit status is not proof that a backgrounded shutdown command
succeeded. The existing shutdown command is unchanged.

After building and flashing on the ESP-IDF laptop:
1. Capture serial output at 115200 baud and press once with the NAS running.
2. Check stage timings, the final `RESULT`, LED indication, and actual NAS state.
3. Verify releasing or holding the button does not cause a second command.
4. Repeat across several NAS wake/shutdown cycles.
5. With the NAS offline, verify two bounded pre-execution attempts and three blinks.
6. If five blinks occur, inspect NAS state before any manual retry.

# Reference
https://github.com/nopnop2002/esp-idf-scp-client

You can use scp and ssh to do heavy processing that esp32 alone cannot.
- Execute server-side command with ssh.
- Output the processing result to a file.
- Copy file from server using scp-get.


