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

#define MONSTER_FIFO_SZ   4096
#define MONSTER_MAX_NAME  24
#define MONSTER_MAX_LINE  256

MODULE_LICENSE("GPL");
MODULE_AUTHOR("you");
MODULE_DESCRIPTION("Kernel monster (char device) — 'Monster in the Dark'");


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

/* Monster movement cooldown, in ticks (uses tick_ms as the unit) */
static unsigned int move_cooldown_ticks = 12;
module_param(move_cooldown_ticks, uint, 0644);
MODULE_PARM_DESC(move_cooldown_ticks, "Monster move cooldown in ticks (default 12)");

/* Where the monster starts (room id) */
static int start_room = 1;
module_param(start_room, int, 0644);
MODULE_PARM_DESC(start_room, "Initial monster room id (default 1)");

/* Optional deterministic RNG seed (0 = use kernel randomness) */
static unsigned int rng_seed = 0;
module_param(rng_seed, uint, 0644);
MODULE_PARM_DESC(rng_seed, "Deterministic RNG seed (0 disables)");

/* ---- World model ------------------------------------------------------- */

enum dir { N=0, E=1, S=2, W=3, DIRS=4, NONE=255 };

struct room {
	u32 id;
	const char *name;
	const char *desc;
	int exits[DIRS]; /* -1 = no exit */
};

static struct room rooms[] = {
	{0,"Library","Dusty stacks and a broken lantern.", {1,-1,3,-1}},
	{1,"Hallway","A narrow hall. Scratches on the walls.", {2,-1, -1,0}},
	{2,"Kitchen","Cold stove. Knife missing.", {-1,-1,-1,1}},
	{3,"Storage","Crates and a creaking shelf.", {-1,4,-1,0}},
	{4,"Atrium","Moonlight through a cracked dome.", {-1,5,-1,3}},
	{5,"Cellar","Damp, with the smell of iron.", {-1,-1,-1,4}},
};
#define ROOM_COUNT (sizeof(rooms)/sizeof(rooms[0]))

struct monster_state {
	u32 room_id;
	u8  cooldown_ticks;  /* moves when 0 */
	u8  alive;           /* reserved for respawn */
};

struct actor {
	u32 id;
	char name[MONSTER_MAX_NAME];
	u32 room_id;
	u8  hp;
	u8  hiding;          /* 1 if hiding this tick window */
	struct list_head node;  /* for room membership list */
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

static struct monster_state mon;


/* ---- Helpers ----------------------------------------------------------- */

static const char *dir_name(enum dir d) {
	switch (d) { case N: return "north"; case E: return "east";
		case S: return "south"; case W: return "west"; default: return "?"; }
}
static int room_exit(int rid, enum dir d) { return rooms[rid].exits[d]; }
static inline enum dir opposite_dir(enum dir d) { return (d + 2) % DIRS; }
static enum dir direction_to(int from_room, int to_room)
{
	int d;
	for (d = 0; d < DIRS; d++) {
		if (rooms[from_room].exits[d] == to_room)
			return (enum dir)d;
	}
	return NONE;
}

static inline bool room_valid(int rid)
{
	return rid >= 0 && rid < ROOM_COUNT;
}

static void sess_emit_locked(struct monster_session *s, const char *fmt, ...)
{
	char buf[MONSTER_MAX_LINE];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (n < 0) return;
	if (n > sizeof(buf)) n = sizeof(buf);

	/* push and wake */
	kfifo_in(&s->out, buf, n);
	wake_up_interruptible(&s->wq);
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

/* ---- World ops --------------------------------------------------------- */

static void place_actor_in_room(struct actor *a, int new_room)
{
	/* assumes world_lock held */
	if (a->room_id < ROOM_COUNT)
		list_del_init(&a->node);

	a->room_id = new_room;
	list_add_tail(&a->node, &room_lists[new_room]);
}

static void look_room_unlocked(struct monster_session *s)
{
	struct room *r;
	struct actor *a_it;
	char exits[80] = "";
	int i, off = 0;

	if (!s->player) return;
	r = &rooms[s->player->room_id];

	off += scnprintf(exits + off, sizeof(exits)-off, "Exits:");
	for (i = 0; i < DIRS; i++)
		if (r->exits[i] >= 0)
			off += scnprintf(exits + off, sizeof(exits)-off, " %s", dir_name(i));
	off += scnprintf(exits + off, sizeof(exits)-off, "\n");

	sess_emit(s, "You are in %s. %s\n", r->name, r->desc);
	sess_emit(s, "%s", exits);

	/* who is here (players) */
	sess_emit(s, "You see:");
	list_for_each_entry(a_it, &room_lists[r->id], node) {
		if (a_it != s->player)
			sess_emit(s, " %s", a_it->name);
	}
	sess_emit(s, "\n");

	/* monster presence */
	if (mon.room_id == r->id)
		sess_emit(s, "The monster is here. (fight/hide?)\n");
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
	INIT_LIST_HEAD(&a->node);

	mutex_lock(&world_lock);
	a->id = (u32)(uintptr_t)a; /* lazy unique */
	list_add_tail(&actors, &a->node); /* (optional: separate list) */
	place_actor_in_room(a, 0);
	mutex_unlock(&world_lock);

	s->player = a;

	sess_emit(s, "WELCOME %s. You are in %s.\n", a->name, rooms[a->room_id].name);
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

static void cmd_hide(struct monster_session *s)
{
	if (!s->player) { sess_emit(s, "login first\n"); return; }
	mutex_lock(&world_lock);
	if (mon.room_id == s->player->room_id) {
		u8 r; get_random_bytes(&r,1);
		if ((r % 100) < 70) {
			sess_emit(s, "You hide behind debris. The monster seems confused.\n");
		} else {
			sess_emit(s, "You try to hide, but make a noise!\n");
		}
	} else {
		sess_emit(s, "You hide for a moment. All quiet.\n");
	}
	mutex_unlock(&world_lock);
}

static void cmd_fight(struct monster_session *s)
{
	if (!s->player) { sess_emit(s, "login first\n"); return; }
	mutex_lock(&world_lock);
	if (mon.room_id != s->player->room_id) {
		mutex_unlock(&world_lock);
		sess_emit(s, "Nothing to fight here.\n");
		return;
	}
	{
		u8 r; get_random_bytes(&r,1);
		if ((r % 100) < 40) {
			sess_emit(s, "You strike boldly! The monster recoils.\n");
		} else {
			if (s->player->hp > 0) s->player->hp--;
			sess_emit(s, "The monster lashes out! HP=%d\n", s->player->hp);
			if (s->player->hp == 0) {
				int rid = s->player->room_id;
				sess_emit(s, "You collapse. Darkness takes you...\n");
				place_actor_in_room(s->player, 0);
				s->player->hp = 5;
				broadcast_room_locked(rid, "%s falls and is dragged away...\n", s->player->name);
				sess_emit(s, "You awaken in %s.\n", rooms[0].name);
				cmd_look(s);
			}
		}
	}
	mutex_unlock(&world_lock);
}

static void cmd_who(struct monster_session *s)
{
	struct actor *a;
	if (!s->player) { sess_emit(s, "login first\n"); return; }
	mutex_lock(&world_lock);
	sess_emit(s, "Players here:");
	list_for_each_entry(a, &room_lists[s->player->room_id], node)
		sess_emit(s, " %s", a->name);
	sess_emit(s, "\n");
	mutex_unlock(&world_lock);
}

/* ---- Ticking ----------------------------------------------------------- */
static void monster_tick_work(struct work_struct *w)
{
	int prev = -1, next = -1;
	enum dir moved_dir = NONE;
	bool moved = false;

	mutex_lock(&world_lock);

	if (mon.cooldown_ticks > 0) {
		mon.cooldown_ticks--;
	} else {
		int tries = 8;
		while (tries--) {
			u8 d;
			get_random_bytes(&d, 1);
			d %= DIRS;

			/* candidate exit? */
			{
				int dst = room_exit(mon.room_id, d);
				if (dst >= 0) {
					prev = mon.room_id;
					next = dst;
					mon.room_id = dst;
					mon.cooldown_ticks = move_cooldown_ticks;
					moved_dir = (enum dir)d; /* direction from prev */
					moved = true;
					break;
				}
			}
		}
	}

	mutex_unlock(&world_lock);

	if (moved) {
		/* Leaving is always the direction we took from prev */
		broadcast_room(prev,
			"The monster leaves to the %s.\n", dir_name(moved_dir));

		/* Entering: compute direction as seen from the destination */
		{
			enum dir enter_from = direction_to(next, prev);
			if (enter_from != NONE) {
				broadcast_room(next,
					"The monster enters from the %s!\n", dir_name(enter_from));
			} else {
				/* No reverse edge; avoid claiming an impossible side */
				broadcast_room(next,
					"The monster enters %s!\n", rooms[next].name);
			}
		}
	}

        unsigned int t = READ_ONCE(tick_ms);
        if (t)  /* never schedule with 0 delay */
	{
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
	sess_emit(s, "Commands: login <name>, look, go n|e|s|w, hide, fight, say <msg>, who, quit\n");
	return 0;
}

static int monster_release(struct inode *ino, struct file *filp)
{
	struct monster_session *s = filp->private_data;

	mutex_lock(&world_lock);
	list_del_init(&s->list);
	if (s->player) {
		list_del_init(&s->player->node);
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

	if (!strcmp(line, "login"))      cmd_login(s, arg);
	else if (!strcmp(line, "look"))  cmd_look(s);
	else if (!strcmp(line, "go"))    cmd_go(s, arg);
	else if (!strcmp(line, "say"))   cmd_say(s, arg ? arg : "");
	else if (!strcmp(line, "hide"))  cmd_hide(s);
	else if (!strcmp(line, "fight")) cmd_fight(s);
	else if (!strcmp(line, "who"))   cmd_who(s);
	else if (!strcmp(line, "quit"))  sess_emit(s, "Goodbye.\n");
	else                             sess_emit(s, "Unknown. Try: login/look/go/say/hide/fight/who\n");
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
	/* monster start */
	mon.room_id = room_valid(start_room) ? start_room : 0;
	mon.cooldown_ticks = 4;
	mon.alive = 1;

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
			list_del_init(&s->player->node);
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

