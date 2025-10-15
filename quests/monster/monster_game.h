#ifndef MONSTER_GAME_H
#define MONSTER_GAME_H

#include <linux/bitops.h>
#include <linux/kfifo.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/wait.h>

struct file;
struct actor;

#define MONSTER_FIFO_SZ   4096
#define MONSTER_MAX_NAME  24
#define MONSTER_MAX_LINE  512

enum monster_mood_state {
    MONSTER_SLEEPING = 0,
    MONSTER_HUNGRY,
    MONSTER_CONTENT,
    MONSTER_OVERFED,
    MONSTER_GLITCHING,
};

enum monster_stage {
    STAGE_HATCHLING = 0,
    STAGE_GROWING,
    STAGE_MATURE,
    STAGE_ELDER,
    STAGE_RETIRED,
    STAGE_COUNT,
};

enum monster_helper_bits {
	MONSTER_HELPER_MEMORY_SPRITE = BIT(0),
	MONSTER_HELPER_SCHED_BLESSING = BIT(1),
	MONSTER_HELPER_IO_PIXIE = BIT(2),
};

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

struct monster_game_stats {
    u32 tick;
    s32 stability;
    s32 hunger;
    s32 mood;
    s32 trust;
    s32 junk_load;
    bool daemon_lost;
    u8 helper_mask;
    enum monster_mood_state monster_state;
    enum monster_stage lifecycle;
};

const char *monster_game_stage_name(enum monster_stage stage);

int monster_game_init(void);
void monster_game_exit(void);

int monster_game_session_start(struct monster_session *s);
void monster_game_session_stop(struct monster_session *s);

void monster_game_shutdown_sessions(void (*cleanup_cb)(struct monster_session *s));

bool monster_game_tick(void);
void monster_game_get_stats(struct monster_game_stats *stats);

enum monster_game_event {
	MONSTER_GAME_EVENT_NONE  = 0,
	MONSTER_GAME_EVENT_RESET = BIT(0),
};

unsigned int monster_game_handle_line(struct monster_session *s, char *line);

#endif /* MONSTER_GAME_H */
