---
layout: base.njk
title: Get started
description: Run the gemifi dashboard from its Docker image, build a device, and flash it from the browser.
permalink: /get-started/
---
<section class="hero">
  <h1>Get started</h1>
  <p>Run the dashboard, build a device, flash it from the browser.</p>
</section>

<div class="prose">

## 1. Run the dashboard

The dashboard, the builder and both toolchains come in one Docker image:

```sh
docker run -d --name gemifi -p 6055:6055 \
  -v ./config:/config -v gemifi-cache:/cache \
  {{ site.image }}:edge
```

Your device files go in `./config`. Use the `edge` tag until there's a
release. To build the image from a checkout instead, run
`docker compose up -d --build` in the repo.

## 2. Open it in the browser

Open <http://localhost:6055/> in Chrome or Edge. Firefox also works for Web MIDI
after a one-time permission add-on; Safari has no Web MIDI.

Flashing needs a secure page: `localhost`, or `https://`. If the dashboard runs
on another machine, use the image's self-signed TLS mode or an SSH tunnel; the
[builder docs]({{ site.blob }}tools/builder/README.md#docker) cover both.

## 3. Build a device

Choose **New device**, start from a preset (the Keystation, or one of the ESP
examples), and edit the YAML. **Validate** shows errors next to their line
numbers. **Save & build** streams the log and leaves the firmware image ready to
flash.
The first ESP build downloads about 6 GB of toolchains.

## 4. Flash it

- **Keystation Mini 32 MK3:** the gemifi bootloader has to go on once over SWD
  before the browser can flash it. See [its page]({{ '/devices/keystation-mini32-mk3/' | url }}).
  After that, connect over USB MIDI and flash from the **Firmware** card.
- **ESP boards:** the first time, use the **Install firmware (first flash)** card (Web
  Serial, Chrome, Edge or Opera on a desktop). After that, updates go over MIDI
  from the **Firmware** card.

When the browser asks to allow MIDI devices with SysEx, allow it.

## 5. Change what it does

Once gemifi is running, the same page shows the device's log and its settings:
MIDI channel, octave, velocity curve, and what each button, knob and encoder
sends. **Save to device** keeps them across power cycles.

</div>

<p class="readme-link">
  More detail: the <a href="{{ site.blob }}tools/builder/README.md" rel="noopener">builder and dashboard</a>,
  the <a href="{{ site.blob }}web/updater/README.md" rel="noopener">web updater</a> and the
  <a href="{{ site.blob }}core/README.md" rel="noopener">device YAML format</a>.
</p>
