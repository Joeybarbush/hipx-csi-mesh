# What this project is — in plain English

Imagine your home WiFi. Right now it's just carrying Netflix and texts. But the same radio waves bouncing around your house also bump into *you* — your body changes them slightly every time you move. Most devices throw that information away. This project keeps it.

I put cheap $6 chips (called ESP32s) in a couple of rooms. Each one quietly listens to how the WiFi signal in that room is getting disturbed. When the room is empty and still, the signal is steady. When someone walks in, the signal gets noisy. The chip measures that noise and reports one of three things: **STILL**, **a little movement**, or **MOTION**.

So without a single camera or microphone, the house can tell when a room is occupied and when someone's moving through it. It's presence-sensing built out of the WiFi you already pay for.

## Why I think it's cool

- **It costs basically nothing.** Two small chips and a computer I already own. No monthly fee, no cloud account.
- **Nothing leaves the house.** No camera footage, no recordings going to some company's server. Everything stays on my own machine. That privacy is the whole point.
- **It actually runs.** This isn't a slideshow. I loaded the software onto the chips, turned them on, and watched two rooms report movement in real time.

## The part I'm actually proud of

I didn't just build it and call it magic. I did a serious research review — pulled 22 sources, and made an AI fact-check every claim against critics before I'd trust it. The honest result:

- **Detecting that someone's in the room, or moving: this works well.** That's what I built.
- **Detecting breathing: possible, with more work.** Doable, not done yet.
- **Detecting a heartbeat: no.** People online claim WiFi can read your pulse. The physics say otherwise on this hardware — your heartbeat moves your chest about 50 times *less* than your breathing does, so it gets drowned out. I tested it, it doesn't work, and I say so plainly.

That last point matters more than the demo. Anyone can show you the thing that works. Knowing exactly where the limit is — and not overselling past it — is the actual skill.

## In one sentence

I turned ordinary home WiFi into a private, no-camera motion sensor using $12 of hardware, got it working live across two rooms, and documented honestly what it can and can't do.
