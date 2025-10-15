// SPDX-License-Identifier: GPL-2.0
#include "monster_game.h"

#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/minmax.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/prandom.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/string.h>

#define ROOM_MAX_OBJECTS  4
#define INVENTORY_SLOTS   3

#define STABILITY_MAX     100
#define HUNGER_MAX        10
#define MOOD_MAX          10
#define TRUST_MAX         10

static const char * const monster_stage_names[STAGE_COUNT] = {
	[STAGE_HATCHLING] = "Hatchling",
	[STAGE_GROWING] = "Growing",
	[STAGE_MATURE] = "Mature",
	[STAGE_ELDER] = "Elder",
	[STAGE_RETIRED] = "Retired",
};

struct stage_rule {
	enum monster_stage stage;
	u32 min_tick;
	s32 min_stability;
};

static const struct stage_rule stage_rules[] = {
	{ STAGE_GROWING, 120, 40 },
	{ STAGE_MATURE, 280, 55 },
	{ STAGE_ELDER, 480, 65 },
	{ STAGE_RETIRED, 720, 75 },
};

struct command_gate {
	const char *cmd;
	const char *display;
	enum monster_stage stage;
};

static const struct command_gate command_gates[] = {
	{ "grab",     "grab <item>",   STAGE_GROWING },
	{ "analyze",  "analyze <slot>", STAGE_GROWING },
	{ "feed",     "feed <slot>",    STAGE_GROWING },
	{ "clean",    "clean <slot>",   STAGE_MATURE },
	{ "rescue",   "rescue",         STAGE_MATURE },
	{ "clear",    "clear",          STAGE_MATURE },
	{ "pet",      "pet",            STAGE_ELDER },
	{ "debug",    "debug",          STAGE_ELDER },
	{ "sing",     "sing",           STAGE_ELDER },
	{ "reset",    "reset",          STAGE_RETIRED },
};


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

struct helper_state {
	u8 helpers;           /* bitmask of MONSTER_HELPER_* flags */
	u8 happy_streak;
	u8 rescue_counter;
	u32 survived_ticks;
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
	enum monster_stage lifecycle;
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
static void announce_stage_unlocks_locked(enum monster_stage stage);
static bool command_permitted(struct monster_session *s, const char *cmd);
static void emit_available_commands(struct monster_session *s);
static void broadcast_available_commands_locked(void);
static void sess_emit(struct monster_session *s, const char *fmt, ...);
static void announce_next_goal_locked(enum monster_stage stage);
static void emit_next_goal(struct monster_session *s);
static const struct stage_rule *next_stage_rule(enum monster_stage stage);
static void stage_spawn_thresholds(enum monster_stage stage, u8 *resource_pct, u8 *daemon_pct);

const char *monster_game_stage_name(enum monster_stage stage)
{
	if (stage < 0 || stage >= STAGE_COUNT)
		return "Unknown";
	return monster_stage_names[stage];
}


static void maybe_advance_lifecycle_locked(void)
{
	enum monster_stage stage_cur = sys_state.lifecycle;
	int i;

	for (i = 0; i < ARRAY_SIZE(stage_rules); i++) {
		const struct stage_rule *rule = &stage_rules[i];

		if (rule->stage <= stage_cur)
			continue;
		if (sys_state.tick < rule->min_tick)
			break;
		if (sys_state.stability < rule->min_stability)
			break;

		sys_state.lifecycle = rule->stage;
		stage_cur = sys_state.lifecycle;
		broadcast_all_locked("[LIFECYCLE] Stage advanced to %s!\n",
			monster_game_stage_name(rule->stage));
		announce_stage_unlocks_locked(rule->stage);
		broadcast_available_commands_locked();
		announce_next_goal_locked(sys_state.lifecycle);
	}
}

static const struct command_gate *find_gate(const char *cmd)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(command_gates); i++)
		if (!strcmp(command_gates[i].cmd, cmd))
			return &command_gates[i];

	return NULL;
}

static size_t format_available_commands(enum monster_stage stage, char *buf, size_t size)
{
	size_t len = scnprintf(buf, size,
		"look, go <dir>, state, inventory, say <msg>, quit");
	int i;

	for (i = 0; i < ARRAY_SIZE(command_gates); i++) {
		const struct command_gate *gate = &command_gates[i];

		if (stage < gate->stage)
			continue;
		len += scnprintf(buf + len, size - len, ", %s",
			gate->display);
	}
	return len;
}

static const struct stage_rule *next_stage_rule(enum monster_stage stage)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(stage_rules); i++) {
		if (stage_rules[i].stage > stage)
			return &stage_rules[i];
	}
	return NULL;
}

static void announce_stage_unlocks_locked(enum monster_stage stage)
{
	char buf[MONSTER_MAX_LINE];
	size_t len = 0;
	bool first = true;
	int i;

	for (i = 0; i < ARRAY_SIZE(command_gates); i++) {
		const struct command_gate *gate = &command_gates[i];

		if (gate->stage != stage)
			continue;
		if (first) {
			len = scnprintf(buf, sizeof(buf),
				"[TIP] Commands unlocked at %s: %s",
				monster_game_stage_name(stage), gate->display);
			first = false;
		} else {
			len += scnprintf(buf + len, sizeof(buf) - len,
				", %s", gate->display);
		}
	}
	if (first)
		return;
	len += scnprintf(buf + len, sizeof(buf) - len, "\n");
	broadcast_all_locked("%s", buf);
}

static void announce_next_goal_locked(enum monster_stage stage)
{
	const struct stage_rule *next = next_stage_rule(stage);

	if (!next) {
		broadcast_all_locked("[QUEST] The Friendly Monster is retired. Enjoy free play!\n");
		return;
	}

	broadcast_all_locked("[QUEST] Goal: reach %s (tick %u+, stability %d+).\n",
		monster_game_stage_name(next->stage),
		next->min_tick,
		next->min_stability);
}

static bool command_permitted(struct monster_session *s, const char *cmd)
{
	const struct command_gate *gate = find_gate(cmd);
	enum monster_stage required = gate ? gate->stage : STAGE_HATCHLING;
	enum monster_stage stage = READ_ONCE(sys_state.lifecycle);
	const char *label = gate ? gate->display : cmd;

	if (stage >= required)
		return true;

	sess_emit(s,
		"[TIP] '%s' unlocks at stage %s (current: %s).\n",
		label,
		monster_game_stage_name(required),
		monster_game_stage_name(stage));
	return false;
}

static void emit_available_commands(struct monster_session *s)
{
	char buf[MONSTER_MAX_LINE];
	enum monster_stage stage = READ_ONCE(sys_state.lifecycle);

	format_available_commands(stage, buf, sizeof(buf));
	sess_emit(s, "[TIP] Commands available: %s\n", buf);
}

static void broadcast_available_commands_locked(void)
{
	char buf[MONSTER_MAX_LINE];
	size_t len;

	len = format_available_commands(sys_state.lifecycle, buf, sizeof(buf));
	len += scnprintf(buf + len, sizeof(buf) - len, "\n");
	broadcast_all_locked("[TIP] Commands available: %s", buf);
}

static void emit_next_goal(struct monster_session *s)
{
	char buf[MONSTER_MAX_LINE];
	enum monster_stage stage = READ_ONCE(sys_state.lifecycle);
	const struct stage_rule *next = next_stage_rule(stage);

	if (!next) {
		sess_emit(s, "[QUEST] The Friendly Monster is retired. Enjoy free play!\n");
		return;
	}

	scnprintf(buf, sizeof(buf),
		"[QUEST] Goal: reach %s (tick %u+, stability %d+).\n",
		monster_game_stage_name(next->stage),
		next->min_tick,
		next->min_stability);
	sess_emit(s, "%s", buf);
}


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
	sys_state.lifecycle = STAGE_HATCHLING;
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

static void stage_spawn_thresholds(enum monster_stage stage, u8 *resource_pct,
	u8 *daemon_pct)
{
	switch (stage) {
	case STAGE_HATCHLING:
		*resource_pct = 40;
		*daemon_pct = 10;
		break;
	case STAGE_GROWING:
		*resource_pct = 55;
		*daemon_pct = 15;
		break;
	case STAGE_MATURE:
		*resource_pct = 65;
		*daemon_pct = 25;
		break;
	case STAGE_ELDER:
		*resource_pct = 70;
		*daemon_pct = 30;
		break;
	case STAGE_RETIRED:
	default:
		*resource_pct = 75;
		*daemon_pct = 35;
		break;
	}
}

struct game_event {
	const char *name;
	u8 weight;
	enum monster_stage min_stage;
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
	{ "Resource mutation", 20, STAGE_GROWING, event_resource_mutation },
	{ "Mood swing", 20, STAGE_HATCHLING, event_mood_swing },
	{ "Lost process", 15, STAGE_MATURE, event_lost_process },
	{ "Glitch storm", 15, STAGE_ELDER, event_glitch_storm },
	{ "Lucky sync", 20, STAGE_HATCHLING, event_lucky_sync },
};
#define GAME_EVENT_COUNT (sizeof(game_events)/sizeof(game_events[0]))

static void run_random_event_locked(void)
{
	u32 total = 0, pick, accum = 0;
	u32 i;
	char buf[MONSTER_MAX_LINE] = "";
	enum monster_stage stage = sys_state.lifecycle;

	for (i = 0; i < GAME_EVENT_COUNT; i++) {
		if (stage < game_events[i].min_stage)
			continue;
		total += game_events[i].weight;
	}
	if (!total)
		return;
	pick = rand_u32() % total;
	for (i = 0; i < GAME_EVENT_COUNT; i++) {
		const struct game_event *ev = &game_events[i];

		if (stage < ev->min_stage)
			continue;
		accum += ev->weight;
		if (pick < accum) {
			if (ev->fn)
				ev->fn(buf, sizeof(buf));
			break;
		}
	}
	if (buf[0])
		broadcast_all_locked("%s", buf);
}

static void spawn_phase_locked(void)
{
	char buf[MONSTER_MAX_LINE] = "";
	u8 resource_pct, daemon_pct;

	stage_spawn_thresholds(sys_state.lifecycle, &resource_pct, &daemon_pct);
	if (rand_percent() < resource_pct)
		spawn_buffet_resource_locked(buf, sizeof(buf));
	if (buf[0])
		broadcast_all_locked("%s", buf);

	buf[0] = '\0';
	if (rand_percent() < daemon_pct)
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
	int hunger_gain = (sys_state.helper.helpers & MONSTER_HELPER_SCHED_BLESSING) ? 0 : 1;
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

	if (!(sys_state.helper.helpers & MONSTER_HELPER_MEMORY_SPRITE) &&
	    sys_state.helper.survived_ticks >= 20) {
		sys_state.helper.helpers |= MONSTER_HELPER_MEMORY_SPRITE;
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

	if (!(sys_state.helper.helpers & MONSTER_HELPER_SCHED_BLESSING) &&
	    sys_state.helper.happy_streak >= 10) {
		sys_state.helper.helpers |= MONSTER_HELPER_SCHED_BLESSING;
		scnprintf(buf, sizeof(buf), "[HELPER] Scheduler Blessing granted: hunger gain slowed!\n");
	}
	if (buf[0]) {
		broadcast_all_locked("%s", buf);
		buf[0] = '\0';
	}

	if (!(sys_state.helper.helpers & MONSTER_HELPER_IO_PIXIE) &&
	    sys_state.helper.rescue_counter >= 3) {
		sys_state.helper.helpers |= MONSTER_HELPER_IO_PIXIE;
		scnprintf(buf, sizeof(buf), "[HELPER] IO Pixie flits in to rescue strays!\n");
	}
	if (buf[0]) {
		broadcast_all_locked("%s", buf);
		buf[0] = '\0';
	}

	if (sys_state.helper.helpers & MONSTER_HELPER_MEMORY_SPRITE) {
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

	if (sys_state.helper.helpers & MONSTER_HELPER_IO_PIXIE) {
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
		if (sys_state.helper.helpers & MONSTER_HELPER_MEMORY_SPRITE)
			sess_emit(s, "MemorySprite ");
		if (sys_state.helper.helpers & MONSTER_HELPER_SCHED_BLESSING)
			sess_emit(s, "SchedulerBlessing ");
		if (sys_state.helper.helpers & MONSTER_HELPER_IO_PIXIE)
			sess_emit(s, "IOPixie ");
		sess_emit(s, "\n");
	}

	if (s->player->room_id == ROOM_NURSERY) {
		sess_emit(s, "[MONSTER] The Friendly Monster is %s.\n",
			monster_state_name(sys_state.monster_state));
		sess_emit(s, "[LIFECYCLE] Stage %s.\n",
			monster_game_stage_name(sys_state.lifecycle));
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
	sess_emit(s, "[LIFECYCLE] Current stage: %s.\n",
		monster_game_stage_name(READ_ONCE(sys_state.lifecycle)));
	emit_available_commands(s);
	emit_next_goal(s);
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

static bool cmd_reset(struct monster_session *s)
{
	struct actor *p = s->player;
	if (!p) { sess_emit(s, "login first\n"); return false; }
	mutex_lock(&world_lock);
	if (!sys_state.crashed) {
		mutex_unlock(&world_lock);
		sess_emit(s, "System still running. No reset needed.\n");
		return false;
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
	announce_next_goal_locked(sys_state.lifecycle);
	broadcast_available_commands_locked();
	mutex_unlock(&world_lock);

	broadcast_all("[PROC] %s restores the kernel. New shift begins!\n", p->name);
	sess_emit(s, "System reset complete. Everyone wakes in /proc/nursery.\n");

	return true;
}

/* ---- Ticking ----------------------------------------------------------- */
bool monster_game_tick(void)
{
	bool crashed_now;
	const char *crash_reason = NULL;

	mutex_lock(&world_lock);

	if (sys_state.crashed) {
		mutex_unlock(&world_lock);
		return true;
	}

	sys_state.tick++;

	spawn_phase_locked();
	update_phase_locked();
	run_random_event_locked();
	helper_phase_locked();
	cleanup_phase_locked();
	maybe_advance_lifecycle_locked();

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

	return crashed_now;
}

void monster_game_get_stats(struct monster_game_stats *stats)
{
	if (!stats)
		return;

	mutex_lock(&world_lock);
	stats->tick = sys_state.tick;
	stats->stability = sys_state.stability;
	stats->hunger = sys_state.hunger;
	stats->mood = sys_state.mood;
	stats->trust = sys_state.trust;
	stats->junk_load = sys_state.junk_load;
	stats->daemon_lost = sys_state.daemon_lost;
	stats->helper_mask = sys_state.helper.helpers;
	stats->monster_state = sys_state.monster_state;
	stats->lifecycle = sys_state.lifecycle;
	mutex_unlock(&world_lock);
}

unsigned int monster_game_handle_line(struct monster_session *s, char *line)
{
	unsigned int events = MONSTER_GAME_EVENT_NONE;
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
	else if (!strcmp(line, "grab")) {
		if (!command_permitted(s, "grab"))
			return events;
		cmd_grab(s, arg);
	}
	else if (!strcmp(line, "analyze")) {
		if (!command_permitted(s, "analyze"))
			return events;
		cmd_analyze(s, arg);
	}
	else if (!strcmp(line, "feed")) {
		if (!command_permitted(s, "feed"))
			return events;
		cmd_feed(s, arg);
	}
	else if (!strcmp(line, "clean")) {
		if (!command_permitted(s, "clean"))
			return events;
		cmd_clean(s, arg);
	}
	else if (!strcmp(line, "rescue")) {
		if (!command_permitted(s, "rescue"))
			return events;
		cmd_rescue(s);
	}
	else if (!strcmp(line, "clear")) {
		if (!command_permitted(s, "clear"))
			return events;
		cmd_clear(s);
	}
	else if (!strcmp(line, "pet")) {
		if (!command_permitted(s, "pet"))
			return events;
		cmd_pet(s);
	}
	else if (!strcmp(line, "debug")) {
		if (!command_permitted(s, "debug"))
			return events;
		cmd_debug(s);
	}
	else if (!strcmp(line, "sing")) {
		if (!command_permitted(s, "sing"))
			return events;
		cmd_sing(s);
	}
	else if (!strcmp(line, "reset")) {
		if (!command_permitted(s, "reset"))
			return events;
		if (cmd_reset(s))
			events |= MONSTER_GAME_EVENT_RESET;
	}
	else if (!strcmp(line, "inventory"))  cmd_inventory(s);
	else if (!strcmp(line, "state"))      cmd_state(s);
	else if (!strcmp(line, "say"))        cmd_say(s, arg ? arg : "");
	else if (!strcmp(line, "quit"))       sess_emit(s, "Goodbye.\n");
	else                                    sess_emit(s, "Unknown command. Try: look/go/grab/analyze/feed/clean/rescue/clear/pet/debug/sing/reset/inventory/state/say/quit\n");

	return events;
}

static void monster_game_session_stop_locked(struct monster_session *s)
{
	if (!list_empty(&s->list))
		list_del_init(&s->list);
	if (s->player) {
		list_del_init(&s->player->room_node);
		list_del_init(&s->player->world_node);
		kfree(s->player);
		s->player = NULL;
	}
}

int monster_game_session_start(struct monster_session *s)
{
	mutex_lock(&world_lock);
	list_add_tail(&s->list, &sessions);
	mutex_unlock(&world_lock);

	s->player = NULL;

	sess_emit(s, "Welcome to /dev/monster.\n");
	sess_emit(s, "Commands: login <name>, look, go <dir>, grab <item>, analyze <slot>, feed <slot>, clean <slot>, rescue, clear, pet, debug, sing, inventory, state, say <msg>, reset, quit\n");
	return 0;
}

void monster_game_session_stop(struct monster_session *s)
{
	mutex_lock(&world_lock);
	monster_game_session_stop_locked(s);
	mutex_unlock(&world_lock);
}

void monster_game_shutdown_sessions(void (*cleanup_cb)(struct monster_session *s))
{
	struct monster_session *s, *tmp;

	mutex_lock(&world_lock);
	list_for_each_entry_safe(s, tmp, &sessions, list) {
		monster_game_session_stop_locked(s);
		mutex_unlock(&world_lock);
		if (cleanup_cb)
			cleanup_cb(s);
		mutex_lock(&world_lock);
	}
	mutex_unlock(&world_lock);
}

int monster_game_init(void)
{
	int i;

	INIT_LIST_HEAD(&sessions);
	INIT_LIST_HEAD(&actors);
	for (i = 0; i < ROOM_COUNT; i++) {
		INIT_LIST_HEAD(&room_lists[i]);
		rooms[i].id = i;
	}

	init_rng_state();
	game_reset_state();
	return 0;
}

void monster_game_exit(void)
{
	/* Nothing to do: all sessions should be drained by monster_game_shutdown_sessions(). */
}
