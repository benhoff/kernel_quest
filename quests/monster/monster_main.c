// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/kfifo.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/workqueue.h>
#include <linux/ktime.h>
#include <linux/random.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/bitops.h>
#include <linux/minmax.h>
#include <linux/prandom.h>

#define MONSTER_FIFO_SZ   4096
#define MONSTER_MAX_NAME  24
#define MONSTER_MAX_LINE  512

#define ROOM_MAX_OBJECTS  4
#define INVENTORY_SLOTS   3

#define STABILITY_MAX     100
#define HUNGER_MAX        10
#define MOOD_MAX          10
#define TRUST_MAX         10

MODULE_LICENSE("GPL");
MODULE_AUTHOR("you");
MODULE_DESCRIPTION("Kernel Caretakers — cooperative kernel critter simulation");


/* ---- Tunables --------------------------------------------------------- */
/* Tick interval for the game loop (ms) */
static unsigned int tick_ms = 250;
static struct delayed_work tick_work;
static bool tick_work_ready;

static unsigned int clamp_tick_ms(unsigned int v)
{
    unsigned int min = jiffies_to_msecs(1);      /* 1 jiffy: ~1/HZ (e.g., 4–10 ms) */
    unsigned int max = 60 * 1000;                /* cap to 60s (tweak as you like) */
    if (v == 0) return 0;                        /* 0 = paused */
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

/* live setter: validates and re-arms the worker */
static int set_tick_ms(const char *val, const struct kernel_param *kp)
{
    unsigned int v = tick_ms;
    int ret = kstrtouint(val, 0, &v);
    if (ret) return ret;

    v = clamp_tick_ms(v);
    *(unsigned int *)kp->arg = v;

    if (!READ_ONCE(tick_work_ready))
	    return 0;

    if (v == 0) {  /* pause */
        cancel_delayed_work_sync(&tick_work);
        pr_info("monster: ticks paused (tick_ms=0)\n");
    } else {
        /* re-arm relative to now; safe from any context */
        unsigned long delay = msecs_to_jiffies(v);
        if (delay == 0) delay = 1;
        /* re-arm relative to now */
        mod_delayed_work(system_wq, &tick_work, delay);
        pr_info("monster: tick interval set to %u ms (min=%u ms)\n",
                v, max(1u, jiffies_to_msecs(1)));
    }
    return 0;
}

static const struct kernel_param_ops tick_ops = {
    .set = set_tick_ms,
    .get = param_get_uint,
};

module_param_cb(tick_ms, &tick_ops, &tick_ms, 0644);
MODULE_PARM_DESC(tick_ms, "Game tick interval in ms (0=paused; clamped to [1 jiffy, 60s])");

/* Optional deterministic RNG seed (0 = use kernel randomness) */
static unsigned int rng_seed = 0;
module_param(rng_seed, uint, 0644);
MODULE_PARM_DESC(rng_seed, "Deterministic RNG seed (0 disables)");

/* ---- World model ------------------------------------------------------- */

enum dir { N=0, E=1, S=2, W=3, DIRS=4, NONE=255 };

enum room_id { ROOM_NURSERY = 0, ROOM_BUFFET = 1, ROOM_FIELDS = 2, ROOM_COUNT = 3 };

struct room {
	u32 id;
	const char *name;
	const char *desc;
	int exits[DIRS]; /* -1 = no exit */
};

static struct room rooms[ROOM_COUNT] = {
	[ROOM_NURSERY] = { ROOM_NURSERY, "/proc/nursery", "Friendly Monster naps amid warm kernel blankets.", {
		[ N ] = -1,
		[ E ] = ROOM_BUFFET,
		[ S ] = -1,
		[ W ] = ROOM_FIELDS,
	}},
	[ROOM_BUFFET] = { ROOM_BUFFET, "/tmp/buffet", "Resource carts roll in and out, piled high with tasty chunks.", {
		[ N ] = -1,
		[ E ] = -1,
		[ S ] = -1,
		[ W ] = ROOM_NURSERY,
	}},
	[ROOM_FIELDS] = { ROOM_FIELDS, "/dev/null/fields", "Windy plains sweep away unwanted bits and lost daemons.", {
		[ N ] = -1,
		[ E ] = ROOM_NURSERY,
		[ S ] = -1,
		[ W ] = -1,
	}},
};

/* Where the monster starts (room id) */
static int start_room = ROOM_NURSERY;
module_param(start_room, int, 0644);
MODULE_PARM_DESC(start_room, "Initial helper spawn room id (default nursery)");

enum item_type {
	ITEM_NONE = 0,
	ITEM_RAM_CHUNK,
	ITEM_IO_TOKEN,
	ITEM_CPU_SLICE,
	ITEM_JUNK_DATA,
	ITEM_BABY_DAEMON,
};

enum item_flag {
	ITEMF_IDENTIFIED = BIT(0),
	ITEMF_MUTATED   = BIT(1),
};

struct held_item {
	enum item_type type;
	u8 flags;
};

struct room_object {
	enum item_type type;
	u8 ttl;                    /* ticks before despawn */
	u8 flags;
};

struct actor {
	u32 id;
	char name[MONSTER_MAX_NAME];
	u32 room_id;
	u8  hp;
	u8  hiding;          /* 1 if hiding this tick window */
	struct list_head room_node;  /* room membership */
	struct list_head world_node; /* global actor roster */
	struct held_item inventory[INVENTORY_SLOTS];
	u8 selected_slot;     /* optional quick slot */
};

enum helper_kind {
	HELPER_NONE = 0,
	HELPER_MEMORY_SPRITE = BIT(0),
	HELPER_SCHED_BLESSING = BIT(1),
	HELPER_IO_PIXIE = BIT(2),
};

struct helper_state {
	u8 helpers;           /* bitmask of helper_kind */
	u8 happy_streak;
	u8 rescue_counter;
	u32 survived_ticks;
};

enum monster_mood_state {
	MONSTER_SLEEPING = 0,
	MONSTER_HUNGRY,
	MONSTER_CONTENT,
	MONSTER_OVERFED,
	MONSTER_GLITCHING,
};

struct system_state {
	s32 stability;        /* 0-100 */
	s32 hunger;           /* 0-10 */
	s32 mood;             /* -10 to 10 */
	s32 trust;            /* 0-10 */
	u32 tick;
	s32 junk_load;        /* aggregated junk pressure */
	bool daemon_lost;
	enum monster_mood_state monster_state;
	struct helper_state helper;
	bool crashed;
};

struct monster_session {
	/* Per-open */
	struct kfifo out;
	struct mutex out_lock;
	wait_queue_head_t wq;
	bool closed;

	struct actor *player; /* NULL until login */
	struct file *filp;    /* for identification if needed */
	struct list_head list; /* global session list link */

	/* line buffer for write() accumulation (simple) */
	char inbuf[256];
	size_t inlen;
};

static DEFINE_MUTEX(world_lock); /* guards world + room actors */
static LIST_HEAD(sessions);      /* all sessions, for broadcasts */
static LIST_HEAD(actors);        /* all actors, if you need global ops */

/* For each room, who is inside (players only; monster tracked separately) */
static struct list_head room_lists[ROOM_COUNT];

static struct room_object room_objects[ROOM_COUNT][ROOM_MAX_OBJECTS];
static struct system_state sys_state;
static bool rng_use_seed;
static struct rnd_state rng_state;

/* Forward declarations for helpers referenced before definition */
static void recompute_monster_state(void);
static void broadcast_all_locked(const char *fmt, ...);
static void broadcast_all_locked_v(const char *fmt, va_list ap);


/* ---- Helpers ----------------------------------------------------------- */

static const char *dir_name(enum dir d) {
	switch (d) { case N: return "north"; case E: return "east";
		case S: return "south"; case W: return "west"; default: return "?"; }
}
static int room_exit(int rid, enum dir d) { return rooms[rid].exits[d]; }

static inline bool room_valid(int rid)
{
	return rid >= 0 && rid < ROOM_COUNT;
}

static const char *item_name(enum item_type type)
{
	switch (type) {
	case ITEM_RAM_CHUNK: return "RAM chunk";
	case ITEM_IO_TOKEN:  return "IO token";
	case ITEM_CPU_SLICE: return "CPU slice";
	case ITEM_JUNK_DATA: return "junk data";
	case ITEM_BABY_DAEMON: return "baby daemon";
	case ITEM_NONE:
	default: return "nothing";
	}
}

static bool item_is_feed(enum item_type type)
{
	return type == ITEM_RAM_CHUNK || type == ITEM_IO_TOKEN || type == ITEM_CPU_SLICE;
}

static bool item_is_junk(enum item_type type)
{
	return type == ITEM_JUNK_DATA;
}

static void clear_room_object(struct room_object *obj)
{
	obj->type = ITEM_NONE;
	obj->ttl = 0;
	obj->flags = 0;
}

static void clear_inventory(struct actor *a)
{
	int i;
	for (i = 0; i < INVENTORY_SLOTS; i++) {
		a->inventory[i].type = ITEM_NONE;
		a->inventory[i].flags = 0;
	}
	a->selected_slot = 0;
}

static inline s32 clamp_s32(s32 v, s32 lo, s32 hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

static const char *monster_state_name(enum monster_mood_state st)
{
	switch (st) {
	case MONSTER_SLEEPING: return "sleeping";
	case MONSTER_HUNGRY: return "hungry";
	case MONSTER_CONTENT: return "content";
	case MONSTER_OVERFED: return "overfed";
	case MONSTER_GLITCHING: return "glitching";
	default: return "???";
	}
}

static void reset_room_objects(void)
{
	int r, i;
	for (r = 0; r < ROOM_COUNT; r++)
		for (i = 0; i < ROOM_MAX_OBJECTS; i++)
			clear_room_object(&room_objects[r][i]);
}

static u32 rand_u32(void)
{
	if (rng_use_seed)
		return prandom_u32_state(&rng_state);
	else {
		u32 v;
		get_random_bytes(&v, sizeof(v));
		return v;
	}
}

static void init_rng_state(void)
{
	if (rng_seed) {
		prandom_seed_state(&rng_state, rng_seed);
		rng_use_seed = true;
	} else {
		rng_use_seed = false;
	}
}

static u8 rand_percent(void)
{
	return (u8)(rand_u32() % 100);
}

static u8 rand_range(u8 min, u8 max)
{
	if (max <= min)
		return min;
	return (u8)(min + (rand_u32() % (max - min + 1)));
}

static void game_reset_state(void)
{
	reset_room_objects();
	sys_state.stability = STABILITY_MAX;
	sys_state.hunger = 3;
	sys_state.mood = 0;
	sys_state.trust = 3;
	sys_state.tick = 0;
	sys_state.junk_load = 0;
	sys_state.daemon_lost = false;
	sys_state.monster_state = MONSTER_SLEEPING;
	sys_state.helper.helpers = 0;
	sys_state.helper.happy_streak = 0;
	sys_state.helper.rescue_counter = 0;
	sys_state.helper.survived_ticks = 0;
	sys_state.crashed = false;
	recompute_monster_state();
}

static int room_first_free_slot(enum room_id rid)
{
	int i;
	for (i = 0; i < ROOM_MAX_OBJECTS; i++)
		if (room_objects[rid][i].type == ITEM_NONE)
			return i;
	return -1;
}

static void adjust_stability(int delta)
{
	sys_state.stability = clamp_s32(sys_state.stability + delta, 0, STABILITY_MAX);
}

static void adjust_hunger(int delta)
{
	sys_state.hunger = clamp_s32(sys_state.hunger + delta, 0, HUNGER_MAX);
}

static void adjust_mood(int delta)
{
	sys_state.mood = clamp_s32(sys_state.mood + delta, -MOOD_MAX, MOOD_MAX);
}

static void adjust_trust(int delta)
{
	sys_state.trust = clamp_s32(sys_state.trust + delta, 0, TRUST_MAX);
}

static void adjust_junk(int delta)
{
	sys_state.junk_load = clamp_s32(sys_state.junk_load + delta, 0, 50);
}

static void recompute_monster_state(void)
{
	if (sys_state.mood <= -4 || sys_state.junk_load >= 12)
		sys_state.monster_state = MONSTER_GLITCHING;
	else if (sys_state.hunger >= 7)
		sys_state.monster_state = MONSTER_HUNGRY;
	else if (sys_state.hunger <= 1 && sys_state.mood >= 2)
		sys_state.monster_state = MONSTER_SLEEPING;
	else if (sys_state.hunger <= 1)
		sys_state.monster_state = MONSTER_OVERFED;
	else
		sys_state.monster_state = MONSTER_CONTENT;
}

static int room_object_count(enum room_id rid)
{
	int i, cnt = 0;
	for (i = 0; i < ROOM_MAX_OBJECTS; i++)
		if (room_objects[rid][i].type != ITEM_NONE)
			cnt++;
	return cnt;
}

static struct room_object *room_add_object(enum room_id rid, enum item_type type, u8 ttl)
{
	int slot = room_first_free_slot(rid);
	if (slot < 0)
		return NULL;
	room_objects[rid][slot].type = type;
	room_objects[rid][slot].ttl = ttl;
	room_objects[rid][slot].flags = 0;
	return &room_objects[rid][slot];
}

static void room_decay_objects(enum room_id rid)
{
	int i;
	for (i = 0; i < ROOM_MAX_OBJECTS; i++) {
		struct room_object *o = &room_objects[rid][i];
		if (o->type == ITEM_NONE)
			continue;
		if (o->ttl > 0) {
			if (--o->ttl == 0) {
				/* evaporate */
				clear_room_object(o);
			}
		}
	}
}

static enum item_type random_buffet_resource(void)
{
	u8 p = rand_percent();
	if (p < 40)
		return ITEM_RAM_CHUNK;
	if (p < 70)
		return ITEM_IO_TOKEN;
	if (p < 90)
		return ITEM_CPU_SLICE;
	return ITEM_JUNK_DATA;
}

static void spawn_buffet_resource_locked(char *logbuf, size_t buflen)
{
	struct room_object *obj;
	enum item_type type;

	if (room_first_free_slot(ROOM_BUFFET) < 0)
		return;
	type = random_buffet_resource();
	obj = room_add_object(ROOM_BUFFET, type, rand_range(3, 5));
	if (!obj)
		return;
	if (type == ITEM_JUNK_DATA && rand_percent() < 30)
		obj->flags |= ITEMF_MUTATED;
	if (logbuf)
		scnprintf(logbuf, buflen, "[SPAWN] %s appears in /tmp/buffet.\n", item_name(type));
}

static void spawn_daemon_locked(char *logbuf, size_t buflen)
{
	if (room_object_count(ROOM_FIELDS) >= ROOM_MAX_OBJECTS)
		return;
	if (sys_state.daemon_lost)
		return;
	if (!room_add_object(ROOM_FIELDS, ITEM_BABY_DAEMON, rand_range(4, 6)))
		return;
	sys_state.daemon_lost = true;
	if (logbuf)
		scnprintf(logbuf, buflen, "[ALERT] A baby daemon wanders into /dev/null/fields!\n");
}

struct game_event {
	const char *name;
	u8 weight;
	void (*fn)(char *buf, size_t len);
};

static void event_resource_mutation(char *buf, size_t len)
{
	int i;
	for (i = 0; i < ROOM_MAX_OBJECTS; i++) {
		struct room_object *o = &room_objects[ROOM_BUFFET][i];
		if (o->type == ITEM_NONE)
			continue;
		if (item_is_feed(o->type)) {
			o->type = ITEM_JUNK_DATA;
			o->flags |= ITEMF_MUTATED;
			scnprintf(buf, len, "[EVENT] A %s mutates into junk data!\n", item_name(ITEM_JUNK_DATA));
			adjust_junk(2);
			return;
		}
	}
}

static void event_mood_swing(char *buf, size_t len)
{
	int delta = (rand_percent() < 50) ? 2 : -2;
	adjust_mood(delta);
	if (delta > 0)
		scnprintf(buf, len, "[EVENT] Monster gets lonely then delighted when you wave. mood %+d\n", delta);
	else
		scnprintf(buf, len, "[EVENT] Monster frets over idle cycles. mood %+d\n", delta);
}

static void event_lost_process(char *buf, size_t len)
{
	spawn_daemon_locked(buf, len);
}

static void event_glitch_storm(char *buf, size_t len)
{
	int spawned = 0;
	int attempts = 2;
	while (attempts-- && room_first_free_slot(ROOM_BUFFET) >= 0) {
		if (!room_add_object(ROOM_BUFFET, ITEM_JUNK_DATA, rand_range(2, 4)))
			break;
		spawned++;
	}
	if (spawned) {
		adjust_junk(spawned * 2);
		scnprintf(buf, len, "[EVENT] Glitch storm sprays %d junk piles across /tmp!\n", spawned);
	}
}

static void event_lucky_sync(char *buf, size_t len)
{
	adjust_hunger(-1);
	adjust_stability(2);
	spawn_buffet_resource_locked(buf, len);
	if (buf && *buf == '\0')
		scnprintf(buf, len, "[EVENT] Lucky sync! Hunger eases and resources sparkle.\n");
}

static const struct game_event game_events[] = {
	{ "Resource mutation", 20, event_resource_mutation },
	{ "Mood swing", 20, event_mood_swing },
	{ "Lost process", 15, event_lost_process },
	{ "Glitch storm", 15, event_glitch_storm },
	{ "Lucky sync", 20, event_lucky_sync },
};
#define GAME_EVENT_COUNT (sizeof(game_events)/sizeof(game_events[0]))

static void run_random_event_locked(void)
{
	u32 total = 0, pick, accum = 0;
	u32 i;
	char buf[MONSTER_MAX_LINE] = "";

	for (i = 0; i < GAME_EVENT_COUNT; i++)
		total += game_events[i].weight;
	if (!total)
		return;
	pick = rand_u32() % total;
	for (i = 0; i < GAME_EVENT_COUNT; i++) {
		accum += game_events[i].weight;
		if (pick < accum) {
			if (game_events[i].fn)
				game_events[i].fn(buf, sizeof(buf));
			break;
		}
	}
	if (buf[0])
		broadcast_all_locked("%s", buf);
}

static void spawn_phase_locked(void)
{
	char buf[MONSTER_MAX_LINE] = "";
	if (rand_percent() < 65)
		spawn_buffet_resource_locked(buf, sizeof(buf));
	if (buf[0])
		broadcast_all_locked("%s", buf);

	buf[0] = '\0';
	if (rand_percent() < 25)
		spawn_daemon_locked(buf, sizeof(buf));
	if (buf[0])
		broadcast_all_locked("%s", buf);
}

static void cleanup_phase_locked(void)
{
	room_decay_objects(ROOM_BUFFET);
	room_decay_objects(ROOM_FIELDS);
	room_decay_objects(ROOM_NURSERY);
}

static void update_phase_locked(void)
{
	char buf[MONSTER_MAX_LINE] = "";
	int hunger_gain = (sys_state.helper.helpers & HELPER_SCHED_BLESSING) ? 0 : 1;
	adjust_hunger(hunger_gain);
	if (sys_state.hunger >= 8)
		adjust_stability(-3);
	if (sys_state.hunger >= 6)
		adjust_mood(-1);
	if (sys_state.junk_load > 0)
		adjust_stability(-min(sys_state.junk_load, 5));
	if (sys_state.trust >= 7 && sys_state.stability < STABILITY_MAX)
		adjust_stability(1);
	recompute_monster_state();

	if (sys_state.monster_state == MONSTER_OVERFED && rand_percent() < 35) {
		if (room_add_object(ROOM_BUFFET, ITEM_JUNK_DATA, rand_range(2, 4))) {
			adjust_junk(2);
			scnprintf(buf, sizeof(buf), "[MONSTER] The Monster sneezes junk into /tmp!\n");
		}
	}
	if (buf[0])
		broadcast_all_locked("%s", buf);

	buf[0] = '\0';
	if (sys_state.monster_state == MONSTER_CONTENT && sys_state.mood >= 3 && rand_percent() < 30) {
		adjust_stability(2);
		scnprintf(buf, sizeof(buf), "[PROC] The Monster forks a helper daemon to tidy things up.\n");
	}
	if (buf[0])
		broadcast_all_locked("%s", buf);
}

static void helper_phase_locked(void)
{
	char buf[MONSTER_MAX_LINE] = "";
	sys_state.helper.survived_ticks++;

	if (!(sys_state.helper.helpers & HELPER_MEMORY_SPRITE) &&
	    sys_state.helper.survived_ticks >= 20) {
		sys_state.helper.helpers |= HELPER_MEMORY_SPRITE;
		scnprintf(buf, sizeof(buf), "[HELPER] Memory Sprite joins you, whisking junk away!\n");
	}
	if (buf[0]) {
		broadcast_all_locked("%s", buf);
		buf[0] = '\0';
	}

	if (sys_state.monster_state == MONSTER_CONTENT || sys_state.monster_state == MONSTER_SLEEPING)
		sys_state.helper.happy_streak = min_t(u8, sys_state.helper.happy_streak + 1, 60);
	else
		sys_state.helper.happy_streak = 0;

	if (!(sys_state.helper.helpers & HELPER_SCHED_BLESSING) &&
	    sys_state.helper.happy_streak >= 10) {
		sys_state.helper.helpers |= HELPER_SCHED_BLESSING;
		scnprintf(buf, sizeof(buf), "[HELPER] Scheduler Blessing granted: hunger gain slowed!\n");
	}
	if (buf[0]) {
		broadcast_all_locked("%s", buf);
		buf[0] = '\0';
	}

	if (!(sys_state.helper.helpers & HELPER_IO_PIXIE) &&
	    sys_state.helper.rescue_counter >= 3) {
		sys_state.helper.helpers |= HELPER_IO_PIXIE;
		scnprintf(buf, sizeof(buf), "[HELPER] IO Pixie flits in to rescue strays!\n");
	}
	if (buf[0]) {
		broadcast_all_locked("%s", buf);
		buf[0] = '\0';
	}

	if (sys_state.helper.helpers & HELPER_MEMORY_SPRITE) {
		if (sys_state.junk_load > 0) {
			int before = sys_state.junk_load;
			adjust_junk(-1);
			if (sys_state.junk_load < before)
				scnprintf(buf, sizeof(buf), "[HELPER] Memory Sprite sweeps away lingering junk.\n");
		}
	}
	if (buf[0]) {
		broadcast_all_locked("%s", buf);
		buf[0] = '\0';
	}

	if (sys_state.helper.helpers & HELPER_IO_PIXIE) {
		int i;
		for (i = 0; i < ROOM_MAX_OBJECTS; i++) {
			struct room_object *o = &room_objects[ROOM_FIELDS][i];
			if (o->type == ITEM_BABY_DAEMON) {
				clear_room_object(o);
				sys_state.daemon_lost = false;
				adjust_trust(1);
				adjust_stability(1);
				scnprintf(buf, sizeof(buf), "[HELPER] IO Pixie swoops a daemon back to safety!\n");
				break;
			}
		}
	}
	recompute_monster_state();
	if (buf[0])
		broadcast_all_locked("%s", buf);
}

static void crash_report_locked(const char *reason)
{
	char buf[MONSTER_MAX_LINE];
	if (sys_state.crashed)
		return;
	sys_state.crashed = true;
	scnprintf(buf, sizeof(buf),
		"[CRASH] Kernel Caretakers collapse: %s after %u ticks. stability=%d hunger=%d mood=%d trust=%d junk=%d\n",
		reason,
		sys_state.tick,
		sys_state.stability,
		sys_state.hunger,
		sys_state.mood,
		sys_state.trust,
		sys_state.junk_load);
	broadcast_all_locked("%s", buf);
	broadcast_all_locked("[CRASH] Friendly Monster dumps core. Thanks for playing!\n");
}

static int room_match_object(enum room_id rid, const char *token)
{
	int i;
	long idx;

	if (!token)
		return -1;

	if (kstrtol(token, 10, &idx) == 0) {
		if (idx >= 1 && idx <= ROOM_MAX_OBJECTS) {
			if (room_objects[rid][idx - 1].type != ITEM_NONE)
				return idx - 1;
		}
	}

	for (i = 0; i < ROOM_MAX_OBJECTS; i++) {
		struct room_object *o = &room_objects[rid][i];
		const char *name;
		if (o->type == ITEM_NONE)
			continue;
		name = item_name(o->type);
		if (!strncasecmp(name, token, strlen(token)))
			return i;
	}
	return -1;
}

static int inventory_first_free(struct actor *a)
{
	int i;
	for (i = 0; i < INVENTORY_SLOTS; i++)
		if (a->inventory[i].type == ITEM_NONE)
			return i;
	return -1;
}

static int inventory_match_slot(struct actor *a, const char *token)
{
	long idx;
	if (!token)
		return -1;
	if (!strcmp(token, "sel"))
		return min_t(int, a->selected_slot, INVENTORY_SLOTS - 1);
	if (kstrtol(token, 10, &idx) == 0) {
		if (idx >= 1 && idx <= INVENTORY_SLOTS)
			return idx - 1;
	}
	return -1;
}

static void sess_emit(struct monster_session *s, const char *fmt, ...)
{
	char buf[MONSTER_MAX_LINE];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	mutex_lock(&s->out_lock);
	kfifo_in(&s->out, buf, n);
	mutex_unlock(&s->out_lock);
	wake_up_interruptible(&s->wq);
}

static void broadcast_room(u32 room_id, const char *fmt, ...)
{
	char buf[MONSTER_MAX_LINE];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	mutex_lock(&world_lock);
	/* walk sessions, check if player in room_id */
	{
		struct monster_session *s;
		list_for_each_entry(s, &sessions, list) {
			if (!s->player) continue;
			if (s->player->room_id != room_id) continue;
			mutex_lock(&s->out_lock);
			kfifo_in(&s->out, buf, n);
			mutex_unlock(&s->out_lock);
			wake_up_interruptible(&s->wq);
		}
	}
	mutex_unlock(&world_lock);
}

static void broadcast_room_locked(u32 room_id, const char *fmt, ...)
{
	char buf[MONSTER_MAX_LINE];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	/* walk sessions, check if player in room_id */
	{
		struct monster_session *s;
		list_for_each_entry(s, &sessions, list) {
			if (!s->player) continue;
			if (s->player->room_id != room_id) continue;
			mutex_lock(&s->out_lock);
			kfifo_in(&s->out, buf, n);
			mutex_unlock(&s->out_lock);
			wake_up_interruptible(&s->wq);
		}
	}
}

static void broadcast_all_locked_v(const char *fmt, va_list ap)
{
	char buf[MONSTER_MAX_LINE];
	int n;
	struct monster_session *s;

	n = vsnprintf(buf, sizeof(buf), fmt, ap);

	list_for_each_entry(s, &sessions, list) {
		if (!s->player)
			continue;
		mutex_lock(&s->out_lock);
		kfifo_in(&s->out, buf, n);
		mutex_unlock(&s->out_lock);
		wake_up_interruptible(&s->wq);
	}
}

static void broadcast_all_locked(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	broadcast_all_locked_v(fmt, ap);
	va_end(ap);
}

static void broadcast_all(const char *fmt, ...)
{
	mutex_lock(&world_lock);
	{
		va_list ap;
		va_start(ap, fmt);
		broadcast_all_locked_v(fmt, ap);
		va_end(ap);
	}
	mutex_unlock(&world_lock);
}

/* ---- World ops --------------------------------------------------------- */

static void place_actor_in_room(struct actor *a, int new_room)
{
	/* assumes world_lock held */
	if (a->room_id < ROOM_COUNT)
		list_del_init(&a->room_node);

	a->room_id = new_room;
	list_add_tail(&a->room_node, &room_lists[new_room]);
}

static void look_room_unlocked(struct monster_session *s)
{
	struct room *r;
	struct actor *a_it;
	int i;

	if (!s->player)
		return;
	r = &rooms[s->player->room_id];

	sess_emit(s, "\n== %s ==\n%s\n", r->name, r->desc);
	sess_emit(s, "[EXITS] ");
	for (i = 0; i < DIRS; i++) {
		if (r->exits[i] >= 0)
			sess_emit(s, "%s ", dir_name(i));
	}
	sess_emit(s, "\n");

	sess_emit(s,
		"[STATE] stability=%d hunger=%d mood=%d trust=%d tick=%u junk=%d%s\n",
		sys_state.stability,
		sys_state.hunger,
		sys_state.mood,
		sys_state.trust,
		sys_state.tick,
		sys_state.junk_load,
		sys_state.daemon_lost ? " daemon-lost" : "");
	if (sys_state.helper.helpers) {
		sess_emit(s, "[HELPERS] ");
		if (sys_state.helper.helpers & HELPER_MEMORY_SPRITE)
			sess_emit(s, "MemorySprite ");
		if (sys_state.helper.helpers & HELPER_SCHED_BLESSING)
			sess_emit(s, "SchedulerBlessing ");
		if (sys_state.helper.helpers & HELPER_IO_PIXIE)
			sess_emit(s, "IOPixie ");
		sess_emit(s, "\n");
	}

	if (s->player->room_id == ROOM_NURSERY) {
		sess_emit(s, "[MONSTER] The Friendly Monster is %s.\n",
			monster_state_name(sys_state.monster_state));
	}

	sess_emit(s, "Objects here:\n");
	for (i = 0; i < ROOM_MAX_OBJECTS; i++) {
		struct room_object *o = &room_objects[r->id][i];
		if (o->type == ITEM_NONE)
			continue;
		sess_emit(s, "  %d) %s%s (ttl %u)%s\n", i + 1,
			item_name(o->type),
			(o->flags & ITEMF_IDENTIFIED) ? " [id]" : "",
			o->ttl ? o->ttl : 1,
			(o->flags & ITEMF_MUTATED) ? " [weird]" : "");
	}
	if (!room_object_count(r->id))
		sess_emit(s, "  (nothing interesting)\n");

	sess_emit(s, "Players present:\n");
	list_for_each_entry(a_it, &room_lists[r->id], room_node) {
		if (a_it == s->player)
			continue;
		sess_emit(s, "  %s\n", a_it->name);
	}

	if (!list_empty(&room_lists[r->id])) {
		if (list_is_singular(&room_lists[r->id])) {
			struct actor *only = list_first_entry(&room_lists[r->id], struct actor, room_node);
			if (only == s->player)
				sess_emit(s, "  (just you)\n");
		}
	} else {
		sess_emit(s, "  (no other helpers)\n");
	}

	sess_emit(s, "\n");
}

/* ---- Command parsing --------------------------------------------------- */
static void cmd_login(struct monster_session *s, const char *arg)
{
	if (s->player) { sess_emit(s, "Already logged in.\n"); return; }
	if (!arg || !*arg) { sess_emit(s, "Usage: login <name>\n"); return; }

	struct actor *a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a) { sess_emit(s, "No mem\n"); return; }
	strscpy(a->name, arg, sizeof(a->name));
	a->hp = 5;
	INIT_LIST_HEAD(&a->room_node);
	INIT_LIST_HEAD(&a->world_node);
	clear_inventory(a);
	a->hiding = 0;

	mutex_lock(&world_lock);
	a->id = (u32)(uintptr_t)a; /* lazy unique */
	list_add_tail(&a->world_node, &actors);
	place_actor_in_room(a, room_valid(start_room) ? start_room : ROOM_NURSERY);
	mutex_unlock(&world_lock);

	s->player = a;

	sess_emit(s, "[PROC] Helper thread %s spawned.\n", a->name);
	sess_emit(s, "[TIP] Commands: look, go <dir>, grab <item>, analyze <slot>, feed <slot>, clean <slot>, rescue, clear, pet, debug, sing, inventory, state, say <msg>, reset, quit\n");
	look_room_unlocked(s);
}

static void cmd_look(struct monster_session *s)
{
	mutex_lock(&world_lock);
	look_room_unlocked(s);
	mutex_unlock(&world_lock);
}

static enum dir parse_dir(const char *arg)
{
	if (!arg) return NONE;
	if (!strcmp(arg,"n") || !strcmp(arg,"north")) return N;
	if (!strcmp(arg,"e") || !strcmp(arg,"east"))  return E;
	if (!strcmp(arg,"s") || !strcmp(arg,"south")) return S;
	if (!strcmp(arg,"w") || !strcmp(arg,"west"))  return W;
	return NONE;
}

static void cmd_go(struct monster_session *s, const char *arg)
{
	enum dir d = parse_dir(arg);
	int dst;

	if (!s->player) { sess_emit(s, "login first\n"); return; }
	if (d == NONE) { sess_emit(s, "Usage: go n|e|s|w\n"); return; }

	mutex_lock(&world_lock);
	dst = room_exit(s->player->room_id, d);
	if (dst < 0) {
		mutex_unlock(&world_lock);
		sess_emit(s, "No exit that way.\n");
		return;
	}
	place_actor_in_room(s->player, dst);
	mutex_unlock(&world_lock);

	sess_emit(s, "You move to %s.\n", rooms[dst].name);
	cmd_look(s);
}

static void cmd_say(struct monster_session *s, const char *msg)
{
	if (!s->player) { sess_emit(s, "login first\n"); return; }
	if (!msg) msg = "";
	broadcast_room(s->player->room_id, "%s says: %s\n", s->player->name, msg);
}

static void cmd_inventory(struct monster_session *s)
{
	int i;
	if (!s->player) { sess_emit(s, "login first\n"); return; }
	mutex_lock(&world_lock);
	sess_emit(s, "Inventory (slots %d):\n", INVENTORY_SLOTS);
	for (i = 0; i < INVENTORY_SLOTS; i++) {
		struct held_item *it = &s->player->inventory[i];
		if (it->type == ITEM_NONE)
			sess_emit(s, "  %d) -- empty --\n", i + 1);
		else
			sess_emit(s, "  %d) %s%s\n", i + 1,
				item_name(it->type),
				(it->flags & ITEMF_IDENTIFIED) ? " [id]" : "");
	}
	mutex_unlock(&world_lock);
}

static void cmd_state(struct monster_session *s)
{
	if (!s->player) { sess_emit(s, "login first\n"); return; }
	mutex_lock(&world_lock);
	sess_emit(s, "[STATE] stability=%d hunger=%d mood=%d trust=%d tick=%u junk=%d daemon_lost=%s\n",
		sys_state.stability,
		sys_state.hunger,
		sys_state.mood,
		sys_state.trust,
		sys_state.tick,
		sys_state.junk_load,
		sys_state.daemon_lost ? "yes" : "no");
	sess_emit(s, "Monster: %s\n", monster_state_name(sys_state.monster_state));
	mutex_unlock(&world_lock);
}

static void cmd_grab(struct monster_session *s, const char *arg)
{
	struct actor *p = s->player;
	int slot, inv;
	struct room_object *obj;

	if (!p) { sess_emit(s, "login first\n"); return; }
	mutex_lock(&world_lock);
	if (!room_object_count(p->room_id)) {
		mutex_unlock(&world_lock);
		sess_emit(s, "Nothing to grab here.\n");
		return;
	}
	if (!arg || !*arg)
		slot = room_match_object(p->room_id, "1");
	else
		slot = room_match_object(p->room_id, arg);
	if (slot < 0) {
		mutex_unlock(&world_lock);
		sess_emit(s, "No such item. Try numbers.\n");
		return;
	}
	obj = &room_objects[p->room_id][slot];
	if (obj->type == ITEM_BABY_DAEMON) {
		mutex_unlock(&world_lock);
		sess_emit(s, "The baby daemon scoots away. Maybe try `rescue`.\n");
		return;
	}
	inv = inventory_first_free(p);
	if (inv < 0) {
		mutex_unlock(&world_lock);
		sess_emit(s, "Inventory full. Try analyze/clean/feed first.\n");
		return;
	}
	p->inventory[inv].type = obj->type;
	p->inventory[inv].flags = obj->flags;
	p->selected_slot = inv;
	clear_room_object(obj);
	mutex_unlock(&world_lock);

	sess_emit(s, "You stash %s in slot %d.\n", item_name(p->inventory[inv].type), inv + 1);
}

static void cmd_analyze(struct monster_session *s, const char *arg)
{
	struct actor *p = s->player;
	int slot;
	struct held_item *it;
	char msg[MONSTER_MAX_LINE] = "";

	if (!p) { sess_emit(s, "login first\n"); return; }
	if (!arg || !*arg)
		slot = p->selected_slot;
	else
		slot = inventory_match_slot(p, arg);
	if (slot < 0 || slot >= INVENTORY_SLOTS) {
		sess_emit(s, "Usage: analyze <slot#>\n");
		return;
	}
	mutex_lock(&world_lock);
	it = &p->inventory[slot];
	if (it->type == ITEM_NONE) {
		mutex_unlock(&world_lock);
		sess_emit(s, "Slot %d is empty.\n", slot + 1);
		return;
	}
	it->flags |= ITEMF_IDENTIFIED;
	if (it->flags & ITEMF_MUTATED) {
		it->type = ITEM_JUNK_DATA;
		it->flags &= ~ITEMF_MUTATED;
		scnprintf(msg, sizeof(msg), "Analysis complete: corrupted -> junk data.\n");
	} else if (item_is_junk(it->type)) {
		scnprintf(msg, sizeof(msg), "Analysis: %s is junk. Handle carefully.\n", item_name(it->type));
	} else {
		scnprintf(msg, sizeof(msg), "Analysis: %s looks tasty for the Monster.\n", item_name(it->type));
	}
	mutex_unlock(&world_lock);
	if (msg[0])
		sess_emit(s, "%s", msg);
}

static void cmd_feed(struct monster_session *s, const char *arg)
{
	struct actor *p = s->player;
	int slot;
	struct held_item *it;

	if (!p) { sess_emit(s, "login first\n"); return; }
	if (p->room_id != ROOM_NURSERY) {
		sess_emit(s, "The Monster is back in /proc/nursery. Feed there.\n");
		return;
	}
	if (!arg || !*arg)
		slot = p->selected_slot;
	else
		slot = inventory_match_slot(p, arg);
	if (slot < 0 || slot >= INVENTORY_SLOTS) {
		sess_emit(s, "Usage: feed <slot#>\n");
		return;
	}
	mutex_lock(&world_lock);
	it = &p->inventory[slot];
	if (it->type == ITEM_NONE) {
		mutex_unlock(&world_lock);
		sess_emit(s, "Slot %d is empty.\n", slot + 1);
		return;
	}
	if (item_is_feed(it->type)) {
		s32 hunger_drop = (it->type == ITEM_CPU_SLICE) ? 4 : 3;
		s32 mood_boost = (it->type == ITEM_IO_TOKEN) ? 3 : 2;
		adjust_hunger(-hunger_drop);
		adjust_mood(mood_boost);
		adjust_trust(1);
		adjust_stability(2);
		if (it->type == ITEM_RAM_CHUNK)
			adjust_stability(1);
		p->inventory[slot].type = ITEM_NONE;
		p->inventory[slot].flags = 0;
		recompute_monster_state();
		broadcast_room_locked(ROOM_NURSERY,
			"[MONSTER] %s feeds the Monster. It purrs happily.\n",
			s->player->name);
	} else if (item_is_junk(it->type)) {
		adjust_hunger(1);
		adjust_mood(-3);
		adjust_trust(-2);
		adjust_stability(-5);
		adjust_junk(2);
		p->inventory[slot].type = ITEM_NONE;
		p->inventory[slot].flags = 0;
		recompute_monster_state();
		broadcast_room_locked(ROOM_NURSERY,
			"[MONSTER] %s accidentally feeds junk! The Monster glitches.\n",
			s->player->name);
	} else {
		mutex_unlock(&world_lock);
		sess_emit(s, "The Monster refuses that offering.\n");
		return;
	}
	mutex_unlock(&world_lock);
}

static void cmd_clean(struct monster_session *s, const char *arg)
{
	struct actor *p = s->player;
	int slot;
	struct held_item *it;
	bool in_fields;
	char msg[MONSTER_MAX_LINE] = "";

	if (!p) { sess_emit(s, "login first\n"); return; }
	if (!arg || !*arg)
		slot = p->selected_slot;
	else
		slot = inventory_match_slot(p, arg);
	if (slot < 0 || slot >= INVENTORY_SLOTS) {
		sess_emit(s, "Usage: clean <slot#>\n");
		return;
	}
	mutex_lock(&world_lock);
	it = &p->inventory[slot];
	if (it->type == ITEM_NONE) {
		mutex_unlock(&world_lock);
		sess_emit(s, "Slot %d is empty.\n", slot + 1);
		return;
	}
	in_fields = (p->room_id == ROOM_FIELDS);
	if (item_is_junk(it->type)) {
		adjust_junk(in_fields ? -3 : -1);
		adjust_mood(in_fields ? 1 : 0);
		if (in_fields)
			adjust_stability(1);
		recompute_monster_state();
		scnprintf(msg, sizeof(msg), "Junk scrubbed. System load eases.\n");
	} else {
		scnprintf(msg, sizeof(msg), "You recycle %s.\n", item_name(it->type));
	}
	it->type = ITEM_NONE;
	it->flags = 0;
	mutex_unlock(&world_lock);
	if (msg[0])
		sess_emit(s, "%s", msg);
}

static void cmd_rescue(struct monster_session *s)
{
	struct actor *p = s->player;
	int i;
	bool rescued = false;

	if (!p) { sess_emit(s, "login first\n"); return; }
	if (p->room_id != ROOM_FIELDS) { sess_emit(s, "Rescues happen in /dev/null/fields.\n"); return; }
	mutex_lock(&world_lock);
	for (i = 0; i < ROOM_MAX_OBJECTS; i++) {
		struct room_object *o = &room_objects[ROOM_FIELDS][i];
		if (o->type == ITEM_BABY_DAEMON) {
			clear_room_object(o);
			adjust_trust(2);
			adjust_mood(2);
			adjust_stability(3);
			sys_state.daemon_lost = false;
			sys_state.helper.rescue_counter = min_t(u8, sys_state.helper.rescue_counter + 1, 5);
			rescued = true;
			break;
		}
	}
	if (!rescued && sys_state.daemon_lost) {
		sys_state.daemon_lost = false;
		adjust_trust(1);
		adjust_mood(1);
		adjust_stability(2);
		sys_state.helper.rescue_counter = min_t(u8, sys_state.helper.rescue_counter + 1, 5);
		rescued = true;
	}
	if (rescued)
		recompute_monster_state();
	mutex_unlock(&world_lock);

	if (rescued)
		sess_emit(s, "You guide the stray daemon back to the nursery.\n");
	else
		sess_emit(s, "Nothing to rescue right now.\n");
}

static void cmd_clear(struct monster_session *s)
{
	struct actor *p = s->player;
	int i, cleared = 0;

	if (!p) { sess_emit(s, "login first\n"); return; }
	if (p->room_id != ROOM_FIELDS) {
		sess_emit(s, "You need to be in /dev/null/fields to clear overflow.\n");
		return;
	}
	mutex_lock(&world_lock);
	for (i = 0; i < ROOM_MAX_OBJECTS; i++) {
		struct room_object *o = &room_objects[ROOM_FIELDS][i];
		if (o->type == ITEM_JUNK_DATA) {
			clear_room_object(o);
			cleared++;
		}
	}
	if (cleared) {
		adjust_junk(-cleared * 2);
		adjust_stability(1);
		adjust_mood(1);
		recompute_monster_state();
	}
	mutex_unlock(&world_lock);

	if (cleared)
		sess_emit(s, "You vent %d junk piles into the void.\n", cleared);
	else
		sess_emit(s, "Fields are tidy already.\n");
}

static void cmd_pet(struct monster_session *s)
{
	struct actor *p = s->player;
	if (!p) { sess_emit(s, "login first\n"); return; }
	if (p->room_id != ROOM_NURSERY) { sess_emit(s, "Petting works best in the nursery.\n"); return; }
	mutex_lock(&world_lock);
	adjust_mood(2);
	adjust_trust(1);
	recompute_monster_state();
	mutex_unlock(&world_lock);
	broadcast_room(ROOM_NURSERY, "[MONSTER] %s gives gentle pats. Warm chimes play.\n", p->name);
}

static void cmd_debug(struct monster_session *s)
{
	struct actor *p = s->player;
	if (!p) { sess_emit(s, "login first\n"); return; }
	if (p->room_id != ROOM_NURSERY) { sess_emit(s, "Debug rituals happen near the Monster.\n"); return; }
	mutex_lock(&world_lock);
	adjust_junk(-1);
	adjust_mood(1);
	adjust_stability(1);
	if (sys_state.monster_state == MONSTER_GLITCHING)
		adjust_mood(1);
	recompute_monster_state();
	mutex_unlock(&world_lock);
	broadcast_room(ROOM_NURSERY, "[SYSLOG] %s patches the Monster's threads. Glitches fade.\n", p->name);
}

static void cmd_sing(struct monster_session *s)
{
	struct actor *p = s->player;
	if (!p) { sess_emit(s, "login first\n"); return; }
	if (p->room_id != ROOM_NURSERY) { sess_emit(s, "Echo your song in the nursery.\n"); return; }
	mutex_lock(&world_lock);
	adjust_mood(3);
	adjust_trust(1);
	adjust_stability(1);
	recompute_monster_state();
	mutex_unlock(&world_lock);
	broadcast_room(ROOM_NURSERY, "[PROC] %s sings a lullaby. The Monster hums along.\n", p->name);
}

static void cmd_reset(struct monster_session *s)
{
	struct actor *p = s->player;
	unsigned int next_ms;
	if (!p) { sess_emit(s, "login first\n"); return; }
	mutex_lock(&world_lock);
	if (!sys_state.crashed) {
		mutex_unlock(&world_lock);
		sess_emit(s, "System still running. No reset needed.\n");
		return;
	}
	init_rng_state();
	game_reset_state();
	{
		struct actor *a;
		list_for_each_entry(a, &actors, world_node) {
			clear_inventory(a);
			place_actor_in_room(a, room_valid(start_room) ? start_room : ROOM_NURSERY);
		}
	}
	next_ms = READ_ONCE(tick_ms);
	mutex_unlock(&world_lock);

	broadcast_all("[PROC] %s restores the kernel. New shift begins!\n", p->name);
	sess_emit(s, "System reset complete. Everyone wakes in /proc/nursery.\n");

	if (next_ms) {
		cancel_delayed_work_sync(&tick_work);
		unsigned long delay = msecs_to_jiffies(next_ms);
		if (!delay)
			delay = 1;
		schedule_delayed_work(&tick_work, delay);
	}
}

/* ---- Ticking ----------------------------------------------------------- */
static void monster_tick_work(struct work_struct *w)
{
	unsigned int t;
	bool crashed_now;
	const char *crash_reason = NULL;

	mutex_lock(&world_lock);

	if (sys_state.crashed) {
		mutex_unlock(&world_lock);
		return;
	}

	sys_state.tick++;

	spawn_phase_locked();
	update_phase_locked();
	run_random_event_locked();
	helper_phase_locked();
	cleanup_phase_locked();

	if (sys_state.stability <= 0)
		crash_reason = "stability exhausted";
	else if (sys_state.mood <= -MOOD_MAX)
		crash_reason = "Monster mood meltdown";
	else if (sys_state.trust <= 0)
		crash_reason = "trust drained";
	else if (sys_state.hunger >= HUNGER_MAX)
		crash_reason = "Monster hunger overflow";
	else if (sys_state.junk_load >= 25)
		crash_reason = "junk backpressure";

	if (crash_reason)
		crash_report_locked(crash_reason);

	crashed_now = sys_state.crashed;
	mutex_unlock(&world_lock);

	t = READ_ONCE(tick_ms);
	if (t && !crashed_now) {
		unsigned long delay = msecs_to_jiffies(t);
		if (delay == 0) delay = 1;
		schedule_delayed_work(&tick_work, delay);
	}
}


/* ---- Char device fops -------------------------------------------------- */

static int monster_open(struct inode *ino, struct file *filp)
{
	struct monster_session *s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s) return -ENOMEM;
	INIT_LIST_HEAD(&s->list);
	mutex_init(&s->out_lock);
	init_waitqueue_head(&s->wq);
	if (kfifo_alloc(&s->out, MONSTER_FIFO_SZ, GFP_KERNEL)) {
		kfree(s); return -ENOMEM;
	}
	filp->private_data = s;
	s->filp = filp;

	/* register session */
	mutex_lock(&world_lock);
	list_add_tail(&s->list, &sessions);
	mutex_unlock(&world_lock);

	sess_emit(s, "Welcome to /dev/monster.\n");
	sess_emit(s, "Commands: login <name>, look, go <dir>, grab <item>, analyze <slot>, feed <slot>, clean <slot>, rescue, clear, pet, debug, sing, inventory, state, say <msg>, reset, quit\n");
	return 0;
}

static int monster_release(struct inode *ino, struct file *filp)
{
	struct monster_session *s = filp->private_data;

	mutex_lock(&world_lock);
	list_del_init(&s->list);
	if (s->player) {
		list_del_init(&s->player->room_node);
		list_del_init(&s->player->world_node);
		kfree(s->player);
	}
	mutex_unlock(&world_lock);

	kfifo_free(&s->out);
	kfree(s);
	return 0;
}

static ssize_t monster_read(struct file *filp, char __user *ubuf, size_t len, loff_t *ppos)
{
	struct monster_session *s = filp->private_data;
	unsigned int copied = 0;
	int ret;

	if (len == 0) return 0;

	/* wait until there is data */
	ret = wait_event_interruptible(s->wq, kfifo_len(&s->out) > 0);
	if (ret) return ret;

	mutex_lock(&s->out_lock);
	ret = kfifo_to_user(&s->out, ubuf, len, &copied);
	mutex_unlock(&s->out_lock);
	return ret ? ret : copied;
}

static void handle_line(struct monster_session *s, char *line)
{
	/* trim trailing newline */
	char *nl = strpbrk(line, "\r\n");
	if (nl) *nl = 0;

	/* split cmd + arg */
	char *sp = strchr(line, ' ');
	char *arg = NULL;
	if (sp) { *sp = 0; arg = sp + 1; }

	if (!strcmp(line, "login"))           cmd_login(s, arg);
	else if (!strcmp(line, "look"))       cmd_look(s);
	else if (!strcmp(line, "go"))         cmd_go(s, arg);
	else if (!strcmp(line, "grab"))       cmd_grab(s, arg);
	else if (!strcmp(line, "analyze"))    cmd_analyze(s, arg);
	else if (!strcmp(line, "feed"))       cmd_feed(s, arg);
	else if (!strcmp(line, "clean"))      cmd_clean(s, arg);
	else if (!strcmp(line, "rescue"))     cmd_rescue(s);
	else if (!strcmp(line, "clear"))      cmd_clear(s);
	else if (!strcmp(line, "pet"))        cmd_pet(s);
	else if (!strcmp(line, "debug"))      cmd_debug(s);
	else if (!strcmp(line, "sing"))       cmd_sing(s);
	else if (!strcmp(line, "reset"))      cmd_reset(s);
	else if (!strcmp(line, "inventory"))  cmd_inventory(s);
	else if (!strcmp(line, "state"))      cmd_state(s);
	else if (!strcmp(line, "say"))        cmd_say(s, arg ? arg : "");
	else if (!strcmp(line, "quit"))       sess_emit(s, "Goodbye.\n");
	else                                    sess_emit(s, "Unknown command. Try: look/go/grab/analyze/feed/clean/rescue/clear/pet/debug/sing/reset/inventory/state/say/quit\n");
}

static ssize_t monster_write(struct file *filp, const char __user *ubuf, size_t len, loff_t *ppos)
{
	struct monster_session *s = filp->private_data;
	size_t n = min(len, sizeof(s->inbuf) - 1 - s->inlen);
	if (copy_from_user(s->inbuf + s->inlen, ubuf, n))
		return -EFAULT;
	s->inlen += n;
	s->inbuf[s->inlen] = 0;

	/* process complete lines */
	{
		char *start = s->inbuf, *nl;
		while ((nl = strpbrk(start, "\n"))) {
			*nl = 0;
			handle_line(s, start);
			start = nl + 1;
		}
		/* compact leftover */
		n = s->inbuf + s->inlen - start;
		if (n && start != s->inbuf)
			memmove(s->inbuf, start, n);
		s->inlen = n;
	}

	return len;
}

static __poll_t monster_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct monster_session *s = filp->private_data;
	__poll_t m = 0;

	poll_wait(filp, &s->wq, wait);
	if (kfifo_len(&s->out) > 0)
		m |= EPOLLIN | EPOLLRDNORM;
	return m;
}

static const struct file_operations monster_fops = {
	.owner   = THIS_MODULE,
	.open    = monster_open,
	.release = monster_release,
	.read    = monster_read,
	.write   = monster_write,
	.poll    = monster_poll,
};

static struct miscdevice monster_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "monster",
	.fops  = &monster_fops,
	.mode  = 0666,
};

/* ---- Init/exit --------------------------------------------------------- */

static int __init monster_init(void)
{
	int i, ret;

	/* init room lists */
	for (i = 0; i < ROOM_COUNT; i++) {
		INIT_LIST_HEAD(&room_lists[i]);
		rooms[i].id = i;
	}
	init_rng_state();
	game_reset_state();

	INIT_DELAYED_WORK(&tick_work, monster_tick_work);
	WRITE_ONCE(tick_work_ready, true);
	tick_ms = clamp_tick_ms(tick_ms);
	if (tick_ms) {
		unsigned long delay = msecs_to_jiffies(tick_ms);
		if (delay == 0) delay = 1;
		schedule_delayed_work(&tick_work, delay);
	}
	pr_info("monster: loaded (tick_ms=%u ms, min=%u ms)\n", tick_ms, jiffies_to_msecs(1));

	ret = misc_register(&monster_miscdev);
	if (ret) {
		cancel_delayed_work_sync(&tick_work);
		return ret;
	}

	pr_info("monster: loaded at /dev/monster\n");
	return 0;
}

static void __exit monster_exit(void)
{
	WRITE_ONCE(tick_work_ready, false);
	cancel_delayed_work_sync(&tick_work);

	/* free sessions & actors */
	mutex_lock(&world_lock);
	while (!list_empty(&sessions)) {
		struct monster_session *s = list_first_entry(&sessions, struct monster_session, list);
		list_del_init(&s->list);
		if (s->player) {
			list_del_init(&s->player->room_node);
			list_del_init(&s->player->world_node);
			kfree(s->player);
		}
		kfifo_free(&s->out);
		kfree(s);
	}
	mutex_unlock(&world_lock);

	misc_deregister(&monster_miscdev);
	pr_info("monster: unloaded\n");
}

module_init(monster_init);
module_exit(monster_exit);
