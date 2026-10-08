# Mario Mode — libsm64 in Marvel's Spider-Man 2 (PC)

Press **M** and Spider-Man is replaced by Super Mario 64's Mario: the real SM64 movement code (via [libsm64](https://github.com/libsm64/libsm64)), his model, animations, voice and sound effects from **your own** Super Mario 64 ROM, running around the game's New York. He collides with the game's own collision world, his footsteps match what he walks on, and his punches, kicks, dives, slide kicks, stomps and ground pounds hit enemies through the game's own damage system. Their hits knock Mario back and cost health wedges. Press M again to turn back into Spider-Man where Mario was standing.

It installs as an Overstrike `.script` mod.

---

## Install

1. Get [Overstrike](https://www.nexusmods.com/marvelsspiderman2/mods/1) (needs the .NET 7 Desktop Runtime).
2. In Overstrike, open **Settings** and tick **Enable .script support** for your Spider-Man 2 profile.
3. **Add mods** (green +) and pick `sm2mario-0.6.1.script` (remove older versions first). Tick it, keep `.script` mods at the top of the list, then press **Install mods**.
4. Launch with **Run modded game** in Overstrike. Launching from Steam/Epic won't load scripts.
5. On the first launch the mod creates `<game folder>\sm2mario\`. Put your **Super Mario 64 (USA)** ROM in that folder:
   - `.z64`, `.n64` and `.v64` all work, under any file name.
   - The ROM is checked by SHA-1. Japanese, European and Shindou ROMs are recognised and rejected with a clear message.
6. Restart the game. In the city you'll see *"Mario Mode ready - press M"*.

Your files in `<game>\sm2mario\` survive Overstrike reinstalls. Overstrike deletes and recreates the game's `scripts` folder every time you press Install, which is why nothing of yours lives there. The files are:

| File | What it is |
|---|---|
| your ROM | |
| `sm2mario.ini` | your settings, copied there on first run |
| `bindings.user.ini` | your binding overrides |
| `sm2mario.log` | the log |

If you're upgrading, your old `sm2mario.ini` is kept, and nothing in it needs editing. Missing keys use the new defaults, and the defaults that changed move with them: 0.4's `RespawnAfterFallSeconds = 12` becomes 30 (a jump off the tallest towers falls for about 20 s), 0.3's `[Collision] Source = world` becomes the game's physics, `KeyPunch = LMB, E` becomes `LMB` (E is the game's interact key), `Specular = 0.03` becomes 0.02, a `Cap = ...` line becomes the three cap switches, and 0.5's `RaysPerFrame = 24`, `RoofSearchHeight = 20` and `VerticalSmoothing = 0.15` become 32, 8 and 0.05. The log lists each one, once. A setting you choose after that stays. The F8 menu writes its settings back into this file. 0.6.1's new `[Camera] Distance` starts at 1.5× for everyone, until you change it.

## Controls

| | Keyboard / mouse | Controller |
|---|---|---|
| Become Mario / Spider-Man | **M** | optional `PadToggle` combo |
| Move | W A S D (hold **Left Ctrl** to walk) | left stick |
| Jump (SM64 **A**) | Space | A / Cross |
| Punch, kick, dive (SM64 **B**) | Left mouse | X, B / Square, Circle |
| Crouch, ground pound, long jump, backflip (SM64 **Z**) | Left Shift, right mouse | LT, RT, LB / L2, R2, L1 |
| Camera | mouse (it aims the game's camera; the mod keeps it behind Mario) | right stick |
| Respawn at the last safe spot | Ctrl+R | |
| Settings and cheats menu | **F8** | optional `PadMenu` combo |
| Pose (wave, peace sign, star dance in turn; in photo mode he holds it) | **P** | optional `PadPose` combo |
| Answer a pedestrian's photo prompt (the game's interact) | E, F | Y / Triangle |

All the SM64 moves work, because it's the real SM64 code: triple jump, long jump, backflip, side flip, wall kick, dive, slide kick, ground pound and crouch slide.

While Mario is active, only his own keys and buttons (and the left stick) are kept from the game, so Spider-Man doesn't also move. Everything else still reaches it: menus, prompts, the d-pad, photo mode, Esc. Mario Mode's own hotkeys (Ctrl+R and the like) don't reach it either. `[Input] BlockOnlyMarioControls = false` brings back 0.3's behaviour, where only `PassThroughKeys` reached the game. When the game pauses (pause menu, photo mode, loading), Mario pauses with it.

## Settings menu and cheats

**F8** opens Mario's menu, at any time. Up/down picks a line, left/right changes it, Enter switches toggles, and Esc or F8 closes it. While it's open, Mario waits and the game doesn't see those keys. Everything takes effect immediately and is saved to your `sm2mario.ini`.

| Setting | What it does |
|---|---|
| MARIO VOLUME | his voice, sounds and cap music |
| ATTACK STRENGTH | 0.25×–4×: the damage and knockback of every attack |
| SHINE | how glossy he looks under the city's lights (`[Render] Gloss`) |
| POSE FOR PHOTOS | pose when people ask for a picture (see below) |
| INFINITE HEALTH | the power meter never runs out |
| WING CAP | triple jump to fly |
| METAL CAP | heavy, invincible, polished metal that reflects the city |
| VANISH CAP | see-through |
| MOON JUMP | hold jump in the air to keep rising |
| BLJ ANYWHERE | the backwards long jump: hold crouch and press jump again while going backwards, and each jump is 1.5× faster (up to SM64 speed 300) |
| CAMERA DISTANCE | 0.5×–3× (5–30 on the pause menu's slider, in steps of 0.1×): how far behind Mario the camera sits, against the game's own distance for Spider-Man. 1.5× to start with |
| CAMERA FOLLOWS JUMPS | the camera stays on Mario, up and down too. Off: the game's own camera, which trails him |

The caps combine, as in SM64: wing and metal together make a metal Mario who flies.

**With [ModSettings](https://www.nexusmods.com/marvelsspiderman2/mods/579) installed**, the same page appears in the game's pause menu as **MARIO MODE**. Both menus show the same values, and either one saves them. Without ModSettings the log says `ModSettings: not installed` and the F8 menu works as usual.

## Photos

Pedestrians walk up and ask Spider-Man for a picture. With Mario out, he turns towards them (and a little towards the camera) and waves. The next request gets a peace sign, then the star dance with its "Here we go!". He poses once he's standing still. Requests while he's busy or in the air wait for up to 5 s, and a pedestrian is only answered once.

When the game's prompt appears over their head, its interact button (Triangle / Y, or E / F on the keyboard) still works for the game, and Mario poses for them again, right away, even mid-run.

**P** strikes the same poses on demand, facing the camera. Moving or pressing a button ends a pose early.

## Photo mode

The game's photo mode works with Mario out:

- **Mario holds still** while you frame the shot, wherever he was (mid-jump too), even though the keys move the game's photo camera. Its lights, time of day, filters, frames, stickers and depth of field all apply to him, because he's drawn inside the game's frame.
- **P strikes a pose and holds it** (wave, peace sign, star dance in turn), facing the photo camera. He has to be standing on something for that.
- **Selfie mode** puts Mario in the picture. The game normally poses a copy of Spider-Man there, holding the phone; the mod hides that copy (and the phone).
- **Mario's HUD stays out of the shot:** no health meter and no messages while photo mode is open. The F8 menu still opens, for the caps, and they go on in photo mode too.
- When photo mode closes, Mario carries on from where he was.

**Note:** the mod swallows M even when Mario is off, so M no longer opens the map. Rebind the map in the game's settings, or change `ToggleKey`.

## Gameplay rules

- **Health:** SM64's 8-wedge meter, top left. You regain a wedge after 8 s without damage.
- **Falling** (`FallDamage`):
  - `capped` (default): a 3-storey drop costs a wedge, a skyscraper drop two, but a fall never kills.
  - `sm64`: vanilla SM64 thresholds.
  - `off`: no fall damage.
- **Enemies:** actors near Mario that have a Health component, filtered by `EnemyNameInclude`/`EnemyNameExclude`.
  - Each hit removes a fraction of the enemy's max health. Ground pound is the strongest, and its landing shockwave hits everyone nearby.
  - The hits go through the game's own damage system (`[Combat] UseGameDamage`), so enemies react the game's way: a punch staggers, a kick knocks back, a slide kick, dive or stomp knocks down, a ground pound knocks down hard and its shockwave pops them up (`*Reaction` keys). If that system can't be used, hits change health directly and Mario pushes enemies back himself, as in 0.3.
  - People in the street that Mario punches or kicks flinch, unhurt (`HitPeople`).
  - Damage the game deals to (hidden) Spider-Man is converted into SM64 damage and knockback for Mario. Spider-Man is then healed back up, so the game never sees him die.
- **Running out of health** respawns Mario at his last safe spot (`OnDeath = respawn`). With `OnDeath = hero`, Spider-Man goes down instead and Mario Mode ends.
- **Fast travel, cutscenes and checkpoints** that move Spider-Man take Mario along (`OnHeroTeleport = follow`).

## How Mario gets into the world

Spider-Man 2 is a deferred renderer. Objects first write what they are into a set of screen-sized buffers (the G-buffer: depth, surface normal, colour, shininess, motion). Lighting, shadows, reflections, fog and anti-aliasing all run afterwards on those buffers. So the mod adds Mario to the G-buffer, and from then on the game treats him like any other object. The details came from static analysis of `Spider-Man2.exe`'s shaders and root signatures:

- **The G-buffer.** The mod watches the game's Direct3D 12 command lists and recognises the G-buffer pass by its pipeline layout:
  - linear depth, motion vectors and two packed 64-bit targets
  - a stencil-tested depth buffer
  - the per-view constants (camera, projection, last frame's projection)
- **Mario's draw.** At the end of a G-buffer pass (it prefers the game's `GBuffer Dynamic` pass), it records one extra draw: Mario, with the pass's own camera constants.
  - His normal, colour, gloss and specular are packed exactly as the game's own materials pack theirs, and motion vectors are written too.
  - Afterwards everything the game had bound is put back, so its next command runs as if nothing happened.
- **Shadows.** Mario goes into the sun's shadow-map regions that the game re-renders in the same frame, with the light's view. Regions served from the shadow cache are left alone, so he never gets baked into a stale cache.
- **Collision** (since 0.4) comes from the game's own physics world: the mod asks it with ray casts, on the game's own thread, right after each of its physics frames (a few dozen rays a frame, leaving most of the game's per-frame query budget to the game).
  - Columns of rays straight down on a 0.5 m grid find every floor, roof and awning around him, with what each is made of: that picks SM64's surface and footstep sound.
  - A ring of rays at knee, hip and head height in 32 directions finds the walls, posts and railings near him, exactly where the game has them.
  - People aren't solid for Mario. Cars and anything else that moves are asked about again every fraction of a second, so a car that drives off stops being solid.
  - Spider-Man is left out of every ray, and a water-only ray finds the river and the sea under Mario, so he swims.
  - Where the game hasn't been asked yet there is no floor at all, so SM64 holds Mario at its edge for the frame or two it takes, instead of letting him drop.
  - If the game's physics can't be used, collision comes from the game's rendered depth, as in 0.2 and 0.3 (`[Collision] Source = world`).
- **Spider-Man** is hidden with the engine's `Transform::Hide`, so every pass skips him: G-buffer, shadows and reflections. The game's own calls to `Transform::Unhide` on him are refused while Mario is out (since 0.5). He still moves with Mario, so the game's camera target follows.
- **The camera** (since 0.5): the game writes its camera's transform every frame through the same engine setters the mod hooks to keep Spider-Man on Mario. The mod finds the one written where the frame is rendered from and gives each write the game's rotation and a position around Mario: the game's own framing (how far back, how high, how far to the side), learnt while Mario stands still, and until then the usual one (4.7 m back, 1.35 m up), times CAMERA DISTANCE (since 0.6.1). Since 0.6 that framing is kept when the game switches cameras, so a new camera is placed as soon as it's found. Three rays to the game's physics with its camera query bring it in front of walls.
- **The game's other cameras** (since 0.6.1). Spider-Man 2 has several cameras and renders from one or another. While the mod places the camera it keeps watching the frames: one rendered from somewhere it didn't put a camera is matched against the transforms the game wrote there, in the frames it was rendered in. A transform seen like that twice within 3 s, in between frames from the camera already placed, and laid out like it, is placed around Mario as well (up to four at once). Every placement says which camera it was, so Mario's draw still finds the one each frame came from. When the game stops writing the first camera and renders from another one the mod places, that one takes over. A camera not written for 10 s is let go. `[Camera] PlaceOtherCameras = false` places only the first, as 0.6.0 did.
- **Mario and his camera, frame by frame** (since 0.6.1). The game writes its camera on its own thread, a moment before or after the mod has made Mario's next frame. Such a write takes Mario's place as it was one frame before it, so the camera moves smoothly whichever way the race goes. Every placement is remembered with the spot it was placed around. When the game draws a frame, Mario's vertex shader picks the placement that frame's view was rendered from, by the view's position, and draws Mario at that placement's spot. So he is always exactly where his camera had him, even when the game draws a frame with a camera from a frame earlier or later. Shadows (another view) use the newest placement. The log says how often each frame found its placement (`camera: Mario drawn where the camera of the view had him ...`). `[Render] AlignToCamera = false` turns this off.
- **Mario's motion vectors across a camera jump** (since 0.6.1). The game's anti-aliasing and motion blur follow each pixel's motion vector. When the camera jumps against Mario by more than a metre from one frame to the next (another camera, a wall bringing it in at once), Mario's motion vectors are worked out from this frame's camera alone, as if it had followed him through the jump (as it does otherwise). They carry his animation and not the jump, which the game's motion blur would otherwise smear him along.
- **Mario's frames** (since 0.6) are numbered with the game frame they are made for, and each frame the game renders draws the one with its own number. If the game records some of its frames before Mario's is ready, every frame draws the one made a frame earlier instead, consistently, and the log says so (`frames:`).

If the frame doesn't look like the one the mod expects (after a game update, say), nothing is injected:

- After 3 s Mario is drawn on top of the frame, the 0.1 way.
- The log gets a **frame report**: every pass the mod saw, what it decided and why.

## What changed in 0.6.1

From your 0.6.0 playtest, its screenshot and its log:

- **The camera sat too close, and nothing changed it.** New: **CAMERA DISTANCE**, 0.5× to 3× the game's own distance for Spider-Man, in the F8 menu, on the pause menu's MARIO MODE page (5 to 30 on its slider) and as `[Camera] Distance`. The camera moves along the same line from Mario, so he stays at the same place on screen, only smaller or bigger. It starts at 1.5×. Walls still bring it in, and the log's `camera: placed by the mod` lines say the distance in use.
- **In flight the camera snapped close and far.** Your log has the answer in its `frames:` lines: `against the camera the mod placed (...): in step 100% (... 992 from elsewhere)`. In that minute 992 of the 3,648 frames weren't rendered from anywhere the mod had put the camera, and other minutes had up to 760 of them. Mario's frames and the camera's were in step in every frame the mod did place. Spider-Man 2 has several cameras and renders from one or another. 0.6 placed only the one its search found first, so the frames from the others came from where the game had put them, further back. Now:
  - While the mod places the camera, it keeps watching the transforms the game writes near it, and near where a frame not from its own placements was last rendered from (carried along with Mario). Each write is stamped with the frame it was made in.
  - A frame rendered from somewhere the mod didn't put a camera is matched against those writes, in the frames it may have been rendered in. A transform the game rendered from twice within 3 s, in between frames from the cameras already placed (the game switching between its cameras, not cutting away to a cinematic one), and laid out like the first, is placed around Mario as well. Up to four at once. When the search first finds the camera, other transforms the game writes in the same spot, laid out alike, are placed with it.
  - Every placement records which camera it was, so Mario's draw finds the one each frame came from, whichever camera that is.
  - When the game stops writing its first camera and keeps rendering from another one the mod places, that one takes over at once. 0.6 noticed only 2 s later, searched for the new camera, and showed the game's own camera in between: your log has 31 of those drops (`the game stopped writing that transform while its view moved on`, `the game renders from somewhere else now`).
  - The camera the frames come from is the one whose framing is learnt, once they've come from it for a second. A camera not written for 10 s is let go.
  - `[Camera] PlaceOtherCameras = false` places only the first one, as 0.6 did.
  - Two smaller things that could also move the camera from one frame to the next are fixed too. **A race:** the game writes its camera on its own thread, a moment before or after the mod makes Mario's next frame. A camera write on another thread than the mod's frames now takes Mario's place as it was one frame before the write, which comes out the same whichever way the race goes, and each frame draws Mario where the placement it was rendered from had him (the shader matches the view to the placements by position). `[Render] AlignToCamera = false` goes back to 0.6's. **Letting go when the game's camera trails far behind:** 0.6 gave the camera back to the game once the game's own camera was more than 25 m from Mario, as it is when he flies or falls faster than it follows. It now keeps placing it while that camera is still chasing him (moving on from its last write, up to 150 m behind), and lets go only when it cuts away.
  - **The camera's height jittered against Mario as he rose or fell.** It eases after his height, and the easing went by a clock read after the mod's work at the start of each frame, which takes longer in some frames than in others. In those frames the easing moved on by a few milliseconds while Mario had moved on by a whole frame, so the camera dipped against him for a frame. It now goes by the moment each frame began, as Mario's own motion does. In the stand-in that took Mario's jitter on screen while jumping from 50–80 px to 16–23 px with the rendered-world collision (whose depth read-back makes that work uneven), and the camera's lag behind his jump from up to 0.6 m to 0.3 m.
- **The pixel glitch.** Your picture shows copies of Mario's cap stacked above his head, in blocky steps, and noise along the lamp post behind him. That's the game's motion blur. It works on blocks of pixels (`CS_MotionBlurDownsampleNghHalf`/`NghQtr`, `CS_MotionBlurGenerate`), each blurred along the fastest motion vector around it. Whenever the view jumped between the near camera (placed by the mod) and the far one (left to the game), Mario's motion vectors said he had moved a long way across the screen in one frame, and the blur smeared him along that. With the other cameras placed, those jumps are gone. And whenever the camera does jump against Mario by more than a metre between two frames (a camera the mod couldn't place, a wall bringing it in at once), his motion vectors are now worked out as if the camera had followed him through it, as it does: his animation, not the jump. The background's are the game's own, as always. That's what the picture looks like, but I can't run the real game, so it isn't confirmed.
- **The log** now also says, every minute:
  - where the frames came from while the mod placed the camera, and what it did about the game's other cameras: `camera: views while placed: N from the mod's placements, M from elsewhere (by how far off; how many matched no transform; how many came from where the game itself had the camera the mod places); the game's cameras placed: up to K at once (another one placed too ..., the first one replaced ..., let go ...)`. If the elsewhere count stays high and they match no transform, the game renders some frames from something the mod can't move, and that's worth sending.
  - `camera: the game renders from another of its cameras too (... m from where the mod put the camera) - the mod places that one around Mario as well`, each time.
  - the game's field of view (`camera: field of view A-B degrees (N changes ...)`), in case the game zooms its camera in flight.
  - frames presented against frames the game rendered (`frames: ... N frames presented, M views read back`). Frame generation would present about twice as many as it renders.
  - which thread the game writes its camera on, against the mod's frames (`camera: in N of the mod's frames the game wrote its camera ... on thread T (the mod's frames on thread P ...)`).

  And `[Debug] TraceCamera = true` logs each of the first 400 frames rendered from somewhere the mod didn't put a camera: where it came from, and the nearest camera the mod placed.

## What changed in 0.6.0

From your 0.5.1 playtest and its log:

- **Mario fell through the floor.** Three ways, all in your log:
  - **Holes under awnings and trees.** The game answers a floor ray with a limited number of hits. A ray starting under an awning, a tree or a fire escape can use them all up before it reaches the ground. 0.5 asked again from where it stopped, but each time that spot was asked about afresh (every 0.4 s near Mario), the new answer replaced the old one, and the ground below the cut-off was gone until the follow-up came back. If Mario was there, he dropped. Now a floor the game has reported stays until the game has been asked about that spot again. Everywhere else a floor missing from one answer is kept, because a ray can slip through the seam between two pieces of floor, and it goes if the next answer misses it too. The floor rays also start 8 m above Mario's feet instead of 20 m (`RoofSearchHeight`), so fewer of their hits go on things overhead, and the game is asked 32 rays a frame instead of 24 (`RaysPerFrame`).
  - **The game's rescue was undone.** After Mario fell through the city, the game tried 230 times to put the hidden Spider-Man back 240 m up. The hero pin let through only moves of more than 250 m, so it undid every one. Moves of more than 25 m up or down are now the game's to make, and Mario follows them.
  - **Respawns where no floor was known yet.** When the game moved Spider-Man somewhere new, Mario respawned there before the game had been asked about the ground, found no floor, and Mario Mode switched itself off ("couldn't respawn (no floor)"). He now stands on a flat floor at that height until the game answers.

  The log now says when and where he falls: `fall:` for a drop of more than 3 m (what was under him where he left the ground, and where he landed), or 4 s in the air, and `collision: the floor under Mario went away` when his floor disappears under him.
- **Mario bumped into things that weren't there.** Where the ground steps up or down between two neighbouring spots, the mod builds a wall. 0.5 also built them from cut-short answers. There the lowest floor found was an awning or a branch, not the ground, so a wall stood from the ground up to it. Steps now come only from spots the game has answered all the way down. Each bump is logged (`bump:`, what Mario was doing and how fast, and the nearest walls with where each came from: the ring of wall rays, with the game's material and the actor that answered, or a step between which floors), so any bump that's left can be traced.
- **The pixelated blur that follows Mario.** It can't be pinned down from the log, so 0.6 fixes what could cause it and logs each one:
  - Mario's data for the GPU went round 4 buffers. If the GPU was still using the one due (with four frames queued up on it), Mario wasn't drawn in that frame. The game's anti-aliasing then blends the frames he is missing from with the ones he's in. There are now 8, and the log counts any frame he's missing from (`frames:`).
  - Each frame drew whichever Mario frame was newest, made a moment before or after the game recorded the frame. Every Mario frame is now numbered with the game frame it's made for, and drawn in that one, so he and the camera placed for that frame match (the log measures it: "in step N%").
  - The stencil mark. The game marks its pixels in the stencil buffer, and its anti-aliasing and upscalers (TAA, DLSS, FSR) treat marked pixels differently (bit 0x80 means "responsive", with no trail). Mario got whatever stencil writes the last draw of the pass he joined had made. 0.6 compares the marks the game draws into the main view in the 150 frames just before you press M (Spider-Man on screen) with the 150 just after (Mario instead). It's the same place a moment apart, so a mark that goes away with Spider-Man is his, not something that only happened to be on screen. Mario gets that mark, and for the other bits whatever every G-buffer draw in his passes writes, as Spider-Man has them (`[Render] MarioStencil = auto`). It decides after the first M (if Spider-Man was on screen for the 150 frames before it) and looks again at each of the next few. Until then Mario gets 0.5's marks. The log says what it found (`stencil:`). `keep` gives no mark, `copy` is 0.5's, and a number sets the whole stencil value (`129` is 0x81). A 0.5 `StencilMatch = false` still means no marks.
- **The camera lagged behind.** Your log shows the mod's camera dropped 8 times. After each drop the game's own camera, with its lag, was back until Mario stood still long enough for the framing to be learnt again. Twice the framing it then learnt came from a camera the game had pulled in or pushed out (1.55 m and 8.23 m behind him):
  - The view the game renders is matched against the mod's placement with room for camera shake and hit effects (0.5 wanted it within 5 cm and dropped the camera in fights).
  - The framing is kept when the camera is lost or changes, and until it's learnt the usual one is used (4.7 m back, 1.35 m up), so the camera is placed as soon as it's found, without waiting for Mario to stand still. Finding it takes 24 frames instead of 40, and of the transforms written in (nearly) every frame it picks the one written nearest the view.
  - Only a framing a follow camera has is learnt (2.2–8 m back, at most 3.5 m up and 2 m to the side).
  - Jumps are followed 3× more tightly (`VerticalSmoothing` 0.15 → 0.05 s).
  - The log says how much of the time the mod placed the camera, and why it let go of it: `camera: placed by the mod N% of the last ... s (found ..., dropped ...: why)`.

## What changed in 0.5.1

- **0.5.0 crashed the game right after start-up** (your log: `unguarded fault ... reading 0x0 at Spider-Man2.exe+0x2cfbbd8`, just after "Mario is ready"). One of the engine functions the mod hooks to keep Spider-Man on Mario, `Transform::SetMatrixEx2`, takes six arguments; the hook passed only five on. The sixth is a pointer the game reads, so it read whatever happened to be on the hook's stack. In 0.4 that was a readable address, by luck (0.4 had the same mistake, and wrote 12 bytes of whatever was there into every object moved that way); in 0.5.0's build it was zero. The hook now passes all six, and `SetMatrixEx`'s four.
- **Why the tests missed it:** the stand-in game had no `SetMatrixEx` or `SetMatrixEx2`, so those hooks were never installed there. It now has both, built like the game's (six arguments, the sixth read), and calls them every frame for the camera, a pedestrian and the character controller; put the old hook back and the stand-in crashes exactly like your game did, at the same place in the mod. Every other function the mod hooks was checked against the game's code for the same mistake: none has it.

## What changed in 0.5.0

- **The camera was still bad:** 0.4 only led the camera's target along Mario's jumps; the game's camera still chased it through springs tuned for swinging (your log: 0.55–0.6 s behind). 0.5 places the camera itself (`[Camera] Override`, see [How Mario gets into the world](#how-mario-gets-into-the-world)): the game's framing and aim, Mario's position, no lag. Its height follows his jumps a little softly (`VerticalSmoothing`, 0.15 s); walls between him and the camera bring it in (`Collision`, the game's `kCamera` query). The game keeps the camera in its pause menu, photo mode and cinematics. `Override = false` brings back 0.4's lead.
- **Attacks didn't connect:** your log has no `combat:` line at all, so no enemy was ever found. Two reasons:
  - enemies' health is the game's `BotHealth`, which derives from `Health`, and the mod only matched the exact name. Components now match through their base types, as the game's own lookup does (`[Combat] HealthComponents`).
  - an actor's handle is `(serial << 20) | slot index`, with the serial at `+0x8` and the slot's own index at `+0xC`. 0.4 read the serial at `+0x10`, so every handle it made was wrong (the people-reaction spheres were the only damage your log shows).
  
  Hits now go to the enemy by handle (`DamageSystem::DamageActor`), not to whatever a sphere touches, and carry a stagger build-up of 100 per hit (the game's own hits use 10, 100 and 1000; 0.4 sent 1). People are recognised by their components, not their names.
- **Spider-Man became visible:** the game calls `Transform::Unhide` on him by itself (your log: "the game showed Spider-Man again", 2,505 times in about a minute); 0.4 hid him again a frame later. 0.5 hooks `Unhide` and refuses those calls while Mario is out (`[Hero] KeepHidden`); the log counts them and names the callers. Photo mode's selfie copy of Spider-Man is hidden too.
- **The torso's blue was white:** libsm64 skipped the first display list of Mario's model, which sets the blue the top of his overalls is drawn in. Fixed in `sm64.dll` (`third_party/libsm64-patches/0002-draw-mario-butt.patch`); checked in the real libsm64 with your ROM: every bib triangle is blue, with every cap.
- **Mario turned invisible standing still:** his draw copied the depth and stencil tests of the G-buffer pass he joined. Your log shows two of those passes test depth *equal*: they only draw where a depth pre-pass already put that exact depth, which Mario never matches. His draw now always tests *greater-or-equal* (reverse-Z) and never fails on stencil. The log names each pass's tests the first time.
- **Invisible bumps when flying or long-jumping:** in SM64, flying into a spot with no floor at all counts as hitting a wall. In the air, the floor under the world (far below) now reaches 40 m past the asked-about area, and a wall whose top no ray found is 8 m tall instead of 20 m (the ring finds the rest as he rises). People and robots also count as people now, not solid obstacles (the actor fix above).
- **Too specular:** 0.4 dimmed the reflections with a specular occlusion so low that the game's lighting took the sun off Mario, which left mostly reflections. 0.5 keeps the occlusion at 0.6 or more (the sun stays), and SHINE starts from the game's plain default material: no gloss and no specular colour at 0, both growing from there.
- **Pedestrian prompts didn't finish:** the hero pin held the (hidden) Spider-Man in place while the game tried to walk him up to the pedestrian and play its side. After you answer one, the game may now move him for up to 8 s (`[Hero] InteractionRelease`), unless Mario walks off.
- **Lava on car roofs (reported after 0.5's first cut):** the game's cars answer the physics rays with material 1, `kAcid`, which SM64 makes lava, so Mario jumped off every car roof burnt. Only the static world's lava and acid burn now; actors' surfaces are plain ground. The log names any actor that reports acid.
- **Photo mode:** see [Photo mode](#photo-mode). It's found by the code that switches its camera modes (`[PhotoMode]`); Mario freezes even if the game's physics keeps running in it.
- **Hardening after an independent review of 0.5's first cut:**
  - If the game had hidden Spider-Man itself when you pressed M (a selfie, a gadget), he could stay invisible after Mario Mode ended. The game trying to show him while Mario is out now counts as the game wanting him back.
  - The camera override no longer stays stuck on a camera the game has stopped using (a new camera after a load, or a cinematic's): it notices, looks again and keeps the framing it learnt. A search that finds nothing is tried again later instead of giving up for the session, and 0.4's camera lead takes over whenever the mod isn't placing the camera.
  - A camera transform the game frees and reuses for something else is never moved, and nothing is learnt from writes that aren't aimed like the view.
  - A prevented crash in photo mode gives the game back its selfie Spider-Man and the pose key.
  - Lamp posts keep their full height when Mario's wall rays only go past them (0.5's first cut could let him jump through their upper part).
  - Mario's depth test follows the game's depth direction instead of assuming it, and where an actor keeps its serial is read from the game's own `GetActor` code.
- **Also:** activating Mario on a wall or a spire where the game has no floor under him no longer switches Mario Mode off straight away ("couldn't respawn"): the first frame without ground under the spawn spin counted as falling out of the world. A fall respawns him after 30 s instead of 12, enough for the tallest towers. The fault at the very end of some logs, as the game closes (`unguarded fault ... Spider-Man2.exe+0x317e527`), is the game calling NVIDIA Streamline's `slShutdown` after `sl.interposer.dll` has already been unloaded. It isn't the mod's, and it happens after the mod has stopped.

## What changed in 0.4.0

- **Mario fell through buildings and walked through walls:** 0.3 built his collision from the depth of the rendered frame, so it only knew what the camera had seen, and only as precisely as the image. 0.4 asks the game's physics instead (see above). It was tested with SM64's real code and your ROM: running and long-jumping into a facade, landing on a car, a ledge and a roof, an 18 m drop onto a roof, walking under a stack of fire-escape decks, people in the way, and the game answering late or with a quarter of the rays.
- **Footsteps** follow the game's materials: stone on asphalt, concrete and metal, grass, wood, sand, snow, and splashes in shallow water.
- **Pausing didn't pause Mario:** the game skips its physics frame while it's paused (pause menu, photo mode, loading screens). The mod watches that frame, and Mario and his sounds stop with it (`[Input] PauseWithGame`).
- **Attacks didn't hit:** enemies now take Mario's hits through the game's own damage system, with its hit reactions (`[Combat] UseGameDamage`, `*Reaction`), and people in the street react too (`HitPeople`).
- **The camera trailed Mario in the air:** the game's camera smooths its target's height heavily, so it rose only 60–75% of his jumps, late. The mod learns how far it trails and leads its target by that much along Mario's jumps and falls (`[Camera] FollowLead`). When the game paused, Mario used to keep running while the camera stood still; he now pauses with it.
- **Menus and prompts were dead with Mario out:** only Mario's own keys and buttons are kept from the game now (`[Input] BlockOnlyMarioControls`). E is no longer a punch key, because it's the game's interact key. The interact button also answers photo prompts (`KeyInteract`, `PadInteract`).
- **Pixelated trail behind Mario:** with steady shadow regions on, Mario's shadow had also been drawn into screen-sized depth buffers. The mod now refuses those as shadow maps (screen-sized targets, depth with colour targets, depth-stencil targets the game redraws every frame), and Mario's motion vectors are cut across teleports and respawns.
- **Too shiny:** Mario's reflections are now damped by specular occlusion as well as a lower reflectance (`Specular = 0.02`). The SHINE slider controls both. The metal cap is now marked with one of the game's reflective material entries (`[Render] MetalMaterial`), so that the game's own reflection buffer (screen-space or ray-traced) is used on him.
- **Caps** are three switches that combine (WING CAP, METAL CAP, VANISH CAP), in the F8 menu, the ModSettings page and the ini.
- **Vanish cap crash:** libsm64 built display-list commands for see-through Mario that its own renderer then read past the end of. That is fixed in `sm64.dll` (`third_party/libsm64-patches/`).

## What changed in 0.3.0

- **Too shiny:** the default gloss is 0.15 instead of 0.45, and a 0.2.0 settings file is updated automatically. SHINE in the F8 menu changes it live.
- **Left and right were swapped:** Mario's mapping now follows the game's own camera handedness. The 0.2.0 log showed the memory-scanned camera had it wrong.
- **The camera didn't follow Mario up:** the game's character controller put Spider-Man back on the ground between frames, and the camera tracked him there. The mod now hooks the engine's transform setters and the follow camera's target (found by run-time type name), so both stay on Mario through the whole frame (`[Hero] HoldDuringFrame`, `[Camera] FollowMarioHeight`).
- **Cars stayed solid after driving off:** the collision now reads the game's motion vectors, so anything moving is never turned into walls. Things that left are forgotten within a few frames, and Mario's collision is rebuilt sooner when something next to him changes.
- **New:** the F8 menu and cheats, the ModSettings page, and the photo poses (see above).
- **Caps** look right in the G-buffer: metal Mario is a real metallic material that the game's reflections light, the wing cap's wings are cut out of their texture (and of the shadow), and vanish Mario is see-through via a dither that the game's TAA blends.
- **Shadows:** if no shadow region qualifies for a few seconds (the real game refreshes them in a way the stand-in didn't), Mario also casts into the regions the game redraws every frame (`[Render] ShadowRegions = auto`).
- **Quitting the game:** the mod now stands down when the game window closes, which should stop the fault some 0.2.0 logs showed on exit. If one still shows up, its log line now names the memory and the call stack involved.

## What changed in 0.2.0

- **In the world:** Mario is drawn into the game's G-buffer and the sun's shadow maps (see above). `[Render] Mode = overlay` brings back the 0.1 drawing.
- **Collision from the rendered world** (`[Collision] Source = world`). It replaces 0.1's flat floor, and it climbs stairs, stands on cars and stops at walls. Safety nets:
  - **Falling through late ground:** if Mario falls through ground that the camera hadn't shown yet (behind a ledge, or at a low frame rate), he's put back on it once it appears.
  - **Off the map:** if he ends up on the safety floor under the map, he respawns.
- **Spider-Man is hidden by the engine** (`[Hero] HideHero = engine`), not by shrinking him. That needs two new signatures, `TransformHide` and `TransformUnhide`, found in the current exe.
- **Handedness** is read from the game's own camera constants. The world is left-handed, so Mario now spawns correctly mirrored straight away.
- **Crash safety:**
  - every D3D12 hook body runs inside the crash guard;
  - a caught fault now also releases any locks the faulting code held, so a prevented crash can't freeze the game;
  - a fault during an injection restores the game's command-list state before carrying on.
- **Diagnostics:**
  - the frame report;
  - the F10 overlay shows the in-world status (G-buffer / shadow / capture counts, last skip reason);
  - `[Debug] TraceCollision = true` logs every Mario tick with the collision around him.

## What's verified (and what isn't)

**In the real game:** 0.2.0 to 0.6.0 ran in your game. 0.6.1's fixes come from your 0.6.0 log and picture: up to 992 of 3,648 frames a minute rendered from somewhere the mod hadn't put the camera (`frames: ... N from elsewhere`), while every frame from where it had was in step with Mario's; the camera dropped and found again 31 times; and the blocky trail above Mario's cap. That log also showed the camera placed by the mod 79–100% of each minute, enemies hit through the game's damage system, and Spider-Man kept hidden. The 0.6.1 log adds, every minute:
- `camera: views while placed: N from the mod's placements, M from elsewhere (...); the game's cameras placed: up to K at once (...)`: whether frames still come from cameras the mod doesn't place, and whether those match a transform it could place.
- `camera: the game renders from another of its cameras too (...)` each time it places one, and `... that one is the game's camera now` when one takes over.
- `camera: field of view A-B degrees (...)` and `frames: ... N frames presented, M views read back`.

**Verified here, without the game** (Linux, Wine 9 and its vkd3d over Mesa's lavapipe):

- **Static analysis of the `Spider-Man2.exe` you sent:** for 0.6.1, the motion blur's passes (`CS_MotionBlurDownsampleNghHalf`/`NghQtr`, `CS_MotionBlurGenerate`), which blur blocks of pixels along the fastest motion around them. For 0.6, how the game uses the stencil: its 39 built-in depth-stencil states, the model draws that mark their pixels "responsive" (bit 0x80) and the pass that writes per-model marks (`ModelStencilWrite`), and what reads bit 0x80 (the TAA's upsample, FSR's reactive mask, PSSR). For 0.5, `GetActor`'s handle check (the serial at `+0x8`, the slot's index at `+0xC`), the component lookup's "derived types too" flag and base-type list (`BotHealth : Health`), `DamageSystem::DamageActor` and its request, `Transform::Unhide`, the camera query presets, and photo mode: `PhotomodeSystem`'s activation and deactivation, the selfie swap that spawns and destroys the copy of Spider-Man, its phone and head, and how the selfie hides the hero. Before that: the physics ray cast, the physics frame and its pause test, the collision requests and hit records, the query pool, `DamageSphere`, and the 90 physics materials. Each pattern in `bindings.ini` matches exactly once in that exe.
- **Unit tests (22,851 checks):** the platform-independent core, under AddressSanitizer and UBSan. New in 0.6.1:
  - CAMERA DISTANCE: the camera moves along the same line from Mario, so he keeps his place on screen; the setting's range and the menu's steps
  - a camera write on another thread takes Mario's place as it was one frame before it, between the mod's last two frames (just before a frame and just after it come out the same), not across a jump, a pause or a hitch
  - the game's own camera chased up to 150 m behind Mario, and let go of when it cuts away; which writes are the camera's (aimed like the view, or moving on from its last write and looking at Mario) and which are left alone
  - nothing learnt from a camera the game only just switched to

  New in 0.6:
  - floors under a fire escape whose rays stop at the slats: the street stays when the column is asked about again and the follow-ups haven't answered yet (0.5 lost it), and is the follow-up's again once they have; a floor missing from one answer is kept that once and gone the second time
  - no wall along the edge of slats, awnings or trees whose rays haven't reached the ground yet (0.5 built a 3 m one), and none once they have
  - the camera: a framing kept when the camera is lost and used at once for another camera with its rows laid out differently; the usual framing while Mario has never stood still; a framing from a camera pulled in to 1.5 m not learnt
  - the stencil census: Spider-Man's own mark found from the frames either side of M (and someone's hair, the river, a menu, a count left out); the river on screen for 80% of the time he was, but not where M was pressed, isn't his; a quick double press of M counts for nothing; Mario gets his mark and, for the other bits, what the passes he's drawn in write (before the static world's, which has more draws); his mark's value wins where they overlap; a mark that only happened to be on screen with him drops out at the next presses of M; a game writing an id per object doesn't flood the log
  - the 0.5 → 0.6 settings migration
  
  New in 0.5:
  - the camera override: learning the game's framing while Mario stands, placing the camera with no lag while he runs (the game's own camera trails by 3.7 m at 8 m/s) and jumps, easing in and out, cinematics far away left alone, learning nothing while paused or from writes not aimed like the view, a broken rotation ignored; walls from three rays (a wall pulls it in at once, a post alone doesn't, it eases back out)
  - the values the game's thread and the mod share, hammered by two writer threads and a reader: never half of one write and half of another
  - a ring ray going past a lamp post doesn't end the post, one going over a long low wall does, all along it
  - Mario's depth test in both depth directions; an actor answering `kAcid` isn't lava, the world's lava is; the 0.4 → 0.5 settings migration
- **Real SM64 physics on that collision (86 checks, your ROM):** as in 0.4 (facades, people, grass, car roofs, ledges, falls, stacked decks, outrunning the rays), and now Mario lands on a car roof that answers `kAcid` and walks on it, while the world's own lava still boosts him.
- **Cheats and poses with the real SM64 code (53 checks):** every cap and combination, moon jump, BLJs, infinite health, every pose, and poses held for a photo.
- **libsm64's own collision with the surfaces the mod builds (40 checks)** and **the rendered-world collision of 0.2/0.3 (71 checks)**, still the fallback.
- **Crash guard (25 checks under Wine).**
- **End to end (100 checks):** the release `sm2mario.dll` and the real `sm64.dll` with your ROM, inside a stand-in game under Wine that copies Spider-Man 2's frame (root signatures, view constants, G-buffer formats and packing, PIX-named passes, a cached sun shadow map, compute lighting) and its engine: the hide and unhide functions, all five transform setters the mod hooks (since 0.5.1 with `SetMatrixEx2`'s six arguments, called every frame), the physics with its own thread, the damage system with `DamageActor`, a component registry, photo mode, and a follow camera that writes its transform every frame through the hooked setters and trails its target through a spring. Mario's place on screen and his motion vectors are read back from the G-buffer in every frame.

  The scripted session: the game hides Spider-Man itself just before M, then keeps calling `Transform::Unhide` on him while Mario is out. Mario punches a thug (whose health is a type derived from `Health`, under a name none of the mod's lists has), answers a pedestrian, runs up the stairs and over a platform into a wall while a car drives across his path, is paused by the game mid-walk, jumps, poses; a wall only the camera's query sees appears between him and the camera; the game's camera switches to a new transform while he jumps; then the F8 menu, the ModSettings page, photo mode (walking its camera, a held pose, a selfie, Mario Mode off and on again in the selfie), a moon jump, and M again. The checks include:
  - Mario is in the G-buffer and lit, Spider-Man hidden; Mario's shadow is in the refreshed shadow region only.
  - The camera: found, placed 4 m behind Mario with no lag while he walks (0.00 m across, 0.03 m back), following his jump, coming in front of the camera-only wall. On screen Mario keeps his place but for his animation (23 px walking, 14 px jumping), and every frame found the camera placement it was rendered from (99%). When the game's camera becomes a new transform, the mod sees the game render from it and places it within a second, and it takes over from the old one (new in 0.6.1; with `PlaceOtherCameras = false` it's lost, searched for and found again, as in 0.6).
  - Spider-Man stayed hidden through every `Unhide` call, and was given back when Mario Mode ended, though the game had hidden him first.
  - The punch reached the thug through `DamageActor`, by his handle, as a melee hit of 100 with a stagger, from Spider-Man; a punch at nobody asked people in reach to react, unhurt.
  - Photo mode: Mario stood still while its keys went to the game, struck the peace sign and held it; the selfie's copy of Spider-Man was hidden, came back when Mario Mode went off, and was hidden again when it came back on.
  - Collision from the game's physics (every binding resolved, thousands of rays on the game's thread only, Spider-Man left out, every request released, the query pool respected, footsteps by material, people not solid); Mario climbed to exactly the platform's height, walked through the car's lane after it passed, stopped against the wall, and caught its top after the moon jump.
  - The game got its interact key and none of Mario's; F8, P and the menu's keys never reached it.
  - Every argument of the game's `SetMatrixEx` and `SetMatrixEx2` got through the hooks (over 500 and 1,000 calls). With 0.5.0's hook put back, the stand-in crashes at start-up just like your game did, at the same place in the mod.
  - The stencil (new in 0.6): the stand-in marks everything in its G-buffer 0x01 and Spider-Man also 0x80, in a depth-only pass of his own like the game's `ModelStencilWrite`. The mod found his mark and gave Mario 0x81, read back from the stencil buffer on all of Mario's pixels, while he walked and in photo mode. With `MarioStencil = copy` (0.5's) those checks fail, as they should: Mario's pixels are 0x01.
  - Mario's frames against the camera placed for them: 100% in step.

  New in 0.6.1, the camera as your log showed it:
  - **The game's other camera** (`E2E_CAMERA_RIGS`): a second camera, 3 m further back, 1 m higher and aimed a degree lower, written every frame, which the stand-in renders from in every other frame for 6.5 s. The mod placed it after its first few frames; from then on the camera's place against Mario held still whichever one the frame came from (it moved 0.18 m from frame to frame, where 0.6.0 moved it 6 m), Mario kept his place on screen (15 px; 0.6.0: 171 px), and his motion vectors in the frames where the camera switched were 5.5 px (0.6.0: 32 px). With 0.6.0's `sm2mario.dll` in the same stand-in, 13 checks fail, those among them. With `PlaceOtherCameras = false` the second camera is left to the game (the view still jumps, so the camera checks in that window fail, as they should), but Mario's motion vectors across each jump are still his animation's: 4 px.
  - **The race:** the stand-in writes its camera on a thread of its own, at a random moment before or after the mod's frame (`E2E_CAMERA_THREAD`). In 10–15% of frames the camera came from the mod's frame before Mario's, as in a real race; every write on that thread took Mario's place as it was a frame before, and on screen he kept his place (23 px walking, 16 px jumping). With `AlignToCamera = false` that check fails (46–56 px).
  - **The game's own camera 50 m behind** (`E2E_CAMERA_FAR`): the mod kept placing it behind Mario in every frame (0.6.0 let go of it).
  - **CAMERA DISTANCE 1.5×** (`E2E_CAMERA_DISTANCE`): the camera 6 m behind Mario and 3.3 m up, walking and jumping.

  Variants: a left-handed screen (100 checks), no ModSettings (89), the rendered-world collision (83), a game whose physics frame never runs and whose component registry answers nothing (78: Mario falls back to the rendered world, hits change health directly, components are found by name), 0.4's camera (`Override = false`, 92), the camera on its own thread (102), the game's camera 50 m behind (101, and 103 with the camera on its own thread too), CAMERA DISTANCE 1.5× (100), the game's other camera (105, and 107 with the camera on its own thread too), and these negative controls: without the pin hooks (its 4 expected failures: the camera doesn't follow), 0.5's stencil marks (`MarioStencil = copy`: its 3 expected failures, Mario's pixels 0x01), `AlignToCamera = false` with the camera on its own thread (1: Mario jumps about on screen), `PlaceOtherCameras = false` with the other camera (3 or 4, depending on which frames the reports catch: the view jumps between the two), and 0.6.0's `sm2mario.dll` with the other camera (13).
- **Smoke tests:** the 0.1 session tests still pass. With no G-buffer or physics in that mock, the mod falls back to the overlay and a flat floor.
- **Reviews:** 0.6.1's changes were reviewed by me only, against 0.6.0's source, not by an independent review as before. 0.2.0's and 0.3.0's independent reviews found 16 bugs, 0.4.0's 10 more, all fixed. 0.6.0's review, of everything changed since 0.5.1, found no crash in the new frame pairing, buffers or camera code. It did find these, all fixed:
  - The stencil census's lock wasn't one the crash guard releases, so a fault caught while it was held would have frozen the game instead of switching Mario Mode off. It also took that lock on every draw that writes the stencil, on the game's recording threads. It now takes it once per command list, and only while Mario's mark is still being learnt.
  - Mario's stencil mark could be written where the game had bound the stencil read-only, or in a render pass without stencil access, both invalid in D3D12. It isn't now. The stencil reference is put back after a fault too, and also when the game set separate front and back references (`OMSetFrontAndBackStencilRef`, now tracked).
  - Old capture textures, retired when the resolution changes, were freed only when the GPU was idle (a 0.6 regression).
  - Shared stencil bits were chosen by draw count, so the static world's would have won over what characters get. Marks that only happened to be on screen with Spider-Man could pass as his. That's what the comparison either side of M is for.
  - A sideways camera offset could flip sides on a camera whose rows are laid out differently, and a camera written in fewer frames could crowd out the right one in the search.
  - `StencilMatch = false` from 0.5 was ignored. While Mario was off, the census looked up Spider-Man every frame through the game's `GetActor`. One crash there would have kept Mario Mode off for the session. It's now looked up twice a second, and only while the census needs it.
  - Two weak checks: the stand-in's camera check still allowed 0.5's 0.15 s lag (now it fails with it), and the stacked-decks tests no longer ran out of hits with the rays starting at 8 m.

  0.5.0 had two reviews (neither caught the start-up crash: it needed the game's own code for that function, which the stand-in now copies):
  - The first, of the whole release, found 17 issues. The worst: Spider-Man could stay invisible after Mario Mode if the game had hidden him first. Others: the camera override could stay stuck on a camera the game no longer used, keep learning in photo mode, or give up after three failed searches; a crash in photo mode left its changes in place; narrow posts lost their tops; the serial offset wasn't checked against the game's code. All fixed, with tests.
  - The second, of those fixes, found 9 more problems and 3 weak tests. Fixed: a vote on the depth direction that a single pipeline without a depth test could have turned (hiding Mario again); the serial check never running with the shipped `bindings.ini`; a camera found again being placed with the old camera's framing (a cinematic's would have been); fast mouse turns being taken for a different camera; a camera the game stops writing while it stands still being dropped; a drop-and-find-again loop (with log spam) when the camera isn't following Mario; searches backing off too far; the post fix treating walls seen at an angle as posts; and test checks that couldn't fail. The ninth is a limit, below (Spider-Man shown when Mario Mode ends).

**Not verified, because it needs the real game:**

| Area | What happens if it differs |
|---|---|
| The physics ray cast, its requests and results | Found by static analysis of your exe, with the layouts read from the code that uses them. Every call runs in the crash guard: if one faults, ray casting stops for the session and collision comes from the rendered world (as in 0.3). Same if no ray is answered within 3 s of M. |
| Which query preset rays use (`FloorQuery`, `WallQuery` = 3, the hero's movement) | If it misses some geometry, or hits invisible walls, `[Collision] FloorQuery`/`WallQuery` choose another preset. |
| Whether the game answers hits from both sides of a face | Both are handled and tested. |
| Pause detection | From the flag and timestep the physics frame itself tests. If Mario pauses when he shouldn't, `[Input] PauseWithGame = false`. |
| The damage system | Found by static analysis, with the request's fields written as the game's own code writes them. If it faults, hits change health directly. Whether people in the street react to a 0-damage hit is a guess. |
| Finding the game's camera transform | The log says `camera: the game's camera is the transform written each frame where it renders from` a few seconds after M, then `camera: placed by the mod` once Mario has stood still. When the game switches to another camera (after a load, a cinematic), it says `... - the mod stops placing the camera (looking again)` and finds the new one; meanwhile, and if no camera is found at all, 0.4's lead keeps the game's camera with Mario's jumps (`[Camera] Override = false` does that on purpose). Cinematic shots within 25 m of Mario that reuse the follow camera can't be told apart: the mod places them too. |
| The game's other cameras (new in 0.6.1) | Found from the frames rendered from them, as in the stand-in, but how many the game has, and whether it writes all of them through the hooked setters, is only known from your log. Each one placed is logged (`camera: the game renders from another of its cameras too ...`), and every minute `camera: views while placed: ...` says how many frames still came from elsewhere. A cinematic cut to a camera laid out like the follow camera, within 25 m of Mario, could be placed around him too (0.6 did the same after 2 s); `[Camera] PlaceOtherCameras = false` stops that. |
| The camera's wall query (`[Camera] CollisionQuery = 11`, `kCamera`) | The game's own camera preset, by name and by its use in the camera code. If the camera pulls in for nothing, or not at all, `[Camera] Collision = false` falls back to the game's own camera distance. |
| `DamageActor`, `BotHealth` and the actor handle layout | Read from the code that uses them (`GetActor`, the game's component lookup, the damage system's request for one actor). The log's `combat:` and `hit an enemy ...` lines show them working; `LogHits = true` shows every hit. |
| Keeping Spider-Man hidden (`Transform::Unhide` hooked) | The log counts the refused calls and names their callers (`hero: the game tried to show Spider-Man ...`). `[Hero] KeepHidden = false` turns it off. |
| Photo mode (`[PhotoMode]`: its open flag and the selfie copy's actor handles) | Read from photo mode's own activation, deactivation and selfie code. If it's not found, the log says so: Mario still holds still in photo mode (it pauses the game), but the selfie shows Spider-Man and the pose key isn't held. |
| Cars' "acid" | Seen as Mario landing on a car (a lava boost) in your playtest, and reproduced here with a car that answers `kAcid`. The log names any actor that reports acid or lava. |
| The camera's lag (0.4's lead, now only with `Override = false`) | Learnt as Mario jumps. `[Camera] FollowLead = 0` turns the lead off. |
| The metal cap's reflections | `MetalMaterial = 0` is read from the lighting shaders' use of the material table, not seen in the game. If metal Mario doesn't reflect the city, try other small numbers (1, 2, 3...), or -1 for 0.3's look. |
| The frame structure, shadow passes, D3D12 hooks | Unchanged from 0.3, which ran in your game. |
| Game signatures | The physics and damage ones were found in the exe you sent. The log names any that fail. Fix: [BINDINGS.md](docs/BINDINGS.md). |
| Input | Selective blocking covers raw input, XInput, DualSense (libScePad) and window messages. Which keys the game uses for what on keyboard isn't known here: if a game action you need is on one of Mario's keys, rebind one of them. |
| The camera target hooks, photo requests, ModSettings | As in 0.3 and 0.4. |

## Known limits

- **Ray-traced effects don't see Mario.** RT reflections, RT shadows and RT ambient occlusion trace the game's own ray-tracing scene, which a mod can't add him to. Screen-space effects and the sun's shadow maps do include him. The metal cap is marked with one of the game's reflective material entries, so that the game's reflections (screen-space or ray-traced) land on him.
- **Shadows only where the game re-renders.** Shadow-map regions the game serves from its cache keep their cached contents, so far from the camera Mario may have no shadow.
- **Collision is only as big as what the game has been asked about:** about 5 m around Mario. If he moves faster than the game can be asked about the ground ahead (BLJ speeds), he stops for a moment at its edge until it has answered.
- **Moving cars push Mario** rather than knock him over: SM64 has no moving platforms here, so a car that drives into him shoves him along.
- **Water** is where the game's water is. Water with no bottom the game knows of is deep enough to sink in.
- **Caps:** metal Mario's look depends on the game's reflections around him (dark surroundings make dark metal). Vanish Mario is a dither: smooth with the game's TAA or DLSS, a checkerboard without.
- **The F8 menu** reads the keys once per frame, so at very low frame rates a quick tap can be missed.
- **Poses** only happen while Mario stands on the ground (in photo mode too).
- **When Mario Mode ends, Spider-Man is shown again** if the mod hid him, or if the game tried to show him while Mario was out. If the game then hid him again itself (a cinematic beat), he may stay visible until the game hides him the next time.
- **Photo mode's options for the character** (expressions, suits, poses) are for Spider-Man; Mario has his three poses. If photo mode can hide the character, it doesn't hide Mario: press M first to bring Spider-Man back.
- **The camera's framing** is the game's own for Spider-Man, learnt while Mario stands still (the usual 4.7 m back and 1.35 m up until then): if the game zooms its camera out or in (sprinting, combat), the mod keeps the framing it learnt until Mario stands still again.
- **The camera's aim is the game's.** The mod moves the camera with Mario wherever he goes, but where it looks is left to the game: it turns when you turn it (mouse or right stick), or when the game itself turns it. The mod doesn't swing it round behind Mario as he runs, the way SM64's camera does.
- **The pixel glitch's cause isn't confirmed in the game.** 0.6.1 removes the camera jumps that made Mario's motion vectors spike, and keeps any jump that's left out of them. If a smear is still there, the log's `camera: views while placed` line says whether frames still came from cameras the mod didn't place, and the `frames:` and `stencil:` lines cover 0.6's three other suspects. `[Render] MarioStencil = copy` puts 0.5's stencil marks back, to compare.
- **Cameras the mod can't move.** A camera the game renders from without writing it through the engine's transform setters can't be placed. The log counts the frames rendered from somewhere that matches no transform (`camera: views while placed: ... N not matched to a transform`). Mario's motion vectors stay clean across those frames, but the view still jumps.
- **Camera walls** come from three rays: a gap between two walls the camera fits through, or a thin post, doesn't stop it.
- **Cost:** one extra draw per G-buffer pass and shadow region, and up to 32 ray casts per game frame on the game's thread (the log reports the time they take).

## Troubleshooting

| What you see | What to do |
|---|---|
| "Mario is drawn on top of the game (in-world rendering unavailable)" | The frame didn't look as expected. Send `sm2mario.log`: the frame report after that line shows what the mod saw. |
| Mario in the world but no shadow | Possibly a cached shadow region (normal far from the camera), or the shadow shaders changed. Look for `depth-only pipeline that isn't a known shadow caster` in the log, and add those hashes under `[Shaders]` in `bindings.user.ini`. |
| Mario too shiny, too dark or too bright | SHINE in the F8 menu; `[Render] Specular`, `AlbedoScale`. |
| The camera lags behind Mario | Send the log. `camera: placed by the mod N% of the last 60 s (... dropped ...: why)` says how much of the time the mod placed it and why it let go. While it doesn't, the game's own camera (which trails him) is back. |
| The camera misbehaves | Send the log (`camera:` lines). `[Camera] VerticalSmoothing` softens or stiffens how it follows jumps; `Collision = false` stops it coming in front of walls; `Override = false` gives the camera back to the game (0.4's camera); CAMERA FOLLOWS JUMPS off in the F8 menu does that too, live. `[Hero] HoldDuringFrame = false` removes the hooks entirely (after a restart). |
| The camera still jumps between near and far | Send the log. `camera: views while placed: ...` says how many frames came from somewhere the mod didn't put a camera, and whether they matched one of the game's transforms (`not matched to a transform`). `[Debug] TraceCamera = true` logs the first 400 of those frames, each with where it came from and the nearest camera the mod placed. `[Camera] PlaceOtherCameras = false` goes back to 0.6.0's single camera, to compare. |
| A cutscene shows Mario from behind | A cinematic camera close to Mario got placed (see [What's verified](#whats-verified-and-what-isnt)). `[Camera] PlaceOtherCameras = false`, and send the log (`camera: the game renders from another of its cameras too` lines). |
| Spider-Man shows up | Send the log: `hero: the game tried to show Spider-Man N time(s) - kept hidden (from exe+...)` lines name what tried. |
| Mario jumps off a surface as if it were lava | Send the log: `collision: Mario landed on lava at (...) - the floor there is ...` says what the game called it. |
| No MARIO MODE page in the pause menu | The log's `ModSettings:` line says whether ModSettings was found. It needs ModSettings installed as a script like this mod. |
| Mario doesn't pose for photos | Check POSE FOR PHOTOS, and look for `photo poses:` and `photo request:` lines in the log. |
| Mario falls through the floor | Send the log: each `fall:` line says where he left the ground and where he landed, with what the game had reported under him at both places, and `collision: the floor under Mario went away` says when his floor disappeared under him. |
| Mario bumps into something that isn't there | Send the log: each `bump:` line names the walls nearest Mario, where each came from (the game's wall rays, with its material and the actor that answered, or a step between two floors) and when it was found. |
| A blur or smear follows Mario | Send the log (`camera: views while placed`, `frames:` and `stencil:` lines) and, if you can, a picture. Try `[Render] MarioStencil = copy` (0.5's marks) or `keep` (none), and say whether it changes. |
| Mario walks through something or stands on air | Set `[Debug] TraceCollision = true`, reproduce, and send the log. It shows every tick and the collision around him. `[Collision] Source = world` brings back 0.3's collision. |
| `collision: ... - Mario collides with what the camera sees instead` | The game's physics couldn't be used (the line says why). Collision is 0.3's. Send the log. |
| Mario freezes while the game isn't paused | `[Input] PauseWithGame = false`, and send the log (`the game is paused` lines). |
| With `Override = false`, the camera swings ahead of Mario's jumps | Lower `[Camera] FollowLead` (0 = the game's own follow). The log's `camera lead:` line says how far it thinks the camera trails. |
| A game key doesn't work with Mario out | It's one of Mario's keys. Rebind it in the game or in `[Controls]`. |
| "Mario Mode hit an error and switched itself off" | A crash was caught. Send `sm2mario.log`: the `CRASH PREVENTED` line says exactly where. |
| Red toast at start-up | It says exactly what's wrong (ROM missing or wrong version, game hooks not found). Details are in `sm2mario.log`. |
| Mario looks mirrored, or left/right are swapped | Ctrl+F7 (mirror). |
| Spider-Man still visible | The hero line in F10 says how hiding went. `HideHero = write` with a `[HideHero]` binding is the fallback. |
| No enemies take damage | Set `LogHits = true` and press Ctrl+F9 near enemies, then tune `EnemyNameInclude`/`Exclude` from the actor names in the log. `[Combat] UseGameDamage = false` makes hits change health directly. |
| "pin drift" in the log | Try `[Hero] PositionWrites = direct` (or `function`). |

Anything else: set `LogLevel = debug`, reproduce, and look at `sm2mario.log`.

## Building from source

**Linux / WSL** (needs `g++-mingw-w64-x86-64`, cmake, ninja, git, make, python3; Wine, Xvfb and Mesa's lavapipe for the Wine tests):

```sh
./build.sh                      # -> dist/sm2mario-<version>.script
tests/run_tests.sh              # host unit tests (ASan/UBSan)
tests/run_guard_test.sh /tmp/w  # crash guard under Wine
tests/run_smoke.sh /tmp/w [default|MOCK_BAD_SETPOS|MOCK_CRASH_SETPOS]
# with a native libsm64 build (make lib) and your ROM:
tests/run_libsm64_test.sh /path/to/libsm64
SM2MARIO_TEST_ROM=/path/sm64.us.z64 tests/run_world_test.sh /path/to/libsm64
SM2MARIO_TEST_ROM=/path/sm64.us.z64 tests/run_cheats_test.sh /path/to/libsm64
SM2MARIO_TEST_ROM=/path/sm64.us.z64 tests/run_physics_test.sh /path/to/libsm64
# end to end in the stand-in deferred renderer (Microsoft's d3dcompiler_47.dll needed for its shaders):
SM2MARIO_TEST_ROM=/path/sm64.us.z64 D3DCOMPILER=/path/d3dcompiler_47.dll tests/run_e2e.sh /tmp/w
#   variants: E2E_COLLISION=world, E2E_NO_GAME_THREAD=1, E2E_SCREEN=lh, E2E_NO_PIN=1, E2E_NO_MODSETTINGS=1,
#   E2E_NO_REGISTRY=1 (components found by name), E2E_INI_EXTRA='[Camera]\nOverride = false\n' (0.4's camera),
#   E2E_CAMERA_TAU=0.6 (a laggier game camera), E2E_INI_EXTRA='[Render]\nMarioStencil = copy\n' (0.5's stencil
#   marks: the stencil checks must fail), E2E_STENCIL_FRAMES=n (frames the stencil census waits for, 40),
#   E2E_CAMERA_THREAD=1 (the camera written on a thread of its own, racing the Present), E2E_CAMERA_FAR=1 (the
#   game's own camera falls 50 m behind), E2E_CAMERA_DISTANCE=1.5, E2E_CAMERA_RIGS=1 (a second camera rendered
#   from every other frame, 21-27.5 s), E2E_INI_EXTRA='[Camera]\nPlaceOtherCameras = false\n' (with RIGS: the
#   camera checks in that window must fail), E2E_INI_EXTRA='[Render]\nAlignToCamera = false\n' (with THREAD: the
#   on-screen check must fail), MOD_DLL=/path/sm2mario.dll (another build, e.g. an older release, to compare)
```

The shaders Mario is drawn with are precompiled into `src/render/inject_shaders_bin.h`, because Proton's shader compiler can't build them. After editing `src/render/inject_shaders.h`, regenerate the header with `tools/compile_shaders.sh /path/to/d3dcompiler_47.dll`. A unit check fails if the header is out of date.

**Windows:** run `build.ps1` (Visual Studio 2022 + CMake). `sm64.dll` itself needs MinGW: the script uses MSYS2 if it's installed, or you can pass `-Sm64Dll` with a prebuilt one.

### Layout

```
src/dllmain.cpp        script_enable() entry (Overstrike's scripts proxy calls it)
src/mod/               orchestration: activation, 30 Hz SM64 tick + interpolation, pinning, combat, HUD, photo mode,
                       the world-model worker thread; settings + F8 menu, cheats and poses, the ModSettings bridge,
                       the camera override (and 0.4's camera lead)
src/sm64/              libsm64 loader and Mario controller (snapshots, origin shifts)
src/world/             game <-> SM64 coordinates (floating origin), collision from the game's ray answers
                       (physics_world) and from depth (world model), the collision builder, physics materials
src/game/              SM2 bindings (signatures, actors, components, health, hide), RTTI class finder, hero pin,
                       camera and camera-target hooks, photo-request hook, photo mode, camera finder/tracker, combat,
                       the game's physics and damage system (sm2_physics: ray casts and damage on the game's thread,
                       pause detection)
src/render/            d3d12_hook: Present/ExecuteCommandLists; frame_tracker: device/list hooks and state
                       tracking; frame_policy: where Mario goes in the frame; injector: his draws, the state
                       restore and the depth read-back; inject_shaders: the HLSL; renderer: overlay mode + HUD
src/input/             keyboard/XInput/DualSense for Mario + input shim that hides it from the game
src/audio/             XAudio2 stream for Mario's voice and SFX
src/win/               crash guard
package/               info.json and the default sm2mario.ini / bindings.ini
tests/                 unit tests, libsm64 tests, Wine smoke and end-to-end hosts (mock game, stand-in renderer
                       and physics)
third_party/           libsm64 headers and Mario Mode's patch to it, MinHook
tools/                 packaging, source zip, shader precompiler
```

## Credits and licences

- **[libsm64](https://github.com/libsm64/libsm64)** (CC0). `sm64.dll` contains Mario's model geometry from the SM64 decompilation, as every libsm64 build does. Textures, animations, voice and sounds are read from your ROM at runtime. No Nintendo assets ship with this mod.
- **[SM2ScriptTemplate](https://github.com/hbgda/SM2ScriptTemplate)** by LDD565 (GPLv3): the Spider-Man 2 signatures and structure layouts.
- **[Overstrike](https://github.com/Tkachov/Overstrike)** by Tkachov (GPLv3): the mod manager and scripts proxy.
- **[MinHook](https://github.com/TsudaKageyu/minhook)** (BSD-2-Clause), vendored.
- This project is GPLv3 (see `LICENSE.txt`), following the template it builds on.

Super Mario 64 and Mario are Nintendo's. Marvel's Spider-Man 2 is Sony Interactive Entertainment's and Insomniac Games'. This is an unofficial fan mod: bring your own legally obtained ROM.
