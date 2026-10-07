# Engine bindings: how they work, how to fix them, how to find the missing ones

Mario Mode needs a few Spider-Man 2 engine functions and structure offsets. None of them are hard-coded. They live in `bindings.ini`, which ships in the mod's resources. You can override any entry from `<game>\sm2mario\bindings.user.ini`, which Overstrike never deletes. That means a game patch can be handled without recompiling.

## Format

```ini
[GetActor]
pattern = 44 8B 01 41 8B D0 C1 EA 14 ...   ; hex bytes, ?? = any byte, ** = displacement to resolve
resolve = direct                            ; direct | call (E8 rel32) | rip (disp32 of mov/lea/add)
offset = 0                                  ; added after resolving
```

This is the same pattern syntax as LDD565's SM2ScriptTemplate, so signatures shared in the modding community can be pasted in unchanged.

- `resolve = rip`: reads the 32-bit displacement at the first `**` and returns `instruction end + displacement`. Use it for globals referenced with `mov [rip+X], rax`, `lea rcx, [rip+X]` and so on.
- `resolve = call`: the same, for an `E8 rel32` call target.

At start-up the log lists every binding as `ok`, `--` (optional, missing) or `FAIL` (required).

**Function bindings are validated.** For each one, the log prints the 16 bytes before the match and the 24 bytes after it (`GetActor ... CC CC | 44 8B 01 ...`). The mod then checks that the address really is a function start. Functions in the game are 16-byte aligned and padded with `int3` (`CC`), so it applies these rules:

| Situation | What the mod does |
|---|---|
| The match is aligned, or comes right after `CC` padding | Uses it as is. |
| The pattern matched a few bytes into a function, and only prologue instructions (`push rbx`, `sub rsp, 28h`, `mov [rsp+8], rbx`, …) separate the two | Uses the real start and logs a warning. Current builds need this for `TransformSetPosition`. |
| Anything else | Refuses to call it and logs an error. |

If a future build really does use unaligned functions, add `validate = false` to that binding's section in `bindings.user.ini`.

**Every call into game code runs inside a crash guard.** If a function crashes, the guard catches it and that binding is switched off for the session. For `SetPosition`, the mod falls back to writing the position directly. The log gets a `... crashed ...` line with the faulting `module+offset`. If a pattern matches more than once, the log warns. The mod then uses the first match, so make the pattern longer.

## What exists and what it's for

| Section | Required | Used for |
|---|---|---|
| `HeroSystem` | yes | the global hero system; its handle at `+0x14` resolves to the hero actor |
| `GetActor` | yes | `Actor* GetActor(ActorHandle*)`. Its body also gives the actor pool: `pool_base_disp` and `pool_count_disp` are the offsets of the `add rcx,[rip+X]` / `cmp r8d,[rip+Y]` displacements inside it. |
| `TransformSetPosition` | no | moves the hero with Mario (falls back to writing the floats; see `[Hero] PositionWrites`) |
| `TransformHide`, `TransformUnhide` | no | hide Spider-Man the engine's way (`[Hero] HideHero = engine`, the default): every render pass skips him. Since 0.5 `Unhide` is also hooked: while Mario stands in, the game's own calls to it on Spider-Man are refused (`[Hero] KeepHidden`) |
| `TransformSetMatrix`, `TransformSetMatrixEx`, `TransformSetMatrixEx2`, `TransformMarkDirty` | no | with `TransformSetPosition`, the engine's transform setters. They're hooked so the game's character controller can't move Spider-Man off Mario between frames (`[Hero] HoldDuringFrame`). Moves farther than `HoldRadius` are the game's own teleports and go through. Since 0.5 the same hooks place the camera (`[Camera] Override`, see [The camera](#the-camera)). |
| `CameraTarget` | no | the follow camera's target (`Camera2::CameraTarget`, found by class name - see below). Its position getters are hooked so the camera follows Mario up and down. |
| `PedestrianInteract` | no | the AI behaviour of a pedestrian asking for a picture (`BehaviorPedestrianInteract`, found by class name). Its activation is hooked so Mario can pose. |
| `ComponentRegistry`, `GetComponentInfo`, `GetComponent1/2` | no | the hero's Health component (incoming damage), and the type information enemies' components are matched against. Enemies are found with a pure memory walk of their component list, so no game code ever runs on unvalidated actors: a component matches a type by name or through its base types, as the game's own lookup does (`component_info_parents`, `_parent_count`, `_flags` in `[Layout]`) - enemies' `BotHealth` is a `Health`. |
| `[Layout]` | — | structure offsets: actor fields, transform matrix/position, Health max/current, pool stride. An actor's handle is `(serial << 20) \| slot index`, with the 16-bit serial at `actor_serial` (`+0x8`) and the slot's own index at `actor_index` (`+0xC`), as `GetActor` checks them |
| `Camera` | no | a direct path to the camera. Without it, a memory scan finds the camera. |
| `PhysicsRaycast`, `PhysicsFrame`, `CollRequestInit`, `CollRequestRelease`, `CollRequestIgnoreActor`, `QueryResultActor`, `PhysicsQueryPool` | no | Mario's collision from the game's own physics (`[Collision] Source = physics`, the default since 0.4), and telling when the game is paused. See [The game's physics](#the-games-physics-04). Without them, collision comes from the rendered world (0.3's way) and Mario doesn't pause with the game. |
| `DamageSphere`, `DamageActor` | no | Mario's hits through the game's damage system, so enemies react the game's way (`[Combat] UseGameDamage`): to the enemy he hit, by its handle (`DamageActor`), or what a sphere touches. Without them, hits change health directly. |
| `PhotoMode` | no | the game's photo mode (`PhotomodeSystem`, a global object): whether it's open, and the selfie copy of Spider-Man it spawns. See [Photo mode](#photo-mode-05). Without it, Mario still holds still in photo mode (it pauses the game), but a selfie shows Spider-Man. |
| `HideHero` | no | a visibility flag for `[Hero] HideHero = write` (fallback if the engine hide isn't found) |
| `[Shaders]` | — | extra shader hashes for finding the shadow passes after a game update (see below) |

## Bindings found by class name

Some engine functions are virtual functions of a class. The game is built with run-time type information, so the class's vtable can be found by its decorated name. That survives most game updates better than a byte pattern does:

```ini
[CameraTarget]
class = .?AVCameraTarget@Camera2@@     ; the type name the compiler stored
get_position = 10                      ; vtable slot (0-based)
check_get_position = 48 8B 49 10       ; must appear in the function's first 48 bytes
```

The mod finds the type descriptor with that name, the complete object locator that points at it, and the vtable whose slot -1 points at the locator. Then it reads the slot. The `check_` pattern guards against a reshuffled vtable after an update. If it doesn't match, the log says `doesn't look like it did - not hooked` and only that feature is lost.

To fix one after an update, open the exe in IDA or Ghidra (both show RTTI vtables by class name), find the function that does the same job in the new vtable, and put its slot and the start of its code in `bindings.user.ini`.

| Section | Slots | What they do |
|---|---|---|
| `CameraTarget` | `get_position` (10), `get_matrix` (11), `get_track_position` (18); `actor_offset` 0x10 | where the camera's target actor is, and the joint it tracks |
| `PedestrianInteract` | `enter` (1); `actor_handle_offset` 0x48 | the behaviour's activation; the pedestrian's actor handle in it |

## Fixing a binding after a game update

1. Open both the old and new `MarvelsSpiderMan2.exe` in IDA, Ghidra or x64dbg.
2. Find the old function using the pattern from `bindings.ini` (in IDA: Search → Sequence of bytes). Note what makes it recognisable: strings it references, the functions it calls, its callers.
3. Find the same function in the new exe by those landmarks. Usually only offsets inside it moved.
4. Build a new pattern from its first ~20–40 bytes, replacing anything address- or offset-like (rip displacements, call targets, struct offsets that might move) with `??`. Check that it matches exactly once.
5. Put it in `bindings.user.ini` under the same section name and restart.

Structure offsets (`[Layout]`) rarely change in a patch. If health or positions read as garbage, press **Ctrl+F9** and compare the dump against the game in Cheat Engine.

## Shader hashes for the shadow passes (`[Shaders]`)

Mario is put into the sun's shadow maps only where the game re-renders them in the same frame. To tell those passes apart, the mod recognises the game's shadow shaders by their hash: the 16-byte checksum every compiled shader container carries, written as 32 hex digits.

| Key | Shaders |
|---|---|
| `CasterVS`, `CasterPS` | draw objects into a shadow map |
| `CacheCopyPS` | copy a cached shadow region into the working map (that region is refreshed this frame, so Mario may join it) |
| `CacheMovePS` | write the cache itself (Mario must stay out of it) |

The current build's hashes are built in. If an update recompiles these shaders, Mario keeps everything except his shadow. When he first appears, the log's frame report then lists every depth-only shader that drew but wasn't recognised:

```
depth-only pipeline that isn't a known shadow caster (27 draws): VS 9c1f... PS 5e0a...
```

Put the hashes in `bindings.user.ini`, comma-separated, and restart:

```ini
[Shaders]
CasterVS = 9c1f...
CasterPS = 5e0a...
```

The draw counts help: the casters draw many objects per shadow region, while a cache copy is one full-screen triangle per region.

## The game's physics (0.4)

Since 0.4 Mario collides with the game's own collision world, the Havok world Spider-Man moves through, asked with ray casts. These bindings were found by analysing the current `Spider-Man2.exe`. All of them are optional: without them collision comes from the rendered world (0.3's way), and the log says which one failed.

| Section | What it is |
|---|---|
| `PhysicsRaycast` | `NQueryResult* CastRayImmediate(PhysicsSystem*, const CollRequest*, const Vec3* from, const Vec3* to, const char* tag)`, found where the game calls it (`resolve = call`). It is a small thunk (`add rcx, imm32 ; jmp`): the mod reads the query system's offset from it. `physics_disp`: the `PhysicsSystem*` global in the match. |
| `PhysicsFrame` | the physics system's end of frame, on the game's main thread. The mod hooks it: Mario's rays are cast right after it, a few dozen per frame (`[Collision] RaysPerFrame`). It skips its work while the world is paused, and the flag, timestep and time scale it tests (`paused_flag_disp`, `timestep_disp`, `timescale_disp`) are how Mario pauses with the game. |
| `CollRequestInit`, `CollRequestRelease` | a `CollRequest`'s constructor (by preset: 3 = the hero's movement, 17 = water, 9 = damage) and destructor. `request_max_hits` (under `PhysicsRaycast`) is the offset of its 16-bit hit count. |
| `CollRequestIgnoreActor` | `CollRequest::AddIgnoreActor(Actor*)`: Mario's rays don't see Spider-Man. |
| `QueryResultActor` | the actor a hit belongs to, if any. People (actors with `[Collision] NotSolidComponents`) aren't solid for Mario. Things that moved in the last 2 s are asked about again often. |
| `PhysicsQueryPool` | the per-frame pool of query results (`counter_disp`, `limit_disp`). Mario's rays only use what the game leaves free (`[Collision] QueryHeadroom`). |
| `DamageSphere` | `DamageRequest* DamageSystem::DamageSphere(DamageSystem*, const Vec3& centre, float radius, const CollRequest&)`. The `DamageSystem` object is taken from a call to it (`lea rcx, [rip+X] ; call`). The `DamageRequest`'s fields are given as `offset, bit` (the bit it sets in both field masks at `masks`). |
| `DamageActor` (0.5) | `DamageRequest* DamageSystem::DamageActor(DamageSystem*, const ActorHandle*)`: a request for one actor (type 1, its handle at `+4`). Mario's hits on an enemy he reached use it; the sphere is the fallback and is still how people in reach are asked to react. The request's `knockbackAmount` (stagger build-up) is 100 per hit at ATTACK STRENGTH 1, as the game's own hits use 10, 100 and 1000. |

A hit record is 64 bytes: the fraction along the ray at `+0`, the normal at `+0xC`, the position at `+0x18` and the physics material at `+0x3A`. The result holds a pointer to the records at `+0` and their count at `+0xA`. The materials (`kAsphalt`, `kConcreteDirty`, `kGrass`, `kWoodCreaky`, `kWaterDeep` and 85 more, from the exe's reflection table) pick SM64's surface and footstep sound for each floor (`src/world/surface_types.cpp`).

**How Mario uses them** (`src/game/sm2_physics.cpp`, `src/world/physics_world.cpp`):

- Columns of multi-hit rays straight down on a 0.5 m grid give every floor, roof and awning around him, with their materials.
- A ring of horizontal rays at knee, hip and head height in 32 directions gives the walls, posts and railings near him, exactly where the game has them.
- A water-only ray finds the river and the sea under him.
- The rays are cast on the game's thread right after its physics frame, a few dozen per frame. The ground under Mario is asked first, then the ring, then the rest of the area, and old answers are refreshed (often near Mario, fast for anything that moves).
- Until the game has answered for a spot there is no floor there at all, so SM64 holds Mario at the edge of what's known for the frame or two it takes, instead of letting him drop through.

Every call into these functions runs inside the crash guard. If one faults, ray casting stops for the session and Mario's collision switches to the rendered world, with a `game physics: ... crashed` line in the log. If the hooked frame never runs, or no ray gets an answer within 3 s of pressing M, the same happens (`collision: ... - Mario collides with what the camera sees instead`).

**After a game update**, the log's start-up block prints each binding with the bytes around its match. Fix a pattern the usual way (above). The thunk, the frame function and the pool allocator are short and distinctive: search for `48 81 C1 90 00 00 00 E9` (the thunk), and for the frame function's test of a byte flag followed by `vmulss` of two floats. `[Collision] Source = world` brings back 0.3's collision at any time.

## The camera

### Placing the game's camera (0.5)

Spider-Man 2's follow camera chases its target through springs tuned for swinging, so it trails Mario's runs and jumps (0.4's log measured 0.55-0.6 s). Since 0.5 the mod places it itself (`[Camera] Override`), through the transform setters it already hooks:

1. **Finding it.** The camera is an actor whose transform the game writes each frame. While searching, every transform written within 1.5 m of where the frame is rendered from, with a rotation row along the view, is a candidate; the one written in at least 60% of 40 frames is the camera's. If the frame keeps being rendered from elsewhere afterwards, it's dropped and the search runs again (three tries).
2. **Learning the framing.** While Mario stands still and the camera has settled, the camera's offset from him in its own rotation rows (how far back, how high, how far to the side) is learnt.
3. **Placing it.** From then on each write keeps the game's rotation (the player's stick and mouse, its own turns) and gets the position `Mario + offset` in those rows, eased in and out over 0.35 s. Its height follows Mario through `[Camera] VerticalSmoothing`.
4. **Walls.** Three rays from Mario's chest to the camera's spot (and one either side) go to the game's physics with its camera query (`[Camera] CollisionQuery = 11`, `kCamera`). The camera comes in front of what two of them hit (one alone is a post it can see past) and eases back out when the way is clear. Without the game's physics, the game's own camera distance limits it instead.

The log says `camera: the game's camera is the transform written each frame where it renders from ...`, then `camera: placed by the mod - ...`. If no transform qualifies, it says so and 0.4's way (the camera's target led along Mario's jumps, `[Camera] FollowLead`) carries on.

### Finding the rendered view

The scanner looks for a rotation + translation matrix 0.6–14 m from the hero, with one axis pointing at the hero's chest. It also looks for a projection matrix whose aspect ratio matches the swap chain, which gives the live FOV. It then tracks the best candidate every frame and rescans if it's lost.

To make it instant and robust:

1. Press F10 when Mario is drawn correctly. The overlay shows `cand N/M @<address>`.
2. In Cheat Engine, pointer-scan that address. Pick a chain from a static `MarvelsSpiderMan2.exe+offset` base that survives a game restart.
3. Find the code that reads that base (`mov rax,[rip+X]` / `lea`) and make a pattern for it.
4. Fill in `[Camera]`:

   | Key | Value |
   |---|---|
   | `pattern`, `resolve` | as above |
   | `pointer_chain` | the offsets to dereference |
   | `matrix_offset` | where the matrix sits in the final object |
   | `layout` | `rows` or `columns` |
   | optional: `fov_offset`, `fov_units` | where the FOV float is and its units |

The calibration cubes (Ctrl+F11) are the quickest check: red at the hero's feet, green at his head, a white bar along SM64's +X. If they stick to Spider-Man while you orbit the camera, the camera binding is right.

## Photo mode (0.5)

`[PhotoMode]` finds the game's `PhotomodeSystem`, a global object, where its camera-mode switch passes it to the selfie swap: `cmp ebx, 3 ; jne ; lea rcx, [rip+X] ; call` (mode 3 is the selfie camera; `resolve = rip` takes `X`). From the code that uses it:

| Key | What it is |
|---|---|
| `active_offset` (`0xCE9`) | a byte the system's activation (vtable slot 20) sets to 1 and its deactivation (slot 21) clears: photo mode is open |
| `doppelganger_offset` (`0x884`) | the actor handle of the copy of Spider-Man the selfie mode poses (it's placed on the hero's transform, and the hero is swapped out for it) |
| `doppelganger_head_offset` (`0x88C`) | its head, a separate actor |
| `selfie_phone_offset` (`0x888`) | the phone it holds |

The mod only reads these: Mario freezes while the flag is set (even if the game's physics keeps running), and the three actors are hidden with `Transform::Hide` (and kept hidden by the `Unhide` hook) while Mario stands in. After an update, search for the switch's `83 FB 03 75 0C 48 8D 0D` and fix the pattern; the offsets appear in the same functions (`mov byte ptr [rdi+0CE9h], 1` in the activation).

## Hiding Spider-Man

Since 0.2 the default, `HideHero = engine`, calls the game's own `Transform::Hide` and `Transform::Unhide`. These set and clear the transform's hidden bit (`0x20` in the flags at `+0x5C`) and mark it dirty, so every render pass skips Spider-Man. That includes his shadow and reflections. Their signatures are `[TransformHide]` and `[TransformUnhide]`.

`Unhide` is the only code in the game that clears that bit, and the game calls it on its own (cutscene cuts, swaps, some menus): 0.4 showed Spider-Man then. Since 0.5 it's hooked, and while Mario stands in, calls on Spider-Man's transform are refused (`[Hero] KeepHidden`; the log counts them and names the callers).

If they stop resolving after an update, the F10 hero line says so. A visibility flag written directly is the fallback:

- The **First Person Camera** mod hides Spider-Man's mask/head. Its author credits LDD565 for showing how, so that technique is a good lead.
- Per the template, the actor's `+0x00` pointer doubles as its `ModelInst`. Look for a flag or LOD/visibility byte near it.
- Once you find a byte that hides the model when toggled in Cheat Engine, configure it:

```ini
[HideHero]
base = transform        ; transform | actor
offset = 0x...          ; byte offset from that base
type = u8               ; u8 | u16 | u32 | f32
hidden_value = 1
shown_value = 0
```

Then set `HideHero = write` in `sm2mario.ini`.

## Live actor test

Enemy discovery walks the actor pool. Free slots are skipped with pure memory checks: a readable transform, a non-zero handle serial (`actor_serial`, which the mod also reads out of `GetActor`'s own code, `[GetActor] serial_disp`), the slot's own index at `actor_index`, and a sane component list. No game function is ever called on them.

Leave `actor_live_mask` at 0: the template's actor "flags" at `+0x8` (bit 0 "Enabled", bit 1 "Spawned") turned out to be the handle's serial, so a mask over them doesn't mean "live".
