# Kernel Quests — Monster Module

This project hosts experiments for the Monster caretaker kernel module and its accompanying client. The module exposes `/dev/monster`, letting helpers guide the creature through its rooms, state changes, and events.

![Monster Overview](docs/images/monster-overview.png)

## Structure
- `quests/monster/monster_main.c` – core kernel module implementation.
- `quests/monster/monster_client.py` – interactive userspace client.
- `quests/monster/tests/` – client unit tests (`python -m unittest discover quests/monster/tests`).

## Getting Started
1. Build the module with `make -C quests/monster`.
2. Insert it with `sudo insmod quests/monster/monster.ko`.
3. Connect using `./quests/monster/monster_client.py`.
