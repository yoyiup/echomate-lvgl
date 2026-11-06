#include "hotspot_control.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
std::atomic<int> busy{0};
std::atomic<int> last_error{0};

bool process_alive(const char *file)
{
	FILE *fp = std::fopen(file, "r");
	if (!fp) return false;
	long pid = 0;
	int ok = std::fscanf(fp, "%ld", &pid);
	std::fclose(fp);
	return ok == 1 && pid > 1 && ::kill(static_cast<pid_t>(pid), 0) == 0;
}

bool running()
{
	return process_alive("/tmp/echomate-hostapd.pid") &&
	       process_alive("/tmp/echomate-dnsmasq.pid") &&
	       process_alive("/tmp/echomate-transfer.pid");
}
} // namespace

extern "C" int hotspot_is_running(void)
{
	return running() ? 1 : 0;
}

extern "C" void hotspot_get(hotspot_info *out)
{
	if (!out) return;
	std::memset(out, 0, sizeof(*out));
	out->running = running() ? 1 : 0;
	out->busy = busy.load(std::memory_order_acquire);
	out->last_error = last_error.load(std::memory_order_acquire);
	FILE *fp = std::fopen("/root/.echomate-hotspot-password", "r");
	if (fp) {
		if (std::fgets(out->password, sizeof(out->password), fp))
			out->password[std::strcspn(out->password, "\r\n")] = '\0';
		std::fclose(fp);
	}
}

extern "C" int hotspot_request(int on)
{
	int expected = 0;
	if (!busy.compare_exchange_strong(expected, on ? 1 : 2))
		return -EBUSY;
	if (!!on == running()) {
		last_error.store(0, std::memory_order_release);
		busy.store(0, std::memory_order_release);
		return 0;
	}
	last_error.store(0, std::memory_order_release);
	try {
		std::thread([on] {
			const char *cmd = on
				? "/root/bin/echomate-hotspot.sh start >/dev/null 2>&1"
				: "/root/bin/echomate-hotspot.sh stop >/dev/null 2>&1";
			int rc = std::system(cmd);
			last_error.store(rc >= 0 && WIFEXITED(rc) ? WEXITSTATUS(rc) : 1,
					 std::memory_order_release);
			busy.store(0, std::memory_order_release);
		}).detach();
	} catch (...) {
		last_error.store(1, std::memory_order_release);
		busy.store(0, std::memory_order_release);
		return -ENOMEM;
	}
	return 0;
}
