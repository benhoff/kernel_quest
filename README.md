# Kernel Quests — Monster Module
Learn kernel programming the fun way — by playing inside it.

![Monster Overview](docs/images/monster-overview.png)

## What Is Kernel Quests
Kernel Quests is an experimental playground for exploring how the Linux kernel really behaves — not through dry academics, but through tiny inhabitants you build, load, and befriend.

Each quest is a self-contained kernel module that turns low-level concepts (character devices, interrupts, memory management, and more) into a playful arena. You don’t just read about mechanics; you march through `/dev`, trigger events, and duel—or dance—with the kernel itself.

## Meet the Monster Module
The debut quest, Monster, is a caretaker sim that lives entirely inside the kernel. It exposes `/dev/monster`, a window into the creature’s world where userspace helpers can:
- Guide the critter through interconnected rooms.
- Trigger state changes and react to kernel-driven events.
- Watch emergent behavior unfold from tightly scoped primitives.

Think of it as a hybrid between a game and a systems-engineering dojo. While you’re feeding the beast and cleaning its room, you’re also wielding:
- Kernel ↔ userspace comms
- Character device interfaces
- Event loops and state machines
- Scheduling, timing, and synchronization practices

## Lifecycle & Progression
- The monster grows through five lifecycle stages — Hatchling → Growing → Mature → Elder → Retired — each announced over `/dev/monster` and exported in sysfs.
- Every stage unlocks fresh commands; attempt a future ability early and the kernel nudges you with a friendly `[TIP]` about the required stage.
- Stage transitions rebalance spawn rates and enable spicier events (glitch storms wait until Elder), keeping early play sessions calm.
- Each milestone broadcasts a `[QUEST]` goal (tick targets and stability expectations) so new helpers always know the next objective.
- The client’s bottom toolbar mirrors stage/command availability live, and logs the latest quest hint.

## Project Structure
- `quests/monster/monster_main.c` — kernel-facing entry: module params, workqueue, misc device glue.
- `quests/monster/monster_game.c` — gameplay engine, state machine, command handlers, broadcast logic.
- `quests/monster/monster_game.h` — shared types/API so the kernel layer can drive the game loop.
- `quests/monster/monster_client.py` — interactive userspace client.
- `quests/monster/tests/` — pytest suites: fast client/unit coverage and optional `/dev/monster` integration checks.
- Sysfs: `/sys/class/misc/monster/status` and `/sys/class/misc/monster/helpers` expose live state snapshots for tooling.

## Getting Started
1. Build the module: `make -C quests/monster`
2. Insert it into the kernel: `sudo insmod quests/monster/monster.ko`
3. Connect to your monster: `./quests/monster/monster_client.py`

### Client Tips
- Use `--theme minimal` for a bare-text view or stick with the default HUD theme for dynamic stage/quest/toolbars.
- The client understands local helpers: `/summary`, `/filter ±tip`, `/filters`, `/help`.
- Command autocompletion adapts to the monster’s current stage; the bottom toolbar highlights unlocked verbs and quest goals.

### Testing
- Run the fast client tests: `python -m pytest quests/monster/tests/test_monster_client.py`
- With the module loaded (`sudo insmod quests/monster/monster.ko`): `python -m pytest quests/monster/tests/test_monster_device.py`
- Or run everything (integration tests auto-skip if `/dev/monster` is absent): `python -m pytest quests/monster/tests`

## Why Build Games in the Kernel?
Should you build games in the kernel? Probably not.
Should you use a monster to understand devices, concurrency, and memory? Absolutely.

Games make the invisible visible. Every quest turns opaque kernel machinery into something you can see, poke, and play with. The result: intuition that sticks.

## Join the Adventure
Add a new quest. Design a creature that teaches semaphores. Stage a DMA boss fight. Or just wander the rooms until the kernel nips you back.

Kernel Quests — because learning systems should feel alive.
