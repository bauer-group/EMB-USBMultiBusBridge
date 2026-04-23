# Hardware

KiCad 9 design files for the RPBridge rev-B board. Currently empty —
the authoritative design document is
[`docs/drafts/rpbridge-hardware-design.md`](../docs/drafts/rpbridge-hardware-design.md),
pending translation into a checked-in KiCad project.

## Planned layout

```
hardware/
├── rpbridge-rev-B.kicad_pro
├── rpbridge-rev-B.kicad_sch
├── rpbridge-rev-B.kicad_pcb
├── rpbridge-rev-B.kicad_sym        # project-specific symbols
├── rpbridge-rev-B.pretty/          # project-specific footprints
├── bom/
│   ├── rpbridge-rev-B.bom.csv      # parametric BOM
│   └── rpbridge-rev-B.pos.csv      # SMT pick-and-place
├── gerbers/
│   └── rpbridge-rev-B/             # JLC/Aisler-ready Gerber ZIP
├── 3d/
│   └── rpbridge-rev-B.step
└── renderings/
    └── front.png, back.png
```

## License

Hardware design files are released under **CERN-OHL-S-2.0** (strong
reciprocity). Derivative boards keep the source open. See
[`docs/licensing.md`](../docs/licensing.md).
