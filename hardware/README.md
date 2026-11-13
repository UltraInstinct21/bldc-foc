# Hardware

KiCad sources for board revision v6 (`bldc_control_final_v6`).

| File | Contents |
|---|---|
| `bldc_control_final_v6.kicad_pro` | KiCad project |
| `bldc_control_final_v6.kicad_sch` | Schematic |
| `bldc_control_final_v6.kicad_pcb` | PCB layout |

Open the `.kicad_pro` file with KiCad 7 or newer.

The board is a 24 V three-phase drive: STM32G431CBT6, DRV8316R, three
low-side shunts, a 24 V to 5 V buck, a 5 V power mux and a 3.3 V LDO. Motor
phases, SWD, the encoder and the debug UART are on separate connectors.

Pin assignments, connector pinouts and known board limitations are documented
in [`../docs/hardware.md`](../docs/hardware.md).

The schematic and PCB PDFs, datasheets and the motor design report are not
part of this repository. They are kept locally and covered by `.gitignore`.
