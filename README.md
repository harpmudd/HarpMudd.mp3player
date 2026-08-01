# MP3 Player — Analogue Pocket

An Analogue Pocket port of **MP3 Player** (HarpMudd, 2026) by
**HarpMudd**, built on the openFPGA framework.

<!-- TODO when porting: fill the TODO sections below. Pull the MiSTer/FPGA
     author credits from the source headers, NOT memory:
       grep -i "Port to MiSTer" <Arcade-X>.sv       -> MiSTer porter
       grep -iE "copyright|by Dar|MikeJ" rtl/*.vhd   -> FPGA core author
     Delete this comment and any unused sections (e.g. Notes & Caveats) when done. -->

## The Game

<!-- TODO: 1–2 paragraph high-level overview of the arcade game. -->

## Hardware

| Part | Role |
|---|---|
| <!-- TODO: CPU --> | Main CPU |
| <!-- TODO: audio chip --> | Sound |
| Display | <!-- TODO: e.g. Horizontal CRT, 15 kHz, RGB --> |

## The Port

Built on the MiSTer **Arcade-<!-- TODO -->** core:

- **MiSTer port:** <!-- TODO: porter (from the top .sv "Port to MiSTer" header) -->
- **FPGA arcade hardware implementation:** <!-- TODO: core author (from RTL copyright headers) -->

This Analogue Pocket build adapts that RTL to the openFPGA / APF framework.
<!-- TODO: one line on resolution / orientation. --> Many thanks to the authors above.

## Controls

| Pocket | Action |
|---|---|
| **D-Pad** | <!-- TODO --> |
| **A** | <!-- TODO --> |
| **Start** | 1P Start |
| **Select** | Insert coin |

## Notes & Caveats

<!-- TODO: any per-core quirks (calibration steps, pending work, ROM variant
     notes…). Delete this whole section if there are none. -->

## ROMs

ROMs are **not** included. Build your own from the bundled `.mra` recipe in
`Assets/mp3player/common/` — it lists the required MAME romset files by name
and CRC32, with no copyrighted data. Run it through the `mra` tool to produce
`mp3player.rom`, then keep that `.rom` in the same folder (and on your Pocket
SD card).

## Credits

- **Original arcade game:** HarpMudd (2026)
- **MiSTer port:** <!-- TODO -->
- **FPGA arcade core:** <!-- TODO -->
- **Analogue Pocket port:** HarpMudd

## About / Support

I'm into retro games and the Analogue Pocket, always cooking up something new.
I love being part of a community built on sharing and the love of games — so if
any of my projects bring you joy, grab me a coffee; it fuels the next thing.

☕ **[buymeacoffee.com/harpmudd](https://buymeacoffee.com/harpmudd)**
