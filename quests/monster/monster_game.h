#ifndef MONSTER_GAME_H
#define MONSTER_GAME_H

#include <linux/kfifo.h>
#include <linux/list.h>
#include <linux/bitops.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/wait.h>

struct file;
struct actor;

struct monster_session {
	struct kfifo out;
	struct mutex out_lock;
	wait_queue_head_t wq;
	bool closed;

	struct list_head list;
	struct actor *player;
	struct file *filp;

	char inbuf[256];
	size_t inlen;
};

int monster_game_init(void);
void monster_game_exit(void);

int monster_game_session_start(struct monster_session *s);
void monster_game_session_stop(struct monster_session *s);

void monster_game_shutdown_sessions(void (*cleanup_cb)(struct monster_session *s));

bool monster_game_tick(void);

enum monster_game_event {
	MONSTER_GAME_EVENT_NONE  = 0,
	MONSTER_GAME_EVENT_RESET = BIT(0),
};

unsigned int monster_game_handle_line(struct monster_session *s, char *line);

#endif /* MONSTER_GAME_H */
#define MONSTER_FIFO_SZ   4096
#define MONSTER_MAX_NAME  24
#define MONSTER_MAX_LINE  512
