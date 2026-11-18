# Frontend: motor-control UI

React + TypeScript + Vite + Tailwind v4 + shadcn/ui (Radix, Nova preset), with
Magic UI components from the shadcn registry and `motion` for animation.
Visual language: [`../DESIGN.md`](../DESIGN.md).

```sh
npm install
npm run dev        # http://localhost:5173, proxies /api and /ws to the backend on :8000
npm run build      # dist/, served by the backend at http://127.0.0.1:8000
```

Pages: **Drive** (inputs, measured values, outputs, trends), **Scope** (20 kHz
snapshots), **Tests** (catalogue + live job), **Parameters** (every console
parameter), **Console** (raw).

Component sources, checked against libraries before building anything by hand:

| Need | Source |
|---|---|
| Button, card, tabs, slider, select, switch, input, table, tooltip, sheet, alert dialog, collapsible, toggle group, progress, scroll area, sonner, kbd, chart | shadcn/ui |
| Animated numbers, ring gauge, entrance fade, list item animation | Magic UI (`number-ticker`, `animated-circular-progress-bar`, `blur-fade`, `animated-list`) |
| Rotor angle / space-vector dial, per-phase duty bars | custom SVG + `motion` (no library has them) |

Phase colours (A terracotta, B green, C plum) were validated for colour-vision
deficiency on the card surface; they are the warmest set that passed. Clay
`#d97757` stays reserved for the one decisive action on screen (Start when
idle, STOP while running).
