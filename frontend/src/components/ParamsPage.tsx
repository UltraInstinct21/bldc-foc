import { Wand2 } from 'lucide-react'
import { BlurFade } from '@/components/ui/blur-fade'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table'
import { cmd } from '@/lib/api'
import { useStore } from '@/lib/store'
import type { Params } from '@/lib/types'
import { SectionTitle, Stat } from './common'
import { ParamField } from './drive/Controls'

interface Row {
  key: keyof Params
  label: string
  unit: string
  range: string
  what: string
  cmd: (v: number, p: Params) => string
}

// Every console parameter of test_08_drive_lab, with its limits from the firmware.
const GROUPS: { title: string; rows: Row[] }[] = [
  {
    title: 'Supply & protection',
    rows: [
      { key: 'vbus', label: 'Supply voltage', unit: 'V', range: '4.5 – 30', what: 'No VM sense on the board: tell it the supply. Use ≥ 9 V (VM UVLO trips at 6 V under load).', cmd: (v) => `vbus ${v}` },
      { key: 'ilim', label: 'Current limit', unit: 'A', range: '0.2 – 4.5', what: 'Phase-current limiter (open loop) or reference bound (FOC). Trip at min(2·ilim, 5.3 A) for 20 ms.', cmd: (v) => `ilim ${v}` },
      { key: 'ampmax', label: 'Amplitude ceiling', unit: '%', range: '10 – 95', what: '60 % protects a low-resistance motor; above that only with series resistors.', cmd: (v) => `set ampmax ${v}` },
    ],
  },
  {
    title: 'Open-loop drive',
    rows: [
      { key: 'amp', label: 'Amplitude', unit: '%', range: '0 – ampmax', what: '6-step: duty. Others: line-line peak / Vbus.', cmd: (v) => `amp ${v}` },
      { key: 'f', label: 'Target frequency', unit: 'Hz el.', range: '0.05 – 400', what: 'Ramp target (FOC: speed reference).', cmd: (v) => `f ${v}` },
      { key: 'fstart', label: 'Start frequency', unit: 'Hz', range: '0.05 – 50', what: 'Frequency after alignment.', cmd: (v) => `fstart ${v}` },
      { key: 'acc0', label: 'Acceleration', unit: 'Hz/s', range: '—', what: 'Ramp rate = acc0 + acc1·f.', cmd: (v, p) => `accel ${v} ${p.acc1}` },
      { key: 'acc1', label: 'Acceleration per Hz', unit: '1/s', range: '—', what: 'Speed-proportional part of the ramp.', cmd: (v, p) => `accel ${p.acc0} ${v}` },
      { key: 'align', label: 'Align time', unit: 'ms', range: '0 – 5000', what: 'Hold the first vector before ramping.', cmd: (v) => `align ${Math.round(v)}` },
    ],
  },
  {
    title: 'Motor model',
    rows: [
      { key: 'R', label: 'Phase resistance', unit: 'Ω', range: '> 0', what: 'From "Measure R/L", or set by hand. Needed for FOC.', cmd: (v) => `set R ${v}` },
      { key: 'L', label: 'Phase inductance', unit: 'µH', range: '> 0', what: 'From "Measure R/L". Needed for FOC.', cmd: (v) => `set L ${v}` },
      { key: 'lam', label: 'Flux linkage λ', unit: 'Wb', range: '0 = estimate', what: 'Sensorless uses this, or the I/f back-EMF estimate when 0.', cmd: (v) => `set lam ${v}` },
    ],
  },
  {
    title: 'Current loop & sensorless',
    rows: [
      { key: 'bw', label: 'Current-loop bandwidth', unit: 'Hz', range: '50 – 2000', what: 'Kp = L·ωc, Ki = R·ωc.', cmd: (v) => `set bw ${v}` },
      { key: 'ifa', label: 'I/f current', unit: 'A', range: '0.1 – 4', what: 'd-axis current along the open-loop angle.', cmd: (v) => `set ifa ${v}` },
      { key: 'fsw', label: 'Handover speed', unit: 'Hz', range: '5 – 300', what: 'Sensorless takes over from I/f here once the PLL agrees.', cmd: (v) => `set fsw ${v}` },
      { key: 'kps', label: 'Speed loop Kp', unit: 'A/Hz', range: '—', what: '', cmd: (v) => `set kps ${v}` },
      { key: 'kis', label: 'Speed loop Ki', unit: 'A/Hz/s', range: '—', what: '', cmd: (v) => `set kis ${v}` },
      { key: 'pll', label: 'PLL bandwidth', unit: 'Hz', range: '5 – 200', what: 'Observer angle tracking.', cmd: (v) => `set pll ${v}` },
      { key: 'idsl', label: 'Sensorless id', unit: 'A', range: '-3 – 0', what: 'Keeps |I| up so the bridge keeps conducting.', cmd: (v) => `set idsl ${v}` },
      { key: 'vrec', label: 'Observer voltage', unit: '', range: '0, 1, 2', what: '0 commanded, 1 written duties × Vbus, 2 + floating-phase rebuild.', cmd: (v) => `set vrec ${v}` },
      { key: 'dtc', label: 'Dead-time compensation', unit: '', range: '0 / 1', what: '±2 % by current sign on switching phases.', cmd: (v) => `set dtc ${v}` },
    ],
  },
  {
    title: 'Logging',
    rows: [
      { key: 'log', label: 'Console log period', unit: 'ms', range: '50 – 10000', what: 'Human-readable status lines.', cmd: (v) => `log ${Math.round(v)}` },
      { key: 'tel', label: 'Telemetry period', unit: 'ms', range: '20 – 10000', what: 'Machine (@T) lines that feed this UI.', cmd: (v) => `tel ${Math.round(v)}` },
      { key: 'trdec', label: 'Trace decimation', unit: '× 50 µs', range: '1 – 100', what: 'Scope sample spacing.', cmd: (v) => `set trdec ${Math.round(v)}` },
    ],
  },
]

export function ParamsPage() {
  const p = useStore((s) => s.params)
  const hello = useStore((s) => s.hello)
  const status = useStore((s) => s.status)
  const isLab = status?.connected && status.app === 'test_08_drive_lab'

  return (
    <div className="flex flex-col gap-6">
      <BlurFade inView={false}>
        <SectionTitle eyebrow="Inputs" title="Parameters">
          <div className="flex gap-2">
            <Button variant="ivory" className="gap-2" disabled={!isLab}
              onClick={() => { cmd('set R 0.1378'); cmd('set L 11.3'); cmd('set lam 0.0009'); cmd('vbus 9') }}>
              <Wand2 className="size-4" />Drone motor (bring-up values)
            </Button>
            <Button variant="outline" className="rounded-xl" disabled={!isLab} onClick={() => cmd('params')}>Refresh</Button>
          </div>
        </SectionTitle>
        <p className="mt-2 max-w-3xl font-voice text-[17px] leading-relaxed text-muted-foreground">
          Settings live in the board's RAM and are lost on every reset or flash. The table always shows what the board
          reports back after each command.
        </p>
      </BlurFade>

      {hello && (
        <BlurFade delay={0.04} inView={false}>
          <Card className="bg-oat ring-0">
            <CardContent className="grid grid-cols-2 gap-5 sm:grid-cols-3 lg:grid-cols-6">
              <Stat label="Firmware" value={hello.app.replace('test_08_', '')} />
              <Stat label="Built" value={hello.built} className="col-span-1" />
              <Stat label="Protocol" value={`v${hello.proto}`} />
              <Stat label="Loop rate" value={(hello.fs / 1000).toFixed(0)} unit="kHz" />
              <Stat label="CSA gain" value={hello.csa.toFixed(2)} unit="V/A" />
              <Stat label="Hard trip" value={hello.itrip.toFixed(1)} unit="A" />
            </CardContent>
          </Card>
        </BlurFade>
      )}

      <div className="grid gap-5 xl:grid-cols-2">
        {GROUPS.map((g, gi) => (
          <BlurFade key={g.title} delay={0.06 + gi * 0.04} inView={false}>
            <Card className="h-full">
              <CardHeader><CardTitle className="font-voice text-xl font-normal">{g.title}</CardTitle></CardHeader>
              <CardContent>
                <Table>
                  <TableHeader>
                    <TableRow>
                      <TableHead className="w-[38%]">Parameter</TableHead>
                      <TableHead className="w-36">Value</TableHead>
                      <TableHead className="text-right">Range</TableHead>
                    </TableRow>
                  </TableHeader>
                  <TableBody>
                    {g.rows.map((r) => (
                      <TableRow key={r.key} className="align-top">
                        <TableCell className="whitespace-normal">
                          <div className="font-sans text-[13px] font-medium">{r.label}</div>
                          {r.what && <div className="font-voice text-[13px] leading-snug text-muted-foreground">{r.what}</div>}
                        </TableCell>
                        <TableCell>
                          <ParamField label="" value={p ? (p[r.key] as number) : undefined} unit={r.unit}
                            command={(v) => r.cmd(v, p!)} className="[&>label]:hidden" />
                        </TableCell>
                        <TableCell className="num text-right text-xs text-muted-foreground">{r.range}</TableCell>
                      </TableRow>
                    ))}
                  </TableBody>
                </Table>
              </CardContent>
            </Card>
          </BlurFade>
        ))}
      </div>
    </div>
  )
}
