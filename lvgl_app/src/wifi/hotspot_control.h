#ifndef ECHOMATE_HOTSPOT_CONTROL_H
#define ECHOMATE_HOTSPOT_CONTROL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int running;
	int busy;       /* 0 idle, 1 starting, 2 stopping */
	int last_error;
	char password[32];
} hotspot_info;

void hotspot_get(hotspot_info *out);
int hotspot_request(int on);
int hotspot_is_running(void);

#ifdef __cplusplus
}
#endif
#endif
