# mod-playerbots for Conquest of Azeroth

This `coa` branch of mod-playerbots adds bots for the **Conquest of Azeroth custom classes** of the
[jealous-sound CoA server](https://github.com/jealous-sound/azerothcore-wotlk-coa):

- random bots of every CoA class pick a specialization at level 10 (tank / healer / DPS) and spend their talents
  level by level (builds from [ascensionsidekick.com](https://ascensionsidekick.com));
- CoA spells are classified from `Spell.dbc` (heals, HoTs, taunts, AoE, buffs, defensives, dispels, interrupts),
  so tanks taunt, healers heal and dispel, and everyone interrupts;
- gear is weighted with the stats of the bot's CoA specialization;
- `.playerbots coa tank|heal|dps` recruits a bot of that role into your group.

## Current status

> [!WARNING]
> **Bots are tested on levels 1 to 25.** Levels 30 and above are **not supported yet**: many higher-level CoA
> spells have never been cast by bots, and some of them can crash the server. Keep
> `AiPlayerbot.RandomBotMaxLevel = 1` so bots start at level 1 and level up naturally.

My own server runs 24/7 and my bots level up naturally, so I can watch them and fix problems as they reach higher
levels. If too many different spells crash at once, I will let the bots level up (for example to level 40) and fix
a whole level range in one go.

**Found a crash?** Open an issue with the newest `.txt` file from the `Crashes` folder next to `worldserver.exe`,
and tell what the bots were doing (class, level, dungeon or open world) if you know.

## Which versions go together

The core and the module must come from the **same tag**. Each tag matches one jealous-sound release.

| jealous-sound release | Tag (core and module) |
|---|---|
| issue-batch-20260915 (`a8b28faac98d`) | `bots-issue-batch-20260915-v1.1` (latest, same code as the CoA Bots v1.1 zip) |
| issue-batch-20260915 (`a8b28faac98d`) | `bots-issue-batch-20260915` (v1.0) |

The core needs the mod-playerbots core hooks and a small CoA specialization API. Until they are part of
jealous-sound's repository, use [Zyth45/azerothcore-wotlk-coa](https://github.com/Zyth45/azerothcore-wotlk-coa):

- tag `bots-issue-batch-20260915-v1.1` (branch `coa-bots`): the jealous-sound release, the core hooks, the
  specialization API and fixes for crashes that bots trigger often. **Use this one.**
- branch `playerbots-support`: only the core hooks and the specialization API, on the latest jealous-sound `main`.
  These are the changes proposed to jealous-sound in
  [PR #3069](https://github.com/jealous-sound/azerothcore-wotlk-coa/pull/3069).

### Building on the latest jealous-sound `main`

Use the `coa` branch of this module with the `playerbots-support` core branch (tested on `main` `67ce9cb305`, #1498):

```bash
git clone --branch playerbots-support https://github.com/Zyth45/azerothcore-wotlk-coa.git
git clone --branch coa https://github.com/Zyth45/mod-playerbots.git azerothcore-wotlk-coa/modules/mod-playerbots
```

- Since #1498 the worldserver stops at startup unless `DataDir/dbc` holds the CoA client DBC set (download link in
  the CoA Discord's information channel, or `apps/coa-dbc/client_dbc.py`). Players need the matching client patch.
- Bots trigger some existing crashes and a server freeze much faster than players. The fixes are proposed separately
  ([#3072](https://github.com/jealous-sound/azerothcore-wotlk-coa/pull/3072),
  [#3075](https://github.com/jealous-sound/azerothcore-wotlk-coa/pull/3075),
  [#3080](https://github.com/jealous-sound/azerothcore-wotlk-coa/pull/3080),
  [#3083](https://github.com/jealous-sound/azerothcore-wotlk-coa/pull/3083),
  [#3085](https://github.com/jealous-sound/azerothcore-wotlk-coa/pull/3085)). Until they are merged, add them to your
  build for a long-running server with many bots.

## Build

1. Get the core and this module:

   ```bash
   git clone --branch bots-issue-batch-20260915-v1.1 https://github.com/Zyth45/azerothcore-wotlk-coa.git
   git clone --branch bots-issue-batch-20260915-v1.1 https://github.com/Zyth45/mod-playerbots.git azerothcore-wotlk-coa/modules/mod-playerbots
   ```

2. Build and install the server as usual:
   [AzerothCore installation guide](https://www.azerothcore.org/wiki/installation) and
   [mod-playerbots installation guide](https://github.com/mod-playerbots/mod-playerbots/wiki/Installation-Guide).
   Only `mod-ascension-compat` (already in the core) and `mod-playerbots` are needed.
   On Windows, copy `libmysql.dll`, `libcrypto-3-x64.dll`, `libssl-3-x64.dll` and `legacy.dll` next to
   `worldserver.exe`.

3. Game data: the CoA `dbc`, `maps`, `vmaps` and `mmaps` cannot be extracted from the client with the stock
   extractors. Use the `Data` folder of the jealous-sound CoA repack and point `DataDir` to it.

4. Databases: import the world with `apps/coa-world/world_data.py bootstrap` (see `apps/coa-world/README.md`),
   create an empty `acore_playerbots` database for the `acore` user, then start worldserver once
   (`Updates.AutoSetup = 1`) to create the other tables.

5. Copy `playerbots.conf.dist` to `playerbots.conf` and set `PlayerbotsDatabaseInfo`.

## Recommended settings

| Setting | File | Why |
|---|---|---|
| `CharacterCreating.Disabled.ClassMask = 2047` | worldserver.conf | random bots are created with CoA classes only |
| `MapUpdate.Threads = 8` (half your CPU threads) | worldserver.conf | hundreds of bots need several map threads |
| `AiPlayerbot.MinRandomBots = 200` / `MaxRandomBots = 200` | playerbots.conf | about 7 GB RAM for 200 bots |
| `AiPlayerbot.RandomBotMaxLevel = 1` | playerbots.conf | bots start at level 1 and level up while playing (default: random levels up to 80) |
| `AiPlayerbot.BotActiveAlone = 60` | playerbots.conf | more bots stay active away from players |
| `AiPlayerbot.GroupInvitationPermission = 2` | playerbots.conf | every bot accepts group invites |
| `AiPlayerbot.BotTextLocale = 2` | playerbots.conf | bot chat in French (-1 client locale, 0 English, 3 German, 6 Spanish, 8 Russian) |
| `AiPlayerbot.CoaSpecRotations = 1` | playerbots.conf | bots follow the authored rotation of their specialization on top of the automatic spell choice (default: 0) |
| `Appender.CoaBots=2,4,1,CoaBots.log,a` and `Logger.playerbots.coa=4,CoaBots` | worldserver.conf | optional: every 10 minutes, which CoA spells bots cast and why casts fail |

Known issue: `mod-aoe-loot` crashes the CoA release at the first bot login (its module string is missing from the
release database). Leave it out or import its SQL.

## Commands

Everything below is typed in the game chat. A bot has to be **in your group** to answer most whispers, and a
trailing `?` shows instead of changes: `co ?` lists, `co +name` adds, `co -name` removes.

| Command | What it does |
|---|---|
| `.playerbots coa tank\|heal\|dps` | recruits the nearest free CoA bot of that role into your group, raised to your level |
| `.playerbots bot add\|remove <name>` | takes control of a bot, or sends it away |
| `.playerbots bot self` | drives your own character with the bot AI |
| `.playerbots rndbot teleport` | sends every random bot to a place that fits its level, instead of waiting for the automatic move |
| `.playerbots rndbot stats\|grind\|init\|levelup\|revive\|refresh` | state, send hunting, re-roll level/gear/talents, level up, revive, restore |
| `.playerbots rndbot reload` | re-reads `playerbots.conf` without a restart |
| `/w <bot> co ?` | its combat strategies: position (`close` / `ranged`), the CoA classifier, and its authored rotation |
| `/w <bot> talents` / `talents spec list` / `talents spec <name>` | its specialization, the list with roles, or switch (`tank`, `heal`, `dps`, `random` work too) |
| `/w <bot> stats\|spells\|equip\|autogear\|upgrade` | its state, spells and gear |
| `/w <bot> follow\|stay\|flee\|attack\|formation\|rti` | movement and group position |
| `/w <bot> help` | the full list |

Things that surprise people:

- `co` alone answers nothing, it needs `co ?`.
- Out of a group, a bot ignores most commands.
- Bots do not level from 1: each one is given a random level on its first login, with the gear and talents that go
  with it, and only moves to a zone of its level on the next automatic teleport (up to five hours).
- The bot pool is larger than the number online: the module rotates characters.

## Updating to a new jealous-sound release

The bot work is a few commits on top of the release in each repository. Cherry-pick them onto the new release
revision (`targetSourceRevision` in the release `UPDATE.json`), rebuild, and tag `bots-<release id>`.

## Credits

- [jealous-sound](https://github.com/jealous-sound/azerothcore-wotlk-coa) for the Conquest of Azeroth server
- [AzerothCore](https://github.com/azerothcore/azerothcore-wotlk) and
  [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots)
- [ascensionsidekick.com](https://ascensionsidekick.com) for the CoA specialization roles and talent builds
- [steviecraycray](https://github.com/steviecraycray) for the authored per-specialization rotations, the gear
  weights and weapon shapes, and several fixes the nine base classes benefit from as well

Same license as mod-playerbots (GPL-2.0).
